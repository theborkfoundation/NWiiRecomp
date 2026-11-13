#pragma once
#include <array>
#include <cmath>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <stack>

namespace nwii {
namespace runtime {

struct ConditionField {
  bool lt; 
  bool gt; 
  bool eq; 
  bool so; 

  ConditionField() : lt(false), gt(false), eq(false), so(false) {}
};

void GX_WGPIPE_Write8(uint8_t val);
void GX_WGPIPE_Write16(uint16_t val);
void GX_WGPIPE_Write32(uint32_t val);
void GX_DL_Snapshot(uint32_t addr, uint32_t size);
bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out);
void GX_WGPIPE_WriteF32(float val);
void GX_WGPIPE_WriteF64(double val);
// Bytes pushed into the GX FIFO so far (equals the offset in an NWII_GXDUMP).
uint64_t GX_WGP_Count();

// Writes that hit the write-gather pipe page, split by whether they targeted the
// pipe itself (offset 0x8000 within the page) or some other address in it.
extern uint64_t g_wgp_addr_exact;
extern uint64_t g_wgp_addr_page;
extern uint32_t g_wgp_addr_hist[1024];
// Diagnostic: ignore writes at page offset 4 (0x0C008004) to test whether they
// belong to the FIFO at all.
extern bool g_wgp_drop4;

static inline bool wgp_skip(uint32_t paddr) {
  return g_wgp_drop4 && (paddr & 0xFFFu) == 4u;
}

uint16_t HW_Reg_Read16(uint32_t addr);
uint32_t HW_Reg_Read32(uint32_t addr);
void HW_Reg_Write16(uint32_t addr, uint16_t val);
void HW_Reg_Write32(uint32_t addr, uint32_t val);

struct CPUContext;
void init_ipc_client(CPUContext &ctx);
bool handle_syscall(CPUContext &ctx);
bool process_pending_callbacks(CPUContext &ctx);

struct CallbackInfo {
    uint32_t cb_addr;
    uint32_t arg1;
    uint32_t arg2;
    bool is_irq;
};



void watch_hit(uint32_t addr, uint32_t value, int width);
extern uint32_t g_watch_addr;
extern uint32_t g_watch_len;
static inline bool watch_contains(uint32_t paddr) {
  if (g_watch_addr == 0) return false;
  const uint32_t w = g_watch_addr & 0x3FFFFFFF;
  return paddr >= w && paddr < w + g_watch_len;
}

// Strict Dolphin-accurate MMU with inlined fast-path
struct MMU {
  std::vector<uint8_t> mem1;
  std::vector<uint8_t> mem2;
  uint8_t* mem1_ptr = nullptr;
  uint8_t* mem2_ptr = nullptr;

  MMU() {
    mem1.resize(24 * 1024 * 1024 + 8, 0); 
    mem2.resize(64 * 1024 * 1024 + 8, 0); 
    mem1_ptr = mem1.data();
    mem2_ptr = mem2.data();
  }

  MMU(const MMU& other) : mem1(other.mem1), mem2(other.mem2) {
    mem1_ptr = mem1.data();
    mem2_ptr = mem2.data();
  }

  MMU& operator=(const MMU& other) {
    if (this != &other) {
      mem1 = other.mem1;
      mem2 = other.mem2;
      mem1_ptr = mem1.data();
      mem2_ptr = mem2.data();
    }
    return *this;
  }

  MMU(MMU&& other) noexcept : mem1(std::move(other.mem1)), mem2(std::move(other.mem2)) {
    mem1_ptr = mem1.data();
    mem2_ptr = mem2.data();
  }

  MMU& operator=(MMU&& other) noexcept {
    if (this != &other) {
      mem1 = std::move(other.mem1);
      mem2 = std::move(other.mem2);
      mem1_ptr = mem1.data();
      mem2_ptr = mem2.data();
    }
    return *this;
  }

  inline uint8_t *get_ptr(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF; 
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      return mem1_ptr + paddr;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      return mem2_ptr + offset2;
    }
    return nullptr;
  }

