#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>

namespace nwii::runtime {
    extern CPUContext* g_ctx_ptr;
    extern MMU* g_mmu;
    extern int g_ipc_interrupt_delay;
}

namespace nwii::runtime::hw {

static uint32_t ipc_arm_msg = 0;
static uint32_t ipc_arm_ctrl = 0;
static uint32_t ipc_ppc_ctrl = 0;
static uint32_t ipc_irq_flags = 0;
static uint32_t ipc_irq_mask = 0x40000000;

static void ipc_update_interrupt() {
    if (((ipc_ppc_ctrl & 4) && (ipc_ppc_ctrl & 0x10)) ||
        ((ipc_ppc_ctrl & 2) && (ipc_ppc_ctrl & 0x20)))
        ipc_irq_flags |= 0x40000000;
    if (ipc_irq_flags & ipc_irq_mask) trigger_pi_interrupt(0x4000);
    else clear_pi_interrupt(0x4000);
}
uint32_t ipc_ppc_msg = 0;

static std::deque<uint32_t> g_ipc_reply_queue;

void check_deliver_queued_reply() {
    if (!(ipc_ppc_ctrl & 6) && !ipc_irq_flags && nwii::runtime::g_ipc_interrupt_delay == 0 && !g_ipc_reply_queue.empty()) {
        ipc_arm_msg = g_ipc_reply_queue.front();
        g_ipc_reply_queue.pop_front();
        ipc_arm_ctrl = 0x00000003;
        ipc_ppc_ctrl &= ~0x01;
        ipc_ppc_ctrl |= 0x06;
        nwii::runtime::g_ipc_interrupt_delay = 5;
    }
}

static void push_reply(uint32_t req_addr) {
    uint32_t msg = req_addr & 0x1FFFFFFF;
    if (g_ipc_reply_queue.empty() && !(ipc_ppc_ctrl & 6) && !ipc_irq_flags && nwii::runtime::g_ipc_interrupt_delay == 0) {
        ipc_arm_msg = msg;
        ipc_arm_ctrl = 0x00000003;
        ipc_ppc_ctrl &= ~0x01;
        ipc_ppc_ctrl |= 0x06;
        nwii::runtime::g_ipc_interrupt_delay = 50;
    } else {
        g_ipc_reply_queue.push_back(msg);
    }
}

void ipc_tick() {
    if (nwii::runtime::g_ipc_interrupt_delay > 0) {
        nwii::runtime::g_ipc_interrupt_delay--;
        if (nwii::runtime::g_ipc_interrupt_delay == 0) {
            ipc_update_interrupt();
        }
        return;
    }
    check_deliver_queued_reply();
}

extern "C" int32_t handle_ios_ipc(nwii::runtime::CPUContext& ctx, uint32_t request_addr);

int32_t dispatch_ipc(CPUContext& ctx, uint32_t virt_addr) {
  int32_t result = handle_ios_ipc(ctx, virt_addr);

  if (result != -0x70000001) {
    push_reply(virt_addr);
  }
  
  return result;
}

int32_t ipc_dispatch_request(CPUContext &ctx, uint32_t req_addr) {
  uint32_t virt_addr = req_addr;
  if ((virt_addr & 0xF0000000) == 0xC0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x80000000;
  if ((virt_addr & 0xF0000000) == 0xD0000000) virt_addr = (virt_addr & 0x0FFFFFFF) | 0x90000000;

  return dispatch_ipc(ctx, virt_addr);
}

void hle_set_ipc_arm_msg(uint32_t req_addr) {
  push_reply(req_addr);
}

void ipc_post_reply(uint32_t req_addr) {
  push_reply(req_addr);
}

void register_ipc(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCD000000, 0xCD00003F,
        [](uint32_t addr) -> uint32_t {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: return ipc_ppc_msg;
            case 0x000004: return ipc_ppc_ctrl;
            case 0x000008: return ipc_arm_msg;
            case 0x00000C: return ipc_arm_ctrl;
            case 0x000030: return ipc_irq_flags;
            case 0x000034: return ipc_irq_mask;
            default: return 0;
            }
        },
        [](uint32_t addr, uint32_t val) {
            switch (addr & 0x00FFFFFF) {
            case 0x000000: {
                ipc_ppc_msg = val;
                break;
            }
            case 0x000004: {
                uint32_t old_ctrl = ipc_ppc_ctrl;

                ipc_ppc_ctrl = (ipc_ppc_ctrl & ~0x30) | (val & 0x30);

                if (val & 0x01) {
                    ipc_ppc_ctrl |= 0x01;
                    int32_t result = 0;
                    if (g_ctx_ptr) {
                        result = ipc_dispatch_request(*g_ctx_ptr, ipc_ppc_msg);
                    }
                    if (([](){ static const bool s = (std::getenv("NWII_DEBUG") != nullptr); return s; })()) std::cout << "[IPC] Request sent from PPC! result=" << result << "\n";
                    ipc_ppc_ctrl &= ~0x01;
                    if (result != -0x70000001) { 
                        ipc_ppc_ctrl |= 0x06;
                    } else {
                        ipc_ppc_ctrl |= 0x02;
                    }
                }

                if (val & 0x04) ipc_ppc_ctrl &= ~0x04; 
                if (val & 0x02) ipc_ppc_ctrl &= ~0x02; 
                if (val & 0x08) ipc_ppc_ctrl &= ~0x08; 
                (void)old_ctrl;
                ipc_update_interrupt();
                break;
            }

            case 0x000030:
                ipc_irq_flags &= ~val;
                ipc_update_interrupt();
                check_deliver_queued_reply();
                break;
            case 0x000034:
                ipc_irq_mask = val;
                ipc_update_interrupt();
                break;
            case 0x00000C: {
                ipc_arm_ctrl &= ~(val & 0x03);
                if (!(ipc_arm_ctrl & 3)) {
                    if (([](){ static const bool s = (std::getenv("NWII_SAMPLE") != nullptr); return s; })() && (pi_intsr & 0x4000)) {
                        static int n = 0;
                        if (n++ < 6)
                            std::cout << "[IPC] armctrl write 0x" << std::hex
                                      << val << " swallows pending irq\n" << std::dec;
                    }
                    clear_pi_interrupt(0x00004000);
                    check_deliver_queued_reply();
                }
                break;
            }
            } 
        }
    );
    dispatcher.register_region(0x0D000000, 0x0D00003F,
        [&dispatcher](uint32_t a) { return dispatcher.read32(a | 0xC0000000); },
        [&dispatcher](uint32_t a, uint32_t v) { dispatcher.write32(a | 0xC0000000, v); }
    );
}}

} 
