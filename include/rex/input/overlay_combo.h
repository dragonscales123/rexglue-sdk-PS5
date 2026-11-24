/**
 * @file        rex/input/overlay_combo.h
 *
 * @brief       Controller combo (L3 + R3) that toggles the performance overlay,
 *              for platforms without a keyboard (the PS5).
 */
#pragma once

#include <cstdint>

namespace rex::input {

// Called on the input thread when L3 and R3 are both held down; the callback
// must hand the work to the UI thread itself.
using OverlayComboCallback = void (*)(void* context);
void SetOverlayComboCallback(OverlayComboCallback callback, void* context);
void FireOverlayCombo();

// The in-game settings menu (the DualSense touchpad, or Create + Options).
// Called on the input thread; the callback hands its work to the UI thread.
using MenuButtonCallback = void (*)(void* context);
void SetMenuButtonCallback(MenuButtonCallback callback, void* context);
void FireMenuButton();

// While the menu has the controller the game sees an idle pad, and each
// button press (XInput button bit) goes to the navigation callback instead.
using MenuNavCallback = void (*)(void* context, uint16_t xinput_button);
void SetMenuNavCallback(MenuNavCallback callback, void* context);
void SetMenuInputCaptured(bool captured);
bool IsMenuInputCaptured();
void DispatchMenuNav(uint16_t xinput_button);

}  // namespace rex::input
