// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/DrawStateLog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "VideoCommon/BPMemory.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/XFMemory.h"

namespace DrawStateLog
{
namespace
{
// blend, z mode, alpha test, gen mode, z control, dst alpha, z texture, first texture,
// first tev colour and alpha combiner, projection type, viewport z range and far z.
using DrawKey = std::array<u32, 13>;
// address, width, height, format, flags.
using CopyKey = std::array<u32, 5>;

struct Entry
{
  u64 count = 0;
  u64 indices = 0;
  u64 first_seen = 0;
};

constexpr std::size_t MAX_ENTRIES = 1500;

std::atomic<bool> s_recording{false};
std::mutex s_mutex;
std::map<DrawKey, Entry> s_draws;
std::map<CopyKey, Entry> s_copies;
u64 s_sequence = 0;
u64 s_dropped = 0;

constexpr u32 MAX_DETAIL_FRAMES = 6;
constexpr std::size_t MAX_DETAIL_SIZE = 200 * 1024;
std::string s_detail;
u32 s_detail_frames = 0;

u32 FloatBits(float value)
{
  u32 bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

template <typename Map, typename Key>
void Add(Map& map, const Key& key, u32 indices)
{
  auto it = map.find(key);
  if (it == map.end())
  {
    if (map.size() >= MAX_ENTRIES)
    {
      ++s_dropped;
      return;
    }
    it = map.emplace(key, Entry{}).first;
    it->second.first_seen = s_sequence;
  }
  ++it->second.count;
  it->second.indices += indices;
}
}  // namespace

void Start()
{
  const std::lock_guard lock(s_mutex);
  s_draws.clear();
  s_copies.clear();
  s_sequence = 0;
  s_dropped = 0;
  s_detail.clear();
  s_detail_frames = 0;
  s_recording.store(true, std::memory_order_release);
}

void RecordDraw(u32 index_count)
{
  if (!s_recording.load(std::memory_order_relaxed))
    return;

  const DrawKey key{bpmem.blendmode.hex,
                    bpmem.zmode.hex,
                    bpmem.alpha_test.hex,
                    bpmem.genMode.hex,
                    bpmem.zcontrol.hex,
                    bpmem.dstalpha.hex,
                    bpmem.ztex2.hex,
                    bpmem.tex.GetUnit(0).texImage0.hex,
                    bpmem.combiners[0].colorC.hex,
                    bpmem.combiners[0].alphaC.hex,
                    static_cast<u32>(xfmem.projection.type),
                    FloatBits(xfmem.viewport.zRange),
                    FloatBits(xfmem.viewport.farZ)};

  const std::lock_guard lock(s_mutex);
  ++s_sequence;
  Add(s_draws, key, index_count);
}

void RecordFlatDraw(const u8* positions, u32 position_components, const u8* colors, u32 stride,
                    u32 vertex_count)
{
  if (!s_recording.load(std::memory_order_relaxed))
    return;
  if (xfmem.projection.type != ProjectionType::Orthographic)
    return;

  const std::lock_guard lock(s_mutex);
  if (s_detail_frames >= MAX_DETAIL_FRAMES || s_detail.size() >= MAX_DETAIL_SIZE)
    return;

  // Normalised device coordinates of the corners of everything this draw covers.
  const float* const matrix = &xfmem.posMatrices[xfmem.MatrixIndexA.PosNormalMtxIdx * 4];
  const auto& projection = xfmem.projection.rawProjection;
  float min_x = 0, max_x = 0, min_y = 0, max_y = 0, min_z = 0, max_z = 0;
  for (u32 i = 0; i < vertex_count; ++i)
  {
    float p[3] = {0.0f, 0.0f, 0.0f};
    std::memcpy(p, positions + std::size_t{i} * stride,
                sizeof(float) * std::min<u32>(position_components, 3));
    const float vx = matrix[0] * p[0] + matrix[1] * p[1] + matrix[2] * p[2] + matrix[3];
    const float vy = matrix[4] * p[0] + matrix[5] * p[1] + matrix[6] * p[2] + matrix[7];
    const float vz = matrix[8] * p[0] + matrix[9] * p[1] + matrix[10] * p[2] + matrix[11];
    const float x = projection[0] * vx + projection[1];
    const float y = projection[2] * vy + projection[3];
    const float z = projection[4] * vz + projection[5];
    if (i == 0)
    {
      min_x = max_x = x;
      min_y = max_y = y;
      min_z = max_z = z;
    }
    min_x = std::min(min_x, x);
    max_x = std::max(max_x, x);
    min_y = std::min(min_y, y);
    max_y = std::max(max_y, y);
    min_z = std::min(min_z, z);
    max_z = std::max(max_z, z);
  }

  u32 first_color = 0;
  if (colors && vertex_count != 0)
    std::memcpy(&first_color, colors, sizeof(first_color));

  const auto& constants = Core::System::GetInstance().GetPixelShaderManager().constants;
  const auto& unit = bpmem.tex.GetUnit(0);
  s_detail += fmt::format(
      "  v{} x {:.3f}..{:.3f} y {:.3f}..{:.3f} z {:.4f}..{:.4f} | blend {:06x} z {:06x} atest "
      "{:06x} gen {:06x} pe {:06x} | tex {:08x} {:06x} | tev0 {:06x} {:06x} | vtxcol {:08x} | "
      "",
      vertex_count, min_x, max_x, min_y, max_y, min_z, max_z, bpmem.blendmode.hex, bpmem.zmode.hex,
      bpmem.alpha_test.hex, bpmem.genMode.hex, bpmem.zcontrol.hex,
      static_cast<u32>(unit.texImage3.image_base) << 5, unit.texImage0.hex,
      bpmem.combiners[0].colorC.hex, bpmem.combiners[0].alphaC.hex, first_color);
  // What a small texture holds in guest memory, to tell an empty texture from one that fails to
  // show: how many of its first bytes are not zero, and the first of them.
  {
    const u32 address = static_cast<u32>(unit.texImage3.image_base) << 5;
    const u32 width = unit.texImage0.width + 1;
    const u32 height = unit.texImage0.height + 1;
    if (width <= 64 && height <= 64)
    {
      const u32 length = std::min<u32>(width * height * 4, 4096);
      const u8* const data = Core::System::GetInstance().GetMemory().GetPointerForRange(
          address, length);
      if (data)
      {
        u32 nonzero = 0;
        for (u32 i = 0; i < length; ++i)
          nonzero += data[i] != 0;
        s_detail += fmt::format(" texels {}/{} nonzero, first", nonzero, length);
        for (u32 i = 0; i < 16; ++i)
          s_detail += fmt::format(" {:02x}", data[i]);
        s_detail += " |";
      }
    }
  }

  s_detail += " reg";
  for (const auto& color : constants.colors)
    s_detail += fmt::format(" {},{},{},{}", color[0], color[1], color[2], color[3]);
  s_detail += " | konst";
  for (const auto& color : constants.kcolors)
    s_detail += fmt::format(" {},{},{},{}", color[0], color[1], color[2], color[3]);
  s_detail += fmt::format(" | scissor {:06x} {:06x} viewport {:.1f} {:.1f} {:.1f} {:.1f}\n",
                          bpmem.scissorTL.hex, bpmem.scissorBR.hex, xfmem.viewport.xOrig,
                          xfmem.viewport.yOrig, xfmem.viewport.wd, xfmem.viewport.ht);
}

void RecordCopy(u32 dest_address, u32 width, u32 height, u32 format, bool depth, bool xfb,
                bool to_ram, bool to_vram, bool intensity, bool half_scale)
{
  if (!s_recording.load(std::memory_order_relaxed))
    return;

  const u32 flags = (depth ? 1 : 0) | (xfb ? 2 : 0) | (to_ram ? 4 : 0) | (to_vram ? 8 : 0) |
                    (intensity ? 16 : 0) | (half_scale ? 32 : 0);
  const CopyKey key{dest_address, width, height, format, flags};

  const std::lock_guard lock(s_mutex);
  ++s_sequence;
  Add(s_copies, key, 0);

  if (s_detail_frames < MAX_DETAIL_FRAMES && s_detail.size() < MAX_DETAIL_SIZE)
  {
    s_detail += fmt::format("  -- copy to {:08x} {}x{} format {:x} flags {:x}\n", dest_address,
                            width, height, format, flags);
    if (xfb)
      ++s_detail_frames;
  }
}

std::string Finish()
{
  s_recording.store(false, std::memory_order_release);

  const std::lock_guard lock(s_mutex);
  std::string out = fmt::format(
      "\nDraw states ({} distinct, {} draws and copies in all, {} left out). Raw register values: "
      "draws indices first | blend zmode alphatest genmode zcontrol dstalpha ztex2 teximage0 "
      "tev0color tev0alpha projection zrange farz\n",
      s_draws.size(), s_sequence, s_dropped);

  std::vector<std::pair<DrawKey, Entry>> draws(s_draws.begin(), s_draws.end());
  std::sort(draws.begin(), draws.end(),
            [](const auto& a, const auto& b) { return a.second.first_seen < b.second.first_seen; });
  for (const auto& [key, entry] : draws)
  {
    float z_range;
    float far_z;
    std::memcpy(&z_range, &key[11], sizeof(z_range));
    std::memcpy(&far_z, &key[12], sizeof(far_z));
    out += fmt::format("  {} {} {} | {:06x} {:06x} {:06x} {:06x} {:06x} {:06x} {:06x} {:06x} "
                       "{:06x} {:06x} {} {} {}\n",
                       entry.count, entry.indices, entry.first_seen, key[0], key[1], key[2],
                       key[3], key[4], key[5], key[6], key[7], key[8], key[9], key[10], z_range,
                       far_z);
  }

  out += fmt::format("EFB copies ({} distinct): count first | address width height format "
                     "flags(1 depth, 2 xfb, 4 to ram, 8 to vram, 16 intensity, 32 half)\n",
                     s_copies.size());
  std::vector<std::pair<CopyKey, Entry>> copies(s_copies.begin(), s_copies.end());
  std::sort(copies.begin(), copies.end(),
            [](const auto& a, const auto& b) { return a.second.first_seen < b.second.first_seen; });
  for (const auto& [key, entry] : copies)
  {
    out += fmt::format("  {} {} | {:08x} {} {} {:x} {:x}\n", entry.count, entry.first_seen,
                       key[0], key[1], key[2], key[3], key[4]);
  }

  out += "Flat draws of the first frames, in order. Coordinates run from -1 to 1 across the "
         "viewport.\n";
  out += s_detail;
  s_detail.clear();

  s_draws.clear();
  s_copies.clear();
  return out;
}
}  // namespace DrawStateLog
