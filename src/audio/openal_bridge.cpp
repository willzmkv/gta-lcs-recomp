#include "openal_bridge.h"
#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"

#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_set>

namespace lcs::audio {

OpenALBridge& OpenALBridge::instance() {
    static OpenALBridge s_instance;
    return s_instance;
}

uint64_t OpenALBridge::alc_open_device(const char* name) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::cout << "[Audio] alcOpenDevice(\"" << (name ? name : "default") << "\") -> handle=0x1000\n";
    return 0x1000ULL; // Dummy valid ALCdevice pointer
}

uint64_t OpenALBridge::alc_create_context(uint64_t device, const int32_t* attrlist) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::cout << "[Audio] alcCreateContext(device=0x" << std::hex << device << std::dec << ") -> context=0x2000\n";
    (void)attrlist;
    return 0x2000ULL; // Dummy valid ALCcontext pointer
}

int OpenALBridge::alc_make_context_current(uint64_t context) {
    (void)context;
    return ALC_TRUE;
}

void OpenALBridge::alc_process_context(uint64_t context) {
    (void)context;
}

void OpenALBridge::alc_suspend_context(uint64_t context) {
    (void)context;
}

int OpenALBridge::al_get_error() {
    return AL_NO_ERROR;
}

void OpenALBridge::al_distance_model(int model) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_distance_model = model;
}

void OpenALBridge::al_listener_f(int param, float value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (param == AL_GAIN) {
        m_listener.gain = std::max(0.0f, value);
    }
}

void OpenALBridge::al_listener_3f(int param, float v1, float v2, float v3) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (param == AL_POSITION) {
        m_listener.pos[0] = v1;
        m_listener.pos[1] = v2;
        m_listener.pos[2] = v3;
    } else if (param == AL_VELOCITY) {
        m_listener.vel[0] = v1;
        m_listener.vel[1] = v2;
        m_listener.vel[2] = v3;
    }
}

void OpenALBridge::al_listener_fv(int param, const float* values) {
    if (!values) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (param == AL_ORIENTATION) {
        for (int i = 0; i < 6; i++) {
            m_listener.orientation[i] = values[i];
        }
    } else if (param == AL_POSITION) {
        m_listener.pos[0] = values[0];
        m_listener.pos[1] = values[1];
        m_listener.pos[2] = values[2];
    } else if (param == AL_VELOCITY) {
        m_listener.vel[0] = values[0];
        m_listener.vel[1] = values[1];
        m_listener.vel[2] = values[2];
    }
}

void OpenALBridge::al_gen_buffers(int n, uint32_t* buffers) {
    if (n <= 0 || !buffers) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (int i = 0; i < n; i++) {
        uint32_t id = m_next_buffer_id++;
        m_buffers[id] = std::make_shared<ALBuffer>(ALBuffer{id, AL_FORMAT_MONO16, 44100, 1, {}});
        buffers[i] = id;
    }
}

void OpenALBridge::al_delete_buffers(int n, const uint32_t* buffers) {
    if (n <= 0 || !buffers) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (int i = 0; i < n; i++) {
        m_buffers.erase(buffers[i]);
    }
}

