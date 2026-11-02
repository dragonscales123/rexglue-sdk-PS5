// Copyright 2026 Fable II Recomp contributors. BSD license; see LICENSE.
#pragma once
#include <chrono>
#include <cstdint>

namespace rex::graphics {
// Counts guest XE_SWAP intervals, not host UI draws or game-loop iterations.
// Owned and called by one command processor thread. Half-second averaging
// keeps the overlay readable and includes limiter and rendering wait time.
class GuestFrameMeter {
 public:
  using Clock = std::chrono::steady_clock;
  int64_t Record(Clock::time_point now) {
    if (!started_) {
      started_ = true;
      start_ = now;
      return 0;
    }
    ++intervals_;
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count();
    if (elapsed >= 500000) {
      mean_us_ = elapsed / intervals_;
      intervals_ = 0;
      start_ = now;
    }
    return mean_us_;
  }
 private:
  bool started_ = false;
  Clock::time_point start_{};
  int64_t intervals_ = 0;
  int64_t mean_us_ = 0;
};
}  // namespace rex::graphics
