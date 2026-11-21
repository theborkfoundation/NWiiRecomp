#include "runtime/bink_video.h"
#include "runtime/cpu_context.h"

#ifdef NWII_HAVE_BINK_DECODER
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace nwii::runtime {
#ifdef NWII_HAVE_BINK_DECODER
namespace {
struct Decoder {
  AVCodecContext *codec = nullptr;
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  uint32_t handle = 0;
  uint32_t last_frame = 0;

  ~Decoder() {
    avcodec_free_context(&codec);
    av_frame_free(&frame);
    av_packet_free(&packet);
  }

  bool start(uint32_t width, uint32_t height) {
    avcodec_free_context(&codec);
    const AVCodec *bink = avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
    if (!bink || !frame || !packet) return false;
    codec = avcodec_alloc_context3(bink);
    if (!codec) return false;
    codec->codec_tag = MKTAG('B', 'I', 'K', 'i');
    codec->width = width;
    codec->height = height;
    codec->thread_count = 1;
    codec->extradata = static_cast<uint8_t *>(av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec->extradata) return false;
    codec->extradata_size = 4; // SOJE41's BIKi files have zero video flags.
    return avcodec_open2(codec, bink, nullptr) >= 0;
  }
};
struct Plane {
  uint8_t *dst;
  unsigned pitch, width, height;
};
} // namespace
#endif

bool decode_rayman_bink_frame(CPUContext &ctx) {
#ifdef NWII_HAVE_BINK_DECODER
  static const bool enabled = [] {
    const char *v = std::getenv("NWII_NATIVE_BINK");
    return !v || v[0] != '0';
  }();
  if (!enabled || ctx.lr != 0x8000e4a0) return false;
  auto &mem = ctx.mmu;
  const uint32_t handle = ctx.gpr[24];
  const uint32_t buffers = ctx.gpr[3];
  if (!handle || !buffers || !mem.range_is_ram(handle, 0xd4) ||
      !mem.range_is_ram(buffers, 0x78))
    return false;
  const uint32_t number = mem.read32(handle + 0x0c);
  const uint32_t slot = mem.read32(buffers + 0x14);
  const uint32_t width = mem.read32(handle);
  const uint32_t height = mem.read32(handle + 4);
  const uint32_t begin = ctx.gpr[6];
  const uint32_t size = ctx.gpr[8] - begin;
  // Only the full-colour, no-alpha, no dirty-map call used by this title.
  if (ctx.gpr[4] || ctx.gpr[10] != 0x0c000400 || mem.read32(handle + 0x24) ||
      mem.read32(handle + 0xd0) != buffers || slot > 1 || !number ||
      width != 640 || height != 456 || mem.read32(buffers + 4) != width ||
      mem.read32(buffers + 8) != height || mem.read32(buffers + 12) != 320 ||
      mem.read32(buffers + 16) != 232 || size < 4 || size > (1u << 20) ||
      size % 4 || !mem.range_is_ram(begin, size)) return false;

  // Guest slots hold Y, Cr, Cb. FFmpeg exposes Y, Cb, Cr. Bink processes
  // 8x8 blocks, including four extra chroma rows after the visible picture.
  std::array<Plane, 3> planes{};
  // The index names the previous (reference) frame. The SDK writes the
  // opposite slot when allocated, and uses the reference slot in-place
  // for single-buffer playback. It rotates the index in guest code.
  const uint32_t reference = buffers + slot * 0x30;
  const uint32_t alternate = buffers + (slot ^ 1u) * 0x30;
  const uint32_t base = mem.read32(alternate + 0x1c) ? alternate : reference;
  constexpr unsigned offsets[] = {0x1c, 0x34, 0x28};
  for (unsigned i = 0; i < planes.size(); ++i) {
    const uint32_t address = mem.read32(base + offsets[i]);
    const unsigned pitch = mem.read32(base + offsets[i] + 4);
    const unsigned w = i ? 320 : width;
    const unsigned h = i ? 232 : height;
    if (!address || pitch != mem.read32(reference + offsets[i] + 4) ||
        pitch < w || pitch > 4096 ||
        !mem.range_is_ram(address, (h - 1) * pitch + w)) return false;
    planes[i] = {mem.get_ptr(address), pitch, w, h};
  }
  static thread_local Decoder decoder;
  if (number == 1 || decoder.handle != handle || number != decoder.last_frame + 1) {
    if (number != 1 && ctx.gpr[7] != 1) return false;
    decoder.handle = 0;
    if (!decoder.start(width, height)) return false;
  }
  decoder.handle = 0; // A failed packet requires a fresh key frame.
  av_packet_unref(decoder.packet);
  if (av_new_packet(decoder.packet, size) < 0) return false;
  const uint8_t *src = mem.get_ptr(begin);
  // The Wii reader has already swapped each little-endian packet word.
  for (unsigned i = 0; i < size; i += 4)
    std::reverse_copy(src + i, src + i + 4, decoder.packet->data + i);
  av_frame_unref(decoder.frame);
  if (avcodec_send_packet(decoder.codec, decoder.packet) < 0 ||
      avcodec_receive_frame(decoder.codec, decoder.frame) < 0 ||
      decoder.frame->format != AV_PIX_FMT_YUV420P) return false;

  // Validate the padded final block row before changing any guest plane.
  for (unsigned i = 0; i < planes.size(); ++i) {
    const auto &p = planes[i];
    const auto *buf = av_frame_get_plane_buffer(decoder.frame, i);
    const int stride = decoder.frame->linesize[i];
    if (!buf || stride < int(p.width) || decoder.frame->data[i] < buf->data ||
        size_t(decoder.frame->data[i] - buf->data) +
            size_t(p.height - 1) * stride + p.width > buf->size) return false;
  }
  for (unsigned i = 0; i < planes.size(); ++i) {
    const auto &p = planes[i];
    for (unsigned y = 0; y < p.height; ++y)
      std::memcpy(p.dst + y * p.pitch, decoder.frame->data[i] +
                  y * decoder.frame->linesize[i], p.width);
  }
  decoder.handle = handle;
  decoder.last_frame = number;
  if (number == 1)
    std::fprintf(stderr, "[Bink] native decoder: %ux%u\n", width, height);
  ctx.gpr[3] = 100; // Percentage refreshed (the native path writes every block).
  return true;
#else
  (void)ctx;
  return false;
#endif
}
} // namespace nwii::runtime
