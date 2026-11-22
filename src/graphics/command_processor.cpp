/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include <immintrin.h>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/sampler_info.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

#if REX_PLATFORM_PS5
#include "../core/ps5_profiler.h"
#endif

REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");
REXCVAR_DEFINE_BOOL(gpu_parse_thread, true, "GPU",
                    "Parse the PM4 command stream on a separate thread, which drops rewrites of "
                    "unchanged shader constants and forwards everything else, in order, to the "
                    "command processor thread.");
REXCVAR_DEFINE_BOOL(hot_page_epoch_on_coherency_waits, true, "GPU",
                    "Start a new hot page check epoch at WAIT_REG_MEMs on COHER_STATUS_HOST "
                    "(GPU cache coherency requests). Off: only other waits, batches, "
                    "interrupts and swaps start one (experimental).");

REXCVAR_DEFINE_INT32(frame_limit, 0, "GPU",
                     "Host guest-frame swap limit in FPS (0 = unlimited). "
                     "VSync and guest timing may impose a lower rate.")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

#if REX_PLATFORM_PS5
// Off on the PS5: re-uploading every CPU-uploaded page each frame cost ~330 MB/s
// of uploads there (Fable 2: 20.7 -> 21.5 fps with hot pages, movies intact).
#define REX_CLEAR_MEMORY_PAGE_STATE_DEFAULT false
#else
#define REX_CLEAR_MEMORY_PAGE_STATE_DEFAULT true
#endif
REXCVAR_DEFINE_BOOL(clear_memory_page_state, REX_CLEAR_MEMORY_PAGE_STATE_DEFAULT, "GPU",
                    "Refresh page-valid state from GPU-written memory at frame end. "
                    "Disable for minor CPU overhead reduction, but may break memory coherency.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(occlusion_query_enable, true, "GPU", "Enable host occlusion query handling")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve, "none", "GPU",
                      "Controls CPU readback of render-to-texture resolve results.\n"
                      " none: Disable readback (default)\n"
                      " fast: Read previous frame (delayed, copy every frame)\n"
                      " some: Read previous frame (delayed, copy on cache miss)\n"
                      " full: Immediate sync readback (accurate but stalls)")
    .allowed({"none", "fast", "some", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_resolve_half_pixel_offset, true, "GPU",
                    "When draw resolution scaling is active, sample from the center of each "
                    "scaled block during resolve readback downscale. With the D3D9-style "
                    "half-pixel offset, the content of each guest pixel starts at host pixel "
                    "(scale/2, scale/2) of its block, so the block's top-left host pixel "
                    "belongs to the previous guest pixel, and the first row and column of "
                    "the resolve read the uncovered edge (a line along the top of the "
                    "screen at 2x in Fable II)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(readback_resolve_force_addresses, "", "GPU",
                      "Comma-separated guest base addresses whose render-to-texture resolve is "
                      "force-read back to guest memory even when readback_resolve is disabled. "
                      "Each entry (hex/decimal) matches [addr, addr + 1 MiB). E.g. Fable II's "
                      "hero/dog face texture at 0x12704000 (approach from just-harry's "
                      "unofficial Xenia femtofork for Fable II).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport, true, "GPU",
                    "Enable CPU readback of shader memexport writes for guest memory "
                    "coherency (can reduce correctness issues, but may add GPU/CPU sync cost)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast, true, "GPU",
                    "Use fast double-buffered memexport readback when possible, with "
                    "automatic fallback to full synchronous readback")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(readback_memexport_fast_wait, true, "GPU",
                    "With readback_memexport_fast, wait for the previous memexport copy when "
                    "the GPU has not finished it yet, instead of reading the current draw. "
                    "The guest then always sees the data from one memexport draw back, at any "
                    "frame rate, rather than alternating between the previous and the current "
                    "draw depending on how far the GPU is behind.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(query_occlusion_fake_sample_count, 1000, "GPU",
                     "Fake sample count for occlusion queries")
    .range(1, 100000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_shader_compilation, true, "GPU",
                    "Compile shaders and create pipelines asynchronously in background "
                    "threads. This reduces stutter but may cause brief visual artifacts while "
                    "pipelines are being prepared.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(async_pipeline_wait, true, "GPU",
                    "When a draw needs a pipeline that is still being created in the background, "
                    "wait for it instead of skipping the draw. Skipped draws cause missing "
                    "objects and corrupted textures, at the cost of a short stall the first time "
                    "a pipeline is needed. Only used with async_shader_compilation.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(async_pipeline_wait_timeout_ms, 5000, "GPU",
                     "Longest time, in milliseconds, to wait for one pipeline with "
                     "async_pipeline_wait before skipping the draw anyway.")
    .range(100, 60000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics {

using namespace rex::graphics::xenos;

namespace {

ReadbackResolveMode ParseReadbackResolveMode(std::string_view value) {
  if (value == "fast") {
    return ReadbackResolveMode::kFast;
  }
  if (value == "some") {
    return ReadbackResolveMode::kSome;
  }
  if (value == "full") {
    return ReadbackResolveMode::kFull;
  }
  return ReadbackResolveMode::kDisabled;
}

}  // namespace

CommandProcessor::CommandProcessor(GraphicsSystem* graphics_system,
                                   system::KernelState* kernel_state)
    : memory_(graphics_system->memory()),
      kernel_state_(kernel_state),
      graphics_system_(graphics_system),
      register_file_(graphics_system_->register_file()),
      worker_running_(true),
      write_ptr_index_event_(rex::thread::Event::CreateAutoResetEvent(false)),
      write_ptr_index_(0) {
  assert_not_null(write_ptr_index_event_);
}

CommandProcessor::~CommandProcessor() = default;

bool CommandProcessor::Initialize() {
  // Initialize the gamma ramps to their default (linear) values - taken from
  // what games set when starting with the sRGB (return value 1)
  // VdGetCurrentDisplayGamma.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t value = i * 0x3FF / 0xFF;
    reg::DC_LUT_30_COLOR& gamma_ramp_entry = gamma_ramp_256_entry_table_[i];
    gamma_ramp_entry.color_10_blue = value;
    gamma_ramp_entry.color_10_green = value;
    gamma_ramp_entry.color_10_red = value;
  }
  for (uint32_t i = 0; i < 128; ++i) {
    reg::DC_LUT_PWL_DATA gamma_ramp_entry = {};
    gamma_ramp_entry.base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
    gamma_ramp_entry.delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t j = 0; j < 3; ++j) {
      gamma_ramp_pwl_rgb_[i][j] = gamma_ramp_entry;
    }
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));
  worker_thread_->set_name("GPU Commands");
  worker_thread_->Create();

  return true;
}

void CommandProcessor::Shutdown() {
  worker_running_ = false;
  write_ptr_index_event_->Set();
  worker_thread_->Wait(0, 0, 0, nullptr);
  worker_thread_.reset();
}

void CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                               uint32_t title_id, bool blocking) {}

void CommandProcessor::CallInThread(std::function<void()> fn) {
  if (pending_fns_.empty() && system::XThread::IsInThread(worker_thread_.get())) {
    fn();
  } else {
    pending_fns_.push(std::move(fn));
  }
}

void CommandProcessor::ClearCaches() {}

void CommandProcessor::InvalidateGpuMemory() {}

ReadbackResolveMode CommandProcessor::GetReadbackResolveMode(
    bool legacy_readback_resolve_enabled) const {
  ReadbackResolveMode shared_mode = ParseReadbackResolveMode(REXCVAR_GET(readback_resolve));
  // HasNonDefaultValue locks the registry and formats the value as a string -
  // rechecked only every 256 calls (resolves and memexport draws are frequent).
  if (!(readback_cvar_check_counter_[0]++ & 255)) {
    readback_resolve_non_default_ = rex::cvar::HasNonDefaultValue("readback_resolve");
  }
  bool shared_mode_overrides_legacy =
      shared_mode != ReadbackResolveMode::kDisabled || readback_resolve_non_default_;
  if (shared_mode_overrides_legacy) {
    return shared_mode;
  }
  return legacy_readback_resolve_enabled ? ReadbackResolveMode::kFast
                                         : ReadbackResolveMode::kDisabled;
}

bool CommandProcessor::ShouldForceReadbackResolve(uint32_t base) const {
  const std::string& spec = REXCVAR_GET(readback_resolve_force_addresses);
  if (spec.empty()) {
    return false;
  }

  // The hero/dog EDRAM-resolve copy is issued as a PM4_DRAW_INDX_2 draw (see the
  // `may_require_readback_resolve_` set in the draw opcode dispatch). Regular
  // PM4_DRAW_INDX draws can also be a kRectangleList/3 copy to the same
  // destination (the game sampling that texture mid-frame); reading those back
  // captures a mid-update state and makes the dog look red / shifting.
  if (!may_require_readback_resolve_) {
    return false;
  }

  // Only the full-texture blit should force a readback, matching the femtofork's
  // detection: a fullscreen kRectangleList draw with 3 indices that copies to the
  // texture base. The dog's texture is touched by extra / partial copies near the
  // same region; reading those back mid-update makes it look glitchy / shifting.
  const reg::VGT_DRAW_INITIATOR vgt = register_file_->Get<reg::VGT_DRAW_INITIATOR>();
  if (vgt.prim_type != xenos::PrimitiveType::kRectangleList || vgt.num_indices != 0x03) {
    return false;
  }

  // Each entry is a guest base address (hex with an optional 0x prefix, or
  // decimal); it matches [addr, addr + kForceReadbackWindow). The window must be
  // small: 0x12704000 sits in a densely-packed region with other EDRAM resolve
  // targets (e.g. 0x12724000) blitted in rapid bursts. A wide window forces an
  // immediate GPU-sync readback for each, stalling the GPU mid-frame (the dog
  // looked glitchy / red / shifting). 64 KiB covers the target while the nearest
  // unrelated target is 128 KiB away.
  constexpr uint32_t kForceReadbackWindow = 1u << 16;  // 64 KiB
  size_t start = 0;
  while (start < spec.size()) {
    size_t end = spec.find(',', start);
    if (end == std::string::npos) {
      end = spec.size();
    }
    std::string_view token = spec.substr(start, end - start);
    // Trim surrounding whitespace.
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front()))) {
      token.remove_prefix(1);
    }
    while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back()))) {
      token.remove_suffix(1);
    }
    if (!token.empty()) {
      uint64_t addr = 0;
      try {
        size_t pos = 0;
        addr = std::stoull(std::string(token), &pos, 0);  // base 0: 0x-hex or decimal
      } catch (const std::exception&) {
        addr = 0;  // Malformed token - ignore.
      }
      uint32_t a = static_cast<uint32_t>(addr);
      if (base >= a && base - a < kForceReadbackWindow) {
        // Throttled diagnostic: confirm the forced readback is triggering and on the
        // expected address (helps discover a second address for the dog).
        static int s_forced_readback_log = 0;
        if (s_forced_readback_log < 24) {
          REXGPU_INFO("Forced readback-resolve for RB_COPY_DEST_BASE={:#010x} "
                      "(matched force address {:#010x})",
                      base, a);
          ++s_forced_readback_log;
        }
        return true;
      }
    }
    start = end + 1;
  }
  return false;
}

