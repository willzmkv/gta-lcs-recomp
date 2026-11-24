#include "video_player.h"

#include <iostream>
#include <filesystem>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace lcs::graphics {

struct VideoPlayer::VideoPlayerImpl {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> video_type;
    ComPtr<IMFMediaType> audio_type;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t stride = 0;
    double frame_duration = 0.0;
    int64_t current_time = 0;
    uint32_t audio_channels = 0;
    uint32_t audio_sample_rate = 0;
    bool has_audio_stream = false;
    bool audio_eos = false;
    bool com_initialized = false;
    bool mf_started = false;
    bool playing = false;
    bool loop = false;
    std::string filepath;

    ~VideoPlayerImpl() {
        close();
    }

    void refresh_media_metrics() {
        if (!reader) return;
        ComPtr<IMFMediaType> actual_type;
        if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actual_type)) || !actual_type) {
            return;
        }
        UINT32 w = 0, h = 0;
        if (SUCCEEDED(MFGetAttributeSize(actual_type.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
            width = w;
            height = h;
        }
        UINT32 fps_num = 0, fps_den = 1;
        if (SUCCEEDED(MFGetAttributeRatio(actual_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) &&
            fps_num > 0 && fps_den > 0) {
            frame_duration = static_cast<double>(fps_den) / static_cast<double>(fps_num);
        } else if (frame_duration <= 0.0) {
            frame_duration = 1.0 / 30.0;
        }
        UINT32 raw_stride = 0;
        if (SUCCEEDED(actual_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride)) && raw_stride != 0) {
            stride = static_cast<int32_t>(raw_stride);
        } else {
            stride = static_cast<int32_t>(width * 4);
        }
    }

    bool open(const std::string& path, bool loop_) {
        close();
        loop = loop_;
        filepath = path;

        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(com_hr)) {
            com_initialized = true;
        } else if (com_hr != RPC_E_CHANGED_MODE) {
            std::cerr << "[VideoPlayer] CoInitializeEx failed: 0x" << std::hex << com_hr << std::dec << "\n";
        }

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            std::cerr << "[VideoPlayer] MFStartup failed: 0x" << std::hex << hr << std::dec << "\n";
            close();
            return false;
        }
        mf_started = true;

        int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, nullptr, 0);
        std::wstring wpath(wlen > 0 ? static_cast<size_t>(wlen) : 1, L'\0');
        if (wlen > 0) {
            MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, wpath.data(), wlen);
        } else {
            wpath.assign(filepath.begin(), filepath.end());
        }

        ComPtr<IMFAttributes> attrs;
        hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr) && attrs) {
            attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
            attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }

        hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs.Get(), &reader);
        if (FAILED(hr) || !reader) {
            std::cerr << "[VideoPlayer] MFCreateSourceReaderFromURL failed for " << filepath
                      << ": 0x" << std::hex << hr << std::dec << "\n";
            close();
            return false;
        }

        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        if (FAILED(hr)) {
            std::cerr << "[VideoPlayer] SetStreamSelection failed: 0x" << std::hex << hr << std::dec << "\n";
            close();
            return false;
        }

        hr = MFCreateMediaType(&video_type);
        if (FAILED(hr) || !video_type) {
            std::cerr << "[VideoPlayer] MFCreateMediaType failed: 0x" << std::hex << hr << std::dec << "\n";
            close();
            return false;
        }

        hr = video_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (FAILED(hr)) { close(); return false; }

        hr = video_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(hr)) { close(); return false; }

        hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, video_type.Get());
        if (FAILED(hr)) {
            std::cerr << "[VideoPlayer] SetCurrentMediaType failed: 0x" << std::hex << hr << std::dec << "\n";
            close();
            return false;
        }

        refresh_media_metrics();

        // Also enable & decode the first audio stream as S16 PCM 44100 Hz stereo (optional; silent videos still work).
        has_audio_stream = false;
        audio_eos = false;
        if (SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
            ComPtr<IMFMediaType> req_audio;
            if (SUCCEEDED(MFCreateMediaType(&req_audio)) && req_audio) {
                req_audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                req_audio->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                req_audio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                req_audio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
                req_audio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                req_audio->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                req_audio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 4);
                if (SUCCEEDED(reader->SetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, req_audio.Get()))) {
                    audio_type = req_audio;
                    has_audio_stream = true;
                    audio_channels = 2;
                    audio_sample_rate = 44100;
                    ComPtr<IMFMediaType> cur_audio;
                    if (SUCCEEDED(reader->GetCurrentMediaType(
                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur_audio)) && cur_audio) {
                        UINT32 ch = 0, sr = 0;
                        if (SUCCEEDED(cur_audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch)) && ch > 0) audio_channels = ch;
                        if (SUCCEEDED(cur_audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr)) && sr > 0) audio_sample_rate = sr;
                    }
                }
            }
        }

        std::cout << "[VideoPlayer] Opened: " << filepath << " (" << width << "x" << height
                  << ", frame_dur=" << frame_duration << "s, stride=" << stride
                  << ", audio=" << (has_audio_stream ? (std::to_string(audio_sample_rate) + "Hz/" + std::to_string(audio_channels) + "ch") : "none")
                  << ")\n";
        return (width > 0 && height > 0);
    }

    void close() {
        reader.Reset();
        video_type.Reset();
        audio_type.Reset();
        width = 0;
        height = 0;
        stride = 0;
        frame_duration = 0.0;
        current_time = 0;
        audio_channels = 0;
        audio_sample_rate = 0;
        has_audio_stream = false;
        audio_eos = false;
        playing = false;
        if (mf_started) {
            MFShutdown();
            mf_started = false;
        }
        if (com_initialized) {
            CoUninitialize();
            com_initialized = false;
        }
    }

    bool read_frame(std::vector<uint8_t>& out_rgba, int64_t& out_timestamp) {
        if (!reader) return false;

        ComPtr<IMFSample> sample;
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;

        for (int tries = 0; tries < 16; tries++) {
            sample.Reset();
            flags = 0;
            HRESULT hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample
            );

            if (FAILED(hr)) {
                std::cerr << "[VideoPlayer] ReadSample failed: 0x" << std::hex << hr << std::dec << "\n";
                return false;
            }

            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                refresh_media_metrics();
            }

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (loop) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    var.vt = VT_I8;
                    var.hVal.QuadPart = 0;
                    reader->SetCurrentPosition(GUID_NULL, var);
                    PropVariantClear(&var);
                    current_time = 0;
                    continue;
                }
                playing = false;
                return false;
            }

            if (sample) break;
        }

        if (!sample || width == 0 || height == 0) return false;

        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr) || !buffer) return false;

        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        hr = buffer->Lock(&data, &max_len, &cur_len);
        if (FAILED(hr) || !data) return false;

        // Convert Media Foundation BGRX (with possibly padded/negative stride) to top-down RGBA8.
        out_rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        int32_t abs_stride = (stride != 0) ? (stride < 0 ? -stride : stride) : static_cast<int32_t>(width * 4);
        for (uint32_t y = 0; y < height; y++) {
            uint32_t src_y = (stride < 0) ? (height - 1 - y) : y;
            size_t row_off = static_cast<size_t>(src_y) * static_cast<size_t>(abs_stride);
            if (row_off + static_cast<size_t>(width) * 4 > cur_len) break;
            const uint8_t* src_row = data + row_off;
            uint8_t* dst_row = out_rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = src_row[x * 4 + 2]; // R
                dst_row[x * 4 + 1] = src_row[x * 4 + 1]; // G
                dst_row[x * 4 + 2] = src_row[x * 4 + 0]; // B
                dst_row[x * 4 + 3] = 255;                // A
            }
        }

        buffer->Unlock();
        current_time = timestamp;
        out_timestamp = timestamp;
        return true;
    }

    bool read_audio_samples(std::vector<int16_t>& out_stereo_44100, int64_t& out_timestamp) {
        out_stereo_44100.clear();
        if (!reader || !has_audio_stream || audio_eos) return false;

        ComPtr<IMFSample> sample;
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;

        for (int tries = 0; tries < 8; tries++) {
            sample.Reset();
            flags = 0;
            HRESULT hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample
            );
            if (FAILED(hr)) return false;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                ComPtr<IMFMediaType> cur_audio;
                if (SUCCEEDED(reader->GetCurrentMediaType(
                        static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur_audio)) && cur_audio) {
                    UINT32 ch = 0, sr = 0;
                    if (SUCCEEDED(cur_audio->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch)) && ch > 0) audio_channels = ch;
                    if (SUCCEEDED(cur_audio->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr)) && sr > 0) audio_sample_rate = sr;
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                audio_eos = true;
                return false;
            }
            if (sample) break;
        }
        if (!sample) return false;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;
        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (FAILED(buffer->Lock(&data, &max_len, &cur_len)) || !data || cur_len == 0) return false;

        uint32_t ch = std::max(1u, audio_channels);
        size_t in_frames = cur_len / (ch * sizeof(int16_t));
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);

        if (audio_sample_rate == 44100 && ch == 2) {
            out_stereo_44100.assign(pcm, pcm + in_frames * 2);
        } else if (in_frames > 0 && audio_sample_rate > 0) {
            size_t out_frames = static_cast<size_t>((static_cast<uint64_t>(in_frames) * 44100ULL) / audio_sample_rate);
            out_stereo_44100.resize(out_frames * 2);
            for (size_t i = 0; i < out_frames; i++) {
                size_t src_i = std::min(in_frames - 1, (i * audio_sample_rate) / 44100ULL);
                int16_t l = pcm[src_i * ch];
                int16_t r = (ch >= 2) ? pcm[src_i * ch + 1] : l;
                out_stereo_44100[i * 2 + 0] = l;
                out_stereo_44100[i * 2 + 1] = r;
            }
        }

        buffer->Unlock();
        out_timestamp = timestamp;
        return !out_stereo_44100.empty();
    }

    void play() { playing = true; }
    void pause() { playing = false; }
    void seek(int64_t time_100ns) {
        if (!reader) return;
        PROPVARIANT var;
        PropVariantInit(&var);
        var.vt = VT_I8;
        var.hVal.QuadPart = time_100ns;
        reader->SetCurrentPosition(GUID_NULL, var);
        PropVariantClear(&var);
        current_time = time_100ns;
        audio_eos = false;
    }
    bool is_playing() const { return playing; }
    bool has_audio() const { return has_audio_stream; }
    uint32_t get_width() const { return width; }
    uint32_t get_height() const { return height; }
    double get_frame_duration() const { return frame_duration; }
};

