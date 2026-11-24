/**
 * @file        rex/ui/overlay/graphics_menu.h
 *
 * @brief       Controller-driven graphics settings menu for the console (no
 *              mouse or keyboard): render scale, texture filtering,
 *              anti-aliasing and frame cap, saved to a settings file next to
 *              the executable that the title loads at startup.
 */
#pragma once

#include <cstdint>
#include <string>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// Queues a controller button press (an XInput button bit) for the menu. Safe
// from any thread; the menu reads the queue when it draws.
void GraphicsMenuPushInput(uint16_t xinput_button);

// The settings file the menu writes: REX_<CVAR>=value lines, read by the
// title at startup after its own environment file.
std::string GraphicsMenuSettingsPath();

class GraphicsMenuDialog : public ImGuiDialog {
 public:
  explicit GraphicsMenuDialog(ImGuiDrawer* imgui_drawer);
  ~GraphicsMenuDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;
};

}  // namespace rex::ui
