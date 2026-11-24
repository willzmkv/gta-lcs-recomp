#include "graphics/texture_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <list>
#include <thread>
#include <unordered_map>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::graphics {

namespace {

inline uint64_t rotl64(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }

inline uint64_t load_u64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// 64-bit content hash with four independent lanes, so consecutive words do not form one
// long dependency chain and the core can keep several multiplies in flight.
uint64_t hash_bytes(const uint8_t* p, size_t n) {
    constexpr uint64_t k1 = 0x9E3779B97F4A7C15ULL;
    constexpr uint64_t k2 = 0xC2B2AE3D27D4EB4FULL;
    constexpr uint64_t k3 = 0x165667B19E3779F9ULL;
    constexpr uint64_t k4 = 0xD6E8FEB86659FD93ULL;
    uint64_t a = k1 ^ n, b = k2, c = k3, d = k4;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        a = rotl64(a ^ (load_u64(p + i +  0) * k2), 31) * k1;
        b = rotl64(b ^ (load_u64(p + i +  8) * k2), 31) * k1;
        c = rotl64(c ^ (load_u64(p + i + 16) * k2), 31) * k1;
        d = rotl64(d ^ (load_u64(p + i + 24) * k2), 31) * k1;
    }
    for (; i + 8 <= n; i += 8) {
        a = rotl64(a ^ (load_u64(p + i) * k2), 27) * k1 + k4;
    }
    for (; i < n; ++i) {
        a = rotl64(a ^ (uint64_t(p[i]) * k3), 11) * k1;
    }
    uint64_t h = a ^ rotl64(b, 7) ^ rotl64(c, 13) ^ rotl64(d, 29);
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ULL;
    h ^= h >> 33;
    return h;
}

int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name); v && *v) return std::atoi(v);
    return fallback;
}

} // namespace

struct TexturePipeline::Impl {
    std::mutex queue_mtx;
    std::condition_variable queue_cv;
    std::deque<std::shared_ptr<TextureJob>> queue;
    std::vector<std::thread> threads;
    bool stopping = false;

    // Decoded-texture LRU cache keyed by (payload hash, format, size)
    struct CacheEntry {
        uint64_t key;
        std::shared_ptr<const std::vector<uint8_t>> rgba;
    };
    std::mutex cache_mtx;
    std::list<CacheEntry> lru;  // front = most recent
    std::unordered_map<uint64_t, std::list<CacheEntry>::iterator> cache;
    size_t cache_bytes = 0;
    size_t cache_budget = 0;

    std::atomic<uint64_t> jobs{0};
    std::atomic<uint64_t> cache_hits{0};
    std::atomic<uint64_t> inline_runs{0};
    std::atomic<uint64_t> waits{0};
    std::atomic<uint64_t> decode_ns{0};
    std::atomic<uint64_t> pixels{0};

    std::shared_ptr<const std::vector<uint8_t>> cache_find(uint64_t key) {
        std::lock_guard<std::mutex> lk(cache_mtx);
        auto it = cache.find(key);
        if (it == cache.end()) return nullptr;
        lru.splice(lru.begin(), lru, it->second);
        return it->second->rgba;
    }

    void cache_insert(uint64_t key, std::shared_ptr<const std::vector<uint8_t>> rgba) {
        if (cache_budget == 0 || !rgba || rgba->size() > cache_budget / 4) return;
        std::lock_guard<std::mutex> lk(cache_mtx);
        if (cache.count(key)) return;
        cache_bytes += rgba->size();
        lru.push_front({key, std::move(rgba)});
        cache[key] = lru.begin();
        while (cache_bytes > cache_budget && !lru.empty()) {
            cache_bytes -= lru.back().rgba->size();
            cache.erase(lru.back().key);
            lru.pop_back();
        }
    }
};

TextureJob::~TextureJob() {
    if (gpu_resource) {
        if (auto release = TexturePipeline::instance().release_hook()) {
            release(gpu_resource, gpu_in_srv_state);
        }
        gpu_resource = nullptr;
    }
}

TexturePipeline& TexturePipeline::instance() {
    static TexturePipeline s_instance;
    return s_instance;
}

TexturePipeline::~TexturePipeline() {
    stop();
}

void TexturePipeline::start(TextureDecodeFn decode) {
    if (m_impl) return;
    m_decode = decode;
    const int hw = static_cast<int>(std::max(2u, std::thread::hardware_concurrency()));
    const int workers = std::clamp(env_int("LCS_TEX_WORKERS", std::clamp(hw / 4, 2, 4)), 0, 32);
    const int cache_mb = std::clamp(env_int("LCS_TEX_CACHE_MB", 256), 0, 8192);
    if (workers == 0 || !decode) {
        std::cout << "[TexturePipeline] Disabled (synchronous texture decode).\n";
        return;
    }

    m_impl = new Impl();
    m_impl->cache_budget = static_cast<size_t>(cache_mb) * 1024u * 1024u;
    m_worker_count = workers;
    for (int i = 0; i < workers; ++i) {
        m_impl->threads.emplace_back([this, i]() {
#if defined(LCS_PLATFORM_WINDOWS)
            wchar_t name[32];
            swprintf(name, 32, L"TexDecode%d", i);
            SetThreadDescription(GetCurrentThread(), name);
            // Decode is background work: never preempt the game's render/logic threads.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
            (void)i;
#endif
            worker_main();
        });
    }
    m_enabled = true;
    std::cout << "[TexturePipeline] " << workers << " decode workers, "
              << cache_mb << " MB decoded-texture cache.\n";
}

