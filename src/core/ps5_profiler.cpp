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
#include <array>
#include <map>
#include <atomic>
#include <cerrno>
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
#include <sched.h>
#include <time.h>
#include <unistd.h>

extern "C" char __eh_frame_hdr_start[] __attribute__((weak));

namespace rex::ps5prof {

Counters& counters() {
  static Counters c;
  return c;
}

namespace {

constexpr size_t kMaxThreads = 512;
constexpr uint64_t kLoadBase = 0x400000;

struct Slot {
  bool used = false;
  pthread_t thread{};
  uint64_t stack_hi = 0;  // one past the highest stack byte, 0 if unknown
  char name[32] = {};
  uint64_t total = 0;
  uint64_t outside = 0;
  std::unordered_map<uint64_t, uint32_t> hits;
  std::unordered_map<uint64_t, uint32_t> outside_hits;  // raw rips (libc, kernel)
  std::unordered_map<uint64_t, uint32_t> outside_callers;  // first title address on the stack
  std::map<std::array<uint64_t, 6>, uint32_t> outside_chains;  // first 6 title addresses
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
std::atomic<uint64_t> g_sample_cpu_ns{0};  // the signalled thread's own CPU time
uint64_t g_sample_chain[6];  // written by the handler before g_sample_seq is published
uint64_t g_text_end = UINT64_MAX;  // set before the handler is installed
std::atomic<uint64_t> g_target_stack_hi{0};  // stack top of the thread being sampled
pthread_t g_target_thread{};  // so a late signal never uses another thread's stack top

uint64_t TextEnd() {
  return __eh_frame_hdr_start ? reinterpret_cast<uint64_t>(__eh_frame_hdr_start)
                              : UINT64_MAX;
}

void SigprofHandler(int, siginfo_t*, void* context) {
  // The interrupted code may be about to read errno; clock_gettime can set it.
  const int saved_errno = errno;
  auto* uc = static_cast<ucontext_t*>(context);
  const uint64_t seq = g_request_seq.load(std::memory_order_acquire);
  const uint64_t rip = uint64_t(uc->uc_mcontext.mc_rip);
  uint64_t caller = 0;
  for (auto& v : g_sample_chain) v = 0;
  if (rip < kLoadBase || rip >= g_text_end) {
    // In a library or a system call: the title return addresses on the stack
    // name who made the call (heuristic, no frame pointers; 160 words above rsp).
    const uint64_t* sp = reinterpret_cast<const uint64_t*>(uc->uc_mcontext.mc_rsp);
    // Never read past the thread's stack top; without one, stay in rsp's page.
    uint64_t limit = g_target_stack_hi.load(std::memory_order_acquire);
    if (!pthread_equal(pthread_self(), g_target_thread)) limit = 0;
    const uint64_t rsp = uint64_t(uc->uc_mcontext.mc_rsp);
    if (limit <= rsp) limit = (rsp | 0x3FFFull) + 1;
    const int words = int(std::min<uint64_t>(160, (limit - rsp) / 8));
    int found = 0;
    for (int i = 0; i < words && found < 6; ++i) {
      const uint64_t v = sp[i];
      if (v >= kLoadBase && v < g_text_end) {
        if (!found) caller = v;
        g_sample_chain[found++] = v;
      }
    }
  }
  g_sample_caller.store(caller, std::memory_order_relaxed);
  g_sample_rip.store(rip, std::memory_order_relaxed);
  timespec cpu_ts;
  g_sample_cpu_ns.store(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_ts) == 0
                            ? uint64_t(cpu_ts.tv_sec) * 1000000000ull + uint64_t(cpu_ts.tv_nsec)
                            : 0,
                        std::memory_order_relaxed);
  g_sample_seq.store(seq, std::memory_order_release);
  errno = saved_errno;
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
  char header[256];
  int n = snprintf(header, sizeof(header), "=== %02d:%02d:%02d interval %.1fs\n", tmv.tm_hour,
                   tmv.tm_min, tmv.tm_sec, seconds);
  write(fd, header, size_t(n));
  {
    // The counters are cumulative (the performance overlay reads them too).
    struct Snapshot {
      uint64_t v[9];
    };
    static Snapshot previous{};
    Counters& counters_now = counters();
    Snapshot now{{counters_now.mprotect_calls.load(), counters_now.mprotect_bytes.load(),
                  counters_now.invalidations.load(), counters_now.invalidated_bytes_fault.load(),
                  counters_now.invalidated_bytes_exact.load(), counters_now.gpu_written_bytes.load(),
                  counters_now.upload_calls.load(), counters_now.upload_bytes.load(),
                  counters_now.hot_page_bytes.load()}};
    struct Delta {
      std::atomic<uint64_t> mprotect_calls, mprotect_bytes, invalidations, invalidated_bytes_fault,
          invalidated_bytes_exact, gpu_written_bytes, upload_calls, upload_bytes, hot_page_bytes;
    } c;
    std::atomic<uint64_t>* fields[9] = {&c.mprotect_calls,         &c.mprotect_bytes,
                                        &c.invalidations,          &c.invalidated_bytes_fault,
                                        &c.invalidated_bytes_exact, &c.gpu_written_bytes,
                                        &c.upload_calls,           &c.upload_bytes,
                                        &c.hot_page_bytes};
    for (int k = 0; k < 9; ++k) fields[k]->store(now.v[k] - previous.v[k]);
    previous = now;
    const double inv = seconds > 0 ? 1.0 / seconds : 0.0;
    n = snprintf(header, sizeof(header),
                 "counters/s: mprotect %.0f (%.1f MB) invalidations %.0f (fault %.1f MB, "
                 "exact %.1f MB, gpu-written %.1f MB) uploads %.0f (%.1f MB, hot %.1f MB)\n",
                 double(c.mprotect_calls.exchange(0)) * inv,
                 double(c.mprotect_bytes.exchange(0)) * inv / 1048576.0,
                 double(c.invalidations.exchange(0)) * inv,
                 double(c.invalidated_bytes_fault.exchange(0)) * inv / 1048576.0,
                 double(c.invalidated_bytes_exact.exchange(0)) * inv / 1048576.0,
                 double(c.gpu_written_bytes.exchange(0)) * inv / 1048576.0,
                 double(c.upload_calls.exchange(0)) * inv,
                 double(c.upload_bytes.exchange(0)) * inv / 1048576.0,
                 double(c.hot_page_bytes.exchange(0)) * inv / 1048576.0);
    write(fd, header, size_t(n));
    // Hot-page check volume (cumulative counters only read here).
    static uint64_t previous_hot[9] = {};
    const uint64_t hot_now[9] = {counters_now.hot_compared_bytes.load(),
                                 counters_now.hot_skipped_bytes.load(),
                                 counters_now.hot_check_epochs.load(),
                                 counters_now.sync_points[0].load(),
                                 counters_now.sync_points[1].load(),
                                 counters_now.sync_points[2].load(),
                                 counters_now.sync_points[3].load(),
                                 counters_now.sync_points[4].load(),
                                 counters_now.watch_fires.load()};
    double d[9];
    for (int k = 0; k < 9; ++k) {
      d[k] = double(hot_now[k] - previous_hot[k]) * inv;
      previous_hot[k] = hot_now[k];
    }
    n = snprintf(header, sizeof(header),
                 "hot-check/s: compared %.1f MB, skipped %.1f MB, epochs %.0f | sync points: "
                 "batch %.0f, wait %.0f, interrupt %.0f, swap %.0f, gpu-signal %.0f | "
                 "watches fired %.0f\n",
                 d[0] / 1048576.0, d[1] / 1048576.0, d[2], d[3], d[4], d[5], d[6], d[7], d[8]);
    write(fd, header, size_t(n));
  }

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
      char line[256];
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
      std::vector<std::pair<uint32_t, const std::array<uint64_t, 6>*>> chains;
      for (auto& [chain, count] : s.outside_chains) chains.emplace_back(count, &chain);
      std::sort(chains.begin(), chains.end(),
                [](const auto& a, const auto& b) { return a.first > b.first; });
      for (size_t k = 0; k < std::min<size_t>(chains.size(), 30); ++k) {
        int m = snprintf(line, sizeof(line), "  k %u", chains[k].first);
        for (uint64_t v : *chains[k].second) {
          m += snprintf(line + m, sizeof(line) - size_t(m), " 0x%llx",
                        (unsigned long long)(v ? v - kLoadBase : 0));
        }
        snprintf(line + m, sizeof(line) - size_t(m), "\n");
        out += line;
      }
      write(fd, out.data(), out.size());
    }
    s.total = 0;
    s.outside = 0;
    s.hits.clear();
    s.outside_hits.clear();
    s.outside_callers.clear();
    s.outside_chains.clear();
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
        g_target_thread = s.thread;
        g_target_stack_hi.store(s.stack_hi, std::memory_order_release);
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
          std::array<uint64_t, 6> chain;
          for (int k = 0; k < 6; ++k) chain[k] = g_sample_chain[k];
          ++s.outside_chains[chain];
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

std::atomic<bool> g_registry_ready{false};

void StartIfEnabled() {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) {
    // Another thread is setting up; registration needs the slots.
    while (!g_registry_ready.load(std::memory_order_acquire)) sched_yield();
    return;
  }
  g_slots = new Slot[kMaxThreads];
  g_text_end = TextEnd();
  // SIGPROF serves both the sampler and per-thread CPU time queries.
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = SigprofHandler;
  sigemptyset(&action.sa_mask);
  const bool handler_installed = sigaction(SIGPROF, &action, nullptr) == 0;
  g_registry_ready.store(true, std::memory_order_release);
  const char* env = getenv("REX_PROFILE");
  if (!env || !*env || *env == '0' || !handler_installed) return;
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
  if (!g_registry_ready.load(std::memory_order_acquire)) return;
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
        slot->stack_hi = 0;
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) == 0) {
          void* stack_addr = nullptr;
          size_t stack_size = 0;
          if (pthread_attr_get_np(self, &attr) == 0 &&
              pthread_attr_getstack(&attr, &stack_addr, &stack_size) == 0 && stack_addr) {
            slot->stack_hi = reinterpret_cast<uint64_t>(stack_addr) + stack_size;
          }
          pthread_attr_destroy(&attr);
        }
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
  if (!g_registry_ready.load(std::memory_order_acquire) || !name) return;
  std::lock_guard<std::mutex> lock(g_lock);
  if (Slot* slot = FindLocked(thread)) {
    strncpy(slot->name, name, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
  }
}

