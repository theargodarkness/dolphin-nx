// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinSwitch/JoyConsSwitch.h"

#include <array>
#include <atomic>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <switch.h>

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/HW/Wiimote.h"
#include "Core/System.h"
#include "DolphinSwitch/SettingsSwitch.h"

namespace JoyCons
{
namespace
{
using ControllerProfiles::Kind;
using ControllerProfiles::SLOT_COUNT;

struct Split
{
  HidNpadIdType left;
  // Where the right Joy-Con went. Only firmware 5.0.0 and up says.
  std::optional<HidNpadIdType> right;
};

// Read by the host thread for the pause menu while Update runs, hence atomic.
std::array<std::atomic<Grip>, SLOT_COUNT> s_grips{};

// Only ever touched from the host thread.
std::vector<Split> s_splits;

constexpr u32 CONTROLLER_STYLES =
    HidNpadStyleTag_NpadFullKey | HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual |
    HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight | HidNpadStyleTag_NpadGc;

u32 GetStyle(HidNpadIdType id)
{
  return hidGetNpadStyleSet(id) & CONTROLLER_STYLES;
}

HidNpadIdType GetNpadId(int slot)
{
  return static_cast<HidNpadIdType>(HidNpadIdType_No1 + slot);
}

bool IsSplit(HidNpadIdType id)
{
  for (const Split& split : s_splits)
  {
    if (split.left == id || split.right == id)
      return true;
  }
  return false;
}

void SplitPair(HidNpadIdType id)
{
  // The left Joy-Con keeps the player number, and the system moves the right one to the first
  // free player, which is the next slot along when the pair was the last controller connected.
  bool has_destination = false;
  HidNpadIdType destination{};
  const Result result =
      hosversionAtLeast(5, 0, 0) ?
          hidSetNpadJoyAssignmentModeSingleWithDestination(id, HidNpadJoyDeviceType_Left,
                                                           &has_destination, &destination) :
          hidSetNpadJoyAssignmentModeSingle(id, HidNpadJoyDeviceType_Left);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "Could not split the Joy-Cons of player {}: {:#010x}",
                 id + 1, result);
    return;
  }

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Split the Joy-Cons of player {}; the right one is now {}",
                 id + 1,
                 has_destination ? fmt::format("player {}", destination + 1) : "another player");
  s_splits.push_back(
      Split{id, has_destination ? std::optional<HidNpadIdType>(destination) : std::nullopt});
}

void MergePair(const Split& split)
{
  // Joy-Cons that connect later pair up again either way.
  hidSetNpadJoyAssignmentModeDual(split.left);
  if (!split.right)
    return;
  hidSetNpadJoyAssignmentModeDual(*split.right);

  // Only put back what is still there. Either half may have been swapped or switched off since.
  const u32 left = GetStyle(split.left);
  const u32 right = GetStyle(*split.right);
  const bool is_pair =
      (left == HidNpadStyleTag_NpadJoyLeft && right == HidNpadStyleTag_NpadJoyRight) ||
      (left == HidNpadStyleTag_NpadJoyRight && right == HidNpadStyleTag_NpadJoyLeft);
  if (!is_pair)
    return;

  const Result result = hidMergeSingleJoyAsDualJoy(split.left, *split.right);
  if (R_FAILED(result))
  {
    WARN_LOG_FMT(CONTROLLERINTERFACE, "Could not pair the Joy-Cons of players {} and {}: {:#010x}",
                 split.left + 1, *split.right + 1, result);
  }
}

Grip ReadGrip(int slot)
{
  if (!Core::System::GetInstance().IsWii())
    return Grip::Paired;
  if (Config::Get(Config::GetInfoForWiimoteSource(slot)) != WiimoteSource::Emulated)
    return Grip::Paired;

  return ControllerProfiles::GetJoyConGrip(
      ControllerProfiles::GetActiveProfile(Kind::Wiimote, slot));
}

struct ButtonMove
{
  u64 from;
  u64 to;
};

// Upright, the stick already reports as itself, so only the D-pad has to become face buttons.
constexpr std::array LEFT_UPRIGHT{
    ButtonMove{HidNpadButton_Right, HidNpadButton_A},
    ButtonMove{HidNpadButton_Down, HidNpadButton_B},
    ButtonMove{HidNpadButton_Up, HidNpadButton_X},
    ButtonMove{HidNpadButton_Left, HidNpadButton_Y},
    ButtonMove{HidNpadButton_ZL, HidNpadButton_R},
};

