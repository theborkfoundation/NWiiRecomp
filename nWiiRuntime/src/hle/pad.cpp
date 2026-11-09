#include "runtime/cpu_context.h"
#include "input/input_manager.h"
#include <iostream>

using namespace nwii::runtime;
using namespace nwii::runtime::input;

extern "C" {

void PADInit(CPUContext& ctx) {}
void WPADInit(CPUContext& ctx) {}
void KPADInit(CPUContext& ctx) {}

void PADRead(CPUContext& ctx) {
    static int n = 0; if (n++ < 5) std::cout << "[PADRead] called" << std::endl;
    uint32_t pad_status_array_addr = ctx.gpr[3];
    GameCubePadState state = InputManager::get().get_gcpad_state(0);

    if (pad_status_array_addr >= 0x80000000 && pad_status_array_addr < 0x81800000) {
        ctx.mmu.write16(pad_status_array_addr + 0, state.buttons);
        ctx.mmu.write8(pad_status_array_addr + 2, state.stick_x);
        ctx.mmu.write8(pad_status_array_addr + 3, state.stick_y);
        ctx.mmu.write8(pad_status_array_addr + 4, state.substick_x);
        ctx.mmu.write8(pad_status_array_addr + 5, state.substick_y);
        ctx.mmu.write8(pad_status_array_addr + 6, state.trigger_l);
        ctx.mmu.write8(pad_status_array_addr + 7, state.trigger_r);
        ctx.mmu.write8(pad_status_array_addr + 8, state.analog_a);
        ctx.mmu.write8(pad_status_array_addr + 9, state.analog_b);
        ctx.mmu.write8(pad_status_array_addr + 10, state.err);
        ctx.gpr[3] = 1; 
    } else {
        ctx.gpr[3] = 0;
    }
    ctx.pc = ctx.lr;
}

void WPADRead(CPUContext& ctx) {
    static int n = 0; if (n++ < 5) std::cout << "[WPADRead] called chan=" << ctx.gpr[3] << std::endl;
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_ptr = ctx.gpr[4];

    if (chan == 0 && data_ptr >= 0x80000000 && data_ptr < 0x81800000) {
        WiimoteState state = InputManager::get().get_wiimote_state(0);
        ctx.mmu.write16(data_ptr + 0, (uint16_t)state.buttons);
        ctx.mmu.write8(data_ptr + 2, (uint8_t)state.err);
    }
    ctx.pc = ctx.lr;
}

void KPADRead(CPUContext& ctx) {
    static int n = 0; if (n++ < 5) std::cout << "[KPADRead] called chan=" << ctx.gpr[3] << std::endl;
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_array_ptr = ctx.gpr[4];
    uint32_t length = ctx.gpr[5];

    if (chan == 0 && data_array_ptr >= 0x80000000 && length > 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(0);
        
        static uint32_t prev_buttons = 0;
        uint32_t hold = state.buttons;
        uint32_t trigger = hold & ~prev_buttons;
        uint32_t release = ~hold & prev_buttons;
        prev_buttons = hold;

        ctx.mmu.write32(data_array_ptr + 0x00, hold);
        ctx.mmu.write32(data_array_ptr + 0x04, trigger);
        ctx.mmu.write32(data_array_ptr + 0x08, release);

        int32_t acc_x = 0;
        int32_t acc_y = 0;
        int32_t acc_z = 0x40000000; 

        ctx.mmu.write32(data_array_ptr + 0x0C, acc_x);
        ctx.mmu.write32(data_array_ptr + 0x10, acc_y);
        ctx.mmu.write32(data_array_ptr + 0x14, acc_z);

        float dp_x = state.ir_x * 2.0f - 1.0f;
        float dp_y = state.ir_y * 2.0f - 1.0f;
        ctx.mmu.write32(data_array_ptr + 0x18, *(uint32_t*)&dp_x);
        ctx.mmu.write32(data_array_ptr + 0x1C, *(uint32_t*)&dp_y);

        ctx.mmu.write8(data_array_ptr + 0x28, (uint8_t)state.err);

        ctx.gpr[3] = 1; 
    } else {
        ctx.gpr[3] = 0;
    }
    ctx.pc = ctx.lr;
} 

} // extern "C"

