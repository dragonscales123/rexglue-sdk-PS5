/**
 * @file        ui/overlay/graphics_menu.cpp
 *
 * @brief       Controller-driven graphics settings menu. See graphics_menu.h.
 */
#include <rex/ui/overlay/graphics_menu.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

namespace rex::ui {

namespace {

// XInput button bits (the menu receives raw XInput buttons).
constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;
constexpr uint16_t kButtonA = 0x1000;

std::mutex input_mutex;
std::vector<uint16_t> input_queue;

struct Option {
  const char* label;
  int value;
};

constexpr std::array<Option, 2> kScaleOptions = {{
    {"1x (1280x720)", 1},
    {"2x (2560x1440)", 2},
}};
constexpr std::array<Option, 6> kAnisoOptions = {{
    {"Game default", -1},
    {"Off", 0},
    {"2x anisotropic", 2},
    {"4x anisotropic", 3},
    {"8x anisotropic", 4},
    {"16x anisotropic", 5},
}};
constexpr std::array<const char*, 3> kAaValues = {"none", "fxaa", "fxaa_extreme"};
constexpr std::array<const char*, 3> kAaLabels = {"Off", "FXAA", "FXAA Extreme"};
constexpr std::array<const char*, 3> kFpsLabels = {"30 fps", "60 fps", "Uncapped (no vsync)"};

struct Values {
  int scale = 0;  // Index into kScaleOptions.
  int aniso = 0;  // Index into kAnisoOptions.
  int aa = 0;     // Index into kAaValues.
  int fps = 0;    // Index into kFpsLabels.
  bool operator==(const Values& o) const {
    return scale == o.scale && aniso == o.aniso && aa == o.aa && fps == o.fps;
  }
};

struct Preset {
  const char* label;
  Values values;
};
constexpr std::array<Preset, 3> kPresets = {{
    {"Performance", {0, 3, 0, 1}},
    {"Balanced", {1, 3, 0, 1}},
    {"Quality", {1, 5, 1, 1}},
}};

enum Row { kRowPreset, kRowScale, kRowAniso, kRowAa, kRowFps, kRowCount };

// Kept across openings of the menu within a run.
struct State {
  bool loaded = false;
  int running_scale = 0;  // Index of the scale this run started with.
  Values values;
  int selected = kRowPreset;
  std::string status;
} state;

int FlagInt(const char* name, int fallback) {
  const std::string value = rex::cvar::GetFlagByName(name);
  if (value.empty()) {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  return end == value.c_str() ? fallback : int(parsed);
}

bool FlagBool(const char* name, bool fallback) {
  const std::string value = rex::cvar::GetFlagByName(name);
  if (value.empty()) {
    return fallback;
  }
  return value == "true" || value == "1";
}

template <size_t N>
int IndexOfValue(const std::array<Option, N>& options, int value, int fallback) {
  for (size_t i = 0; i < N; ++i) {
    if (options[i].value == value) {
      return int(i);
    }
  }
  return fallback;
}

void LoadFromFlags() {
  Values& v = state.values;
  v.scale = IndexOfValue(kScaleOptions, FlagInt("draw_resolution_scale_x", 1), 0);
  v.aniso = IndexOfValue(kAnisoOptions, FlagInt("anisotropic_override", 3), 3);
  const std::string aa = rex::cvar::GetFlagByName("swap_post_effect");
  v.aa = 0;
  for (size_t i = 0; i < kAaValues.size(); ++i) {
    if (aa == kAaValues[i]) {
      v.aa = int(i);
    }
  }
  if (!FlagBool("vsync", true)) {
    v.fps = 2;
  } else {
    v.fps = FlagInt("guest_vblank_multiplier", 1) >= 2 ? 1 : 0;
  }
  state.running_scale = v.scale;
  state.loaded = true;
}

void ApplyLive(const Values& v) {
  rex::cvar::SetFlagByName("anisotropic_override",
                           std::to_string(kAnisoOptions[v.aniso].value));
  rex::cvar::SetFlagByName("swap_post_effect", kAaValues[v.aa]);
  rex::cvar::SetFlagByName("vsync", v.fps == 2 ? "false" : "true");
  rex::cvar::SetFlagByName("guest_vblank_multiplier", v.fps == 0 ? "1" : "2");
}

bool Save(const Values& v) {
  const std::string path = GraphicsMenuSettingsPath();
  const std::string temp_path = path + ".tmp";
  FILE* file = std::fopen(temp_path.c_str(), "w");
  if (!file) {
    REXLOG_WARN("[menu] can't write {}: errno {}", temp_path, errno);
    return false;
  }
  const int scale = kScaleOptions[v.scale].value;
  std::fprintf(file,
               "# Written by the in-game graphics menu (touchpad, or Create + Options).\n"
               "# Loaded at startup after ps5-env.txt, so these settings win.\n"
               "REX_DRAW_RESOLUTION_SCALE_X=%d\n"
               "REX_DRAW_RESOLUTION_SCALE_Y=%d\n"
               "REX_ANISOTROPIC_OVERRIDE=%d\n"
               "REX_SWAP_POST_EFFECT=%s\n"
               "REX_VSYNC=%s\n"
               "REX_GUEST_VBLANK_MULTIPLIER=%d\n",
               scale, scale, kAnisoOptions[v.aniso].value, kAaValues[v.aa],
               v.fps == 2 ? "false" : "true", v.fps == 0 ? 1 : 2);
  const bool write_ok = std::fflush(file) == 0;
  const bool close_ok = std::fclose(file) == 0;
  if (!write_ok || !close_ok || std::rename(temp_path.c_str(), path.c_str()) != 0) {
    REXLOG_WARN("[menu] can't save {}: errno {}", path, errno);
    std::remove(temp_path.c_str());
    return false;
  }
  REXLOG_INFO("[menu] saved: scale {}x, aniso {}, aa {}, fps {}", scale,
              kAnisoOptions[v.aniso].value, kAaValues[v.aa], kFpsLabels[v.fps]);
  return true;
}

int PresetIndex(const Values& v) {
  for (size_t i = 0; i < kPresets.size(); ++i) {
    if (kPresets[i].values == v) {
      return int(i);
    }
  }
  return -1;
}

int Step(int index, int count, int direction) {
  return (index + direction + count) % count;
}

void Change(int row, int direction) {
  Values v = state.values;
  switch (row) {
    case kRowPreset: {
      int preset = PresetIndex(v);
      if (preset < 0) {
        preset = direction > 0 ? 0 : int(kPresets.size()) - 1;
      } else {
        preset = Step(preset, int(kPresets.size()), direction);
      }
      v = kPresets[preset].values;
      break;
    }
    case kRowScale:
      v.scale = Step(v.scale, int(kScaleOptions.size()), direction);
      break;
    case kRowAniso:
      v.aniso = Step(v.aniso, int(kAnisoOptions.size()), direction);
      break;
    case kRowAa:
      v.aa = Step(v.aa, int(kAaValues.size()), direction);
      break;
    case kRowFps:
      v.fps = Step(v.fps, int(kFpsLabels.size()), direction);
      break;
    default:
      return;
  }
  if (v == state.values) {
    return;
  }
  state.values = v;
  ApplyLive(v);
  state.status = Save(v) ? "Saved" : "Couldn't save the settings file";
}

void HandleInput() {
  std::vector<uint16_t> buttons;
  {
    std::lock_guard<std::mutex> lock(input_mutex);
    buttons.swap(input_queue);
  }
  for (uint16_t button : buttons) {
    if (button & kDpadUp) {
      state.selected = Step(state.selected, kRowCount, -1);
    } else if (button & kDpadDown) {
      state.selected = Step(state.selected, kRowCount, 1);
    } else if (button & kDpadLeft) {
      Change(state.selected, -1);
    } else if (button & (kDpadRight | kButtonA)) {
      Change(state.selected, 1);
    }
  }
}

}  // namespace

void GraphicsMenuPushInput(uint16_t xinput_button) {
  std::lock_guard<std::mutex> lock(input_mutex);
  if (input_queue.size() < 64) {
    input_queue.push_back(xinput_button);
  }
}

std::string GraphicsMenuSettingsPath() {
  return (rex::filesystem::GetExecutableFolder() / "ps5-graphics.txt").string();
}

GraphicsMenuDialog::GraphicsMenuDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {
  if (!state.loaded) {
    LoadFromFlags();
  }
  state.status.clear();
  // Drop presses queued before the menu opened.
  std::lock_guard<std::mutex> lock(input_mutex);
  input_queue.clear();
}

GraphicsMenuDialog::~GraphicsMenuDialog() = default;

void GraphicsMenuDialog::OnDraw(ImGuiIO& io) {
  HandleInput();

  // TV output: scale with the screen, as the debug overlay does.
  const float tv_scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(520 * tv_scale, 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.88f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("Graphics settings##graphics_menu", nullptr, flags)) {
    ImGui::SetWindowFontScale(tv_scale);
    const Values& v = state.values;
    const int preset = PresetIndex(v);
    const char* labels[kRowCount] = {"Quick preset", "Internal resolution", "Texture filtering",
                                     "Anti-aliasing", "Frame rate cap"};
    const char* values[kRowCount] = {
        preset >= 0 ? kPresets[preset].label : "Custom",
        kScaleOptions[v.scale].label,
        kAnisoOptions[v.aniso].label,
        kAaLabels[v.aa],
        kFpsLabels[v.fps],
    };
    const ImVec4 selected_color(1.0f, 0.82f, 0.3f, 1.0f);
    const ImVec4 dim_color(0.7f, 0.7f, 0.7f, 1.0f);
    const float value_column = 230.0f * tv_scale;
    for (int row = 0; row < kRowCount; ++row) {
      const bool selected = row == state.selected;
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Text, selected_color);
      }
      ImGui::Text("%s %s", selected ? ">" : " ", labels[row]);
      ImGui::SameLine(value_column);
      if (selected) {
        ImGui::Text("< %s >", values[row]);
      } else {
        ImGui::Text("  %s", values[row]);
      }
      if (selected) {
        ImGui::PopStyleColor();
      }
      if (row == kRowPreset) {
        ImGui::Separator();
      }
    }
    ImGui::Spacing();
    if (v.scale != state.running_scale) {
      ImGui::TextColored(selected_color, "Restart the game to change the resolution.");
    }
    if (v.fps == 2) {
      ImGui::TextColored(dim_color, "Uncapped: the game may run unevenly.");
    }
    if (!state.status.empty()) {
      ImGui::TextColored(dim_color, "%s", state.status.c_str());
    }
    ImGui::Separator();
    ImGui::TextColored(dim_color, "D-pad: choose and change    Circle / touchpad: close");
  }
  ImGui::End();
}

}  // namespace rex::ui
