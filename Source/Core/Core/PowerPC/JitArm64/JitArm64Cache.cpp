// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/JitArm64Cache.h"

#include <algorithm>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/MemoryUtil.h"
#include "Common/RangeSet.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/JitArm64/Jit.h"
#include "Core/PowerPC/JitCommon/JitBase.h"

using namespace Arm64Gen;

JitArm64BlockCache::JitArm64BlockCache(JitBase& jit) : JitBaseBlockCache{jit}
{
}

bool JitArm64BlockCache::Init()
{
  ClearRangesToFree();
  ForgetSetAsideBlocks();
  return JitBaseBlockCache::Init();
}

void JitArm64BlockCache::WriteLinkBlock(Arm64Gen::ARM64XEmitter& emit,
                                        const JitBlock::LinkData& source, const JitBlock* dest)
{
  const u8* start = emit.GetCodePtr();

  if (!dest)
  {
    emit.MOVI2R(DISPATCHER_PC, source.exitAddress);
    if (source.call)
    {
      if (emit.GetCodePtr() == start + BLOCK_LINK_FAST_BL_OFFSET - sizeof(u32))
        emit.NOP();
      DEBUG_ASSERT(emit.GetCodePtr() == start + BLOCK_LINK_FAST_BL_OFFSET || emit.HasWriteFailed());
      emit.BL(m_jit.GetAsmRoutines()->dispatcher);
    }
    else
    {
      emit.B(m_jit.GetAsmRoutines()->dispatcher);
    }
  }
  else
  {
    if (source.call)
    {
      // The "fast" BL should be the last instruction, so that the return address matches the
      // address that was pushed onto the stack by the function that called WriteLinkBlock
      FixupBranch fast = emit.B(CC_GT);
      emit.B(source.exitFarcode);
      DEBUG_ASSERT(emit.GetCodePtr() == start + BLOCK_LINK_FAST_BL_OFFSET || emit.HasWriteFailed());
      emit.SetJumpTarget(fast);
      emit.BL(dest->normalEntry);
    }
    else
    {
      // Are we able to jump directly to the block?
      s64 block_distance = ((s64)dest->normalEntry - (s64)emit.GetCodePtr()) >> 2;
      if (block_distance >= -0x40000 && block_distance <= 0x3FFFF)
      {
        emit.B(CC_GT, dest->normalEntry);
        emit.B(source.exitFarcode);
      }
      else
      {
        FixupBranch slow = emit.B(CC_LE);
        emit.B(dest->normalEntry);
        emit.SetJumpTarget(slow);
        emit.B(source.exitFarcode);
      }
    }
  }

  // Use a fixed number of instructions so we have enough room for any patching needed later.
  const u8* end = start + BLOCK_LINK_SIZE;
  while (emit.GetCodePtr() < end)
  {
    emit.BRK(101);
    if (emit.HasWriteFailed())
      return;
  }
  ASSERT(emit.GetCodePtr() == end);
}

void JitArm64BlockCache::WriteLinkBlock(const JitBlock::LinkData& source, const JitBlock* dest)
{
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  u8* location = source.exitPtrs;
  ARM64XEmitter emit(location, location + BLOCK_LINK_SIZE);

  WriteLinkBlock(emit, source, dest);
  emit.FlushIcache();
}

void JitArm64BlockCache::WriteDestroyBlock(const JitBlock& block)
{
  // Only clear the entry point as we might still be within this block.
  ARM64XEmitter emit(block.normalEntry, block.normalEntry + 4);
  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  emit.BRK(0x123);
  emit.FlushIcache();
}

void JitArm64BlockCache::QueueRangesToFree(const JitBlockData& data)
{
  if (data.near_begin != data.near_end)
    m_ranges_to_free_on_next_codegen_near.emplace_back(data.near_begin, data.near_end);
  if (data.far_begin != data.far_end)
    m_ranges_to_free_on_next_codegen_far.emplace_back(data.far_begin, data.far_end);
}

bool JitArm64BlockCache::KeepsOriginalBuffer() const
{
  return m_jit.m_reuse_translated_code;
}

bool JitArm64BlockCache::CanSetAside(const JitBlock& block) const
{
  // Profiled blocks have the address of their JitBlock compiled in.
  return m_guest_invalidation && m_jit.m_reuse_translated_code && !m_jit.IsDebuggingEnabled() &&
         block.near_begin != block.near_end && !block.original_buffer.empty() &&
         block.original_buffer.size() == block.originalSize;
}

