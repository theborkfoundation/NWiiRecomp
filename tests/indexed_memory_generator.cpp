#include "recompiler/recompiler.h"
#include <fstream>
#include <filesystem>

// Generate real translated functions; the regression executable compiles and
// executes them, including RA=0 with a nonzero GPR0 and destination r0 aliases.
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    nwii::loader::Executable image;
    nwii::analyzer::Analyzer analyzer(image);
    nwii::recomp::Recompiler compiler(analyzer);
    std::ofstream out(argv[1]);
    const unsigned ops[] = {534, 790, 662, 918, 535, 599, 663, 727, 983, 6, 7};
    unsigned address = 0x80010000;
    for (unsigned xo : ops) {
        for (unsigned base : {0u, 4u}) {
            nwii::analyzer::Function fn{};
            fn.start_address = address;
            fn.end_address = address + 8;
            const unsigned reg = xo == 534 || xo == 790 ? 0 : 5;
            fn.instructions = {{address, ((xo == 6 || xo == 7 ? 4u : 31u) << 26) | (reg << 21) |
                (base << 16) | (3u << 11) | (xo << 1)},
                {address + 4, 0x4e800020}};
            out << compiler.generate_function_cpp(fn);
            address += 0x10;
        }
    }
    auto emit = [&](unsigned addr, std::initializer_list<unsigned> words) {
        nwii::analyzer::Function fn{};
        fn.start_address = addr;
        for (unsigned word : words) { fn.instructions.push_back({addr, word}); addr += 4; }
        fn.end_address = addr;
        out << compiler.generate_function_cpp(fn);
    };
    auto d = [](unsigned op, unsigned rt, unsigned ra, int imm) {
        return (op << 26) | (rt << 21) | (ra << 16) | (unsigned(imm) & 65535);
    };
    auto x = [](unsigned xo, unsigned rt, unsigned ra, unsigned rb) {
        return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1);
    };
    // dcbz and dcbz_l both clear an aligned cache line; RA=0 is literal zero.
    emit(0x80011200, {x(1014, 0, 0, 3), 0x4e800020});
    emit(0x80011210, {x(1014, 0, 4, 3), 0x4e800020});
    emit(0x80011220, {(4u << 26) | (3u << 11) | (1014u << 1), 0x4e800020});
    emit(0x80011230, {(4u << 26) | (4u << 16) | (3u << 11) | (1014u << 1), 0x4e800020});
    // Carry consumers make these tests fail if the producer leaves stale CA.
    emit(0x80011000, {d(8, 5, 3, -1), x(202, 6, 4, 0), 0x4e800020});
    emit(0x80011010, {d(8, 3, 3, 0), x(202, 5, 4, 0), 0x4e800020});
    emit(0x80011020, {d(12, 3, 3, -1), x(202, 5, 4, 0), 0x4e800020});
    emit(0x80011030, {d(13, 3, 3, -1), 0x4e800020});
    // Logical instructions encode RS before RA, unlike arithmetic producers.
    emit(0x80011040, {d(29, 3, 5, 0x8000), 0x4e800020});
    emit(0x80011050, {x(792, 3, 3, 4), x(202, 5, 6, 0), 0x4e800020});
    emit(0x80011060, {d(12, 0, 3, -1), x(136, 3, 0, 3), 0x4e800020});
    // Scalar single instructions must duplicate their result into PS1.
    emit(0x80011100, {(59u << 26) | (5u << 21) | (3u << 16) | (4u << 11) | (21u << 1), 0x4e800020});
    emit(0x80011110, {(63u << 26) | (5u << 21) | (3u << 11) | (12u << 1), 0x4e800020});
    nwii::recomp::RecompilerConfig patched_config;
    patched_config.instruction_overrides[0x80566D38] =
        "ctx.mmu.write32(ctx.gpr[13] - 0x5B80, 1); ctx.ctr = ctx.gpr[12];";
    patched_config.instruction_overrides[0x80566D40] =
        "if (ctx.mmu.read32(ctx.gpr[13] - 0x5B80) != 0) ctx.mmu.write32(ctx.gpr[13] - 0x5B80, ctx.gpr[3]);";
    nwii::recomp::Recompiler patched(analyzer, nullptr, patched_config);
    for (auto [addr, word] : {std::pair{0x80566D38u, 0x7D8903A6u},
                             std::pair{0x80566D40u, 0x906DA480u}}) {
        nwii::analyzer::Function fn{};
        fn.start_address = addr; fn.end_address = addr + 8;
        fn.instructions = {{addr, word}, {addr + 4, 0x4e800020}};
        out << patched.generate_function_cpp(fn);
    }
    // Exercise the automatic SDK binding path, not just instruction emission.
    // A native-only setter loses the GXData cache and dirty flag update.
    nwii::loader::Executable sdk_image;
    constexpr unsigned sdk_addr = 0x8007FD40;
    nwii::loader::Section section{sdk_addr, 36, {}, true, false};
    for (unsigned word : {0x80828258u, 0x3803FFFFu, 0x80640254u,
                         0x500354AAu, 0x90640254u, 0x800405FCu,
                         0x60000004u, 0x900405FCu, 0x4E800020u})
        for (int shift = 24; shift >= 0; shift -= 8) section.data.push_back(word >> shift);
    sdk_image.sections.push_back(section); sdk_image.entry_point = sdk_addr;
    nwii::analyzer::Analyzer sdk_analyzer(sdk_image); sdk_analyzer.analyze();
    nwii::recomp::SymbolTable sdk_symbols;
    sdk_symbols.add_symbol(sdk_addr, "GXSetNumTevStages__regression");
    nwii::recomp::RecompilerConfig sdk_config;
    sdk_config.split_output = true;
    sdk_config.output_dir = (std::filesystem::path(argv[1]).parent_path() / "gx_setter_fixture").string();
    std::filesystem::create_directories(sdk_config.output_dir);
    nwii::recomp::Recompiler sdk_compiler(sdk_analyzer, &sdk_symbols, sdk_config);
    sdk_compiler.generate_cpp(sdk_addr);
    out << "#include \"" << sdk_config.output_dir << "/output_0.cpp\"\n";
    return out ? 0 : 1;
}
