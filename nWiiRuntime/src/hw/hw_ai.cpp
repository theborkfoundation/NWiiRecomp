#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace nwii::runtime {
    extern uint64_t get_os_time();
}

namespace nwii::runtime::hw {

static uint32_t ai_cr = 0;
static uint32_t ai_vr = 0;
static uint32_t ai_scnt = 0;
static uint32_t ai_it = 0;
static uint64_t ai_start_time = 0;

void register_ai(MMIODispatcher& dispatcher) {
    auto read_cb = [](uint32_t addr) -> uint32_t {
        uint32_t cur_scnt = ai_scnt;
        if (ai_cr & 0x01) { // Bit 0: PSTAT
            uint64_t current = nwii::runtime::get_os_time();
            uint32_t samples = ((current - ai_start_time) * 48) / 1000;
            cur_scnt = ai_scnt + samples;
            if (ai_it > 0 && cur_scnt >= ai_it) {
                ai_cr |= 0x08; // Bit 3: AIINT
                if (ai_cr & 0x04) trigger_pi_interrupt(0x20); // Bit 2: AIINTMSK
            }
        }
        uint32_t reg = addr & 0xFF;
        if (reg == 0x00) return ai_cr;
        if (reg == 0x04) return ai_vr;
        if (reg == 0x08) return cur_scnt;
        if (reg == 0x0C) return ai_it;
        return 0;
    };

    auto write_cb = [](uint32_t addr, uint32_t val) {
        uint32_t reg = addr & 0xFF;
        if (reg == 0x00) {
            if (val & 0x08) { ai_cr &= ~0x08; clear_pi_interrupt(0x20); } // clear AIINT
            if (val & 0x20) { ai_scnt = 0; ai_start_time = nwii::runtime::get_os_time(); } // SCRESET
            if ((val & 0x01) && !(ai_cr & 0x01)) { ai_start_time = nwii::runtime::get_os_time(); } // PSTAT start
            ai_cr = (ai_cr & 0x08) | (val & ~0x08);
        } else if (reg == 0x04) {
            ai_vr = val;
        } else if (reg == 0x08) {
            ai_scnt = val;
            ai_start_time = nwii::runtime::get_os_time();
        } else if (reg == 0x0C) {
            ai_it = val;
        }
    };

    dispatcher.register_region(0xCC006C00, 0xCC006CFF, read_cb, write_cb);
    dispatcher.register_region(0xCD006C00, 0xCD006CFF, read_cb, write_cb);
    dispatcher.register_region(0x0D006C00, 0x0D006CFF, read_cb, write_cb);
}

void ai_update() {
    if (ai_cr & 0x01) {
        uint64_t current = nwii::runtime::get_os_time();
        uint32_t samples = ((current - ai_start_time) * 48) / 1000;
        uint32_t cur_scnt = ai_scnt + samples;
        if (ai_it > 0 && cur_scnt >= ai_it) {
            ai_cr |= 0x08;
            if (ai_cr & 0x04) trigger_pi_interrupt(0x20);
        }
    }
}

} // namespace nwii::runtime::hw
