#include "runtime/cpu_context.h"
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

using namespace nwii::runtime;
// These fixtures contain no calls, interrupts or MMIO. Keep them independent
// of the runtime's global hardware state.
namespace nwii::runtime {
MMU* g_mmu = nullptr;
CPUContext* g_ctx_ptr = nullptr;
void interpret_step(CPUContext&) { std::abort(); }
}
#include "indexed_memory.inc"

static void require_impl(bool ok, int line) {
    if (!ok) { std::fprintf(stderr, "require failed at line %d\n", line); std::abort(); }
}
#define require(cond) require_impl((cond), __LINE__)
int main() {
    auto ctx = std::make_unique<CPUContext>();
    // Exercise both worker orderings. A completion before handle publication
    // must stay complete; a later completion must clear a published handle.
    for (bool early_completion : {false, true}) {
        auto& c = *ctx;
        c.gpr[13] = 0x80008000;
        c.gpr[12] = 0x80566F94;
        c.lr = 0x80020000;
        c.pc = 0x80566D38;
        func_80566D38(c);
        require(c.ctr == 0x80566F94);
        require(c.mmu.read32(c.gpr[13] - 0x5B80) == 1);
        if (early_completion) c.mmu.write32(c.gpr[13] - 0x5B80, 0);
        c.gpr[3] = 0x90001000;
        c.pc = 0x80566D40;
        func_80566D40(c);
        require(c.mmu.read32(c.gpr[13] - 0x5B80) ==
                (early_completion ? 0u : 0x90001000u));
        if (!early_completion) c.mmu.write32(c.gpr[13] - 0x5B80, 0);
        require(c.mmu.read32(c.gpr[13] - 0x5B80) == 0);
    }
    {
        auto& c = *ctx;
        c.fpr[3] = 1.25; c.fpr[4] = 2.5; c.ps1[5] = -99;
        c.pc = 0x80011100; func_80011100(c);
        require(c.fpr[5] == 3.75 && c.ps1[5] == 3.75);
        c.fpr[3] = 1.0 / 3.0;
        c.pc = 0x80011110; func_80011110(c);
        require(c.fpr[5] == double(float(1.0 / 3.0)) && c.ps1[5] == c.fpr[5]);
        // Destination aliases the scalar source, as in matrix transforms.
        c.fpr[3] = 2; c.ps1[3] = 3; c.fpr[4] = 5; c.ps1[4] = 7;
        c.ps_muls0(4, 3, 4);
        require(c.fpr[4] == 10 && c.ps1[4] == 15);
        c.fpr[4] = 5; c.fpr[5] = 11; c.ps1[5] = 13;
        c.ps_madds0(4, 3, 4, 5);
        require(c.fpr[4] == 21 && c.ps1[4] == 28);
        // Signed six-bit scale -1, not unsigned 63.
        c.gqr[0] = (63u << 24) | (4u << 16) | (63u << 8) | 4u;
        c.mmu.write8(0x80008000, 3); c.mmu.write8(0x80008001, 5);
        c.psq_load(6, 0x80008000, 0, 0);
        require(c.fpr[6] == 6 && c.ps1[6] == 10);
        c.psq_store(6, 0x80008002, 0, 0);
        require(c.mmu.read8(0x80008002) == 3 && c.mmu.read8(0x80008003) == 5);
        c.gqr[0] = 0;
    }
    {
        auto& c = *ctx;
        c.gpr[2] = 0x80010000;
        const uint32_t gx_data = 0x80020000;
        c.mmu.write32(c.gpr[2] - 0x7da8, gx_data);
        c.mmu.write32(gx_data + 0x254, 0x80011);
        c.mmu.write32(gx_data + 0x5fc, 0x80);
        c.gpr[3] = 2; c.pc = 0x8007fd40; c.lr = 0x80030000;
        GXSetNumTevStages__regression(c);
        require(c.mmu.read32(gx_data + 0x254) == (0x80011 | (1u << 10)));
        require(c.mmu.read32(gx_data + 0x5fc) == 0x84);
        require(c.pc == c.lr);
    }
    using Fn = void (*)(CPUContext&);
    const Fn funcs[] = {
        func_80010000, func_80010010, func_80010020, func_80010030,
        func_80010040, func_80010050, func_80010060, func_80010070,
        func_80010080, func_80010090, func_800100A0, func_800100B0,
        func_800100C0, func_800100D0, func_800100E0, func_800100F0,
        func_80010100, func_80010110, func_80010120, func_80010130,
        func_80010140, func_80010150};
    for (unsigned i = 0; i < 22; ++i) {
        auto& c = *ctx;
        const unsigned addr = 0x80008000;
        c.gpr[0] = 0x1000; // poison the incorrectly used zero base
        c.gpr[3] = addr;
        c.gpr[4] = 0x20;
        c.gpr[5] = 0x12345678;
        c.ps1[5] = -1.5;
        c.fpr[5] = i / 2 == 8 ? std::bit_cast<double>(0x1234ull) : 3.25;
        const unsigned ea = addr + (i % 2 ? 0x20 : 0);
        c.mmu.write32(ea, 0x12345678);
        c.mmu.write32(ea + 4, 0x9abcdef0);
        c.pc = 0x80010000 + i * 0x10;
        c.lr = 0x80020000;
        funcs[i](c);
        require(c.pc == c.lr);
        switch (i / 2) {
        case 0: require(c.gpr[0] == 0x78563412); break;
        case 1: require(c.gpr[0] == 0x3412); break;
        case 2: require(c.mmu.read32(ea) == 0x78563412); break;
        case 3: require(c.mmu.read16(ea) == 0x7856); break;
        case 4:
            require(c.fpr[5] == (double)std::bit_cast<float>(0x12345678u));
            require(c.ps1[5] == c.fpr[5]); break;
        case 5: require(std::bit_cast<uint64_t>(c.fpr[5]) == 0x123456789abcdef0ull); break;
        case 6: require(c.mmu.read_f32(ea) == 3.25f); break;
        case 7: require(c.mmu.read_f64(ea) == 3.25); break;
        case 8: require(c.mmu.read32(ea) == 0x1234); break;
        case 9:
            require(c.fpr[5] == (double)std::bit_cast<float>(0x12345678u));
            require(c.ps1[5] == (double)std::bit_cast<float>(0x9abcdef0u)); break;
        case 10:
            require(c.mmu.read_f32(ea) == 3.25f);
            require(c.mmu.read_f32(ea + 4) == -1.5f); break;
        }
    }
    auto run = [&](Fn fn, unsigned pc) { ctx->pc = pc; fn(*ctx); require(ctx->pc == ctx->lr); };
    {
        const Fn clear[] = {func_80011200, func_80011210, func_80011220, func_80011230};
        for (unsigned i = 0; i < 4; ++i) {
            auto& c = *ctx;
            constexpr uint32_t addr = 0x80008100;
            for (unsigned k = 0; k < 96; ++k) c.mmu.write8(addr + k, 0xa5);
            c.gpr[0] = 0x1000; // RA=0 must ignore this register.
            c.gpr[3] = addr + 43 - (i % 2 ? 7 : 0);
            c.gpr[4] = 7;
            run(clear[i], 0x80011200 + i * 0x10);
            for (unsigned k = 0; k < 96; ++k)
                require(c.mmu.read8(addr + k) == (k >= 32 && k < 64 ? 0 : 0xa5));
        }
    }
    {
        auto& c = *ctx;
        uint64_t bits;
        // mffs: FPSCR in the low word, Dolphin's NaN prefix in the high word.
        c.fpscr = 0x12345678u; c.fpr[3] = 0.0;
        run(func_80011300, 0x80011300);
        std::memcpy(&bits, &c.fpr[3], 8);
        require(bits == 0xFFF8000012345678ull);
        // mtfsf 0xF0 replaces the top four FPSCR nibbles only.
        c.fpscr = 0x11223344u;
        bits = 0xAABBCCDDull;
        std::memcpy(&c.fpr[4], &bits, 8);
        run(func_80011310, 0x80011310);
        require(c.fpscr == 0xAABB3344u);
        // mtfsb1/mtfsb0 set and clear one bit.
        c.fpscr = 0;
        run(func_80011330, 0x80011330);
        require(c.fpscr == 0x80000000u);
        run(func_80011320, 0x80011320);
        require(c.fpscr == 0);
        // mtfsfi 0,15 writes the top nibble.
        c.fpscr = 0x12345678u;
        run(func_80011340, 0x80011340);
        require(c.fpscr == 0xF2345678u);
        // mcrfs cr2,cr1 copies field 1 and clears the exception bits read.
        c.fpscr = 0x0F000000u;
        run(func_80011350, 0x80011350);
        require(c.fpscr == 0);
        require(c.cr[2].lt && c.cr[2].gt && c.cr[2].eq && c.cr[2].so);
    }
    for (unsigned input : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        ctx->gpr[3] = input; ctx->gpr[4] = 0; ctx->xer = 0x80000000u;
        run(func_80011000, 0x80011000);
        require(ctx->gpr[5] == ~input && ctx->gpr[6] == 1);
        require((ctx->xer & 0x80000000u) != 0);
        ctx->gpr[3] = input; ctx->xer = 0x20000000u;
        run(func_80011010, 0x80011010);
        require(ctx->gpr[3] == 0u - input && ctx->gpr[5] == (input == 0));
        ctx->gpr[3] = input; ctx->xer = 0x20000000u;
        run(func_80011020, 0x80011020);
        require(ctx->gpr[3] == input - 1 && ctx->gpr[5] == (input != 0));
        ctx->gpr[3] = input; ctx->xer = 0x80000000u;
        run(func_80011030, 0x80011030);
        require(bool(ctx->xer & 0x20000000u) == (input != 0));
        require(ctx->cr[0].eq == (input == 1) && ctx->cr[0].so);
        ctx->gpr[3] = input;
        run(func_80011060, 0x80011060);
        require(ctx->gpr[3] == (input != 0));
        ctx->gpr[3] = input; ctx->cr[0].eq = false; ctx->cr[0].lt = false;
        run(func_80011040, 0x80011040);
        require(ctx->gpr[5] == (input & 0x80000000u));
        require(ctx->cr[0].eq == !(input & 0x80000000u));
        require(ctx->cr[0].lt == bool(input & 0x80000000u));
    }
    for (unsigned initial_ca : {0u, 0x20000000u})
    for (unsigned shift : {0u, 1u, 31u, 32u, 63u}) {
        for (int value : {-4, -3, 0, 7}) {
            ctx->gpr[3] = (unsigned)value; ctx->gpr[4] = shift;
            ctx->gpr[6] = 0; ctx->xer = initial_ca;
            run(func_80011050, 0x80011050);
            require(ctx->gpr[3] == (unsigned)(value >> (shift >= 32 ? 31 : shift)));
            const bool ca = value < 0 && (shift >= 32 ||
                (shift > 0 && ((unsigned)value & ((1u << shift) - 1)) != 0));
            require(ctx->gpr[5] == (unsigned)ca);
        }
    }

}