void JitArm64BlockCache::DestroyBlock(JitBlock& block)
{
  if (!CanSetAside(block))
  {
    JitBaseBlockCache::DestroyBlock(block);
    QueueRangesToFree(block);
    return;
  }

  u32 entry_instruction;
  std::memcpy(&entry_instruction, block.normalEntry, sizeof(entry_instruction));

  JitBaseBlockCache::DestroyBlock(block);

  const u64 key = SetAsideKey(block.effectiveAddress, block.feature_flags);
  if (const auto old = m_set_aside_blocks.find(key); old != m_set_aside_blocks.end())
    RemoveSetAsideBlock(old, true);

  SetAsideBlock& entry = m_set_aside_blocks[key];
  entry.data = static_cast<const JitBlockData&>(block);
  entry.link_data = std::move(block.linkData);
  entry.code = std::move(block.original_buffer);
  entry.first_address = entry.code.front().first;
  entry.last_address = entry.code.front().first;
  for (const auto& [address, inst] : entry.code)
  {
    entry.first_address = std::min(entry.first_address, address);
    entry.last_address = std::max(entry.last_address, address);
  }
  entry.entry_instruction = entry_instruction;
  entry.serial = ++m_set_aside_serial;

  m_set_aside_order.emplace_back(key, entry.serial);
  m_set_aside_code_size +=
      (entry.data.near_end - entry.data.near_begin) + (entry.data.far_end - entry.data.far_begin);
  s_blocks_set_aside.fetch_add(1, std::memory_order_relaxed);

  // Give up the oldest ones once there are too many. Entries whose block has since been reused or
  // replaced are still in the queue, and are recognisable by their serial.
  while (!m_set_aside_order.empty() &&
         (m_set_aside_blocks.size() > MAX_SET_ASIDE_BLOCKS ||
          m_set_aside_code_size > MAX_SET_ASIDE_CODE_SIZE ||
          m_set_aside_order.size() > MAX_SET_ASIDE_BLOCKS * 4))
  {
    const auto [old_key, old_serial] = m_set_aside_order.front();
    m_set_aside_order.pop_front();
    const auto it = m_set_aside_blocks.find(old_key);
    if (it != m_set_aside_blocks.end() && it->second.serial == old_serial)
      RemoveSetAsideBlock(it, true);
  }
}

void JitArm64BlockCache::RemoveSetAsideBlock(std::pmr::unordered_map<u64, SetAsideBlock>::iterator it,
                                             bool free_code)
{
  const JitBlockData& data = it->second.data;
  m_set_aside_code_size -= (data.near_end - data.near_begin) + (data.far_end - data.far_begin);
  if (free_code)
    QueueRangesToFree(data);
  m_set_aside_blocks.erase(it);
}

void JitArm64BlockCache::ForgetSetAsideBlocks()
{
  m_set_aside_blocks.clear();
  m_set_aside_order.clear();
  m_set_aside_code_size = 0;
}

bool JitArm64BlockCache::FreeSetAsideBlocks()
{
  if (m_set_aside_blocks.empty())
    return false;

  for (const auto& [key, entry] : m_set_aside_blocks)
    QueueRangesToFree(entry.data);
  ForgetSetAsideBlocks();
  return true;
}

void JitArm64BlockCache::OnForcedInvalidation(u32 address, u32 length)
{
  if (m_set_aside_blocks.empty())
    return;

  // Whatever caused this (a patch, a breakpoint, a newly needed exception check) changes how the
  // code has to be translated without changing the code, which reusing a block would not notice.
  const u64 start = address;
  const u64 end = start + length;
  for (auto it = m_set_aside_blocks.begin(); it != m_set_aside_blocks.end();)
  {
    const auto current = it++;
    if (current->second.first_address < end && u64{current->second.last_address} + 4 > start)
      RemoveSetAsideBlock(current, true);
  }
}

bool JitArm64BlockCache::TryReuseBlock(u32 em_address, bool block_link)
{
  if (m_set_aside_blocks.empty())
    return false;

  const auto it =
      m_set_aside_blocks.find(SetAsideKey(em_address, m_jit.m_ppc_state.feature_flags));
  if (it == m_set_aside_blocks.end())
    return false;

  // The translation is only good if every instruction it was made from is back where it was.
  Common::RangeSet<u32> physical_addresses;
  for (const auto& [address, inst] : it->second.code)
  {
    const PowerPC::TryReadInstResult result = m_jit.m_mmu.TryReadInstruction(address);
    if (result.valid && result.hex == inst.hex)
    {
      physical_addresses.insert(result.physical_address,
                                result.physical_address + sizeof(UGeckoInstruction));
      continue;
    }

    // If the start of the block is not mapped, the game is about to be asked to page it in, and
    // the block is tried again after that.
    if (!result.valid && !m_jit.m_mmu.TryReadInstruction(em_address).valid)
      return false;

    RemoveSetAsideBlock(it, true);
    s_blocks_not_reusable.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  SetAsideBlock entry = std::move(it->second);
  RemoveSetAsideBlock(it, false);

  JitBlock* const block = AllocateBlock(em_address);
  block->near_begin = entry.data.near_begin;
  block->near_end = entry.data.near_end;
  block->far_begin = entry.data.far_begin;
  block->far_end = entry.data.far_end;
  block->normalEntry = entry.data.normalEntry;
  block->originalSize = entry.data.originalSize;
  block->linkData = std::move(entry.link_data);
  // Destroying the block pointed all of its exits back at the dispatcher.
  for (JitBlock::LinkData& link : block->linkData)
    link.linkStatus = false;
  block->original_buffer = std::move(entry.code);
  block->physical_addresses = std::move(physical_addresses);

  {
    struct RawEmitter : ARM64XEmitter
    {
      using ARM64XEmitter::ARM64XEmitter;
      using ARM64XEmitter::Write32;
    };

    const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
    RawEmitter emit(block->normalEntry, block->normalEntry + sizeof(u32));
    emit.Write32(entry.entry_instruction);
    emit.FlushIcache();
  }

  RegisterBlock(*block, block_link);
  s_blocks_reused.fetch_add(1, std::memory_order_relaxed);
  return true;
}

const std::vector<std::pair<u8*, u8*>>& JitArm64BlockCache::GetRangesToFreeNear() const
{
  return m_ranges_to_free_on_next_codegen_near;
}

const std::vector<std::pair<u8*, u8*>>& JitArm64BlockCache::GetRangesToFreeFar() const
{
  return m_ranges_to_free_on_next_codegen_far;
}

void JitArm64BlockCache::ClearRangesToFree()
{
  m_ranges_to_free_on_next_codegen_near.clear();
  m_ranges_to_free_on_next_codegen_far.clear();
}
