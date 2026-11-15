// Executes translator output from recompiler_resume_generator.cpp and checks
// that entering a function at an interior PC resumes at that instruction, for
// both the dense offset-table dispatch and the sparse switch fallback.
#include <cstdint>
#include <cstdio>

#include "runtime/cpu_context.h"

namespace nwii::runtime {
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;
} // namespace nwii::runtime

using nwii::runtime::CPUContext;

#include "recompiler_resume.inc"

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void check_state(CPUContext &ctx, uint32_t r3, uint32_t r4, uint32_t r5,
                        uint32_t r6, uint64_t inst_count) {
  CHECK(ctx.gpr[3] == r3);
  CHECK(ctx.gpr[4] == r4);
  CHECK(ctx.gpr[5] == r5);
  CHECK(ctx.gpr[6] == r6);
  CHECK(ctx.pc == 0x1234); // blr returns through lr
  CHECK(ctx.inst_count == inst_count);
}

int main() {
  {
    // Entry at the function start runs every instruction.
    CPUContext ctx;
    ctx.pc = 0x80100000;
    ctx.lr = 0x1234;
    func_80100000(ctx);
    check_state(ctx, 1, 2, 3, 4, 5);
  }
  {
    // Interior resume: skip the first two loads.
    CPUContext ctx;
    ctx.pc = 0x80100008;
    ctx.lr = 0x1234;
    func_80100000(ctx);
    check_state(ctx, 0, 0, 3, 4, 3);
  }
  {
    // Idle PC returns without running the body.
    CPUContext ctx;
    ctx.pc = 0;
    ctx.lr = 0x1234;
    func_80100000(ctx);
    CHECK(ctx.gpr[3] == 0);
    CHECK(ctx.pc == 0);
    CHECK(ctx.inst_count == 0);
  }
  {
    // Sparse function, entry at start.
    CPUContext ctx;
    ctx.pc = 0x80200000;
    ctx.lr = 0x1234;
    func_80200000(ctx);
    check_state(ctx, 1, 2, 0, 0, 3);
  }
  {
    // Sparse function, interior resume across the address gap.
    CPUContext ctx;
    ctx.pc = 0x80210000;
    ctx.lr = 0x1234;
    func_80200000(ctx);
    check_state(ctx, 0, 2, 0, 0, 2);
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("recompiler_resume: all checks passed\n");
  return 0;
}
