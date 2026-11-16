// Executes translator output from translator_semantics_generator.cpp.
// Round 5: OE arithmetic must snapshot aliased operands, record-form float
// instructions must update CR1, and indirect branches must clear target bits.
#include <cstdint>
#include <cstdio>

#include "runtime/cpu_context.h"

namespace nwii::runtime {
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;
} // namespace nwii::runtime

using nwii::runtime::CPUContext;

#include "translator_semantics.inc"

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static void reset(CPUContext &ctx) {
  ctx.gpr.fill(0);
  ctx.fpr.fill(0.0);
  ctx.ps1.fill(0.0);
  for (auto &field : ctx.cr)
    field = nwii::runtime::ConditionField{};
  ctx.xer = 0;
  ctx.fpscr = 0;
  ctx.pc = 0;
  ctx.lr = 0;
  ctx.ctr = 0;
  ctx.inst_count = 0;
}

int main() {
  CPUContext ctx;

  // addo. r3,r3,r4 with INT_MAX + 1: the aliased read used to lose the overflow.
  reset(ctx);
  ctx.gpr[3] = 0x7FFFFFFFu;
  ctx.gpr[4] = 1;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020000;
  func_80020000(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK(ctx.pc == 0x1234);
  CHECK((ctx.xer & 0x40000000u) != 0); // OV
  CHECK((ctx.xer & 0x80000000u) != 0); // SO
  CHECK(ctx.cr[0].so);

  // mullwo. r3,r3,r4: 2 * 2^29 fits; the aliased check used to report overflow.
  reset(ctx);
  ctx.gpr[3] = 2;
  ctx.gpr[4] = 0x20000000u;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020010;
  func_80020010(ctx);
  CHECK(ctx.gpr[3] == 0x40000000u);
  CHECK((ctx.xer & 0x40000000u) == 0);
  CHECK((ctx.xer & 0x80000000u) == 0);

  // addco. r3,r3,r4: INT_MAX + 1 overflows, with no carry out.
  reset(ctx);
  ctx.gpr[3] = 0x7FFFFFFFu;
  ctx.gpr[4] = 1;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020020;
  func_80020020(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK((ctx.xer & (1u << 29)) == 0);  // CA
  CHECK((ctx.xer & 0x40000000u) != 0);
  CHECK((ctx.xer & 0x80000000u) != 0);

  // addeo. r3,r3,r4 with carry-in: INT_MAX + 0 + 1 overflows.
  reset(ctx);
  ctx.gpr[3] = 0x7FFFFFFFu;
  ctx.gpr[4] = 0;
  ctx.xer = 1u << 29;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020030;
  func_80020030(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK((ctx.xer & (1u << 29)) == 0);
  CHECK((ctx.xer & 0x40000000u) != 0);

  // subfeo. r3,r3,r4: INT_MIN - 0 - 1 overflows and sets carry out.
  reset(ctx);
  ctx.gpr[3] = 0;
  ctx.gpr[4] = 0x80000000u;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020040;
  func_80020040(ctx);
  CHECK(ctx.gpr[3] == 0x7FFFFFFFu);
  CHECK((ctx.xer & (1u << 29)) != 0); // CA
  CHECK((ctx.xer & 0x40000000u) != 0);

  // addzeo. r3,r3 with carry-in: INT_MAX + 1 overflows.
  reset(ctx);
  ctx.gpr[3] = 0x7FFFFFFFu;
  ctx.xer = 1u << 29;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020050;
  func_80020050(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK((ctx.xer & 0x40000000u) != 0);

  // subfzeo. r3,r3 with carry-in: -INT_MIN - 1 + 1 overflows.
  reset(ctx);
  ctx.gpr[3] = 0x80000000u;
  ctx.xer = 1u << 29;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020060;
  func_80020060(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK((ctx.xer & 0x40000000u) != 0);

  // nego. r3,r3: negating INT_MIN overflows.
  reset(ctx);
  ctx.gpr[3] = 0x80000000u;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020070;
  func_80020070(ctx);
  CHECK(ctx.gpr[3] == 0x80000000u);
  CHECK((ctx.xer & 0x40000000u) != 0);
  CHECK((ctx.xer & 0x80000000u) != 0);

  // fadds. f1,f2,f3: record form copies FX and VX into CR1.
  reset(ctx);
  ctx.fpscr = 0xA0000000u;
  ctx.fpr[2] = 1.0;
  ctx.fpr[3] = 2.0;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020080;
  func_80020080(ctx);
  CHECK(ctx.fpr[1] == 3.0);
  CHECK(ctx.ps1[1] == 3.0);
  CHECK(ctx.cr[1].lt && !ctx.cr[1].gt && ctx.cr[1].eq && !ctx.cr[1].so);

  // fadds without the record bit leaves CR1 alone.
  reset(ctx);
  ctx.fpscr = 0xA0000000u;
  ctx.fpr[2] = 1.0;
  ctx.fpr[3] = 2.0;
  ctx.cr[1].gt = true;
  ctx.cr[1].so = true;
  ctx.lr = 0x1234;
  ctx.pc = 0x80020090;
  func_80020090(ctx);
  CHECK(ctx.fpr[1] == 3.0);
  CHECK(!ctx.cr[1].lt && ctx.cr[1].gt && !ctx.cr[1].eq && ctx.cr[1].so);

  // fadd. f1,f2,f3 also updates CR1 in record form.
  reset(ctx);
  ctx.fpscr = 0xA0000000u;
  ctx.fpr[2] = 1.0;
  ctx.fpr[3] = 2.0;
  ctx.lr = 0x1234;
  ctx.pc = 0x800200A0;
  func_800200A0(ctx);
  CHECK(ctx.fpr[1] == 3.0);
  CHECK(ctx.cr[1].lt && !ctx.cr[1].gt && ctx.cr[1].eq && !ctx.cr[1].so);

  // blr clears the low target bits.
  reset(ctx);
  ctx.lr = 0x80030003u;
  ctx.pc = 0x800200B0;
  func_800200B0(ctx);
  CHECK(ctx.pc == 0x80030000u);

  // blrl captures the old LR before writing the return address.
  reset(ctx);
  ctx.lr = 0x80030007u;
  ctx.pc = 0x800200C0;
  func_800200C0(ctx);
  CHECK(ctx.pc == 0x80030004u);
  CHECK(ctx.lr == 0x800200C4u);

  // bctrl does the same for CTR.
  reset(ctx);
  ctx.ctr = 0x8003000Bu;
  ctx.pc = 0x800200D0;
  func_800200D0(ctx);
  CHECK(ctx.pc == 0x80030008u);
  CHECK(ctx.lr == 0x800200D4u);

  // bctr masks an unaligned CTR target.
  reset(ctx);
  ctx.ctr = 0x8003000Fu;
  ctx.pc = 0x800200E0;
  func_800200E0(ctx);
  CHECK(ctx.pc == 0x8003000Cu);

  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("translator_semantics: all checks passed\n");
  return 0;
}
