// Integration fixture for the extracted SOJE41 opening movie. No game data
// is distributed with the tests. Compare the three visible planes to FFmpeg.
#include "runtime/bink_video.h"
#include "runtime/cpu_context.h"

#include <cstdlib>
#include <fstream>

namespace nwii::runtime {
CPUContext *g_ctx_ptr = nullptr;
MMU *g_mmu = nullptr;
uint32_t g_debug_pc = 0;
}
extern "C" void run_game(nwii::runtime::CPUContext &) { std::abort(); }

static void require(bool ok) { if (!ok) std::abort(); }

static uint32_t little_word(std::istream &in) {
  uint8_t b[4];
  require(bool(in.read(reinterpret_cast<char *>(b), 4)));
  return uint32_t(b[3]) << 24 | uint32_t(b[2]) << 16 | uint32_t(b[1]) << 8 | b[0];
}

int main(int argc, char **argv) {
  using namespace nwii::runtime;
  require(argc == 2);
  std::ifstream movie(argv[1], std::ios::binary);
  require(little_word(movie) == 0x694b4942); // BIKi
  movie.seekg(56); // SOJE41 sample movie has one audio track.
  const uint32_t begin = little_word(movie) & ~1u;
  const uint32_t end = little_word(movie) & ~1u;
  movie.seekg(begin);
  const uint32_t audio_size = little_word(movie);
  require(end > begin + 4 && audio_size < end - begin - 4);
  movie.seekg(audio_size, std::ios::cur);

  CPUContext ctx;
  auto &mem = ctx.mmu;
  constexpr uint32_t handle = 0x80004000, buffers = 0x80005000, packet = 0x80010000;
  const uint32_t size = end - begin - audio_size - 4;
  require(size == 2380);
  for (unsigned i = 0; i < size; i += 4) mem.write32(packet + i, little_word(movie));
  ctx.lr = 0x8000e4a0;
  ctx.gpr[24] = handle;
  ctx.gpr[3] = buffers;
  ctx.gpr[6] = packet;
  ctx.gpr[7] = 1;
  ctx.gpr[8] = packet + size;
  ctx.gpr[10] = 0x0c000400;
  mem.write32(handle, 640);
  mem.write32(handle + 4, 456);
  mem.write32(handle + 12, 1);
  mem.write32(handle + 0xd0, buffers);
  mem.write32(buffers + 4, 640);
  mem.write32(buffers + 8, 456);
  mem.write32(buffers + 12, 320);
  mem.write32(buffers + 16, 232);

  constexpr unsigned offsets[] = {0x1c, 0x28, 0x34}; // Y, Cr, Cb
  constexpr uint32_t hashes[] = {0x4ef7f2a3, 0x95cd05df, 0xd8b01877};
  for (unsigned slot = 0; slot < 2; ++slot) {
    for (unsigned plane = 0; plane < 3; ++plane) {
      const uint32_t field = buffers + slot * 0x30 + offsets[plane];
      mem.write32(field, 0x80100000 + slot * 0x200000 + plane * 0x80000);
      mem.write32(field + 4, plane ? 320 : 640);
    }
  }
  require(decode_rayman_bink_frame(ctx));
  for (unsigned i = 0; i < 3; ++i) {
    const unsigned length = i ? 320 * 228 : 640 * 456;
    const auto *dst = mem.get_ptr(mem.read32(buffers + 0x30 + offsets[i]));
    const auto *ref = mem.get_ptr(mem.read32(buffers + offsets[i]));
    uint32_t hash = 2166136261u;
    for (unsigned j = 0; j < length; ++j) {
      require(ref[j] == 0);
      hash = (hash ^ dst[j]) * 16777619u;
    }
    require(hash == hashes[i]);
  }
  // A skipped inter-frame must go back through the original decoder.
  ctx.gpr[3] = buffers;
  ctx.gpr[7] = 0;
  mem.write32(handle + 12, 3);
  require(!decode_rayman_bink_frame(ctx) && ctx.gpr[3] == buffers);
}
