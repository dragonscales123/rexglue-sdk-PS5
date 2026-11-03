/**
 * PS5 sampling profiler (REX_PROFILE=1 in ps5-env.txt).
 *
 * Every thread started through rex::thread registers itself here. A sampler
 * thread interrupts each registered thread in turn with SIGPROF; the handler
 * records the interrupted instruction pointer. Every REX_PROFILE_INTERVAL
 * seconds (default 10) the hottest addresses of each busy thread are appended
 * to /app0/profile.txt as ELF offsets (rip - 0x400000), for symbolizing
 * against the unstripped link output (llvm-pie.elf).
 *
 * Samples outside the title's code (libc, kernel waits) are counted as
 * "outside", which for most threads means blocked.
 */

#include "ps5_profiler.h"

#if defined(__PROSPERO__)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

extern "C" char __eh_frame_hdr_start[] __attribute__((weak));

namespace rex::ps5prof {
namespace {

constexpr size_t kMaxThreads = 512;
constexpr uint64_t kLoadBase = 0x400000;

struct Slot {
  bool used = false;
  pthread_t thread{};
  char name[32] = {};
  uint64_t total = 0;
  uint64_t outside = 0;
  std::unordered_map<uint64_t, uint32_t> hits;
  std::unordered_map<uint64_t, uint32_t> outside_hits;  // raw rips (libc, kernel)
  std::unordered_map<uint64_t, uint32_t> outside_callers;  // first title address on the stack
};

std::mutex g_lock;
Slot* g_slots = nullptr;  // kMaxThreads, allocated on first use
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_started{false};

// Handshake with the signal handler (one sample in flight at a time).
// The handler tags its sample with the request number, so a late delivery
// (a thread that had SIGPROF blocked) is never counted for the wrong thread.
std::atomic<uint64_t> g_request_seq{0};
std::atomic<uint64_t> g_sample_rip{0};
std::atomic<uint64_t> g_sample_seq{0};
std::atomic<uint64_t> g_sample_caller{0};
uint64_t g_text_end = UINT64_MAX;  // set before the handler is installed

uint64_t TextEnd() {
  return __eh_frame_hdr_start ? reinterpret_cast<uint64_t>(__eh_frame_hdr_start)
                              : UINT64_MAX;
}

void SigprofHandler(int, siginfo_t*, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
  const uint64_t seq = g_request_seq.load(std::memory_order_acquire);
  const uint64_t rip = uint64_t(uc->uc_mcontext.mc_rip);
  uint64_t caller = 0;
  if (rip < kLoadBase || rip >= g_text_end) {
    // In a library or a system call: the nearest title return address on the
    // stack names who made the call (heuristic; 64 words above rsp).
    const uint64_t* sp = reinterpret_cast<const uint64_t*>(uc->uc_mcontext.mc_rsp);
    for (int i = 0; i < 64; ++i) {
      const uint64_t v = sp[i];
      if (v >= kLoadBase && v < g_text_end) {
        caller = v;
        break;
      }
    }
  }
  g_sample_caller.store(caller, std::memory_order_relaxed);
  g_sample_rip.store(rip, std::memory_order_relaxed);
  g_sample_seq.store(seq, std::memory_order_release);
}

uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void SleepNs(uint64_t ns) {
  timespec ts{time_t(ns / 1000000000ull), long(ns % 1000000000ull)};
  nanosleep(&ts, nullptr);
}

void Dump(int fd, double seconds) {
  const uint64_t text_end = TextEnd();
  std::vector<std::pair<uint64_t, size_t>> busy;  // (in-title samples, slot)
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    if (s.total) busy.emplace_back(s.total - s.outside, i);
  }
  std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

  time_t wall = time(nullptr);
  struct tm tmv;
  localtime_r(&wall, &tmv);
  char header[128];
  int n = snprintf(header, sizeof(header), "=== %02d:%02d:%02d interval %.1fs\n", tmv.tm_hour,
                   tmv.tm_min, tmv.tm_sec, seconds);
  write(fd, header, size_t(n));

