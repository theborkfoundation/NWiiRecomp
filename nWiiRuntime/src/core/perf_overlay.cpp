#include "runtime/perf_overlay.h"
#include "runtime/perf_counters.h"

#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/diagnostics.h"

#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

extern uint64_t g_stat_frames;
extern uint64_t g_stat_draws;
extern uint64_t g_stat_parse_us;
extern uint64_t g_stat_render_us;
extern uint64_t nwii_stat_shaders();

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;
namespace hw {
extern uint64_t g_vi_trigger_count;
}
} // namespace nwii::runtime

namespace nwii::runtime::perf {

std::atomic<uint64_t> texture_uploads{0};
std::atomic<uint64_t> texture_hits{0};
std::atomic<uint64_t> texture_misses{0};
std::atomic<uint64_t> callbacks{0};
std::atomic<uint64_t> context_restores{0};
std::atomic<uint64_t> reschedules{0};
std::atomic<uint64_t> fifo_bytes{0};
std::atomic<uint64_t> audio_frames{0};
std::atomic<uint32_t> audio_voices{0};
std::atomic<int> audio_peak{0};

namespace {

constexpr int kGlyphW = 5;
constexpr int kGlyphH = 7;
constexpr int kCell = 8;
constexpr int kAtlasW = 128;
constexpr int kAtlasH = 64;
constexpr int kLineH = 9;

const uint8_t kGlyphs[36][kGlyphW] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
    {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
    {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
    {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
    {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
};

const uint8_t kSpace[kGlyphW] = {0, 0, 0, 0, 0};
const uint8_t kColon[kGlyphW] = {0x00, 0x36, 0x36, 0x00, 0x00};
const uint8_t kDot[kGlyphW] = {0x00, 0x60, 0x60, 0x00, 0x00};
const uint8_t kDash[kGlyphW] = {0x08, 0x08, 0x08, 0x08, 0x08};
const uint8_t kSlash[kGlyphW] = {0x20, 0x10, 0x08, 0x04, 0x02};
const uint8_t kPercent[kGlyphW] = {0x23, 0x13, 0x08, 0x64, 0x62};
const uint8_t kLParen[kGlyphW] = {0x00, 0x1C, 0x22, 0x41, 0x00};
const uint8_t kRParen[kGlyphW] = {0x00, 0x41, 0x22, 0x1C, 0x00};
const uint8_t kPlus[kGlyphW] = {0x08, 0x08, 0x3E, 0x08, 0x08};
const uint8_t kEquals[kGlyphW] = {0x14, 0x14, 0x14, 0x14, 0x14};
const uint8_t kUnder[kGlyphW] = {0x40, 0x40, 0x40, 0x40, 0x40};
const uint8_t kQuestion[kGlyphW] = {0x02, 0x01, 0x51, 0x09, 0x06};

const uint8_t *glyph(char c) {
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9')
        return kGlyphs[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return kGlyphs[10 + c - 'A'];
    switch (c) {
    case ' ': return kSpace;
    case ':': return kColon;
    case '.': return kDot;
    case '-': return kDash;
    case '/': return kSlash;
    case '%': return kPercent;
    case '(': return kLParen;
    case ')': return kRParen;
    case '+': return kPlus;
    case '=': return kEquals;
    case '_': return kUnder;
    default: return kQuestion;
    }
}

struct Env {
    bool enabled = false;
    bool overlay = false;
    bool full = false;
    int interval_ms = 1000;
    std::string log_path;
};

const Env &env() {
    static const Env e = [] {
        Env out;
        const char *p = std::getenv("NWII_PERF");
        if (p && p[0] && std::strcmp(p, "0") && std::strcmp(p, "off")) {
            out.overlay = true;
            out.enabled = true;
            out.full = std::strstr(p, "full") != nullptr || std::strcmp(p, "2") == 0;
        }
        if (const char *l = std::getenv("NWII_PERF_LOG")) {
            if (l[0]) {
                out.log_path = l;
                out.enabled = true;
            }
        }
        if (const char *i = std::getenv("NWII_PERF_INTERVAL_MS")) {
            int v = std::atoi(i);
            if (v >= 100 && v <= 60000)
                out.interval_ms = v;
        }
        return out;
    }();
    return e;
}

struct Snapshot {
    double fps = 0.0;
    double frame_ms = 0.0;
    double p99_ms = 0.0;
    double mips = 0.0;
    uint64_t vi = 0;
    uint64_t draws = 0;
    double parse_ms = 0.0;
    double render_ms = 0.0;
    double wgp_mb = 0.0;
    double fifo_mb = 0.0;
    uint64_t tex_up = 0;
    uint64_t tex_hit = 0;
    uint64_t tex_miss = 0;
    uint64_t shaders = 0;
    uint64_t cb = 0;
    uint64_t ctx = 0;
    uint64_t rs = 0;
    uint64_t audio = 0;
    uint64_t voices = 0;
    int peak = 0;
    uint64_t bad = 0;
    uint64_t unk = 0;
    uint64_t vfix = 0;
    uint64_t hdr = 0;
    uint64_t dl = 0;
    uint64_t frames = 0;
};

Snapshot g_snapshot;

uint64_t take(uint64_t cur, uint64_t &prev) {
    uint64_t d = cur >= prev ? cur - prev : cur;
    prev = cur;
    return d;
}

uint64_t diag_delta(const char *name,
                    std::unordered_map<std::string, uint64_t> &prev) {
    size_t n = 0;
    const diag::Counter *c = diag::counters(n);
    for (size_t i = 0; i < n; ++i) {
        if (!c[i].name || std::strcmp(c[i].name, name) != 0)
            continue;
        uint64_t &p = prev[name];
        uint64_t d = c[i].count >= p ? c[i].count - p : c[i].count;
        p = c[i].count;
        return d;
    }
    return 0;
}

std::ofstream g_log;
bool g_log_headered = false;

void write_log(const Snapshot &s) {
    const Env &e = env();
    if (e.log_path.empty())
        return;
    if (!g_log.is_open()) {
        g_log.open(e.log_path, std::ios::app);
        if (!g_log)
            return;
        g_log.seekp(0, std::ios::end);
        g_log_headered = g_log.tellp() > 0;
    }
    if (!g_log_headered) {
        g_log << "fps,frame_ms,p99_ms,vi,draws,mips,parse_ms,render_ms,wgp_mb,"
                 "fifo_mb,tex_up,tex_hit,tex_miss,shaders,cb,ctx,rs,audio,voices,"
                 "peak,bad,unk,vfix,hdr,dl,frames\n";
        g_log_headered = true;
    }
    g_log << s.fps << ',' << s.frame_ms << ',' << s.p99_ms << ',' << s.vi << ','
          << s.draws << ',' << s.mips << ',' << s.parse_ms << ','
          << s.render_ms << ',' << s.wgp_mb << ',' << s.fifo_mb << ','
          << s.tex_up << ',' << s.tex_hit << ',' << s.tex_miss << ','
          << s.shaders << ',' << s.cb << ',' << s.ctx << ',' << s.rs << ','
          << s.audio << ',' << s.voices << ',' << s.peak << ',' << s.bad << ','
          << s.unk << ',' << s.vfix << ',' << s.hdr << ',' << s.dl << ','
          << s.frames << '\n';
    g_log.flush();
}

struct Vertex {
    float x, y, u, v;
};

GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_font_tex = 0;
GLint g_u_screen = -1, g_u_color = -1, g_u_tex = -1;
bool g_gl_ready = false;

GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[PERF] shader compile failed: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

bool init_gl() {
    if (g_gl_ready)
        return g_prog != 0;
    g_gl_ready = true;

    const bool es = Config::get().backend == Backend::OpenGLES;
    const char *vs = es
        ? "#version 300 es\n"
          "layout(location=0) in vec2 aPos;\n"
          "layout(location=1) in vec2 aUV;\n"
          "uniform vec2 uScreen;\n"
          "out vec2 vUV;\n"
          "void main(){ vUV=aUV; vec2 p=aPos/uScreen*2.0-1.0;"
          "gl_Position=vec4(p.x,-p.y,0.0,1.0); }\n"
        : "#version 330 core\n"
          "layout(location=0) in vec2 aPos;\n"
          "layout(location=1) in vec2 aUV;\n"
          "uniform vec2 uScreen;\n"
          "out vec2 vUV;\n"
          "void main(){ vUV=aUV; vec2 p=aPos/uScreen*2.0-1.0;"
          "gl_Position=vec4(p.x,-p.y,0.0,1.0); }\n";
    const char *fs = es
        ? "#version 300 es\n"
          "precision mediump float;\n"
          "in vec2 vUV;\n"
          "uniform sampler2D uTex;\n"
          "uniform vec4 uColor;\n"
          "out vec4 outColor;\n"
          "void main(){ float a=texture(uTex,vUV).r;"
          "outColor=vec4(uColor.rgb,uColor.a*a); }\n"
        : "#version 330 core\n"
          "in vec2 vUV;\n"
          "uniform sampler2D uTex;\n"
          "uniform vec4 uColor;\n"
          "out vec4 outColor;\n"
          "void main(){ float a=texture(uTex,vUV).r;"
          "outColor=vec4(uColor.rgb,uColor.a*a); }\n";

    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return false;
    }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, v);
    glAttachShader(g_prog, f);
    glLinkProgram(g_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetProgramInfoLog(g_prog, sizeof(log) - 1, nullptr, log);
        std::fprintf(stderr, "[PERF] program link failed: %s\n", log);
        glDeleteProgram(g_prog);
        g_prog = 0;
        return false;
    }
    g_u_screen = glGetUniformLocation(g_prog, "uScreen");
    g_u_color = glGetUniformLocation(g_prog, "uColor");
    g_u_tex = glGetUniformLocation(g_prog, "uTex");

    std::vector<uint8_t> atlas(kAtlasW * kAtlasH, 0);
    for (int ch = 32; ch < 96; ++ch) {
        int idx = ch - 32;
        int cx = (idx % 16) * kCell;
        int cy = (idx / 16) * kCell;
        const uint8_t *g = glyph((char)ch);
        for (int col = 0; col < kGlyphW; ++col) {
            for (int row = 0; row < kGlyphH; ++row) {
                if (g[col] & (1u << row))
                    atlas[(cy + row) * kAtlasW + cx + 1 + col] = 255;
            }
        }
    }
    glGenTextures(1, &g_font_tex);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlasW, kAtlasH, 0, GL_RED,
                 GL_UNSIGNED_BYTE, atlas.data());

    glGenVertexArrays(1, &g_vao);
    glGenBuffers(1, &g_vbo);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (void *)offsetof(Vertex, u));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(g_prog);
    glUniform1i(g_u_tex, 0);
    glUseProgram(0);
    return true;
}

void emit_text(std::vector<Vertex> &v, const std::string &s, float x, float y) {
    for (char c : s) {
        int idx = (unsigned char)c - 32;
        if (idx < 0 || idx >= 64 || c == ' ') {
            x += kCell;
            continue;
        }
        int cx = (idx % 16) * kCell;
        int cy = (idx / 16) * kCell;
        float u0 = (float)cx / kAtlasW;
        float v0 = (float)cy / kAtlasH;
        float u1 = (float)(cx + kCell) / kAtlasW;
        float v1 = (float)(cy + kCell) / kAtlasH;
        float x1 = x + kCell;
        float y1 = y + kCell;
        v.push_back({x, y, u0, v0});
        v.push_back({x1, y, u1, v0});
        v.push_back({x1, y1, u1, v1});
        v.push_back({x, y, u0, v0});
        v.push_back({x1, y1, u1, v1});
        v.push_back({x, y1, u0, v1});
        x += kCell;
    }
}

} // namespace

void on_present() {
    const Env &e = env();
    if (!e.enabled)
        return;

    static const auto boot = std::chrono::steady_clock::now();
    static auto last_frame = boot;
    static auto last_sample = boot;
    static uint64_t frames = 0;
    static std::array<double, 240> ring{};
    static size_t ring_n = 0, ring_i = 0;
    static uint64_t last_frames = 0, last_draws = 0, last_parse = 0,
                    last_render = 0, last_vi = 0, last_inst = 0;
    static uint64_t last_tex_up = 0, last_tex_hit = 0, last_tex_miss = 0;
    static uint64_t last_cb = 0, last_ctx = 0, last_rs = 0;
    static uint64_t last_fifo = 0, last_audio = 0;
    static std::unordered_map<std::string, uint64_t> prev_diag;

    const auto now = std::chrono::steady_clock::now();
    const double dt =
        std::chrono::duration<double, std::milli>(now - last_frame).count();
    last_frame = now;
    if (dt > 0.0 && dt < 500.0) {
        ring[ring_i] = dt;
        ring_i = (ring_i + 1) % ring.size();
        if (ring_n < ring.size())
            ++ring_n;
    }
    ++frames;

    const double elapsed =
        std::chrono::duration<double, std::milli>(now - last_sample).count();
    if (elapsed < (double)e.interval_ms)
        return;
    const double sec = elapsed / 1000.0;

    Snapshot s;
    s.fps = sec > 0.0 ? (double)frames / sec : 0.0;
    if (ring_n) {
        double sum = 0.0;
        std::array<double, 240> sorted{};
        for (size_t i = 0; i < ring_n; ++i) {
            sum += ring[i];
            sorted[i] = ring[i];
        }
        s.frame_ms = sum / (double)ring_n;
        std::sort(sorted.begin(), sorted.begin() + ring_n);
        s.p99_ms = sorted[(size_t)((ring_n - 1) * 0.99)];
    }

    s.frames = take(g_stat_frames, last_frames);
    s.draws = take(g_stat_draws, last_draws);
    s.parse_ms = (double)take(g_stat_parse_us, last_parse) / 1000.0;
    s.render_ms = (double)take(g_stat_render_us, last_render) / 1000.0;
    s.vi = take(nwii::runtime::hw::g_vi_trigger_count, last_vi);
    const uint64_t inst = nwii::runtime::g_ctx_ptr
                              ? nwii::runtime::g_ctx_ptr->inst_count
                              : 0;
    s.mips = sec > 0.0 ? (double)take(inst, last_inst) / sec / 1.0e6 : 0.0;

    s.tex_up = take(texture_uploads.load(std::memory_order_relaxed),
                    last_tex_up);
    s.tex_hit = take(texture_hits.load(std::memory_order_relaxed),
                     last_tex_hit);
    s.tex_miss = take(texture_misses.load(std::memory_order_relaxed),
                      last_tex_miss);
    s.cb = take(callbacks.load(std::memory_order_relaxed), last_cb);
    s.ctx = take(context_restores.load(std::memory_order_relaxed), last_ctx);
    s.rs = take(reschedules.load(std::memory_order_relaxed), last_rs);
    s.fifo_mb = (double)take(fifo_bytes.load(std::memory_order_relaxed),
                             last_fifo) / 1048576.0 / sec;
    s.audio = take(audio_frames.load(std::memory_order_relaxed), last_audio);
    s.voices = audio_voices.load(std::memory_order_relaxed);
    s.peak = audio_peak.exchange(0, std::memory_order_relaxed);
    s.shaders = nwii_stat_shaders();
    s.wgp_mb = (double)diag_delta("wgp.redirected_bytes", prev_diag) /
               1048576.0 / sec;
    s.bad = diag_delta("fifo.bad_continuation", prev_diag);
    s.unk = diag_delta("fifo.unknown_opcode", prev_diag);
    s.vfix = diag_delta("fifo.vsize_repaired", prev_diag);
    s.hdr = diag_delta("fifo.headerless_vertices", prev_diag);
    s.dl = diag_delta("dl.rejected", prev_diag);

    g_snapshot = s;
    write_log(s);

    frames = 0;
    last_sample = now;
}

void draw(int width, int height) {
    const Env &e = env();
    if (!e.overlay || width <= 0 || height <= 0)
        return;
    if (!init_gl())
        return;

    const Snapshot &s = g_snapshot;
    std::vector<std::pair<std::string, bool>> lines;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "FPS %.1f FRAME %.1f P99 %.1f",
                  s.fps, s.frame_ms, s.p99_ms);
    lines.push_back({buf, false});
    std::snprintf(buf, sizeof(buf), "VI %llu DRAWS %llu MIPS %.0f",
                  (unsigned long long)s.vi, (unsigned long long)s.draws,
                  s.mips);
    lines.push_back({buf, false});
    std::snprintf(buf, sizeof(buf), "PARSE %.1f RENDER %.1f WGP %.1f",
                  s.parse_ms, s.render_ms, s.wgp_mb);
    lines.push_back({buf, false});
    std::snprintf(buf, sizeof(buf), "FIFO %.1f TEX %llu/%llu SH %llu",
                  s.fifo_mb, (unsigned long long)s.tex_up,
                  (unsigned long long)(s.tex_hit + s.tex_miss),
                  (unsigned long long)s.shaders);
    lines.push_back({buf, false});
    std::snprintf(buf, sizeof(buf), "CB %llu CTX %llu RS %llu",
                  (unsigned long long)s.cb, (unsigned long long)s.ctx,
                  (unsigned long long)s.rs);
    lines.push_back({buf, false});
    std::snprintf(buf, sizeof(buf), "AUD %llu V %llu PK %d",
                  (unsigned long long)s.audio, (unsigned long long)s.voices,
                  s.peak);
    lines.push_back({buf, false});
    const uint64_t errs = s.bad + s.unk + s.vfix + s.hdr + s.dl;
    std::snprintf(buf, sizeof(buf),
                  "ERR BAD %llu UNK %llu VFIX %llu HDR %llu DL %llu",
                  (unsigned long long)s.bad, (unsigned long long)s.unk,
                  (unsigned long long)s.vfix, (unsigned long long)s.hdr,
                  (unsigned long long)s.dl);
    lines.push_back({buf, errs != 0});
    if (e.full) {
        std::snprintf(buf, sizeof(buf),
                      "FRAMES %llu TEX UP %llu HIT %llu MISS %llu",
                      (unsigned long long)s.frames,
                      (unsigned long long)s.tex_up,
                      (unsigned long long)s.tex_hit,
                      (unsigned long long)s.tex_miss);
        lines.push_back({buf, false});
    }