  inline bool is_hw_reg(uint32_t paddr) const {
    return (paddr >= 0x0C000000 && paddr < 0x0C010000) || 
           (paddr >= 0x0D000000 && paddr < 0x0D010000);
  }

  inline bool is_wgpipe(uint32_t paddr) const {
    return (paddr & 0xFFFFF000u) == 0x0C008000 || (paddr & 0xFFFFF000u) == 0x0D008000;
  }

  // Address accounting for writes that land in the write-gather pipe page. Only
  // 0x0C008000 (and its 0x0D alias) is the FIFO window; anything else in the
  // page is a different address and must not be treated as FIFO data.
  static inline void wgp_account(uint32_t paddr, int width) {
    uint32_t page_off = (paddr & 0xFFFu) >> 2;
    if (page_off == 0) {
      ++g_wgp_addr_exact;
      return;
    }
    ++g_wgp_addr_page;
    ++g_wgp_addr_hist[page_off & 1023u];
    static const bool dbg = std::getenv("NWII_WGPADDR") != nullptr;
    static int n = 0;
    if (dbg && n++ < 24)
      std::cout << "[WGPADDR] at=" << GX_WGP_Count() << " addr=0x" << std::hex
                << paddr << std::dec << " width=" << width << std::endl;
    // Menu phase: does the headerless vertex run come from these writes?
    if (dbg && GX_WGP_Count() > 12000) {
      static int m = 0;
      if (m++ < 4000)
        std::cout << "[WGPADDR-LATE] at=" << GX_WGP_Count() << " addr=0x"
                  << std::hex << paddr << std::dec << " width=" << width
                  << std::endl;
    }
  }

  void custom_watch(uint32_t, uint32_t, int) {}

  // Slow-path fallback handlers (out-of-line)
  __attribute__((noinline)) uint8_t read8_slow(uint32_t paddr) const {
    if (is_hw_reg(paddr)) {
      uint32_t val = HW_Reg_Read32(paddr & ~3);
      int shift = 24 - ((paddr & 3) * 8);
      return (val >> shift) & 0xFF;
    }
    return 0;
  }

  __attribute__((noinline)) void write8_slow(uint32_t paddr, uint8_t value) {
    if (is_wgpipe(paddr)) { if (wgp_skip(paddr)) return; wgp_account(paddr, 8); GX_WGPIPE_Write8(value); return; }
    if (is_hw_reg(paddr)) return; 
  }

  __attribute__((noinline)) uint16_t read16_slow(uint32_t paddr) const {
    if (is_hw_reg(paddr)) return HW_Reg_Read16(paddr);
    return 0;
  }

  __attribute__((noinline)) void write16_slow(uint32_t paddr, uint16_t value) {
    if (is_wgpipe(paddr)) { if (wgp_skip(paddr)) return; wgp_account(paddr, 16); GX_WGPIPE_Write16(value); return; }
    if (is_hw_reg(paddr)) { HW_Reg_Write16(paddr, value); return; }
  }

  __attribute__((noinline)) uint32_t read32_slow(uint32_t paddr) const {
    if (is_hw_reg(paddr)) return HW_Reg_Read32(paddr);
    return 0;
  }

  __attribute__((noinline)) void write32_slow(uint32_t paddr, uint32_t value) {
    if (is_wgpipe(paddr)) { if (wgp_skip(paddr)) return; wgp_account(paddr, 32); GX_WGPIPE_Write32(value); return; }
    if (is_hw_reg(paddr)) { HW_Reg_Write32(paddr, value); return; }
  }

  __attribute__((noinline)) void write64_slow(uint32_t paddr, uint64_t value) {
    if (is_wgpipe(paddr)) {
      if (wgp_skip(paddr)) return;
      wgp_account(paddr, 64);
      GX_WGPIPE_Write32((uint32_t)(value >> 32));
      GX_WGPIPE_Write32((uint32_t)(value & 0xFFFFFFFF));
    }
  }