  size_t shown = 0;
  for (auto& [in_title, index] : busy) {
    Slot& s = g_slots[index];
    if (shown < 12 && in_title) {
      ++shown;
      std::vector<std::pair<uint32_t, uint64_t>> top;
      top.reserve(s.hits.size());
      for (auto& [rip, count] : s.hits) top.emplace_back(count, rip);
      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
      std::string out;
      char line[160];
      snprintf(line, sizeof(line), "thread \"%s\" samples=%llu in-title=%llu outside=%llu\n",
               s.name, (unsigned long long)s.total, (unsigned long long)in_title,
               (unsigned long long)s.outside);
      out += line;
      size_t limit = std::min<size_t>(top.size(), 300);
      for (size_t k = 0; k < limit; ++k) {
        if (top[k].first < 2 && k >= 40) break;
        snprintf(line, sizeof(line), "  %u 0x%llx\n", top[k].first,
                 (unsigned long long)(top[k].second - kLoadBase));
        out += line;
      }
      // Outside the title: raw addresses (module code such as libc, or a
      // kernel wait), so busy library time can be told from blocking.
      std::vector<std::pair<uint32_t, uint64_t>> outside;
      for (auto& [rip, count] : s.outside_hits) outside.emplace_back(count, rip);
      std::sort(outside.begin(), outside.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < std::min<size_t>(outside.size(), 25); ++k) {
        snprintf(line, sizeof(line), "  o %u 0x%llx\n", outside[k].first,
                 (unsigned long long)outside[k].second);
        out += line;
      }
      std::vector<std::pair<uint32_t, uint64_t>> callers;
      for (auto& [caller, count] : s.outside_callers) callers.emplace_back(count, caller);
      std::sort(callers.begin(), callers.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < std::min<size_t>(callers.size(), 40); ++k) {
        snprintf(line, sizeof(line), "  c %u 0x%llx\n", callers[k].first,
                 (unsigned long long)(callers[k].second ? callers[k].second - kLoadBase : 0));
        out += line;
      }
      write(fd, out.data(), out.size());
    }
    s.total = 0;
    s.outside = 0;
    s.hits.clear();
    s.outside_hits.clear();
    s.outside_callers.clear();
  }
  (void)text_end;
}

void SamplerMain() {
  const char* hz_env = getenv("REX_PROFILE_HZ");
  const char* interval_env = getenv("REX_PROFILE_INTERVAL");
  uint64_t hz = hz_env ? strtoull(hz_env, nullptr, 10) : 250;
  if (!hz) hz = 250;
  double interval_s = interval_env ? atof(interval_env) : 10.0;
  if (interval_s < 1.0) interval_s = 1.0;
  const uint64_t round_ns = 1000000000ull / hz;
  const uint64_t text_end = TextEnd();

  int fd = open("/app0/profile.txt", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
  if (fd < 0) return;
  char hello[160];
  int n = snprintf(hello, sizeof(hello),
                   "profile: %llu Hz per thread, %.1fs intervals, offsets are rip-0x400000, "
                   "text end 0x%llx\n",
                   (unsigned long long)hz, interval_s,
                   (unsigned long long)(text_end == UINT64_MAX ? 0 : text_end - kLoadBase));
  write(fd, hello, size_t(n));

  const pthread_t self = pthread_self();
  uint64_t interval_start = NowNs();
  while (true) {
    const uint64_t round_start = NowNs();
    {
      std::lock_guard<std::mutex> lock(g_lock);
      for (size_t i = 0; i < kMaxThreads; ++i) {
        Slot& s = g_slots[i];
        if (!s.used || pthread_equal(s.thread, self)) continue;
        const uint64_t seq = g_request_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (pthread_kill(s.thread, SIGPROF) != 0) continue;
        // The thread may be off-core; give it up to 2 ms to take the signal.
        const uint64_t wait_start = NowNs();
        bool got = false;
        while (!(got = g_sample_seq.load(std::memory_order_acquire) == seq)) {
          if (NowNs() - wait_start > 2000000ull) break;
          sched_yield();
        }
        if (!got) continue;
        const uint64_t rip = g_sample_rip.load(std::memory_order_relaxed);
        ++s.total;
        if (rip < kLoadBase || rip >= text_end) {
          ++s.outside;
          ++s.outside_hits[rip];
          ++s.outside_callers[g_sample_caller.load(std::memory_order_relaxed)];
        } else {
          ++s.hits[rip];
        }
      }
      const uint64_t now = NowNs();
      if (now - interval_start >= uint64_t(interval_s * 1e9)) {
        Dump(fd, double(now - interval_start) / 1e9);
        interval_start = now;
      }
    }
    const uint64_t spent = NowNs() - round_start;
    if (spent < round_ns) SleepNs(round_ns - spent);
  }
}

void* SamplerThread(void*) {
  pthread_set_name_np(pthread_self(), "rex profiler");
  SamplerMain();
  return nullptr;
}

void StartIfEnabled() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  const char* env = getenv("REX_PROFILE");
  if (!env || !*env || *env == '0') return;
  g_slots = new Slot[kMaxThreads];
  g_text_end = TextEnd();
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = SigprofHandler;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGPROF, &action, nullptr) != 0) return;
  g_enabled.store(true, std::memory_order_release);
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  pthread_create(&thread, &attr, SamplerThread, nullptr);
  pthread_attr_destroy(&attr);
}

Slot* FindLocked(pthread_t thread) {
  for (size_t i = 0; i < kMaxThreads; ++i) {
    if (g_slots[i].used && pthread_equal(g_slots[i].thread, thread)) return &g_slots[i];
  }
  return nullptr;
}

}  // namespace

void RegisterCurrentThread(const char* name) {
  StartIfEnabled();
  if (!g_enabled.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(g_lock);
  pthread_t self = pthread_self();
  Slot* slot = FindLocked(self);
  if (!slot) {
    for (size_t i = 0; i < kMaxThreads; ++i) {
      if (!g_slots[i].used) {
        slot = &g_slots[i];
        slot->used = true;
        slot->thread = self;
        slot->name[0] = '\0';
        break;
      }
    }
  }
  if (slot && name && *name) {
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
  }
}

void SetThreadName(pthread_t thread, const char* name) {
  if (!g_enabled.load(std::memory_order_acquire) || !name) return;
  std::lock_guard<std::mutex> lock(g_lock);
  if (Slot* slot = FindLocked(thread)) {
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
  }
}

void UnregisterCurrentThread() {
  if (!g_enabled.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(g_lock);
  if (Slot* slot = FindLocked(pthread_self())) {
    slot->used = false;
    slot->total = slot->outside = 0;
    slot->hits.clear();
    slot->outside_hits.clear();
    slot->outside_callers.clear();
  }
}

}  // namespace rex::ps5prof

#endif  // __PROSPERO__
