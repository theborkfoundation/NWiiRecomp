#include <iostream>
#include <vector>
#include "runtime/gx/fifo_parser.h"
#include "runtime/diagnostics.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <map>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace nwii::runtime {
    extern MMU* g_mmu;
}

namespace nwii::runtime::gx {

namespace {

// Audit counters for the FIFO interpreter. These are the checks that turn a GX
// framing bug from "the screen looks wrong" into a named, counted event:
//
//   fifo.bad_continuation  a command was accepted whose payload does not end on
//                          another command -- i.e. the size we used is wrong.
//   fifo.unknown_opcode    a byte we cannot decode; recovery skipped it.
//   fifo.resync_bytes      bytes discarded to recover framing.
//   fifo.vsize_repaired    a draw whose vertex size was re-derived from the
//                          stream because the VAT disagreed with it.
const int kSlotBadCont = NWII_DIAG_SLOT("fifo.bad_continuation", diag::Kind::Error);
const int kSlotUnknown = NWII_DIAG_SLOT("fifo.unknown_opcode", diag::Kind::Warn);
const int kSlotResync = NWII_DIAG_SLOT("fifo.resync_bytes", diag::Kind::Error);
const int kSlotVsizeFix = NWII_DIAG_SLOT("fifo.vsize_repaired", diag::Kind::Warn);
// A block of vertex data with no draw header in front of it. The CP cannot
// consume that, so the block is dropped; counting it separates "this title
// streams vertices without a header" from "our framing is off" -- the two
// produce the same downstream garbage.
const int kSlotHeaderless =
    NWII_DIAG_SLOT("fifo.headerless_vertices", diag::Kind::Error);

} // namespace

extern GXState g_state;

// VAT index of the most recent well-formed draw. A headerless vertex block
// almost always belongs to the pass that draw started, so this is the best
// guess at the format to decode it with.
static int g_last_draw_vat = 0;

// NWII_SKINDBG state (declared early so ParseCP/ParseBP in the anonymous
// namespace can reference it).
// Parse-time shadow of the XF matrix memory: matrix loads (direct 0x10 and
// indexed 0x20-0x38) are applied here as the stream is parsed, so each draw
// can snapshot the matrix that is current at its point in the stream — before
// the renderer applies later loads over the same slot.
static float g_parse_posmtx[256] = {0};
static float g_parse_nrmmtx[96] = {0};

static void ParseApplyMatrixWrite(uint32_t xf_addr, const std::vector<float>& payload) {
    for (size_t i = 0; i < payload.size(); ++i) {
        uint32_t a = xf_addr + (uint32_t)i;
        if (a < 256) g_parse_posmtx[a] = payload[i];
        else if (a >= 0x400 && a < 0x400 + 96) g_parse_nrmmtx[a - 0x400] = payload[i];
    }
}

static bool g_skindbg = false;
static uint64_t g_skin_loaded_rows = 0;
static uint64_t g_skin_used_rows = 0;
static uint64_t g_skin_used_missing = 0;
static uint64_t g_skin_draws_total = 0, g_skin_draws_pmidx = 0, g_skin_defnz = 0, g_skin_snapped = 0;

namespace {
    inline bool gx_trace() {
        static bool t = std::getenv("NWII_GXTRACE") != nullptr;
        return t;
    }

    inline uint32_t Read24(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 16) | (fifo[offset+1] << 8) | fifo[offset+2];
    }

    inline uint32_t Read32(const std::vector<uint8_t>& fifo, size_t offset) {
        return (fifo[offset] << 24) | (fifo[offset+1] << 16) | (fifo[offset+2] << 8) | fifo[offset+3];
    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t val);

    // CPU parsing can run ahead of rendering. Keep BP read/modify/write state
    // separate so future writes cannot seed an earlier draw's shader key.
    static uint32_t g_parse_bp[256] = {};
    static uint32_t g_bp_mask = 0xFFFFFF;
    uint32_t MergeBP(uint8_t reg, uint32_t val) {
        if (reg == 0xFE) {
            g_bp_mask = val;
        } else {
            val = (g_parse_bp[reg] & ~g_bp_mask) | (val & g_bp_mask);
            g_bp_mask = 0xFFFFFF;
        }
        g_parse_bp[reg] = val;
        return val;
    }