void OpenALBridge::al_buffer_data(uint32_t buffer, int format, const void* data, int size, int freq) {
    if (!data || size <= 0) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto buf_ptr = std::make_shared<ALBuffer>();
    ALBuffer& buf = *buf_ptr;
    buf.id = buffer;
    buf.format = static_cast<uint32_t>(format);
    buf.freq = (freq > 0) ? static_cast<uint32_t>(freq) : 44100;

    if (format == AL_FORMAT_MONO16) {
        buf.channels = 1;
        size_t samples_count = static_cast<size_t>(size) / sizeof(int16_t);
        buf.samples.resize(samples_count);
        std::memcpy(buf.samples.data(), data, samples_count * sizeof(int16_t));
    } else if (format == AL_FORMAT_STEREO16) {
        buf.channels = 2;
        size_t samples_count = static_cast<size_t>(size) / sizeof(int16_t);
        buf.samples.resize(samples_count);
        std::memcpy(buf.samples.data(), data, samples_count * sizeof(int16_t));
    } else if (format == AL_FORMAT_MONO8) {
        buf.channels = 1;
        const uint8_t* u8 = reinterpret_cast<const uint8_t*>(data);
        buf.samples.resize(size);
        for (int i = 0; i < size; i++) {
            buf.samples[i] = static_cast<int16_t>((static_cast<int>(u8[i]) - 128) << 8);
        }
    } else if (format == AL_FORMAT_STEREO8) {
        buf.channels = 2;
        const uint8_t* u8 = reinterpret_cast<const uint8_t*>(data);
        buf.samples.resize(size);
        for (int i = 0; i < size; i++) {
            buf.samples[i] = static_cast<int16_t>((static_cast<int>(u8[i]) - 128) << 8);
        }
    } else if (format == 0x10010 || format == 0x10011) {
        // AL_FORMAT_MONO_FLOAT32 / AL_FORMAT_STEREO_FLOAT32
        buf.channels = (format == 0x10011) ? 2 : 1;
        size_t samples_count = static_cast<size_t>(size) / sizeof(float);
        const float* f32 = reinterpret_cast<const float*>(data);
        buf.samples.resize(samples_count);
        for (size_t i = 0; i < samples_count; i++) {
            float clamped = std::clamp(f32[i], -1.0f, 1.0f);
            buf.samples[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
    } else {
        buf.samples.clear();
        static int s_unknown_fmt_warn = 0;
        if (s_unknown_fmt_warn++ < 8) {
            std::cerr << "[Audio] Warning: Unsupported alBufferData format 0x" << std::hex << format
                      << std::dec << " (size=" << size << ", freq=" << freq << ")\n";
        }
    }

    static size_t s_buf_data_count = 0;
    if (s_buf_data_count++ < 15) {
        int32_t min_s = 0, max_s = 0;
        if (!buf.samples.empty()) {
            min_s = buf.samples[0];
            max_s = buf.samples[0];
            for (int16_t s : buf.samples) {
                if (s < min_s) min_s = s;
                if (s > max_s) max_s = s;
            }
        }
        std::cout << "[Audio] alBufferData(buffer=" << buffer << ", fmt=0x" << std::hex << format
                  << std::dec << ", sz=" << size << ", freq=" << freq << ", ch=" << buf.channels
                  << ", peak=[" << min_s << ".." << max_s << "])\n";
    }
    m_buffers[buffer] = std::move(buf_ptr);
}

void OpenALBridge::al_gen_sources(int n, uint32_t* sources) {
    if (n <= 0 || !sources) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    for (int i = 0; i < n; i++) {
        uint32_t id = m_next_source_id++;
        m_sources[id] = ALSource{id};
        sources[i] = id;
    }
}

void OpenALBridge::al_source_i(uint32_t source, int param, int value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    switch (param) {
        case AL_BUFFER:
            if (src.state == AL_PLAYING) {
                enqueue_fade_tail(src);
            }
            src.queue.clear();
            if (value != 0) {
                src.queue.push_back(static_cast<uint32_t>(value));
            } else {
                // StopChannel/InitialiseChannel/Service clear AL_BUFFER=0 to reset a reused source
                // without always re-setting AL_LOOPING=0 afterwards.
                src.looping = false;
            }
            src.buffers_processed = 0;
            src.queue_idx = 0;
            src.sample_pos = 0.0;
            src.is_streaming = false;
            src.underran = false;
            src.smoothed_gain_l = -1.0;
            src.smoothed_gain_r = -1.0;
            src.smoothed_pitch = -1.0;
            src.fade_in_remaining = 0;
            src.fade_in_total = 0;
            src.total_frames_played = 0;
            break;
        case AL_LOOPING:
            src.looping = (value == AL_TRUE);
            break;
        case AL_SOURCE_RELATIVE:
            src.source_relative = (value == AL_TRUE);
            break;
        default:
            break;
    }
}

void OpenALBridge::al_source_f(uint32_t source, int param, float value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    switch (param) {
        case AL_PITCH:
            src.pitch = std::max(0.01f, value);
            break;
        case AL_GAIN:
            src.gain = std::max(0.0f, value);
            break;
        case AL_MIN_GAIN:
            src.min_gain = std::max(0.0f, value);
            break;
        case AL_MAX_GAIN:
            src.max_gain = std::max(0.0f, value);
            break;
        case AL_REFERENCE_DISTANCE:
            src.ref_distance = std::max(0.01f, value);
            break;
        case AL_MAX_DISTANCE:
            src.max_distance = std::max(0.01f, value);
            break;
        case AL_ROLLOFF_FACTOR:
            src.rolloff_factor = std::max(0.0f, value);
            break;
        default:
            break;
    }
}

void OpenALBridge::al_source_3f(uint32_t source, int param, float v1, float v2, float v3) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    if (param == AL_POSITION) {
        src.pos[0] = v1;
        src.pos[1] = v2;
        src.pos[2] = v3;
    } else if (param == AL_VELOCITY) {
        src.vel[0] = v1;
        src.vel[1] = v2;
        src.vel[2] = v3;
    } else if (param == AL_DIRECTION) {
        src.dir[0] = v1;
        src.dir[1] = v2;
        src.dir[2] = v3;
    }
}

bool OpenALBridge::is_mono_sfx_source(const ALSource& src) const {
    if (src.queue.empty()) return false;
    const size_t idx = (src.queue_idx < src.queue.size()) ? src.queue_idx : 0;
    auto bit = m_buffers.find(src.queue[idx]);
    return bit != m_buffers.end() && bit->second && bit->second->channels == 1;
}

void OpenALBridge::enqueue_fade_tail(const ALSource& src) {
    // Only skip non-mono ring-buffered streams (e.g. stereo MP3Stream radio/cutscenes whose buffers are overwritten live).
    // Mono cached banks (including cSampleManager [A, B, B, B, B] engine accel queues) must smoothly fade out!
    if (src.state != AL_PLAYING || src.queue.empty() || src.queue_idx >= src.queue.size() || !is_mono_sfx_source(src)) {
        return;
    }
    const double base_gain = static_cast<double>(std::clamp(src.gain, src.min_gain, src.max_gain));
    const double gl = (src.smoothed_gain_l >= 0.0) ? src.smoothed_gain_l : base_gain;
    const double gr = (src.smoothed_gain_r >= 0.0) ? src.smoothed_gain_r : base_gain;
    if (gl <= 0.0005 && gr <= 0.0005) {
        return;
    }

    const bool is_loop_or_engine = src.looping ||
        (src.queue.size() >= 2 && src.queue.back() == src.queue[src.queue.size() - 2]);
    constexpr size_t kMaxFadeTails = 24;
    if (m_fade_tails.size() >= kMaxFadeTails) {
        m_fade_tails.erase(m_fade_tails.begin());
    }

    ALFadeTail tail;
    const size_t start_idx = src.queue_idx;
    const size_t copy_end = std::min(src.queue.size(), start_idx + 8);
    tail.buffers.reserve(copy_end - start_idx);
    for (size_t qi = start_idx; qi < copy_end; qi++) {
        auto bit = m_buffers.find(src.queue[qi]);
        tail.buffers.push_back((bit != m_buffers.end()) ? bit->second : nullptr);
    }
    if (tail.buffers.empty() || !tail.buffers[0]) {
        return;
    }
    const size_t cur_buf_frames =
        tail.buffers[0]->samples.size() / std::max<uint32_t>(1u, tail.buffers[0]->channels);
    // Long (>0.5s) 1-shot engine rev/shift buffers interrupted early need ~10ms taper so low frequencies don't thump.
    const bool interrupted_long_rev =
        !is_loop_or_engine && cur_buf_frames >= 22050 &&
        (src.sample_pos + 441.0 < static_cast<double>(cur_buf_frames));
    const size_t fade_out_frames = is_loop_or_engine ? 794 : (interrupted_long_rev ? 441 : 110);
    tail.queue_idx = 0;
    tail.sample_pos = src.sample_pos;
    tail.pitch = (src.smoothed_pitch > 0.0) ? src.smoothed_pitch : static_cast<double>(src.pitch);
    tail.gain_l = gl;
    tail.gain_r = gr;
    tail.looping = src.looping;
    tail.repeat_last_buf = is_loop_or_engine && (tail.buffers.size() >= 2) &&
                           (tail.buffers.back() == tail.buffers[tail.buffers.size() - 2]);
    tail.frames_left = fade_out_frames;
    tail.frames_total = fade_out_frames;
    m_fade_tails.push_back(std::move(tail));
}

void OpenALBridge::al_source_play(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;
    // Preserve sharp transients on short 1-shot SFX, while smoothing loop/engine channels (~10 ms)
    // and long non-looping mono engine/rev buffers (>=16384 frames, ~4 ms anti-click start).
    constexpr size_t kLoopFadeInFrames = 441;
    constexpr size_t kLongMonoFadeInFrames = 176;
    const bool is_loop_channel = src.looping ||
        (src.queue.size() >= 2 && src.queue.back() == src.queue[src.queue.size() - 2]);
    size_t lead_frames = 0;
    if (!src.queue.empty()) {
        auto bit = m_buffers.find(src.queue.front());
        if (bit != m_buffers.end() && bit->second) {
            lead_frames = bit->second->samples.size() / std::max<uint32_t>(1u, bit->second->channels);
        }
    }
    const size_t fade_in = is_mono_sfx_source(src)
        ? (is_loop_channel ? kLoopFadeInFrames : (lead_frames >= 16384 ? kLongMonoFadeInFrames : 0))
        : 0;
    if (src.state == AL_PLAYING) {
        enqueue_fade_tail(src);
        src.sample_pos = 0.0;
        src.queue_idx = 0;
        src.buffers_processed = 0;
        src.smoothed_gain_l = -1.0;
        src.smoothed_gain_r = -1.0;
        src.smoothed_pitch = -1.0;
        src.fade_in_total = fade_in;
        src.fade_in_remaining = fade_in;
        src.total_frames_played = 0;
    } else if (src.state != AL_PAUSED) {
        src.sample_pos = 0.0;
        if (src.buffers_processed > 0 && src.buffers_processed < src.queue.size()) {
            src.queue_idx = src.buffers_processed;
        } else {
            src.queue_idx = 0;
            src.buffers_processed = 0;
        }
        src.smoothed_gain_l = -1.0;
        src.smoothed_gain_r = -1.0;
        src.smoothed_pitch = -1.0;
        src.fade_in_total = fade_in;
        src.fade_in_remaining = fade_in;
        src.total_frames_played = 0;
    }
    src.underran = false;
    src.state = AL_PLAYING;
    static size_t s_play_count = 0;
    if (s_play_count++ < 10) {
        std::cout << "[Audio] alSourcePlay(source=" << source << ", queue=" << src.queue.size()
                  << ", gain=" << src.gain << ", rel=" << src.source_relative << ")\n";
    }
}

void OpenALBridge::al_source_pause(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    it->second.underran = false;
    it->second.state = AL_PAUSED;
}

void OpenALBridge::al_source_stop(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    enqueue_fade_tail(it->second);
    it->second.underran = false;
    it->second.state = AL_STOPPED;
    it->second.sample_pos = 0.0;
    it->second.queue_idx = 0;
    it->second.smoothed_gain_l = -1.0;
    it->second.smoothed_gain_r = -1.0;
    it->second.smoothed_pitch = -1.0;
    it->second.fade_in_remaining = 0;
    it->second.fade_in_total = 0;
    it->second.total_frames_played = 0;
    if (it->second.is_streaming) {
        it->second.buffers_processed = it->second.queue.size();
    }
}

void OpenALBridge::al_source_queue_buffers(uint32_t source, int nb, const uint32_t* buffers) {
    if (nb <= 0 || !buffers) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    const size_t prev_size = src.queue.size();
    for (int i = 0; i < nb; i++) {
        src.queue.push_back(buffers[i]);
    }
    src.is_streaming = true;
    // cSampleManager::Service() replenishes looping engine channels without unqueuing or calling alSourcePlay().
    // If a frame stall starved the queue into AL_STOPPED, resume cleanly at the newly appended buffers.
    if (src.state == AL_STOPPED && src.underran && prev_size > 0) {
        src.queue_idx = prev_size;
        src.sample_pos = 0.0;
        src.underran = false;
        src.state = AL_PLAYING;
    }
}

void OpenALBridge::al_source_unqueue_buffers(uint32_t source, int nb, uint32_t* buffers) {
    if (nb <= 0 || !buffers) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    int popped = 0;
    while (popped < nb && !src.queue.empty()) {
        buffers[popped++] = src.queue.front();
        src.queue.pop_front();
    }
    src.underran = false;
    src.buffers_processed = (src.buffers_processed >= static_cast<size_t>(popped))
                                ? (src.buffers_processed - popped)
                                : 0;
    src.queue_idx = (src.queue_idx >= static_cast<size_t>(popped))
                        ? (src.queue_idx - popped)
                        : 0;
}

void OpenALBridge::al_get_source_i(uint32_t source, int param, int* value) {
    if (!value) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        *value = 0;
        return;
    }
    const ALSource& src = it->second;

    switch (param) {
        case AL_SOURCE_STATE:
            *value = src.state;
            break;
        case AL_BUFFERS_QUEUED:
            *value = static_cast<int>(src.queue.size());
            break;
        case AL_BUFFERS_PROCESSED:
            *value = static_cast<int>(src.buffers_processed);
            break;
        case AL_SOURCE_TYPE:
            *value = src.is_streaming ? AL_STREAMING : (src.queue.empty() ? AL_UNDETERMINED : AL_STATIC);
            break;
        case AL_BUFFER:
            *value = (src.queue_idx < src.queue.size()) ? static_cast<int>(src.queue[src.queue_idx]) : 0;
            break;
        case AL_SAMPLE_OFFSET:
            *value = static_cast<int>(src.sample_pos);
            break;
        case AL_BYTE_OFFSET: {
            int ch = 1;
            if (src.queue_idx < src.queue.size()) {
                auto bit = m_buffers.find(src.queue[src.queue_idx]);
                if (bit != m_buffers.end() && bit->second && bit->second->channels > 0) ch = bit->second->channels;
            }
            *value = static_cast<int>(src.sample_pos) * ch * static_cast<int>(sizeof(int16_t));
            break;
        }
        default:
            *value = 0;
            break;
    }
}