  __attribute__((noinline)) void write_f32_slow(uint32_t paddr, float value) {
    if (is_wgpipe(paddr)) { if (wgp_skip(paddr)) return; wgp_account(paddr, 32); GX_WGPIPE_WriteF32(value); return; }
  }

  __attribute__((noinline)) void write_f64_slow(uint32_t paddr, double value) {
    if (is_wgpipe(paddr)) { if (wgp_skip(paddr)) return; wgp_account(paddr, 64); GX_WGPIPE_WriteF64(value); return; }
  }

  // Fast-path inlined operations
  __attribute__((always_inline)) inline uint8_t read8(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      return mem1_ptr[paddr];
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      return mem2_ptr[offset2];
    }
    return read8_slow(paddr);
  }

  __attribute__((always_inline)) inline void write8(uint32_t addr, uint8_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      if (watch_contains(paddr))
        watch_hit(addr, value, 8);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      mem1_ptr[paddr] = value;
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      mem2_ptr[offset2] = value;
      return;
    }
    write8_slow(paddr, value);
  }

  __attribute__((always_inline)) inline uint16_t read16(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint16_t val;
      std::memcpy(&val, mem1_ptr + paddr, 2);
      return __builtin_bswap16(val);
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint16_t val;
      std::memcpy(&val, mem2_ptr + offset2, 2);
      return __builtin_bswap16(val);
    }
    return read16_slow(paddr);
  }

  __attribute__((always_inline)) inline void write16(uint32_t addr, uint16_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      if (watch_contains(paddr))
        watch_hit(addr, value, 16);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint16_t swp = __builtin_bswap16(value);
      std::memcpy(mem1_ptr + paddr, &swp, 2);
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint16_t swp = __builtin_bswap16(value);
      std::memcpy(mem2_ptr + offset2, &swp, 2);
      return;
    }
    write16_slow(paddr, value);
  }

  __attribute__((always_inline)) inline uint32_t read32(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      if (__builtin_expect(paddr == 0x0000000Cu, 0)) return 0;
      uint32_t val;
      std::memcpy(&val, mem1_ptr + paddr, 4);
      return __builtin_bswap32(val);
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint32_t val;
      std::memcpy(&val, mem2_ptr + offset2, 4);
      return __builtin_bswap32(val);
    }
    return read32_slow(paddr);
  }

  __attribute__((always_inline)) inline void write32(uint32_t addr, uint32_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      if (watch_contains(paddr))
        watch_hit(addr, value, 32);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      if (__builtin_expect(paddr == 0x0000000Cu, 0)) return;
      uint32_t swp = __builtin_bswap32(value);
      std::memcpy(mem1_ptr + paddr, &swp, 4);
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint32_t swp = __builtin_bswap32(value);
      std::memcpy(mem2_ptr + offset2, &swp, 4);
      return;
    }
    write32_slow(paddr, value);
  }

  __attribute__((always_inline)) inline float read_f32(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      if (__builtin_expect(paddr == 0x0000000Cu, 0)) return 0.0f;
      uint32_t val;
      std::memcpy(&val, mem1_ptr + paddr, 4);
      val = __builtin_bswap32(val);
      float f;
      std::memcpy(&f, &val, 4);
      return f;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint32_t val;
      std::memcpy(&val, mem2_ptr + offset2, 4);
      val = __builtin_bswap32(val);
      float f;
      std::memcpy(&f, &val, 4);
      return f;
    }
    uint32_t val = read32_slow(paddr);
    float f;
    std::memcpy(&f, &val, 4);
    return f;
  }

  __attribute__((always_inline)) inline void write_f32(uint32_t addr, float value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      uint32_t v;
      std::memcpy(&v, &value, 4);
      if (watch_contains(paddr))
        watch_hit(addr, v, 32);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint32_t v;
      std::memcpy(&v, &value, 4);
      v = __builtin_bswap32(v);
      std::memcpy(mem1_ptr + paddr, &v, 4);
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint32_t v;
      std::memcpy(&v, &value, 4);
      v = __builtin_bswap32(v);
      std::memcpy(mem2_ptr + offset2, &v, 4);
      return;
    }
    write_f32_slow(paddr, value);
  }

  __attribute__((always_inline)) inline uint64_t read64(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint64_t val;
      std::memcpy(&val, mem1_ptr + paddr, 8);
      return __builtin_bswap64(val);
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint64_t val;
      std::memcpy(&val, mem2_ptr + offset2, 8);
      return __builtin_bswap64(val);
    }
    return 0;
  }

  __attribute__((always_inline)) inline void write64(uint32_t addr, uint64_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      if (watch_contains(paddr))
        watch_hit(addr, value >> 32, 64);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint64_t swp = __builtin_bswap64(value);
      std::memcpy(mem1_ptr + paddr, &swp, 8);
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint64_t swp = __builtin_bswap64(value);
      std::memcpy(mem2_ptr + offset2, &swp, 8);
      return;
    }
    write64_slow(paddr, value);
  }

  __attribute__((always_inline)) inline double read_f64(uint32_t addr) const {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint64_t val;
      std::memcpy(&val, mem1_ptr + paddr, 8);
      val = __builtin_bswap64(val);
      double f;
      std::memcpy(&f, &val, 8);
      return f;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint64_t val;
      std::memcpy(&val, mem2_ptr + offset2, 8);
      val = __builtin_bswap64(val);
      double f;
      std::memcpy(&f, &val, 8);
      return f;
    }
    return 0.0;
  }

  __attribute__((always_inline)) inline void write_f64(uint32_t addr, double value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (__builtin_expect(g_watch_addr != 0, 0)) {
      uint64_t v;
      std::memcpy(&v, &value, 8);
      if (watch_contains(paddr))
        watch_hit(addr, v >> 32, 64);
    }
    if (__builtin_expect(paddr < 0x01800000u, 1)) {
      uint64_t v;
      std::memcpy(&v, &value, 8);
      v = __builtin_bswap64(v);
      std::memcpy(mem1_ptr + paddr, &v, 8);
      return;
    }
    uint32_t offset2 = paddr - 0x10000000u;
    if (__builtin_expect(offset2 < 0x04000000u, 1)) {
      uint64_t v;
      std::memcpy(&v, &value, 8);
      v = __builtin_bswap64(v);
      std::memcpy(mem2_ptr + offset2, &v, 8);
      return;
    }
    write_f64_slow(paddr, value);
  }
};