bool CommandProcessor::IsReadbackMemexportEnabled(bool legacy_backend_flag) const {
  if (legacy_readback_memexport_cvar_name_) {
    if (!(readback_cvar_check_counter_[1]++ & 255)) {
      legacy_readback_memexport_non_default_ =
          rex::cvar::HasNonDefaultValue(legacy_readback_memexport_cvar_name_);
    }
    if (legacy_readback_memexport_non_default_) {
      return legacy_backend_flag;
    }
  }
  return REXCVAR_GET(readback_memexport);
}

void CommandProcessor::SetDesiredSwapPostEffect(SwapPostEffect swap_post_effect) {
  if (swap_post_effect_desired_ == swap_post_effect) {
    return;
  }
  swap_post_effect_desired_ = swap_post_effect;
  CallInThread([this, swap_post_effect]() { swap_post_effect_actual_ = swap_post_effect; });
}

void CommandProcessor::WorkerThreadMain() {
  if (!SetupContext()) {
    rex::FatalError("Unable to setup command processor internal state");
    return;
  }

  if (REXCVAR_GET(gpu_parse_thread)) {
    RunWithParseThread();
    ShutdownContext();
    return;
  }

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }

    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index) {
      SCOPE_profile_cpu_i("gpu", "rex::graphics::CommandProcessor::Stall");
      // We've run out of commands to execute.
      // We spin here waiting for new ones, as the overhead of waiting on our
      // event is too high.
      PrepareForWait();
      uint32_t loop_count = 0;
      do {
        // If we spin around too much, revert to a "low-power" state.
        if (loop_count > 500) {
          const int wait_time_ms = 5;
          rex::thread::Wait(write_ptr_index_event_.get(), true,
                            std::chrono::milliseconds(wait_time_ms));
        }

        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (worker_running_ && pending_fns_.empty() &&
               (write_ptr_index == 0xBAADF00D || read_ptr_index_ == write_ptr_index));
      ReturnFromWait();
      if (!worker_running_ || !pending_fns_.empty()) {
        continue;
      }
    }
    assert_true(read_ptr_index_ != write_ptr_index);

    // New commands from the CPU: data written for them may overwrite bytes
    // earlier commands read.
    OnGuestSyncPoint(GuestSyncPoint::kBatch);
    // Execute. Note that we handle wraparound transparently.
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);

    // TODO(benvanik): use reader->Read_update_freq_ and only issue after moving
    //     that many indices.
    if (read_ptr_writeback_ptr_) {
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                       read_ptr_index_);
    }

    // FIXME: We're supposed to process the WAIT_UNTIL register at this point,
    // but no games seem to actually use it.
  }

  ShutdownContext();
}

void CommandProcessor::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  thread::Fence fence;
  CallInThread([&fence]() {
    fence.Signal();
    thread::Thread::GetCurrentThread()->Suspend();
  });

  fence.Wait();
}

void CommandProcessor::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  worker_thread_->thread()->Resume();
}

bool CommandProcessor::Save(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  stream->Write<uint32_t>(primary_buffer_ptr_);
  stream->Write<uint32_t>(primary_buffer_size_);
  stream->Write<uint32_t>(read_ptr_index_);
  stream->Write<uint32_t>(read_ptr_update_freq_);
  stream->Write<uint32_t>(read_ptr_writeback_ptr_);
  stream->Write<uint32_t>(write_ptr_index_.load());

  return true;
}

bool CommandProcessor::Restore(::rex::stream::ByteStream* stream) {
  assert_true(paused_);

  primary_buffer_ptr_ = stream->Read<uint32_t>();
  primary_buffer_size_ = stream->Read<uint32_t>();
  read_ptr_index_ = stream->Read<uint32_t>();
  read_ptr_update_freq_ = stream->Read<uint32_t>();
  read_ptr_writeback_ptr_ = stream->Read<uint32_t>();
  write_ptr_index_.store(stream->Read<uint32_t>());

  return true;
}

bool CommandProcessor::SetupContext() {
  return true;
}

void CommandProcessor::ShutdownContext() {}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  primary_buffer_size_ = uint32_t(1) << (size_log2 + 3);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  // CP_RB_RPTR_ADDR Ring Buffer Read Pointer Address 0x70C
  // ptr = RB_RPTR_ADDR, pointer to write back the address to.
  read_ptr_writeback_ptr_ = ptr;
  // CP_RB_CNTL Ring Buffer Control 0x704
  // block_size = RB_BLKSZ, log2 of number of quadwords read between updates of
  //              the read pointer.
  read_ptr_update_freq_ = uint32_t(1) << block_size_log2 >> 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_ = value;
  write_ptr_index_event_->Set();
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  if (index < RegisterFile::kRegisterCount) {
    return register_file_->values[index];
  }
  auto it = extended_register_values_.find(index);
  return it != extended_register_values_.end() ? it->second : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  RegisterFile& regs = *register_file_;
  if (index >= RegisterFile::kRegisterCount) {
    auto [it, inserted] = extended_register_values_.insert_or_assign(index, value);
    (void)it;
    if (inserted) {
      REXGPU_WARN(
          "CommandProcessor::WriteRegister index out of bounds: {} (stored as extended register)",
          index);
    }
    return;
  }

  // Volatile for the WAIT_REG_MEM loop.
  const_cast<volatile uint32_t&>(regs.values[index]) = value;
#ifndef NDEBUG
  // A ~1000-case switch on every register write; only for a debug message.
  if (!regs.GetRegisterInfo(index)) {
    REXGPU_DEBUG("GPU: Write to unknown register ({:04X} = {:08X})", index, value);
  }
#endif

  // Scratch register writeback.
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch_reg = index - XE_GPU_REG_SCRATCH_REG0;
    if ((1 << scratch_reg) & regs.values[XE_GPU_REG_SCRATCH_UMSK]) {
      // Enabled - write to address.
      uint32_t scratch_addr = regs.values[XE_GPU_REG_SCRATCH_ADDR];
      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);
      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);
      OnGuestSyncPoint(GuestSyncPoint::kGpuSignal);
    }
  } else {
    switch (index) {
      // If this is a COHER register, set the dirty flag.
      // This will block the command processor the next time it WAIT_REG_MEMs
      // and allow us to synchronize the memory.
      case XE_GPU_REG_COHER_STATUS_HOST: {
        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);
      } break;

      case XE_GPU_REG_DC_LUT_RW_INDEX: {
        // Reset the sequential read / write component index (see the M56
        // DC_LUT_SEQ_COLOR documentation).
        gamma_ramp_rw_component_ = 0;
      } break;

      case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // DC_LUT_SEQ_COLOR is in the red, green, blue order, but the write
        // enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          // Bits 0:5 are hardwired to zero.
          uint32_t gamma_ramp_seq_color = regs.Get<reg::DC_LUT_SEQ_COLOR>().seq_color >> 6;
          switch (gamma_ramp_rw_component_) {
            case 0:
              gamma_ramp_entry.color_10_red = gamma_ramp_seq_color;
              break;
            case 1:
              gamma_ramp_entry.color_10_green = gamma_ramp_seq_color;
              break;
            case 2:
              gamma_ramp_entry.color_10_blue = gamma_ramp_seq_color;
              break;
          }
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          ++new_gamma_ramp_rw_index.rw_index;
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_PWL_DATA: {
        // Should be in the PWL writing mode.
        assert_not_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        // Bit 7 of the index is ignored for PWL.
        uint32_t gamma_ramp_rw_index_pwl = gamma_ramp_rw_index.rw_index & 0x7F;
        // DC_LUT_PWL_DATA is likely in the red, green, blue order because
        // DC_LUT_SEQ_COLOR is, but the write enable mask is blue, green, red.
        bool write_gamma_ramp_component = (regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                                           (UINT32_C(1) << (2 - gamma_ramp_rw_component_))) != 0;
        if (write_gamma_ramp_component) {
          reg::DC_LUT_PWL_DATA& gamma_ramp_entry =
              gamma_ramp_pwl_rgb_[gamma_ramp_rw_index_pwl][gamma_ramp_rw_component_];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_PWL_DATA>();
          // Bits 0:5 are hardwired to zero.
          gamma_ramp_entry.base = gamma_ramp_value.base & ~UINT32_C(0x3F);
          gamma_ramp_entry.delta = gamma_ramp_value.delta & ~UINT32_C(0x3F);
        }
        if (++gamma_ramp_rw_component_ >= 3) {
          gamma_ramp_rw_component_ = 0;
          reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
          // TODO(Triang3l): Should this increase beyond 7 bits for PWL?
          // Direct3D 9 explicitly sets rw_index to 0x80 after writing the last
          // PWL entry. However, the DC_LUT_RW_INDEX documentation says that for
          // PWL, the bit 7 is ignored.
          new_gamma_ramp_rw_index.rw_index = (gamma_ramp_rw_index.rw_index & ~UINT32_C(0x7F)) |
                                             ((gamma_ramp_rw_index_pwl + 1) & 0x7F);
          WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                        rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        }
        if (write_gamma_ramp_component) {
          OnGammaRampPWLValueWritten();
        }
      } break;

      case XE_GPU_REG_DC_LUT_30_COLOR: {
        // Should be in the 256-entry table writing mode.
        assert_zero(regs[XE_GPU_REG_DC_LUT_RW_MODE] & 0b1);
        auto gamma_ramp_rw_index = regs.Get<reg::DC_LUT_RW_INDEX>();
        uint32_t gamma_ramp_write_enable_mask = regs[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
        if (gamma_ramp_write_enable_mask) {
          reg::DC_LUT_30_COLOR& gamma_ramp_entry =
              gamma_ramp_256_entry_table_[gamma_ramp_rw_index.rw_index];
          auto gamma_ramp_value = regs.Get<reg::DC_LUT_30_COLOR>();
          if (gamma_ramp_write_enable_mask & 0b001) {
            gamma_ramp_entry.color_10_blue = gamma_ramp_value.color_10_blue;
          }
          if (gamma_ramp_write_enable_mask & 0b010) {
            gamma_ramp_entry.color_10_green = gamma_ramp_value.color_10_green;
          }
          if (gamma_ramp_write_enable_mask & 0b100) {
            gamma_ramp_entry.color_10_red = gamma_ramp_value.color_10_red;
          }
        }
        // TODO(Triang3l): Should this reset the component write index? If this
        // increase is assumed to behave like a full DC_LUT_RW_INDEX write, it
        // probably should. Currently this also calls WriteRegister for
        // DC_LUT_RW_INDEX, which resets gamma_ramp_rw_component_ as well.
        gamma_ramp_rw_component_ = 0;
        reg::DC_LUT_RW_INDEX new_gamma_ramp_rw_index = gamma_ramp_rw_index;
        ++new_gamma_ramp_rw_index.rw_index;
        WriteRegister(XE_GPU_REG_DC_LUT_RW_INDEX,
                      rex::memory::Reinterpret<uint32_t>(new_gamma_ramp_rw_index));
        if (gamma_ramp_write_enable_mask) {
          OnGammaRamp256EntryTableValueWritten();
        }
      } break;
    }
  }
}

