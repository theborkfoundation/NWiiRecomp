#include "runtime/hw/hw.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <atomic>

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;
}

namespace nwii::runtime::hw {

// PE control register (0xCC00100A), Dolphin UPECtrlReg: bit 0 = token
// interrupt enable, bit 1 = finish interrupt enable. Bits 2 and 3 are
// interrupt status/acknowledge flags (write 1 to clear).
static std::atomic<uint32_t> g_pe_ctrl = 0; 
static std::atomic<bool> g_pe_token_pending = false;
static std::atomic<bool> g_pe_finish_pending = false;

std::atomic<uint32_t> g_pe_sr = 0;
std::atomic<uint32_t> g_pe_token = 0;

static void pe_update_interrupts() {
    g_pe_sr = (g_pe_token_pending ? 0x04u : 0u) | (g_pe_finish_pending ? 0x08u : 0u);
    if (g_pe_token_pending && (g_pe_ctrl & 0x01))
        trigger_pi_interrupt(0x200); 
    else
        clear_pi_interrupt(0x200);
    if (g_pe_finish_pending && (g_pe_ctrl & 0x02))
        trigger_pi_interrupt(0x400); 
    else
        clear_pi_interrupt(0x400);
}

void register_pe(MMIODispatcher &dispatcher) {
    dispatcher.register_region(0xCC001000, 0xCC0010FF,
        [](uint32_t addr) -> uint32_t {
            uint32_t pe_sr = (g_pe_ctrl.load() & 0x03) | g_pe_sr.load();
            if (addr == 0xCC00100A || addr == 0xCC001008)
                return pe_sr;
            if (addr == 0xCC00100E || addr == 0xCC00100C)
                return g_pe_token & 0xFFFF;
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            if (addr == 0xCC00100A || addr == 0xCC001008) {
                uint32_t v = (addr == 0xCC001008 && val > 0xFFFF) ? (val & 0xFFFF) : val;
                if (std::getenv("NWII_SAMPLE")) {
                    static int n = 0;
                    if (n++ < 12)
                        if (std::getenv("NWII_DEBUG")) std::cout << "[PE] ctrl write @" << std::hex << addr
                                  << " val=0x" << val << std::dec << "\n";
                }
                if (v & 0x04) g_pe_token_pending = false;
                if (v & 0x08) g_pe_finish_pending = false;
                g_pe_ctrl = v & 0x03;
                pe_update_interrupts();
            }
        },
        [](uint32_t addr) -> uint16_t {
            uint32_t pe_sr = (g_pe_ctrl.load() & 0x03) | g_pe_sr.load();
            if (addr == 0xCC00100A) return (uint16_t)pe_sr;
            if (addr == 0xCC00100E) return (uint16_t)(g_pe_token & 0xFFFF);
            return 0;
        },
        [](uint32_t addr, uint16_t val) {
            if (addr == 0xCC00100A) {
                if (val & 0x04) g_pe_token_pending = false;
                if (val & 0x08) g_pe_finish_pending = false;
                g_pe_ctrl = val & 0x03;
                pe_update_interrupts();
            }
        }
    );
}

void pe_signal_token(uint32_t token, bool raise_interrupt) {
    static int n = 0;
    if (n++ < 6)
        if (std::getenv("NWII_DEBUG")) std::cout << "[PE] token 0x" << std::hex << token << (raise_interrupt ? " +int" : "") << std::dec << "\n";
    g_pe_token = token;
    if (raise_interrupt) {
        g_pe_token_pending = true;
        pe_update_interrupts();
    }
}

void pe_signal_finish() {
    static int n = 0;
    if (n++ < 6)
        if (std::getenv("NWII_DEBUG")) std::cout << "[PE] finish\n";
    g_pe_finish_pending = true;
    pe_update_interrupts();
}

} // namespace nwii::runtime::hw
