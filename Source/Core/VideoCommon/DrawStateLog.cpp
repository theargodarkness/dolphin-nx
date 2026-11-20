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

#include "VideoCommon/BPMemory.h"
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

  s_draws.clear();
  s_copies.clear();
  return out;
}
}  // namespace DrawStateLog