void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  for (uint32_t i = 0; i < num_registers; ++i) {
    uint32_t data = memory::load_and_swap<uint32_t>(base + i);
    WriteRegister(start_index + i, data);
  }
}

void CommandProcessor::WriteRegisterRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  memory::RingBuffer::ReadRange range = ring->BeginRead(size_t(num_registers) * sizeof(uint32_t));
  if (range.first_length != 0) {
    uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.first)),
                          first_count);
    base += first_count;
  }
  if (range.second_length != 0) {
    WriteRegistersFromMem(base, reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(range.second)),
                          uint32_t(range.second_length / sizeof(uint32_t)));
  }
  ring->EndRead(range);
}

void CommandProcessor::WriteALURangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                             uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4000, num_registers);
}

void CommandProcessor::WriteFetchRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                               uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4800, num_registers);
}

void CommandProcessor::WriteBoolRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4900, num_registers);
}

void CommandProcessor::WriteLoopRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                              uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x4908, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromRing(memory::RingBuffer* ring, uint32_t base,
                                                   uint32_t num_registers) {
  WriteRegisterRangeFromRing(ring, base + 0x2000, num_registers);
}

void CommandProcessor::WriteALURangeFromMem(uint32_t start_index, uint32_t* base,
                                            uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4000, base, num_registers);
}

void CommandProcessor::WriteFetchRangeFromMem(uint32_t start_index, uint32_t* base,
                                              uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4800, base, num_registers);
}

void CommandProcessor::WriteBoolRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4900, base, num_registers);
}

void CommandProcessor::WriteLoopRangeFromMem(uint32_t start_index, uint32_t* base,
                                             uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x4908, base, num_registers);
}

void CommandProcessor::WriteREGISTERSRangeFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  WriteRegistersFromMem(start_index + 0x2000, base, num_registers);
}

void CommandProcessor::MakeCoherent() {
  SCOPE_profile_cpu_f("gpu");

  // Status host often has 0x01000000 or 0x03000000.
  // This is likely toggling VC (vertex cache) or TC (texture cache).
  // Or, it also has a direction in here maybe - there is probably
  // some way to check for dest coherency (what all the COHER_DEST_BASE_*
  // registers are for).
  // Best docs I've found on this are here:
  // https://web.archive.org/web/20160711162346/https://amd-dev.wpengine.netdna-cdn.com/wordpress/media/2013/10/R6xx_R7xx_3D.pdf
  // https://cgit.freedesktop.org/xorg/driver/xf86-video-radeonhd/tree/src/r6xx_accel.c?id=3f8b6eccd9dba116cc4801e7f80ce21a879c67d2#n454

  // Volatile because this may be called from the WAIT_REG_MEM loop.
  volatile uint32_t* regs_volatile = register_file_->values;
  auto status_host = rex::memory::Reinterpret<reg::COHER_STATUS_HOST>(
      uint32_t(regs_volatile[XE_GPU_REG_COHER_STATUS_HOST]));
  uint32_t base_host = regs_volatile[XE_GPU_REG_COHER_BASE_HOST];
  uint32_t size_host = regs_volatile[XE_GPU_REG_COHER_SIZE_HOST];

  if (!status_host.status) {
    return;
  }

  const char* action = "N/A";
  if (status_host.vc_action_ena && status_host.tc_action_ena) {
    action = "VC | TC";
  } else if (status_host.tc_action_ena) {
    action = "TC";
  } else if (status_host.vc_action_ena) {
    action = "VC";
  }

  // TODO(benvanik): notify resource cache of base->size and type.
  REXGPU_TRACE("Make {:08X} -> {:08X} ({}b) coherent, action = {}", base_host,
               base_host + size_host, size_host, action);

  // Mark coherent.
  regs_volatile[XE_GPU_REG_COHER_STATUS_HOST] = 0;
}

void CommandProcessor::PrepareForWait() {}

void CommandProcessor::ReturnFromWait() {}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_), primary_buffer_size_);
  reader.set_read_offset(read_index * sizeof(uint32_t));
  reader.set_write_offset(write_index * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // This probably should be fatal - but we're going to continue anyways.
      REXGPU_ERROR("**** PRIMARY RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());

  OnPrimaryBufferEnd();

  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      // Return up a level if we encounter a bad packet.
      REXGPU_ERROR("**** INDIRECT RINGBUFFER: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::ExecutePacket(uint32_t ptr, uint32_t count) {
  // Execute commands!
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ExecutePacket(&reader)) {
      REXGPU_ERROR("**** ExecutePacket: Failed to execute packet.");
      assert_always();
      break;
    }
  } while (reader.read_count());
}

bool CommandProcessor::ExecutePacket(memory::RingBuffer* reader) {
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  const uint32_t packet_type = packet >> 30;
  if (packet == 0) {
    return true;
  }

  if (packet == 0xCDCDCDCD) {
    REXGPU_WARN("GPU packet is CDCDCDCD - probably read uninitialized memory!");
  }

  switch (packet_type) {
    case 0x00:
      return ExecutePacketType0(reader, packet);
    case 0x01:
      return ExecutePacketType1(reader, packet);
    case 0x02:
      return ExecutePacketType2(reader, packet);
    case 0x03:
      return ExecutePacketType3(reader, packet);
    default:
      assert_unhandled_case(packet_type);
      return false;
  }
}

bool CommandProcessor::ExecutePacketType0(memory::RingBuffer* reader, uint32_t packet) {
  // Type-0 packet.
  // Write count registers in sequence to the registers starting at
  // (base_index << 2).

  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType0 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  uint32_t base_index = (packet & 0x7FFF);
  uint32_t write_one_reg = (packet >> 15) & 0x1;
  if (!write_one_reg) {
    // Consecutive registers: one range write, so the backend can copy whole
    // constant blocks instead of taking two virtual calls per register.
    WriteRegisterRangeFromRing(reader, base_index, count);
    return true;
  }
  for (uint32_t m = 0; m < count; m++) {
    uint32_t reg_data = reader->ReadAndSwap<uint32_t>();
    WriteRegister(base_index, reg_data);
  }

  return true;
}

bool CommandProcessor::ExecutePacketType1(memory::RingBuffer* reader, uint32_t packet) {
  // Type-1 packet.
  // Contains two registers of data. Type-0 should be more common.
  uint32_t reg_index_1 = packet & 0x7FF;
  uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
  uint32_t reg_data_1 = reader->ReadAndSwap<uint32_t>();
  uint32_t reg_data_2 = reader->ReadAndSwap<uint32_t>();
  WriteRegister(reg_index_1, reg_data_1);
  WriteRegister(reg_index_2, reg_data_2);
  return true;
}

bool CommandProcessor::ExecutePacketType2(memory::RingBuffer* reader, uint32_t packet) {
  // Type-2 packet.
  // No-op. Do nothing.
  return true;
}

