// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "DolphinSwitch/ControllerProfilesSwitch.h"

// Lets each half of a Joy-Con pair be its own player while a Wii Remote profile asks for a single
// Joy-Con, and puts the pair back together afterwards.
namespace JoyCons
{
using Grip = ControllerProfiles::JoyConGrip;

// Re-reads the Wii Remote profiles of the running game, then splits or re-pairs Joy-Cons to
// match. Call on the host thread once a game has booted and whenever a profile changes.
void Update();

// Re-pairs everything Update split. Call on the host thread when the game ends.
void Restore();

// The grip of the profile in a Wii Remote slot, as of the last Update.
Grip GetGrip(int slot);

// A lone Joy-Con has half the buttons the pause menu listens for. Given the style set and buttons
// of the pad that drives the menu, returns the buttons a whole controller would have reported:
// the stick moves the selection, the face buttons under the thumb are A and B, and holding Plus
// counts as holding Minus. Anything other than a lone Joy-Con comes back untouched.
u64 TranslateMenuButtons(u32 style_set, u64 buttons, Grip grip);
}  // namespace JoyCons
