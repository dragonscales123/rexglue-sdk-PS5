/**
 * Points in the command stream where the guest CPU and the emulated GPU
 * synchronize (see SharedMemory::AdvanceHotCheckEpoch).
 */

#pragma once

#include <cstdint>

namespace rex::graphics {

enum class GuestSyncPoint : uint32_t {
  // A new batch from the ring buffer: commands (and the data they use) the
  // CPU wrote after anything the GPU has processed so far.
  kBatch,
  // WAIT_REG_MEM: the GPU waits for a value the CPU writes, so the CPU may
  // have rewritten data for the commands after the wait.
  kWait,
  kInterrupt,
  kSwap,
  // The GPU wrote memory the CPU can read (fences, MEM_WRITE, scratch
  // writeback...). Commands the CPU writes in response come in a later batch.
  kGpuSignal,
  kCount,
};

}  // namespace rex::graphics
