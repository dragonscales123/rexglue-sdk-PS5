/**
 * @file        ui/overlay/debug_overlay.cpp
 *
 * @brief       Debug overlay implementation. See debug_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/debug_overlay.h>
#include <rex/platform.h>
#include <rex/version.h>
#include <imgui.h>
#include <algorithm>
#if REX_PLATFORM_PS5
#include <time.h>

#include <ps5platform/kernel.h>

#include "../../core/ps5_profiler.h"
#endif
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
#include <rex/perf/counter.h>
#include <cinttypes>
#endif

namespace rex::ui {

DebugOverlayDialog::DebugOverlayDialog(ImGuiDrawer* imgui_drawer, FrameStatsProvider stats_provider)
    : ImGuiDialog(imgui_drawer), stats_provider_(std::move(stats_provider)) {}

DebugOverlayDialog::~DebugOverlayDialog() {}

void DebugOverlayDialog::OnDraw(ImGuiIO& io) {
  ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  ImGui::SetNextWindowSize(ImVec2(280, 280), ImGuiCond_FirstUseEver);
#elif REX_PLATFORM_PS5
  // TV output: scale with the screen (2.0 at 1080p, 3.0 at 4K, as a TV is
  // viewed from across the room).
  const float tv_scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  ImGui::SetNextWindowPos(ImVec2(20 * tv_scale, 20 * tv_scale), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(360 * tv_scale, 200 * tv_scale), ImGuiCond_FirstUseEver);
#else
  ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
#endif
  ImGui::SetNextWindowBgAlpha(0.5f);
  if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
#if REX_PLATFORM_PS5
    ImGui::SetWindowFontScale(tv_scale);
#endif
    if (stats_provider_) {
      auto stats = stats_provider_();
      if (stats.frame_count > 0) {
        ImGui::Text("Guest: %.1f FPS (%.2f ms)", stats.fps, stats.frame_time_ms);
      }
    }
    DrawPs5Stats();
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    ImGui::Separator();

    // Frame time graph
    auto ft_us = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFrameTimeUs);
    float ft_ms = static_cast<float>(ft_us) / 1000.0f;
    frame_time_history_[frame_history_idx_] = ft_ms;
    frame_history_idx_ = (frame_history_idx_ + 1) % kFrameHistorySize;
    ImGui::PlotLines("##ft", frame_time_history_.data(), kFrameHistorySize,
                     static_cast<int>(frame_history_idx_), "Frame (ms)", 0.0f, 50.0f,
                     ImVec2(200, 40));

    // GPU
    ImGui::Text("Draw: %" PRId64 "  Stalls: %" PRId64 "  Verts: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kDrawCalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCommandBufferStalls),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kVerticesProcessed));

    // Audio
    ImGui::Text("XMA: %" PRId64 "  Lat: %.1fms  BufQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kXmaFramesDecoded),
                static_cast<float>(
                    rex::perf::GetSnapshotCounter(rex::perf::CounterId::kAudioFrameLatencyUs)) /
                    1000.0f,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kBufferQueueDepth));

    // Dispatch
    ImGui::Text("Dispatch: %" PRId64 "  IRQ: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kFunctionsDispatched),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kInterruptDispatches));

    // Threading
    ImGui::Text("Threads: %" PRId64 "  APC: %" PRId64 "  Contention: %" PRId64,
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kActiveThreads),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kApcQueueDepth),
                rex::perf::GetSnapshotCounter(rex::perf::CounterId::kCriticalRegionContentions));

    // Caches
    auto tex_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheHits);
    auto tex_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kTextureCacheMisses);
    auto pip_h = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheHits);
    auto pip_m = rex::perf::GetSnapshotCounter(rex::perf::CounterId::kPipelineCacheMisses);
    ImGui::Text("TexCache: %" PRId64 "/%" PRId64 "  PipeCache: %" PRId64 "/%" PRId64, tex_h,
                tex_h + tex_m, pip_h, pip_h + pip_m);
#endif
  }
  ImGui::End();

  // Build stamp watermark -- centered near bottom of screen
  auto text_size = ImGui::CalcTextSize(REXGLUE_BUILD_STAMP);
  float padding = ImGui::GetStyle().WindowPadding.x * 2.0f;
  float bottom_offset = io.DisplaySize.y * 0.03f;
  ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - text_size.x - padding) * 0.5f,
                                 io.DisplaySize.y - text_size.y - bottom_offset));
  ImGui::SetNextWindowSize(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_Text, imgui_drawer()->style().debug.muted_text);
  if (ImGui::Begin("##watermark", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                       ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(REXGLUE_BUILD_STAMP);
  }
  ImGui::End();
  ImGui::PopStyleColor();
}

void DebugOverlayDialog::DrawPs5Stats() {
#if REX_PLATFORM_PS5
  timespec now_ts;
  clock_gettime(CLOCK_MONOTONIC, &now_ts);
  const uint64_t now = uint64_t(now_ts.tv_sec) * 1000000000ull + uint64_t(now_ts.tv_nsec);
  Ps5Stats& p = ps5_;
  if (!p.sample_time_ns || now - p.sample_time_ns >= 500000000ull) {
    const double seconds = p.sample_time_ns ? double(now - p.sample_time_ns) / 1e9 : 0.0;
    // Busy share of a thread: CPU time it used over the wall time elapsed.
    auto load = [&](const char* prefix, uint64_t& last_cpu, double& out) {
      uint64_t cpu = 0;
      if (!rex::ps5prof::ThreadCpuTimeNs(prefix, &cpu)) {
        out = -1;
        return;
      }
      if (seconds > 0 && last_cpu) {
        out = 100.0 * double(cpu - last_cpu) / (seconds * 1e9);
      }
      last_cpu = cpu;
    };
    load("GPU Commands", p.gpu_thread_cpu_ns, p.gpu_thread_load);
    load("3D Engine", p.render_thread_cpu_ns, p.render_thread_load);
    load("GameThread", p.game_thread_cpu_ns, p.game_thread_load);
    rex::ps5prof::Counters& c = rex::ps5prof::counters();
    const uint64_t upload_bytes = c.upload_bytes.load(std::memory_order_relaxed);
    const uint64_t mprotect_calls = c.mprotect_calls.load(std::memory_order_relaxed);
    const uint64_t invalidations = c.invalidations.load(std::memory_order_relaxed);
    if (seconds > 0) {
      p.upload_mb_per_s = double(upload_bytes - p.upload_bytes) / seconds / 1048576.0;
      p.mprotect_per_s = double(mprotect_calls - p.mprotect_calls) / seconds;
      p.invalidations_per_s = double(invalidations - p.invalidations) / seconds;
    }
    p.upload_bytes = upload_bytes;
    p.mprotect_calls = mprotect_calls;
    p.invalidations = invalidations;
    // Direct memory: the console's physical pool for the title (guest memory,
    // GPU resources). Flexible memory: its budget for heap and anonymous maps.
    p.direct_total_mb = double(sceKernelGetDirectMemorySize()) / 1048576.0;
    int64_t free_start = 0;
    size_t direct_free = 0;
    if (sceKernelAvailableDirectMemorySize(0, sceKernelGetDirectMemorySize(), 0, &free_start,
                                           &direct_free) == 0) {
      p.direct_free_mb = double(direct_free) / 1048576.0;
    }
    size_t flexible_free = 0, flexible_total = 0;
    if (sceKernelAvailableFlexibleMemorySize(&flexible_free) == 0) {
      p.flexible_free_mb = double(flexible_free) / 1048576.0;
    }
    if (sceKernelConfiguredFlexibleMemorySize(&flexible_total) == 0) {
      p.flexible_total_mb = double(flexible_total) / 1048576.0;
    }
    p.sample_time_ns = now;
  }
  ImGui::Separator();
  auto load_text = [](const char* label, double value) {
    if (value < 0) {
      ImGui::Text("%s: -", label);
    } else {
      ImGui::Text("%s: %.0f%%", label, value);
    }
  };
  // The emulated GPU runs on the CPU: its thread's load is what limits the
  // frame rate when it nears 100%.
  load_text("GPU emulation thread", p.gpu_thread_load);
  load_text("Render thread (game)", p.render_thread_load);
  load_text("Game thread", p.game_thread_load);
  ImGui::Text("GPU uploads: %.1f MB/s", p.upload_mb_per_s);
  ImGui::Text("Write faults: %.0f/s  Protect: %.0f/s", p.invalidations_per_s, p.mprotect_per_s);
  ImGui::Separator();
  ImGui::Text("Direct memory: %.0f / %.0f MB used", p.direct_total_mb - p.direct_free_mb,
              p.direct_total_mb);
  if (p.flexible_total_mb > 0) {
    ImGui::Text("Flexible memory: %.0f / %.0f MB used", p.flexible_total_mb - p.flexible_free_mb,
                p.flexible_total_mb);
  }
#endif
}

}  // namespace rex::ui
