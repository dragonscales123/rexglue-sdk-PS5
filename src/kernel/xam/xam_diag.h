// xam_diag.h - lightweight XAM call tracing (header-only, no SDK deps).
//
// Logs XamUser*/XamInput*/XamEnumerate entry calls with arguments and
// return values to fable2_xam_trace.log (CWD = exe dir) so guest-side
// user-index behavior can be diffed against real-console semantics.
//
// Enable: FABLE2_XAM_TRACE=1 (default is OFF; leading "1" enables,
// anything else keeps it off). The file is small: XamInputGetState lines
// are logged only when the polled controller state CHANGES (plus one
// "connected" line per user slot), everything else is logged per call.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex::kernel::xam::diag {

inline bool enabled() {
  static const bool e = [] {
    char v[8] = {};
    size_t n = 0;
#if REX_PLATFORM_WIN32
    return ::getenv_s(&n, v, sizeof(v), "FABLE2_XAM_TRACE") == 0 && v[0] == '1';
#else
    const char* s = std::getenv("FABLE2_XAM_TRACE");
    return s != nullptr && s[0] == '1';
#endif
  }();
  return e;
}

inline uint64_t now_ms() {
#if REX_PLATFORM_WIN32
  return static_cast<uint64_t>(::GetTickCount64());
#else
  return 0;
#endif
}

inline void write_line(const char* text, size_t len) {
  static FILE* f = nullptr;
  static char buf[64 * 1024];
  static size_t used = 0;
  if (used + len + 2 >= sizeof(buf)) {
    if (f) std::fwrite(buf, 1, used, f);
    used = 0;
  }
  if (used + len + 2 < sizeof(buf)) {
    memcpy(buf + used, text, len);
    buf[used + len] = '\n';
    used += len + 1;
  }
  if (!f) {
#if REX_PLATFORM_WIN32
    ::fopen_s(&f, "fable2_xam_trace.log", "a");
#else
    f = std::fopen("fable2_xam_trace.log", "a");
#endif
  }
}

// Formats and enqueues one line: "<ms> <tid> <msg>".
inline void trace(const char* msg) {
  if (!enabled()) return;
  char line[256];
#if REX_PLATFORM_WIN32
  int n = std::snprintf(line, sizeof(line), "%llu %lu %s",
                        static_cast<unsigned long long>(now_ms()),
                        static_cast<unsigned long>(::GetCurrentThreadId()), msg);
#else
  int n = std::snprintf(line, sizeof(line), "%llu %s",
                        static_cast<unsigned long long>(now_ms()), msg);
#endif
  if (n > 0) write_line(line, static_cast<size_t>(n));
}

// Tracks the last logged button state per user slot; returns true when the
// state changed from what was last logged (and marks it as logged).
inline bool input_state_changed(uint32_t user_index, uint16_t buttons,
                                uint8_t lt, uint8_t rt) {
  static uint32_t last[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
  const uint32_t cur = (uint32_t(buttons) << 16) | (uint32_t(lt) << 8) | uint32_t(rt);
  if (user_index >= 4) return true;
  const bool changed = (last[user_index] != cur);
  last[user_index] = cur;
  return changed;
}

}  // namespace rex::kernel::xam::diag
