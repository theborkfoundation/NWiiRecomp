#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/gx/fifo_parser.h"
#include <bit>
#include <cstdlib>
#include <memory>
#include <vector>

namespace nwii::runtime {
CPUContext* g_ctx_ptr = nullptr;
MMU* g_mmu = nullptr;
}
extern std::vector<uint8_t> g_hw_fifo;
static void require(bool ok) { if (!ok) std::abort(); }
int main() {
    using namespace nwii::runtime;
    using namespace nwii::runtime::hw;
    auto ctx = std::make_unique<CPUContext>();
    g_ctx_ptr = ctx.get(); g_mmu = &ctx->mmu;
    {
        using namespace nwii::runtime::gx;
        GXState a{}, b{};
        b.bp[0xF6] = 0x10;
        require(a.GetShaderHash(0x90) != b.GetShaderHash(0x90));
        auto word = [](std::vector<uint8_t>& v, uint32_t w) {
            for (int shift = 24; shift >= 0; shift -= 8) v.push_back(w >> shift);
        };
        auto bp = [&](std::vector<uint8_t>& v, unsigned reg, unsigned val) {
            v.push_back(0x61); word(v, (reg << 24) | val);
        };
        std::vector<uint8_t> stream;
        bp(stream, 0x41, 0x1234); bp(stream, 0xFE, 0xf);
        bp(stream, 0x41, 0xffffff); bp(stream, 0x41, 0xababab);
        std::vector<GXCommand> commands;
        FifoParser::Parse(stream, commands);
        require(stream.empty() && commands.size() == 4);
        require(commands[2].val == 0x123f && commands[3].val == 0xababab);
        require(g_state.bp[0x41] == 0); // parser cannot seed future render state
        for (const auto& c : commands) ApplyBPRegister(c.reg, c.val);
        require(g_state.bp[0x41] == 0xababab);
        // Reusing the same display list under a new matrix must not reuse the
        // first call's cached transform snapshot.
        constexpr uint32_t dl = 0x8000b000;
        std::vector<uint8_t> list{0x90, 0, 3};
        for (int i = 0; i < 3; ++i) {
            word(list, std::bit_cast<uint32_t>(1.f));
            word(list, 0); word(list, 0);
        }
        for (unsigned i = 0; i < list.size(); ++i) ctx->mmu.write8(dl+i, list[i]);
        for (float translation : {2.f, 5.f}) {
            stream.clear(); commands.clear();
            stream.push_back(8); stream.push_back(0x50); word(stream, 1u << 9);
            stream.push_back(8); stream.push_back(0x60); word(stream, 0);
            stream.push_back(8); stream.push_back(0x70); word(stream, 9);
            stream.insert(stream.end(), {0x10, 0, 11, 0, 0});
            const float m[] = {1,0,0,translation, 0,1,0,0, 0,0,1,0};
            for (float f : m) word(stream, std::bit_cast<uint32_t>(f));
            stream.push_back(0x40); word(stream, dl); word(stream, list.size());
            FifoParser::Parse(stream, commands);
            bool saw_draw = false;
            for (auto& c : commands) if (c.type == GXCommandType::DrawPrimitive) {
                FifoParser::DecodeDraw(c);
                require(c.vertices.size() == 3);
                require(c.vertices[0].pos[0] == 1.f + translation);
                saw_draw = true;
            }
            require(saw_draw && stream.empty());
        }
    }
    auto& mmio = MMIODispatcher::get();
    register_pi(mmio); register_cp(mmio);
    mmio.write32(0xCC000020, 0x00100000); // GP FIFO base
    // GXRedirectWriteGatherPipe uses base=0 and an arbitrary RAM write pointer.
    mmio.write32(0xCC00300C, 0);
    mmio.write32(0xCC003010, 0x04000000);
    mmio.write32(0xCC003014, 0x10008000);
    ctx->mmu.write8(0x80008000, 0xaa); // MEM1 must not receive the MEM2 upload.
    for (unsigned i = 0; i < 31; ++i) GX_WGPIPE_Write8(i + 1);
    require(mmio.read32(0xCC003014) == 0x10008000);
    require(ctx->mmu.read8(0x90008000) == 0 && g_hw_fifo.empty());
    GX_WGPIPE_Write8(32);
    require(mmio.read32(0xCC003014) == 0x10008020);
    for (unsigned i = 0; i < 32; ++i) require(ctx->mmu.read8(0x90008000+i) == i+1);
    require(ctx->mmu.read8(0x80008000) == 0xaa && g_hw_fifo.empty());
    // GXFlush commits the last partial burst without overrunning the buffer.
    GX_WGPIPE_Write32(0x12345678);
    for (int i = 0; i < 32; ++i) GX_WGPIPE_Write8(0);
    require(mmio.read32(0xCC003014) == 0x10008040);
    require(ctx->mmu.read32(0x90008020) == 0x12345678);
    require(ctx->mmu.read32(0x90008040) == 0);
    // Restore the GP FIFO. Zero padding must not become vertex data there.
    mmio.write32(0xCC00300C, 0x00100000);
    mmio.write32(0xCC003010, 0x00100ffc);
    mmio.write32(0xCC003014, 0x00100000);
    GX_WGPIPE_Write8(0x90); GX_WGPIPE_Write16(3);
    require(g_hw_fifo == std::vector<uint8_t>({0x90, 0, 3}));
    ctx->mmu.write32(0xCC008004, 0x12345678);
    require(g_hw_fifo == std::vector<uint8_t>({0x90, 0, 3, 0x12, 0x34, 0x56, 0x78}));
    // Display lists also redirect with nonzero base; end identifies last word.
    g_hw_fifo.clear();
    mmio.write32(0xCC00300C, 0x10009000);
    mmio.write32(0xCC003010, 0x1000901c);
    mmio.write32(0xCC003014, 0x10009000);
    for (unsigned i = 0; i < 32; ++i) GX_WGPIPE_Write8(0x80+i);
    require(mmio.read32(0xCC003014) == 0x10009000);
    for (unsigned i = 0; i < 32; ++i) require(ctx->mmu.read8(0x90009000+i) == 0x80+i);
    require(g_hw_fifo.empty());
    return 0;
}
