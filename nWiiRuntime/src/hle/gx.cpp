
#include <map>
#include "runtime/cpu_context.h"

namespace nwii::runtime { extern CPUContext *g_ctx_ptr; }

#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/hw/hw.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include "runtime/diagnostics.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <iterator>
#include <chrono>
#include <cstring>

using namespace nwii::runtime;
using namespace nwii::runtime::gx;

GXState nwii::runtime::gx::g_state = {};

static std::unique_ptr<nwii::runtime::gx::IRenderer> g_renderer = nullptr;
static void* g_window = nullptr;

void GX_SetWindow(void* window) { g_window = window; }

void GX_Init() {
    g_renderer = nwii::runtime::gx::IRenderer::Create();
    if (g_renderer) g_renderer->Initialize(nullptr);
}

namespace nwii::runtime {
    extern MMU* g_mmu;
}

std::mutex g_fifo_mutex;
std::vector<uint8_t> g_hw_fifo;

namespace nwii::runtime {
uint64_t g_wgp_addr_exact = 0;
uint64_t g_wgp_addr_page = 0;
uint32_t g_wgp_addr_hist[1024] = {0};
bool g_wgp_drop4 = []() {
    const char *e = std::getenv("NWII_WGP_DROP4");
    return e && e[0] == '1';
}();
}
// Monotonic count of bytes ever pushed into the GX FIFO. Equals the offset in an
// NWII_GXDUMP stream, so a producer-side event can be matched against the bytes
// that reach the parser.
static uint64_t g_wgp_total_bytes = 0;

namespace nwii::runtime {
uint64_t GX_WGP_Count() { return ::g_wgp_total_bytes; }
}

namespace {

// Draw-header probe (NWII_HDRLOG). A GX draw command is
// <opcode 0x80..0xB7><count u16>; the engine writes it as stb then sth. Recording
// the FIFO offset of each emitted header makes it possible to check whether that
// header is still there in the captured stream.
static const bool g_hdrlog = std::getenv("NWII_HDRLOG") != nullptr;
static bool g_hdr_pending = false;
static uint64_t g_hdr_off = 0;
static uint8_t g_hdr_op = 0;
static uint64_t g_hdr_emitted = 0;

void hdr_note8(uint8_t v, uint64_t off_before) {
    if (!g_hdrlog) return;
    if (v >= 0x80 && v <= 0xB7) {
        g_hdr_pending = true;
        g_hdr_off = off_before;
        g_hdr_op = v;
    } else {
        g_hdr_pending = false;
    }
}

void hdr_note16(uint16_t v, uint64_t off_before) {
    if (!g_hdrlog) return;
    if (g_hdr_pending && off_before == g_hdr_off + 1) {
        ++g_hdr_emitted;
        if (g_hdr_emitted <= 300)
            printf("[HDR] off=%llu op=0x%02X count=%u\n",
                   (unsigned long long)g_hdr_off, g_hdr_op, v);
        else if ((g_hdr_emitted % 500) == 0)
            printf("[HDR] ... emitted so far: %llu\n",
                   (unsigned long long)g_hdr_emitted);
    }
    g_hdr_pending = false;
}

} // namespace

// Display lists are read out of guest RAM when the FIFO is parsed, which is
// long after the game issued the call -- and the game double-buffers those
// buffers, so by then the bytes can already belong to the next frame. The
// GXCallDisplayList seam copies them at the moment of the call; the parser
// takes the copy when one exists.
static std::mutex g_dl_snap_mtx;
static std::map<uint64_t, std::vector<uint8_t>> g_dl_snap;

namespace nwii::runtime {

void GX_DL_Snapshot(uint32_t addr, uint32_t size) {
    if (!nwii::runtime::g_mmu || size == 0 || size > (8u << 20)) return;
    std::vector<uint8_t> buf(size);
    if (const uint8_t* p = nwii::runtime::g_mmu->get_ptr(0x80000000u | (addr & 0x03FFFFFF)))
        std::memcpy(buf.data(), p, size);
    else
        for (uint32_t i = 0; i < size; ++i) buf[i] = nwii::runtime::g_mmu->read8(addr + i);
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    if (g_dl_snap.size() > 1024) g_dl_snap.clear();
    g_dl_snap[((uint64_t)(addr & 0x03FFFFFF) << 24) ^ size] = std::move(buf);
}

bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    auto it = g_dl_snap.find(((uint64_t)(addr & 0x03FFFFFF) << 24) ^ size);
    if (it == g_dl_snap.end() || it->second.size() != size) return false;
    out = it->second;
    return true;
}
} // namespace nwii::runtime


