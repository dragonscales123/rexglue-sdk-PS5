#pragma once

// PS5 sampling profiler, enabled with REX_PROFILE=1 (see ps5_profiler.cpp).

#if defined(__PROSPERO__)
#include <pthread.h>

namespace rex::ps5prof {
void RegisterCurrentThread(const char* name);
void SetThreadName(pthread_t thread, const char* name);
void UnregisterCurrentThread();
}  // namespace rex::ps5prof
#endif