void UnregisterCurrentThread() {
  if (!g_registry_ready.load(std::memory_order_acquire)) return;
  std::lock_guard<std::mutex> lock(g_lock);
  if (Slot* slot = FindLocked(pthread_self())) {
    slot->used = false;
    slot->total = slot->outside = 0;
    slot->hits.clear();
    slot->outside_hits.clear();
    slot->outside_callers.clear();
    slot->outside_chains.clear();
  }
}

bool ThreadCpuTimeNs(const char* name_prefix, uint64_t* cpu_ns) {
  if (!g_registry_ready.load(std::memory_order_acquire) || !name_prefix) return false;
  std::lock_guard<std::mutex> lock(g_lock);
  const size_t prefix_length = strlen(name_prefix);
  for (size_t i = 0; i < kMaxThreads; ++i) {
    Slot& s = g_slots[i];
    if (!s.used || strncmp(s.name, name_prefix, prefix_length) != 0) continue;
    if (pthread_equal(s.thread, pthread_self())) {
      timespec ts;
      if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return false;
      *cpu_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
      return true;
    }
    g_target_thread = s.thread;
    g_target_stack_hi.store(s.stack_hi, std::memory_order_release);
    const uint64_t seq = g_request_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (pthread_kill(s.thread, SIGPROF) != 0) return false;
    const uint64_t wait_start = NowNs();
    while (g_sample_seq.load(std::memory_order_acquire) != seq) {
      if (NowNs() - wait_start > 5000000ull) return false;
      sched_yield();
    }
    const uint64_t ns = g_sample_cpu_ns.load(std::memory_order_relaxed);
    if (!ns) return false;
    *cpu_ns = ns;
    return true;
  }
  return false;
}

}  // namespace rex::ps5prof

#endif  // __PROSPERO__