static std::vector<nwii::runtime::gx::GXCommand> s_carry;

uint64_t g_stat_frames = 0;
uint64_t g_stat_draws = 0;


uint64_t g_stat_parse_us = 0;
uint64_t g_stat_render_us = 0;

void GX_GetClearColor(unsigned char rgba[4]) {
    rgba[0] = (unsigned char)(g_state.clearAR & 0xFF);
    rgba[1] = (unsigned char)(g_state.clearGB >> 8);
    rgba[2] = (unsigned char)(g_state.clearGB & 0xFF);
    rgba[3] = (unsigned char)(g_state.clearAR >> 8);
}

void GX_GetXfb(uint32_t* addr, unsigned* w, unsigned* h, unsigned* stride) {
    *addr   = g_state.xfbAddr;
    *w      = g_state.xfbW;
    *h      = g_state.xfbH;
    *stride = g_state.xfbStride;
}

void ProcessGXFifo() {

    
    
    static std::vector<uint8_t> s_parse_buf;
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        if (!g_hw_fifo.empty()) {
            s_parse_buf.insert(s_parse_buf.end(), g_hw_fifo.begin(),
                               g_hw_fifo.end());
            g_hw_fifo.clear();
        }
    }
    if (s_parse_buf.empty()) return;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<nwii::runtime::gx::GXCommand> commands;
    nwii::runtime::gx::FifoParser::Parse(s_parse_buf, commands);
    auto t1 = std::chrono::steady_clock::now();
    g_stat_parse_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    static const bool s_sample = std::getenv("NWII_SAMPLE") != nullptr;
    if (s_sample && !s_parse_buf.empty()) {
        static size_t last_size = 0;
        static int stuck_ticks = 0, dumps = 0;
        if (s_parse_buf.size() == last_size) {
            if (++stuck_ticks == 60 && dumps < 8) {
                dumps++;
                stuck_ticks = 0;
                std::cout << "[GX] stalled remainder " << std::dec
                          << s_parse_buf.size() << " bytes, head:" << std::hex;
                for (size_t i = 0; i < s_parse_buf.size() && i < 24; i++)
                    std::cout << " " << (unsigned)s_parse_buf[i];
                std::cout << std::dec << "\n";
            }
        } else {
            last_size = s_parse_buf.size();
            stuck_ticks = 0;
        }
    }

    if (s_parse_buf.size() > (4u << 20))
        s_parse_buf.clear();

    

    // "sometimes only Press any button" frames. Dolphin has no such boundary

    
    
    s_carry.insert(s_carry.end(), std::make_move_iterator(commands.begin()),
                   std::make_move_iterator(commands.end()));

    int last_frame_end = -1;
    int prev_frame_end = -1; 
    for (size_t i = 0; i < s_carry.size(); ++i) {
        const auto &c = s_carry[i];
        if (c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
            c.reg == 0x52 && (c.val & 0x4000)) {
            prev_frame_end = last_frame_end;
            last_frame_end = (int)i;
        }
    }
    if (last_frame_end < 0) {
        // Bytes are pending but no frame boundary was found: the frame model, not
        // the game, is what is holding the picture back.
        if (!s_carry.empty()) {
            static const int kSlot =
                NWII_DIAG_SLOT("frame.carry_without_end", diag::Kind::Warn);
            diag::bump(kSlot);
        }
        return; 
    }

    

    

    

    std::vector<nwii::runtime::gx::GXCommand> frame;
    frame.reserve(s_carry.size());
    // NWII_KEEP_STALE: keep draws that belong to the previous frame instead of
    // dropping them. They are normally dropped to avoid half-buffer artefacts,
    // but if the frame boundary is being mis-detected here, every draw can end up
    // classified as stale and nothing reaches the screen.
    static const bool keep_stale = std::getenv("NWII_KEEP_STALE") != nullptr;
    for (int i = 0; i <= last_frame_end; ++i) {
        auto &c = s_carry[i];
        if (!keep_stale && i <= prev_frame_end &&
            c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive)
            continue; 
        frame.push_back(std::move(c));
    }
    s_carry.erase(s_carry.begin(), s_carry.begin() + last_frame_end + 1);

    // NWII_FRAMESIG: per-frame signature (draw count + TEV colour/konst regs
    // + alpha test). If the WARNING "press" blink exists in our command
    // stream, some field here must alternate between frames.
    static const bool framesig = std::getenv("NWII_FRAMESIG") != nullptr;
    if (framesig && g_stat_frames < 240) {
        size_t draws = 0, verts = 0;
        for (const auto &c : frame)
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
                draws++;
                verts += c.raw ? c.raw->count : c.vertices.size();
            }
        // Sampled hash of guest MEM1: distinguishes "game logic frozen" from
        // "logic running but the screen is legitimately static".
        uint32_t ramsig = 0;
        if (nwii::runtime::g_mmu) {
            for (uint32_t a = 0x80003000; a < 0x81800000; a += 4096)
                ramsig = ramsig * 33 + nwii::runtime::g_mmu->read32(a);
        }
        // Hash the actual draw payloads and XF matrix payloads: counts alone
        // can't distinguish "static screen" from "animation we drop".
        uint32_t drawsig = 0, xfsig = 0;
        for (const auto &c : frame) {
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive && c.raw)
                for (size_t i = 0; i < c.raw->bytes.size(); i += 4)
                    drawsig = drawsig * 33 + c.raw->bytes[i];
            if (c.type == nwii::runtime::gx::GXCommandType::XFRegister)
                for (float f : c.payload) {
                    uint32_t u; std::memcpy(&u, &f, 4);
                    xfsig = xfsig * 33 + u;
                }
        }
        uint32_t tevsig = 0;
        for (int r = 0xE0; r <= 0xE7; r++) tevsig = tevsig * 33 + g_state.bp[r];
        std::cout << "[FRAMESIG] f=" << g_stat_frames << " draws=" << draws
                  << " verts=" << verts << " tev=" << std::hex << tevsig
                  << " draw=" << drawsig << " xf=" << xfsig
                  << " ram=" << ramsig
                  << " alpha=" << g_state.bp[0xF3] << " cmode=" << g_state.bp[0x41]
                  << std::dec << "\n";
    }

    ++g_stat_frames;
    {
        static const int kSlot = NWII_DIAG_SLOT("frame.count", diag::Kind::Info);
        diag::bump(kSlot);
    }
    // Decode only the surviving frame's draws (stale frames' draws were
    // dropped above and their vertices are never built).
    auto d0 = std::chrono::steady_clock::now();
    for (auto &c : frame) {
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            nwii::runtime::gx::FifoParser::DecodeDraw(c);
            ++g_stat_draws;
        }
    }
    auto d1 = std::chrono::steady_clock::now();
    uint64_t decode_us =
        std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count();
    g_stat_parse_us += decode_us;
    nwii::runtime::hw::cp_signal_draw_done();

    // [GXPROF]: once a second, split the drain cost by cause. Prints in
    // headless too (the [STAT] line is window-only), so the 3D-scene
    // bottleneck can be measured without a display.
    {
        namespace fp = nwii::runtime::gx;
        extern uint64_t g_prof_decode_us_accum;
        g_prof_decode_us_accum += decode_us;
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 2) {
            last = now;
            if (std::getenv("NWII_DEBUG")) std::cout << "[GXPROF] parse+decode_ms=" << g_stat_parse_us / 1000
                      << " (decode_ms=" << g_prof_decode_us_accum / 1000 << ")"
                      << " draws_snap=" << fp::g_prof_draws
                      << " draw_bytes=" << fp::g_prof_draw_bytes
                      << " dl_calls=" << fp::g_prof_dl_calls
                      << " dl_bytes=" << fp::g_prof_dl_bytes
                      << " dl_ms=" << fp::g_prof_dl_us / 1000
                      << " snap_ms=" << fp::g_prof_snap_us / 1000
                      << " dl_loadindx=" << fp::g_prof_dl_loadindx
                      << " dl_hits=" << fp::g_prof_dl_hits
                      << " unknown=" << fp::g_prof_unknown
                      << " carry=" << s_carry.size()
                      << " buf=" << s_parse_buf.size() << "\n";
            g_prof_decode_us_accum = 0;
            fp::g_prof_draws = 0;
            fp::g_prof_draw_bytes = 0;
            fp::g_prof_dl_calls = 0;
            fp::g_prof_dl_bytes = 0;
            fp::g_prof_unknown = 0;
            fp::g_prof_dl_us = 0;
            fp::g_prof_snap_us = 0;
            fp::g_prof_dl_loadindx = 0;
            fp::g_prof_dl_hits = 0;
        }
    }

    if (g_window) {
        if (!g_renderer) {
            g_renderer = nwii::runtime::gx::IRenderer::Create();
            g_renderer->Initialize(nullptr);
        }
        if (g_renderer) {
            auto r0 = std::chrono::steady_clock::now();
            g_renderer->Render(frame);
            auto r1 = std::chrono::steady_clock::now();
            g_stat_render_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0)
                    .count();
        }
    }
}