bool CommandProcessor::ExecutePacketType3(memory::RingBuffer* reader, uint32_t packet) {
  // Type-3 packet.
  uint32_t opcode = (packet >> 8) & 0x7F;
  uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  auto data_start_offset = reader->read_offset();

  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ExecutePacketType3 overflow (read count {:08X}, packet count {:08X})",
                 reader->read_count(), count * sizeof(uint32_t));
    return false;
  }

  // & 1 == predicate - when set, we do bin check to see if we should execute
  // the packet. Only type 3 packets are affected.
  // We also skip predicated swaps, as they are never valid (probably?).
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (!any_pass || opcode == PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:
      result = ExecutePacketType3_ME_INIT(reader, packet, count);
      break;
    case PM4_NOP:
      result = ExecutePacketType3_NOP(reader, packet, count);
      break;
    case PM4_INTERRUPT:
      result = ExecutePacketType3_INTERRUPT(reader, packet, count);
      break;
    case PM4_XE_SWAP:
      result = ExecutePacketType3_XE_SWAP(reader, packet, count);
      break;
    case PM4_INDIRECT_BUFFER:
    case PM4_INDIRECT_BUFFER_PFD:
      result = ExecutePacketType3_INDIRECT_BUFFER(reader, packet, count);
      break;
    case PM4_WAIT_REG_MEM:
      result = ExecutePacketType3_WAIT_REG_MEM(reader, packet, count);
      break;
    case PM4_REG_RMW:
      result = ExecutePacketType3_REG_RMW(reader, packet, count);
      break;
    case PM4_REG_TO_MEM:
      result = ExecutePacketType3_REG_TO_MEM(reader, packet, count);
      break;
    case PM4_MEM_WRITE:
      result = ExecutePacketType3_MEM_WRITE(reader, packet, count);
      break;
    case PM4_COND_WRITE:
      result = ExecutePacketType3_COND_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE:
      result = ExecutePacketType3_EVENT_WRITE(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_SHD:
      result = ExecutePacketType3_EVENT_WRITE_SHD(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_EXT:
      result = ExecutePacketType3_EVENT_WRITE_EXT(reader, packet, count);
      break;
    case PM4_EVENT_WRITE_ZPD:
      result = ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
      break;
    case PM4_DRAW_INDX:
      result = ExecutePacketType3_DRAW_INDX(reader, packet, count);
      break;
    case PM4_DRAW_INDX_2:
      result = ExecutePacketType3_DRAW_INDX_2(reader, packet, count);
      break;
    case PM4_SET_CONSTANT:
      result = ExecutePacketType3_SET_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_CONSTANT2:
      result = ExecutePacketType3_SET_CONSTANT2(reader, packet, count);
      break;
    case PM4_LOAD_ALU_CONSTANT:
      result = ExecutePacketType3_LOAD_ALU_CONSTANT(reader, packet, count);
      break;
    case PM4_SET_SHADER_CONSTANTS:
      result = ExecutePacketType3_SET_SHADER_CONSTANTS(reader, packet, count);
      break;
    case PM4_IM_LOAD:
      result = ExecutePacketType3_IM_LOAD(reader, packet, count);
      break;
    case PM4_IM_LOAD_IMMEDIATE:
      result = ExecutePacketType3_IM_LOAD_IMMEDIATE(reader, packet, count);
      break;
    case PM4_INVALIDATE_STATE:
      result = ExecutePacketType3_INVALIDATE_STATE(reader, packet, count);
      break;
    case PM4_VIZ_QUERY:
      result = ExecutePacketType3_VIZ_QUERY(reader, packet, count);
      break;

    case PM4_SET_BIN_MASK_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_MASK_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_LO: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | value;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT_HI: {
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (static_cast<uint64_t>(value) << 32);
      result = true;
    } break;
    case PM4_SET_BIN_MASK: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_mask_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_SET_BIN_SELECT: {
      assert_true(count == 2);
      uint64_t val_hi = reader->ReadAndSwap<uint32_t>();
      uint64_t val_lo = reader->ReadAndSwap<uint32_t>();
      bin_select_ = (val_hi << 32) | val_lo;
      result = true;
    } break;
    case PM4_CONTEXT_UPDATE: {
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU context update = {:08X}", value);
      assert_true(value == 0);
      result = true;
      break;
    }
    case PM4_WAIT_FOR_IDLE: {
      // This opcode is used by 5454084E while going / being ingame.
      assert_true(count == 1);
      uint32_t value = reader->ReadAndSwap<uint32_t>();
      REXGPU_INFO("GPU wait for idle = {:08X}", value);
      result = true;
      break;
    }

    default:
      REXGPU_INFO("Unimplemented GPU OPCODE: 0x{:02X}\t\tCOUNT: {}\n", opcode, count);
      assert_always();
      reader->AdvanceRead(count * sizeof(uint32_t));
      break;
  }
  switch (opcode) {
    case PM4_WAIT_REG_MEM:
      if (!last_wait_was_coherency_ || REXCVAR_GET(hot_page_epoch_on_coherency_waits)) {
        OnGuestSyncPoint(GuestSyncPoint::kWait);
      }
      break;
    case PM4_INTERRUPT:
      OnGuestSyncPoint(GuestSyncPoint::kInterrupt);
      break;
    case PM4_XE_SWAP:
      OnGuestSyncPoint(GuestSyncPoint::kSwap);
      break;
    case PM4_REG_TO_MEM:
    case PM4_MEM_WRITE:
    case PM4_COND_WRITE:
    case PM4_EVENT_WRITE_SHD:
    case PM4_EVENT_WRITE_EXT:
    case PM4_EVENT_WRITE_ZPD:
      OnGuestSyncPoint(GuestSyncPoint::kGpuSignal);
      break;
    default:
      break;
  }

  assert_true(reader->read_offset() ==
              (data_start_offset + (count * sizeof(uint32_t))) % reader->capacity());
  return result;
}

bool CommandProcessor::ExecutePacketType3_ME_INIT(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // initialize CP's micro-engine
  me_bin_.clear();
  for (uint32_t i = 0; i < count; i++) {
    me_bin_.push_back(reader->ReadAndSwap<uint32_t>());
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_NOP(memory::RingBuffer* reader, uint32_t packet,
                                              uint32_t count) {
  // skip N 32-bit words to get to the next packet
  // No-op, ignore some data.
  reader->AdvanceRead(count * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INTERRUPT(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // generate interrupt from the command stream
  uint32_t cpu_mask = reader->ReadAndSwap<uint32_t>();
  for (int n = 0; n < 6; n++) {
    if (cpu_mask & (1 << n)) {
      if (graphics_system_) {
        graphics_system_->DispatchInterruptCallback(1, n);
      }
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  frame_limiter_.Pace(REXCVAR_GET(frame_limit));

  // The small guest FPS readout also works in Release, where the full
  // profiling macros compile out. Shared runtime counters feed the F3 UI.
  auto mean_us = guest_frame_meter_.Record(GuestFrameMeter::Clock::now());
  rex::perf::SetCounter(rex::perf::CounterId::kFrameTimeUs, mean_us);
  rex::perf::SetCounter(rex::perf::CounterId::kFps, mean_us > 0 ? 1000000 / mean_us : 0);
#ifndef REXGLUE_ENABLE_PERF_COUNTERS
  rex::perf::ResetFrameCounters();
#endif
  rex::perf::Profiler::Flip();

  // Xenia-specific VdSwap hook.
  // VdSwap will post this to tell us we need to swap the screen/fire an
  // interrupt.
  // 63 words here, but only the first has any data.
  uint32_t magic = reader->ReadAndSwap<memory::fourcc_t>();
  assert_true(magic == kSwapSignature);

  // TODO(benvanik): only swap frontbuffer ptr.
  uint32_t frontbuffer_ptr = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_width = reader->ReadAndSwap<uint32_t>();
  uint32_t frontbuffer_height = reader->ReadAndSwap<uint32_t>();
  reader->AdvanceRead((count - 4) * sizeof(uint32_t));

  IssueSwap(frontbuffer_ptr, frontbuffer_width, frontbuffer_height);

  ++counter_;
  return true;
}

bool CommandProcessor::ExecutePacketType3_INDIRECT_BUFFER(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // indirect buffer dispatch
  uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
  uint32_t list_length = reader->ReadAndSwap<uint32_t>();
  assert_zero(list_length & ~0xFFFFF);
  list_length &= 0xFFFFF;
  ExecuteIndirectBuffer(GpuToCpu(list_ptr), list_length);
  return true;
}

bool CommandProcessor::ExecutePacketType3_WAIT_REG_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // wait until a register or memory location is a specific value

  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t wait = reader->ReadAndSwap<uint32_t>();

  bool is_memory = (wait_info & 0x10) != 0;
  last_wait_was_coherency_ = !is_memory && poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST;

  bool matched = false;
  do {
    uint32_t value = 0;
    if (is_memory) {
      value =
          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
    } else {
      value = ReadRegisterValue(poll_reg_addr);
      if (poll_reg_addr == XE_GPU_REG_COHER_STATUS_HOST) {
        MakeCoherent();
        value = ReadRegisterValue(poll_reg_addr);
      }
    }
    switch (wait_info & 0x7) {
      case 0x0:  // Never.
        matched = false;
        break;
      case 0x1:  // Less than reference.
        matched = (value & mask) < ref;
        break;
      case 0x2:  // Less than or equal to reference.
        matched = (value & mask) <= ref;
        break;
      case 0x3:  // Equal to reference.
        matched = (value & mask) == ref;
        break;
      case 0x4:  // Not equal to reference.
        matched = (value & mask) != ref;
        break;
      case 0x5:  // Greater than or equal to reference.
        matched = (value & mask) >= ref;
        break;
      case 0x6:  // Greater than reference.
        matched = (value & mask) > ref;
        break;
      case 0x7:  // Always
        matched = true;
        break;
    }
    if (!matched) {
      // Wait.
      if (wait >= 0x100) {
        // Diagnostic: per-second summary of WAIT_REG_MEM wait-hint sleeps
        // (the vsync path that sleeps wait/0x100 ms). Long, frequent
        // sleeps here mean the guest's own frame loop is pacing itself.
        static uint64_t diag_waits = 0;
        static uint64_t diag_sleep_ms = 0;
        static auto diag_last_log =
            std::chrono::steady_clock::now();
        diag_waits++;
        if (REXCVAR_GET(vsync)) {
          diag_sleep_ms += wait / 0x100;
        }
        const auto now_log = std::chrono::steady_clock::now();
        if (now_log - diag_last_log >= std::chrono::seconds(1)) {
          const auto elapsed_ms = std::chrono::duration_cast<
              std::chrono::milliseconds>(now_log - diag_last_log)
                                      .count();
          REXGPU_INFO(
              "WAIT_REG_MEM long-waits: {} in {} ms "
              "(~{:.1f}/s, total sleep {} ms/s, vsync={})",
              diag_waits, elapsed_ms,
              1000.0 * static_cast<double>(diag_waits) /
                  static_cast<double>(elapsed_ms),
              diag_sleep_ms, REXCVAR_GET(vsync) ? 1 : 0);
          diag_waits = 0;
          diag_sleep_ms = 0;
          diag_last_log = now_log;
        }
        PrepareForWait();
        if (!REXCVAR_GET(vsync)) {
          // User wants it fast and dangerous.
          rex::thread::MaybeYield();
        } else {
          rex::thread::Sleep(std::chrono::milliseconds(wait / 0x100));
        }
        rex::thread::SyncMemory();
        ReturnFromWait();

        if (!worker_running_) {
          // Short-circuited exit.
          return false;
        }
      } else {
        rex::thread::MaybeYield();
      }
    }
  } while (!matched);

  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_RMW(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  // register read/modify/write
  // ? (used during shader upload and edram setup)
  uint32_t rmw_info = reader->ReadAndSwap<uint32_t>();
  uint32_t and_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t or_mask = reader->ReadAndSwap<uint32_t>();
  uint32_t value = register_file_->values[rmw_info & 0x1FFF];
  if ((rmw_info >> 31) & 0x1) {
    // & reg
    value &= register_file_->values[and_mask & 0x1FFF];
  } else {
    // & imm
    value &= and_mask;
  }
  if ((rmw_info >> 30) & 0x1) {
    // | reg
    value |= register_file_->values[or_mask & 0x1FFF];
  } else {
    // | imm
    value |= or_mask;
  }
  WriteRegister(rmw_info & 0x1FFF, value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_REG_TO_MEM(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // Copy Register to Memory (?)
  // Count is 2, assuming a Register Addr and a Memory Addr.

  uint32_t reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t mem_addr = reader->ReadAndSwap<uint32_t>();

  uint32_t reg_val = ReadRegisterValue(reg_addr);

  auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
  mem_addr &= ~0x3;
  reg_val = GpuSwap(reg_val, endianness);
  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);

  return true;
}

bool CommandProcessor::ExecutePacketType3_MEM_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  uint32_t write_addr = reader->ReadAndSwap<uint32_t>();
  for (uint32_t i = 0; i < count - 1; i++) {
    uint32_t write_data = reader->ReadAndSwap<uint32_t>();

    auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
    auto addr = write_addr & ~0x3;
    write_data = GpuSwap(write_data, endianness);
    memory::store(memory_->TranslatePhysical(addr), write_data);
    write_addr += 4;
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3_COND_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                     uint32_t count) {
  // conditional write to memory or register
  uint32_t wait_info = reader->ReadAndSwap<uint32_t>();
  uint32_t poll_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t ref = reader->ReadAndSwap<uint32_t>();
  uint32_t mask = reader->ReadAndSwap<uint32_t>();
  uint32_t write_reg_addr = reader->ReadAndSwap<uint32_t>();
  uint32_t write_data = reader->ReadAndSwap<uint32_t>();
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory.
    auto endianness = static_cast<xenos::Endian>(poll_reg_addr & 0x3);
    poll_reg_addr &= ~0x3;
    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));
    value = GpuSwap(value, endianness);
  } else {
    // Register.
    value = ReadRegisterValue(poll_reg_addr);
  }
  bool matched = false;
  switch (wait_info & 0x7) {
    case 0x0:  // Never.
      matched = false;
      break;
    case 0x1:  // Less than reference.
      matched = (value & mask) < ref;
      break;
    case 0x2:  // Less than or equal to reference.
      matched = (value & mask) <= ref;
      break;
    case 0x3:  // Equal to reference.
      matched = (value & mask) == ref;
      break;
    case 0x4:  // Not equal to reference.
      matched = (value & mask) != ref;
      break;
    case 0x5:  // Greater than or equal to reference.
      matched = (value & mask) >= ref;
      break;
    case 0x6:  // Greater than reference.
      matched = (value & mask) > ref;
      break;
    case 0x7:  // Always
      matched = true;
      break;
  }
  if (matched) {
    // Write.
    if (wait_info & 0x100) {
      // Memory.
      auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
      write_reg_addr &= ~0x3;
      write_data = GpuSwap(write_data, endianness);
      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);
    } else {
      // Register.
      WriteRegister(write_reg_addr, write_data);
    }
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // generate an event that creates a write to memory when completed
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  if (count == 1) {
    // Just an event flag? Where does this write?
  } else {
    // Write to an address.
    assert_always();
    reader->AdvanceRead((count - 1) * sizeof(uint32_t));
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a VS|PS_done event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  uint32_t value = reader->ReadAndSwap<uint32_t>();

  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t data_value;
  if ((initiator >> 31) & 0x1) {
    // Write counter (GPU vblank counter?).
    data_value = counter_;
  } else {
    // Write value.
    data_value = value;
  }
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;
  data_value = GpuSwap(data_value, endianness);
  memory::store(memory_->TranslatePhysical(address), data_value);
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_EXT(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // generate a screen extent event
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  auto endianness = static_cast<xenos::Endian>(address & 0x3);
  address &= ~0x3;

  // Let us hope we can fake this.
  // This callback tells the driver the xy coordinates affected by a previous
  // drawcall.
  // https://www.google.com/patents/US20060055701
  uint16_t extents[] = {
      0 >> 3,                                    // min x
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max x
      0 >> 3,                                    // min y
      xenos::kTexture2DCubeMaxWidthHeight >> 3,  // max y
      0,                                         // min z
      1,                                         // max z
  };
  assert_true(endianness == xenos::Endian::k8in16);
  memory::copy_and_swap_16_unaligned(memory_->TranslatePhysical(address), extents,
                                     rex::countof(extents));
  return true;
}

bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                          uint32_t packet, uint32_t count) {
  // Set by D3D as BE but struct ABI is LE
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  // Writeback initiator.
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  // Occlusion queries:
  // This command is send on query begin and end.
  // As a workaround report some fixed amount of passed samples.
  auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
  if (fake_sample_count >= 0) {
    auto* pSampleCounts = memory_->TranslatePhysical<xe_gpu_depth_sample_counts*>(
        register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR]);
    if (!pSampleCounts) {
      return true;
    }
    // 0xFFFFFEED is written to this two locations by D3D only on D3DISSUE_END
    // and used to detect a finished query.
    bool is_end_via_z_pass =
        pSampleCounts->ZPass_A == kQueryFinished || pSampleCounts->ZPass_B == kQueryFinished;
    // Older versions of D3D also checks for ZFail (4D5307D5).
    bool is_end_via_z_fail =
        pSampleCounts->ZFail_A == kQueryFinished || pSampleCounts->ZFail_B == kQueryFinished;
    std::memset(pSampleCounts, 0, sizeof(xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      pSampleCounts->ZPass_A = fake_sample_count;
      pSampleCounts->Total_A = fake_sample_count;
    }
  }

  return true;
}

bool CommandProcessor::ExecutePacketType3Draw(memory::RingBuffer* reader, uint32_t packet,
                                              const char* opcode_name, uint32_t viz_query_condition,
                                              uint32_t count_remaining) {
  // if viz_query_condition != 0, this is a conditional draw based on viz query.
  // This ID matches the one issued in PM4_VIZ_QUERY
  // uint32_t viz_id = viz_query_condition & 0x3F;
  // when true, render conditionally based on query result
  // uint32_t viz_use = viz_query_condition & 0x100;

  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("{}: Packet too small, can't read VGT_DRAW_INITIATOR", opcode_name);
    return false;
  }
  reg::VGT_DRAW_INITIATOR vgt_draw_initiator;
  vgt_draw_initiator.value = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, vgt_draw_initiator.value);

  bool draw_succeeded = true;
  // TODO(Triang3l): Remove IndexBufferInfo and replace handling of all this
  // with PrimitiveProcessor when the old Vulkan renderer is removed.
  bool is_indexed = false;
  IndexBufferInfo index_buffer_info;
  switch (vgt_draw_initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      // Indexed draw.
      is_indexed = true;

      // Two separate bounds checks so if there's only one missing register
      // value out of two, one uint32_t will be skipped in the command buffer,
      // not two.
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_BASE", opcode_name);
        return false;
      }
      uint32_t vgt_dma_base = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, vgt_dma_base);
      reg::VGT_DMA_SIZE vgt_dma_size;
      assert_not_zero(count_remaining);
      if (!count_remaining) {
        REXGPU_ERROR("{}: Packet too small, can't read VGT_DMA_SIZE", opcode_name);
        return false;
      }
      vgt_dma_size.value = reader->ReadAndSwap<uint32_t>();
      --count_remaining;
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, vgt_dma_size.value);

      uint32_t index_size_bytes = vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16
                                      ? sizeof(uint16_t)
                                      : sizeof(uint32_t);
      // The base address must already be word-aligned according to the R6xx
      // documentation, but for safety.
      index_buffer_info.guest_base = vgt_dma_base & ~(index_size_bytes - 1);
      index_buffer_info.endianness = vgt_dma_size.swap_mode;
      index_buffer_info.format = vgt_draw_initiator.index_size;
      index_buffer_info.length = vgt_dma_size.num_words * index_size_bytes;
      index_buffer_info.count = vgt_draw_initiator.num_indices;
    } break;
    case xenos::SourceSelect::kImmediate: {
      // TODO(Triang3l): VGT_IMMED_DATA.
      REXGPU_ERROR(
          "{}: Using immediate vertex indices, which are not supported yet. "
          "Report the game to Xenia developers!",
          opcode_name, uint32_t(vgt_draw_initiator.source_select));
      draw_succeeded = false;
      assert_always();
    } break;
    case xenos::SourceSelect::kAutoIndex: {
      // Auto draw.
      index_buffer_info.guest_base = 0;
      index_buffer_info.length = 0;
    } break;
    default: {
      // Invalid source selection.
      draw_succeeded = false;
      assert_unhandled_case(vgt_draw_initiator.source_select);
    } break;
  }

  // Skip to the next command, for example, if there are immediate indexes that
  // we don't support yet.
  reader->AdvanceRead(count_remaining * sizeof(uint32_t));

  if (draw_succeeded) {
    auto viz_query = register_file_->Get<reg::PA_SC_VIZ_QUERY>();
    if (!(viz_query.viz_query_ena && viz_query.kill_pix_post_hi_z)) {
      // TODO(Triang3l): Don't drop the draw call completely if the vertex
      // shader has memexport.
      // TODO(Triang3l || JoelLinn): Handle this properly in the render
      // backends.

      bool major_mode_explicit =
          xenos::IsMajorModeExplicit(vgt_draw_initiator.major_mode, vgt_draw_initiator.prim_type);
      draw_succeeded = IssueDraw(vgt_draw_initiator.prim_type, vgt_draw_initiator.num_indices,
                                 is_indexed ? &index_buffer_info : nullptr, major_mode_explicit);
      if (!draw_succeeded) {
        auto vgt_output_path_cntl = register_file_->Get<reg::VGT_OUTPUT_PATH_CNTL>();
        auto vgt_hos_cntl = register_file_->Get<reg::VGT_HOS_CNTL>();
        auto rb_modecontrol = register_file_->Get<reg::RB_MODECONTROL>();
        REXGPU_ERROR(
            "{}({}, {}, {}): Failed in backend "
            "(major_mode={}, explicit_major={}, path_select={}, tess_mode={}, edram_mode={})",
            opcode_name, static_cast<uint32_t>(vgt_draw_initiator.num_indices),
            uint32_t(vgt_draw_initiator.prim_type), uint32_t(vgt_draw_initiator.source_select),
            uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
            uint32_t(vgt_output_path_cntl.path_select), uint32_t(vgt_hos_cntl.tess_mode),
            uint32_t(rb_modecontrol.edram_mode));
      }
    }
  }

  // If read the packed correctly, but merely couldn't execute it (because of,
  // for instance, features not supported by the host), don't terminate command
  // buffer processing as that would leave rendering in a way more inconsistent
  // state than just a single dropped draw command.
  return true;
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // "initiate fetch of index buffer and draw"
  // Generally used by Xbox 360 Direct3D 9 for kDMA and kAutoIndex sources.
  // With a viz query token as the first one.
  uint32_t count_remaining = count;
  assert_not_zero(count_remaining);
  if (!count_remaining) {
    REXGPU_ERROR("PM4_DRAW_INDX: Packet too small, can't read the viz query token");
    return false;
  }
  uint32_t viz_query_condition = reader->ReadAndSwap<uint32_t>();
  --count_remaining;
  // PM4_DRAW_INDX is a regular indexed draw (may carry a viz query token). The
  // hero/dog EDRAM-resolve copy is NOT issued with this opcode, so never
  // force-readback from it.
  may_require_readback_resolve_ = false;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX", viz_query_condition,
                                count_remaining);
}

