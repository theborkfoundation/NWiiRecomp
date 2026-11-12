#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include <cstdlib>

namespace nwii::runtime {
CPUContext* g_ctx_ptr = nullptr;
MMU* g_mmu = nullptr;
int g_ipc_interrupt_delay = 0;
}
extern "C" int32_t handle_ios_ipc(nwii::runtime::CPUContext&, uint32_t) {
    std::abort(); // This test drives replies, never dispatches IOS requests.
}

static void require(bool ok) { if (!ok) std::abort(); }
int main() {
    using namespace nwii::runtime;
    using namespace nwii::runtime::hw;
    auto& mmio = MMIODispatcher::get();
    register_ipc(mmio);
    constexpr uint32_t base = 0xCD000000, ipc = 0x4000, flag = 0x40000000;
    pi_intsr = 0;
    // Enable reply and acknowledgement interrupts, then queue two replies.
    mmio.write32(base + 4, 0x30);
    ipc_post_reply(0x80001000);
    ipc_post_reply(0x80002000);
    for (int i = 0; i < 50; ++i) ipc_tick();
    require(pi_intsr == ipc); // IPC must not assert EXI (0x10) or CP (0x800).
    require(mmio.read32(base + 8) == 0x1000);
    require(mmio.read32(base + 0x30) == flag);
    // Acknowledging the PI latch while a reply is pending must reassert it.
    mmio.write32(base + 0x30, flag);
    require(pi_intsr == ipc);
    // Clear Y1/Y2 first, then acknowledge the latch through the physical alias.
    mmio.write32(base + 4, 0x36);
    require(pi_intsr == ipc);
    mmio.write32(0x0D000030, flag);
    require(pi_intsr == 0);
    for (int i = 0; i < 5; ++i) ipc_tick();
    require(pi_intsr == ipc);
    require(mmio.read32(base + 8) == 0x2000);
    mmio.write32(base + 4, 0x36);
    mmio.write32(base + 0x30, flag);
    require(pi_intsr == 0);
    // Masking preserves a latched cause; unmasking must deliver it again.
    mmio.write32(base + 0x34, 0);
    ipc_post_reply(0x80003000);
    for (int i = 0; i < 50; ++i) ipc_tick();
    require(pi_intsr == 0 && mmio.read32(base + 0x30) == flag);
    mmio.write32(base + 0x34, flag);
    require(pi_intsr == ipc);
    return 0;
}
