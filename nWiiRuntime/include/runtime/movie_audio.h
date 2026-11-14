#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace nwii::runtime {

// Host-side audio for Bink movies. The guest Bink camera decodes video but its
// audio path currently delivers no samples (the AX stream voice is never
// started), so the soundtrack would be silent. When the guest reads a .bik,
// decode its audio track with ffmpeg, follow the read position and mix it into
// the SDL queue. NWII_MOVIEAUDIO=0 disables.
void movie_audio_on_disc_read(const std::string &host_path, uint64_t rel_offset,
                              uint64_t file_size);
void movie_audio_mix(int16_t *out, size_t frames);

// Called when the guest closes/switches away from a disc file.
void movie_audio_on_close(const std::string &host_path);

} // namespace nwii::runtime