    void ParseBP(uint8_t reg, uint32_t val) {

        
        
        if (reg == 0x45 && (val & 0xF) == 2) {
            nwii::runtime::hw::pe_signal_finish();
        } else if (reg == 0x47) {
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, false);
        } else if (reg == 0x48) {
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, true);
        }

        

    }

    void ApplyBPRegisterImpl(uint8_t reg, uint32_t val) {
        g_state.bp[reg] = val;
        if (reg == 0x00) {
            g_state.numTexGens = (val & 0xF);
            g_state.numChans = ((val >> 4) & 0x7);
            g_state.numTevStages = ((val >> 10) & 0xF) + 1;
            g_state.cullMode = (val >> 14) & 3;
        } else if (reg >= 0x28 && reg <= 0x2F) {

            
            int stage = (reg - 0x28) * 2;
            for (int half = 0; half < 2; ++half) {
                uint32_t f = val >> (half * 12);
                auto& s = g_state.tevStages[stage + half];
                bool enabled = (f >> 6) & 1;
                s.texMap = enabled ? (uint8_t)(f & 0x7) : 0xFF;
                s.texCoord = (f >> 3) & 0x7;
                s.colorChan = (f >> 7) & 0x7;
            }
        } else if (reg >= 0xC0 && reg <= 0xDF) {
            
            // Layout per Dolphin BPMemory TevStageCombiner — the inputs are

            
            int stage = (reg - 0xC0) / 2;
            auto& s = g_state.tevStages[stage];
            if (((reg - 0xC0) & 1) == 0) { 
                s.colorInD    = val & 0xF;
                s.colorInC    = (val >> 4) & 0xF;
                s.colorInB    = (val >> 8) & 0xF;
                s.colorInA    = (val >> 12) & 0xF;
                s.colorBias   = (val >> 16) & 0x3;
                s.colorOp     = (val >> 18) & 0x1;
                s.colorClamp  = (val >> 19) & 0x1;
                s.colorScale  = (val >> 20) & 0x3;
                s.colorRegId  = (val >> 22) & 0x3;
            } else {                       
                s.alphaInD    = (val >> 4) & 0x7;
                s.alphaInC    = (val >> 7) & 0x7;
                s.alphaInB    = (val >> 10) & 0x7;
                s.alphaInA    = (val >> 13) & 0x7;
                s.alphaBias   = (val >> 16) & 0x3;
                s.alphaOp     = (val >> 18) & 0x1;
                s.alphaClamp  = (val >> 19) & 0x1;
                s.alphaScale  = (val >> 20) & 0x3;
                s.alphaRegId  = (val >> 22) & 0x3;
            }
        } else if (reg >= 0x80 && reg <= 0x87) {
            // TexMode0 for tiles 0-7: wrap_s at bits 0-1, wrap_t at bits 2-3
            // (Dolphin BPMemory TexMode0). GX has no default of its own here --
            // the game always sets it -- and the hardware default is clamp.
            int idx = reg - 0x80;
            g_state.texStages[idx].wrap_s = (uint8_t)(val & 3);
            g_state.texStages[idx].wrap_t = (uint8_t)((val >> 2) & 3);
        } else if ((reg >= 0x88 && reg <= 0x8B) || (reg >= 0xA8 && reg <= 0xAB)) {
            
            // 10-19, format bits 20-23 (Dolphin BPMemory TexImage0).
            int idx = (reg >= 0x88 && reg <= 0x8B) ? (reg - 0x88) : (reg - 0xA8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].width  = ((val >>  0) & 0x3FF) + 1;
                g_state.texStages[idx].height = ((val >> 10) & 0x3FF) + 1;
                g_state.texStages[idx].format = (val >> 20) & 0xF;
            }
        } else if ((reg >= 0x94 && reg <= 0x97) || (reg >= 0xB4 && reg <= 0xB7)) {
            
            int idx = (reg >= 0x94 && reg <= 0x97) ? (reg - 0x94) : (reg - 0xB4 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].base_addr = (val & 0xFFFFFF) << 5;
            }
        } else if ((reg >= 0x98 && reg <= 0x9F) || (reg >= 0xB8 && reg <= 0xBF)) {

            int idx = (reg >= 0x98 && reg <= 0x9F) ? (reg - 0x98) : (reg - 0xB8 + 4);
            if (idx < (int)g_state.texStages.size()) {
                g_state.texStages[idx].tlut_offset = (val & 0x3FF) << 9;
                g_state.texStages[idx].tlut_format = (val >> 10) & 0x3;
            }
        } else if (reg == 0x64) {

            // (Dolphin names Wind Waker and Double Dash; MP7 does it too, which

            uint32_t addr = (val & 0xFFFFFF) << 5;
            if (nwii::runtime::Config::get().platform ==
                nwii::runtime::Platform::GameCube)
                addr &= 0x01FFFFFF;
            g_state.tlutSrcAddr = addr;
        } else if (reg == 0x65) {

            uint32_t dst = (val & 0x3FF) << 9;
            uint32_t bytes = ((val >> 10) & 0x7FF) * 32;
            if (nwii::runtime::g_mmu && dst + bytes <= sizeof(g_state.tlutMem)) {
                if (const uint8_t* p =
                        nwii::runtime::g_mmu->get_ptr(g_state.tlutSrcAddr)) {
                    std::memcpy(&g_state.tlutMem[dst], p, bytes);
                } else {
                    for (uint32_t i = 0; i < bytes; i++)
                        g_state.tlutMem[dst + i] =
                            nwii::runtime::g_mmu->read8(g_state.tlutSrcAddr + i);
                }
            }
        } else if (reg == 0x40) {
            g_state.zMode.enable  = (val >> 0) & 1;
            g_state.zMode.func    = (val >> 1) & 7;
            g_state.zMode.update  = (val >> 4) & 1;
        } else if (reg == 0xF3) {

            g_state.alphaTest.ref0  = val & 0xFF;
            g_state.alphaTest.ref1  = (val >> 8) & 0xFF;
            g_state.alphaTest.comp0 = (val >> 16) & 0x7;
            g_state.alphaTest.comp1 = (val >> 19) & 0x7;
            g_state.alphaTest.logic = (val >> 22) & 0x3;
        } else if (reg == 0x4F) {
            g_state.clearAR = val & 0xFFFF; 
        } else if (reg == 0x50) {
            g_state.clearGB = val & 0xFFFF; 
        } else if (reg == 0x49) { 
            g_state.efbSrcX = val & 0x3FF;
            g_state.efbSrcY = (val >> 10) & 0x3FF;
        } else if (reg == 0x4A) { 
            g_state.efbW = (val & 0x3FF) + 1;
            g_state.efbH = ((val >> 10) & 0x3FF) + 1;
        } else if (reg == 0x4B) { 
            g_state.efbCopyDest = (val & 0xFFFFFF) << 5;
        } else if (reg == 0x4D) { 
            g_state.efbCopyStride = (val & 0x3FF) << 5;
        } else if (reg == 0x52) { 

            if (gx_trace()) {
                printf("[GXTRACE] BP 0x52 PE_COPY_EXECUTE val=0x%08X (bit14=%d bit11=%d)\n",
                       val, (val & 0x4000) != 0, (val & 0x800) != 0);
            }

            
            if (val & 0x4000) {
                g_state.xfbAddr = g_state.efbCopyDest;
                g_state.xfbW = g_state.efbW;
                g_state.xfbH = g_state.efbH;
                g_state.xfbStride = g_state.efbCopyStride
                                        ? g_state.efbCopyStride
                                        : (uint32_t)g_state.efbW * 2;
                g_state.frame_ready = true;
            }
            if (val & 0x800) {
                g_state.pe_clear_pending = true;
            }
            nwii::runtime::hw::pe_signal_finish();
        }
    }

    

    
    void ParseCP(uint8_t reg, uint32_t val) {
        g_state.cp[reg] = val;
        if (g_skindbg && (reg == 0x50 || reg == 0x30 || reg == 0x60)) {
            static uint32_t last50=0xFFFFFFFF, last30=0xFFFFFFFF, last60=0xFFFFFFFF;
            uint32_t* lp = reg==0x50?&last50:(reg==0x30?&last30:&last60);
            if (val != *lp) {
                *lp = val;
                printf("[CPTR] reg=0x%02X val=0x%08X (0x50.bit0/posMatIdx=%d)\n",
                       reg, val, reg==0x50 ? (val & 1) : -1);
            }
        }
        if (reg >= 0xA0 && reg <= 0xAF) {
            g_state.arrayBase[reg - 0xA0] = val & 0x3FFFFFFF;
        } else if (reg >= 0xB0 && reg <= 0xBF) {
            g_state.arrayStride[reg - 0xB0] = val & 0xFF;
        } else if (reg == 0x30) {
            // Matrix index A: pos/normal (bits 0-5), tex0-3 (6-29).
            g_state.defPosMtxIdx = val & 0x3F;
            for (int t = 0; t < 4; t++)
                g_state.defTexMtxIdx[t] = (val >> (6 + t * 6)) & 0x3F;
            static const bool tmdbg = std::getenv("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 4; t++) seen |= (1u << g_state.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX30] tex0-3 indices:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf(" numTexGens=%d\n", g_state.numTexGens);
                    seen = 0;
                }
            }
        } else if (reg == 0x40) {
            // Matrix index B: tex4-7.
            for (int t = 0; t < 4; t++)
                g_state.defTexMtxIdx[4 + t] = (val >> (t * 6)) & 0x3F;
            static const bool tmdbg = std::getenv("NWII_TEXMTXDBG") != nullptr;
            if (tmdbg) {
                static uint32_t seen = 0;
                for (int t = 0; t < 8; t++) seen |= (1u << g_state.defTexMtxIdx[t]);
                static auto t0 = std::chrono::steady_clock::now();
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[TEXMTX] indices seen:");
                    for (int i = 0; i < 32; i++) if (seen & (1u << i)) printf(" %d", i);
                    printf("\n");
                    seen = 0;
                }
            }
        } else if (reg == 0x50) {

            
            
            for (int i = 0; i < 8; i++) {
                g_state.vat[i].posMatIdx  = (val >> 0) & 1;
                for (int t = 0; t < 8; t++)
                    g_state.vat[i].texMatIdx[t] = (val >> (1 + t)) & 1;
                g_state.vat[i].posMask    = (VtxAttrMask)((val >> 9)  & 3);
                g_state.vat[i].nrmMask    = (VtxAttrMask)((val >> 11) & 3);
                g_state.vat[i].clrMask[0] = (VtxAttrMask)((val >> 13) & 3);
                g_state.vat[i].clrMask[1] = (VtxAttrMask)((val >> 15) & 3);
                g_state.vat[i].nrmIndex3  = ((val >> 31) & 1) != 0;
            }
        } else if (reg == 0x60) {
            
            for (int i = 0; i < 8; i++)
                for (int t = 0; t < 8; t++)
                    g_state.vat[i].texMask[t] = (VtxAttrMask)((val >> (t * 2)) & 3);
        } else if (reg >= 0x70 && reg <= 0x77) {
            
            int i = reg - 0x70;
            g_state.vat[i].posElements  = (val >> 0)  & 1;
            g_state.vat[i].posType      = (VtxAttrType)((val >> 1) & 7);
            g_state.vat[i].posShift     = (val >> 4)  & 0x1F;
            g_state.vat[i].nrmElements  = (val >> 9)  & 1;
            g_state.vat[i].nrmType      = (VtxAttrType)((val >> 10) & 7);
            g_state.vat[i].clrElements[0] = (val >> 13) & 1;
            g_state.vat[i].clrType[0]   = (VtxAttrType)((val >> 14) & 7);
            g_state.vat[i].clrElements[1] = (val >> 17) & 1;
            g_state.vat[i].clrType[1]   = (VtxAttrType)((val >> 18) & 7);
            g_state.vat[i].texElements[0] = (val >> 21) & 1;
            g_state.vat[i].texType[0]   = (VtxAttrType)((val >> 22) & 7);
            g_state.vat[i].texShift[0]  = (val >> 25) & 0x1F;
        } else if (reg >= 0x80 && reg <= 0x87) {
            
            int i = reg - 0x80;
            g_state.vat[i].texElements[1] = (val >> 0)  & 1;
            g_state.vat[i].texType[1]   = (VtxAttrType)((val >> 1) & 7);
            g_state.vat[i].texShift[1]  = (val >> 4)  & 0x1F;
            g_state.vat[i].texElements[2] = (val >> 9)  & 1;
            g_state.vat[i].texType[2]   = (VtxAttrType)((val >> 10) & 7);
            g_state.vat[i].texShift[2]  = (val >> 13) & 0x1F;
            g_state.vat[i].texElements[3] = (val >> 18) & 1;
            g_state.vat[i].texType[3]   = (VtxAttrType)((val >> 19) & 7);
            g_state.vat[i].texShift[3]  = (val >> 22) & 0x1F;
            g_state.vat[i].texElements[4] = (val >> 27) & 1;
            g_state.vat[i].texType[4]   = (VtxAttrType)((val >> 28) & 7);
        } else if (reg >= 0x90 && reg <= 0x97) {
            
            int i = reg - 0x90;
            g_state.vat[i].texShift[4]  = (val >> 0)  & 0x1F;
            g_state.vat[i].texElements[5] = (val >> 5)  & 1;
            g_state.vat[i].texType[5]   = (VtxAttrType)((val >> 6) & 7);
            g_state.vat[i].texShift[5]  = (val >> 9)  & 0x1F;
            g_state.vat[i].texElements[6] = (val >> 14) & 1;
            g_state.vat[i].texType[6]   = (VtxAttrType)((val >> 15) & 7);
            g_state.vat[i].texShift[6]  = (val >> 18) & 0x1F;
            g_state.vat[i].texElements[7] = (val >> 23) & 1;
            g_state.vat[i].texType[7]   = (VtxAttrType)((val >> 24) & 7);
            g_state.vat[i].texShift[7]  = (val >> 27) & 0x1F;
        }
    }

    inline int TypeSize(VtxAttrType type) {
        switch (type) {
            case VtxAttrType::U8: case VtxAttrType::S8: return 1;
            case VtxAttrType::U16: case VtxAttrType::S16: return 2;
            default: return 4; 
        }
    }

    inline float DecodeScalar(const uint8_t* p, VtxAttrType type, uint8_t shift) {
        switch (type) {
            case VtxAttrType::U8:  return (float)p[0] / (float)(1 << shift);
            case VtxAttrType::S8:  return (float)(int8_t)p[0] / (float)(1 << shift);
            case VtxAttrType::U16: return (float)(uint16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            case VtxAttrType::S16: return (float)(int16_t)((p[0] << 8) | p[1]) / (float)(1 << shift);
            default: {
                uint32_t v = ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
                float f; std::memcpy(&f, &v, 4); return f;
            }
        }
    }

    

    
    bool ReadVectorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                        VtxAttrMask mask, VtxAttrType type, uint8_t shift,
                        uint32_t array_base, uint32_t array_stride, int ncomp, float* out) {
        for (int i = 0; i < ncomp; i++) out[i] = 0.0f;
        if (mask == VtxAttrMask::None) return true;

        int sz = TypeSize(type);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)sz * ncomp > fifo_size) return false;
            for (int i = 0; i < ncomp; i++)
                out[i] = DecodeScalar(&fifo[fifo_offset + (size_t)sz * i], type, shift);
            fifo_offset += (size_t)sz * ncomp;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else { 
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;

        

        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        {
            static const bool s_attr = std::getenv("NWII_ATTRDBG") != nullptr;
            if (s_attr &&
                g_state.texStages[0].base_addr == 0x105A40E0u) {
                static int an = 0;
                if (an++ < 10) {
                    const float *ff = reinterpret_cast<const float *>(p);
                    std::printf("[ATTR] base=%08X stride=%u idx=%u addr=%08X "
                                "ncomp=%d f=(%.4f %.4f %.4f %.4f)\n",
                                array_base, array_stride, index, base, ncomp,
                                ff[0], ff[1], ff[2], ff[3]);
                }
            }
        }
        float inv = 1.0f / (float)(1 << shift);
        for (int i = 0; i < ncomp; i++) {
            const uint8_t* q = p + (size_t)sz * i;
            switch (type) {
                case VtxAttrType::U8:  out[i] = (float)q[0] * inv; break;
                case VtxAttrType::S8:  out[i] = (float)(int8_t)q[0] * inv; break;
                case VtxAttrType::U16: out[i] = (float)(uint16_t)((q[0] << 8) | q[1]) * inv; break;
                case VtxAttrType::S16: out[i] = (float)(int16_t)((q[0] << 8) | q[1]) * inv; break;
                default: {
                    uint32_t w = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
                                 ((uint32_t)q[2] << 8) | q[3];
                    std::memcpy(&out[i], &w, 4);
                    break;
                }
            }
        }
        return true;
    }

    
    
    int ColorBytes(VtxAttrType clrType) {
        switch ((int)clrType) {
            case 0: case 3: return 2;
            case 1: case 4: return 3;
            default:        return 4; 
        }
    }

    void DecodeColor(const uint8_t* p, VtxAttrType clrType, float out[4]) {
        switch ((int)clrType) {
            case 0: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 11) & 0x1F) / 31.0f;
                out[1] = ((c >> 5) & 0x3F) / 63.0f;
                out[2] = (c & 0x1F) / 31.0f;
                out[3] = 1.0f;
                break;
            }
            case 1: 
            case 2: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = 1.0f;
                break;
            case 3: { 
                uint16_t c = (p[0] << 8) | p[1];
                out[0] = ((c >> 12) & 0xF) / 15.0f;
                out[1] = ((c >> 8) & 0xF) / 15.0f;
                out[2] = ((c >> 4) & 0xF) / 15.0f;
                out[3] = (c & 0xF) / 15.0f;
                break;
            }
            case 4: { 
                uint32_t c = (p[0] << 16) | (p[1] << 8) | p[2];
                out[0] = ((c >> 18) & 0x3F) / 63.0f;
                out[1] = ((c >> 12) & 0x3F) / 63.0f;
                out[2] = ((c >> 6) & 0x3F) / 63.0f;
                out[3] = (c & 0x3F) / 63.0f;
                break;
            }
            default: 
                out[0] = p[0] / 255.0f;
                out[1] = p[1] / 255.0f;
                out[2] = p[2] / 255.0f;
                out[3] = p[3] / 255.0f;
                break;
        }
    }

    
    bool ReadColorAttr(const std::vector<uint8_t>& fifo, size_t& fifo_offset, size_t fifo_size,
                       VtxAttrMask mask, VtxAttrType clrType,
                       uint32_t array_base, uint32_t array_stride, float out[4]) {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        if (mask == VtxAttrMask::None) return true;

        int nbytes = ColorBytes(clrType);
        if (mask == VtxAttrMask::Direct) {
            if (fifo_offset + (size_t)nbytes > fifo_size) return false;
            DecodeColor(&fifo[fifo_offset], clrType, out);
            fifo_offset += nbytes;
            return true;
        }

        uint32_t index = 0;
        if (mask == VtxAttrMask::Index8) {
            if (fifo_offset + 1 > fifo_size) return false;
            index = fifo[fifo_offset++];
        } else {
            if (fifo_offset + 2 > fifo_size) return false;
            index = (fifo[fifo_offset] << 8) | fifo[fifo_offset + 1];
            fifo_offset += 2;
        }
        if (!nwii::runtime::g_mmu) return true;
        uint32_t base = array_base + index * array_stride;
        const uint8_t* p = nwii::runtime::g_mmu->get_ptr(base);
        if (!p) return true;
        DecodeColor(p, clrType, out);
        return true;
    }
}


void ApplyBPRegister(uint8_t reg, uint32_t val) { ApplyBPRegisterImpl(reg, val); }

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth);



// Display lists are static geometry the game CALLs every frame; re-parsing
// them dominated the 3D-scene drain (measured 1.3s per 2s). Cache the parsed
// commands keyed by address+content, guarded by a hash of the parser state
// that gets baked into draw snapshots (VAT, arrays, matrix indices, channel
// control) so a state change forces a re-parse. LOAD_INDX payloads are the
// one thing read from RAM at parse time — on a cache hit they are re-read so
// skinning animation stays live. NWII_NODLCACHE=1 disables.
struct DLCacheEntry {
    uint64_t state_hash;
    uint64_t content_hash;
    std::vector<GXCommand> cmds;
};
static std::unordered_map<uint64_t, DLCacheEntry> s_dl_cache;
uint64_t g_prof_dl_hits = 0;
// NWII_SKINDBG report (state declared earlier for ParseCP visibility).
static void skin_report() {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() < 2) return;
    t0 = now;
    printf("[SKIN] loaded rows:");
    for (int i = 0; i < 64; i++) if (g_skin_loaded_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used posMtxIdx:");
    for (int i = 0; i < 64; i++) if (g_skin_used_rows & (1ull<<i)) printf(" %d", i);
    printf(" | used-but-unloaded:");
    for (int i = 0; i < 64; i++) if (g_skin_used_missing & (1ull<<i)) printf(" %d", i);
    printf(" | draws=%llu perVtxIdx=%llu defNonZero=%llu snapped=%llu\n",
           (unsigned long long)g_skin_draws_total,
           (unsigned long long)g_skin_draws_pmidx,
           (unsigned long long)g_skin_defnz,
           (unsigned long long)g_skin_snapped);
    g_skin_loaded_rows = g_skin_used_rows = g_skin_used_missing = 0;
    g_skin_draws_total = g_skin_draws_pmidx = g_skin_defnz = g_skin_snapped = 0;
}

static uint64_t DLStateHash() {
    uint64_t h = 14695981039346656037ull;
    for (int r = 0x30; r <= 0xBF; ++r) { h ^= g_state.cp[r]; h *= 1099511628211ull; }
    h ^= g_state.xf[0x00E]; h *= 1099511628211ull;
    h ^= g_state.xf[0x00F]; h *= 1099511628211ull;
    // Cached draws contain matrix snapshots and merged BP writes. Neither may
    // be replayed under a different incoming matrix/material state.
    for (uint32_t v : g_parse_bp) { h ^= v; h *= 1099511628211ull; }
    h ^= g_bp_mask; h *= 1099511628211ull;
    for (float v : g_parse_posmtx) {
        uint32_t bits; std::memcpy(&bits, &v, 4);
        h ^= bits; h *= 1099511628211ull;
    }
    for (float v : g_parse_nrmmtx) {
        uint32_t bits; std::memcpy(&bits, &v, 4);
        h ^= bits; h *= 1099511628211ull;
    }
    return h;
}

