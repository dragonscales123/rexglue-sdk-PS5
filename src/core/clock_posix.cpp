/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_POSIX, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#if REX_PLATFORM_PS5
#include <x86intrin.h>

#include <ps5platform/kernel.h>
#endif

// FreeBSD (the PS5) has no CLOCK_MONOTONIC_RAW; its CLOCK_MONOTONIC is not
// slewed by NTP either.
#if defined(__PROSPERO__) && !defined(CLOCK_MONOTONIC_RAW)
#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC
#endif

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() {
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#else
  // host_tick_count_platform() counts nanoseconds whatever the clock's
  // resolution, so the tick rate is 1 GHz. (Deriving it from clock_getres was
  // only right where the resolution is 1 ns: FreeBSD, and so the PS5, report
  // a coarser one, and guest time ran that many times too fast.)
  return 1000000000ull;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#ifdef __APPLE__
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(CLOCK_MONOTONIC_RAW) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#elif REX_PLATFORM_PS5
  // clock_gettime is a system call on the PS5 (about a microsecond). Guest code
  // reads the clock constantly (the game thread spent ~19% of its time in it),
  // so read the invariant TSC instead, converted to nanoseconds on the same
  // epoch as CLOCK_MONOTONIC.
  struct TscClock {
    uint64_t base_tsc;
    uint64_t base_ns;
    uint64_t ns_per_tsc_q32;  // nanoseconds per TSC tick, 32.32 fixed point
    TscClock() {
      timespec tp;
      clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
      base_tsc = __rdtsc();
      base_ns = tp.tv_nsec + tp.tv_sec * 1000000000ull;
      uint64_t frequency = sceKernelGetTscFrequency();
      if (!frequency) {
        frequency = 1000000000ull;
      }
      ns_per_tsc_q32 = uint64_t((unsigned __int128)(1000000000ull) << 32) / frequency;
    }
  };
  static const TscClock tsc_clock;
  const uint64_t delta = __rdtsc() - tsc_clock.base_tsc;
  return tsc_clock.base_ns +
         uint64_t(((unsigned __int128)delta * tsc_clock.ns_per_tsc_q32) >> 32);
#else
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