void OpenALBridge::mix_samples(int16_t* out_interleaved, size_t frame_count, uint32_t out_rate) {
    if (!out_interleaved || frame_count == 0) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    static thread_local std::vector<float> acc_l;
    static thread_local std::vector<float> acc_r;
    acc_l.assign(frame_count, 0.0f);
    acc_r.assign(frame_count, 0.0f);

    // Compute listener coordinate system
    float at_x = m_listener.orientation[0];
    float at_y = m_listener.orientation[1];
    float at_z = m_listener.orientation[2];
    float up_x = m_listener.orientation[3];
    float up_y = m_listener.orientation[4];
    float up_z = m_listener.orientation[5];

    // Right vector = At cross Up
    float rx = at_y * up_z - at_z * up_y;
    float ry = at_z * up_x - at_x * up_z;
    float rz = at_x * up_y - at_y * up_x;
    float r_len = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (r_len > 0.0001f) {
        rx /= r_len;
        ry /= r_len;
        rz /= r_len;
    } else {
        rx = 1.0f; ry = 0.0f; rz = 0.0f;
    }

    for (auto& [id, src] : m_sources) {
        if (src.state != AL_PLAYING || src.queue.empty()) {
            continue;
        }

        // OpenAL 1.1: 3D distance attenuation & spatial panning apply to all mono sources.
        // AL_SOURCE_RELATIVE=AL_TRUE means position is already in listener-relative space (cam at 0,0,0),
        // NOT that 3D attenuation/panning is disabled (cSampleManager initializes all 24 SFX channels rel=1).
        // Stereo buffers (e.g. MP3Stream music/radio/cutscenes) play unattenuated L/R.
        int lead_channels = 1;
        size_t check_idx = std::min(src.queue_idx, src.queue.size() - 1);
        if (auto cbit = m_buffers.find(src.queue[check_idx]); cbit != m_buffers.end() && cbit->second) {
            lead_channels = std::max(1, static_cast<int>(cbit->second->channels));
        }

        float att = 1.0f;
        float pan = 0.0f;
        const bool spatialize = (lead_channels == 1);

        if (spatialize) {
            const float dx = src.source_relative ? src.pos[0] : (src.pos[0] - m_listener.pos[0]);
            const float dy = src.source_relative ? src.pos[1] : (src.pos[1] - m_listener.pos[1]);
            const float dz = src.source_relative ? src.pos[2] : (src.pos[2] - m_listener.pos[2]);
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (m_distance_model != 0 /* AL_NONE */ && dist > src.ref_distance) {
                float max_d = std::max(src.max_distance, src.ref_distance + 0.001f);
                float clamped_d = std::min(dist, max_d);
                if (m_distance_model == 0xD001 /* AL_INVERSE_DISTANCE */) {
                    att = src.ref_distance / (src.ref_distance + src.rolloff_factor * (dist - src.ref_distance));
                } else if (m_distance_model == 0xD003 /* AL_LINEAR_DISTANCE */ || m_distance_model == 0xD004 /* AL_LINEAR_DISTANCE_CLAMPED */) {
                    float d_val = (m_distance_model == 0xD004) ? clamped_d : dist;
                    att = 1.0f - src.rolloff_factor * (d_val - src.ref_distance) / (max_d - src.ref_distance);
                } else {
                    // AL_INVERSE_DISTANCE_CLAMPED (0xD002, default used by GTA LCS)
                    att = src.ref_distance / (src.ref_distance + src.rolloff_factor * (clamped_d - src.ref_distance));
                }
                att = std::clamp(att, 0.0f, 1.0f);
            }

            if (dist > 0.001f) {
                pan = (dx * rx + dy * ry + dz * rz) / dist;
                pan = std::clamp(pan, -1.0f, 1.0f);
            }
        }

        // Constant-power stereo pan for mono 3D sources (never amplifies > 1.0)
        float pan_l = 1.0f;
        float pan_r = 1.0f;
        if (spatialize) {
            constexpr float kHalfPi = 1.57079632679f;
            float t = (pan + 1.0f) * 0.5f; // [0..1]
            float angle = t * kHalfPi;
            pan_l = std::cos(angle);
            pan_r = std::sin(angle);
        }
        // OpenAL 1.1 §4.3.3: AL_MIN_GAIN / AL_MAX_GAIN clamp effective gain after distance attenuation.
        const float pre_clamp_gain = std::max(0.0f, src.gain) * (spatialize ? att : 1.0f);
        const float eff_gain = std::clamp(pre_clamp_gain, src.min_gain, src.max_gain);
        const double target_gl = static_cast<double>(eff_gain) * (spatialize ? static_cast<double>(pan_l) : 1.0);
        const double target_gr = static_cast<double>(eff_gain) * (spatialize ? static_cast<double>(pan_r) : 1.0);
        const double target_pitch = std::max(0.01, static_cast<double>(src.pitch));

        if (src.smoothed_gain_l < 0.0) src.smoothed_gain_l = target_gl;
        if (src.smoothed_gain_r < 0.0) src.smoothed_gain_r = target_gr;
        if (src.smoothed_pitch <= 0.0) src.smoothed_pitch = target_pitch;

        // Cap smoothing ramp to ~10ms (out_rate / 100) so larger device buffers never lag audio transitions.
        const size_t ramp_frames = std::max<size_t>(1, std::min(frame_count, static_cast<size_t>(out_rate / 100)));
        const double inv_ramp = 1.0 / static_cast<double>(ramp_frames);
        const double step_gl = (target_gl - src.smoothed_gain_l) * inv_ramp;
        const double step_gr = (target_gr - src.smoothed_gain_r) * inv_ramp;
        const double step_pitch = (target_pitch - src.smoothed_pitch) * inv_ramp;

        auto sample_step = [&](const std::deque<uint32_t>& queue, size_t& queue_idx,
                               double& sample_pos, size_t* buffers_processed, bool looping,
                               double pitch, float& out_l, float& out_r) -> bool {
            while (queue_idx < queue.size()) {
                auto bit = m_buffers.find(queue[queue_idx]);
                if (bit == m_buffers.end() || !bit->second) return false;
                const ALBuffer& buf = *bit->second;
                const size_t total_frames = buf.samples.size() / buf.channels;
                if (total_frames == 0) {
                    if (buffers_processed) (*buffers_processed)++;
                    queue_idx++;
                    continue;
                }

                const size_t idx0 = static_cast<size_t>(sample_pos);
                const size_t idx1 = idx0 + 1;
                const float frac = static_cast<float>(sample_pos - idx0);

                const ALBuffer* next_buf = nullptr;
                if (idx1 >= total_frames) {
                    if (queue_idx + 1 < queue.size()) {
                        auto nbit = m_buffers.find(queue[queue_idx + 1]);
                        if (nbit != m_buffers.end() && nbit->second) next_buf = nbit->second.get();
                    } else if (looping) {
                        auto fbit = m_buffers.find(queue.front());
                        next_buf = (fbit != m_buffers.end() && fbit->second) ? fbit->second.get() : &buf;
                    }
                }

                auto cubic_hermite = [](float ym1, float y0, float y1, float y2, float t) -> float {
                    float c0 = y0;
                    float c1 = 0.5f * (y1 - ym1);
                    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
                    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
                    return ((c3 * t + c2) * t + c1) * t + c0;
                };

                if (buf.channels == 1) {
                    const float ym1 = (idx0 > 0) ? static_cast<float>(buf.samples[idx0 - 1]) : static_cast<float>(buf.samples[idx0]);
                    const float y0  = static_cast<float>(buf.samples[idx0]);
                    const float y1  = (idx1 < total_frames) ? static_cast<float>(buf.samples[idx1]) :
                                      (next_buf && !next_buf->samples.empty() ? static_cast<float>(next_buf->samples[0]) : y0);
                    const float y2  = (idx0 + 2 < total_frames) ? static_cast<float>(buf.samples[idx0 + 2]) :
                                      (next_buf && next_buf->samples.size() > (idx0 + 2 - total_frames)
                                           ? static_cast<float>(next_buf->samples[idx0 + 2 - total_frames]) : y1);
                    const float s = cubic_hermite(ym1, y0, y1, y2, frac);
                    out_l = s;
                    out_r = s;
                } else {
                    const size_t im1 = (idx0 > 0) ? (idx0 - 1) : idx0;
                    const float ym1_l = static_cast<float>(buf.samples[im1 * 2]);
                    const float ym1_r = static_cast<float>(buf.samples[im1 * 2 + 1]);
                    const float y0_l  = static_cast<float>(buf.samples[idx0 * 2]);
                    const float y0_r  = static_cast<float>(buf.samples[idx0 * 2 + 1]);
                    const float y1_l  = (idx1 < total_frames) ? static_cast<float>(buf.samples[idx1 * 2]) :
                                        (next_buf && next_buf->samples.size() >= 2 ? static_cast<float>(next_buf->samples[0]) : y0_l);
                    const float y1_r  = (idx1 < total_frames) ? static_cast<float>(buf.samples[idx1 * 2 + 1]) :
                                        (next_buf && next_buf->samples.size() >= 2 ? static_cast<float>(next_buf->samples[1]) : y0_r);
                    const size_t i2 = idx0 + 2;
                    const float y2_l  = (i2 < total_frames) ? static_cast<float>(buf.samples[i2 * 2]) :
                                        (next_buf && next_buf->samples.size() >= (i2 - total_frames + 1) * 2
                                             ? static_cast<float>(next_buf->samples[(i2 - total_frames) * 2]) : y1_l);
                    const float y2_r  = (i2 < total_frames) ? static_cast<float>(buf.samples[i2 * 2 + 1]) :
                                        (next_buf && next_buf->samples.size() >= (i2 - total_frames + 1) * 2
                                             ? static_cast<float>(next_buf->samples[(i2 - total_frames) * 2 + 1]) : y1_r);
                    out_l = cubic_hermite(ym1_l, y0_l, y1_l, y2_l, frac);
                    out_r = cubic_hermite(ym1_r, y0_r, y1_r, y2_r, frac);
                }

                const double advance = (pitch * buf.freq) / static_cast<double>(out_rate);
                sample_pos += advance;
                if (sample_pos >= static_cast<double>(total_frames)) {
                    if (looping) {
                        sample_pos = std::fmod(sample_pos, static_cast<double>(total_frames));
                        queue_idx = (queue_idx + 1) % queue.size();
                    } else {
                        sample_pos -= static_cast<double>(total_frames);
                        if (buffers_processed) (*buffers_processed)++;
                        queue_idx++;
                    }
                }
                return true;
            }
            return false;
        };

        for (size_t f = 0; f < frame_count; f++) {
            if (f < ramp_frames) {
                src.smoothed_gain_l += step_gl;
                src.smoothed_gain_r += step_gr;
                src.smoothed_pitch += step_pitch;
            } else {
                src.smoothed_gain_l = target_gl;
                src.smoothed_gain_r = target_gr;
                src.smoothed_pitch = target_pitch;
            }

            float sl = 0.0f, sr = 0.0f;
            if (!sample_step(src.queue, src.queue_idx, src.sample_pos, &src.buffers_processed,
                             src.looping, src.smoothed_pitch, sl, sr)) {
                if (src.looping && !src.queue.empty()) {
                    src.queue_idx = 0;
                    src.sample_pos = 0.0;
                    if (!sample_step(src.queue, src.queue_idx, src.sample_pos, &src.buffers_processed,
                                     src.looping, src.smoothed_pitch, sl, sr)) {
                        src.state = AL_STOPPED;
                        break;
                    }
                } else {
                    src.state = AL_STOPPED;
                    src.underran = src.is_streaming;
                    break;
                }
            }

            constexpr float kHalfPi = 1.5707963267948966f;
            float env = 1.0f;
            if (src.fade_in_remaining > 0 && src.fade_in_total > 0) {
                const size_t elapsed = src.fade_in_total - src.fade_in_remaining;
                const float t = static_cast<float>(elapsed + 1) / static_cast<float>(src.fade_in_total);
                env = std::sin(kHalfPi * t);
                src.fade_in_remaining--;
            }
            // Anti-click ~5ms taper at the natural end of long non-looping mono buffers (>=16384 frames, e.g. engine gear-shifts)
            if (!src.looping && src.queue_idx >= src.queue.size()) {
                auto prev_bit = m_buffers.find(src.queue.back());
                if (prev_bit != m_buffers.end() && prev_bit->second && prev_bit->second->channels == 1 &&
                    prev_bit->second->samples.size() >= 16384) {
                    env = 0.0f;
                }
            } else if (!src.looping && src.queue_idx + 1 == src.queue.size()) {
                auto bit = m_buffers.find(src.queue[src.queue_idx]);
                if (bit != m_buffers.end() && bit->second && bit->second->channels == 1) {
                    const size_t total_frames = bit->second->samples.size();
                    if (total_frames >= 16384 && src.sample_pos < static_cast<double>(total_frames)) {
                        const double advance = (src.smoothed_pitch * bit->second->freq) / static_cast<double>(out_rate);
                        if (advance > 0.0) {
                            const double rem_out_frames = (static_cast<double>(total_frames) - src.sample_pos) / advance;
                            constexpr double kEndTaperFrames = 220.0;
                            if (rem_out_frames < kEndTaperFrames) {
                                const float t = static_cast<float>(std::clamp(rem_out_frames / kEndTaperFrames, 0.0, 1.0));
                                env *= std::sin(kHalfPi * t);
                            }
                        }
                    }
                }
            }
            src.total_frames_played++;

            acc_l[f] += sl * static_cast<float>(src.smoothed_gain_l) * env;
            acc_r[f] += sr * static_cast<float>(src.smoothed_gain_r) * env;

            if (!src.looping && src.queue_idx >= src.queue.size()) {
                src.state = AL_STOPPED;
                src.underran = src.is_streaming;
                break;
            }
        }
    }

    // Mix active fade-out tails so stopped/requeued sources overlap smoothly instead of clicking
    for (auto tail_it = m_fade_tails.begin(); tail_it != m_fade_tails.end();) {
        ALFadeTail& tail = *tail_it;
        for (size_t f = 0; f < frame_count && tail.frames_left > 0; f++) {
            const auto& bptr = (tail.queue_idx < tail.buffers.size()) ? tail.buffers[tail.queue_idx] : nullptr;
            if (!bptr) {
                tail.frames_left = 0;
                break;
            }
            const ALBuffer& buf = *bptr;
            const size_t total_frames = buf.samples.size() / buf.channels;
            if (total_frames == 0) {
                tail.queue_idx++;
                continue;
            }
            const size_t idx0 = std::min(static_cast<size_t>(tail.sample_pos), total_frames - 1);
            const size_t idx1 = idx0 + 1;
            const float frac = static_cast<float>(tail.sample_pos - idx0);
            float sl = 0.0f, sr = 0.0f;
            if (buf.channels == 1) {
                const float s0 = buf.samples[idx0];
                const float s1 = (idx1 < total_frames) ? buf.samples[idx1] : s0;
                sl = sr = s0 + frac * (s1 - s0);
            } else {
                const float s0_l = buf.samples[idx0 * 2];
                const float s0_r = buf.samples[idx0 * 2 + 1];
                const float s1_l = (idx1 < total_frames) ? buf.samples[idx1 * 2] : s0_l;
                const float s1_r = (idx1 < total_frames) ? buf.samples[idx1 * 2 + 1] : s0_r;
                sl = s0_l + frac * (s1_l - s0_l);
                sr = s0_r + frac * (s1_r - s0_r);
            }
            constexpr float kHalfPi = 1.5707963267948966f;
            const float frac_left = static_cast<float>(tail.frames_left) / static_cast<float>(tail.frames_total);
            const float env = std::sin(kHalfPi * frac_left);
            acc_l[f] += sl * static_cast<float>(tail.gain_l) * env;
            acc_r[f] += sr * static_cast<float>(tail.gain_r) * env;
            tail.frames_left--;

            const double advance = (tail.pitch * buf.freq) / static_cast<double>(out_rate);
            tail.sample_pos += advance;
            if (tail.sample_pos >= static_cast<double>(total_frames)) {
                if (tail.looping) {
                    tail.sample_pos = std::fmod(tail.sample_pos, static_cast<double>(total_frames));
                    tail.queue_idx = (tail.queue_idx + 1) % tail.buffers.size();
                } else {
                    tail.sample_pos -= static_cast<double>(total_frames);
                    tail.queue_idx++;
                    if (tail.queue_idx >= tail.buffers.size()) {
                        if (tail.repeat_last_buf && tail.frames_left > 0) {
                            tail.queue_idx = tail.buffers.size() - 1;
                            tail.sample_pos = 0.0;
                        } else {
                            tail.frames_left = 0;
                            break;
                        }
                    }
                }
            }
        }
        if (tail.frames_left == 0) {
            tail_it = m_fade_tails.erase(tail_it);
        } else {
            ++tail_it;
        }
    }

    // Master listener gain and soft-knee limiter (prevents harsh digital clipping/distortion)
    auto soft_limit = [](float x) -> int16_t {
        constexpr float kMax = 32767.0f;
        float norm = x / kMax;
        if (norm > 0.7f) {
            norm = 0.7f + 0.3f * std::tanh((norm - 0.7f) / 0.3f);
        } else if (norm < -0.7f) {
            norm = -0.7f + 0.3f * std::tanh((norm + 0.7f) / 0.3f);
        }
        float out = norm * kMax;
        return static_cast<int16_t>(std::clamp(out, -32768.0f, 32767.0f));
    };

    float master_gain = m_listener.gain;
    float max_pre_limit = 0.0f;
    for (size_t i = 0; i < frame_count; i++) {
        float fl = acc_l[i] * master_gain;
        float fr = acc_r[i] * master_gain;
        max_pre_limit = std::max(max_pre_limit, std::max(std::abs(fl), std::abs(fr)));
        out_interleaved[i * 2]     = soft_limit(fl);
        out_interleaved[i * 2 + 1] = soft_limit(fr);
    }

    static size_t s_mix_diag = 0;
    if (max_pre_limit > 1.0f && s_mix_diag < 8) {
        s_mix_diag++;
        size_t playing_cnt = 0;
        for (const auto& [sid, src] : m_sources) {
            if (src.state == AL_PLAYING) playing_cnt++;
        }
        std::cout << "[Audio] mix diag #" << s_mix_diag << ": active_sources=" << playing_cnt
                  << ", master_gain=" << master_gain << ", peak_pre_limit=" << max_pre_limit << "\n";
    }
}