static void ExpandDisplayList(uint32_t addr, uint32_t size,
                              std::vector<GXCommand>& commands, int depth) {
    uint32_t phys = addr & 0x1FFFFFFF;
    // Display lists may live in either MEM1 or MEM2. This engine keeps its
    // command buffers in MEM2 (the same buffers it hands to the PI FIFO), so a
    // MEM1-only bound check silently dropped whole lists and their draws.
    const uint64_t end = (uint64_t)phys + size;
    const bool in_mem1 = end <= 0x01800000ull;
    const bool in_mem2 = phys >= 0x10000000u && end <= 0x14000000ull;
    if (!nwii::runtime::g_mmu || size == 0 || size > 0x400000 ||
        (!in_mem1 && !in_mem2) || depth >= 4) {
        // A refused display list is silently-lost geometry: count it so it cannot
        // hide behind an otherwise plausible-looking frame. An empty list is a
        // legal no-op and must not be reported as an error, or the counters cry
        // wolf and stop being trusted.
        if (size == 0) {
            static const int kEmpty =
                NWII_DIAG_SLOT("dl.empty", diag::Kind::Info);
            diag::bump(kEmpty);
        } else {
            static const int kSlot =
                NWII_DIAG_SLOT("dl.rejected", diag::Kind::Error);
            char d[128];
            std::snprintf(d, sizeof(d),
                          "list addr=0x%08X size=%u depth=%d (mem1=%d mem2=%d)",
                          addr, size, depth, (int)in_mem1, (int)in_mem2);
            diag::fail(kSlot, d);
        }
        return;
    }
    g_prof_dl_calls++;
    g_prof_dl_bytes += size;
    auto dl0 = std::chrono::steady_clock::now();

    static const bool nocache = std::getenv("NWII_NODLCACHE") != nullptr;
    const uint8_t* src = nwii::runtime::g_mmu->get_ptr(0x80000000u | phys);
    // Prefer the copy taken when the game actually made the call -- guest RAM
    // may already hold the next frame's list by the time we get here.
    std::vector<uint8_t> snap;
    if (nwii::runtime::GX_DL_Fetch(addr, size, snap)) src = snap.data();

    if (src && !nocache && depth == 0) {
        // Sampled content hash: stride 512 plus the tail, cheap even for
        // multi-MB lists. DLs are effectively immutable once written.
        uint64_t ch = 14695981039346656037ull ^ size;
        for (uint32_t i = 0; i < size; i += 512) { ch ^= src[i]; ch *= 1099511628211ull; }
        for (uint32_t i = size > 32 ? size - 32 : 0; i < size; i++) { ch ^= src[i]; ch *= 1099511628211ull; }
        uint64_t key = ((uint64_t)phys << 23) ^ size;
        uint64_t sh = DLStateHash();
        auto it = s_dl_cache.find(key);
        if (it != s_dl_cache.end() && it->second.state_hash == sh &&
            it->second.content_hash == ch) {
            g_prof_dl_hits++;
            for (const GXCommand& c0 : it->second.cmds) {
                commands.push_back(c0);
                GXCommand& c = commands.back();
                // Replay parser-state side effects so main-stream commands
                // after the CALL still see the DL's CP/BP writes.
                if (c.type == GXCommandType::CPRegister)
                    ParseCP((uint8_t)c.reg, c.val);
                else if (c.type == GXCommandType::BPRegister) {
                    g_parse_bp[(uint8_t)c.reg] = c.val;
                    if (c.reg == 0xFE) g_bp_mask = c.val;
                    else g_bp_mask = 0xFFFFFF;
                    ParseBP((uint8_t)c.reg, c.val);
                }
                else if (c.type == GXCommandType::XFRegister &&
                         (c.val & 0x80000000u)) {
                    // LOAD_INDX marker: re-read the (possibly animated)
                    // matrix data from guest RAM at today's array base.
                    int array = 12 + ((c.val >> 24) & 0x3);
                    int num = (c.val >> 16) & 0xFF;
                    uint32_t index = c.val & 0xFFFF;
                    uint32_t base = g_state.arrayBase[array] +
                                    index * g_state.arrayStride[array];
                    for (int i = 0; i < num && i < (int)c.payload.size(); i++)
                        c.payload[i] =
                            nwii::runtime::g_mmu->read_f32(base + i * 4);
                    if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                        ParseApplyMatrixWrite(c.reg, c.payload);
                } else if (c.type == GXCommandType::XFRegister &&
                           (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))) {
                    // Direct matrix write cached in the DL: feed the parse
                    // shadow so re-snapshots below stay live.
                    ParseApplyMatrixWrite(c.reg, c.payload);
                } else if (c.type == GXCommandType::DrawPrimitive && c.raw &&
                           c.raw->have_snap_mtx) {
                    // Re-snapshot the matrix from the current parse shadow so a
                    // cached skinned draw animates instead of freezing at the
                    // pose from when it was first parsed. (raw is shared_ptr;
                    // copy before mutating so the cache entry stays pristine.)
                    c.raw = std::make_shared<DrawRaw>(*c.raw);
                    uint32_t pb = (uint32_t)c.raw->defPosMtxIdx * 4;
                    uint32_t nb = 3u * (c.raw->defPosMtxIdx & 31);
                    if (pb + 12 <= 256 && nb + 9 <= 96) {
                        std::memcpy(c.raw->snap_pos_mtx, &g_parse_posmtx[pb], 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                    }
                }
            }
            auto dl1 = std::chrono::steady_clock::now();
            g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
            return;
        }
        // Miss: parse into a local vector so it can be stored.
        std::vector<uint8_t> dl(src, src + size);
        size_t off = 0;
        std::vector<GXCommand> local;
        ParseStream(dl, off, local, depth + 1);
        if (s_dl_cache.size() > 512)
            s_dl_cache.clear();
        s_dl_cache[key] = {sh, ch, local};
        commands.insert(commands.end(), local.begin(), local.end());
        auto dl1 = std::chrono::steady_clock::now();
        g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
        return;
    }

    std::vector<uint8_t> dl(size);
    if (src) {
        std::memcpy(dl.data(), src, size);
    } else {
        for (uint32_t i = 0; i < size; i++)
            dl[i] = nwii::runtime::g_mmu->read8(addr + i);
    }
    size_t off = 0;
    ParseStream(dl, off, commands, depth + 1);
    auto dl1 = std::chrono::steady_clock::now();
    g_prof_dl_us += std::chrono::duration_cast<std::chrono::microseconds>(dl1 - dl0).count();
}

// Drain-path profiling counters, reported by ProcessGXFifo's [GXPROF] line.
// The 3D-scene drain still burns seconds somewhere after deferred decode;
// these split the cost by cause instead of guessing.
uint64_t g_prof_draws = 0;        // draws snapshotted
uint64_t g_prof_draw_bytes = 0;   // raw vertex bytes copied
uint64_t g_prof_dl_calls = 0;     // display-list expansions
uint64_t g_prof_dl_bytes = 0;     // display-list bytes copied+parsed
uint64_t g_prof_cmds = 0;         // commands emitted
uint64_t g_prof_unknown = 0;
uint64_t g_prof_dl_us = 0;      // time inside display-list expansion
uint64_t g_prof_snap_us = 0;    // time snapshotting draws
uint64_t g_prof_dl_loadindx = 0; // LOAD_INDX commands seen inside display lists      // unknown-opcode single-byte skips

namespace {
    // Byte width of one attribute in the vertex stream.
    inline size_t AttrBytes(VtxAttrMask mask, VtxAttrType type, int ncomp,
                            bool is_color) {
        switch (mask) {
        case VtxAttrMask::None:    return 0;
        case VtxAttrMask::Index8:  return 1;
        case VtxAttrMask::Index16: return 2;
        default:
            return is_color ? (size_t)ColorBytes(type)
                            : (size_t)TypeSize(type) * ncomp;
        }
    }

    size_t VertexSize(const VATSlot& vat) {
        size_t sz = vat.posMatIdx ? 1 : 0;
        for (int t = 0; t < 8; t++)
            if (vat.texMatIdx[t]) sz++;
        sz += AttrBytes(vat.posMask, vat.posType, vat.posElements ? 3 : 2, false);
        {
            size_t nrm = AttrBytes(vat.nrmMask, vat.nrmType,
                                   vat.nrmElements ? 9 : 3, false);
            // Three indices, not one, when NBT normals come in by index.
            if (vat.nrmIndex3 && vat.nrmElements &&
                (vat.nrmMask == VtxAttrMask::Index8 ||
                 vat.nrmMask == VtxAttrMask::Index16))
                nrm *= 3;
            sz += nrm;
        }
        for (int ci = 0; ci < 2; ci++)
            sz += AttrBytes(vat.clrMask[ci], vat.clrType[ci], 1, true);
        for (int t = 0; t < 8; t++)
            sz += AttrBytes(vat.texMask[t], vat.texType[t],
                            vat.texElements[t] ? 2 : 1, false);
        return sz;
    }
} // namespace

