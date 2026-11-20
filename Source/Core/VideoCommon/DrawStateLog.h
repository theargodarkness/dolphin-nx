// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Common/CommonTypes.h"

// Collects which combinations of GPU state a game drew with, and which EFB copies it made, while
// a profile capture runs. Comparing two captures of the same screen, one with an element that
// fails to appear and one without it, shows whether the game drew the element and how.
namespace DrawStateLog
{
void Start();
// Returns the report and stops recording.
std::string Finish();

// Both are called on the video thread and do nothing unless recording.
void RecordDraw(u32 index_count);
// Logs one orthographic draw in full for the first frames of a capture: where it lands on screen,
// its texture, and the colours its TEV stages combine. positions points at the first vertex's
// position, which is position_components floats.
void RecordFlatDraw(const u8* positions, u32 position_components, const u8* colors, u32 stride,
                    u32 vertex_count);
void RecordCopy(u32 dest_address, u32 width, u32 height, u32 format, bool depth, bool xfb,
                bool to_ram, bool to_vram, bool intensity, bool half_scale);
}  // namespace DrawStateLog
