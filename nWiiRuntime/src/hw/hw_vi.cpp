#include "runtime/hw/hw.h"
#include <chrono>
#include <cstdlib>
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace nwii::runtime::hw {

static uint32_t vi_dcr = 0;

uint32_t g_vi_top_field_base = 0;
uint32_t g_vi_btm_field_base = 0;
uint64_t g_vi_trigger_count = 0;

// bit15 of HI), bit28 = enable. Layout per Dolphin VideoInterface.h:
static uint32_t vi_di[4] = {0, 0, 0, 0};

static void vi_update_interrupt() {
    for (int i = 0; i < 4; ++i) {
        if (vi_di[i] & 0x80000000) {
            pi_intmr |= 0x00000100;
            trigger_pi_interrupt(0x00000100);
            return;
        }
    }
    clear_pi_interrupt(0x00000100);
}

void vi_trigger_interrupt() {
    // The VI fires once per field (~59.94 Hz). Several HLE paths can request a
    // retrace (VIWaitForRetrace, vblank callbacks, DSP mailboxes); without a
    // gate the guest sees 1.5-2x more retraces than frames are presented, so
    // every frame-based timer in the game runs fast. Coalesce to one per field.
    static const bool s_nogate =
        std::getenv("NWII_NO_VI_GATE") != nullptr;
    if (!s_nogate) {
        static auto next = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        next = now + std::chrono::microseconds(16683);
    }
    ++g_vi_trigger_count;
    static const bool s_vitrace = (std::getenv("NWII_VITRACE") != nullptr);
    if (s_vitrace) {
        static int n = 0;
        if (n++ < 40) std::cout << "[VItrig] #" << std::dec << n << "\n";
    }

    vi_di[0] |= 0x80000000;
    vi_di[1] |= 0x80000000;
    // VI = PI_INTSR bit 8 = 0x00000100 (Dolphin ProcessorInterface)
    pi_intmr |= 0x00000100;
    trigger_pi_interrupt(0x00000100);
}

void register_vi(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCC002000, 0xCC0020FF,
        [](uint32_t addr) -> uint32_t {
            if (addr == 0xCC00202C)
                return (uint32_t)((nwii::runtime::get_os_time() / 64) % 313) + 1;
            
            if (addr == 0xCC00202E) return 1;

            if (addr == 0xCC002002) return vi_dcr | 0x0001;
            
            if (addr == 0xCC00201C) return g_vi_top_field_base;
            if (addr == 0xCC002024) return g_vi_btm_field_base;

            if (addr >= 0xCC002030 && addr <= 0xCC00203C && ((addr - 0xCC002030) % 4 == 0)) {
                return vi_di[(addr - 0xCC002030) / 4] >> 16;
            }
            
            if (addr >= 0xCC002032 && addr <= 0xCC00203E && ((addr - 0xCC002032) % 4 == 0)) {
                return vi_di[(addr - 0xCC002032) / 4] & 0xFFFF;
            }
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            if (addr == 0xCC002002) {
                vi_dcr = val;
            }
            else if (addr == 0xCC00201C) {
                g_vi_top_field_base = val;
            }
            else if (addr == 0xCC002024) {
                g_vi_btm_field_base = val;
            }
            else if (addr >= 0xCC002030 && addr <= 0xCC00203C && ((addr - 0xCC002030) % 4 == 0)) {
                int idx = (addr - 0xCC002030) / 4;

                if ((val & 0x8000) == 0) {
                    vi_di[idx] &= ~0x80000000;
                }
                vi_di[idx] = (vi_di[idx] & 0x8000FFFF) | ((val & ~0x8000u) << 16);
                vi_update_interrupt();
            }
            else if (addr >= 0xCC002032 && addr <= 0xCC00203E && ((addr - 0xCC002032) % 4 == 0)) {
                int idx = (addr - 0xCC002032) / 4;
                vi_di[idx] = (vi_di[idx] & 0xFFFF0000) | (val & 0xFFFF);
            }
        }
    );
}}

} 