namespace nwii::runtime {

static inline void wgp_push(uint8_t b) {

    static const char* s_dump_path = std::getenv("NWII_GXDUMP");
    static const char* s_seg_path = std::getenv("NWII_WGSEG");
    if (s_dump_path) {
        static FILE* f = fopen(s_dump_path, "wb");
        static size_t n = 0;
        if (f && n < 4000000) {
            fputc(b, f);
            fflush(f);
            ++n;
        }
    }

    
    if (const char* segp = s_seg_path) {
        static FILE* sf = fopen(segp, "w");
        static uint64_t off = 0;
        static uint32_t last_pc = 0;
        extern CPUContext* g_ctx_ptr;
        uint32_t pc = g_ctx_ptr ? g_ctx_ptr->pc : 0;
        if (sf && pc != last_pc) {
            fprintf(sf, "%llx %x\n", (unsigned long long)off, pc);
            last_pc = pc;
        }
        ++off;
    }

    
    static const bool wgtrace = std::getenv("NWII_WGTRACE") != nullptr;
    if (wgtrace) {
        static int zeros = 0;
        static int logged = 0;
        static uint8_t prev = 0xFF;
        static int vcd_collect = 0;      
        static uint32_t vcd_val = 0;
        static uint8_t vcd_reg = 0;
        extern CPUContext* g_ctx_ptr;
        if (vcd_collect > 0) {
            vcd_val = (vcd_val << 8) | b;
            if (--vcd_collect == 0)
                std::cout << "[WGP] VCD 0x" << std::hex << (unsigned)vcd_reg
                          << " = 0x" << vcd_val << " lr=0x"
                          << (g_ctx_ptr ? g_ctx_ptr->lr : 0) << std::dec << "\n";
        } else if (prev == 0x08 && (b == 0x50 || b == 0x60) && g_ctx_ptr &&
                   (g_ctx_ptr->pc == 0x800bbbac)) {
            vcd_collect = 4; vcd_val = 0; vcd_reg = b;
        }
        prev = b;
        if (b == 0) {
            ++zeros;
        } else {
            if (zeros >= 24 && b >= 0x80 && logged < 16 && g_ctx_ptr) {
                std::cout << "[WGP] draw 0x" << std::hex << (unsigned)b
                          << " after " << std::dec << zeros << " zeros pc=0x"
                          << std::hex << g_ctx_ptr->pc << " lr=0x"
                          << g_ctx_ptr->lr << std::dec << "\n";
                ++logged;
            }
            zeros = 0;
        }
    }
    // NWII_WGPCALL: identify the exact instruction (pc) and its caller (lr) that
    // pushes a non-zero byte right after a zero run -- i.e. the byte that the
    // parser cannot place.
    static const bool wgcall = std::getenv("NWII_WGPCALL") != nullptr;
    if (wgcall) {
        static int zeros = 0;
        if (b == 0) {
            ++zeros;
        } else {
            if (zeros >= 20 && b >= 0x80 && g_ctx_ptr) {
                static int shown = 0;
                if (shown++ < 40)
                    printf("[WGCALL] val=0x%02X after %d zeros pc=0x%08X lr=0x%08X\n",
                           b, zeros, g_ctx_ptr->pc, g_ctx_ptr->lr);
            }
            zeros = 0;
        }
    }

    // NWII_WGTHR: is more than one OS thread writing the FIFO? Two interleaved
    // writers would explain commands landing inside a vertex stream.
    static const bool wgthr = std::getenv("NWII_WGTHR") != nullptr;
    if (wgthr) {
        static uint64_t total = 0;
        static uint32_t last_thread = 0;
        static int transitions = 0;
        if (g_ctx_ptr) {
            uint32_t th = g_ctx_ptr->mmu.read32(0x800000E4u);
            if (th != last_thread) {
                if (transitions++ < 40)
                    printf("[WGTHR] byte %llu: thread 0x%08X -> 0x%08X (lr=0x%08X)\n",
                           (unsigned long long)total, last_thread, th,
                           g_ctx_ptr->lr);
                last_thread = th;
            }
        }
        ++total;
    }

    g_hw_fifo.push_back(b);
    ++g_wgp_total_bytes;
}





// The GX write-gather pipe (0xCC008000) always feeds the CP's command FIFO on
// real hardware; the PI FIFO registers at 0xCC00300C/10/14 govern a separate PI
// channel and never divert GX commands. Titles that use the PI FIFO for their
// own buffers (Rayman Origins does, for in-MEM1 display lists) were therefore
// having whole blocks of GX commands silently stolen out of the middle of a
// frame, which desynced the FIFO parser and dropped draws. Keep the diversion
// available, but opt-in only.
static bool pip_fifo_redirect_enabled() {
    static const bool on = []() {
        const char *e = std::getenv("NWII_PI_FIFO_REDIRECT");
        return e && e[0] == '1';
    }();
    return on;
}

static bool wgp_redirect(uint8_t b) {
    if (!pip_fifo_redirect_enabled()) return false;
    uint32_t base, end, wptr;
    nwii::runtime::hw::pi_fifo_get(base, end, wptr);
    bool redirect = base != 0;
    if (redirect) {
        uint32_t cp_base = nwii::runtime::hw::cp_fifo_base_reg();
        if ((base & 0x03FFFFFF) == (cp_base & 0x03FFFFFF))
            redirect = false;
    }
    if (redirect && !nwii::runtime::g_mmu) redirect = false;

    // Report transitions: FIFO bytes silently leaving the GX stream would drop
    // whole commands out of the middle of a frame.
    {
        static bool s_last = false;
        static uint64_t s_bytes = 0;
        static int s_msgs = 0;
        if (redirect != s_last && s_msgs++ < 20) {
            printf("[WGPREDIR] %s at %llu bytes redirected (pi base=0x%08X end=0x%08X "
                   "wptr=0x%08X cp_base=0x%08X)\n",
                   redirect ? "START" : "STOP", (unsigned long long)s_bytes, base,
                   end, wptr, nwii::runtime::hw::cp_fifo_base_reg());
            s_last = redirect;
        }
        if (redirect) ++s_bytes;
    }

    if (!redirect) return false;
    {
        static const int kSlot =
            NWII_DIAG_SLOT("wgp.redirected_bytes", diag::Kind::Warn);
        diag::bump(kSlot);
    }
    nwii::runtime::g_mmu->write8(0x80000000u | (wptr & 0x01FFFFFFu), b);
    wptr += 1;
    if ((wptr & 0x03FFFFFF) > (end & 0x03FFFFFF))
        wptr = base;
    nwii::runtime::hw::pi_fifo_set_wptr(wptr);
    return true;
}

void GX_WGPIPE_Write8(uint8_t val) {
    if (wgp_redirect(val)) return;
    const uint64_t off_before = g_wgp_total_bytes;
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    wgp_push(val);
    hdr_note8(val, off_before);
}

void GX_WGPIPE_Write16(uint16_t val) {
    if (wgp_redirect(val >> 8)) { wgp_redirect(val & 0xFF); return; }
    const uint64_t off_before = g_wgp_total_bytes;
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    wgp_push(val >> 8);
    wgp_push(val & 0xFF);
    hdr_note16(val, off_before);
}

void GX_WGPIPE_Write32(uint32_t val) {
    if (wgp_redirect(val >> 24)) {
        wgp_redirect((val >> 16) & 0xFF);
        wgp_redirect((val >> 8) & 0xFF);
        wgp_redirect(val & 0xFF);
        return;
    }
    if (std::getenv("NWII_SAMPLE")) {
        static uint64_t n = 0;
        ++n;
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 ||
            (n % 100000) == 0)
            std::cout << "[GX] WGP write32 #" << std::dec << n
                      << " fifo_bytes=" << g_hw_fifo.size() << "\n";
    }
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    wgp_push(val >> 24);
    wgp_push((val >> 16) & 0xFF);
    wgp_push((val >> 8) & 0xFF);
    wgp_push(val & 0xFF);
}

void GX_WGPIPE_WriteF32(float val) {
    union { float f; uint32_t i; } u; u.f = val;
    GX_WGPIPE_Write32(u.i);
}

void GX_WGPIPE_WriteF64(double val) {
    union { double d; uint64_t i; } u; u.d = val;
    if (wgp_redirect((u.i >> 56) & 0xFF)) {
        for (int s = 48; s >= 0; s -= 8)
            wgp_redirect((u.i >> s) & 0xFF);
        return;
    }
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    for (int s = 56; s >= 0; s -= 8)
        wgp_push((u.i >> s) & 0xFF);
}

} 
uint64_t g_prof_decode_us_accum = 0;
