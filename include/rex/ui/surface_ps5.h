#pragma once
/**
 * @file        ui/surface_ps5.h
 * @brief       PS5 presentation surface: an SDL window on the console's display
 *
 * The console has no window system. SDL's PS5 video driver
 * (tools/patches/sdl-ps5.patch) owns the one display and makes a
 * VK_KHR_display plane surface for a window, through the RADV linked into the
 * title, so the presenter asks SDL for the VkSurfaceKHR.
 */

#include <rex/ui/surface.h>

struct SDL_Window;

namespace rex {
namespace ui {

class SDLVulkanSurface final : public Surface {
 public:
  // `window` is borrowed: the WindowSDL that made this surface outlives it.
  explicit SDLVulkanSurface(SDL_Window* window) : window_(window) {}

  TypeIndex GetType() const override { return kTypeIndex_SDLVulkan; }
  SDL_Window* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  SDL_Window* window_;
};

}  // namespace ui
}  // namespace rex
