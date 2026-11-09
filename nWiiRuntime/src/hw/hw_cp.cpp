#include "runtime/hw/hw.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <vector>
#include <mutex>

extern std::mutex g_fifo_mutex;
extern std::vector<uint8_t> g_hw_fifo;

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;
}

namespace nwii::runtime::hw {

static uint16_t g_cp_cr = 0;
static uint16_t g_cp_sr = 0;
static uint16_t g_cp_clear = 0;
static uint32_t g_cp_fifo_base = 0;
static uint32_t g_cp_fifo_end = 0;
static uint32_t g_cp_fifo_hi = 0;
static uint32_t g_cp_fifo_lo = 0;
static uint32_t g_cp_fifo_rw_dist = 0;
static uint32_t g_cp_fifo_wp = 0;
static uint32_t g_cp_fifo_rp = 0;
static uint32_t g_cp_fifo_bp = 0;

void cp_signal_draw_done() {
    static int n = 0;
    if (n++ < 10) {
        std::cout << "[CP] signal_draw_done -> trigger interrupt 17 (0x4000)\n";
    }
    g_cp_sr |= 0x12; // Breakpoint / Draw done flag in CP_SR (bit 4) + Underflow flag (bit 1)
    pi_intmr |= 0x4000;
    trigger_pi_interrupt(0x4000);
}

void cp_process_fifo() {
    if (!g_ctx_ptr) return;
    if (!(g_cp_cr & 1)) return; 

    uint32_t rp = g_cp_fifo_rp & 0x03FFFFFF;
    uint32_t wp = g_cp_fifo_wp & 0x03FFFFFF;
    uint32_t base = g_cp_fifo_base & 0x03FFFFFF;
    uint32_t end = g_cp_fifo_end & 0x03FFFFFF;

    if (base == 0 || end == 0 || base >= end) return;
    if (rp < base || rp >= end) rp = base;
    if (wp < base || wp >= end) wp = base;

    if (rp == wp) {
        g_cp_sr |= 0x0E; // Read idle (bit 2) + Cmd idle (bit 3) + Underflow (bit 1)
        return;
    }

    g_cp_sr &= ~0x0E;

    std::vector<uint8_t> batch;
    uint32_t rp0 = rp;
    int count = 0;
    while (rp != wp && count++ < 100000) {
        uint8_t b = g_ctx_ptr->mmu.read8(0x80000000u | rp);
        batch.push_back(b);
        rp++;
        if (rp >= end) {
            rp = base;
        }
    }
    g_cp_fifo_rp = rp;

    if (!batch.empty()) {
        std::lock_guard<std::mutex> lock(::g_fifo_mutex);
        ::g_hw_fifo.insert(::g_hw_fifo.end(), batch.begin(), batch.end());
    }

    if (g_cp_fifo_rp == g_cp_fifo_wp) {
        g_cp_sr |= 0x0E;
    }

    if (count > 0 && ([](){ static const bool s = (std::getenv("NWII_CPTRACE") != nullptr); return s; })())
        if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[CPFIFO] drained " << std::dec << count << " bytes rp=0x"
                  << std::hex << rp0 << "->0x" << rp << " wp=0x" << wp
                  << " base=0x" << base << " end=0x" << end << std::dec << "\n";
}

uint32_t cp_fifo_base_reg() { return g_cp_fifo_base; }

static int cp_debug_count = 0;
void register_cp(MMIODispatcher &dispatcher) {
    dispatcher.register_region(0xCC000000, 0xCC00007F,
        // read32
        [](uint32_t addr) -> uint32_t {
            switch (addr) {
                case 0xCC000000: {
                    uint16_t sr = g_cp_sr;
                    if (g_cp_fifo_rp == g_cp_fifo_wp) sr |= 0x0E;
                    return ((uint32_t)sr << 16) | g_cp_cr;
                }
                case 0xCC000002: return g_cp_cr;
                case 0xCC000020: return g_cp_fifo_base;
                case 0xCC000024: return g_cp_fifo_end;
                case 0xCC000028: return g_cp_fifo_hi;
                case 0xCC00002C: return g_cp_fifo_lo;
                case 0xCC000030: return g_cp_fifo_rw_dist;
                case 0xCC000034: return g_cp_fifo_wp;
                case 0xCC000038: return g_cp_fifo_rp;
                case 0xCC00003C: return g_cp_fifo_bp;
            }
            return 0;
        },
        // write32
        [](uint32_t addr, uint32_t val) {
            if (cp_debug_count++ < 100)
                if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[CP] write32 addr=" << std::hex << addr << " val=" << val << "\n";
            if (addr == 0xCC000000) {
                g_cp_sr = (uint16_t)(val >> 16);
                g_cp_cr = (uint16_t)val;
                if (val & 0x10) {
                    g_cp_sr &= ~0x10;
                }
                if ((g_cp_sr & 0x12) == 0) {
                    clear_pi_interrupt(0x4000);
                }
                cp_process_fifo();
            }
            else if (addr == 0xCC000002) {
                g_cp_cr = (uint16_t)val;
                if (val & 0x10) {
                    g_cp_sr &= ~0x10;
                }
                if ((g_cp_sr & 0x12) == 0) {
                    clear_pi_interrupt(0x4000);
                }
                cp_process_fifo();
            }
            else if (addr == 0xCC000004) {
                g_cp_clear = (uint16_t)val;
                if (val & 0x01) g_cp_sr &= ~0x01; // clear overflow
                if (val & 0x02) g_cp_sr &= ~0x02; // clear underflow
                if (val & 0x10) g_cp_sr &= ~0x10; // clear breakpoint
                if ((g_cp_sr & 0x13) == 0) {
                    clear_pi_interrupt(0x4000);
                }
            }
            else if (addr == 0xCC000020) { g_cp_fifo_base = val; }
            else if (addr == 0xCC000024) { g_cp_fifo_end = val; }
            else if (addr == 0xCC000028) { g_cp_fifo_hi = val; }
            else if (addr == 0xCC00002C) { g_cp_fifo_lo = val; }
            else if (addr == 0xCC000030) { g_cp_fifo_rw_dist = val; }
            else if (addr == 0xCC000034) {
                g_cp_fifo_wp = val;
                cp_process_fifo();
            }
            else if (addr == 0xCC000038) { g_cp_fifo_rp = val; }
            else if (addr == 0xCC00003C) { g_cp_fifo_bp = val; }
        },
        // read16
        [](uint32_t addr) -> uint16_t {
            switch (addr) {
                case 0xCC000000: {
                    uint16_t sr = g_cp_sr;
                    if (g_cp_fifo_rp == g_cp_fifo_wp) sr |= 0x0E;
                    return sr;
                }
                case 0xCC000002: return g_cp_cr;
                case 0xCC000020: return g_cp_fifo_base & 0xFFFF;
                case 0xCC000022: return (g_cp_fifo_base >> 16) & 0xFFFF;
                case 0xCC000024: return g_cp_fifo_end & 0xFFFF;
                case 0xCC000026: return (g_cp_fifo_end >> 16) & 0xFFFF;
                case 0xCC000028: return g_cp_fifo_hi & 0xFFFF;
                case 0xCC00002A: return (g_cp_fifo_hi >> 16) & 0xFFFF;
                case 0xCC00002C: return g_cp_fifo_lo & 0xFFFF;
                case 0xCC00002E: return (g_cp_fifo_lo >> 16) & 0xFFFF;
                case 0xCC000030: return g_cp_fifo_rw_dist & 0xFFFF;
                case 0xCC000032: return (g_cp_fifo_rw_dist >> 16) & 0xFFFF;
                case 0xCC000034: return g_cp_fifo_wp & 0xFFFF;
                case 0xCC000036: return (g_cp_fifo_wp >> 16) & 0xFFFF;
                case 0xCC000038: return g_cp_fifo_rp & 0xFFFF;
                case 0xCC00003A: return (g_cp_fifo_rp >> 16) & 0xFFFF;
                case 0xCC00003C: return g_cp_fifo_bp & 0xFFFF;
                case 0xCC00003E: return (g_cp_fifo_bp >> 16) & 0xFFFF;
            }
            return 0;
        },
        // write16
        [](uint32_t addr, uint16_t val) {
            if (cp_debug_count++ < 100)
                if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[CP] write16 addr=" << std::hex << addr << " val=" << val << "\n";
            if (addr == 0xCC000000) { g_cp_sr = val; }
            else if (addr == 0xCC000002) {
                g_cp_cr = val;
                if (val & 0x10) {
                    g_cp_sr &= ~0x10;
                }
                if ((g_cp_sr & 0x12) == 0) {
                    clear_pi_interrupt(0x4000);
                }
                cp_process_fifo();
            }
            else if (addr == 0xCC000004) {
                g_cp_clear = val;
                if (val & 0x01) g_cp_sr &= ~0x01; // clear overflow
                if (val & 0x02) g_cp_sr &= ~0x02; // clear underflow
                if (val & 0x10) g_cp_sr &= ~0x10; // clear breakpoint
                if ((g_cp_sr & 0x13) == 0) {
                    clear_pi_interrupt(0x4000);
                }
            }
            else if (addr == 0xCC000020) { g_cp_fifo_base = (g_cp_fifo_base & 0xFFFF0000) | val; }
            else if (addr == 0xCC000022) { g_cp_fifo_base = (g_cp_fifo_base & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC000024) { g_cp_fifo_end = (g_cp_fifo_end & 0xFFFF0000) | val; }
            else if (addr == 0xCC000026) { g_cp_fifo_end = (g_cp_fifo_end & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC000028) { g_cp_fifo_hi = (g_cp_fifo_hi & 0xFFFF0000) | val; }
            else if (addr == 0xCC00002A) { g_cp_fifo_hi = (g_cp_fifo_hi & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC00002C) { g_cp_fifo_lo = (g_cp_fifo_lo & 0xFFFF0000) | val; }
            else if (addr == 0xCC00002E) { g_cp_fifo_lo = (g_cp_fifo_lo & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC000030) { g_cp_fifo_rw_dist = (g_cp_fifo_rw_dist & 0xFFFF0000) | val; }
            else if (addr == 0xCC000032) { g_cp_fifo_rw_dist = (g_cp_fifo_rw_dist & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC000034) {
                g_cp_fifo_wp = (g_cp_fifo_wp & 0xFFFF0000) | val;
                cp_process_fifo();
            }
            else if (addr == 0xCC000036) {
                g_cp_fifo_wp = (g_cp_fifo_wp & 0x0000FFFF) | ((uint32_t)val << 16);
                cp_process_fifo();
            }
            else if (addr == 0xCC000038) { g_cp_fifo_rp = (g_cp_fifo_rp & 0xFFFF0000) | val; }
            else if (addr == 0xCC00003A) { g_cp_fifo_rp = (g_cp_fifo_rp & 0x0000FFFF) | ((uint32_t)val << 16); }
            else if (addr == 0xCC00003C) { g_cp_fifo_bp = (g_cp_fifo_bp & 0xFFFF0000) | val; }
            else if (addr == 0xCC00003E) { g_cp_fifo_bp = (g_cp_fifo_bp & 0x0000FFFF) | ((uint32_t)val << 16); }
        }
    );
}

} // namespace nwii::runtime::hw