void FifoParser::DecodeDraw(GXCommand& c) {
    if (c.type != GXCommandType::DrawPrimitive || !c.raw || !c.vertices.empty())
        return;
    const DrawRaw& r = *c.raw;
    const std::vector<uint8_t>& buf = r.bytes;
    const size_t buf_size = buf.size();
    // NWII_IDXHASH: report draws whose position index array changed between the
    // parse-time snapshot and this deferred decode.
    if (r.idx_hash && nwii::runtime::g_mmu) {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t k = 0; k < r.idx_len; ++k) {
            h ^= nwii::runtime::g_mmu->read8(r.idx_base + k);
            h *= 1099511628211ull;
        }
        if (h != r.idx_hash) {
            static int mn = 0;
            if (mn++ < 20)
                std::printf("[IDXCHG] n=%u base=%08X len=%u: array changed "
                            "between parse and decode\n",
                            (unsigned)r.count, r.idx_base, r.idx_len);
        }
    }
    // NWII_LOGOVAT: dump the vertex format actually used by the title-logo
    // meshes so a mismatched attribute type/shift is visible.
    static const bool s_logovat = std::getenv("NWII_LOGOVAT") != nullptr;
    if (s_logovat &&
        (r.count == 264 || r.count == 576)) {
        static int vn = 0;
        if (vn++ < 8)
            std::printf("[LOGOVAT] n=%u stream=%zu bytes/vert=%.2f "
                        "pos(mask=%d type=%d shift=%d elem=%d) "
                        "tex0(mask=%d type=%d shift=%d elem=%d) "
                        "clr0(mask=%d type=%d elem=%d) nrm(mask=%d elem=%d) "
                        "arr pos(base=%08X stride=%u) tex0(base=%08X stride=%u)\n",
                        r.count, buf.size(),
                        (double)buf.size() / (double)r.count,
                        (int)r.vat.posMask, (int)r.vat.posType,
                        (int)r.vat.posShift, (int)r.vat.posElements,
                        (int)r.vat.texMask[0], (int)r.vat.texType[0],
                        (int)r.vat.texShift[0], (int)r.vat.texElements[0],
                        (int)r.vat.clrMask[0], (int)r.vat.clrType[0],
                        (int)r.vat.clrElements[0], (int)r.vat.nrmMask,
                        (int)r.vat.nrmElements, r.arrayBase[0],
                        (unsigned)r.arrayStride[0], r.arrayBase[4],
                        (unsigned)r.arrayStride[4]);
    }
    size_t off = 0;
    c.vertices.reserve(r.count);
    // NWII_NOCOLORIDX: experiment — when the vertex stream is exactly two bytes
    // per vertex (position index + texcoord index), the colour index the VAT
    // claims is not really there; consuming it desyncs every following vertex.
    const bool skip_clr_idx =
        std::getenv("NWII_NOCOLORIDX") != nullptr &&
        (size_t)r.count * 2 == buf_size;
    for (uint32_t i = 0; i < r.count; i++) {
        VertexData vtx;
        vtx.posMtxIdx = r.defPosMtxIdx;
        for (int t = 0; t < 8; t++)
            vtx.texMtxIdx[t] = r.defTexMtxIdx[t];
        if (r.vat.posMatIdx) {
            if (off + 1 > buf_size) return;
            vtx.posMtxIdx = buf[off++];
        }
        if (g_skindbg && vtx.posMtxIdx < 64) {
            g_skin_used_rows |= (1ull << vtx.posMtxIdx);
            if (!(g_skin_loaded_rows & (1ull << vtx.posMtxIdx)))
                g_skin_used_missing |= (1ull << vtx.posMtxIdx);
        }
        for (int t = 0; t < 8; t++) {
            if (r.vat.texMatIdx[t]) {
                if (off + 1 > buf_size) return;
                vtx.texMtxIdx[t] = buf[off++];
            }
        }
        vtx.has_pos = (r.vat.posMask != VtxAttrMask::None);
        if (!ReadVectorAttr(buf, off, buf_size, r.vat.posMask, r.vat.posType,
                            r.vat.posShift, r.arrayBase[0], r.arrayStride[0],
                            r.vat.posElements ? 3 : 2, vtx.pos))
            return;
        // CLR1 and tex1-7 are parsed for stream position only (the renderer
        // uses clr0/tex0). Normals are decoded only for lit draws.
        // Three indices when NBT normals arrive by index: consume the extra two
        // so the stream position stays right even though only the normal is
        // used. Getting this wrong is invisible on this vertex and fatal on
        // every one after it.
        const bool nrm_idx3 =
            r.vat.nrmIndex3 && r.vat.nrmElements &&
            (r.vat.nrmMask == VtxAttrMask::Index8 ||
             r.vat.nrmMask == VtxAttrMask::Index16);
        const size_t nrm_idx_bytes =
            (r.vat.nrmMask == VtxAttrMask::Index16) ? 2 : 1;

        if (r.vat.nrmMask != VtxAttrMask::None && r.need_normal) {
            vtx.has_norm = true;
            float nbt[9];
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.nrmMask, r.vat.nrmType,
                                0, r.arrayBase[1], r.arrayStride[1],
                                r.vat.nrmElements ? 9 : 3, nbt))
                return;
            if (nrm_idx3) off += 2 * nrm_idx_bytes;
            vtx.norm[0] = nbt[0]; vtx.norm[1] = nbt[1]; vtx.norm[2] = nbt[2];
        } else {
            off += AttrBytes(r.vat.nrmMask, r.vat.nrmType,
                             r.vat.nrmElements ? 9 : 3, false);
            if (nrm_idx3) off += 2 * nrm_idx_bytes;
        }
        if (r.vat.clrMask[0] != VtxAttrMask::None && !skip_clr_idx) {
            float col[4];
            if (!ReadColorAttr(buf, off, buf_size, r.vat.clrMask[0],
                               r.vat.clrType[0], r.arrayBase[2],
                               r.arrayStride[2], col))
                return;
            vtx.has_color = true;
            vtx.color[0] = col[0]; vtx.color[1] = col[1];
            vtx.color[2] = col[2]; vtx.color[3] = col[3];
        }
        if (!skip_clr_idx)
            off += AttrBytes(r.vat.clrMask[1], r.vat.clrType[1], 1, true);
        if (r.vat.texMask[0] != VtxAttrMask::None) {
            vtx.has_tex[0] = true;
            if (!ReadVectorAttr(buf, off, buf_size, r.vat.texMask[0],
                                r.vat.texType[0], r.vat.texShift[0],
                                r.arrayBase[4], r.arrayStride[4],
                                r.vat.texElements[0] ? 2 : 1, vtx.tex[0]))
                return;
        }
        for (int t = 1; t < 8; t++)
            off += AttrBytes(r.vat.texMask[t], r.vat.texType[t],
                             r.vat.texElements[t] ? 2 : 1, false);
        if (off > buf_size)
            return;

        // NWII_LOGORAW: raw (pre-matrix) position/uv of the letters mesh.
        static const bool s_logoraw = std::getenv("NWII_LOGORAW") != nullptr;
        if (s_logoraw && r.count == 312) {
            static int rn = 0;
            if (rn == 0) {
                std::printf("[LOGOMTX2] defPosMtxIdx=%u snap=%zu have=%d "
                            "m=(%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | "
                            "%.3f %.3f %.3f %.3f)\n",
                            (unsigned)r.defPosMtxIdx, r.snap_pos_bank.size(),
                            (int)r.have_snap_mtx, r.snap_pos_mtx[0],
                            r.snap_pos_mtx[1], r.snap_pos_mtx[2],
                            r.snap_pos_mtx[3], r.snap_pos_mtx[4],
                            r.snap_pos_mtx[5], r.snap_pos_mtx[6],
                            r.snap_pos_mtx[7], r.snap_pos_mtx[8],
                            r.snap_pos_mtx[9], r.snap_pos_mtx[10],
                            r.snap_pos_mtx[11]);
                std::printf("[LOGOLIVE] parse=(%.3f %.3f %.3f %.3f | %.3f %.3f "
                            "%.3f %.3f) live=(%.3f %.3f %.3f %.3f | %.3f %.3f "
                            "%.3f %.3f)\n",
                            g_parse_posmtx[0], g_parse_posmtx[1],
                            g_parse_posmtx[2], g_parse_posmtx[3],
                            g_parse_posmtx[4], g_parse_posmtx[5],
                            g_parse_posmtx[6], g_parse_posmtx[7],
                            g_state.posMatrices[0], g_state.posMatrices[1],
                            g_state.posMatrices[2], g_state.posMatrices[3],
                            g_state.posMatrices[4], g_state.posMatrices[5],
                            g_state.posMatrices[6], g_state.posMatrices[7]);
                std::printf("[LOGOWRAP] tex0 wrap_s=%u wrap_t=%u bp80=%06X "
                            "bp81=%06X bp82=%06X bp83=%06X\n",
                            (unsigned)g_state.texStages[0].wrap_s,
                            (unsigned)g_state.texStages[0].wrap_t,
                            g_state.bp[0x80], g_state.bp[0x81], g_state.bp[0x82],
                            g_state.bp[0x83]);
                if (i == 0) {
                    std::printf("[LOGOIDX] bytes:");
                    for (size_t b = 0; b < 12 && b < buf.size(); ++b)
                        std::printf(" %02X", buf[b]);
                    std::printf(" | stream=%zu count=%u bytes/vert=%.2f\n",
                                buf.size(), r.count,
                                (double)buf.size() / (double)r.count);
                    // Big-endian float reader: guest memory is BE, so a host
                    // reinterpret_cast would print garbage.
                    auto bef = [](const uint8_t *q) -> float {
                        uint32_t w = ((uint32_t)q[0] << 24) |
                                     ((uint32_t)q[1] << 16) |
                                     ((uint32_t)q[2] << 8) | q[3];
                        float f;
                        std::memcpy(&f, &w, 4);
                        return f;
                    };
                    if (nwii::runtime::g_mmu) {
                        for (int k = 0; k < 3; ++k) {
                            const uint8_t *pp = nwii::runtime::g_mmu->get_ptr(
                                r.arrayBase[0] + (uint32_t)k * r.arrayStride[0]);
                            const uint8_t *pt = nwii::runtime::g_mmu->get_ptr(
                                r.arrayBase[4] + (uint32_t)k * r.arrayStride[4]);
                            if (!pp || !pt) continue;
                            std::printf("[LOGOENT] i=%d pos=(%.4f %.4f %.4f) "
                                        "uv=(%.4f %.4f)\n",
                                        k, bef(pp), bef(pp + 4), bef(pp + 8),
                                        bef(pt), bef(pt + 4));
                        }
                    }
                }
            }
            if (rn < 12) {
                std::printf("[LOGORAW] i=%u pos=(%.4f,%.4f) uv=(%.4f,%.4f)\n",
                            i, vtx.pos[0], vtx.pos[1], vtx.tex[0][0],
                            vtx.tex[0][1]);
                ++rn;
            }
            if (i + 1 == r.count) {
                int over = 0;
                for (const auto &vv : c.vertices)
                    if (vv.has_tex[0] && vv.tex[0][1] > 1.0f) ++over;
                std::printf("[LOGOVR] n=%u verts_with_v_gt_1=%d\n", r.count, over);
            }
        }

        // NWII_LOGOFIT experiment: the letters mesh arrives in its own small
        // coordinate space. Fit its raw bbox onto the silhouette's view box so
        // we can tell a transform problem from a geometry problem.
        static const bool s_logofit =
            std::getenv("NWII_LOGOFIT") != nullptr && false;
        if (s_logofit && r.count == 312 && vtx.has_pos) {
            vtx.pos[0] = 371.2f + (vtx.pos[0] + 11.87f) * (350.0f / 2.1f);
            vtx.pos[1] = 190.0f + (vtx.pos[1] - 0.2f) * (110.0f / 2.6f);
            vtx.pos[2] = 0.0f;
            vtx.pre_xf = true;
        }

        // NWII_NO_SNAPMTX: use the renderer's live position matrix instead of
        // the parse-time snapshot (diagnostic for stale-snapshot transforms).
        static const bool s_no_snap =
            std::getenv("NWII_NO_SNAPMTX") != nullptr;

        // Transform by the per-draw matrix snapshot (GX 3x4). Doing it here,
        // with the matrix captured at this draw's stream position, fixes the
        // matrix-swap skinning scramble where a live render-time lookup would
        // use whatever matrix was written last.
        if (!s_no_snap && !r.snap_pos_bank.empty() && vtx.has_pos) {
            const uint32_t pb = (uint32_t)vtx.posMtxIdx * 4;
            const uint32_t nb = 3u * (vtx.posMtxIdx & 31);
            if (pb + 12 <= 256 && nb + 9 <= 96) {
                const float* m = &r.snap_pos_bank[pb];
                float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
                vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
                vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
                vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
                if (vtx.has_norm && !r.snap_nrm_bank.empty()) {
                    const float* n = &r.snap_nrm_bank[nb];
                    float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                    vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                    vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                    vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
                }
                vtx.pre_xf = true;
            }
        } else if (!s_no_snap && r.have_snap_mtx && vtx.has_pos) {
            const float* m = r.snap_pos_mtx;
            float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
            vtx.pos[0] = m[0]*x + m[1]*y + m[2]*z + m[3];
            vtx.pos[1] = m[4]*x + m[5]*y + m[6]*z + m[7];
            vtx.pos[2] = m[8]*x + m[9]*y + m[10]*z + m[11];
            if (vtx.has_norm) {
                const float* n = r.snap_nrm_mtx;
                float a = vtx.norm[0], b = vtx.norm[1], cc = vtx.norm[2];
                vtx.norm[0] = n[0]*a + n[1]*b + n[2]*cc;
                vtx.norm[1] = n[3]*a + n[4]*b + n[5]*cc;
                vtx.norm[2] = n[6]*a + n[7]*b + n[8]*cc;
            }
            vtx.pre_xf = true;
        }
        c.vertices.push_back(vtx);
    }
    c.raw.reset(); // decoded: drop the raw copy
}

void FifoParser::Parse(std::vector<uint8_t>& fifo, std::vector<GXCommand>& commands) {
    static bool skin_init = (g_skindbg = std::getenv("NWII_SKINDBG") != nullptr, true);
    (void)skin_init;
    if (g_skindbg) skin_report();
    size_t offset = 0;
    ParseStream(fifo, offset, commands, 0);

    
    if (offset > 0 && offset <= fifo.size())
        fifo.erase(fifo.begin(), fifo.begin() + offset);
}

// Byte size of a well-formed command at `off`, or 0 when the bytes there do not
// form one. Used only to recover from a lost frame, so it is deliberately
// strict: anything questionable is rejected rather than guessed at.
static size_t CommandSizeAt(const std::vector<uint8_t>& f, size_t off) {
    const size_t n = f.size();
    if (off >= n) return 0;
    const uint8_t cmd = f[off];
    switch (cmd) {
    case 0x61:
        return (off + 5 <= n) ? 5 : 0;
    case 0x08:
        return (off + 6 <= n) ? 6 : 0;
    case 0x20: case 0x28: case 0x30: case 0x38:
        return (off + 5 <= n) ? 5 : 0;
    case 0x40:
        return (off + 9 <= n) ? 9 : 0;
    case 0x48:
        return 1;
    case 0x78: {
        // Seam read-modify-write marker: kind, reg, mask, value.
        if (off + 11 > n) return 0;
        return (f[off + 1] <= 1) ? 11 : 0;
    }
    case 0x10: {
        if (off + 5 > n) return 0;
        size_t total = 5 + ((size_t)((f[off + 1] << 8) | f[off + 2]) + 1) * 4;
        return (off + total <= n) ? total : 0;
    }
    default:
        break;
    }
    if (cmd >= 0x80 && cmd <= 0xB7) {
        if (off + 3 > n) return 0;
        uint16_t count = (uint16_t)((f[off + 1] << 8) | f[off + 2]);
        size_t need = 3 + (size_t)count * VertexSize(g_state.vat[cmd & 0x07]);
        return (off + need <= n) ? need : 0;
    }
    return 0;
}

// Is `cmd` one of the unambiguous state-write opcodes? Float vertex data can
// accidentally look like a draw opcode, but it practically never forms a
// well-framed BP/CP/XF write followed by another command.
static bool IsStateOpcode(uint8_t cmd) {
    switch (cmd) {
    case 0x61: case 0x08: case 0x10: case 0x40: case 0x78:
    case 0x20: case 0x28: case 0x30: case 0x38:
        return true;
    default:
        return false;
    }
}

// First offset >= `from` that looks like a real command boundary. A candidate is
// only accepted when `chain` consecutive commands validate: float payloads can
// accidentally look like one command, but not like a run of them.
static size_t ResyncFrom(const std::vector<uint8_t>& f, size_t from,
                         bool state_only, int chain) {
    for (size_t p = from; p + 1 < f.size(); ++p) {
        if (state_only && !IsStateOpcode(f[p])) continue;
        size_t q = p;
        bool ok = true;
        for (int k = 0; k < chain; ++k) {
            size_t s = CommandSizeAt(f, q);
            if (!s) { ok = false; break; }
            q += s;
            if (q >= f.size()) {
                // Ran out of buffer mid-chain: accept only if the last command
                // ended exactly at the end.
                ok = (q == f.size() && k + 1 >= 2);
                break;
            }
        }
        if (ok) return p;
    }
    return (size_t)-1;
}

// First CP->XFB copy (BP 0x52 with PE_COPY_EXECUTE), which is what ends a
// frame. Resyncing here keeps the frame structure intact: the renderer drops
// draws that arrive without a frame edge, so swallowing one turns the screen
// black even when the commands around it are fine.
static size_t ResyncToFrameEdge(const std::vector<uint8_t>& f, size_t from) {
    const size_t limit = std::min(f.size(), from + (size_t)(256u << 10));
    for (size_t p = from; p + 5 <= limit; ++p) {
        if (f[p] != 0x61 || f[p + 1] != 0x52) continue;
        uint32_t val = ((uint32_t)f[p + 2] << 16) | ((uint32_t)f[p + 3] << 8) |
                       (uint32_t)f[p + 4];
        if (val & 0x4000) return p;
    }
    return (size_t)-1;
}

/**
 * Does the stream continue like a command at `p`? Used to tell a correctly
 * sized draw payload from an over/under-consumed one.
 */
static bool NextLooksLikeCommand(const std::vector<uint8_t>& f, size_t p) {
    if (p >= f.size()) return true; // end of buffer: nothing to contradict us
    uint8_t c = f[p];
    if (c == 0x00 || c == 0x44 || c == 0x48) return true; // NOP-ish opcodes
    return CommandSizeAt(f, p) != 0;
}

