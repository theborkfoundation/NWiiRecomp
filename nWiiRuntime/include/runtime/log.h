#pragma once

// Minimal log-level gate. Default is WARN so normal play is quiet; set
// NWII_LOG_LEVEL=error|warn|info|debug|trace (or a numeric level) to open it up.
//
// Levels: 0 error, 1 warn, 2 info, 3 debug, 4 trace.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

namespace nwii::runtime {

enum LogLevel {
  LOG_ERROR = 0,
  LOG_WARN = 1,
  LOG_INFO = 2,
  LOG_DEBUG = 3,
  LOG_TRACE = 4,
};

inline int &log_level_storage() {
  static int level = []() -> int {
    const char *e = std::getenv("NWII_LOG_LEVEL");
    if (!e || !e[0]) return (int)LOG_WARN;
    if (!strcasecmp(e, "error")) return (int)LOG_ERROR;
    if (!strcasecmp(e, "warn") || !strcasecmp(e, "warning"))
      return (int)LOG_WARN;
    if (!strcasecmp(e, "info")) return (int)LOG_INFO;
    if (!strcasecmp(e, "debug")) return (int)LOG_DEBUG;
    if (!strcasecmp(e, "trace")) return (int)LOG_TRACE;
    return std::atoi(e);
  }();
  return level;
}

inline bool log_on(int level) { return level <= log_level_storage(); }

inline const char *log_level_name() {
  switch (log_level_storage()) {
  case LOG_ERROR: return "error";
  case LOG_WARN: return "warn";
  case LOG_INFO: return "info";
  case LOG_DEBUG: return "debug";
  case LOG_TRACE: return "trace";
  default: return "custom";
  }
}

inline void log_banner() {
  std::printf("[Log] level=%s (NWII_LOG_LEVEL=info|debug|trace for more)\n",
              log_level_name());
  std::fflush(stdout);
}

inline void log_printf(int level, const char *fmt, ...) {
  if (!log_on(level)) return;
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

} // namespace nwii::runtime
