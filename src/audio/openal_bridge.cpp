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
        m_buffers[id] = ALBuffer{id, AL_FORMAT_MONO16, 44100, 1, {}};
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

    auto it = m_buffers.find(buffer);
    if (it == m_buffers.end()) {
        m_buffers[buffer] = ALBuffer{buffer, static_cast<uint32_t>(format), static_cast<uint32_t>(freq), 1, {}};
        it = m_buffers.find(buffer);
    }

    ALBuffer& buf = it->second;
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
    }

    static size_t s_buf_data_count = 0;
    if (s_buf_data_count++ < 10) {
        std::cout << "[Audio] alBufferData(buffer=" << buffer << ", fmt=0x" << std::hex << format
                  << std::dec << ", sz=" << size << ", freq=" << freq << ", ch=" << buf.channels << ")\n";
    }
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
            src.queue.clear();
            if (value != 0) {
                src.queue.push_back(static_cast<uint32_t>(value));
            }
            src.buffers_processed = 0;
            src.queue_idx = 0;
            src.sample_pos = 0.0;
            src.is_streaming = false;
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

void OpenALBridge::al_source_play(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    it->second.state = AL_PLAYING;
    static size_t s_play_count = 0;
    if (s_play_count++ < 10) {
        std::cout << "[Audio] alSourcePlay(source=" << source << ", queue=" << it->second.queue.size() << ")\n";
    }
}

void OpenALBridge::al_source_pause(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    it->second.state = AL_PAUSED;
}

void OpenALBridge::al_source_stop(uint32_t source) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    it->second.state = AL_STOPPED;
    it->second.sample_pos = 0.0;
    it->second.queue_idx = 0;
}

void OpenALBridge::al_source_queue_buffers(uint32_t source, int nb, const uint32_t* buffers) {
    if (nb <= 0 || !buffers) return;
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_sources.find(source);
    if (it == m_sources.end()) return;
    ALSource& src = it->second;

    for (int i = 0; i < nb; i++) {
        src.queue.push_back(buffers[i]);
    }
    src.is_streaming = true;
    if (src.state == AL_STOPPED && src.queue_idx < src.queue.size()) {
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
        case AL_BYTE_OFFSET:
            *value = static_cast<int>(src.sample_pos * sizeof(int16_t));
            break;
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

        float att = 1.0f;
        float pan = 0.0f;

        if (!src.source_relative) {
            float dx = src.pos[0] - m_listener.pos[0];
            float dy = src.pos[1] - m_listener.pos[1];
            float dz = src.pos[2] - m_listener.pos[2];
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (dist > src.ref_distance) {
                float max_d = std::max(src.max_distance, src.ref_distance + 0.001f);
                float clamped_d = std::min(dist, max_d);
                att = src.ref_distance / (src.ref_distance + src.rolloff_factor * (clamped_d - src.ref_distance));
                att = std::clamp(att, 0.0f, 1.0f);
            }

            if (dist > 0.001f) {
                pan = (dx * rx + dy * ry + dz * rz) / dist;
                pan = std::clamp(pan, -1.0f, 1.0f);
            }
        }

        float pan_l = src.source_relative ? 1.0f : ((1.0f - pan) * 0.5f * 2.0f);
        float pan_r = src.source_relative ? 1.0f : ((1.0f + pan) * 0.5f * 2.0f);
        float eff_gain = std::clamp(src.gain, src.min_gain, src.max_gain);

        for (size_t f = 0; f < frame_count; f++) {
            if (src.queue_idx >= src.queue.size()) {
                if (src.looping && !src.queue.empty()) {
                    src.queue_idx = 0;
                    src.sample_pos = 0.0;
                } else if (src.is_streaming) {
                    // Buffer underrun during audio stream decoding: remain in playing state
                    // and wait smoothly for the decoder thread to queue more frames.
                    break;
                } else {
                    src.state = AL_STOPPED;
                    break;
                }
            }

            uint32_t buf_id = src.queue[src.queue_idx];
            auto bit = m_buffers.find(buf_id);
            if (bit == m_buffers.end()) {
                src.state = AL_STOPPED;
                break;
            }

            const ALBuffer& buf = bit->second;
            size_t total_frames = buf.samples.size() / buf.channels;
            if (total_frames == 0) {
                src.queue_idx++;
                continue;
            }

            size_t idx0 = static_cast<size_t>(src.sample_pos);
            size_t idx1 = idx0 + 1;
            float frac = static_cast<float>(src.sample_pos - idx0);

            // Interpolate across buffer boundaries if next buffer is queued (eliminates clicks/pops)
            const ALBuffer* next_buf = nullptr;
            if (idx1 >= total_frames && src.queue_idx + 1 < src.queue.size()) {
                auto nbit = m_buffers.find(src.queue[src.queue_idx + 1]);
                if (nbit != m_buffers.end()) next_buf = &nbit->second;
            }

            if (buf.channels == 1) {
                float s0 = buf.samples[idx0];
                float s1 = (idx1 < total_frames) ? buf.samples[idx1] :
                           (next_buf && !next_buf->samples.empty() ? next_buf->samples[0] : s0);
                float s = s0 + frac * (s1 - s0);
                acc_l[f] += s * eff_gain * att * pan_l;
                acc_r[f] += s * eff_gain * att * pan_r;
            } else {
                float s0_l = buf.samples[idx0 * 2];
                float s0_r = buf.samples[idx0 * 2 + 1];
                float s1_l = (idx1 < total_frames) ? buf.samples[idx1 * 2] :
                             (next_buf && next_buf->samples.size() >= 2 ? next_buf->samples[0] : s0_l);
                float s1_r = (idx1 < total_frames) ? buf.samples[idx1 * 2 + 1] :
                             (next_buf && next_buf->samples.size() >= 2 ? next_buf->samples[1] : s0_r);
                float sl = s0_l + frac * (s1_l - s0_l);
                float sr = s0_r + frac * (s1_r - s0_r);
                acc_l[f] += sl * eff_gain * att;
                acc_r[f] += sr * eff_gain * att;
            }

            double step = (static_cast<double>(src.pitch) * buf.freq) / static_cast<double>(out_rate);
            src.sample_pos += step;
            if (src.sample_pos >= static_cast<double>(total_frames)) {
                src.sample_pos -= static_cast<double>(total_frames);
                src.buffers_processed++;
                src.queue_idx++;
            }
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
    for (size_t i = 0; i < frame_count; i++) {
        float fl = acc_l[i] * master_gain;
        float fr = acc_r[i] * master_gain;
        out_interleaved[i * 2]     = soft_limit(fl);
        out_interleaved[i * 2 + 1] = soft_limit(fr);
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