// Vertex-size repair.
//
// The VAT is global state, so if a single CP 0x50/0x60 (VCD) write is missed or
// arrives late, every following draw is sized against a stale format and the
// whole stream desyncs. The stream itself still says how big the payload is: the
// next command must start right after it. So when the draw's current size lands
// on something that is not a command, try the other plausible strides and adopt
// the one whose continuation reads as a command.
static size_t RepairVertexSize(const std::vector<uint8_t>& f, size_t offset,
                               uint16_t count, size_t expected) {
    static const size_t kStrides[] = {6, 8, 12, 16, 20, 24, 28, 32,
                                      36, 40, 44, 48, 56, 64};
    size_t best = expected;
    size_t best_dist = (size_t)-1;
    for (size_t vs : kStrides) {
        if (vs == expected) continue;
        size_t need = 3 + (size_t)count * vs;
        if (offset + need > f.size()) continue;
        size_t q = offset + need;
        // A draw payload is followed by the next draw's state setup, so require
        // an unambiguous state write -- and a second one behind it. float payload
        // bytes can fake one command, not a burst of BP/XF writes.
        if (q >= f.size() || !IsStateOpcode(f[q])) continue;
        size_t s1 = CommandSizeAt(f, q);
        if (!s1 || q + s1 >= f.size() || !IsStateOpcode(f[q + s1])) continue;
        size_t dist = vs > expected ? vs - expected : expected - vs;
        if (dist < best_dist) {
            best_dist = dist;
            best = vs;
        }
    }
    return best;
}

// Does the stream at `q` -- after any NOP padding -- begin a burst of state
// writes? A finished draw is followed by the next draw's state setup, and the
// engine pads its batch with NOPs first, so zeros have to be skipped before the
// check. Float payload cannot fake two consecutive BP/XF writes.
static bool StateBurstAt(const std::vector<uint8_t> &f, size_t q) {
    while (q < f.size() && (f[q] == 0x00 || f[q] == 0x44 || f[q] == 0x48)) ++q;
    if (q + 1 >= f.size() || !IsStateOpcode(f[q])) return false;
    size_t s1 = CommandSizeAt(f, q);
    if (!s1 || q + s1 >= f.size() || !IsStateOpcode(f[q + s1])) return false;
    return true;
}

// How far does the stream keep parsing from `q`? Vertex-index payloads contain
// plenty of small bytes that can look like a state opcode (index 0x08, 0x10),
// so a single boundary check is not enough to pick a stride. A correct boundary
// is followed by a long run of well-formed commands; a wrong one derails almost
// immediately.
static int ScoreContinuation(const std::vector<uint8_t> &f, size_t q,
                             int max_cmds = 64) {
    int n = 0;
    for (int i = 0; i < max_cmds && q < f.size(); ++i) {
        uint8_t b = f[q];
        if (b >= 0x80 && b <= 0xBF) { n += 4; break; } // next draw: a good sign
        if (b == 0x00 || b == 0x44) { ++q; ++n; continue; } // NOPs are 1 byte
        size_t s = CommandSizeAt(f, q);
        if (!s) break;
        q += s;
        ++n;
    }
    return n;
}

// The declared vertex format needs more bytes than have arrived, yet a shorter
// stride yields a payload that fits and is followed by a well-formed stream.
// That is the stale-VCD signature: the engine streams a compact index format
// while the state we track still describes an uncompressed direct one, so every
// draw looks like an incomplete payload and is dropped.
//
// Candidates are ranked by how far the stream keeps parsing after the payload,
// then by closeness to the declared stride. A single state-write check is not
// enough here: 3-byte index triples are full of bytes that mimic opcodes.
static size_t RepairStrideToFit(const std::vector<uint8_t> &f, size_t offset,
                               uint16_t count, size_t expected) {
    static const size_t kStrides[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24};
    size_t best = expected;
    int best_score = -1;
    for (size_t vs : kStrides) {
        if (vs >= expected) continue;
        size_t need = 3 + (size_t)count * vs;
        // Require real lookahead behind the candidate payload. Near the end of
        // the buffer the following command simply has not arrived yet, which is
        // not evidence that the declared format is wrong -- waiting is correct
        // there, and repairing would truncate a draw that is still streaming.
        if (offset + need + 256 > f.size()) continue;
        int score = ScoreContinuation(f, offset + need);
        static const bool dbg = std::getenv("NWII_STRIDEDBG") != nullptr;
        static int shown = 0;
        if (dbg && shown < 40)
            printf("[STRIDEDBG] count=%u vs=%zu end=%zu score=%d\n", count, vs,
                   offset + need, score);
        if (score < 3) continue; // must reach at least a few real commands
        // Ties go to the more compact stride: the index formats are the ones a
        // stale attribute description hides, and a wrong (larger) stride is
        // exactly what swallows the rest of the frame.
        if (score > best_score || (score == best_score && vs < best)) {
            best = vs;
            best_score = score;
        }
    }
    if (best != expected) {
        static int sel = 0;
        if (std::getenv("NWII_STRIDEDBG") && sel++ < 20)
            printf("[STRIDEDBG] chose %zu (score %d)\n", best, best_score);
    }
    return best;
}

// Vertex layout for a given byte stride, for the headerless-block recovery.
// Only the layouts this title actually emits are built; anything else is
// rejected so the recovery cannot invent geometry of the wrong shape.
static bool BuildVatForStride(size_t stride, VATSlot &out) {
    std::memset(&out, 0, sizeof(out));
    switch (stride) {
    case 24: // pos f32 XYZ + colour RGBA8 + texcoord f32 ST
        out.posMask = VtxAttrMask::Direct;
        out.posType = VtxAttrType::F32;
        out.posElements = true;
        out.clrMask[0] = VtxAttrMask::Direct;
        out.clrType[0] = (VtxAttrType)5; // RGBA8
        out.texMask[0] = VtxAttrMask::Direct;
        out.texType[0] = VtxAttrType::F32;
        out.texElements[0] = true;
        break;
    case 16: // pos f32 XYZ + colour RGBA8
        out.posMask = VtxAttrMask::Direct;
        out.posType = VtxAttrType::F32;
        out.posElements = true;
        out.clrMask[0] = VtxAttrMask::Direct;
        out.clrType[0] = (VtxAttrType)5; // RGBA8
        break;
    case 12: // pos f32 XYZ
        out.posMask = VtxAttrMask::Direct;
        out.posType = VtxAttrType::F32;
        out.posElements = true;
        break;
    default:
        return false;
    }
    return VertexSize(out) == stride;
}

// A run of vertices that has no draw header. Such a block is self-describing:
// it is a whole number of vertices at some stride, and it ends exactly where a
// burst of state writes resumes (float data can fake one command opcode, not a
// chain of BP/XF writes). Returns the number of bytes in the block, or 0.
//
// This matters because the parser otherwise skips one byte into the float data,
// reads a mantissa byte as a draw opcode, and loses the rest of the frame -- a
// cascade that looks like thousands of unrelated desyncs.
// Score how well a candidate stride decodes as vertex data. A wrong stride
// misaligns the position triple onto other attributes, which is exactly how
// colour words (0xFFFFFFFF) end up read as floats -- so count the position
// components that are not finite numbers. The right stride has almost none.
static int ScoreVertexStride(const std::vector<uint8_t> &f, size_t offset,
                             size_t stride, size_t count) {
    int bad = 0;
    size_t n = std::min(count, (size_t)32);
    for (size_t i = 0; i < n; i++) {
        size_t v = offset + i * stride;
        for (int c = 0; c < 3; ++c) {
            size_t p = v + (size_t)c * 4;
            if (p + 4 > f.size()) { ++bad; continue; }
            uint32_t u = ((uint32_t)f[p] << 24) | ((uint32_t)f[p + 1] << 16) |
                         ((uint32_t)f[p + 2] << 8) | f[p + 3];
            float x;
            std::memcpy(&x, &u, 4);
            if (!std::isfinite(x) || x > 1e6f || x < -1e6f) ++bad;
        }
    }
    return bad;
}

static size_t MatchVertexBlock(const std::vector<uint8_t> &f, size_t offset,
                              size_t preferred, size_t *stride_out,
                              size_t *count_out) {
    // Candidate strides, most likely first. Every candidate that finds a valid
    // end is then scored on whether it decodes as numbers, because the boundary
    // alone cannot tell a 24-byte vertex from a 16-byte one when both divide the
    // same block length.
    const size_t cand[] = {24, 16, 12, 20, 32, 28, 36,
                           40, preferred, 8, 6, 44, 48, 56, 64};
    const size_t max_len = std::min(f.size() - offset, (size_t)(1u << 20));
    size_t best_len = 0, best_stride = 0, best_count = 0;
    int best_bad = 0x7FFFFFFF;
    for (size_t vs : cand) {
        if (vs < 4 || vs > 64) continue;
        // Walk vertex counts upward and stop at the first end that is followed by
        // real state writes. The engine pads a finished batch with NOPs, so zeros
        // between the vertices and the next command are expected.
        size_t found_k = 0;
        for (size_t k = 3; k * vs <= max_len; ++k) {
            size_t q = offset + k * vs;
            if (!StateBurstAt(f, q)) continue;
            found_k = k;
            break;
        }
        if (!found_k) continue;
        size_t len = found_k * vs;
        if (len < 3 * vs) continue;
        int bad = ScoreVertexStride(f, offset, vs, found_k);
        // Fewest misdecoded positions wins; on a tie the longer block wins (a
        // wrong stride usually truncates), then the earlier candidate.
        if (bad < best_bad || (bad == best_bad && len > best_len)) {
            best_bad = bad;
            best_len = len;
            best_stride = vs;
            best_count = found_k;
        }
    }
    if (!best_stride) return 0;
    if (stride_out) *stride_out = best_stride;
    if (count_out) *count_out = best_count;
    return best_len;
}