void TexturePipeline::stop() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->queue_mtx);
        m_impl->stopping = true;
    }
    m_impl->queue_cv.notify_all();
    for (auto& t : m_impl->threads) {
        if (t.joinable()) t.join();
    }
    const Stats s = stats();
    std::cout << "[TexturePipeline] jobs=" << s.jobs << " cache_hits=" << s.cache_hits
              << " inline=" << s.inline_runs << " waits=" << s.waits
              << " decoded_mpx=" << (s.pixels / 1000000ULL)
              << " decode_ms=" << (s.decode_ns / 1000000ULL) << "\n";
    // Jobs still queued are dropped; their owners see them as never finished.
    m_impl->queue.clear();
    m_enabled = false;
    delete m_impl;
    m_impl = nullptr;
}

void TexturePipeline::set_resource_hooks(TextureResourceAcquireFn acquire, TextureResourceReleaseFn release) {
    m_acquire.store(acquire, std::memory_order_release);
    m_release.store(release, std::memory_order_release);
}

std::shared_ptr<TextureJob> TexturePipeline::submit(uint32_t internal_format, uint32_t width, uint32_t height,
                                                    int32_t image_size, const void* data) {
    if (!m_enabled || !data || image_size <= 0 || width == 0 || height == 0) return nullptr;
    auto job = std::make_shared<TextureJob>();
    job->internal_format = internal_format;
    job->width = width;
    job->height = height;
    job->src.resize(static_cast<size_t>(image_size));
    std::memcpy(job->src.data(), data, job->src.size());
    m_impl->jobs.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(m_impl->queue_mtx);
        m_impl->queue.push_back(job);
    }
    m_impl->queue_cv.notify_one();
    return job;
}

void TexturePipeline::complete(TextureJob& job) {
    if (job.done()) return;
    int expected = TextureJob::kQueued;
    if (job.state.compare_exchange_strong(expected, TextureJob::kRunning, std::memory_order_acq_rel)) {
        job.ran_inline = true;
        if (m_impl) m_impl->inline_runs.fetch_add(1, std::memory_order_relaxed);
        run(job);
        return;
    }
    if (m_impl) m_impl->waits.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(job.mtx);
    job.cv.wait(lk, [&job]() { return job.done(); });
}

void TexturePipeline::run(TextureJob& job) {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t h = hash_bytes(job.src.data(), job.src.size());
    const uint64_t key = h ^ (uint64_t(job.internal_format) * 0x9E3779B97F4A7C15ULL) ^
                         (uint64_t(job.width) << 40) ^ (uint64_t(job.height) << 20);

    std::shared_ptr<const std::vector<uint8_t>> rgba = m_impl ? m_impl->cache_find(key) : nullptr;
    if (rgba) {
        m_impl->cache_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
        auto out = std::make_shared<std::vector<uint8_t>>();
        if (m_decode(job.internal_format, job.width, job.height,
                     static_cast<int32_t>(job.src.size()), job.src.data(), *out)) {
            rgba = std::move(out);
            if (m_impl) {
                m_impl->pixels.fetch_add(uint64_t(job.width) * job.height, std::memory_order_relaxed);
                m_impl->cache_insert(key, rgba);
            }
        }
    }
    job.ok = (rgba != nullptr);
    job.rgba = std::move(rgba);
    if (job.ok) {
        if (auto acquire = m_acquire.load(std::memory_order_acquire)) {
            job.gpu_resource = acquire(job.width, job.height, job.gpu_in_srv_state);
        }
    }
    std::vector<uint8_t>().swap(job.src);

    if (m_impl) {
        m_impl->decode_ns.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
            std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lk(job.mtx);
        job.state.store(TextureJob::kDone, std::memory_order_release);
    }
    job.cv.notify_all();
}

void TexturePipeline::worker_main() {
    for (;;) {
        std::shared_ptr<TextureJob> job;
        {
            std::unique_lock<std::mutex> lk(m_impl->queue_mtx);
            m_impl->queue_cv.wait(lk, [this]() { return m_impl->stopping || !m_impl->queue.empty(); });
            if (m_impl->stopping) return;
            job = std::move(m_impl->queue.front());
            m_impl->queue.pop_front();
        }
        int expected = TextureJob::kQueued;
        // A job whose texture was deleted meanwhile is only referenced by us: skip it.
        if (job.use_count() > 1 &&
            job->state.compare_exchange_strong(expected, TextureJob::kRunning, std::memory_order_acq_rel)) {
            run(*job);
        }
    }
}

TexturePipeline::Stats TexturePipeline::stats() const {
    Stats s;
    if (!m_impl) return s;
    s.jobs = m_impl->jobs.load(std::memory_order_relaxed);
    s.cache_hits = m_impl->cache_hits.load(std::memory_order_relaxed);
    s.inline_runs = m_impl->inline_runs.load(std::memory_order_relaxed);
    s.waits = m_impl->waits.load(std::memory_order_relaxed);
    s.decode_ns = m_impl->decode_ns.load(std::memory_order_relaxed);
    s.pixels = m_impl->pixels.load(std::memory_order_relaxed);
    return s;
}

} // namespace lcs::graphics