bool CommandProcessor::ExecutePacketType3_DRAW_INDX_2(memory::RingBuffer* reader, uint32_t packet,
                                                      uint32_t count) {
  // "draw using supplied indices in packet"
  // Generally used by Xbox 360 Direct3D 9 for kAutoIndex source.
  // No viz query token.
  // This is the opcode the EDRAM-resolve copy (e.g. the hero/dog texture blit)
  // is issued with, so it is the only one that may force a readback.
  may_require_readback_resolve_ = true;
  return ExecutePacketType3Draw(reader, packet, "PM4_DRAW_INDX_2", 0, count);
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT(memory::RingBuffer* reader, uint32_t packet,
                                                       uint32_t count) {
  // load constant into chip and to memory
  // PM4_REG(reg) ((0x4 << 16) | (GSL_HAL_SUBBLOCK_OFFSET(reg)))
  //                                     reg - 0x2000
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t count_registers = count - 1;
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromRing(reader, index, count_registers);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromRing(reader, index, count_registers);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromRing(reader, index, count_registers);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromRing(reader, index, count_registers);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromRing(reader, index, count_registers);
      break;
    default:
      assert_always();
      reader->AdvanceRead((count - 1) * sizeof(uint32_t));
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_CONSTANT2(memory::RingBuffer* reader, uint32_t packet,
                                                        uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_LOAD_ALU_CONSTANT(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  // load constants from memory
  uint32_t address = reader->ReadAndSwap<uint32_t>();
  address &= 0x3FFFFFFF;
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0x7FF;
  uint32_t size_dwords = reader->ReadAndSwap<uint32_t>();
  size_dwords &= 0xFFF;
  uint32_t type = (offset_type >> 16) & 0xFF;
  uint32_t* xlat_address = memory_->TranslatePhysical<uint32_t*>(address);
  switch (type) {
    case 0:  // ALU
      WriteALURangeFromMem(index, xlat_address, size_dwords);
      break;
    case 1:  // FETCH
      WriteFetchRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 2:  // BOOL
      WriteBoolRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 3:  // LOOP
      WriteLoopRangeFromMem(index, xlat_address, size_dwords);
      break;
    case 4:  // REGISTERS
      WriteREGISTERSRangeFromMem(index, xlat_address, size_dwords);
      break;
    default:
      assert_always();
      return true;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_SET_SHADER_CONSTANTS(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
  uint32_t index = offset_type & 0xFFFF;
  WriteRegisterRangeFromRing(reader, index, count - 1);
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (pointer-based)
  uint32_t addr_type = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(addr_type & 0x3);
  uint32_t addr = addr_type & ~0x3;
  uint32_t start_size = reader->ReadAndSwap<uint32_t>();
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);

  auto shader =
      LoadShader(shader_type, addr, memory_->TranslatePhysical<uint32_t*>(addr), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  return true;
}

bool CommandProcessor::ExecutePacketType3_IM_LOAD_IMMEDIATE(memory::RingBuffer* reader,
                                                            uint32_t packet, uint32_t count) {
  SCOPE_profile_cpu_f("gpu");

  // load sequencer instruction memory (code embedded in packet)
  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();
  uint32_t dword1 = reader->ReadAndSwap<uint32_t>();
  auto shader_type = static_cast<xenos::ShaderType>(dword0);
  uint32_t start_size = dword1;
  uint32_t start = start_size >> 16;
  uint32_t size_dwords = start_size & 0xFFFF;  // dwords
  assert_true(start == 0);
  assert_true(reader->read_count() >= size_dwords * 4);
  assert_true(count - 2 >= size_dwords);
  auto shader = LoadShader(shader_type, uint32_t(reader->read_ptr()),
                           reinterpret_cast<uint32_t*>(reader->read_ptr()), size_dwords);
  switch (shader_type) {
    case xenos::ShaderType::kVertex:
      active_vertex_shader_ = shader;
      break;
    case xenos::ShaderType::kPixel:
      active_pixel_shader_ = shader;
      break;
    default:
      assert_unhandled_case(shader_type);
      return false;
  }
  reader->AdvanceRead(size_dwords * sizeof(uint32_t));
  return true;
}

bool CommandProcessor::ExecutePacketType3_INVALIDATE_STATE(memory::RingBuffer* reader,
                                                           uint32_t packet, uint32_t count) {
  // selective invalidation of state pointers
  /*uint32_t mask =*/reader->ReadAndSwap<uint32_t>();
  // driver_->InvalidateState(mask);
  return true;
}

bool CommandProcessor::ExecutePacketType3_VIZ_QUERY(memory::RingBuffer* reader, uint32_t packet,
                                                    uint32_t count) {
  // begin/end initiator for viz query extent processing
  // https://www.google.com/patents/US20050195186
  assert_true(count == 1);

  uint32_t dword0 = reader->ReadAndSwap<uint32_t>();

  uint32_t id = dword0 & 0x3F;
  uint32_t end = dword0 & 0x100;
  if (!end) {
    // begin a new viz query @ id
    // On hardware this clears the internal state of the scan converter (which
    // is different to the register)
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_START);
    REXGPU_INFO("Begin viz query ID {:02X}", id);
  } else {
    // end the viz query
    WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, VIZQUERY_END);
    REXGPU_INFO("End viz query ID {:02X}", id);
    // The scan converter writes the internal result back to the register here.
    // We just fake it and say it was visible in case it is read back.
    // Through WriteRegister, so backends see the change (no side effects for
    // these registers).
    if (id < 32) {
      WriteRegister(XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0,
                    register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |
                        (uint32_t(1) << id));
    } else {
      WriteRegister(XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1,
                    register_file_->values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |
                        (uint32_t(1) << (id - 32)));
    }
  }

  return true;
}


// PM4 parse thread.

void CommandProcessor::RunWithParseThread() {
  parse_ring_ = std::make_unique<uint32_t[]>(kParseRingDwords);
  parse_head_.store(0, std::memory_order_relaxed);
  parse_tail_.store(0, std::memory_order_relaxed);
  parse_head_local_ = 0;
  parse_head_published_ = 0;
  parse_tail_cached_ = 0;
  parse_head_cached_ = 0;
  parse_read_ptr_index_ = read_ptr_index_;
  parse_bin_select_ = bin_select_;
  parse_bin_mask_ = bin_mask_;
  std::memcpy(parse_constants_, register_file_->values + kParseConstantsFirst,
              sizeof(parse_constants_));
  parse_thread_exit_.store(false);
  rex::thread::Thread::CreationParameters parse_thread_params;
  parse_thread_params.stack_size = 1024 * 1024;
  parse_thread_ =
      rex::thread::Thread::Create(parse_thread_params, [this]() { ParseThreadMain(); });
  if (!parse_thread_) {
    parse_ring_.reset();
    rex::FatalError("Unable to create the PM4 parse thread (set gpu_parse_thread = false)");
    return;
  }
  parse_thread_->set_name("GPU Parse");
  parse_thread_enabled_ = true;
  REXGPU_INFO("PM4 command stream parsed on a separate thread");

  while (worker_running_) {
    while (!pending_fns_.empty()) {
      auto fn = std::move(pending_fns_.front());
      pending_fns_.pop();
      fn();
    }
    if (ConsumeParsedCommands()) {
      continue;
    }
    // Nothing parsed to execute - don't hold back prepared work.
#if REX_PLATFORM_PS5
    const auto idle_start = std::chrono::steady_clock::now();
#endif
    PrepareForWait();
    bool available = false;
    for (uint32_t i = 0; i < 4096; ++i) {
      if (parse_head_.load(std::memory_order_acquire) !=
          parse_tail_.load(std::memory_order_relaxed)) {
        available = true;
        break;
      }
      _mm_pause();
    }
    if (!available) {
      std::unique_lock<std::mutex> lock(parse_mutex_);
      parse_consumer_sleeping_.store(true, std::memory_order_seq_cst);
      while (worker_running_ && pending_fns_.empty() &&
             parse_head_.load(std::memory_order_seq_cst) ==
                 parse_tail_.load(std::memory_order_relaxed)) {
        parse_consumer_cv_.wait_for(lock, std::chrono::milliseconds(5));
      }
      parse_consumer_sleeping_.store(false, std::memory_order_relaxed);
    }
    ReturnFromWait();
#if REX_PLATFORM_PS5
    rex::ps5prof::counters().parse_consumer_idle_ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - idle_start)
                     .count()),
        std::memory_order_relaxed);
#endif
  }

  {
    std::lock_guard<std::mutex> lock(parse_mutex_);
    parse_thread_exit_.store(true);
  }
  parse_producer_cv_.notify_all();
  write_ptr_index_event_->Set();
  rex::thread::Wait(parse_thread_.get(), false);
  parse_thread_.reset();
  parse_thread_enabled_ = false;
  parse_ring_.reset();
}

