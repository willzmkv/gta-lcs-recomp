#include "audio_system.h"
#include "openal_bridge.h"

#include <iostream>
#include <vector>

#if defined(LCS_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace lcs::audio {

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

    std::cout << "[Audio] Native SDL3 audio playback initialized (44100 Hz stereo 16-bit PCM).\n";
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