struct CPUContext {
  
  std::array<uint32_t, 32> gpr;

  std::array<double, 32> fpr;

  std::array<double, 32> ps1;

  std::array<ConditionField, 8> cr;

  uint32_t pc;    
  uint32_t lr;    
  uint32_t ctr;   
  uint32_t xer;   
  uint32_t msr;   
  uint32_t fpscr; 
  uint32_t srr0;  
  uint32_t srr1;  

  std::array<uint32_t, 8> gqr;
  std::array<uint32_t, 4> sprg; 

  struct BackupState {
    std::array<uint32_t, 32> gpr;
    std::array<double, 32> fpr;
    std::array<double, 32> ps1;
    std::array<ConditionField, 8> cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t pc;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t msr;
    uint32_t fpscr;
    std::array<uint32_t, 8> gqr;
    std::array<uint32_t, 4> sprg;
  };
  std::stack<BackupState> backup_stack;
  bool in_callback = false;
  int callback_depth = 0;

  MMU mmu;

  uint32_t exception_pc;
  uint64_t inst_count = 0;
  
  std::atomic<bool> is_running;
  std::atomic<bool> vblank_pending;
  std::mutex cb_mutex;
  std::queue<CallbackInfo> pending_callbacks;
  
  std::atomic<int> pending_cb_count{0};
  jmp_buf exception_jmp_buf;

  uint32_t reservation_addr = 0xFFFFFFFF;

  