constexpr std::array RIGHT_UPRIGHT{
    ButtonMove{HidNpadButton_StickRLeft, HidNpadButton_StickLLeft},
    ButtonMove{HidNpadButton_StickRUp, HidNpadButton_StickLUp},
    ButtonMove{HidNpadButton_StickRRight, HidNpadButton_StickLRight},
    ButtonMove{HidNpadButton_StickRDown, HidNpadButton_StickLDown},
    ButtonMove{HidNpadButton_ZR, HidNpadButton_L},
    ButtonMove{HidNpadButton_Plus, HidNpadButton_Minus},
};

// On its side, with the rail away from the player, a left Joy-Con is turned a quarter anticlockwise
// and a right one a quarter clockwise. This is the same turn the Horizon input backend applies.
constexpr std::array LEFT_SIDEWAYS{
    ButtonMove{HidNpadButton_Down, HidNpadButton_A},
    ButtonMove{HidNpadButton_Left, HidNpadButton_B},
    ButtonMove{HidNpadButton_Up, HidNpadButton_Y},
    ButtonMove{HidNpadButton_Right, HidNpadButton_X},
    ButtonMove{HidNpadButton_StickLRight, HidNpadButton_StickLUp},
    ButtonMove{HidNpadButton_StickLLeft, HidNpadButton_StickLDown},
    ButtonMove{HidNpadButton_StickLUp, HidNpadButton_StickLLeft},
    ButtonMove{HidNpadButton_StickLDown, HidNpadButton_StickLRight},
    ButtonMove{HidNpadButton_LeftSL, HidNpadButton_L},
    ButtonMove{HidNpadButton_LeftSR, HidNpadButton_R},
};

constexpr std::array RIGHT_SIDEWAYS{
    ButtonMove{HidNpadButton_X, HidNpadButton_A},
    ButtonMove{HidNpadButton_A, HidNpadButton_B},
    ButtonMove{HidNpadButton_B, HidNpadButton_Y},
    ButtonMove{HidNpadButton_Y, HidNpadButton_X},
    ButtonMove{HidNpadButton_StickRLeft, HidNpadButton_StickLUp},
    ButtonMove{HidNpadButton_StickRRight, HidNpadButton_StickLDown},
    ButtonMove{HidNpadButton_StickRDown, HidNpadButton_StickLLeft},
    ButtonMove{HidNpadButton_StickRUp, HidNpadButton_StickLRight},
    ButtonMove{HidNpadButton_RightSL, HidNpadButton_L},
    ButtonMove{HidNpadButton_RightSR, HidNpadButton_R},
    ButtonMove{HidNpadButton_Plus, HidNpadButton_Minus},
};

u64 Apply(std::span<const ButtonMove> moves, u64 buttons)
{
  u64 moved = 0;
  u64 kept = buttons;
  for (const ButtonMove& move : moves)
  {
    if (buttons & move.from)
      moved |= move.to;
    kept &= ~move.from;
  }
  return kept | moved;
}
}  // namespace

void Update()
{
  for (int slot = 0; slot < SLOT_COUNT; ++slot)
    s_grips[slot].store(ReadGrip(slot), std::memory_order_relaxed);

  // Pairs whose first player went back to a profile that needs both halves.
  std::erase_if(s_splits, [](const Split& split) {
    const int slot = split.left - HidNpadIdType_No1;
    if (slot < SLOT_COUNT && GetGrip(slot) != Grip::Paired)
      return false;
    MergePair(split);
    return true;
  });

  if (!Config::Get(Config::SWITCH_SPLIT_JOYCONS))
    return;

  for (int slot = 0; slot < SLOT_COUNT; ++slot)
  {
    const HidNpadIdType id = GetNpadId(slot);
    if (GetGrip(slot) == Grip::Paired || IsSplit(id))
      continue;
    if (GetStyle(id) == HidNpadStyleTag_NpadJoyDual)
      SplitPair(id);
  }
}

void Restore()
{
  for (std::atomic<Grip>& grip : s_grips)
    grip.store(Grip::Paired, std::memory_order_relaxed);

  for (const Split& split : s_splits)
    MergePair(split);
  s_splits.clear();
}

Grip GetGrip(int slot)
{
  return s_grips.at(slot).load(std::memory_order_relaxed);
}

u64 TranslateMenuButtons(u32 style_set, u64 buttons, Grip grip)
{
  const bool sideways = grip == Grip::Sideways;
  switch (style_set & CONTROLLER_STYLES)
  {
  case HidNpadStyleTag_NpadJoyLeft:
    return sideways ? Apply(LEFT_SIDEWAYS, buttons) : Apply(LEFT_UPRIGHT, buttons);
  case HidNpadStyleTag_NpadJoyRight:
    return sideways ? Apply(RIGHT_SIDEWAYS, buttons) : Apply(RIGHT_UPRIGHT, buttons);
  default:
    return buttons;
  }
}
}  // namespace JoyCons
