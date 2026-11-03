/**
 * @file        rex/input/overlay_combo.h
 *
 * @brief       Controller combo (L3 + R3) that toggles the performance overlay,
 *              for platforms without a keyboard (the PS5).
 */
#pragma once

namespace rex::input {

// Called on the input thread when L3 and R3 are both held down; the callback
// must hand the work to the UI thread itself.
using OverlayComboCallback = void (*)(void* context);
void SetOverlayComboCallback(OverlayComboCallback callback, void* context);
void FireOverlayCombo();

}  // namespace rex::input
