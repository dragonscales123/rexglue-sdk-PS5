// Copyright 2026 Fable II Recomp contributors.
// Released under the BSD license; see LICENSE in the SDK root.
#pragma once

#include <chrono>

namespace rex::graphics {

// Worker-thread-only host swap limiter. It neither changes the guest clock nor
// advertises a different video mode. Late frames reset the schedule, not burst.
class FrameLimiter {
 public:
  FrameLimiter() = default;
  ~FrameLimiter();
  FrameLimiter(const FrameLimiter&) = delete;
  FrameLimiter& operator=(const FrameLimiter&) = delete;
  void Pace(int frames_per_second) {
    if (frames_per_second <= 0 || frames_per_second > 240) {
      rate_ = 0;
      return;
    }
    auto now = Clock::now();
    if (rate_ == frames_per_second) {
      auto period = std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(1.0 / frames_per_second));
      WaitUntil(previous_ + period);
      now = Clock::now();
    }
    previous_ = now;
    rate_ = frames_per_second;
  }

 private:
  using Clock = std::chrono::steady_clock;
  void WaitUntil(Clock::time_point deadline);
  void* timer_ = nullptr;
  bool timer_attempted_ = false;
  Clock::time_point previous_{};
  int rate_ = 0;
};

}  // namespace rex::graphics