bool CommandProcessor::ConsumeParsedCommands() {
  uint64_t tail = parse_tail_.load(std::memory_order_relaxed);
  if (tail == parse_head_cached_) {
    parse_head_cached_ = parse_head_.load(std::memory_order_acquire);
    if (tail == parse_head_cached_) {
      return false;
    }
  }
  uint32_t* const ring = parse_ring_.get();
  uint32_t executed = 0;
  while (tail != parse_head_cached_) {
    const uint32_t position = uint32_t(tail & (kParseRingDwords - 1));
    const uint32_t header = ring[position];
    const uint32_t length = header & 0xFFFFFF;
    switch (ParsedCommandType(header >> 24)) {
      case ParsedCommandType::kWrap:
        break;
      case ParsedCommandType::kRegisters:
        WriteRegistersFromMem(ring[position + 1], ring + position + 2, length - 2);
        break;
      case ParsedCommandType::kPacket: {
        const uint32_t packet_bytes = (length - 1) * uint32_t(sizeof(uint32_t));
        memory::RingBuffer reader(reinterpret_cast<uint8_t*>(ring + position + 1), packet_bytes);
        reader.set_write_offset(packet_bytes);
        if (!ExecutePacket(&reader)) {
          REXGPU_ERROR("**** PARSED PACKET: Failed to execute packet.");
        }
      } break;
      case ParsedCommandType::kPrimaryEnd:
        read_ptr_index_ = ring[position + 1];
        OnPrimaryBufferEnd();
        if (read_ptr_writeback_ptr_) {
          memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),
                                           read_ptr_index_);
        }
        break;
      case ParsedCommandType::kBatch:
        OnGuestSyncPoint(GuestSyncPoint::kBatch);
        break;
    }
    tail += length;
    // The parse thread may be waiting for this command (a synchronous packet)
    // or for room.
    parse_tail_.store(tail, std::memory_order_seq_cst);
    if (parse_producer_sleeping_.load(std::memory_order_seq_cst)) {
      std::lock_guard<std::mutex> lock(parse_mutex_);
      parse_producer_cv_.notify_one();
    }
    if (tail == parse_head_cached_) {
      parse_head_cached_ = parse_head_.load(std::memory_order_acquire);
    }
    if (!(++executed & 255) && !pending_fns_.empty()) {
      break;
    }
  }
  return true;
}

