// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PowerPC/JitArm64/Jit.h"

#include <array>

#include "Common/Arm64Emitter.h"
#include "Common/CommonTypes.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/JitArm64/JitArm64_RegCache.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"

using namespace Arm64Gen;

namespace
{
// __ieee754_sqrt as built into the Metrowerks runtime of Wii Sports + Wii Sports Resort. It finds
// the root one bit at a time with integer arithmetic, which comes to several hundred instructions
// a call, and the game calls it constantly.
//
// The five words in MASKED_WORDS hold addresses that differ from game to game (errno, a NaN
// constant and the constant one), so their low halves are zeroed here and ignored when matching.
constexpr std::array<u32, 146> SQRT_ROUTINE{
    0x9421ffe0, 0xd8210008, 0x80c10008, 0x8001000c, 0x54c30056, 0x3c638010,
    0x28030000, 0x40820018, 0xfc010072, 0x38000021, 0x900d0000, 0xfc21002a,
    0x48000210, 0x2c860000, 0x4185002c, 0x54c3007e, 0x7c031b79, 0x40820008,
    0x480001f8, 0x40840018, 0x3c600000, 0x38000021, 0x900d0000, 0xc0230000,
    0x480001e0, 0x7cc3a671, 0x40820050, 0x48000014, 0x5404aafe, 0x5400a814,
    0x7cc62378, 0x3863ffeb, 0x2c060000, 0x4182ffec, 0x38e00000, 0x4800000c,
    0x54c6083c, 0x38e70001, 0x54c402d7, 0x4182fff4, 0x20870020, 0x38a7ffff,
    0x7c042430, 0x7c003830, 0x7c651850, 0x7cc62378, 0x3883fc01, 0x54c5033e,
    0x548407ff, 0x64a60010, 0x41820014, 0x54050ffe, 0x7c863214, 0x7cc52214,
    0x7c000214, 0x54050ffe, 0x7c863214, 0x7cc52214, 0x7c000214, 0x39200000,
    0x39600000, 0x39400000, 0x39800000, 0x3ce00020, 0x48000030, 0x7c8b3a14,
    0x7c043000, 0x41810010, 0x7d643a14, 0x7cc43050, 0x7d8c3a14, 0x54050ffe,
    0x7c863214, 0x7cc52214, 0x7c000214, 0x54e7f87e, 0x2c070000, 0x4082ffd0,
    0x3ce08000, 0x4800006c, 0x7c0b3000, 0x7d655b78, 0x7d093a14, 0x41800010,
    0x40820044, 0x7c080040, 0x4181003c, 0x55040000, 0x7d283a14, 0x3c848000,
    0x28040000, 0x40820010, 0x55240001, 0x40820008, 0x396b0001, 0x7c004040,
    0x7cc53050, 0x40800008, 0x38c6ffff, 0x7c080050, 0x7d4a3a14, 0x54050ffe,
    0x7c863214, 0x7cc52214, 0x7c000214, 0x54e7f87e, 0x2c070000, 0x4082ff94,
    0x7cc00379, 0x41820058, 0xc8020000, 0xd8010010, 0xfc000040, 0x4c411382,
    0x40820044, 0x3c0a0001, 0xd8010010, 0x2800ffff, 0x40820010, 0x39400000,
    0x398c0001, 0x48000028, 0xfc000040, 0x40810018, 0x2800fffe, 0x40820008,
    0x398c0001, 0x394a0002, 0x4800000c, 0x554007fe, 0x7d4a0214, 0x558007fe,
    0x7d840e70, 0x2c000001, 0x5545f87e, 0x3c843fe0, 0x40820008, 0x64a58000,
    0x3803fc01, 0x90a10014, 0x54009816, 0x7c840214, 0x90810010, 0xc8210010,
    0x38210020, 0x4e800020,
};
constexpr std::array<u32, 5> MASKED_WORDS{10, 20, 22, 23, 110};

// What a call costs the guest, to keep emulated time where it would have been. The routine runs
// its two loops 22 and 32 times whatever the argument.
constexpr u32 SQRT_ROUTINE_CYCLES = 850;

bool IsMasked(size_t index)
{
  for (const u32 masked : MASKED_WORDS)
  {
    if (masked == index)
      return true;
  }
  return false;
}
}  // namespace

void JitArm64::EmitNativeSqrt(u32 em_address)
{
  // The routine leaves FPSCR's compare bits in a particular state, which only matters if they are
  // being emulated.
  if (!m_native_sqrt || m_fprf || jo.fp_exceptions || IsDebuggingEnabled())
    return;

  for (size_t i = 0; i < SQRT_ROUTINE.size(); ++i)
  {
    const auto word = m_mmu.TryReadInstruction(em_address + static_cast<u32>(i * sizeof(u32)));
    if (!word.valid)
      return;
    const u32 mask = IsMasked(i) ? 0xFFFF0000 : 0xFFFFFFFF;
    if ((word.hex & mask) != SQRT_ROUTINE[i])
      return;
  }

  // The routine and the host agree exactly on every positive normal number when rounding to
  // nearest. Everything else takes the routine itself, which follows this: zeroes, denormals,
  // negative numbers, infinities and NaNs (some of which set errno), other rounding modes, and
  // the floating point unavailable exception that the OS uses to switch FPU contexts lazily.
  // Nothing is cached in host registers this early in a block.
  LDR(IndexType::Unsigned, ARM64Reg::W1, PPC_REG, PPCSTATE_OFF(msr));
  FixupBranch fp_unavailable = TBZ(ARM64Reg::W1, 13);

  LDR(IndexType::Unsigned, ARM64Reg::X0, PPC_REG, PPCSTATE_OFF_PS0(1));
  LSR(ARM64Reg::X1, ARM64Reg::X0, 52);
  SUB(ARM64Reg::W1, ARM64Reg::W1, 1);
  CMP(ARM64Reg::W1, 0x7FE);
  FixupBranch not_positive_normal = B(CC_HS);

  LDR(IndexType::Unsigned, ARM64Reg::W1, PPC_REG, PPCSTATE_OFF(fpscr));
  TST(ARM64Reg::W1, LogicalImm(3, GPRSize::B32));
  FixupBranch not_round_to_nearest = B(CC_NEQ);

  m_float_emit.FMOV(ARM64Reg::D0, ARM64Reg::X0);
  m_float_emit.FSQRT(ARM64Reg::D0, ARM64Reg::D0);
  m_float_emit.STR(64, IndexType::Unsigned, ARM64Reg::D0, PPC_REG, PPCSTATE_OFF_PS0(1));

  // Return to the caller the way blr would.
  LDR(IndexType::Unsigned, DISPATCHER_PC, PPC_REG, PPCSTATE_OFF_SPR(SPR_LR));
  AND(DISPATCHER_PC, DISPATCHER_PC, LogicalImm(~0x3, GPRSize::B32));

  js.downcountAmount = SQRT_ROUTINE_CYCLES;
  WriteBLRExit(DISPATCHER_PC);
  js.downcountAmount = 0;

  SetJumpTarget(fp_unavailable);
  SetJumpTarget(not_positive_normal);
  SetJumpTarget(not_round_to_nearest);
}
