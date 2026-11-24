#include "audio_system.h"
#include "openal_bridge.h"

#include <algorithm>
#include <deque>
#include <iostream>
#include <mutex>
#include <vector>

#if defined(LCS_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace lcs::audio {

static std::mutex s_video_audio_mutex;
static std::deque<int16_t> s_video_pcm_fifo;

void queue_video_audio(const int16_t* interleaved_stereo_44100, size_t frame_count) {
    if (!interleaved_stereo_44100 || frame_count == 0) return;
    std::lock_guard<std::mutex> lock(s_video_audio_mutex);
    constexpr size_t kMaxBufferedSamples = 44100 * 2 * 2; // 2s stereo cap
    if (s_video_pcm_fifo.size() > kMaxBufferedSamples) {
        size_t drop = s_video_pcm_fifo.size() - kMaxBufferedSamples;
        drop &= ~size_t(1);
        s_video_pcm_fifo.erase(s_video_pcm_fifo.begin(), s_video_pcm_fifo.begin() + static_cast<ptrdiff_t>(drop));
    }
    const size_t n = frame_count * 2;
    s_video_pcm_fifo.insert(s_video_pcm_fifo.end(), interleaved_stereo_44100, interleaved_stereo_44100 + n);
}

void clear_video_audio() {
    std::lock_guard<std::mutex> lock(s_video_audio_mutex);
    s_video_pcm_fifo.clear();
}

#if defined(LCS_HAS_SDL3)
static SDL_AudioStream* s_audio_stream = nullptr;

static void SDLCALL audio_stream_callback(void* userdata, SDL_AudioStream* stream, int additional_amount, int total_amount) {
    (void)userdata;
    (void)total_amount;
    if (additional_amount <= 0) return;

    size_t frame_count = static_cast<size_t>(additional_amount) / (2 * sizeof(int16_t));
    if (frame_count == 0) return;

    static thread_local std::vector<int16_t> mix_buffer;
    mix_buffer.resize(frame_count * 2);

    OpenALBridge::instance().mix_samples(mix_buffer.data(), frame_count, 44100);

    {
        std::lock_guard<std::mutex> lock(s_video_audio_mutex);
        const size_t avail = std::min(mix_buffer.size(), s_video_pcm_fifo.size());
        for (size_t i = 0; i < avail; i++) {
            int32_t mixed = static_cast<int32_t>(mix_buffer[i]) + static_cast<int32_t>(s_video_pcm_fifo[i]);
            mix_buffer[i] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
        }
        if (avail > 0) {
            s_video_pcm_fifo.erase(s_video_pcm_fifo.begin(), s_video_pcm_fifo.begin() + static_cast<ptrdiff_t>(avail));
        }
    }

    SDL_PutAudioStreamData(stream, mix_buffer.data(), static_cast<int>(frame_count * 2 * sizeof(int16_t)));
}
#endif

bool init_audio_system() {
#if defined(LCS_HAS_SDL3)
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::cerr << "[Audio] Failed to initialize SDL3 audio subsystem: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = 44100;

    s_audio_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_stream_callback, nullptr);
    if (!s_audio_stream) {
        std::cerr << "[Audio] Failed to open SDL3 audio device stream: " << SDL_GetError() << "\n";
        return false;
    }

    if (!SDL_ResumeAudioStreamDevice(s_audio_stream)) {
        std::cerr << "[Audio] Failed to resume SDL3 audio device stream: " << SDL_GetError() << "\n";
    }

    SDL_AudioSpec src_spec{}, dst_spec{};
    if (SDL_GetAudioStreamFormat(s_audio_stream, &src_spec, &dst_spec)) {
        std::cout << "[Audio] SDL3 stream format: src=(" << src_spec.freq << " Hz, ch=" << src_spec.channels
                  << ", fmt=0x" << std::hex << static_cast<int>(src_spec.format) << std::dec
                  << ") -> dst=(" << dst_spec.freq << " Hz, ch=" << dst_spec.channels
                  << ", fmt=0x" << std::hex << static_cast<int>(dst_spec.format) << std::dec << ")\n";
    }
    return true;
#else
    std::cout << "[Audio] SDL3 not available. Audio output disabled.\n";
    return false;
#endif
}

void shutdown_audio_system() {
#if defined(LCS_HAS_SDL3)
    if (s_audio_stream) {
        SDL_DestroyAudioStream(s_audio_stream);
        s_audio_stream = nullptr;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    std::cout << "[Audio] Audio subsystem shut down.\n";
#endif
}

bool is_audio_initialized() {
#if defined(LCS_HAS_SDL3)
    return s_audio_stream != nullptr;
#else
    return false;
#endif
}

} // namespace lcs::audio