  uint32_t dispatch_saved_ctx = 0;

  
  bool ext_resched_pending = false;

  



  

  
  uint64_t tb_freq = 40500000;
  std::chrono::steady_clock::time_point tb_start =
      std::chrono::steady_clock::now();

  

  
  // The timebase must advance at tb_freq (60.75 MHz) relative to wall time.
  // Using inst_count made it advance at emulator instruction throughput, which
  // is several times 60.75M/s here: guest timers ran ~3x fast, so the 15s boot
  // movie played in under 5s. Wall clock is the default now; NWII_INSTTB=1
  // restores the old virtual clock for experiments.
  uint64_t read_timebase() const {
    static const bool inst =
        std::getenv("NWII_INSTTB") != nullptr;
    if (inst)
      return inst_count;
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - tb_start)
                      .count();
    return us / 1000000 * tb_freq + us % 1000000 * tb_freq / 1000000;
  }

  
  uint64_t dec_value = 0xFFFFFFFF;
  uint64_t dec_written_tb = 0;
  bool dec_irq_pending = false;

  void write_dec(uint32_t v) {
    static const bool s_sample = std::getenv("NWII_SAMPLE") != nullptr;
    if (s_sample) {
      static uint64_t n = 0;
      ++n;
      if (n <= 10 || (n % 500) == 0)
        std::cout << "[DEC] mtdec #" << n << " val=" << v << " pc=0x"
                  << std::hex << pc << " lr=0x" << lr << std::dec
                  << " inst=" << inst_count << "\n";
    }
    dec_value = v;
    dec_written_tb = read_timebase();
    dec_irq_pending = (v != 0xFFFFFFFF);
  }

  uint32_t read_dec() {
    uint64_t elapsed = read_timebase() - dec_written_tb;
    if (elapsed >= dec_value)
      return 0;
    return (uint32_t)(dec_value - elapsed);
  }

  bool dec_expired() {
    return dec_irq_pending && (read_timebase() - dec_written_tb) >= dec_value;
  }

  CPUContext() : gpr{0}, fpr{0.0}, ps1{0.0}, cr{}, pc(0), lr(0), ctr(0), xer(0), 
                 msr(0), fpscr(0), srr0(0), srr1(0), gqr{0}, exception_pc(0), is_running(true), vblank_pending(false) {}

  // The Wii CPU's data cache line is 32 bytes. dcbz clears the whole
  // aligned line, including bytes before the effective address.
  void clear_cache_line(uint32_t addr) {
    addr &= ~31u;
    for (uint32_t i = 0; i < 32; i += 4)
      mmu.write32(addr + i, 0);
  }

  // mffs copies FPSCR into the low 32 bits of the destination FPR. Hardware
  // leaves the high 32 bits undefined; use Dolphin's signalling-NaN prefix so
  // the value survives stfd/lfd round trips bit-exactly.
  uint64_t mffs_bits() const { return 0xFFF8000000000000ull | fpscr; }

  // mtfsf writes the FPSCR nibbles selected by the 8-bit field mask.
  void mtfsf(uint32_t fm, uint32_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; ++i)
      if (fm & (1u << i)) mask |= 0xFu << (i * 4);
    fpscr = (fpscr & ~mask) | (value & mask);
  }

  // mtfsfi writes one 4-bit FPSCR field from an instruction immediate.
  void mtfsfi(uint32_t field, uint32_t imm) {
    field &= 7;
    uint32_t mask = 0xF0000000u >> (4 * field);
    fpscr = (fpscr & ~mask) | ((imm & 0xFu) << (28 - 4 * field));
  }

  // mtfsb0 (set=false) / mtfsb1 (set=true) change a single FPSCR bit.
  void set_fpscr_bit(uint32_t bit, bool set) {
    uint32_t mask = 0x80000000u >> (bit & 31);
    if (set) fpscr |= mask;
    else fpscr &= ~mask;
  }

  // mcrfs copies one FPSCR field into a CR field and clears the exception
  // bits that were read (FPSCR.FX and the exception bits in that field).
  void mcrfs(uint32_t crfd, uint32_t crfs) {
    const uint32_t shift = 4 * (7 - (crfs & 7));
    const uint32_t value = (fpscr >> shift) & 0xF;
    fpscr &= ~((0xFu << shift) & 0xBFF80000u); // FX | all exception bits
    auto &f = cr[crfd & 7];
    f.lt = (value & 8) != 0;
    f.gt = (value & 4) != 0;
    f.eq = (value & 2) != 0;
    f.so = (value & 1) != 0;
  }

  void queue_callback(uint32_t cb, uint32_t arg1, uint32_t arg2, bool is_irq = false) {
    std::lock_guard<std::mutex> lock(cb_mutex);
    pending_callbacks.push({cb, arg1, arg2, is_irq});
    pending_cb_count.fetch_add(1, std::memory_order_relaxed);
  }

  void psq_load(uint32_t frD, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t ld_type = (gqr_val >> 16) & 0x7;
    int32_t ld_scale = int32_t(gqr_val << 2) >> 26;
    float scale = std::pow(2.0f, -((int)ld_scale));

    auto load_element = [&](uint32_t a) -> double {
      switch (ld_type) {
      case 0:
        return mmu.read_f32(a);
      case 4:
        return mmu.read8(a) * scale;
      case 5:
        return mmu.read16(a) * scale;
      case 6:
        return (int8_t)mmu.read8(a) * scale;
      case 7:
        return (int16_t)mmu.read16(a) * scale;
      default:
        return 0.0;
      }
    };

    uint32_t elem_size =
        (ld_type == 0) ? 4 : ((ld_type == 4 || ld_type == 6) ? 1 : 2);

    fpr[frD] = load_element(addr);
    if (W == 0) { 
      ps1[frD] = load_element(addr + elem_size);
    } else { 
      ps1[frD] = 1.0;
    }
  }

  void psq_store(uint32_t frS, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t st_type = gqr_val & 0x7;
    int32_t st_scale = int32_t(gqr_val << 18) >> 26;
    float scale = std::pow(2.0f, (int)st_scale);

    auto store_element = [&](uint32_t a, double val) {
      switch (st_type) {
      case 0:
        mmu.write_f32(a, (float)val);
        break;
      case 4:
        mmu.write8(a, (uint8_t)(val * scale));
        break;
      case 5:
        mmu.write16(a, (uint16_t)(val * scale));
        break;
      case 6:
        mmu.write8(a, (int8_t)(val * scale));
        break;
      case 7:
        mmu.write16(a, (int16_t)(val * scale));
        break;
      }
    };

    uint32_t elem_size =
        (st_type == 0) ? 4 : ((st_type == 4 || st_type == 6) ? 1 : 2);

    store_element(addr, fpr[frS]);
    if (W == 0) {
      store_element(addr + elem_size, ps1[frS]);
    }
  }

  inline void ps_add(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fpr[A] + fpr[B];
    ps1[D] = ps1[A] + ps1[B];
  }
  inline void ps_sub(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fpr[A] - fpr[B];
    ps1[D] = ps1[A] - ps1[B];
  }
  inline void ps_mul(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = fpr[A] * fpr[C];
    ps1[D] = ps1[A] * ps1[C];
  }
  inline void ps_madd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = std::fma(fpr[A], fpr[C], fpr[B]);
    ps1[D] = std::fma(ps1[A], ps1[C], ps1[B]);
  }
  inline void ps_msub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = std::fma(fpr[A], fpr[C], -fpr[B]);
    ps1[D] = std::fma(ps1[A], ps1[C], -ps1[B]);
  }
  inline void ps_nmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = -std::fma(fpr[A], fpr[C], fpr[B]);
    ps1[D] = -std::fma(ps1[A], ps1[C], ps1[B]);
  }
  inline void ps_nmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = -std::fma(fpr[A], fpr[C], -fpr[B]);
    ps1[D] = -std::fma(ps1[A], ps1[C], -ps1[B]);
  }
  inline void ps_div(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = fpr[A] / fpr[B];
    ps1[D] = ps1[A] / ps1[B];
  }
  inline void ps_sum0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {

    
    double s = fpr[A] + ps1[B];
    double t = ps1[C];
    fpr[D] = (float)s;
    ps1[D] = (float)t;
  }
  inline void ps_sum1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    
    double s = fpr[C];
    double t = fpr[A] + ps1[B];
    fpr[D] = (float)s;
    ps1[D] = (float)t;
  }
  inline void ps_muls0(uint32_t D, uint32_t A, uint32_t C) {
    const double c0 = fpr[C];
    fpr[D] = fpr[A] * c0;
    ps1[D] = ps1[A] * c0;
  }
  inline void ps_muls1(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = fpr[A] * ps1[C];
    ps1[D] = ps1[A] * ps1[C];
  }
  inline void ps_madds0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double c0 = fpr[C];
    fpr[D] = std::fma(fpr[A], c0, fpr[B]);
    ps1[D] = std::fma(ps1[A], c0, ps1[B]);
  }
  inline void ps_madds1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = std::fma(fpr[A], ps1[C], fpr[B]);
    ps1[D] = std::fma(ps1[A], ps1[C], ps1[B]);
  }
  inline void ps_sel(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (fpr[A] >= 0.0) ? fpr[C] : fpr[B];
    ps1[D] = (ps1[A] >= 0.0) ? ps1[C] : ps1[B];
  }
  inline void ps_mr(uint32_t D, uint32_t B) {
    fpr[D] = fpr[B];
    ps1[D] = ps1[B];
  }
  inline void ps_neg(uint32_t D, uint32_t B) {
    fpr[D] = -fpr[B];
    ps1[D] = -ps1[B];
  }
  inline void ps_abs(uint32_t D, uint32_t B) {
    fpr[D] = std::abs(fpr[B]);
    ps1[D] = std::abs(ps1[B]);
  }
  inline void ps_nabs(uint32_t D, uint32_t B) {
    fpr[D] = -std::abs(fpr[B]);
    ps1[D] = -std::abs(ps1[B]);
  }

  
  inline void ps_merge00(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge01(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge10(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge11(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_cmpu0(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(fpr[A]) || std::isnan(fpr[B]);
    cr[crD].lt = !un && (fpr[A] < fpr[B]);
    cr[crD].gt = !un && (fpr[A] > fpr[B]);
    cr[crD].eq = !un && (fpr[A] == fpr[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpo0(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(fpr[A]) || std::isnan(fpr[B]);
    cr[crD].lt = !un && (fpr[A] < fpr[B]);
    cr[crD].gt = !un && (fpr[A] > fpr[B]);
    cr[crD].eq = !un && (fpr[A] == fpr[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpu1(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(ps1[A]) || std::isnan(ps1[B]);
    cr[crD].lt = !un && (ps1[A] < ps1[B]);
    cr[crD].gt = !un && (ps1[A] > ps1[B]);
    cr[crD].eq = !un && (ps1[A] == ps1[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpo1(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(ps1[A]) || std::isnan(ps1[B]);
    cr[crD].lt = !un && (ps1[A] < ps1[B]);
    cr[crD].gt = !un && (ps1[A] > ps1[B]);
    cr[crD].eq = !un && (ps1[A] == ps1[B]);
    cr[crD].so = un;
  }
};

void hle_set_ipc_arm_msg(uint32_t req_addr);
void micro_interpret(CPUContext& ctx, uint32_t opcode, uint32_t pc);

bool interpret_one(CPUContext& ctx);
void interpret_step(CPUContext& ctx);
void add_recompiled_range(uint32_t start, uint32_t end);
bool in_recompiled_code(uint32_t pc);

extern bool g_trace_calls;
void trace_call(uint32_t func_addr, CPUContext& ctx);
void trace_recent_dump(const char* why);




void note_null_call(uint32_t site, uint32_t lr);

} 
} 
