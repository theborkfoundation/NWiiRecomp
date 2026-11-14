// Host-side movie audio: decode the .bik soundtrack with ffmpeg and mix it
// into the SDL output while the guest is reading that file.

#include "runtime/movie_audio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nwii::runtime {
namespace {

constexpr int kRate = 32000;
constexpr int kChannels = 2;

struct MovieAudio {
  std::mutex mutex;
  std::string path;
  std::vector<int16_t> pcm;
  size_t total_frames = 0;
  std::atomic<bool> ready{false};
  std::atomic<bool> active{false};
  std::atomic<bool> decoding{false};
  size_t pos = 0;
  std::chrono::steady_clock::time_point last_read{};
  std::chrono::steady_clock::time_point start_time{};
  double read_fraction = 0.0;
  double logged_fraction = 0.0;
  bool have_fraction = false;

  bool enabled = true;

  MovieAudio() {
    const char *e = std::getenv("NWII_MOVIEAUDIO");
    enabled = !(e && e[0] == '0');
  }
};

MovieAudio &state() {
  static MovieAudio s;
  return s;
}

bool is_bik(const std::string &p) {
  return p.size() > 4 && p.compare(p.size() - 4, 4, ".bik") == 0;
}

void decode_async(const std::string &path) {
  std::thread([path]() {
    MovieAudio &s = state();
    std::string cmd = "ffmpeg -v error -i '" + path +
                      "' -f s16le -acodec pcm_s16le -ac 2 -ar 32000 - 2>/dev/null";
    FILE *p = popen(cmd.c_str(), "r");
    std::vector<int16_t> pcm;
    if (p) {
      int16_t buf[4096];
      size_t n;
      while ((n = fread(buf, sizeof(int16_t), 4096, p)) > 0)
        pcm.insert(pcm.end(), buf, buf + n);
      pclose(p);
    }
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!pcm.empty()) {
      s.pcm = std::move(pcm);
      s.total_frames = s.pcm.size() / kChannels;
      s.pos = 0;
      s.ready = true;
      if (std::getenv("NWII_MOVIEAUDDBG"))
        std::printf("[MovieAudio] decoded %s: %zu frames\n", path.c_str(),
                    s.total_frames);
    }
    s.decoding = false;
  }).detach();
}

} // namespace

void movie_audio_on_disc_read(const std::string &host_path, uint64_t rel_offset,
                              uint64_t file_size) {
  MovieAudio &s = state();
  if (!s.enabled || !is_bik(host_path))
    return;

  std::lock_guard<std::mutex> lock(s.mutex);
  const double frac =
      file_size ? std::min(1.0, (double)rel_offset / (double)file_size) : 0.0;

  if (s.path != host_path) {
    s.path = host_path;
    s.pcm.clear();
    s.total_frames = 0;
    s.ready = false;
    s.active = false;
    s.pos = 0;
    s.have_fraction = false;
    s.start_time = std::chrono::steady_clock::now();
    s.logged_fraction = 0.0;
    if (!s.decoding) {
      s.decoding = true;
      decode_async(host_path);
    }
  }

  s.last_read = std::chrono::steady_clock::now();
  s.active = true;

  // Keep playback near the guest's read position (the Bink reader stays close
  // to the frame it is decoding). Snap only on large drift.
  if (s.ready && s.total_frames) {
    const size_t expected = (size_t)(frac * (double)s.total_frames);
    const size_t drift =
        expected > s.pos ? expected - s.pos : s.pos - expected;
    if (drift > (size_t)kRate / 2)
      s.pos = std::min(expected, s.total_frames);
    s.have_fraction = true;
    s.read_fraction = frac;
    if (std::getenv("NWII_MOVIEAUDDBG") && frac - s.logged_fraction >= 0.1) {
      s.logged_fraction = frac;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - s.start_time)
                          .count();
      std::printf("[MovieAudio] t=%lldms read_frac=%.2f pos=%zu/%zu\n",
                  (long long)ms, frac, s.pos, s.total_frames);
    }
  }
}

void movie_audio_on_close(const std::string &host_path) {
  MovieAudio &s = state();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (s.path == host_path) {
    s.active = false;
    if (std::getenv("NWII_MOVIEAUDDBG"))
      std::printf("[MovieAudio] stop %s\n", host_path.c_str());
  }
}

void movie_audio_mix(int16_t *out, size_t frames) {
  MovieAudio &s = state();
  if (!s.enabled)
    return;

  std::lock_guard<std::mutex> lock(s.mutex);
  if (!s.active || !s.ready || s.total_frames == 0)
    return;

  if (s.pos >= s.total_frames) {
    s.active = false;
    return;
  }
  static size_t dbg_frames = 0;
  if (std::getenv("NWII_MOVIEAUDDBG")) {
    dbg_frames += frames;
    if (dbg_frames >= (size_t)kRate) {
      dbg_frames = 0;
      std::printf("[MovieAudio] mix pos=%zu/%zu\n", s.pos, s.total_frames);
    }
  }

  for (size_t i = 0; i < frames && s.pos < s.total_frames; ++i, ++s.pos) {
    for (int c = 0; c < kChannels; ++c) {
      const int32_t mixed =
          (int32_t)out[i * kChannels + c] +
          (int32_t)s.pcm[s.pos * kChannels + c];
      out[i * kChannels + c] =
          (int16_t)std::clamp<int32_t>(mixed, -0x8000, 0x7FFF);
    }
  }
}

} // namespace nwii::runtime