bool is_openal_symbol(std::string_view name) {
    static const std::unordered_set<std::string_view> s_openal_syms = {
        "alcOpenDevice",
        "alcCreateContext",
        "alcMakeContextCurrent",
        "alcProcessContext",
        "alcSuspendContext",
        "alGenBuffers",
        "alDeleteBuffers",
        "alBufferData",
        "alGenSources",
        "alSourcei",
        "alSourcef",
        "alSource3f",
        "alSourcePlay",
        "alSourcePause",
        "alSourceStop",
        "alSourceQueueBuffers",
        "alSourceUnqueueBuffers",
        "alGetSourcei",
        "alListenerf",
        "alListener3f",
        "alListenerfv",
        "alDistanceModel",
        "alGetError"
    };
    return s_openal_syms.find(name) != s_openal_syms.end();
}

void register_openal_hle(lcs::cpu::HLEManager& hle) {
    hle.register_function("alcOpenDevice", [](const uint64_t* args) -> uint64_t {
        const char* name = reinterpret_cast<const char*>(args[0]);
        return OpenALBridge::instance().alc_open_device(name);
    });

    hle.register_function("alcCreateContext", [](const uint64_t* args) -> uint64_t {
        uint64_t dev = args[0];
        const int32_t* attrlist = reinterpret_cast<const int32_t*>(args[1]);
        return OpenALBridge::instance().alc_create_context(dev, attrlist);
    });

    hle.register_function("alcMakeContextCurrent", [](const uint64_t* args) -> uint64_t {
        return OpenALBridge::instance().alc_make_context_current(args[0]);
    });

    hle.register_function("alcProcessContext", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().alc_process_context(args[0]);
        return 0;
    });

    hle.register_function("alcSuspendContext", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().alc_suspend_context(args[0]);
        return 0;
    });

    hle.register_function("alGetError", [](const uint64_t*) -> uint64_t {
        return OpenALBridge::instance().al_get_error();
    });

    hle.register_function("alDistanceModel", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().al_distance_model(static_cast<int>(args[0]));
        return 0;
    });

    hle.register_function("alListenerf", [](const uint64_t* args) -> uint64_t {
        auto* cpu = lcs::cpu::HLEManager::instance().get_active_cpu();
        float val = cpu ? cpu->get_fp_float(0) : 0.0f;
        OpenALBridge::instance().al_listener_f(static_cast<int>(args[0]), val);
        return 0;
    });

    hle.register_function("alListener3f", [](const uint64_t* args) -> uint64_t {
        auto* cpu = lcs::cpu::HLEManager::instance().get_active_cpu();
        float v1 = cpu ? cpu->get_fp_float(0) : 0.0f;
        float v2 = cpu ? cpu->get_fp_float(1) : 0.0f;
        float v3 = cpu ? cpu->get_fp_float(2) : 0.0f;
        OpenALBridge::instance().al_listener_3f(static_cast<int>(args[0]), v1, v2, v3);
        return 0;
    });

    hle.register_function("alListenerfv", [](const uint64_t* args) -> uint64_t {
        const float* vals = reinterpret_cast<const float*>(args[1]);
        OpenALBridge::instance().al_listener_fv(static_cast<int>(args[0]), vals);
        return 0;
    });

    hle.register_function("alGenBuffers", [](const uint64_t* args) -> uint64_t {
        int n = static_cast<int>(args[0]);
        uint32_t* bufs = reinterpret_cast<uint32_t*>(args[1]);
        OpenALBridge::instance().al_gen_buffers(n, bufs);
        return 0;
    });

    hle.register_function("alDeleteBuffers", [](const uint64_t* args) -> uint64_t {
        int n = static_cast<int>(args[0]);
        const uint32_t* bufs = reinterpret_cast<const uint32_t*>(args[1]);
        OpenALBridge::instance().al_delete_buffers(n, bufs);
        return 0;
    });

    hle.register_function("alBufferData", [](const uint64_t* args) -> uint64_t {
        uint32_t buf = static_cast<uint32_t>(args[0]);
        int fmt = static_cast<int>(args[1]);
        const void* data = reinterpret_cast<const void*>(args[2]);
        int sz = static_cast<int>(args[3]);
        int freq = static_cast<int>(args[4]);
        OpenALBridge::instance().al_buffer_data(buf, fmt, data, sz, freq);
        return 0;
    });

    hle.register_function("alGenSources", [](const uint64_t* args) -> uint64_t {
        int n = static_cast<int>(args[0]);
        uint32_t* srcs = reinterpret_cast<uint32_t*>(args[1]);
        OpenALBridge::instance().al_gen_sources(n, srcs);
        return 0;
    });

    hle.register_function("alSourcei", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int param = static_cast<int>(args[1]);
        int val = static_cast<int>(args[2]);
        OpenALBridge::instance().al_source_i(src, param, val);
        return 0;
    });

    hle.register_function("alSourcef", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int param = static_cast<int>(args[1]);
        auto* cpu = lcs::cpu::HLEManager::instance().get_active_cpu();
        float val = cpu ? cpu->get_fp_float(0) : 0.0f;
        OpenALBridge::instance().al_source_f(src, param, val);
        return 0;
    });

    hle.register_function("alSource3f", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int param = static_cast<int>(args[1]);
        auto* cpu = lcs::cpu::HLEManager::instance().get_active_cpu();
        float v1 = cpu ? cpu->get_fp_float(0) : 0.0f;
        float v2 = cpu ? cpu->get_fp_float(1) : 0.0f;
        float v3 = cpu ? cpu->get_fp_float(2) : 0.0f;
        OpenALBridge::instance().al_source_3f(src, param, v1, v2, v3);
        return 0;
    });

    hle.register_function("alSourcePlay", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().al_source_play(static_cast<uint32_t>(args[0]));
        return 0;
    });

    hle.register_function("alSourcePause", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().al_source_pause(static_cast<uint32_t>(args[0]));
        return 0;
    });

    hle.register_function("alSourceStop", [](const uint64_t* args) -> uint64_t {
        OpenALBridge::instance().al_source_stop(static_cast<uint32_t>(args[0]));
        return 0;
    });

    hle.register_function("alSourceQueueBuffers", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int nb = static_cast<int>(args[1]);
        const uint32_t* bufs = reinterpret_cast<const uint32_t*>(args[2]);
        OpenALBridge::instance().al_source_queue_buffers(src, nb, bufs);
        return 0;
    });

    hle.register_function("alSourceUnqueueBuffers", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int nb = static_cast<int>(args[1]);
        uint32_t* bufs = reinterpret_cast<uint32_t*>(args[2]);
        OpenALBridge::instance().al_source_unqueue_buffers(src, nb, bufs);
        return 0;
    });

    hle.register_function("alGetSourcei", [](const uint64_t* args) -> uint64_t {
        uint32_t src = static_cast<uint32_t>(args[0]);
        int param = static_cast<int>(args[1]);
        int* val = reinterpret_cast<int*>(args[2]);
        OpenALBridge::instance().al_get_source_i(src, param, val);
        return 0;
    });
}

void register_openal_stubs() {
    register_openal_hle(lcs::cpu::HLEManager::instance());
}

} // namespace lcs::audio
