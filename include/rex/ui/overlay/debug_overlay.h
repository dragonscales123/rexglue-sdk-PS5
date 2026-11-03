/**
 * @file        rex/ui/overlay/debug_overlay.h
 *
 * @brief       ImGui debug overlay dialog for frame timing display.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>
#include <array>
#include <cstdint>
#include <functional>

namespace rex::ui {

struct FrameStats {
  double frame_time_ms = 0;
  double fps = 0;
  uint64_t frame_count = 0;
};

class DebugOverlayDialog : public ImGuiDialog {
 public:
  using FrameStatsProvider = std::function<FrameStats()>;

  explicit DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider = {});
  ~DebugOverlayDialog();

  void SetStatsProvider(FrameStatsProvider provider) { stats_provider_ = std::move(provider); }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  FrameStatsProvider stats_provider_;
  // PS5: memory, thread load and GPU-memory traffic, refreshed twice a second.
  void DrawPs5Stats();
  struct Ps5Stats {
    uint64_t sample_time_ns = 0;
    uint64_t gpu_thread_cpu_ns = 0;
    uint64_t render_thread_cpu_ns = 0;
    uint64_t game_thread_cpu_ns = 0;
    uint64_t upload_bytes = 0;
    uint64_t mprotect_calls = 0;
    uint64_t invalidations = 0;
    double gpu_thread_load = -1;
    double render_thread_load = -1;
    double game_thread_load = -1;
    double upload_mb_per_s = 0;
    double mprotect_per_s = 0;
    double invalidations_per_s = 0;
    double direct_total_mb = 0;
    double direct_free_mb = 0;
    double flexible_total_mb = 0;
    double flexible_free_mb = 0;
  } ps5_{};
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  static constexpr size_t kFrameHistorySize = 120;
  std::array<float, kFrameHistorySize> frame_time_history_{};
  size_t frame_history_idx_ = 0;
#endif
};

}  // namespace rex::ui