static void ParseStream(const std::vector<uint8_t>& fifo, size_t& offset, std::vector<GXCommand>& commands, int depth) {
    const size_t fifo_size = fifo.size();

    // Per-call command trace, only materialised when a bogus draw shows up, so
    // the sequence that produced it can be read back verbatim.
    static std::vector<std::string> s_trace;
    if (depth == 0 && offset == 0)
        s_trace.clear();

    // Persistent history of the last non-NOP commands, across ticks. The parser
    // erases consumed bytes and can stall at offset 0, so the per-call trace is
    // empty exactly when the context is most needed.
    static std::string s_hist[24];
    static int s_hist_n = 0;
    static void (*s_dummy)() = nullptr;
    (void)s_dummy;

    // Diagnostic ring: the last commands with their consumed byte counts, so a
    // desync can be attributed to the command that over/under-ran.
    struct TraceEnt { size_t off; uint8_t cmd; size_t len; };
    static TraceEnt trace_ring[12] = {};
    static int trace_ring_n = 0;
    static size_t trace_prev_off = (size_t)-1;
    static uint8_t trace_prev_cmd = 0;
    static int trace_prev_depth = 0;

    while (offset < fifo_size) {
        uint8_t cmd = fifo[offset];

        if (trace_prev_off != (size_t)-1 && depth == trace_prev_depth &&
            offset > trace_prev_off) {
            trace_ring[trace_ring_n % 12] = {trace_prev_off, trace_prev_cmd,
                                             offset - trace_prev_off};
            ++trace_ring_n;

            // Continuation invariant: the command that just ended must be
            // followed by another command. Only checked with real lookahead, so
            // a buffer that is merely truncated mid-frame is not reported.
            //
            // NOPs are exempt: they carry no payload, so they cannot have been
            // mis-sized. Reporting "NOP followed by non-command" double-counts
            // the real defect (and fires for every NOP the engine writes in front
            // of a headerless vertex block, which is legitimate padding).
            const size_t span = offset - trace_prev_off;
            const bool prev_nop = (trace_prev_cmd == 0x00 ||
                                   trace_prev_cmd == 0x44 ||
                                   trace_prev_cmd == 0x48);
            if (fifo_size >= offset + 64 && !prev_nop) {
                const uint8_t nb = fifo[offset];
                const bool nopish = (nb == 0x00 || nb == 0x44 || nb == 0x48);
                if (!nopish && CommandSizeAt(fifo, offset) == 0) {
                    char d[160];
                    std::snprintf(d, sizeof(d),
                                  "cmd 0x%02X at 0x%zX sized %zu -> next byte 0x%02X "
                                  "is not a command",
                                  trace_prev_cmd, trace_prev_off, span, nb);
                    diag::fail(kSlotBadCont, d);
                }
            }
        }
        trace_prev_off = offset;
        trace_prev_cmd = cmd;
        trace_prev_depth = depth;

        if (s_trace.size() < 4000) {
            char line[96];
            std::snprintf(line, sizeof(line), "off=%zu cmd=%02X %02X %02X %02X",
                          offset, cmd, fifo[offset + 1],
                          offset + 2 < fifo_size ? fifo[offset + 2] : 0,
                          offset + 3 < fifo_size ? fifo[offset + 3] : 0);
            s_trace.push_back(line);
        }
        if (cmd != 0x00) {
            char hl[96];
            std::snprintf(hl, sizeof(hl), "%zu:%02X %02X %02X %02X %02X %02X",
                          offset, cmd, fifo[offset + 1],
                          offset + 2 < fifo_size ? fifo[offset + 2] : 0,
                          offset + 3 < fifo_size ? fifo[offset + 3] : 0,
                          offset + 4 < fifo_size ? fifo[offset + 4] : 0,
                          offset + 5 < fifo_size ? fifo[offset + 5] : 0);
            s_hist[s_hist_n % 24] = hl;
            ++s_hist_n;
        }

        static const bool ftrace_any = std::getenv("NWII_FIFOTRACE") != nullptr;
        static int ftrace_any_n = 0;
        if (ftrace_any && ftrace_any_n < 400 && cmd < 0x80 && cmd != 0x00)
            printf("[FT %03d] off=%zu cmd=%02X argc=%02X %02X %02X\n",
                   ftrace_any_n++, offset, cmd, fifo[offset + 1],
                   offset + 2 < fifo_size ? fifo[offset + 2] : 0,
                   offset + 3 < fifo_size ? fifo[offset + 3] : 0);

        if (cmd == 0x00) {
            
            offset++;
        } else if (cmd == 0x40) {
            
            if (offset + 9 > fifo_size) break;
            uint32_t dl_addr = Read32(fifo, offset + 1);
            uint32_t dl_size = Read32(fifo, offset + 5);
            // NWII_DLDBG: are we expanding the same lists over and over?
            static const bool dldbg = std::getenv("NWII_DLDBG") != nullptr;
            if (dldbg) {
                static std::map<uint64_t, uint32_t> hits;
                static auto t0 = std::chrono::steady_clock::now();
                hits[((uint64_t)dl_addr << 32) | dl_size]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    std::vector<std::pair<uint32_t, uint64_t>> top;
                    for (auto &kv : hits) top.push_back({kv.second, kv.first});
                    std::sort(top.rbegin(), top.rend());
                    printf("[DLDBG] distinct=%zu top:", hits.size());
                    for (size_t i = 0; i < top.size() && i < 5; i++)
                        printf(" %08X/%uB x%u", (uint32_t)(top[i].second >> 32),
                               (uint32_t)top[i].second, top[i].first);
                    printf("\n");
                    hits.clear();
                }
            }
            ExpandDisplayList(dl_addr, dl_size, commands, depth);
            offset += 9;
        } else if (cmd == 0x78) {
            // Seam RMW marker (not a real GP opcode). Native GX handlers own
            // only some bits of GEN_MODE / CMODE0 / CP 0x30; merging at call
            // time raced this parser and used a base value up to a frame
            // stale, clobbering the other fields (texgen counts, dither bits,
            // texture matrix indices). The merge happens here instead, at the
            // marker's position in the stream, so the base is always current.
            if (offset + 11 > fifo_size) break;
            uint8_t kind = fifo[offset + 1];
            uint8_t reg  = fifo[offset + 2];
            uint32_t mask = Read32(fifo, offset + 3);
            uint32_t val  = Read32(fifo, offset + 7);
            GXCommand c;
            c.reg = reg;
            if (kind == 0) {
                uint32_t merged = (g_parse_bp[reg] & ~mask) | (val & mask);
                merged = MergeBP(reg, merged);
                ParseBP(reg, merged);
                c.type = GXCommandType::BPRegister;
                c.val = merged;
            } else {
                uint32_t merged = (g_state.cp[reg] & ~mask) | (val & mask);
                g_state.cp[reg] = merged;
                ParseCP(reg, merged);
                c.type = GXCommandType::CPRegister;
                c.val = merged;
            }
            commands.push_back(std::move(c));
            offset += 11;
        } else if (cmd == 0x48) {
            
            offset++;
        } else if (cmd == 0x20 || cmd == 0x28 || cmd == 0x30 || cmd == 0x38) {
            if (depth > 0) g_prof_dl_loadindx++;
            if (offset + 5 > fifo_size) break;
            uint32_t val = Read32(fifo, offset + 1);
            int array = 12 + (cmd - 0x20) / 8;
            uint32_t index = val >> 16;
            uint32_t xf_addr = val & 0xFFF;
            int num = ((val >> 12) & 0xF) + 1;
            if (nwii::runtime::g_mmu) {
                GXCommand c;
                c.type = GXCommandType::XFRegister;
                c.reg = xf_addr;
                c.length = num - 1;
                // LOAD_INDX marker for the DL cache: array/num/index packed so
                // a cache hit can re-read live matrix data.
                c.val = 0x80000000u | ((uint32_t)(array - 12) << 24) |
                        ((uint32_t)num << 16) | (index & 0xFFFF);
                c.payload.resize(num);
                uint32_t base = g_state.arrayBase[array] +
                                index * g_state.arrayStride[array];
                for (int i = 0; i < num; i++)
                    c.payload[i] = nwii::runtime::g_mmu->read_f32(base + i * 4);
                if (gx_trace()) {
                    static int ln = 0;
                    if (ln++ < 24) {
                        printf("[GXTRACE] LOAD_INDX arr=%d idx=%u xf=0x%03X n=%d base=0x%X stride=%u [",
                               array, index, xf_addr, num, base,
                               g_state.arrayStride[array]);
                        for (int i = 0; i < num && i < 8; i++)
                            printf("%.3f ", c.payload[i]);
                        printf("]\n");
                    }
                }
                if (g_skindbg && xf_addr < 0x100 && (xf_addr % 4) == 0)
                    g_skin_loaded_rows |= (1ull << (xf_addr / 4));
                if (xf_addr < 256 || (xf_addr >= 0x400 && xf_addr < 0x460))
                    ParseApplyMatrixWrite(xf_addr, c.payload);
                commands.push_back(std::move(c));
            }
            offset += 5;
        } else if (cmd == 0x61) {
            
            if (offset + 5 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = MergeBP(reg, Read24(fifo, offset + 2));
            ParseBP(reg, val);

            GXCommand c;
            c.type = GXCommandType::BPRegister;
            c.reg = reg;
            c.val = val;
            commands.push_back(std::move(c));

            offset += 5;
        } else if (cmd == 0x08) {
            
            if (offset + 6 > fifo_size) break;
            uint8_t reg = fifo[offset + 1];
            uint32_t val = Read32(fifo, offset + 2);
            ParseCP(reg, val);

            GXCommand c;
            c.type = GXCommandType::CPRegister;
            c.reg = reg;
            c.val = val;
            commands.push_back(std::move(c));

            offset += 6;
        } else if (cmd == 0x10) {
            
            if (offset + 5 > fifo_size) break;
            uint16_t length = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint32_t total_size = 5 + ((length + 1) * 4);
            if (offset + total_size > fifo_size) break;

            GXCommand c;
            c.type = GXCommandType::XFRegister;
            c.length = length;
            c.reg = (fifo[offset + 3] << 8) | fifo[offset + 4];

            if (gx_trace() && c.reg >= 0x1020 && c.reg <= 0x1027) {
                static int xf_print_proj = 0;
                if (xf_print_proj++ < 5)
                    printf("[GXTRACE] XF proj load: len=%d reg=0x%04X\n", length, c.reg);
            }
            if (gx_trace() && c.reg < 0x100) {
                static int xf_print_mtx = 0;
                if (xf_print_mtx++ < 24)
                    printf("[GXTRACE] XF direct mtx: reg=0x%03X len=%d\n", c.reg, length + 1);
            }

            // NWII_XFDBG: is the game actually animating the colour-channel
            // material/ambient registers? (0x100a-0x1011). Runs in the parser
            // so it works headless, where the renderer never runs.
            static const bool xfdbg = std::getenv("NWII_XFDBG") != nullptr;
            if (xfdbg && c.reg >= 0x1009 && c.reg <= 0x1011) {
                static uint32_t last[9] = {0};
                static uint32_t changes[9] = {0};
                static uint64_t writes[9] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i <= length; i++) {
                    int r = c.reg + i - 0x1009;
                    if (r < 0 || r > 8) continue;
                    uint32_t v = Read32(fifo, offset + 5 + i * 4);
                    writes[r]++;
                    if (v != last[r]) { changes[r]++; last[r] = v; }
                }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 1) {
                    t0 = now;
                    printf("[XFDBG] chan/mat regs (reg: writes/changes last):");
                    static const char* nm[9] = {"numchan","amb0","amb1","mat0","mat1",
                                                "cc0","cc1","ac0","ac1"};
                    for (int r = 0; r < 9; r++)
                        printf(" %s:%llu/%u=%08X", nm[r],
                               (unsigned long long)writes[r], changes[r], last[r]);
                    printf("\n");
                }
            }

            int num_floats = length + 1;
            c.payload.resize(num_floats);
            for (int i = 0; i < num_floats; ++i) {
                uint32_t val = Read32(fifo, offset + 5 + (i * 4));
                std::memcpy(&c.payload[i], &val, 4);
            }

            if (c.reg < 256 || (c.reg >= 0x400 && c.reg < 0x460))
                ParseApplyMatrixWrite(c.reg, c.payload);

            commands.push_back(std::move(c));

            offset += total_size;
        } else if (cmd >= 0x80 && cmd <= 0xBF) {

            if (offset + 3 > fifo_size) break;

            uint16_t vtx_count = (fifo[offset + 1] << 8) | fifo[offset + 2];
            uint8_t vat_idx = cmd & 0x07;

            // NWII_FIFOTRACE: dump the front of the stream command-by-command so
            // a desync can be bisected to the command that over/under-ran.
            static const bool ftrace = std::getenv("NWII_FIFOTRACE") != nullptr;
            static int ftrace_n = 0;
            if (ftrace && ftrace_n < 400) {
                printf("[FT %03d] draw off=%zu cmd=%02X vat=%u count=%u vsize=%zu |",
                       ftrace_n++, offset, cmd, vat_idx, vtx_count,
                       VertexSize(g_state.vat[vat_idx]));
                for (size_t i = offset; i < fifo_size && i < offset + 20; ++i)
                    printf(" %02X", fifo[i]);
                printf("\n");
            }
            // A count this large cannot be a real draw; report the run that led
            // here instead of guessing.
            if (vtx_count > 4000) {
                static int bad_n = 0;
                printf("[FT-BAD %d] off=%zu buf=%zu cmd=%02X vat=%u count=%u vsize=%zu\n",
                       bad_n++, offset, fifo_size, cmd, vat_idx, vtx_count,
                       VertexSize(g_state.vat[vat_idx]));
                if (bad_n <= 3) {
                    size_t from = s_trace.size() > 90 ? s_trace.size() - 90 : 0;
                    for (size_t i = from; i < s_trace.size(); ++i)
                        printf("   %s\n", s_trace[i].c_str());
                    printf("   --- hexdump ---\n");
                    size_t hs = offset > 160 ? offset - 160 : 0;
                    for (size_t i = hs; i < fifo_size && i < offset + 48; i += 16) {
                        printf("   %04zu:", i);
                        for (size_t k = i; k < fifo_size && k < i + 16; ++k)
                            printf(" %02X", fifo[k]);
                        printf("\n");
                    }
                    printf("   --- bad draw bytes:");
                    for (size_t i = offset; i < fifo_size && i < offset + 16; ++i)
                        printf(" %02X", fifo[i]);
                    printf("\n");
                }
            }

            VATSlot& vat = g_state.vat[vat_idx];
            g_last_draw_vat = vat_idx;

            // Vertex size is constant for a draw (every attribute is either
            // direct with a fixed byte width or a fixed 1/2-byte index), so
            // the payload length is known without touching the vertices.
            // Decode is deferred: frame-skip drops most draws, and decoding
            // only survivors is what keeps parse cost off the drain path.
            size_t vsize = VertexSize(vat);
            size_t need = 3 + (size_t)vtx_count * vsize;
            // Repair a stale VAT: if this size lands on something that cannot be
            // a command, the format we were told is wrong, and the stream itself
            // knows better.
            {
                static const bool vsizefix = []() {
                    const char *e = std::getenv("NWII_VSIZEFIX");
                    return e && e[0] == '1';
                }();
                // Only repair when there is plenty of data behind the draw. Near
                // the buffer end the following command merely has not arrived yet,
                // which is not evidence of a wrong size.
                if (vsizefix && depth == 0 && vtx_count <= 4096 &&
                    offset + need <= fifo_size &&
                    offset + need + 8192 <= fifo_size &&
                    !NextLooksLikeCommand(fifo, offset + need)) {
                    size_t fixed = RepairVertexSize(fifo, offset, vtx_count, vsize);
                    if (fixed != vsize) {
                        diag::bump(kSlotVsizeFix);
                        static int s_fixes = 0;
                        if (s_fixes++ < 12)
                            printf("[GX] vsize repair at 0x%zX: count=%u %zu -> %zu "
                                   "(vat tex0 mask=%d)\n",
                                   offset, vtx_count, vsize, fixed,
                                   (int)vat.texMask[0]);
                        vsize = fixed;
                        need = 3 + (size_t)vtx_count * vsize;
                    }
                }
            }
            if (offset + need > fifo_size) {
                // The declared format does not fit in what has arrived. Before
                // waiting for more data (which never comes when the format is
                // stale), see whether a shorter stride lands exactly on the next
                // burst of state writes. This is what turns the menu phase's
                // 528-vertex `0x90` draws from "needs 8451 bytes, have 4132" into
                // a parsed draw.
                if (depth == 0 && vtx_count >= 1 && vtx_count <= 4096) {
                    size_t fixed = RepairStrideToFit(fifo, offset, vtx_count, vsize);
                    if (fixed != vsize) {
                        diag::bump(kSlotVsizeFix);
                        static int s_fitfix = 0;
                        if (s_fitfix++ < 8)
                            printf("[GX] stride repair at 0x%zX: count=%u %zu -> %zu "
                                   "bytes (payload did not fit; cp50=%08X cp60=%08X)\n",
                                   offset, vtx_count, vsize, fixed,
                                   g_state.cp[0x50], g_state.cp[0x60]);
                        vsize = fixed;
                        need = 3 + (size_t)vtx_count * vsize;
                    }
                }
            }
            if (offset + need > fifo_size) {
                // Incomplete payload. Normally the GP simply has not written the
                // rest yet, so wait for the next tick. But if this same position
                // keeps failing while the buffer keeps growing, the opcode was
                // misread (we lost framing upstream) and waiting will never
                // help -- a bogus 16-bit count can demand hundreds of KB and
                // jam the whole FIFO, freezing every command behind it. In that
                // case step past the byte and rescan for the next command.
                static size_t s_stall_off = (size_t)-1;
                static int s_stall_ticks = 0;
                // Only the main stream is retried across ticks. A display list is
                // a self-contained buffer: if its tail is truncated there is
                // nothing to wait for, and letting it drive the shared counter
                // would resync the main stream spuriously.
                if (depth > 0) break;
                if (s_stall_off != offset) {
                    s_stall_off = offset;
                    s_stall_ticks = 0;
                    // NWII_FINDMEM: search for the failing bytes in guest RAM on
                    // the very first stall. If the engine builds this data in
                    // memory and copies it out, the copy is still live now (four
                    // ticks later it is usually overwritten) and the bytes just
                    // before it in RAM reveal what the stream should contain.
                    static const bool findmem =
                        std::getenv("NWII_FINDMEM") != nullptr;
                    static int s_found = 0;
                    if (findmem && s_found < 6 && offset + 16 <= fifo_size &&
                        nwii::runtime::g_mmu) {
                        ++s_found;
                        const uint8_t *sig = &fifo[offset];
                        for (int region = 0; region < 2; ++region) {
                            uint32_t rbase = region ? 0x10000000u : 0u;
                            size_t rsize = region ? 0x04000000u : 0x01800000u;
                            const uint8_t *p =
                                nwii::runtime::g_mmu->get_ptr(0x80000000u | rbase);
                            if (!p) continue;
                            for (size_t i = 0; i + 16 < rsize; i += 4) {
                                if (p[i] != sig[0] || p[i + 1] != sig[1] ||
                                    p[i + 2] != sig[2] || p[i + 3] != sig[3])
                                    continue;
                                if (std::memcmp(p + i, sig, 16) != 0) continue;
                                printf("[GXFIND] stall source %s+0x%zX:\n",
                                       region ? "MEM2" : "MEM1", i);
                                for (size_t k = (i > 64 ? i - 64 : 0); k < i + 32;
                                     k += 16) {
                                    printf("   %+5zd:", (ssize_t)k - (ssize_t)i);
                                    for (size_t j = k; j < k + 16; ++j)
                                        printf(" %02X", p[j]);
                                    printf("\n");
                                }
                                break;
                            }
                        }
                    }
                } else if (++s_stall_ticks >= 4) {
                    s_stall_off = (size_t)-1;
                    s_stall_ticks = 0;
                    // NWII_FINDMEM: if the bytes at the failure point also exist
                    // in guest RAM, they are being copied into the FIFO from a
                    // buffer -- and that buffer shows what the stream should have
                    // contained (e.g. the draw header we seem to be missing).
                    static const bool findmem =
                        std::getenv("NWII_FINDMEM") != nullptr;
                    static int s_found = 0;
                    if (findmem && s_found < 3 && offset + 16 <= fifo_size &&
                        nwii::runtime::g_mmu) {
                        ++s_found;
                        const uint8_t *sig = &fifo[offset];
                        for (int region = 0; region < 2; ++region) {
                            uint32_t rbase = region ? 0x10000000u : 0u;
                            size_t rsize = region ? 0x04000000u : 0x01800000u;
                            const uint8_t *p =
                                nwii::runtime::g_mmu->get_ptr(0x80000000u | rbase);
                            if (!p) continue;
                            for (size_t i = 0; i + 16 < rsize; i += 4) {
                                if (p[i] != sig[0] || p[i + 1] != sig[1] ||
                                    p[i + 2] != sig[2] || p[i + 3] != sig[3])
                                    continue;
                                if (std::memcmp(p + i, sig, 16) != 0) continue;
                                printf("[GXFIND] copy source %s+0x%zX:\n",
                                       region ? "MEM2" : "MEM1", i);
                                for (size_t k = (i > 64 ? i - 64 : 0); k < i + 32;
                                     k += 16) {
                                    printf("   %+5zd:", (ssize_t)k - (ssize_t)i);
                                    for (size_t j = k; j < k + 16; ++j)
                                        printf(" %02X", p[j]);
                                    printf("\n");
                                }
                                break;
                            }
                        }
                    }

                    // How many consecutive commands must validate before a
                    // resync point is trusted. A real state burst is long; a
                    // coincidental match inside float data is not.
                    static const int chain_len = []() {
                        if (const char *e = std::getenv("NWII_RESYNC_CHAIN"))
                            return std::atoi(e);
                        return 3;
                    }();
                    // NWII_FINDCTR: the engine keeps its own FIFO byte counter
                    // (obj+0x24, bumped by +4 per attribute and +8 per texcoord
                    // pair). Locate it once by scanning for a dword just above the
                    // number of bytes we have pushed, then report the delta at
                    // every later desync: a constant delta means our byte
                    // accounting matches the engine's, a drifting one means bytes
                    // are being invented or lost.
                    static const bool findctr =
                        std::getenv("NWII_FINDCTR") != nullptr;
                    static uint32_t s_ctr_phys = 0xFFFFFFFFu;
                    static int s_ctr_msgs = 0;
                    if (findctr && nwii::runtime::g_mmu) {
                        uint64_t ours = nwii::runtime::GX_WGP_Count();
                        if (s_ctr_phys == 0xFFFFFFFFu) {
                            int best = 0x7FFFFFFF;
                            for (int region = 0; region < 2; ++region) {
                                uint32_t rbase = region ? 0x10000000u : 0u;
                                uint32_t rsize = region ? 0x04000000u : 0x01800000u;
                                const uint8_t *p =
                                    nwii::runtime::g_mmu->get_ptr(0x80000000u | rbase);
                                if (!p) continue;
                                for (uint32_t i = 0; i + 4 < rsize; i += 4) {
                                    uint32_t v = ((uint32_t)p[i] << 24) |
                                                 ((uint32_t)p[i + 1] << 16) |
                                                 ((uint32_t)p[i + 2] << 8) | p[i + 3];
                                    if (v < ours || v > ours + 0x20000) continue;
                                    if ((int)(v - ours) < best) {
                                        best = (int)(v - ours);
                                        s_ctr_phys = rbase + i;
                                    }
                                }
                            }
                            printf("[GXCTR] candidate counter at phys 0x%08X, ours=%llu "
                                   "delta=%d\n",
                                   s_ctr_phys, (unsigned long long)ours, best);
                        } else {
                            const uint8_t *p = nwii::runtime::g_mmu->get_ptr(
                                0x80000000u | s_ctr_phys);
                            if (p && s_ctr_msgs++ < 12) {
                                uint32_t v = ((uint32_t)p[0] << 24) |
                                             ((uint32_t)p[1] << 16) |
                                             ((uint32_t)p[2] << 8) | p[3];
                                printf("[GXCTR] engine=%u ours=%llu delta=%lld\n", v,
                                       (unsigned long long)ours,
                                       (long long)v - (long long)ours);
                            }
                        }
                    }

                    size_t resume = ResyncFrom(fifo, offset + 1, true, chain_len);
                    const char *how = "state-chain";
                    if (resume == (size_t)-1) {
                        resume = ResyncFrom(fifo, offset + 1, true, 2);
                        how = "state-chain2";
                    }
                    if (resume == (size_t)-1) {
                        resume = ResyncFrom(fifo, offset + 1, false, 3);
                        how = "any-chain3";
                    }
                    if (resume == (size_t)-1) {
                        resume = ResyncToFrameEdge(fifo, offset + 1);
                        how = "frame-edge";
                    }
                    if (resume == (size_t)-1) resume = fifo_size;
                    if (resume > offset) diag::bump(kSlotResync, resume - offset);

                    // NWII_SYNTH_DRAW: the engine streams a block of vertex data
                    // here with no draw header of its own (the block is a whole
                    // number of vertices and ends exactly where state writes
                    // resume). Emit a strip for it so the geometry reaches the
                    // renderer instead of being discarded with the desync.
                    static const bool synth =
                        std::getenv("NWII_SYNTH_DRAW") != nullptr;
                    if (synth && resume > offset && resume <= fifo_size) {
                        size_t len = resume - offset;
                        // The block is raw float vertex data, but the VAT active at
                        // that point describes index16 vertices. Decoding is driven
                        // by the VAT, so a stride override alone still reads the
                        // bytes as indices. Build the format the data actually has:
                        // pos f32/XYZ + RGBA8 colour + tex f32/ST = 24 bytes.
                        VATSlot sv = g_state.vat[vat_idx];
                        if (std::getenv("NWII_SYNTH_DIRECT")) {
                            std::memset(&sv, 0, sizeof(sv));
                            sv.posMask = VtxAttrMask::Direct;
                            sv.posType = VtxAttrType::F32;
                            sv.posElements = true;
                            sv.clrMask[0] = VtxAttrMask::Direct;
                            sv.clrType[0] = (VtxAttrType)5; // RGBA8
                            sv.texMask[0] = VtxAttrMask::Direct;
                            sv.texType[0] = VtxAttrType::F32;
                            sv.texElements[0] = true;
                        }
                        size_t vs = VertexSize(sv);
                        if (const char *e = std::getenv("NWII_SYNTH_VSIZE")) {
                            size_t v = (size_t)std::atoi(e);
                            if (v) vs = v;
                        }
                        uint8_t prim = 0x90;
                        if (const char *e = std::getenv("NWII_SYNTH_PRIM"))
                            prim = (uint8_t)std::strtoul(e, nullptr, 0);
                        size_t cnt = vs ? len / vs : 0;
                        if (vs && len % vs == 0 && cnt >= 3 && cnt <= 0xFFFF) {
                            GXCommand c;
                            c.type = GXCommandType::DrawPrimitive;
                            c.prim_type = prim;
                            c.raw = std::make_shared<DrawRaw>();
                            c.raw->vat = sv;
                            c.raw->defPosMtxIdx = g_state.defPosMtxIdx;
                            std::memcpy(c.raw->defTexMtxIdx,
                                        g_state.defTexMtxIdx, 8);
                            std::memcpy(c.raw->arrayBase, g_state.arrayBase,
                                        sizeof(c.raw->arrayBase));
                            std::memcpy(c.raw->arrayStride, g_state.arrayStride,
                                        sizeof(c.raw->arrayStride));
                            c.raw->need_normal = false;
                            c.raw->count = (uint16_t)cnt;
                            c.raw->bytes.assign(fifo.begin() + offset,
                                                fifo.begin() + offset + cnt * vs);
                            printf("[GX] synth draw at 0x%zX: %zu bytes -> %zu "
                                   "verts (vsize=%zu)\n",
                                   offset, len, cnt, vs);
                            commands.push_back(std::move(c));
                        }
                    }
                    static int s_resyncs = 0;
                    if (s_resyncs++ < 8) {
                        const VATSlot &sv = g_state.vat[vat_idx];
                        printf("[GX] desync: cmd=%02X vat=%u count=%u vsize=%zu "
                               "needs %zu bytes > %zu; resync(%s) %zu -> %zu "
                               "[wgp exact=%llu off-page=%llu]\n",
                               cmd, vat_idx, vtx_count, vsize, need,
                               fifo_size - offset, how, offset, resume,
                               (unsigned long long)nwii::runtime::g_wgp_addr_exact,
                               (unsigned long long)nwii::runtime::g_wgp_addr_page);
                        printf("   vat: posMask=%d posType=%d posElems=%d posMI=%d | "
                               "nrmMask=%d nrmType=%d nrmElems=%d | "
                               "clr0 mask=%d type=%d | "
                               "tex0 mask=%d type=%d elems=%d\n",
                               (int)sv.posMask, (int)sv.posType,
                               (int)sv.posElements, (int)sv.posMatIdx,
                               (int)sv.nrmMask, (int)sv.nrmType,
                               (int)sv.nrmElements, (int)sv.clrMask[0],
                               (int)sv.clrType[0], (int)sv.texMask[0],
                               (int)sv.texType[0], (int)sv.texElements[0]);
                        for (int ci = 1; ci < 2; ++ci)
                            printf("   clr%d mask=%d type=%d\n", ci,
                                   (int)sv.clrMask[ci], (int)sv.clrType[ci]);
                        for (int t = 1; t < 8; ++t)
                            if ((int)sv.texMask[t])
                                printf("   tex%d mask=%d type=%d elems=%d\n", t,
                                       (int)sv.texMask[t], (int)sv.texType[t],
                                       (int)sv.texElements[t]);
                        printf("   raw cp50=%08X cp60=%08X\n", g_state.cp[0x50],
                               g_state.cp[0x60]);
                        printf("   raw vat%u regs:", vat_idx);
                        for (int r = 0; r < 8; ++r)
                            printf(" %02X=%08X", 0x70 + r,
                                   g_state.cp[0x70 + r]);
                        printf("\n   raw vatB:");
                        for (int r = 0; r < 4; ++r)
                            printf(" %02X=%08X", 0x80 + r,
                                   g_state.cp[0x80 + r]);
                        printf("\n");
                        if (s_resyncs <= 2) {
                            printf("   wgp page offsets used: ");
                            for (int b = 1; b < 1024 && b < 512; ++b)
                                if (nwii::runtime::g_wgp_addr_hist[b])
                                    printf("0x%03X:%u ", b * 4,
                                           nwii::runtime::g_wgp_addr_hist[b]);
                            printf("\n");
                        }
                        if (s_resyncs <= 3) {
                            // Persistent non-NOP history: shows whether a display
                            // list call (0x40) precedes the block, i.e. whether the
                            // draw header lives inside a list we expand.
                            printf("   history:");
                            int n = s_hist_n < 24 ? s_hist_n : 24;
                            for (int k = 0; k < n; ++k)
                                printf(" [%s]",
                                       s_hist[(s_hist_n - n + k) % 24].c_str());
                            printf("\n");
                        }
                    }
                    offset = resume;
                    if (offset >= fifo_size) break;
                    continue;
                }
                break; // incomplete: wait for more
            }

            auto sn0 = std::chrono::steady_clock::now();
            GXCommand c;
            c.type = GXCommandType::DrawPrimitive;
            c.prim_type = cmd & 0xF8;
            c.raw = std::make_shared<DrawRaw>();
            c.raw->vat = vat;
            c.raw->defPosMtxIdx = g_state.defPosMtxIdx;
            std::memcpy(c.raw->defTexMtxIdx, g_state.defTexMtxIdx, 8);
            // NWII_IDXHASH: checksum the position index array as it looks right
            // now, so the deferred decode can tell whether the guest has
            // overwritten it in the meantime.
            if ((vat.posMask == VtxAttrMask::Index8 ||
                 vat.posMask == VtxAttrMask::Index16) &&
                g_state.arrayBase[0] && g_state.arrayStride[0] &&
                nwii::runtime::g_mmu &&
                std::getenv("NWII_IDXHASH") != nullptr) {
                const uint32_t n =
                    std::min<uint32_t>(g_state.arrayStride[0] * 64u, 2048u);
                uint64_t h = 1469598103934665603ull;
                for (uint32_t k = 0; k < n; ++k) {
                    h ^= nwii::runtime::g_mmu->read8(g_state.arrayBase[0] + k);
                    h *= 1099511628211ull;
                }
                c.raw->idx_hash = h;
                c.raw->idx_base = g_state.arrayBase[0];
                c.raw->idx_len = n;
            }
            // Snapshot this draw's position/normal matrix from the parse-time
            // shadow, unless the draw carries a per-vertex matrix index (then
            // each vertex may use a different matrix — fall back to live).
            if (vat.posMatIdx) {
                // Per-vertex matrix index: freeze the entire bank, because the
                // vertices in this one draw will index different slots and the
                // renderer's live copy is whatever the frame ended up with.
                bool any = false;
                for (int k = 0; k < 256 && !any; ++k) any = g_parse_posmtx[k] != 0.0f;
                if (any) {
                    c.raw->snap_pos_bank.assign(g_parse_posmtx, g_parse_posmtx + 256);
                    c.raw->snap_nrm_bank.assign(g_parse_nrmmtx, g_parse_nrmmtx + 96);
                }
            } else {
                uint32_t pb = (uint32_t)g_state.defPosMtxIdx * 4;
                uint32_t nb = 3u * (g_state.defPosMtxIdx & 31);
                if (pb + 12 <= 256 && nb + 9 <= 96) {
                    const float* pm = &g_parse_posmtx[pb];
                    // Only trust the snapshot if the slot was actually written
                    // (a zero matrix = never loaded in the parse shadow) — else
                    // fall back to the renderer's live matrix.
                    bool nonzero = false;
                    for (int k = 0; k < 12 && !nonzero; ++k) nonzero = pm[k] != 0.0f;
                    if (nonzero) {
                        std::memcpy(c.raw->snap_pos_mtx, pm, 12 * sizeof(float));
                        std::memcpy(c.raw->snap_nrm_mtx, &g_parse_nrmmtx[nb], 9 * sizeof(float));
                        c.raw->have_snap_mtx = true;
                    }
                }
            }
            if (g_skindbg) {
                g_skin_draws_total++;
                if (vat.posMatIdx) g_skin_draws_pmidx++;
                if (g_state.defPosMtxIdx != 0) g_skin_defnz++;
                if (c.raw->have_snap_mtx) g_skin_snapped++;
            }
            // Normals only matter when a colour channel is lit (XF 0x100e/0x100f
            // bit1); decoding them otherwise is pure cost.
            c.raw->need_normal = ((g_state.xf[0x00E] >> 1) & 1) ||
                                 ((g_state.xf[0x00F] >> 1) & 1);
            std::memcpy(c.raw->arrayBase, g_state.arrayBase, sizeof(c.raw->arrayBase));
            std::memcpy(c.raw->arrayStride, g_state.arrayStride, sizeof(c.raw->arrayStride));
            c.raw->count = vtx_count;
            c.raw->bytes.assign(fifo.begin() + offset + 3, fifo.begin() + offset + need);
            static const bool vatdbg = std::getenv("NWII_VATDBG") != nullptr;
            if (vatdbg) {
                // Compact VAT signature: does the computed vsize land the next
                // byte on a plausible opcode? If not, vsize is wrong.
                uint8_t nb = (offset + need < fifo_size) ? fifo[offset + need] : 0;
                bool good = (nb==0||nb==0x08||nb==0x10||nb==0x61||nb==0x40||nb==0x48||
                             (nb>=0x20&&nb<=0x38)||(nb>=0x80&&nb<=0xBF));
                uint32_t sig = (vat.posMatIdx?1:0) | ((uint32_t)vat.posType<<1) |
                    ((uint32_t)vat.posElements<<4) | ((uint32_t)vat.nrmMask<<5) |
                    ((uint32_t)vat.nrmType<<7) | ((uint32_t)vat.nrmElements<<10) |
                    ((uint32_t)vat.clrMask[0]<<11) | ((uint32_t)vat.clrType[0]<<13) |
                    ((uint32_t)vat.texMask[0]<<16) | ((uint32_t)vat.texType[0]<<18);
                static std::map<uint32_t,std::pair<uint64_t,uint64_t>> sigs; // sig->(count,badnext)
                auto& e = sigs[sig]; e.first++;
                if(!good){ e.second++;
                    // scan forward for the next plausible opcode to learn the
                    // TRUE payload size, vs our computed 'need'.
                    size_t scan=offset+3;
                    while(scan<fifo_size && scan<offset+3+(size_t)vtx_count*64){
                        uint8_t b=fifo[scan];
                        if(b==0||b==0x08||b==0x10||b==0x61||b==0x40||b==0x48||
                           (b>=0x20&&b<=0x38)||(b>=0x80&&b<=0xBF)) break;
                        scan++;
                    }
                    size_t actual_payload=scan-(offset+3);
                    static int shown=0;
                    if(shown++<30 && vtx_count>0)
                        printf("[SZERR] vtx_count=%u our_vsize=%zu our_need=%zu actual_payload=%zu actual_per_vtx=%.2f nextbyte=0x%02X\n",
                            vtx_count, VertexSize(vat), need-3, actual_payload,
                            (double)actual_payload/vtx_count, scan<fifo_size?fifo[scan]:0);
                }
                static auto t0=std::chrono::steady_clock::now();
                auto now=std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now-t0).count()>=2){
                    t0=now;
                    printf("[VAT] sig(posMI,posT,posE,nrmM,nrmT,nrmE,clrM,clrT,texM,texT)=count/badnext vsize:\n");
                    for(auto&kv:sigs){uint32_t g=kv.first;
                        printf("  (%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)=%llu/%llu sz=%zu\n",
                        g&1,(g>>1)&7,(g>>4)&1,(g>>5)&3,(g>>7)&7,(g>>10)&1,(g>>11)&3,(g>>13)&7,(g>>16)&3,(g>>18)&7,
                        (unsigned long long)kv.second.first,(unsigned long long)kv.second.second,VertexSize(vat));}
                    sigs.clear();
                }
            }
            g_prof_draws++;
            g_prof_draw_bytes += need;

            // NWII_VATLOG: correlate the raw VAT descriptor dwords with the draw
            // that is about to consume them, so the field layout can be derived
            // from geometry whose size is known independently.
            static const bool vatlog = std::getenv("NWII_VATLOG") != nullptr;
            static int vatlog_n = 0;
            if (vatlog && vatlog_n < 120) {
                ++vatlog_n;
                printf("[VATLOG %3d] off=%zu op=%02X vat=%u count=%u vsize=%zu "
                       "cp50=%08X cp60=%08X A=%08X B=%08X C=%08X\n",
                       vatlog_n, offset, cmd, vat_idx, vtx_count, vsize,
                       g_state.cp[0x50], g_state.cp[0x60],
                       g_state.cp[0x70 + vat_idx],
                       g_state.cp[0x80 + vat_idx],
                       g_state.cp[0x90 + vat_idx]);
            }

            commands.push_back(std::move(c));
            offset += need;
            g_prof_snap_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - sn0).count();
        } else {
            g_prof_unknown++;
            diag::bump(kSlotUnknown);
            // NWII_DESYNC_CTX: the byte just past a command that was sized
            // wrongly is the first one that is not a plausible opcode. Report
            // that command -- its opcode, the size we consumed and the run that
            // led to it -- so the mis-sizing (not its fallout) is identified.
            static const bool desync_ctx =
                std::getenv("NWII_DESYNC_CTX") != nullptr;
            if (desync_ctx) {
                static int shown = 0;
                if (shown++ < 6) {
                    printf("[DESYNC-CTX %d] unknown opcode 0x%02X at 0x%zX:",
                           shown, cmd, offset);
                    int n = trace_ring_n < 6 ? trace_ring_n : 6;
                    for (int k = 0; k < n; ++k) {
                        const TraceEnt &e =
                            trace_ring[(trace_ring_n - n + k) % 12];
                        printf(" [0x%zX op=%02X len=%zu]", e.off, e.cmd, e.len);
                    }
                    printf("\n");
                    if (n > 0) {
                        const TraceEnt &e =
                            trace_ring[(trace_ring_n - 1) % 12];
                        printf("   last cmd op=%02X at 0x%zX len=%zu bytes:", e.cmd,
                               e.off, e.len);
                        for (size_t i = e.off; i < fifo_size && i < e.off + 24; ++i)
                            printf(" %02X", fifo[i]);
                        printf("\n");
                    }
                }
            }
            // NWII_DESYNC: histogram the byte the parser can't recognise, plus
            // the 3 bytes before it (where the previous command over/under-ran).
            static const bool desync = std::getenv("NWII_DESYNC") != nullptr;
            if (desync) {
                static uint64_t hist[256] = {0};
                static auto t0 = std::chrono::steady_clock::now();
                hist[fifo[offset]]++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 2) {
                    t0 = now;
                    printf("[DESYNC] top unknown opcodes:");
                    for (int pass = 0; pass < 6; pass++) {
                        int bi = 0; uint64_t bv = 0;
                        for (int i = 0; i < 256; i++) if (hist[i] > bv) { bv = hist[i]; bi = i; }
                        if (!bv) break;
                        printf(" 0x%02X:%llu", bi, (unsigned long long)bv);
                        hist[bi] = 0;
                    }
                    printf("\n");
                    for (int i = 0; i < 256; i++) hist[i] = 0;
                }
            }
            // Headerless vertex block: the engine streams a coherent run of
            // vertices with no draw command in front of it. Consume the whole
            // run and submit it as a draw, instead of stepping one byte into the
            // payload and turning the rest of the frame into a desync cascade.
            size_t block_stride = 0, block_count = 0;
            size_t block_len = 0;
            static const bool block_recovery = []() {
                const char *e = std::getenv("NWII_NO_BLOCK_RECOVERY");
                return !(e && e[0] == '1');
            }();
            size_t preferred = VertexSize(g_state.vat[g_last_draw_vat]);
            if (block_recovery)
                block_len = MatchVertexBlock(fifo, offset, preferred,
                                             &block_stride, &block_count);
            if (block_len) {
                diag::bump(kSlotHeaderless);
                static int s_blocks = 0;
                VATSlot sv;
                bool have_vat = BuildVatForStride(block_stride, sv);
                if (s_blocks++ < 8)
                    printf("[GX] headerless vertex block at 0x%zX: %zu bytes = %zu "
                           "verts x %zu (vat %s, last draw vat=%d)\n",
                           offset, block_len, block_count, block_stride,
                           have_vat ? "built" : "unavailable", g_last_draw_vat);
                if (have_vat) {
                    GXCommand c;
                    c.type = GXCommandType::DrawPrimitive;
                    // The primitive is not in the stream; a strip is what this
                    // engine emits through its batched path. NWII_SYNTH_PRIM
                    // overrides it when the guess is visibly wrong.
                    c.prim_type = 0x90;
                    if (const char *e = std::getenv("NWII_SYNTH_PRIM"))
                        c.prim_type = (uint8_t)std::strtoul(e, nullptr, 0);
                    c.raw = std::make_shared<DrawRaw>();
                    c.raw->vat = sv;
                    c.raw->defPosMtxIdx = g_state.defPosMtxIdx;
                    std::memcpy(c.raw->defTexMtxIdx, g_state.defTexMtxIdx, 8);
                    std::memcpy(c.raw->arrayBase, g_state.arrayBase,
                                sizeof(c.raw->arrayBase));
                    std::memcpy(c.raw->arrayStride, g_state.arrayStride,
                                sizeof(c.raw->arrayStride));
                    c.raw->need_normal = false;
                    c.raw->count = (uint16_t)block_count;
                    c.raw->bytes.assign(fifo.begin() + offset,
                                        fifo.begin() + offset + block_len);
                    commands.push_back(std::move(c));
                }
                offset += block_len;
                // The continuation invariant compares the previous command with
                // the byte at `offset`. Jumping over the block leaves that state
                // pointing into the block, which would report the recovery
                // itself as a framing error. Resynchronise it instead.
                trace_prev_off = offset;
                trace_prev_cmd = 0x00;
                trace_prev_depth = depth;
                continue;
            }
            offset++;
        }
    }
}

}
