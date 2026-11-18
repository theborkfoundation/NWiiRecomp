#pragma once

#include <atomic>
#include <cstdint>

namespace nwii::runtime::perf {

extern std::atomic<uint64_t> texture_uploads;
extern std::atomic<uint64_t> texture_hits;
extern std::atomic<uint64_t> texture_misses;
extern std::atomic<uint64_t> callbacks;
extern std::atomic<uint64_t> context_restores;
extern std::atomic<uint64_t> reschedules;
extern std::atomic<uint64_t> fifo_bytes;
extern std::atomic<uint64_t> audio_frames;
extern std::atomic<uint32_t> audio_voices;
extern std::atomic<int> audio_peak;

} // namespace nwii::runtime::perf
