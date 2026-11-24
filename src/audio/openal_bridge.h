#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>

namespace lcs::cpu {
class HLEManager;
}

namespace lcs::audio {

// OpenAL / ALC Constants
constexpr int ALC_FALSE = 0;
constexpr int ALC_TRUE = 1;

constexpr int AL_FALSE = 0;
constexpr int AL_TRUE = 1;

constexpr int AL_SOURCE_RELATIVE = 0x202;
constexpr int AL_CONE_INNER_ANGLE = 0x1001;
constexpr int AL_CONE_OUTER_ANGLE = 0x1002;
constexpr int AL_PITCH = 0x1003;
constexpr int AL_POSITION = 0x1004;
constexpr int AL_DIRECTION = 0x1005;
constexpr int AL_VELOCITY = 0x1006;
constexpr int AL_LOOPING = 0x1007;
constexpr int AL_BUFFER = 0x1009;
constexpr int AL_GAIN = 0x100A;
constexpr int AL_MIN_GAIN = 0x100D;
constexpr int AL_MAX_GAIN = 0x100E;
constexpr int AL_ORIENTATION = 0x100F;
constexpr int AL_SOURCE_STATE = 0x1010;
constexpr int AL_INITIAL = 0x1011;
constexpr int AL_PLAYING = 0x1012;
constexpr int AL_PAUSED = 0x1013;
constexpr int AL_STOPPED = 0x1014;
constexpr int AL_BUFFERS_QUEUED = 0x1015;
constexpr int AL_BUFFERS_PROCESSED = 0x1016;
constexpr int AL_REFERENCE_DISTANCE = 0x1020;
constexpr int AL_ROLLOFF_FACTOR = 0x1021;
constexpr int AL_CONE_OUTER_GAIN = 0x1022;
constexpr int AL_MAX_DISTANCE = 0x1023;
constexpr int AL_SEC_OFFSET = 0x1024;
constexpr int AL_SAMPLE_OFFSET = 0x1025;
constexpr int AL_BYTE_OFFSET = 0x1026;
constexpr int AL_SOURCE_TYPE = 0x1027;
constexpr int AL_STATIC = 0x1028;
constexpr int AL_STREAMING = 0x1029;
constexpr int AL_UNDETERMINED = 0x1030;

constexpr int AL_FORMAT_MONO8 = 0x1100;
constexpr int AL_FORMAT_MONO16 = 0x1101;
constexpr int AL_FORMAT_STEREO8 = 0x1102;
constexpr int AL_FORMAT_STEREO16 = 0x1103;

constexpr int AL_NO_ERROR = 0;

struct ALBuffer {
    uint32_t id = 0;
    uint32_t format = AL_FORMAT_MONO16;
    uint32_t freq = 44100;
    uint32_t channels = 1;
    std::vector<int16_t> samples;
};

struct ALSource {
    uint32_t id = 0;
    int state = AL_INITIAL;
    bool looping = false;
    float pitch = 1.0f;
    float gain = 1.0f;
    float min_gain = 0.0f;
    float max_gain = 1.0f;
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float vel[3] = {0.0f, 0.0f, 0.0f};
    float dir[3] = {0.0f, 0.0f, 0.0f};
    bool source_relative = false;
    float ref_distance = 1.0f;
    float max_distance = 100000.0f;
    float rolloff_factor = 1.0f;

    std::deque<uint32_t> queue;
    size_t buffers_processed = 0;
    size_t queue_idx = 0;
    double sample_pos = 0.0;
    bool is_streaming = false;
    bool underran = false;

    // Smoothing & anti-click envelope state for mixer
    double smoothed_gain_l = -1.0;
    double smoothed_gain_r = -1.0;
    double smoothed_pitch = -1.0;
    size_t fade_in_remaining = 0;
    size_t fade_in_total = 0;
    size_t total_frames_played = 0;
};

struct ALFadeTail {
    std::vector<std::shared_ptr<const ALBuffer>> buffers;
    size_t queue_idx = 0;
    double sample_pos = 0.0;
    double pitch = 1.0;
    double gain_l = 0.0;
    double gain_r = 0.0;
    bool looping = false;
    bool repeat_last_buf = false;
    size_t frames_left = 0;
    size_t frames_total = 0;
};

struct ALListener {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float vel[3] = {0.0f, 0.0f, 0.0f};
    float orientation[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f}; // At (0,0,-1), Up (0,1,0)
    float gain = 1.0f;
};

class OpenALBridge {
public:
    static OpenALBridge& instance();

    void mix_samples(int16_t* out_interleaved, size_t frame_count, uint32_t out_rate = 44100);

    // Context & Device
    uint64_t alc_open_device(const char* name);
    uint64_t alc_create_context(uint64_t device, const int32_t* attrlist);
    int alc_make_context_current(uint64_t context);
    void alc_process_context(uint64_t context);
    void alc_suspend_context(uint64_t context);

    // Global
    int al_get_error();
    void al_distance_model(int model);

    // Listener
    void al_listener_f(int param, float value);
    void al_listener_3f(int param, float v1, float v2, float v3);
    void al_listener_fv(int param, const float* values);

    // Buffers
    void al_gen_buffers(int n, uint32_t* buffers);
    void al_delete_buffers(int n, const uint32_t* buffers);
    void al_buffer_data(uint32_t buffer, int format, const void* data, int size, int freq);

    // Sources
    void al_gen_sources(int n, uint32_t* sources);
    void al_source_i(uint32_t source, int param, int value);
    void al_source_f(uint32_t source, int param, float value);
    void al_source_3f(uint32_t source, int param, float v1, float v2, float v3);
    void al_source_play(uint32_t source);
    void al_source_pause(uint32_t source);
    void al_source_stop(uint32_t source);
    void al_source_queue_buffers(uint32_t source, int nb, const uint32_t* buffers);
    void al_source_unqueue_buffers(uint32_t source, int nb, uint32_t* buffers);
    void al_get_source_i(uint32_t source, int param, int* value);

private:
    OpenALBridge() = default;
    ~OpenALBridge() = default;

    std::recursive_mutex m_mutex;
    std::unordered_map<uint32_t, std::shared_ptr<ALBuffer>> m_buffers;
    std::unordered_map<uint32_t, ALSource> m_sources;
    std::vector<ALFadeTail> m_fade_tails;
    ALListener m_listener;

    bool is_mono_sfx_source(const ALSource& src) const;
    void enqueue_fade_tail(const ALSource& src);
    uint32_t m_next_buffer_id = 1;
    uint32_t m_next_source_id = 1;
    int m_distance_model = 0xD002; // AL_INVERSE_DISTANCE_CLAMPED
};

bool is_openal_symbol(std::string_view name);
void register_openal_hle(lcs::cpu::HLEManager& hle);
void register_openal_stubs();

} // namespace lcs::audio
