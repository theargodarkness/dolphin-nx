// Copyright 2026 Dolphin Emulator Project
// Copyright 2026 PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>

#include <deko3d.hpp>

#include "Common/CommonTypes.h"

#include "VideoCommon/TextureConfig.h"

namespace Deko3D
{
class DKFramebuffer;
class DKTexture;

// Wraps a DkSwapchain over the libnx nwindow.
class DKSwapChain
{
public:
  // Three, not two. With two, the image to draw the next frame into is the one on screen, which
  // the display only lets go of at the vertical blank after the frame before has finished, and
  // deko3d makes the GPU wait for that. A frame that takes the GPU a little over one refresh then
  // costs two, so anything heavier than 16.7 ms dropped a game from 60 to 30 frames a second with
  // the GPU and both emulation threads half idle. The third image is free whenever that happens.
  // This is the most there can be. The triple buffering setting decides between it and two.
  static constexpr u32 NUM_IMAGES = 3;
  static constexpr DkImageFormat FORMAT = DkImageFormat_RGBA8_Unorm;

  ~DKSwapChain();

  static std::unique_ptr<DKSwapChain> Create(void* native_window);

  u32 GetWidth() const { return m_width; }
  u32 GetHeight() const { return m_height; }
  const dk::Image& GetImage(int slot) const { return m_images[slot]; }

  // Each slot is wrapped in a framebuffer so the presenter can draw the XFB blit and the UI through
  // the ordinary pipeline rather than a backbuffer-only path.
  DKFramebuffer* GetFramebuffer(int slot) const { return m_framebuffers[slot].get(); }

  static AbstractTextureFormat GetTextureFormat() { return AbstractTextureFormat::RGBA8; }

  // Blocks until an image is free, returning its slot, or -1 on error.
  int Acquire();
  void Present(int slot);

  // Rebuilds the images/swapchain for a new window size. Returns false on failure.
  bool Recreate();

private:
  bool Initialize();
  void Destroy();

  void* m_native_window = nullptr;
  u32 m_width = 0;
  u32 m_height = 0;

  dk::UniqueMemBlock m_image_memblock;
  u32 m_num_images = NUM_IMAGES;
  std::array<dk::Image, NUM_IMAGES> m_images;
  std::array<std::unique_ptr<DKTexture>, NUM_IMAGES> m_textures;
  std::array<std::unique_ptr<DKFramebuffer>, NUM_IMAGES> m_framebuffers;
  dk::UniqueSwapchain m_swapchain;
};
}  // namespace Deko3D