uint32_t* CommandProcessor::ParseReserve(uint32_t dword_count) {
  assert_true(dword_count < kParseRingDwords / 4);
  uint32_t position = uint32_t(parse_head_local_ & (kParseRingDwords - 1));
  uint32_t needed = dword_count;
  const bool wrap = position + dword_count > kParseRingDwords;
  if (wrap) {
    needed += uint32_t(kParseRingDwords) - position;
  }
  if (parse_head_local_ + needed - parse_tail_cached_ > kParseRingDwords) {
    parse_tail_cached_ = parse_tail_.load(std::memory_order_acquire);
    if (parse_head_local_ + needed - parse_tail_cached_ > kParseRingDwords) {
      ParsePublish();
      for (uint32_t i = 0; i < 4096; ++i) {
        parse_tail_cached_ = parse_tail_.load(std::memory_order_acquire);
        if (parse_head_local_ + needed - parse_tail_cached_ <= kParseRingDwords) {
          break;
        }
        _mm_pause();
      }
      if (parse_head_local_ + needed - parse_tail_cached_ > kParseRingDwords) {
        std::unique_lock<std::mutex> lock(parse_mutex_);
        parse_producer_sleeping_.store(true, std::memory_order_seq_cst);
        while (!parse_thread_exit_.load() &&
               parse_head_local_ + needed -
                       (parse_tail_cached_ = parse_tail_.load(std::memory_order_seq_cst)) >
                   kParseRingDwords) {
          parse_producer_cv_.wait_for(lock, std::chrono::milliseconds(5));
        }
        parse_producer_sleeping_.store(false, std::memory_order_relaxed);
      }
    }
  }
  if (wrap) {
    parse_ring_[position] =
        (uint32_t(ParsedCommandType::kWrap) << 24) | (uint32_t(kParseRingDwords) - position);
    parse_head_local_ += uint32_t(kParseRingDwords) - position;
    position = 0;
  }
  return parse_ring_.get() + position;
}

void CommandProcessor::ParsePublish() {
  if (parse_head_published_ == parse_head_local_) {
    return;
  }
  parse_head_published_ = parse_head_local_;
  parse_head_.store(parse_head_local_, std::memory_order_seq_cst);
  if (parse_consumer_sleeping_.load(std::memory_order_seq_cst)) {
    std::lock_guard<std::mutex> lock(parse_mutex_);
    parse_consumer_cv_.notify_one();
  }
}

void CommandProcessor::ParseWaitForConsumer() {
  ParsePublish();
  const uint64_t target = parse_head_local_;
#if REX_PLATFORM_PS5
  struct WaitTimer {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~WaitTimer() {
      auto& counters = rex::ps5prof::counters();
      counters.parse_sync_waits.fetch_add(1, std::memory_order_relaxed);
      counters.parse_sync_wait_ns.fetch_add(
          uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count()),
          std::memory_order_relaxed);
    }
  } wait_timer;
#endif
  for (uint32_t i = 0; i < 4096; ++i) {
    if (parse_tail_.load(std::memory_order_acquire) == target) {
      parse_tail_cached_ = target;
      return;
    }
    _mm_pause();
  }
  std::unique_lock<std::mutex> lock(parse_mutex_);
  parse_producer_sleeping_.store(true, std::memory_order_seq_cst);
  while (!parse_thread_exit_.load() && parse_tail_.load(std::memory_order_seq_cst) != target) {
    parse_producer_cv_.wait_for(lock, std::chrono::milliseconds(5));
  }
  parse_producer_sleeping_.store(false, std::memory_order_relaxed);
  parse_tail_cached_ = parse_tail_.load(std::memory_order_acquire);
}

void CommandProcessor::ParseEmitRegisters(uint32_t start, const uint32_t* source_be,
                                          uint32_t count) {
  constexpr uint32_t kMaxRun = 16384;
  while (count) {
    const uint32_t run = std::min(count, kMaxRun);
    uint32_t* record = ParseReserve(2 + run);
    record[0] = (uint32_t(ParsedCommandType::kRegisters) << 24) | (2 + run);
    record[1] = start;
    std::memcpy(record + 2, source_be, sizeof(uint32_t) * run);
    ParseCommit(2 + run);
    start += run;
    source_be += run;
    count -= run;
  }
}

void CommandProcessor::ParseWriteRange(uint32_t start, const uint32_t* source_be, uint32_t count) {
  if (!count) {
    return;
  }
  const uint32_t end = start + count;
  if (end <= kParseConstantsFirst || start >= kParseConstantsEnd) {
    ParseEmitRegisters(start, source_be, count);
    return;
  }
  if (start < kParseConstantsFirst) {
    const uint32_t before = kParseConstantsFirst - start;
    ParseEmitRegisters(start, source_be, before);
    start += before;
    source_be += before;
    count -= before;
  }
  const uint32_t inside = std::min(end, kParseConstantsEnd) - start;
  // Only the groups of 8 constants with a changed value are forwarded.
  uint32_t* shadow = parse_constants_ + (start - kParseConstantsFirst);
  uint32_t run_begin = UINT32_MAX, run_end = 0;
  for (uint32_t i = 0; i < inside; i += 8) {
    const uint32_t group = std::min(inside - i, uint32_t(8));
    bool changed = false;
#if defined(__AVX2__)
    if (group == 8) {
      const __m256i swap_shuffle =
          _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6,
                           5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
      const __m256i value = _mm256_shuffle_epi8(
          _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source_be + i)), swap_shuffle);
      const __m256i old_value = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadow + i));
      if (uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi32(value, old_value))) != 0xFFFFFFFFu) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(shadow + i), value);
        changed = true;
      }
    } else
#endif
    {
      for (uint32_t j = 0; j < group; ++j) {
        const uint32_t value = rex::byte_swap(source_be[i + j]);
        if (shadow[i + j] != value) {
          shadow[i + j] = value;
          changed = true;
        }
      }
    }
    if (changed) {
      if (run_begin == UINT32_MAX) {
        run_begin = i;
      }
      run_end = i + group;
    } else if (run_begin != UINT32_MAX) {
      ParseEmitRegisters(start + run_begin, source_be + run_begin, run_end - run_begin);
      run_begin = UINT32_MAX;
    }
  }
  if (run_begin != UINT32_MAX) {
    ParseEmitRegisters(start + run_begin, source_be + run_begin, run_end - run_begin);
  }
  if (count > inside) {
    ParseEmitRegisters(start + inside, source_be + inside, count - inside);
  }
}

void CommandProcessor::ParseWriteRangeFromRing(memory::RingBuffer* reader, uint32_t start,
                                               uint32_t count) {
  if (!count) {
    return;
  }
  memory::RingBuffer::ReadRange range = reader->BeginRead(size_t(count) * sizeof(uint32_t));
  if (range.first_length) {
    const uint32_t first_count = uint32_t(range.first_length / sizeof(uint32_t));
    ParseWriteRange(start, reinterpret_cast<const uint32_t*>(range.first), first_count);
    start += first_count;
  }
  if (range.second_length) {
    ParseWriteRange(start, reinterpret_cast<const uint32_t*>(range.second),
                    uint32_t(range.second_length / sizeof(uint32_t)));
  }
  reader->EndRead(range);
}

void CommandProcessor::ParseForwardPacket(memory::RingBuffer* reader, uint32_t packet,
                                          uint32_t count, bool sync, bool refresh_constants,
                                          uint32_t first_dword_override) {
  uint32_t* record = ParseReserve(2 + count);
  record[0] = (uint32_t(ParsedCommandType::kPacket) << 24) | (2 + count);
  record[1] = rex::byte_swap(packet);
  if (count) {
    memory::RingBuffer::ReadRange range = reader->BeginRead(size_t(count) * sizeof(uint32_t));
    std::memcpy(record + 2, range.first, range.first_length);
    if (range.second_length) {
      std::memcpy(reinterpret_cast<uint8_t*>(record + 2) + range.first_length, range.second,
                  range.second_length);
    }
    reader->EndRead(range);
    if (first_dword_override != UINT32_MAX) {
      record[2] = rex::byte_swap(first_dword_override);
    }
  }
  ParseCommit(2 + count);
  if (sync) {
    ParseWaitForConsumer();
    if (refresh_constants) {
      std::memcpy(parse_constants_, register_file_->values + kParseConstantsFirst,
                  sizeof(parse_constants_));
    }
  } else {
    ParsePublish();
  }
}