VideoPlayer::VideoPlayer() : pimpl(std::make_unique<VideoPlayerImpl>()) {}
VideoPlayer::~VideoPlayer() = default;

bool VideoPlayer::open(const std::string& path, bool loop) {
    return pimpl->open(path, loop);
}

void VideoPlayer::close() {
    pimpl->close();
}

bool VideoPlayer::read_frame(std::vector<uint8_t>& out_rgba, int64_t& out_timestamp) {
    return pimpl->read_frame(out_rgba, out_timestamp);
}

bool VideoPlayer::read_audio_samples(std::vector<int16_t>& out_stereo_44100, int64_t& out_timestamp_100ns) {
    return pimpl->read_audio_samples(out_stereo_44100, out_timestamp_100ns);
}

void VideoPlayer::play() { pimpl->play(); }
void VideoPlayer::pause() { pimpl->pause(); }
void VideoPlayer::seek(int64_t time_100ns) { pimpl->seek(time_100ns); }
bool VideoPlayer::is_playing() const { return pimpl->is_playing(); }
bool VideoPlayer::has_audio() const { return pimpl->has_audio(); }
uint32_t VideoPlayer::get_width() const { return pimpl->get_width(); }
uint32_t VideoPlayer::get_height() const { return pimpl->get_height(); }
double VideoPlayer::get_frame_duration() const { return pimpl->get_frame_duration(); }

} // namespace lcs::graphics

#else

namespace lcs::graphics {
VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() = default;
bool VideoPlayer::open(const std::string&, bool) { return false; }
void VideoPlayer::close() {}
bool VideoPlayer::read_frame(std::vector<uint8_t>&, int64_t&) { return false; }
bool VideoPlayer::read_audio_samples(std::vector<int16_t>&, int64_t&) { return false; }
void VideoPlayer::play() {}
void VideoPlayer::pause() {}
void VideoPlayer::seek(int64_t) {}
bool VideoPlayer::is_playing() const { return false; }
bool VideoPlayer::has_audio() const { return false; }
uint32_t VideoPlayer::get_width() const { return 0; }
uint32_t VideoPlayer::get_height() const { return 0; }
double VideoPlayer::get_frame_duration() const { return 0.0; }
} // namespace lcs::graphics

#endif