    size_t max_len = 0;
    for (const auto &l : lines)
        max_len = std::max(max_len, l.first.size());
    const int box_w = (int)max_len * kCell + 8;
    const int box_h = (int)lines.size() * kLineH + 8;

    GLint viewport[4] = {0, 0, 0, 0};
    GLint scissor[4] = {0, 0, 0, 0};
    GLint program = 0, vao = 0, array_buf = 0, tex = 0, active_tex = 0;
    GLint fb = 0, read_fb = 0, draw_fb = 0;
    GLint blend_src_rgb = GL_ONE, blend_dst_rgb = GL_ZERO;
    GLint blend_src_a = GL_ONE, blend_dst_a = GL_ZERO;
    GLint depth_func = GL_LESS, cull_mode = GL_BACK, front_face = GL_CCW;
    GLfloat clear_color[4] = {0, 0, 0, 0};
    const GLboolean depth_test = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean cull_face = glIsEnabled(GL_CULL_FACE);
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor_test = glIsEnabled(GL_SCISSOR_TEST);

    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
    glGetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_a);
    glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
    glGetIntegerv(GL_CULL_FACE_MODE, &cull_mode);
    glGetIntegerv(GL_FRONT_FACE, &front_face);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, height - (4 + box_h), box_w, box_h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.72f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(g_prog);
    glUniform2f(g_u_screen, (float)width, (float)height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_font_tex);
    glBindVertexArray(g_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);

    float y = 8.0f;
    for (const auto &line : lines) {
        std::vector<Vertex> verts;
        verts.reserve(line.first.size() * 6);
        emit_text(verts, line.first, 8.0f, y);
        if (!verts.empty()) {
            glBufferData(GL_ARRAY_BUFFER,
                         (GLsizeiptr)(verts.size() * sizeof(Vertex)),
                         verts.data(), GL_STREAM_DRAW);
            glUniform4f(g_u_color, line.second ? 1.0f : 1.0f,
                        line.second ? 0.35f : 1.0f,
                        line.second ? 0.35f : 1.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
        }
        y += kLineH;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    glUseProgram((GLuint)program);
    glBindVertexArray((GLuint)vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)array_buf);
    glActiveTexture((GLenum)active_tex);
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    glBlendFuncSeparate((GLenum)blend_src_rgb, (GLenum)blend_dst_rgb,
                        (GLenum)blend_src_a, (GLenum)blend_dst_a);
    glClearColor(clear_color[0], clear_color[1], clear_color[2],
                 clear_color[3]);
    glDepthFunc((GLenum)depth_func);
    glCullFace((GLenum)cull_mode);
    glFrontFace((GLenum)front_face);
    if (depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

} // namespace nwii::runtime::perf
