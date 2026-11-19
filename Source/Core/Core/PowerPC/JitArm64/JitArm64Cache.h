// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/Arm64Emitter.h"
#include "Core/PowerPC/JitCommon/JitCache.h"

class JitBase;

typedef void (*CompiledCode)();

class JitArm64BlockCache : public JitBaseBlockCache
{
public:
  explicit JitArm64BlockCache(JitBase& jit);

  [[nodiscard]] bool Init() override;

  void DestroyBlock(JitBlock& block) override;

  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeNear() const;
  const std::vector<std::pair<u8*, u8*>>& GetRangesToFreeFar() const;

  void ClearRangesToFree();

  // Games that page their own code in and out invalidate code only to bring the very same code
  // back a moment later. Instead of freeing the translation of such code, it is set aside, and
  // TryReuseBlock puts it back in service if the guest code at that address is unchanged.
  // Returns true if the block at em_address is ready to run again.
  bool TryReuseBlock(u32 em_address, bool block_link);
  // Forgets every set aside block without freeing its code, for when the code space is reset.
  void ForgetSetAsideBlocks();
  // Queues the code of every set aside block for freeing. Returns false if there was none.
  bool FreeSetAsideBlocks();

  void WriteLinkBlock(Arm64Gen::ARM64XEmitter& emit, const JitBlock::LinkData& source,
                      const JitBlock* dest = nullptr);

  static constexpr size_t BLOCK_LINK_SIZE = 3 * sizeof(u32);
  static constexpr size_t BLOCK_LINK_FAST_BL_OFFSET = BLOCK_LINK_SIZE - sizeof(u32);

private:
  void WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest) override;
  void WriteDestroyBlock(const JitBlock& block) override;
  void OnForcedInvalidation(u32 address, u32 length) override;
  bool KeepsOriginalBuffer() const override;

  struct SetAsideBlock
  {
    JitBlockData data;
    std::vector<JitBlock::LinkData> link_data;
    std::vector<std::pair<u32, UGeckoInstruction>> code;
    // The lowest and highest guest address in code.
    u32 first_address;
    u32 last_address;
    // The host instruction at the entry point, which destroying a block overwrites.
    u32 entry_instruction;
    u64 serial;
  };

  static constexpr size_t MAX_SET_ASIDE_BLOCKS = 0x8000;
  static constexpr size_t MAX_SET_ASIDE_CODE_SIZE = 24 * 1024 * 1024;

  static u64 SetAsideKey(u32 em_address, CPUEmuFeatureFlags feature_flags)
  {
    return (u64{feature_flags} << 32) | em_address;
  }
  bool CanSetAside(const JitBlock& block) const;
  void QueueRangesToFree(const JitBlockData& data);
  void RemoveSetAsideBlock(std::unordered_map<u64, SetAsideBlock>::iterator it, bool free_code);

  std::unordered_map<u64, SetAsideBlock> m_set_aside_blocks;
  // Keys in the order they were set aside, with the serial that tells stale entries apart.
  std::deque<std::pair<u64, u64>> m_set_aside_order;
  size_t m_set_aside_code_size = 0;
  u64 m_set_aside_serial = 0;

  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen_near;
  std::vector<std::pair<u8*, u8*>> m_ranges_to_free_on_next_codegen_far;
};
