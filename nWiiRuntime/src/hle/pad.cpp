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
    uint32_t pad_status_array_addr = ctx.gpr[3];
    GameCubePadState state = InputManager::get().get_gcpad_state(0);

    ctx.mmu.write16(pad_status_array_addr, state.buttons);
    ctx.mmu.write8(pad_status_array_addr + 2, (uint8_t)state.stick_x);
    ctx.mmu.write8(pad_status_array_addr + 3, (uint8_t)state.stick_y);
    ctx.mmu.write8(pad_status_array_addr + 4, (uint8_t)state.substick_x);
    ctx.mmu.write8(pad_status_array_addr + 5, (uint8_t)state.substick_y);
    ctx.mmu.write8(pad_status_array_addr + 6, state.trigger_l);
    ctx.mmu.write8(pad_status_array_addr + 7, state.trigger_r);
    ctx.mmu.write8(pad_status_array_addr + 8, state.buttons & 0x0100 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 9, state.buttons & 0x0200 ? 0xFF : 0);
    ctx.mmu.write8(pad_status_array_addr + 10, state.err);
    ctx.mmu.write8(pad_status_array_addr + 11, 0);

    for (int i = 1; i < 4; i++) {
        uint32_t offset = pad_status_array_addr + (i * 12);
        ctx.mmu.write16(offset, 0);
        ctx.mmu.write8(offset + 10, (uint8_t)-1); 
    }
}

void WPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_ptr = ctx.gpr[4];

    if (chan >= 0 && chan < 4 && data_ptr != 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);
        ctx.mmu.write32(data_ptr + 0x00, state.err);
        ctx.mmu.write32(data_ptr + 0x04, state.buttons);
    }
    ctx.gpr[3] = 0; 
}

void KPADRead(CPUContext& ctx) {
    int32_t chan = (int32_t)ctx.gpr[3];
    uint32_t data_array_ptr = ctx.gpr[4];
    uint32_t length = ctx.gpr[5];

    int read_count = 0;
    if (chan >= 0 && chan < 4 && data_array_ptr != 0 && length > 0) {
        WiimoteState state = InputManager::get().get_wiimote_state(chan);

        if (state.err == 0) {
            static uint32_t prev_buttons[4] = {};
            uint32_t hold    = state.buttons;
            uint32_t trigger = hold & ~prev_buttons[chan];  
            uint32_t release = ~hold & prev_buttons[chan];  
            prev_buttons[chan] = hold;

            ctx.mmu.write32(data_array_ptr + 0x00, hold);
            ctx.mmu.write32(data_array_ptr + 0x04, trigger);
            ctx.mmu.write32(data_array_ptr + 0x08, release);
            
            ctx.mmu.write32(data_array_ptr + 0x0C, 0);
            ctx.mmu.write32(data_array_ptr + 0x10, 0);
            ctx.mmu.write32(data_array_ptr + 0x14, 0);
            
            for (int b = 0x18; b < 0x48; b += 4)
                ctx.mmu.write32(data_array_ptr + b, 0);
            
            ctx.mmu.write32(data_array_ptr + 0x48, 0);
            
            ctx.mmu.write32(data_array_ptr + 0x4C, 0);

            read_count = 1;
        }
    }
    ctx.gpr[3] = read_count;
}

} 

void hle_wpad_probe(CPUContext& ctx) {
    uint32_t chan = ctx.gpr[3];
    uint32_t dev_type_ptr = ctx.gpr[4];

    // Ensure guest WPADChannel 0 is marked connected
    uint32_t ch0 = ctx.mmu.read32(0x806E79B0);
    if (ch0 >= 0x80000000 && ch0 < 0x81800000) {
        ctx.mmu.write32(ch0 + 2304, 0); // WPAD_ERR_NONE
        ctx.mmu.write8(ch0 + 2309, 0);  // WPAD_DEV_CORE
        ctx.mmu.write32(ch0 + 2336, 1); // Handshake complete
        ctx.mmu.write32(ch0 + 2300, 42);// Data format size
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

        // Auto-pulse button 2 (0x0001) and button A (0x0800) periodically:
        // 30 frames pressed, 30 frames released.
        // This advances through strap screen, Ubisoft/UbiArt logos, and menus automatically.
        if (s_kpad_reads > 60 && (s_kpad_reads % 60) < 30) {
            buttons |= 0x0801; // Wiimote 2 | Wiimote A
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

        if ((s_kpad_reads % 120) == 1) {
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
