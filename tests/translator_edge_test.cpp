// Regression tests for the translator/runtime edge cases in BUG_REPORT.md
// Round 3: guest float->int conversion and PowerPC reservations.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "runtime/cpu_context.h"

namespace nwii::runtime {
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;
} // namespace nwii::runtime

using nwii::runtime::MMU;

static int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static void test_fctiw() {
  const uint32_t RN_NEAREST = 0u << 30;
  const uint32_t RN_ZERO = 1u << 30;
  const uint32_t RN_PINF = 2u << 30;
  const uint32_t RN_NINF = 3u << 30;

  // NaN and out-of-range values saturate instead of invoking host UB.
  CHECK(nwii::runtime::guest_fctiw(std::numeric_limits<double>::quiet_NaN(),
                                   RN_NEAREST, true) == (int32_t)0x80000000);
  CHECK(nwii::runtime::guest_fctiw(1e30, RN_NEAREST, true) == INT32_MAX);
  CHECK(nwii::runtime::guest_fctiw(-1e30, RN_NEAREST, true) ==
        (int32_t)0x80000000);
  CHECK(nwii::runtime::guest_fctiw(std::numeric_limits<double>::infinity(),
                                   RN_NEAREST, false) == INT32_MAX);

  // fctiwz truncates, fctiw follows the guest rounding mode.
  CHECK(nwii::runtime::guest_fctiw(2.9, RN_NEAREST, true) == 2);
  CHECK(nwii::runtime::guest_fctiw(-2.9, RN_NEAREST, true) == -2);
  CHECK(nwii::runtime::guest_fctiw(2.5, RN_ZERO, false) == 2);
  CHECK(nwii::runtime::guest_fctiw(2.5, RN_PINF, false) == 3);
  CHECK(nwii::runtime::guest_fctiw(-2.5, RN_NINF, false) == -3);
}

static void test_reservation(MMU &mmu) {
  const uint32_t addr = 0x80001020;
  const uint32_t same_line = 0x80001030;
  const uint32_t other_line = 0x80002000;

  mmu.clear_reservation();
  CHECK(!mmu.reservation_matches(addr));

  mmu.set_reservation(addr);
  CHECK(mmu.reservation_matches(addr));
  CHECK(mmu.reservation_matches(same_line)); // reservation is per 32-byte line
  CHECK(!mmu.reservation_matches(other_line));

  mmu.write8(same_line + 4, 0xAA); // any store in the line clears it
  CHECK(!mmu.reservation_matches(addr));

  mmu.set_reservation(addr);
  mmu.write32(other_line, 0x12345678); // a different line keeps it
  CHECK(mmu.reservation_matches(addr));

  mmu.clear_reservation();
  CHECK(!mmu.reservation_matches(addr));
}

int main() {
  test_fctiw();
  MMU mmu;
  nwii::runtime::g_mmu = &mmu;
  test_reservation(mmu);
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("translator_edge: all checks passed\n");
  return 0;
}
