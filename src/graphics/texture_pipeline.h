// Asynchronous texture decode pipeline.
//
// The game uploads every texture through glCompressedTexImage2D on its render thread, and
// decoding PVRTC/ETC1/DXT to RGBA8 there (~14 ms per megapixel, one thread) plus creating a
// D3D12 resource per texture stalled driving for 30..175 ms whenever a new area streamed in.
//
// Here the render thread only copies the compressed payload and queues a job. A pool of
// worker threads hashes the payload, reuses an already decoded copy when the same texture
// comes back (decoded-texture LRU cache), decodes it otherwise, and asks the backend for a
// GPU resource (recycled or newly created; ID3D12Device is free-threaded). The backend
// attaches finished jobs at its next upload flush; a draw that needs a texture whose job is
// still queued runs that job inline, so nothing is ever drawn without its contents.
//
// Environment switches (A/B testing):
//   LCS_TEX_WORKERS=N     worker threads (0 = old synchronous path; default: a quarter of
//                         the hardware threads, 2..4 — more workers measurably slowed the
//                         game's own threads on an 8-core/16-thread CPU)
//   LCS_TEX_CACHE_MB=N    decoded-texture cache budget (default 256, 0 = off)
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace lcs::graphics {

using TextureDecodeFn = bool (*)(uint32_t internal_format, uint32_t width, uint32_t height,
                                 int32_t image_size, const void* data, std::vector<uint8_t>& out);

// Backend hooks for GPU resources, callable from any thread. `acquire` returns an owned
// resource handle (or nullptr) and whether it is currently in the shader-resource state;
// `release` takes back a handle that was never attached to a texture.
using TextureResourceAcquireFn = void* (*)(uint32_t width, uint32_t height, bool& in_srv_state);
using TextureResourceReleaseFn = void (*)(void* resource, bool in_srv_state);

struct TextureJob {
    uint32_t internal_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> src;  // copy of the compressed payload

    // Results, valid once done() is true
    bool ok = false;
    std::shared_ptr<const std::vector<uint8_t>> rgba;
    void* gpu_resource = nullptr;  // owned until taken by the backend
    bool gpu_in_srv_state = false;

    bool done() const { return state.load(std::memory_order_acquire) == kDone; }

    ~TextureJob();

private:
    friend class TexturePipeline;
    static constexpr int kQueued = 0;
    static constexpr int kRunning = 1;
    static constexpr int kDone = 2;
    std::atomic<int> state{kQueued};
    bool ran_inline = false;
    std::mutex mtx;
    std::condition_variable cv;
};

class TexturePipeline {
public:
    static TexturePipeline& instance();

    void start(TextureDecodeFn decode);
    void stop();
    bool enabled() const { return m_enabled; }

    void set_resource_hooks(TextureResourceAcquireFn acquire, TextureResourceReleaseFn release);
    TextureResourceReleaseFn release_hook() const { return m_release.load(std::memory_order_acquire); }

    // Copies `image_size` bytes from `data` and queues the decode. Returns nullptr when the
    // pipeline is disabled; the caller then decodes synchronously.
    std::shared_ptr<TextureJob> submit(uint32_t internal_format, uint32_t width, uint32_t height,
                                       int32_t image_size, const void* data);

    // Makes sure the job is done: runs it on the calling thread if no worker picked it up yet,
    // otherwise waits for the worker.
    void complete(TextureJob& job);

    struct Stats {
        uint64_t jobs = 0;
        uint64_t cache_hits = 0;
        uint64_t inline_runs = 0;
        uint64_t waits = 0;
        uint64_t decode_ns = 0;
        uint64_t pixels = 0;
    };
    Stats stats() const;
    int worker_count() const { return m_worker_count; }

private:
    TexturePipeline() = default;
    ~TexturePipeline();
    TexturePipeline(const TexturePipeline&) = delete;
    TexturePipeline& operator=(const TexturePipeline&) = delete;

    struct Impl;
    void run(TextureJob& job);
    void worker_main();

    bool m_enabled = false;
    int m_worker_count = 0;
    TextureDecodeFn m_decode = nullptr;
    std::atomic<TextureResourceAcquireFn> m_acquire{nullptr};
    std::atomic<TextureResourceReleaseFn> m_release{nullptr};
    Impl* m_impl = nullptr;
};

} // namespace lcs::graphics