bool CommandProcessor::ParseExecutePacket(memory::RingBuffer* reader) {
  const uint32_t packet = reader->ReadAndSwap<uint32_t>();
  if (!packet) {
    return true;
  }
  switch (packet >> 30) {
    case 0: {
      const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (reader->read_count() < count * sizeof(uint32_t)) {
        REXGPU_ERROR("ParseExecutePacket type 0 overflow");
        return false;
      }
      const uint32_t base_index = packet & 0x7FFF;
      if (!((packet >> 15) & 0x1)) {
        ParseWriteRangeFromRing(reader, base_index, count);
      } else {
        for (uint32_t m = 0; m < count; ++m) {
          const uint32_t value = reader->Read<uint32_t>();
          ParseWriteRange(base_index, &value, 1);
        }
      }
      return true;
    }
    case 1: {
      const uint32_t value_1 = reader->Read<uint32_t>();
      const uint32_t value_2 = reader->Read<uint32_t>();
      ParseWriteRange(packet & 0x7FF, &value_1, 1);
      ParseWriteRange((packet >> 11) & 0x7FF, &value_2, 1);
      return true;
    }
    case 2:
      return true;
    default:
      break;
  }
  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  const auto data_start_offset = reader->read_offset();
  if (reader->read_count() < count * sizeof(uint32_t)) {
    REXGPU_ERROR("ParseExecutePacket type 3 overflow");
    return false;
  }
  if (packet & 1) {
    const bool any_pass = (parse_bin_select_ & parse_bin_mask_) != 0;
    if (!any_pass || opcode == xenos::PM4_XE_SWAP) {
      reader->AdvanceRead(count * sizeof(uint32_t));
      return true;
    }
  }
  static constexpr uint32_t kConstantTypeBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
  switch (opcode) {
    case xenos::PM4_NOP:
      break;
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      const uint32_t list_ptr = CpuToGpu(reader->ReadAndSwap<uint32_t>());
      const uint32_t list_length = reader->ReadAndSwap<uint32_t>() & 0xFFFFF;
      ParseIndirectBuffer(GpuToCpu(list_ptr), list_length);
    } break;
    case xenos::PM4_SET_CONSTANT: {
      const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
      const uint32_t type = (offset_type >> 16) & 0xFF;
      if (type < 5) {
        ParseWriteRangeFromRing(reader, kConstantTypeBases[type] + (offset_type & 0x7FF),
                                count - 1);
      }
    } break;
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS: {
      const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
      ParseWriteRangeFromRing(reader, offset_type & 0xFFFF, count - 1);
    } break;
    case xenos::PM4_LOAD_ALU_CONSTANT: {
      const uint32_t address = reader->ReadAndSwap<uint32_t>() & 0x3FFFFFFF;
      const uint32_t offset_type = reader->ReadAndSwap<uint32_t>();
      const uint32_t size_dwords = reader->ReadAndSwap<uint32_t>() & 0xFFF;
      const uint32_t type = (offset_type >> 16) & 0xFF;
      if (type < 5) {
        ParseWriteRange(kConstantTypeBases[type] + (offset_type & 0x7FF),
                        memory_->TranslatePhysical<const uint32_t*>(address), size_dwords);
      }
    } break;
    case xenos::PM4_SET_BIN_MASK_LO:
    case xenos::PM4_SET_BIN_MASK_HI:
    case xenos::PM4_SET_BIN_SELECT_LO:
    case xenos::PM4_SET_BIN_SELECT_HI:
    case xenos::PM4_SET_BIN_MASK:
    case xenos::PM4_SET_BIN_SELECT: {
      // Followed here (for the predicated packets parsed here) and forwarded.
      memory::RingBuffer peek = *reader;
      const uint32_t value_0 = peek.ReadAndSwap<uint32_t>();
      const uint32_t value_1 = count >= 2 ? peek.ReadAndSwap<uint32_t>() : 0;
      switch (opcode) {
        case xenos::PM4_SET_BIN_MASK_LO:
          parse_bin_mask_ = (parse_bin_mask_ & 0xFFFFFFFF00000000ull) | value_0;
          break;
        case xenos::PM4_SET_BIN_MASK_HI:
          parse_bin_mask_ = (parse_bin_mask_ & 0xFFFFFFFFull) | (uint64_t(value_0) << 32);
          break;
        case xenos::PM4_SET_BIN_SELECT_LO:
          parse_bin_select_ = (parse_bin_select_ & 0xFFFFFFFF00000000ull) | value_0;
          break;
        case xenos::PM4_SET_BIN_SELECT_HI:
          parse_bin_select_ = (parse_bin_select_ & 0xFFFFFFFFull) | (uint64_t(value_0) << 32);
          break;
        case xenos::PM4_SET_BIN_MASK:
          parse_bin_mask_ = (uint64_t(value_0) << 32) | value_1;
          break;
        default:
          parse_bin_select_ = (uint64_t(value_0) << 32) | value_1;
          break;
      }
      ParseForwardPacket(reader, packet, count, false, false);
    } break;
    case xenos::PM4_WAIT_REG_MEM: {
      // May wait for the CPU to write data the following packets load - the
      // parse thread mustn't run ahead of an unsatisfied memory wait.
      memory::RingBuffer peek = *reader;
      const uint32_t wait_info = peek.ReadAndSwap<uint32_t>();
      const uint32_t poll_reg_addr = peek.ReadAndSwap<uint32_t>();
      const uint32_t ref = peek.ReadAndSwap<uint32_t>();
      const uint32_t mask = peek.ReadAndSwap<uint32_t>();
      if (!(wait_info & 0x10)) {
        // Register waits: COHER_STATUS_HOST (cleared by MakeCoherent when
        // executed) needs nothing from here; others may depend on the CPU.
        ParseForwardPacket(reader, packet, count,
                           poll_reg_addr != XE_GPU_REG_COHER_STATUS_HOST, false);
        break;
      }
      uint32_t value = *reinterpret_cast<const uint32_t*>(
          memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));
      value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_reg_addr & 0x3));
      value &= mask;
      bool matched = false;
      switch (wait_info & 0x7) {
        case 0x1:
          matched = value < ref;
          break;
        case 0x2:
          matched = value <= ref;
          break;
        case 0x3:
          matched = value == ref;
          break;
        case 0x4:
          matched = value != ref;
          break;
        case 0x5:
          matched = value >= ref;
          break;
        case 0x6:
          matched = value > ref;
          break;
        case 0x7:
          matched = true;
          break;
        default:
          break;
      }
      if (matched) {
        // Already satisfied: forwarded as an "always" wait (the value may
        // change again before it's executed).
        ParseForwardPacket(reader, packet, count, false, false, (wait_info & ~uint32_t(0x7)) | 0x7);
      } else {
        ParseForwardPacket(reader, packet, count, true, false);
      }
    } break;
    case xenos::PM4_REG_RMW:
      // Only registers below the constants (0x1FFF mask) - executed in order.
      ParseForwardPacket(reader, packet, count, false, false);
      break;
    case xenos::PM4_COND_WRITE: {
      memory::RingBuffer peek = *reader;
      const uint32_t wait_info = peek.ReadAndSwap<uint32_t>();
      peek.AdvanceRead(3 * sizeof(uint32_t));
      const uint32_t write_reg_addr = peek.ReadAndSwap<uint32_t>();
      const bool writes_constant = !(wait_info & 0x100) &&
                                   write_reg_addr >= kParseConstantsFirst &&
                                   write_reg_addr < kParseConstantsEnd;
      ParseForwardPacket(reader, packet, count, writes_constant, writes_constant);
    } break;
    default:
      ParseForwardPacket(reader, packet, count, false, false);
      break;
  }
  reader->set_read_offset((data_start_offset + count * sizeof(uint32_t)) % reader->capacity());
  return true;
}

void CommandProcessor::ParseIndirectBuffer(uint32_t ptr, uint32_t count) {
  memory::RingBuffer reader(memory_->TranslatePhysical(ptr), count * sizeof(uint32_t));
  reader.set_write_offset(count * sizeof(uint32_t));
  do {
    if (!ParseExecutePacket(&reader)) {
      REXGPU_ERROR("**** INDIRECT RINGBUFFER (parse): Failed to execute packet.");
      break;
    }
  } while (reader.read_count());
}

void CommandProcessor::ParseThreadMain() {
#if REX_PLATFORM_PS5
  rex::ps5prof::RegisterCurrentThread("GPU Parse");
#endif
  while (!parse_thread_exit_.load(std::memory_order_relaxed)) {
    uint32_t write_ptr_index = write_ptr_index_.load();
    if (write_ptr_index == 0xBAADF00D || parse_read_ptr_index_ == write_ptr_index) {
      ParsePublish();
      uint32_t loop_count = 0;
      do {
        if (loop_count > 500) {
          rex::thread::Wait(write_ptr_index_event_.get(), true, std::chrono::milliseconds(5));
        }
        rex::thread::MaybeYield();
        loop_count++;
        write_ptr_index = write_ptr_index_.load();
      } while (!parse_thread_exit_.load(std::memory_order_relaxed) &&
               (write_ptr_index == 0xBAADF00D || parse_read_ptr_index_ == write_ptr_index));
      if (parse_thread_exit_.load(std::memory_order_relaxed)) {
        break;
      }
    }
    {
      uint32_t* record = ParseReserve(1);
      record[0] = (uint32_t(ParsedCommandType::kBatch) << 24) | 1;
      ParseCommit(1);
    }
    memory::RingBuffer reader(memory_->TranslatePhysical(primary_buffer_ptr_),
                              primary_buffer_size_);
    reader.set_read_offset(parse_read_ptr_index_ * sizeof(uint32_t));
    reader.set_write_offset(write_ptr_index * sizeof(uint32_t));
    do {
      if (!ParseExecutePacket(&reader)) {
        REXGPU_ERROR("**** PRIMARY RINGBUFFER (parse): Failed to execute packet.");
        break;
      }
    } while (reader.read_count());
    parse_read_ptr_index_ = write_ptr_index;
    {
      uint32_t* record = ParseReserve(2);
      record[0] = (uint32_t(ParsedCommandType::kPrimaryEnd) << 24) | 2;
      record[1] = write_ptr_index;
      ParseCommit(2);
    }
    ParsePublish();
  }
  ParsePublish();
}

}  // namespace rex::graphics
