/**
 * @file        ui/surface_ps5.cpp
 * @brief       PS5 presentation surface (see surface_ps5.h)
 */

#include <rex/ui/surface_ps5.h>

#include <SDL3/SDL_video.h>

namespace rex {
namespace ui {

bool SDLVulkanSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  int width = 0;
  int height = 0;
  if (!window_ || !SDL_GetWindowSizeInPixels(window_, &width, &height) || width <= 0 ||
      height <= 0) {
    return false;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace ui
}  // namespace rex
