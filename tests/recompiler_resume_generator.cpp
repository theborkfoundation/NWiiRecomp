// Generate translated functions whose entry points exercise the resume
// dispatch: one dense function (offset table) and one sparse function (the
// switch fallback used for analyzer artifacts with a large address span).
#include "recompiler/recompiler.h"

#include <fstream>
#include <iostream>
#include <string>

static unsigned addi(unsigned rd, unsigned ra, int imm) {
  return (14u << 26) | (rd << 21) | (ra << 16) | (unsigned(imm) & 0xFFFF);
}

static constexpr unsigned kBlr = 0x4E800020u;

int main(int argc, char** argv) {
  if (argc != 2)
    return 1;

  nwii::loader::Executable image;
  nwii::analyzer::Analyzer analyzer(image);
  nwii::recomp::Recompiler compiler(analyzer);

  std::string code;

  // Dense: four register immediates followed by blr. All addresses are resume
  // points.
  nwii::analyzer::Function dense{};
  dense.start_address = 0x80100000;
  unsigned addr = dense.start_address;
  for (unsigned i = 0; i < 4; ++i) {
    dense.instructions.push_back({addr, addi(3 + i, 0, int(i) + 1)});
    addr += 4;
  }
  dense.instructions.push_back({addr, kBlr});
  dense.end_address = addr + 4;
  code += compiler.generate_function_cpp(dense);

  // Sparse: two instructions 64 KiB apart. The dense offset table would need
  // 16k slots for three instructions, so this keeps the switch path.
  nwii::analyzer::Function sparse{};
  sparse.start_address = 0x80200000;
  sparse.instructions = {{0x80200000u, addi(3, 0, 1)},
                         {0x80210000u, addi(4, 0, 2)},
                         {0x80210004u, kBlr}};
  sparse.end_address = 0x80210008u;
  code += compiler.generate_function_cpp(sparse);

  if (code.find("switch (ctx.pc)") == std::string::npos) {
    std::cerr << "sparse function lost its switch resume dispatch\n";
    return 1;
  }
  if (code.find("NWII_RESUME_LOC(") == std::string::npos ||
      code.find("NWII_RESUME_DEFAULT") == std::string::npos) {
    std::cerr << "dense function is missing the resume offset table\n";
    return 1;
  }
  // The dense function must not still be carrying the old per-address switch.
  size_t dense_end = code.find("void func_80200000");
  if (dense_end == std::string::npos ||
      code.substr(0, dense_end).find("switch (ctx.pc)") != std::string::npos) {
    std::cerr << "dense function still uses the switch resume dispatch\n";
    return 1;
  }

  std::ofstream out(argv[1]);
  out << code;
  return out ? 0 : 1;
}
