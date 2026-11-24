#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace lcs::graphics {

class VideoPlayer {
public:
    VideoPlayer();
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;
    VideoPlayer(VideoPlayer&&) = default;
    VideoPlayer& operator=(VideoPlayer&&) = default;

    bool open(const std::string& path, bool loop = false);
    void close();

    bool read_frame(std::vector<uint8_t>& out_rgba, int64_t& out_timestamp_100ns);
    bool read_audio_samples(std::vector<int16_t>& out_stereo_44100, int64_t& out_timestamp_100ns);

    void play();
    void pause();
    void seek(int64_t time_100ns);
    bool is_playing() const;
    bool has_audio() const;

    uint32_t get_width() const;
    uint32_t get_height() const;
    double get_frame_duration() const;

private:
    struct VideoPlayerImpl;
    std::unique_ptr<VideoPlayerImpl> pimpl;
};

} // namespace lcs::graphics