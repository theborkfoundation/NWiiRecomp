#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"

#include <cstdlib>

namespace nwii::runtime {
CPUContext *g_ctx_ptr = nullptr;
MMU *g_mmu = nullptr;
uint32_t g_debug_pc = 0;
}
extern "C" void run_game(nwii::runtime::CPUContext &) { std::abort(); }

static void require(bool value) {
  if (!value) std::abort();
}

int main() {
  using namespace nwii::runtime;
  Config::get().game_id = "SOJE41";
  CPUContext ctx;
  constexpr uint32_t thread = 0x80010000;
  constexpr uint32_t resume = 0x80012340;
  constexpr uint32_t stack = 0x80020000;
  constexpr uint32_t link = 0x80056780;
  constexpr uint32_t sentinel = 0xfffffffc;
  ctx.mmu.write32(0x800000d4, thread);
  ctx.mmu.write32(0x800000e4, thread);
  ctx.mmu.write32(thread + 4, stack);
  ctx.mmu.write32(thread + 12, 0x12345678);
  ctx.mmu.write32(thread + 0x84, link);
  ctx.mmu.write32(thread + 0x198, resume);
  ctx.mmu.write32(thread + 0x19c, 0x8000);
  ctx.mmu.write16(thread + 0x1a2, 2);
  ctx.pc = sentinel;
  ctx.lr = sentinel;
  ctx.srr0 = 0x80099990; // A previous exception, not this thread's resume PC.
  ctx.dispatch_saved_ctx = thread;
  ctx.ext_resched_pending = true;

  // Restore the interrupted thread before entering the scheduler.
  require(process_pending_callbacks(ctx));
  require(ctx.pc == resume && ctx.lr == link && ctx.gpr[1] == stack);
  require(ctx.gpr[3] == 0x12345678 && ctx.ext_resched_pending);
  require(ctx.dispatch_saved_ctx == 0);

  // The scheduler receives a complete saved context of the real guest state.
  require(process_pending_callbacks(ctx));
  require(ctx.pc == 0x8009dd90 && ctx.lr == sentinel);
  require(ctx.dispatch_saved_ctx == thread && !ctx.ext_resched_pending);
  require(ctx.mmu.read32(thread + 0x198) == resume);
  require(ctx.mmu.read32(thread + 0x84) == link);
  require(ctx.mmu.read32(thread + 12) == 0x12345678);
  require((ctx.mmu.read16(thread + 0x1a2) & 2) != 0);

  // If no thread switch occurs, scheduler return restores the real state.
  ctx.pc = sentinel;
  require(process_pending_callbacks(ctx));
  require(ctx.pc == resume && ctx.lr == link && ctx.gpr[1] == stack);
  require(ctx.gpr[3] == 0x12345678 && ctx.dispatch_saved_ctx == 0);

  // A running SOJE41 thread outranks priority 12. Complete the request
  // without entering the scheduler, then honor a fresh higher-priority wakeup.
  ctx.gpr[13] = 0x80040000;
  ctx.mmu.write16(thread + 0x2c8, 2);
  ctx.mmu.write32(thread + 0x2d0, 8);
  ctx.mmu.write32(ctx.gpr[13] - 24944, 0x80000000u >> 12);
  ctx.ext_resched_pending = true;
  require(!process_pending_callbacks(ctx));
  require(!ctx.ext_resched_pending && ctx.pc == resume && !ctx.dispatch_saved_ctx);
  ctx.mmu.write32(ctx.gpr[13] - 24944, 0x80000000u >> 4);
  ctx.ext_resched_pending = true;
  require(process_pending_callbacks(ctx));
  require(!ctx.ext_resched_pending && ctx.pc == 0x8009dd90);
  ctx.pc = sentinel;
  require(process_pending_callbacks(ctx));
  require(ctx.pc == resume && !ctx.dispatch_saved_ctx);

  // The guest scheduler can call a hook with interrupts disabled while its
  // current context still names the thread it is switching to. Rescheduling
  // here would save the hook into that context and make it loop forever.
  ctx.pc = 0x80018180;
  ctx.msr = 0x32;
  ctx.ext_resched_pending = true;
  require(!process_pending_callbacks(ctx));
  require(ctx.pc == 0x80018180 && ctx.ext_resched_pending);
  require(ctx.dispatch_saved_ctx == 0);
  require(ctx.mmu.read32(thread + 0x198) == resume);

  // An idle PC must be resolved before it can be saved for rescheduling.
  ctx.pc = 0;
  ctx.ext_resched_pending = true;
  require(process_pending_callbacks(ctx));
  require(ctx.pc == resume && ctx.ext_resched_pending);

  // A callback return belongs to the generated dispatcher's backup stack.
  ctx.pc = sentinel;
  ctx.in_callback = true;
  ctx.callback_depth = 1;
  require(!process_pending_callbacks(ctx));
  require(ctx.pc == sentinel && ctx.in_callback && ctx.callback_depth == 1);

  // A missing current context must not be replaced with a sentinel save.
  ctx.in_callback = false;
  ctx.mmu.write32(0x800000d4, 0);
  require(!process_pending_callbacks(ctx));
  require(ctx.pc == sentinel && ctx.ext_resched_pending);

  // A normal branch can return early only while no work is pending. A
  // vblank must still reach the full poll even inside the throttle window.
  ctx.pc = resume;
  ctx.ext_resched_pending = false;
  ctx.inst_count = 1;
  ctx.dec_irq_pending = false;
  ctx.vblank_pending = false;
  hw::pi_intsr = 0;
  require(!process_pending_callbacks(ctx));
  ctx.vblank_pending = true;
  require(!process_pending_callbacks(ctx));
  require(!ctx.vblank_pending);

  return 0;
}
