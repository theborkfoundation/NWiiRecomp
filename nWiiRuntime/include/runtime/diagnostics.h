#pragma once

// Runtime self-diagnostics.
//
// The purpose is to make silent failure loud. Every path that discards
// game-visible work -- a rejected display list, a draw that never reached the
// screen, a shader that failed to link, a FIFO command the parser could not
// place -- records a counter here. The counters are always on and cost one
// relaxed atomic increment when they fire, so a run with no instrumentation at
// all still tells you what it threw away.
//
// Two things are reported automatically:
//   * the periodic status line prints a [DIAG] summary whenever any counter is
//     non-zero (see core/main.cpp),
//   * the process prints the same summary at exit, and exits non-zero when
//     NWII_STRICT=1, so a scripted/CI run turns "it rendered something" into a
//     pass/fail.
//
// Detail lines (offsets, contexts) are opt-in via NWII_DIAG=<off|error|warn|
// info|trace> and are rate-limited per counter so a hot loop cannot flood.
//
// Counters are addressed by a `static const int` slot resolved once per call
// site, so instrumenting a per-draw or per-command path stays cheap.

#include <cstdint>
#include <string>

namespace nwii::runtime::diag {

enum class Level { Off = 0, Error = 1, Warn = 2, Info = 3, Trace = 4 };

// Info  : worth knowing, never a bug on its own.
// Warn  : suspicious; the run may still be correct.
// Error : the runtime discarded or corrupted work. Any non-zero error counter
//         means a code path is lying about what it did.
enum class Kind { Info = 0, Warn = 1, Error = 2 };

// Read NWII_DIAG once. Safe to call repeatedly.
void init();

Level level();
bool at_least(Level l);

// Resolve (and register on first use) a counter name. Stable for the process.
int slot(const char* name, Kind kind);

// Count `n` units of dropped/failed work. Lock-free.
void bump(int slot, uint64_t n = 1);

// Bump and print a detail line the first `max` times this slot fires.
void fail(int slot, const std::string& detail, int max = 4);

uint64_t count(int slot);

struct Counter {
    const char* name;
    Kind kind;
    uint64_t count;
};

// Fixed snapshot in registration order.
const Counter* counters(size_t& n);

uint64_t error_total();
uint64_t warn_total();

// Print the non-zero counters. `force` prints even when a summary was already
// emitted for this generation.
void report(const char* tag, bool force = false);

// Forget that a report was printed (called at the start of each status period).
void begin_period();

// Convenience for a slot resolved once per call site.
#define NWII_DIAG_SLOT(name, kind)                                            \
    ([]() {                                                                   \
        static const int s_slot = ::nwii::runtime::diag::slot(name, kind);     \
        return s_slot;                                                        \
    }())

} // namespace nwii::runtime::diag
