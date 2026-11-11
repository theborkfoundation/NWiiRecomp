#include "runtime/diagnostics.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nwii::runtime::diag {
namespace {

constexpr int kMaxCounters = 96;

struct Entry {
    char name[48];
    std::atomic<uint64_t> count{0};
    std::atomic<int> printed{0};
    Kind kind = Kind::Error;
    bool used = false;
};

Entry g_entries[kMaxCounters];
int g_used = 0;
std::mutex g_mtx;

std::atomic<int> g_level{(int)Level::Warn};
std::atomic<bool> g_reported{false};
std::mutex g_report_mtx;

const char *kind_name(Kind k) {
    switch (k) {
    case Kind::Info: return "info";
    case Kind::Warn: return "warn";
    default: return "ERROR";
    }
}

} // namespace

void init() {
    static std::once_flag once;
    std::call_once(once, [] {
        const char *e = std::getenv("NWII_DIAG");
        if (!e || !e[0]) return; // default: warn
        if (!std::strcmp(e, "off")) g_level = (int)Level::Off;
        else if (!std::strcmp(e, "error")) g_level = (int)Level::Error;
        else if (!std::strcmp(e, "warn")) g_level = (int)Level::Warn;
        else if (!std::strcmp(e, "info")) g_level = (int)Level::Info;
        else if (!std::strcmp(e, "trace")) g_level = (int)Level::Trace;
    });
}

Level level() {
    init();
    return (Level)g_level.load(std::memory_order_relaxed);
}

bool at_least(Level l) { return (int)level() >= (int)l; }

int slot(const char *name, Kind kind) {
    std::lock_guard<std::mutex> lk(g_mtx);
    for (int i = 0; i < g_used; ++i)
        if (g_entries[i].used && !std::strcmp(g_entries[i].name, name))
            return i;
    if (g_used >= kMaxCounters) return -1;
    int i = g_used++;
    Entry &e = g_entries[i];
    std::snprintf(e.name, sizeof(e.name), "%s", name);
    e.kind = kind;
    e.used = true;
    return i;
}

void bump(int s, uint64_t n) {
    if (s < 0 || s >= kMaxCounters) return;
    g_entries[s].count.fetch_add(n, std::memory_order_relaxed);
}

void fail(int s, const std::string &detail, int max) {
    if (s < 0 || s >= kMaxCounters) return;
    Entry &e = g_entries[s];
    e.count.fetch_add(1, std::memory_order_relaxed);
    if (level() < Level::Error) return;
    int n = e.printed.fetch_add(1, std::memory_order_relaxed);
    if (n < max)
        std::printf("[DIAG] %s x%d: %s\n", e.name, n + 1, detail.c_str());
}

uint64_t count(int s) {
    if (s < 0 || s >= kMaxCounters) return 0;
    return g_entries[s].count.load(std::memory_order_relaxed);
}

const Counter *counters(size_t &n) {
    // Static so the snapshot is stable while the caller walks it. Names live in
    // the entries, so this is only the (name, kind, count) view.
    static Counter view[kMaxCounters];
    std::lock_guard<std::mutex> lk(g_mtx);
    n = 0;
    for (int i = 0; i < g_used; ++i) {
        if (!g_entries[i].used) continue;
        view[n].name = g_entries[i].name;
        view[n].kind = g_entries[i].kind;
        view[n].count = g_entries[i].count.load(std::memory_order_relaxed);
        ++n;
    }
    return view;
}

uint64_t error_total() {
    uint64_t t = 0;
    size_t n = 0;
    const Counter *c = counters(n);
    for (size_t i = 0; i < n; ++i)
        if (c[i].kind == Kind::Error) t += c[i].count;
    return t;
}

uint64_t warn_total() {
    uint64_t t = 0;
    size_t n = 0;
    const Counter *c = counters(n);
    for (size_t i = 0; i < n; ++i)
        if (c[i].kind == Kind::Warn) t += c[i].count;
    return t;
}

void begin_period() { g_reported.store(false, std::memory_order_relaxed); }

void report(const char *tag, bool force) {
    size_t n = 0;
    const Counter *c = counters(n);
    uint64_t errors = 0, warns = 0;
    for (size_t i = 0; i < n; ++i) {
        if (c[i].count == 0) continue;
        if (c[i].kind == Kind::Error) errors += c[i].count;
        else if (c[i].kind == Kind::Warn) warns += c[i].count;
    }
    if (!errors && !warns) return;
    if (!force && g_reported.exchange(true)) return;

    std::lock_guard<std::mutex> lk(g_report_mtx);
    std::printf("[DIAG] %s:", tag);
    for (size_t i = 0; i < n; ++i)
        if (c[i].count) std::printf(" %s=%llu", c[i].name,
                                    (unsigned long long)c[i].count);
    std::printf("\n");
    if (errors) {
        std::printf("[DIAG] *** %llu error-class events across:",
                    (unsigned long long)errors);
        for (size_t i = 0; i < n; ++i)
            if (c[i].count && c[i].kind == Kind::Error)
                std::printf(" %s", c[i].name);
        std::printf("\n");
    }
}

} // namespace nwii::runtime::diag
