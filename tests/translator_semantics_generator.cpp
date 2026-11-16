// Generates translated functions covering the Round 5 translator fixes:
// overflow-enabled arithmetic, record-form CR1 updates and indirect-branch
// target masking. The regression executable compiles and runs the output.
#include "recompiler/recompiler.h"

#include <fstream>
#include <initializer_list>

static constexpr unsigned kBlr = 0x4E800020u;

static unsigned x(unsigned xo, unsigned rd, unsigned ra, unsigned rb,
                  unsigned rc = 0) {
  return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
}

static unsigned f59(unsigned xo, unsigned fd, unsigned fa, unsigned fb,
                    unsigned fc, unsigned rc) {
  return (59u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (fc << 6) |
         (xo << 1) | rc;
}

static unsigned f63(unsigned xo, unsigned fd, unsigned fa, unsigned fb,
                    unsigned rc) {
  return (63u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (xo << 1) | rc;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;

  nwii::loader::Executable image;
  nwii::analyzer::Analyzer analyzer(image);
  nwii::recomp::Recompiler compiler(analyzer);
  std::ofstream out(argv[1]);

  auto emit = [&](unsigned addr, std::initializer_list<unsigned> words) {
    nwii::analyzer::Function fn{};
    fn.start_address = addr;
    for (unsigned word : words) {
      fn.instructions.push_back({addr, word});
      addr += 4;
    }
    fn.end_address = addr;
    out << compiler.generate_function_cpp(fn);
  };

  // OE arithmetic with the destination aliasing a source operand.
  emit(0x80020000, {x(778, 3, 3, 4, 1), kBlr}); // addo.  r3,r3,r4
  emit(0x80020010, {x(747, 3, 3, 4, 1), kBlr}); // mullwo. r3,r3,r4
  emit(0x80020020, {x(522, 3, 3, 4, 1), kBlr}); // addco. r3,r3,r4
  emit(0x80020030, {x(650, 3, 3, 4, 1), kBlr}); // addeo. r3,r3,r4
  emit(0x80020040, {x(648, 3, 3, 4, 1), kBlr}); // subfeo. r3,r3,r4
  emit(0x80020050, {x(714, 3, 3, 0, 1), kBlr}); // addzeo. r3,r3
  emit(0x80020060, {x(712, 3, 3, 0, 1), kBlr}); // subfzeo. r3,r3
  emit(0x80020070, {x(616, 3, 3, 0, 1), kBlr}); // nego.   r3,r3

  // Record-form floating point must refresh CR1 from FPSCR.
  emit(0x80020080, {f59(21, 1, 2, 3, 0, 1), kBlr}); // fadds. f1,f2,f3
  emit(0x80020090, {f59(21, 1, 2, 3, 0, 0), kBlr}); // fadds  f1,f2,f3
  emit(0x800200A0, {f63(21, 1, 2, 3, 1), kBlr});    // fadd.  f1,f2,f3

  // Indirect branches clear the low two bits of the target.
  emit(0x800200B0, {kBlr, kBlr});             // blr
  emit(0x800200C0, {0x4E800021u, kBlr});      // blrl
  emit(0x800200D0, {0x4E800421u, kBlr});      // bctrl
  emit(0x800200E0, {0x4E800420u, kBlr});      // bctr

  return out ? 0 : 1;
}
