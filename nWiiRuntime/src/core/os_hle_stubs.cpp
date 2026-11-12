#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/hw/mmio_dispatcher.h"
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

using namespace nwii::runtime;

namespace nwii::runtime {
uint64_t get_os_time();
}

uint64_t nwii::runtime::get_os_time() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
      .count();
}

namespace nwii::runtime {
static const bool g_trace_print = []() {
  const char *env = std::getenv("NWII_TRACE_CALLS");
  return env && env[0] == '1';
}();

// Cheap always-on recorder of the most recently entered generated
// functions, dumped once when the interpreter lands on a bad PC so the
// caller chain that got there is still visible. No printing on the hot path.
static const bool g_trace_recent_enabled =
    (std::getenv("NWII_TRACE_RECENT") != nullptr);

bool g_trace_calls = (g_trace_print || g_trace_recent_enabled);

namespace {
struct RecentCall {
  uint32_t func;
  uint32_t lr;
  uint32_t r3;
  uint32_t r4;
  uint32_t r13;
};
constexpr size_t kRecentCallCount = 160;
RecentCall g_recent_calls[kRecentCallCount];
size_t g_recent_pos = 0;
size_t g_recent_len = 0;
bool g_recent_dumped = false;
} // namespace

void trace_recent_dump(const char* why) {
  if (g_recent_dumped) return;
  g_recent_dumped = true;
  std::cerr << "[TraceRecent] dump (" << why << "): last " << g_recent_len
            << " generated calls, oldest first\n";
  for (size_t i = 0; i < g_recent_len; ++i) {
    const RecentCall& e =
        g_recent_calls[(g_recent_pos + kRecentCallCount - g_recent_len + i) %
                       kRecentCallCount];
    std::cerr << "  func=0x" << std::hex << e.func << " lr=0x" << e.lr
             << " r3=0x" << e.r3 << " r4=0x" << e.r4 << " r13=0x" << e.r13
             << std::dec << "\n";
  }
}

uint32_t g_watch_addr = []() -> uint32_t {
  const char *env = std::getenv("NWII_WATCH");
  return env ? (uint32_t)std::strtoul(env, nullptr, 16) : 0;
}();

uint32_t g_watch_len = []() -> uint32_t {
  const char *env = std::getenv("NWII_WATCHLEN");
  return env ? (uint32_t)std::strtoul(env, nullptr, 16) : 4;
}();

extern CPUContext *g_ctx_ptr;

void watch_hit(uint32_t addr, uint32_t value, int width) {
  static const bool nonzero_only = std::getenv("NWII_WATCH_NZ") != nullptr;
  if (nonzero_only && value == 0)
    return;
  CPUContext *c = g_ctx_ptr;
  std::cout << "[Watch] write" << width << " 0x" << std::hex << addr << " = 0x"
            << value;
  if (c) {
    std::cout << " pc=0x" << c->pc << " lr=0x" << c->lr;

    
    static const bool regs = std::getenv("NWII_WATCH_REGS") != nullptr;
    if (regs) {
      for (int r = 24; r <= 31; ++r)
        std::cout << " r" << std::dec << r << "=0x" << std::hex << c->gpr[r];
      std::cout << " r3=0x" << c->gpr[3] << " r4=0x" << c->gpr[4];
    }
  }
  std::cout << std::dec << "\n";
}


static std::set<uint32_t> g_trace_only = []() {
  std::set<uint32_t> s;
  const char *env = std::getenv("NWII_TRACE_ONLY");
  if (env) {
    std::string in(env);
    size_t p = 0;
    while (p < in.size()) {
      size_t c = in.find(',', p);
      if (c == std::string::npos) c = in.size();
      s.insert((uint32_t)std::strtoul(in.substr(p, c - p).c_str(), nullptr, 16));
      p = c + 1;
    }
  }
  return s;
}();

void note_null_call(uint32_t site, uint32_t lr) {
  static int n = 0;
  if (n++ < 20)
    std::cout << "[NullCall] no-op call through null ptr at site 0x" << std::hex
              << site << " lr=0x" << lr << std::dec << "\n";
}

void trace_call(uint32_t func_addr, CPUContext &ctx) {
  // Optional address-range filter (NWII_TRACE_RANGE=lo-hi, hex) so a busy
  // library region can be traced without logging every call in the game.
  static const uint32_t range_lo = []() -> uint32_t {
    const char *e = std::getenv("NWII_TRACE_RANGE");
    return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0;
  }();
  static const uint32_t range_hi = []() -> uint32_t {
    const char *e = std::getenv("NWII_TRACE_RANGE");
    if (!e) return 0;
    const char *dash = std::strchr(e, '-');
    return dash ? (uint32_t)std::strtoul(dash + 1, nullptr, 16) : 0;
  }();
  if (range_lo && (func_addr < range_lo || func_addr > range_hi)) return;
  if (g_trace_recent_enabled) {
    g_recent_calls[g_recent_pos] = {func_addr, ctx.lr, ctx.gpr[3], ctx.gpr[4],
                                    ctx.gpr[13]};
    g_recent_pos = (g_recent_pos + 1) % kRecentCallCount;
    if (g_recent_len < kRecentCallCount) ++g_recent_len;
  }
  if (!g_trace_print) return;
  if (!g_trace_only.empty() && !g_trace_only.count(func_addr)) return;
  std::cout << "[CALL] 0x" << std::hex << func_addr << " lr=0x" << ctx.lr
            << " r3=0x" << ctx.gpr[3] << " r4=0x" << ctx.gpr[4] << " r5=0x"
            << ctx.gpr[5] << " r6=0x" << ctx.gpr[6] << " r7=0x" << ctx.gpr[7]
            << " r13=0x" << ctx.gpr[13] << " r1=0x" << ctx.gpr[1]
            << std::dec << "\n";
}
} 