void hle_wpad_probe(CPUContext& ctx) {
    static int n = 0; if (n++ < 5) std::cout << "[hle_wpad_probe] called chan=" << ctx.gpr[3] << std::endl;
    uint32_t chan = ctx.gpr[3];
    uint32_t dev_type_ptr = ctx.gpr[4];

    // Ensure guest WPADChannel 0 is marked connected
    uint32_t ch0 = ctx.mmu.read32(0x806E79B0);
    if (ch0 >= 0x80000000 && ch0 < 0x81800000) {
        ctx.mmu.write32(ch0 + 2304, 0); // WPAD_ERR_NONE
        ctx.mmu.write8(ch0 + 2309, 0);  // WPAD_DEV_CORE
        ctx.mmu.write32(ch0 + 2336, 1); // connected
        ctx.mmu.write32(ch0 + 2300, 42); // non-zero state
    }

    if (chan == 0) {
        if (dev_type_ptr >= 0x80000000 && dev_type_ptr < 0x81800000) {
            ctx.mmu.write32(dev_type_ptr, 0); // WPAD_DEV_CORE
        }
        ctx.gpr[3] = 0; // WPAD_ERR_NONE
    } else {
        ctx.gpr[3] = (uint32_t)-1; // WPAD_ERR_NO_CONTROLLER
    }
    ctx.pc = ctx.lr;
}

void hle_kpad_read_ex(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_array_ptr = ctx.gpr[4];
    uint32_t length = ctx.gpr[5];
    uint32_t p_result = ctx.gpr[6];

    static uint64_t s_kpad_reads = 0;
    ++s_kpad_reads;

    // Enable channel 0 in Rayman Origins controller manager (0x8071C068)
    if (ctx.mmu.read8(0x8071C068) == 0) {
        ctx.mmu.write8(0x8071C068, 1);
    }

    // Keep guest WPADChannel 0 updated
    uint32_t ch0 = ctx.mmu.read32(0x806E79B0);
    if (ch0 >= 0x80000000 && ch0 < 0x81800000) {
        ctx.mmu.write32(ch0 + 2304, 0);
        ctx.mmu.write8(ch0 + 2309, 0);
        ctx.mmu.write32(ch0 + 2336, 1);
        ctx.mmu.write32(ch0 + 2300, 42);
    }

    if (chan == 0 && data_array_ptr >= 0x80000000 && length > 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(0);
        uint32_t buttons = state.buttons;

        // Auto-advance past strap screen if unattended/automated test (brief pulse at ~4 seconds)
        if (s_kpad_reads >= 240 && s_kpad_reads <= 245) {
            buttons |= 0x0001;
        }

        static uint32_t prev_buttons = 0;
        uint32_t hold    = buttons;
        uint32_t trigger = hold & ~prev_buttons;
        uint32_t release = ~hold & prev_buttons;
        prev_buttons = hold;

        ctx.mmu.write32(data_array_ptr + 0x00, hold);
        ctx.mmu.write32(data_array_ptr + 0x04, trigger);
        ctx.mmu.write32(data_array_ptr + 0x08, release);

        // Fill remaining fields (acceleration, pointer, etc.)
        for (uint32_t b = 0x0C; b < 240; b += 4) {
            ctx.mmu.write32(data_array_ptr + b, 0);
        }

        if (ch0 >= 0x80000000 && ch0 < 0x81800000) {
            ctx.mmu.write16(ch0 + 160 + 0, (uint16_t)hold);
            ctx.mmu.write8(ch0 + 160 + 40, 0);
            ctx.mmu.write8(ch0 + 160 + 41, 0);
            ctx.mmu.write16(ch0 + 256 + 0, (uint16_t)hold);
            ctx.mmu.write8(ch0 + 256 + 40, 0);
            ctx.mmu.write8(ch0 + 256 + 41, 0);
            ctx.mmu.write8(ch0 + 2316, 0);
        }

        if (p_result >= 0x80000000 && p_result < 0x81800000) {
            ctx.mmu.write32(p_result, 0);
        }

        if (hold != 0 && (s_kpad_reads % 60) == 1) {
            std::cout << "[KPAD] ReadEx chan=" << chan << " hold=0x" << std::hex << hold
                      << " trig=0x" << trigger << " reads=" << std::dec << s_kpad_reads << std::endl;
        }

        ctx.gpr[3] = 1; // 1 sample read
    } else {
        if (p_result >= 0x80000000 && p_result < 0x81800000) {
            ctx.mmu.write32(p_result, (uint32_t)-1);
        }
        ctx.gpr[3] = 0;
    }
    ctx.pc = ctx.lr;
}
