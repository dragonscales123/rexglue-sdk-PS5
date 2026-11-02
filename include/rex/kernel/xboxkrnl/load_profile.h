/**
 * Load profiler and the experimental faster_loading boosts.
 *
 * faster_loading (off by default, EXPERIMENTAL): finds loads from the disk read
 * rate and, only while a load runs, briefly runs the guest clock faster when
 *   - the game mostly waits on its own timers (idle_boost), or
 *   - no frame is presented for a while (stall_boost),
 * so guest-time waits and timeouts end sooner. A small monitor thread and
 * per-read/per-sleep bookkeeping run only while it (or diag_load_profile) is on.
 *
 * diag_load_profile: counts guest disk reads, sleeps, waits and yields per guest
 * thread and logs them once a second with the CPU use of the busiest threads
 * ([load-prof] lines), plus long waits and stalls.
 */

#pragma once

#include <cstdint>

namespace rex::kernel::xboxkrnl::load_profile {

// Whether diag_load_profile is on (detailed profiling and wait tracking).
bool Enabled();
// Whether faster_loading or diag_load_profile is on (read/sleep/frame bookkeeping).
bool Active();
// Whether guest file reads should be counted (Active() or load_timer).
bool ReadsTracked();
// Monotonic microseconds for timing a call.
uint64_t NowUs();

void AddRead(uint64_t bytes, uint64_t elapsed_us);
// requested_interval is the guest interval (negative = relative, 100 ns units).
void AddDelay(uint64_t elapsed_us, int64_t requested_interval);
void AddWait(uint64_t elapsed_us);
void AddYield();

// Wait monitor (diag_load_profile): the wait a guest thread is in, so long
// waits can be logged with the game functions that made them.
enum WaitKind : uint32_t {
  kWaitKeSingle = 1,
  kWaitNtSingle,
  kWaitKeMultiple,
  kWaitNtMultiple,
  kWaitSignalAndWait,
  kWaitSleep,
};
// `what` must be a string literal (object type); for kWaitSleep, object is
// the requested ms.
void BeginWait(uint32_t kind, uint32_t object, const char* what, uint32_t count);
void EndWait();
// Called for every presented frame (VdSwap), for the stall detector.
void NoteSwap();

}  // namespace rex::kernel::xboxkrnl::load_profile