static std::string read_guest_string(CPUContext &ctx, uint32_t addr) {
  std::string str;
  while (true) {
    uint8_t c = ctx.mmu.read8(addr++);
    if (c == 0)
      break;
    str += (char)c;
  }
  return str;
}

extern "C" void OSReport(CPUContext &ctx) {
  uint32_t format_addr = ctx.gpr[3];
  std::string format_str = read_guest_string(ctx, format_addr);

  std::string result;
  int arg_idx = 4;
  int fpr_idx = 1;

  for (size_t i = 0; i < format_str.length(); ++i) {
    if (format_str[i] == '%') {
      if (i + 1 < format_str.length() && format_str[i + 1] == '%') {
        result += '%';
        i++;
        continue;
      }

      std::string specifier = "%";
      i++;
      while (i < format_str.length() &&
             std::string("0123456789.-+ #hlz").find(format_str[i]) !=
                 std::string::npos) {
        specifier += format_str[i];
        i++;
      }

      if (i < format_str.length()) {
        char type = format_str[i];
        specifier += type;

        if (type == 'd' || type == 'i' || type == 'c') {
          char buf[64];
          std::snprintf(buf, sizeof(buf), specifier.c_str(),
                        (int32_t)ctx.gpr[arg_idx++]);
          result += buf;
        } else if (type == 'u' || type == 'x' || type == 'X' || type == 'p') {
          char buf[64];
          std::snprintf(buf, sizeof(buf), specifier.c_str(),
                        (uint32_t)ctx.gpr[arg_idx++]);
          result += buf;
        } else if (type == 's') {
          uint32_t str_ptr = ctx.gpr[arg_idx++];
          if (str_ptr != 0)
            result += read_guest_string(ctx, str_ptr);
          else
            result += "(null)";
        } else if (type == 'f') {
          char buf[64];
          if (fpr_idx <= 8)
            std::snprintf(buf, sizeof(buf), specifier.c_str(),
                          ctx.fpr[fpr_idx++]);
          else
            std::snprintf(buf, sizeof(buf), "[float_spilled]");
          result += buf;
        } else {
          result += specifier;
        }
      }
    } else {
      result += format_str[i];
    }
  }
  std::cout << "[OSReport] [LR:0x" << std::hex << ctx.lr << std::dec << "] " << result;
}

namespace nwii::runtime {
uint32_t HW_Reg_Read32(uint32_t addr) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  return MMIODispatcher::get().read32(addr);
}
uint16_t HW_Reg_Read16(uint32_t addr) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  return MMIODispatcher::get().read16(addr);
}
void HW_Reg_Write32(uint32_t addr, uint32_t val) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  MMIODispatcher::get().write32(addr, val);
}
void HW_Reg_Write16(uint32_t addr, uint16_t val) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  MMIODispatcher::get().write16(addr, val);
}
} 

extern "C" void DVD_Callback(CPUContext &ctx) {
  std::cout << "[HLE DVD_Callback] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIInit(CPUContext &ctx) {
  std::cout << "[HLE VIInit] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
  
}

extern "C" void VIConfigure(CPUContext &ctx) {
  std::cout << "[HLE VIConfigure] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIConfigurePan(CPUContext &ctx) {
  std::cout << "[HLE VIConfigurePan] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VISetBlack(CPUContext &ctx) {
  std::cout << "[HLE VISetBlack] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIGetNextField(CPUContext &ctx) {
  static uint32_t field = 0;
  field ^= 1;
  ctx.gpr[3] = field;
}

extern "C" void VIWaitForRetrace(CPUContext &ctx) {
  // Pace the guest's frame clock to the real display rate. Without this the
  // interrupt fires as fast as the CPU can run, so every frame-based timer in
  // the game (animations, the intro movie, menu idle timers) advances far
  // faster than wall clock.
  static const bool s_nopace = std::getenv("NWII_NOPACE") != nullptr;
  static auto next = std::chrono::steady_clock::now();
  if (!s_nopace) {
    next += std::chrono::microseconds(16683);  // 59.94 Hz
    const auto now = std::chrono::steady_clock::now();
    if (next > now) {
      std::this_thread::sleep_for(next - now);
    } else {
      next = now;
    }
  }
  nwii::runtime::hw::vi_trigger_interrupt();
}