#pragma once

// PS5 sampling profiler, enabled with REX_PROFILE=1 (see ps5_profiler.cpp).

#if defined(__PROSPERO__)
#include <atomic>
#include <cstdint>

#include <pthread.h>

namespace rex::ps5prof {
// Event counters, written to profile.txt (as per-second rates) each interval.
struct Counters {
  std::atomic<uint64_t> mprotect_calls{0};
  std::atomic<uint64_t> mprotect_bytes{0};
  std::atomic<uint64_t> invalidations{0};    // shared-memory invalidations (mostly write faults)
  std::atomic<uint64_t> invalidated_bytes_fault{0};  // widened ranges from write faults
  std::atomic<uint64_t> invalidated_bytes_exact{0};  // exact ranges (heap protect/free, etc.)
  std::atomic<uint64_t> gpu_written_bytes{0};        // resolves/memexport marking pages GPU-written
  std::atomic<uint64_t> upload_calls{0};     // shared-memory upload batches
  std::atomic<uint64_t> upload_bytes{0};
  std::atomic<uint64_t> hot_page_bytes{0};   // uploaded bytes left unwatched (hot pages)
};
Counters& counters();

void RegisterCurrentThread(const char* name);
void SetThreadName(pthread_t thread, const char* name);
void UnregisterCurrentThread();
}  // namespace rex::ps5prof
#endif
