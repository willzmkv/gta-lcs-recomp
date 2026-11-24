#pragma once

#include <cstddef>
#include <cstdint>

namespace lcs::audio {

bool init_audio_system();
void shutdown_audio_system();
bool is_audio_initialized();

void queue_video_audio(const int16_t* interleaved_stereo_44100, size_t frame_count);
void clear_video_audio();

} // namespace lcs::audio
