// Copyright 2026 Fable II Recomp contributors.
// Released under the BSD license; see LICENSE in the SDK root.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <system_error>

namespace rex::audio {

// Silent consumer, retaining the normal 256-sample / 48 kHz producer clock.
// No SDL subsystem state or guest memory is touched by this worker.
class ClockedAudioSink {
 public:
  explicit ClockedAudioSink(std::function<void()> consumed)
      : consumed_(std::move(consumed)) {}
  ~ClockedAudioSink() { Stop(); }
  ClockedAudioSink(const ClockedAudioSink&) = delete;
  ClockedAudioSink& operator=(const ClockedAudioSink&) = delete;

  bool Start() {
    std::lock_guard lock(mutex_);
    if (running_) return false;
    running_ = true;
    try {
      worker_ = std::thread([this] { Run(); });
    } catch (const std::system_error&) {
      running_ = false;
      return false;
    }
    return true;
  }

  bool Submit() {
    std::lock_guard lock(mutex_);
    if (!running_ || queued_ == 64) return false;
    ++queued_;
    wake_.notify_one();
    return true;
  }

  // Caller must serialize Start/Stop; concurrent Submit is safe.
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      running_ = false;
      wake_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    std::lock_guard lock(mutex_);
    queued_ = 0;
  }

 private:
  void Run() {
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(256.0 / 48000.0));
    std::unique_lock lock(mutex_);
    auto deadline = Clock::now() + period;
    while (running_) {
      if (!queued_) {
        wake_.wait(lock, [this] { return !running_ || queued_ != 0; });
        deadline = Clock::now() + period;
      }
      if (!running_) break;
      if (wake_.wait_until(lock, deadline, [this] { return !running_; })) break;
      --queued_;
      lock.unlock();
      consumed_();
      lock.lock();
      deadline += period;
      // Do not burst through old deadlines after suspension or a slow callback.
      if (deadline < Clock::now()) deadline = Clock::now() + period;
    }
  }

  std::function<void()> consumed_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool running_ = false;
  unsigned queued_ = 0;
};

}  // namespace rex::audio
