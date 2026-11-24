#include "game/lcs_world_hd.h"
#include "game/lcs_batched_fast.h"
#include <chrono>
#include <condition_variable>
#include <thread>

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "vfs/logical_fs.h"
#include "settings/settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

// Verified libGame.so RVAs & GOT slots (arm64-v8a)
constexpr uint64_t kGotWsSingletonRva      = 0x7f9a30ULL; // cSingleton<cWorldStream>::mspInstance
constexpr uint64_t kGotWorldMgrRva         = 0x7fab50ULL; // gWorldManager
constexpr uint64_t kGotWorldCreatorRva     = 0x7fab88ULL; // gWorldCreator
constexpr uint64_t kGotCameraRva           = 0x7f9190ULL; // TheCamera
constexpr uint64_t kGotDynShadowsActiveRva = 0x7f9880ULL; // CDynamicShadows::s_bActive
constexpr uint64_t kGotWaterScrollRva      = 0x7f9750ULL; // gCurrentViewOffs

constexpr uint64_t kGotWsRenderRva         = 0x8040A0ULL; // cWorldStream::Render(int)
constexpr uint64_t kGotWsUpdateRva         = 0x8044F0ULL; // cWorldStream::Update()
constexpr uint64_t kGotWsLoadLevelRva      = 0x8044B8ULL; // cWorldStream::LoadLevel(eLevelName)
constexpr uint64_t kGotWsLoadSceneRva      = 0x8044C0ULL; // cWorldStream::LoadScene(CVector const&)
constexpr uint64_t kGotWsRemoveOldRva      = 0x804448ULL; // cWorldStream::RemoveOldSectorModels()
constexpr uint64_t kGotWsSwapSectorRva     = 0x8044c8ULL; // cWorldStream::SwapToStreamedSector()
constexpr uint64_t kGotWsSwapInBufRva      = 0x804498ULL; // cWorldStream::SwapInStreamingBuffer()
constexpr uint64_t kGotWsStreamRva         = 0x804480ULL; // cWorldStream::Stream(base::sChunkHeader*)
constexpr uint64_t kGotWsPollStreamingRva  = 0x804468ULL; // cWorldStream::PollStreaming(bool)
constexpr uint64_t kGotWsCancelStreamRva   = 0x804440ULL; // cWorldStream::CancelStreaming(bool)
constexpr uint64_t kGotWsStreamingCbRva    = 0x804590ULL; // cWorldStream::StreamingCallback(cUmdRequest*)
constexpr uint64_t kGotLglDestroyRva       = 0x804470ULL; // lglWorldManager::destroy(bool)
constexpr uint64_t kGotIsLucidCreatedRva   = 0x808aa8ULL; // IsLucidWorldCreated(sDMAModel*)
constexpr uint64_t kGotLglTexGetRva        = 0x808848ULL; // lglTextureManager::getTexture(...)
constexpr uint64_t kGotLglTexDestroyRva    = 0x8095E0ULL; // lglTextureManager::destroyTexture(C_Texture*)
constexpr uint64_t kGotDispReleaseTexRva   = 0x807898ULL; // Display::ReleaseTexture(C_Texture*&)

constexpr uint64_t kSymFixupRva            = 0x2d5160ULL; // base::cRelocatableChunk::Fixup
constexpr uint64_t kSymGetModelRva         = 0x5887b8ULL; // lglWorldManager::getModel
constexpr uint64_t kSymDestroyModelRva     = 0x58859cULL; // lglWorldManager::destroy(uint64_t, bool)
constexpr uint64_t kSymCreatorRemoveRva    = 0x588fd8ULL; // lglWorldCreator::remove(uint64_t)
constexpr uint64_t kSymLglMutexLockRva     = 0x585b18ULL; // lglMutex::lock()
constexpr uint64_t kSymLglMutexUnlockRva   = 0x585b20ULL; // lglMutex::unlock()
constexpr uint64_t kSymIsTimeInRangeRva    = 0x32ea2cULL; // CClock::GetIsTimeInRange

constexpr uint64_t kGotCullFrustumRva       = 0x7fe8d0ULL; // Display::C_Culling::s_frustum
constexpr uint64_t kGotCullViewMatRva       = 0x7fe218ULL; // Display::C_Culling::s_viewMatrix
constexpr uint64_t kGotCullChangeFlagRva    = 0x7fe898ULL; // Display::C_Culling::s_changeFlag
constexpr uint64_t kGotCullIsPerspRva       = 0x7fe8a8ULL; // Display::C_Culling::s_bIsPerspective
constexpr uint64_t kGotCullSphereEnabledRva = 0x7fe8f0ULL; // gSphereCullingEnabled (+0x3d)
constexpr uint64_t kGotClipSphereFudgeRva   = 0x7facc8ULL; // gClipSphereFudge
constexpr uint64_t kSymCullingUpdateRva     = 0x581784ULL; // Display::C_Culling::Update(ECullSpace)
constexpr size_t   kMaxFilteredInstances    = 16384;       // Fits inside 2 MiB tail slack

constexpr uint32_t kDlrwMagic              = 0x57524c44u; // 'DLRW'
constexpr size_t   kGeomInstSize           = 0x78ULL;     // sizeof(sGeomInstance) = 120 bytes
constexpr size_t   kNumRenderBuckets       = 7;           // buckets[0..6]; buckets[7] is sentinel end
constexpr size_t   kMaxCombinedInstances   = 32768;
constexpr size_t   kArenaTotalBytes        = 144ULL * 1024ULL * 1024ULL;
constexpr size_t   kCellPoolBytes          = 134ULL * 1024ULL * 1024ULL;
constexpr int      kMaxCachedCells         = 160;

inline float cell_active_dist_sq(int radius) {
    const float d = (radius >= 3) ? 500.0f : (static_cast<float>(radius) * 95.0f + 65.0f);
    return d * d;
}

inline float cell_preload_dist_sq(int radius) {
    const float d = (radius >= 3) ? 560.0f : (static_cast<float>(radius) * 105.0f + 85.0f);
    return d * d;
}

static inline lcs::cpu::DynarmicCore* active_cpu(lcs::cpu::DynarmicCore* fallback) {
    if (auto* cur = lcs::cpu::DynarmicCore::current()) return cur;
    return fallback;
}

// Struct offsets inside cWorldStream (0x8ab0 bytes)
constexpr size_t   kWsArenaSizeOff         = 0x10ULL;
constexpr size_t   kWsCurrLevelOff         = 0x14ULL;
constexpr size_t   kWsDirOff               = 0x18ULL;
constexpr size_t   kWsArenaPtrOff          = 0x20ULL;
constexpr size_t   kWsCurBaseOff           = 0x30ULL;
constexpr size_t   kWsCurBeginOff          = 0x38ULL;
constexpr size_t   kWsCurEndOff            = 0x40ULL;
constexpr size_t   kWsCurCapOff            = 0x48ULL;
constexpr size_t   kWsCurCxOff             = 0x50ULL;
constexpr size_t   kWsCurCyOff             = 0x54ULL;
constexpr size_t   kWsWaterFadeOff         = 0x58ULL;
constexpr size_t   kWsPrevBaseOff          = 0x78ULL;
constexpr size_t   kWsPrevBeginOff         = 0x80ULL;
constexpr size_t   kWsPrevEndOff           = 0x88ULL;
constexpr size_t   kWsPrevCapOff           = 0x90ULL;
constexpr size_t   kWsStreamBaseOff        = 0xc8ULL;
constexpr size_t   kWsAreaStatesOff        = 0x3b4ULL;

#pragma pack(push, 1)
struct sChunkHeader {
    uint32_t magic;
    uint32_t shrinkFlag;
    uint32_t totalSize;
    uint32_t shrinkOffset;
    uint32_t relocOffset;
    uint32_t numRelocs;
    uint32_t offsetInImg;
    uint16_t unk1c;
    uint16_t unk1e;
};
static_assert(sizeof(sChunkHeader) == 0x20);

struct sDirRowEntry {
    uint64_t chunks_ptr;
    int32_t  min_x;
    uint32_t pad;
};
static_assert(sizeof(sDirRowEntry) == 0x10);

struct sBuildingDirEntry {
    uint64_t geomPtr;
    uint64_t glTex;
    uint32_t flags;
    uint32_t pad;
};
static_assert(sizeof(sBuildingDirEntry) == 0x18);

struct sPairEntry {
    uint64_t geomPtr;
    uint32_t gid;
    uint32_t pad;
};
static_assert(sizeof(sPairEntry) == 0x10);

struct sAreaEntry {
    uint8_t  flag;
    uint8_t  val;
    uint16_t inst_id;
};
static_assert(sizeof(sAreaEntry) == 0x04);
#pragma pack(pop)

inline uint32_t ldr_x16_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000010u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

void patch_bytes_prot(lcs::cpu::DynarmicCore& cpu, uint64_t va, const void* src, size_t sz) {
#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(va), sz, PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(reinterpret_cast<void*>(va), src, sz);
    VirtualProtect(reinterpret_cast<void*>(va), sz, old_prot, &old_prot);
#else
    std::memcpy(reinterpret_cast<void*>(va), src, sz);
#endif
    cpu.invalidate_cache(va, sz);
}

uint64_t patch_got_entry(lcs::cpu::DynarmicCore& cpu, uint64_t load_bias, uint64_t got_rva,
                         uint64_t new_target, const lcs::cpu::ElfImage& img, const char* sym) {
    const uint64_t slot = load_bias + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot);
    if (!orig && sym) orig = img.find_symbol(sym);
    patch_bytes_prot(cpu, slot, &new_target, sizeof(uint64_t));
    return orig;
}

const char* level_img_name(int level) {
    switch (level) {
        case 1: return "Models/indust.img";
        case 2: return "Models/commer.img";
        case 3: return "Models/suburb.img";
        case 4: return "Models/underg.img";
        default: return nullptr;
    }
}

inline uint64_t pack_cell_key(int32_t cx, int32_t cy) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) |
           static_cast<uint32_t>(cy);
}

inline void world_to_hex_cell(float wx, float wy, int32_t& out_cx, int32_t& out_cy) {
    out_cy = static_cast<int32_t>((wy + 2000.0f) * 0.011538461f);
    out_cx = static_cast<int32_t>((wx + 2000.0f) * 0.01f + static_cast<float>(out_cy & 1) * 0.5f);
}

inline void hex_cell_center(int32_t cx, int32_t cy, float& out_x, float& out_y) {
    const float sx = static_cast<float>(cx) + 0.5f - static_cast<float>(cy & 1) * 0.5f;
    const float sy = static_cast<float>(cy) + 0.5f;
    out_x = sx * 100.0f - 2000.0f;
    out_y = sy * 86.6f - 2000.0f;
}

inline float f16_to_f32(uint16_t h) {
    const uint32_t sign = (static_cast<uint32_t>(h & 0x8000u)) << 16;
    const uint32_t exp  = (h >> 10) & 0x1fu;
    const uint32_t mant = h & 0x03ffu;
    uint32_t fbits = 0;
    if (exp == 0) {
        if (mant == 0) {
            fbits = sign;
        } else {
            uint32_t m = mant;
            int e = 127 - 15 + 1;
            while ((m & 0x0400u) == 0) {
                m <<= 1;
                --e;
            }
            m &= 0x03ffu;
            fbits = sign | (static_cast<uint32_t>(e) << 23) | (m << 13);
        }
    } else if (exp == 31) {
        fbits = sign | 0x7f800000u | (mant << 13);
    } else {
        fbits = sign | ((exp + (127u - 15u)) << 23) | (mant << 13);
    }
    float out = 0.0f;
    std::memcpy(&out, &fbits, sizeof(out));
    return out;
}

inline uint16_t f32_to_f16(float f) {
    uint32_t x = 0;
    std::memcpy(&x, &f, sizeof(x));
    const uint16_t sign = static_cast<uint16_t>((x >> 16) & 0x8000u);
    const int32_t exp = static_cast<int32_t>((x >> 23) & 0xffu);
    const uint32_t mant = x & 0x7fffffu;
    if (exp == 0xff) {
        return static_cast<uint16_t>(sign | 0x7c00u | (mant ? 1u : 0u));
    }
    const int32_t new_exp = exp - 127 + 15;
    if (new_exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7c00u);
    }
    if (new_exp <= 0) {
        if (new_exp < -10) return sign;
        const uint32_t m = (0x800000u | mant) >> static_cast<uint32_t>(14 - new_exp);
        return static_cast<uint16_t>(sign | (m & 0x03ffu));
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(new_exp) << 10) | (mant >> 13));
}

// World-stream instance ids split in two disjoint ranges (measured over 90 loaded cell lists):
// detailed geometry below ~3320 (small spheres, in a few lists) and the resident low-detail
// (LQ) image of the island from ~4715 up (large spheres, in most lists). A cell's list leaves
// out the LQ instances its own detailed geometry replaces.
constexpr uint16_t kLqInstanceIdMin = 4700;

inline uint32_t instance_key(uint16_t raw_id, uint16_t gid) {
    return (static_cast<uint32_t>(raw_id & 0x7fffu) << 16) | gid;
}

// Per cell list: which LQ instances it keeps, and where its detailed geometry is.
struct LqIndex {
    bool built = false;
    float center_x = 0.0f, center_y = 0.0f;
    float detail_extent = 0.0f;                 // farthest detailed sphere centre from the cell centre
    std::unordered_set<uint32_t> lq_keys;
    std::vector<std::array<float, 2>> detail_xy; // world-space centres of detailed instances

    // `native`: the game's own list (hidden records are not drawn). HD cell lists are drawn
    // with bit 15 cleared, but only records whose geometry is resident (table[gid].geomPtr).
    void build(const uint64_t* buckets, float cell_x, float cell_y, bool native,
               const sBuildingDirEntry* table, int32_t table_count) {
        lq_keys.clear();
        detail_xy.clear();
        center_x = cell_x;
        center_y = cell_y;
        detail_extent = 0.0f;
        for (size_t b = 0; b < 7; ++b) {
            for (uint64_t p = buckets[b]; p < buckets[b + 1]; p += 0x78ULL) {
                uint16_t raw_id, gid, hcx, hcy;
                std::memcpy(&raw_id, reinterpret_cast<const void*>(p), 2);
                std::memcpy(&gid, reinterpret_cast<const void*>(p + 2), 2);
                if ((raw_id & 0x7fffu) >= kLqInstanceIdMin) {
                    if (!(raw_id & 0x8000u)) lq_keys.insert(instance_key(raw_id, gid));
                    continue;
                }
                // Only drawn detail replaces anything.
                if (native ? (raw_id & 0x8000u) != 0
                           : (gid >= static_cast<uint32_t>(table_count) || !table[gid].geomPtr)) continue;
                std::memcpy(&hcx, reinterpret_cast<const void*>(p + 4), 2);
                std::memcpy(&hcy, reinterpret_cast<const void*>(p + 6), 2);
                const float x = f16_to_f32(hcx);  // spheres are world coordinates
                const float y = f16_to_f32(hcy);
                const float from_cell = std::hypot(x - cell_x, y - cell_y);
                // Records with their own matrix (+0x30) are not cell-relative: their sphere
                // cannot be placed this way (they showed up 400-530 m from their cell).
                if (*reinterpret_cast<const uint64_t*>(p + 0x30ULL) != 0 || from_cell > 250.0f) continue;
                detail_xy.push_back({x, y});
                detail_extent = std::max(detail_extent, from_cell);
            }
        }
        built = true;
    }

    // True when this list drops the LQ instance and has detailed geometry inside its sphere,
    // i.e. the list's own detail replaces it.
    bool replaces(uint32_t key, float x, float y, float r) const {
        if (lq_keys.count(key)) return false;
        if (std::hypot(x - center_x, y - center_y) > r + detail_extent) return false;
        const float r2 = r * r;
        for (const auto& d : detail_xy) {
            const float dx = d[0] - x, dy = d[1] - y;
            if (dx * dx + dy * dy <= r2) return true;
        }
        return false;
    }
};

} // namespace

struct LcsWorldHd::Impl {
    struct ArenaSpan {
        size_t offset = 0;
        size_t size   = 0;
        bool   used   = false;
    };

    struct CachedCell {
        int32_t level = 0;
        int32_t cx    = 0;
        int32_t cy    = 0;
        size_t  arena_offset = 0;
        size_t  alloc_size   = 0;
        uint8_t* payload     = nullptr;
        size_t  payload_size = 0;
        uint64_t last_used_frame = 0;
        std::vector<uint32_t> pair_gids;
        std::vector<uint32_t> unique_gids;
        std::vector<uint64_t> dma_models;
        bool models_registered = false;
        bool gpu_ready = false;
        LqIndex lq_index;  // built lazily the first time the cell is drawn
    };

    struct SavedCurState {
        bool     active = false;
        uint64_t ws_ptr = 0;
        uint64_t cur_begin = 0;
        uint64_t cur_end = 0;
        uint64_t prev_end = 0;
    };

    uint8_t* arena_base = nullptr;
    uint8_t* stub_page  = nullptr;

    // Sub-region pointers inside arena_base[kCellPoolBytes .. kArenaTotalBytes) (3 MiB tail)
    uint8_t*  combined_chunk_hdr_va = nullptr; // sChunkHeader scratch for Fixup
    uint8_t*  combined_chunk_bufs[2]{nullptr, nullptr}; // Double-buffered synthetic sLevelChunk
    uint8_t*  combined_insts_bufs[2]{nullptr, nullptr}; // Double-buffered sGeomInstance arrays
    uint64_t* combined_vec_slot     = nullptr; // Single-element vector backing { &combined_chunk }
    uint64_t* temp_hdr_fixup        = nullptr; // Scratch sChunkHeader for Fixup call
    int       published_buf_idx     = 0;
    int       render_buf_idx        = 0;

    struct CachedCullSphere {
        float    cx = 0.0f;
        float    cy = 0.0f;
        float    cz = 0.0f;
        float    r  = 0.0f;
        uint8_t  keep_always = 0;
        uint8_t  pad[3]{};
    };
    uint8_t*  filtered_chunk_buf    = nullptr;
    uint8_t*  filtered_insts_buf    = nullptr;
    std::vector<CachedCullSphere> cached_spheres[2];
    uint32_t  cached_bucket_idx[2][kNumRenderBuckets + 1]{};
    uint64_t  got_cull_frustum        = 0;
    uint64_t  got_cull_view_mat       = 0;
    uint64_t  got_cull_change_flag    = 0;
    uint64_t  got_cull_is_persp       = 0;
    uint64_t  got_cull_sphere_enabled = 0;
    uint64_t  got_clip_sphere_fudge   = 0;

    mutable std::recursive_mutex mtx;
    mutable std::recursive_mutex ws_mtx;       // Serializes cWorldStream::Render (RenderThread) vs Update/LoadLevel/LoadScene
    mutable std::recursive_mutex tex_mgr_mtx;  // Serializes lglTextureManager RB-tree & refcount ops
    mutable std::mutex           stream_mtx;   // Coordinates StreamingCallback (cUmdThread) vs Update/Stream/CancelStreaming
    std::condition_variable      stream_cv;
    int                          stream_cb_depth = 0;
    std::thread::id              stream_cb_owner{};
    int                          gtt_stream_depth = 0;
    std::thread::id              gtt_stream_owner{};
    mutable std::mutex covered_mtx;
    std::atomic<int> cell_count{0};
    std::atomic<int> ready_cell_count{0};
    std::atomic<int> render_depth{0};

    std::vector<ArenaSpan> spans;
    std::unordered_map<uint64_t, CachedCell> cells;
    std::unordered_set<uint64_t> covered_cells;
    std::array<std::atomic<uint64_t>, 64> covered_rows{};
    std::atomic<bool> covered_nonempty{false};
    std::atomic<bool> covered_has_oob{false};
    std::unordered_set<uint32_t> dirty_model_gids;

    void sync_covered_fast() {
        uint64_t rows[64]{};
        bool oob = false;
        const bool nonempty = !covered_cells.empty();
        for (uint64_t key : covered_cells) {
            const int32_t cx = static_cast<int32_t>(static_cast<uint32_t>(key >> 32));
            const int32_t cy = static_cast<int32_t>(static_cast<uint32_t>(key & 0xffffffffULL));
            if (static_cast<uint32_t>(cx) < 64u && static_cast<uint32_t>(cy) < 64u) {
                rows[cy] |= (1ULL << cx);
            } else {
                oob = true;
            }
        }
        for (size_t i = 0; i < 64; ++i) {
            covered_rows[i].store(rows[i], std::memory_order_relaxed);
        }
        covered_has_oob.store(oob, std::memory_order_relaxed);
        covered_nonempty.store(nonempty, std::memory_order_release);
    }

    FILE* img_file = nullptr;
    int   img_level = 0;

    SavedCurState saved_render;
    SavedCurState saved_remove;
    uint64_t orig_swap_sector = 0;
    uint64_t orig_swap_in_buf = 0;
    uint64_t orig_lgl_destroy = 0;
    uint64_t orig_load_scene = 0;
    uint64_t orig_stream = 0;
    uint64_t orig_poll_streaming = 0;
    uint64_t orig_cancel_streaming = 0;
    uint64_t orig_streaming_cb = 0;
    uint64_t orig_tex_get = 0;
    uint64_t orig_tex_destroy = 0;
    uint64_t orig_disp_release_tex = 0;
    uint64_t fn_is_lucid_created = 0;
    bool     allow_lgl_destroy = false;

    uint64_t stats_cells_loaded = 0;
    uint64_t stats_cells_evicted = 0;
    uint64_t stats_draws_swapped = 0;
    uint64_t last_native_chunk = 0;
    // LQ index of the game's current (native) cell list, rebuilt when the native chunk changes
    LqIndex native_lq_index;
    uint64_t native_lq_chunk = 0;
    int last_lq_replaced = 0;
    float    last_cam_x = 0.0f;
    float    last_cam_y = 0.0f;
    bool     first_scene_pending = true;

    void reset_allocator() {
        spans.clear();
        spans.push_back({0, kCellPoolBytes, false});
    }

    bool alloc_block(size_t bytes, size_t& out_offset, size_t& out_size) {
        const size_t aligned = (bytes + 63ULL) & ~size_t(63);
        for (size_t i = 0; i < spans.size(); ++i) {
            if (!spans[i].used && spans[i].size >= aligned) {
                out_offset = spans[i].offset;
                out_size = aligned;
                const size_t rem = spans[i].size - aligned;
                spans[i].size = aligned;
                spans[i].used = true;
                if (rem >= 64) {
                    spans.insert(spans.begin() + static_cast<ptrdiff_t>(i + 1),
                                 ArenaSpan{out_offset + aligned, rem, false});
                } else {
                    spans[i].size += rem;
                    out_size = spans[i].size;
                }
                return true;
            }
        }
        return false;
    }

    void free_block(size_t offset) {
        for (size_t i = 0; i < spans.size(); ++i) {
            if (spans[i].used && spans[i].offset == offset) {
                spans[i].used = false;
                if (i + 1 < spans.size() && !spans[i + 1].used) {
                    spans[i].size += spans[i + 1].size;
                    spans.erase(spans.begin() + static_cast<ptrdiff_t>(i + 1));
                }
                if (i > 0 && !spans[i - 1].used) {
                    spans[i - 1].size += spans[i].size;
                    spans.erase(spans.begin() + static_cast<ptrdiff_t>(i));
                }
                return;
            }
        }
    }
};

struct LcsWorldHd::HookOps {
    static uint64_t render_pre(uint64_t ws, uint64_t pass) {
        return LcsWorldHd::instance().on_world_render_pre(ws, static_cast<int>(pass));
    }
    static uint64_t render_post(uint64_t ws) {
        LcsWorldHd::instance().on_world_render_post(ws);
        return 0;
    }
    static uint64_t update_pre(uint64_t ws) {
        LcsWorldHd::instance().on_world_update_pre(ws);
        return 0;
    }
    static uint64_t update_post(uint64_t ws) {
        LcsWorldHd::instance().on_world_update_post(ws);
        return 0;
    }
    static uint64_t load_level_pre(uint64_t ws, uint64_t lvl) {
        LcsWorldHd::instance().on_world_load_level_pre(ws, static_cast<int>(lvl));
        return 0;
    }
    static uint64_t load_level_post(uint64_t ws) {
        LcsWorldHd::instance().on_world_load_level_post(ws);
        return 0;
    }
    static uint64_t load_scene_pre(uint64_t ws) {
        LcsWorldHd::instance().on_world_load_scene_pre(ws);
        return 0;
    }
    static uint64_t load_scene_post(uint64_t ws) {
        LcsWorldHd::instance().on_world_load_scene_post(ws);
        return 0;
    }
    static uint64_t stream_call_pre(uint64_t ws, uint64_t hdr) {
        LcsWorldHd::instance().on_stream_call_pre(ws, hdr);
        return 0;
    }
    static uint64_t stream_post(uint64_t ws) {
        LcsWorldHd::instance().on_stream_post(ws);
        return 0;
    }
    static uint64_t poll_streaming_pre(uint64_t ws) {
        LcsWorldHd::instance().on_poll_streaming_pre(ws);
        return 0;
    }
    static uint64_t poll_streaming_post(uint64_t ws) {
        LcsWorldHd::instance().on_poll_streaming_post(ws);
        return 0;
    }
    static uint64_t cancel_streaming_pre(uint64_t ws) {
        LcsWorldHd::instance().on_cancel_streaming_pre(ws);
        return 0;
    }
    static uint64_t cancel_streaming_post(uint64_t ws) {
        LcsWorldHd::instance().on_cancel_streaming_post(ws);
        return 0;
    }
    static uint64_t streaming_cb_pre(uint64_t ws) {
        LcsWorldHd::instance().on_streaming_cb_pre(ws);
        return 0;
    }
    static uint64_t streaming_cb_post(uint64_t ws) {
        LcsWorldHd::instance().on_streaming_cb_post(ws);
        return 0;
    }
    static uint64_t swap_in_buf_pre(uint64_t ws) {
        LcsWorldHd::instance().on_swap_in_buf_pre(ws);
        return 0;
    }
    static uint64_t swap_in_buf_post(uint64_t ws) {
        LcsWorldHd::instance().on_swap_in_buf_post(ws);
        return 0;
    }
    static uint64_t tex_mgr_lock(uint64_t) {
        LcsWorldHd::instance().on_tex_mgr_lock();
        return 0;
    }
    static uint64_t tex_mgr_unlock(uint64_t) {
        LcsWorldHd::instance().on_tex_mgr_unlock();
        return 0;
    }
    static uint64_t remove_old_pre(uint64_t ws) {
        return LcsWorldHd::instance().on_remove_old_models_pre(ws);
    }
    static uint64_t remove_old_post(uint64_t ws) {
        LcsWorldHd::instance().on_remove_old_models_post(ws);
        return 0;
    }
    static uint64_t swap_pre(uint64_t ws) {
        return LcsWorldHd::instance().on_swap_sector_pre(ws);
    }
    static uint64_t swap_post(uint64_t ws) {
        LcsWorldHd::instance().on_swap_sector_post(ws);
        return 0;
    }
    static uint64_t lgl_destroy_pre(uint64_t mgr, uint64_t destroy_all) {
        return LcsWorldHd::instance().on_lgl_destroy_pre(mgr, static_cast<int>(destroy_all & 0xffu));
    }
};

LcsWorldHd::~LcsWorldHd() {
    close_level_img();
#if defined(LCS_PLATFORM_WINDOWS)
    if (m_impl) {
        if (m_impl->arena_base) {
            VirtualFree(m_impl->arena_base, 0, MEM_RELEASE);
        }
        if (m_impl->stub_page) {
            VirtualFree(m_impl->stub_page, 0, MEM_RELEASE);
        }
    }
#endif
    delete m_impl;
    m_impl = nullptr;
}

LcsWorldHd& LcsWorldHd::instance() {
    static LcsWorldHd s_instance;
    return s_instance;
}

void LcsWorldHd::set_radius(int level) {
    const int clamped = std::clamp(level, 0, 3);
    if (m_impl) {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        if (clamped != m_radius) {
            m_radius = clamped;
            m_combined_dirty = true;
            if (clamped == 0) {
                {
                    std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
                    m_impl->covered_cells.clear();
                    m_impl->sync_covered_fast();
                }
                m_combined_inst_count = 0;
            } else {
                m_impl->first_scene_pending = true;
            }
            std::cout << "  [WorldHD] Radius set to " << m_radius << "\n";
        }
        return;
    }
    if (clamped != m_radius) {
        m_radius = clamped;
        m_combined_dirty = true;
        std::cout << "  [WorldHD] Radius set to " << m_radius << "\n";
    }
}

int LcsWorldHd::loaded_cell_count() const {
    return m_impl ? m_impl->cell_count.load(std::memory_order_relaxed) : 0;
}

LcsWorldHd::Stats LcsWorldHd::get_stats() const {
    Stats st{};
    st.radius = m_radius;
    st.active_level = m_active_level;
    st.center_cx = m_center_cx;
    st.center_cy = m_center_cy;
    st.combined_instances = m_combined_inst_count;
    if (m_impl) {
        st.cached_cells = m_impl->cell_count.load(std::memory_order_relaxed);
        st.ready_cells = m_impl->ready_cell_count.load(std::memory_order_relaxed);
        st.lq_replaced = m_impl->last_lq_replaced;
    }
    return st;
}

bool LcsWorldHd::is_point_covered_by_hd(float x, float y) const {
    if (m_radius <= 0 || !m_impl) {
        return false;
    }
    int32_t cx = 0, cy = 0;
    world_to_hex_cell(x, y, cx, cy);
    if (!m_impl->covered_has_oob.load(std::memory_order_acquire)) {
        if (!m_impl->covered_nonempty.load(std::memory_order_relaxed)) return false;
        if (static_cast<uint32_t>(cx) >= 64u || static_cast<uint32_t>(cy) >= 64u) return false;
        return (m_impl->covered_rows[cy].load(std::memory_order_relaxed) & (1ULL << cx)) != 0ULL;
    }
    std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
    if (m_impl->covered_cells.empty()) return false;
    return m_impl->covered_cells.find(pack_cell_key(cx, cy)) != m_impl->covered_cells.end();
}

bool LcsWorldHd::is_circle_covered_by_hd(float x, float y, float radius) const {
    if (m_radius <= 0 || !m_impl) {
        return false;
    }
    if (!m_impl->covered_has_oob.load(std::memory_order_acquire)) {
        if (!m_impl->covered_nonempty.load(std::memory_order_relaxed)) return false;
        auto cell_hit_fast = [&](float px, float py) -> bool {
            int32_t cx = 0, cy = 0;
            world_to_hex_cell(px, py, cx, cy);
            if (static_cast<uint32_t>(cx) >= 64u || static_cast<uint32_t>(cy) >= 64u) return false;
            return (m_impl->covered_rows[cy].load(std::memory_order_relaxed) & (1ULL << cx)) != 0ULL;
        };
        if (cell_hit_fast(x, y)) return true;

        if (radius > 8.0f) {
            static constexpr float kDirX[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
            static constexpr float kDirY[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};
            const float r1 = std::clamp(radius * 0.50f, 18.0f, 120.0f);
            for (int i = 0; i < 8; ++i) {
                if (cell_hit_fast(x + kDirX[i] * r1, y + kDirY[i] * r1)) return true;
            }
            if (radius > 45.0f) {
                const float r2 = std::clamp(radius * 0.85f, 36.0f, 165.0f);
                for (int i = 0; i < 8; ++i) {
                    if (cell_hit_fast(x + kDirX[i] * r2, y + kDirY[i] * r2)) return true;
                }
            }
        }
        return false;
    }
    std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
    if (m_impl->covered_cells.empty()) return false;

    auto cell_hit = [&](float px, float py) -> bool {
        int32_t cx = 0, cy = 0;
        world_to_hex_cell(px, py, cx, cy);
        return m_impl->covered_cells.find(pack_cell_key(cx, cy)) != m_impl->covered_cells.end();
    };
    if (cell_hit(x, y)) return true;

    if (radius > 8.0f) {
        // Test footprint ring inside the BigBuilding's bounding radius so coastal/border/block-spanning
        // LOD* meshes anchored in empty water or outer cells cannot protrude over HD geometry.
        static constexpr float kDirX[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
        static constexpr float kDirY[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};
        const float r1 = std::clamp(radius * 0.50f, 18.0f, 120.0f);
        for (int i = 0; i < 8; ++i) {
            if (cell_hit(x + kDirX[i] * r1, y + kDirY[i] * r1)) return true;
        }
        if (radius > 45.0f) {
            const float r2 = std::clamp(radius * 0.85f, 36.0f, 165.0f);
            for (int i = 0; i < 8; ++i) {
                if (cell_hit(x + kDirX[i] * r2, y + kDirY[i] * r2)) return true;
            }
        }
    }
    return false;
}

void LcsWorldHd::initialize(const lcs::cpu::ElfImage& image_game,
                            lcs::cpu::DynarmicCore& cpu,
                            bool enable_dump) {
    m_enable_dump = enable_dump;
    if (m_initialized) return;

    m_cpu = &cpu;
    m_load_bias = image_game.get_load_bias();
    m_impl = new Impl();

    if (!init_arena_and_stubs(image_game, cpu)) {
        std::cerr << "  [WorldHD] WARN: Arena/stub initialization failed; HD streaming disabled.\n";
        return;
    }

    m_initialized = true;
    std::cout << "  [WorldHD] Initialized Multi-Cell HD cWorldStream ("
              << (kArenaTotalBytes / (1024 * 1024)) << " MiB arena, radius=" << m_radius << ").\n";
}

bool LcsWorldHd::init_arena_and_stubs(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
#if defined(LCS_PLATFORM_WINDOWS)
    void* arena = VirtualAlloc(nullptr, kArenaTotalBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* stub  = VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* arena = std::aligned_alloc(4096, kArenaTotalBytes);
    void* stub  = nullptr;
#endif
    if (!arena || !stub) {
        return false;
    }
    std::memset(arena, 0, kArenaTotalBytes);
    std::memset(stub, 0, 0x4000);

    m_impl->arena_base = static_cast<uint8_t*>(arena);
    m_impl->stub_page  = static_cast<uint8_t*>(stub);
    m_impl->reset_allocator();

    uint8_t* tail = m_impl->arena_base + kCellPoolBytes;
    m_impl->temp_hdr_fixup           = reinterpret_cast<uint64_t*>(tail + 0x0000ULL);     // 64 bytes
    m_impl->combined_vec_slot        = reinterpret_cast<uint64_t*>(tail + 0x0100ULL);     // 16 bytes
    m_impl->combined_chunk_bufs[0]   = tail + 0x0200ULL;                                  // 0x70 bytes header + 32768 * 120B (< 4 MiB)
    m_impl->combined_insts_bufs[0]   = m_impl->combined_chunk_bufs[0] + 0x70ULL;
    m_impl->combined_chunk_bufs[1]   = tail + 0x400200ULL;                                // Second ~4 MiB double buffer
    m_impl->combined_insts_bufs[1]   = m_impl->combined_chunk_bufs[1] + 0x70ULL;
    m_impl->filtered_chunk_buf       = tail + 0x800200ULL;                                // Per-pass frustum-filtered sLevelChunk
    m_impl->filtered_insts_buf       = m_impl->filtered_chunk_buf + 0x70ULL;
    m_impl->published_buf_idx        = 0;
    m_impl->render_buf_idx           = 0;
    m_impl->combined_vec_slot[0]     = reinterpret_cast<uint64_t>(m_impl->combined_chunk_bufs[0]);

    m_got_ws_inst            = m_load_bias + kGotWsSingletonRva;
    m_got_world_mgr          = m_load_bias + kGotWorldMgrRva;
    m_got_world_creator      = m_load_bias + kGotWorldCreatorRva;
    m_got_camera             = m_load_bias + kGotCameraRva;
    m_got_dyn_shadows_active = m_load_bias + kGotDynShadowsActiveRva;
    m_got_water_scroll       = m_load_bias + kGotWaterScrollRva;
    m_impl->got_cull_frustum        = m_load_bias + kGotCullFrustumRva;
    m_impl->got_cull_view_mat       = m_load_bias + kGotCullViewMatRva;
    m_impl->got_cull_change_flag    = m_load_bias + kGotCullChangeFlagRva;
    m_impl->got_cull_is_persp       = m_load_bias + kGotCullIsPerspRva;
    m_impl->got_cull_sphere_enabled = m_load_bias + kGotCullSphereEnabledRva;
    m_impl->got_clip_sphere_fudge   = m_load_bias + kGotClipSphereFudgeRva;

    auto resolve = [&](uint64_t rva, const char* sym) -> uint64_t {
        if (sym) {
            if (uint64_t s = image_game.find_symbol(sym)) return s;
        }
        return m_load_bias + rva;
    };
    m_fn_fixup               = resolve(kSymFixupRva, "_ZN4base17cRelocatableChunk5FixupERKNS_12sChunkHeaderEPv");
    m_fn_get_model           = resolve(kSymGetModelRva, "_ZN15lglWorldManager8getModelEyP17sBuildingGeometrybb");
    m_fn_destroy_model       = resolve(kSymDestroyModelRva, "_ZN15lglWorldManager7destroyEyb");
    m_fn_creator_remove      = resolve(kSymCreatorRemoveRva, "_ZN15lglWorldCreator6removeEy");
    m_fn_lgl_mutex_lock      = resolve(kSymLglMutexLockRva, "_ZN8lglMutex4lockEv");
    m_fn_lgl_mutex_unlock    = resolve(kSymLglMutexUnlockRva, "_ZN8lglMutex6unlockEv");
    m_fn_is_time_in_range    = resolve(kSymIsTimeInRangeRva, "_ZN6CClock16GetIsTimeInRangeEhh");
    m_impl->fn_is_lucid_created = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotIsLucidCreatedRva);
    if (!m_impl->fn_is_lucid_created) {
        m_impl->fn_is_lucid_created = resolve(0x563428ULL, "_Z19IsLucidWorldCreatedP9sDMAModel");
    }

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t cb_render_pre     = hle.register_function("__lcs_whd_render_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::render_pre(a[0], a[1]); });
    const uint64_t cb_render_post    = hle.register_function("__lcs_whd_render_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::render_post(a[0]); });
    const uint64_t cb_update_pre     = hle.register_function("__lcs_whd_update_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::update_pre(a[0]); });
    const uint64_t cb_update_post    = hle.register_function("__lcs_whd_update_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::update_post(a[0]); });
    const uint64_t cb_load_pre       = hle.register_function("__lcs_whd_load_level_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::load_level_pre(a[0], a[1]); });
    const uint64_t cb_load_post      = hle.register_function("__lcs_whd_load_level_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::load_level_post(a[0]); });
    const uint64_t cb_scene_pre      = hle.register_function("__lcs_whd_load_scene_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::load_scene_pre(a[0]); });
    const uint64_t cb_scene_post     = hle.register_function("__lcs_whd_load_scene_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::load_scene_post(a[0]); });
    const uint64_t cb_stream_call_pre = hle.register_function("__lcs_whd_stream_call_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::stream_call_pre(a[0], a[1]); });
    const uint64_t cb_stream_post    = hle.register_function("__lcs_whd_stream_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::stream_post(a[0]); });
    const uint64_t cb_poll_pre       = hle.register_function("__lcs_whd_poll_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::poll_streaming_pre(a[0]); });
    const uint64_t cb_poll_post      = hle.register_function("__lcs_whd_poll_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::poll_streaming_post(a[0]); });
    const uint64_t cb_cancel_pre     = hle.register_function("__lcs_whd_cancel_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::cancel_streaming_pre(a[0]); });
    const uint64_t cb_cancel_post    = hle.register_function("__lcs_whd_cancel_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::cancel_streaming_post(a[0]); });
    const uint64_t cb_stream_cb_pre  = hle.register_function("__lcs_whd_stream_cb_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::streaming_cb_pre(a[0]); });
    const uint64_t cb_stream_cb_post = hle.register_function("__lcs_whd_stream_cb_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::streaming_cb_post(a[0]); });
    const uint64_t cb_swap_in_pre    = hle.register_function("__lcs_whd_swap_in_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::swap_in_buf_pre(a[0]); });
    const uint64_t cb_swap_in_post   = hle.register_function("__lcs_whd_swap_in_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::swap_in_buf_post(a[0]); });
    const uint64_t cb_tex_lock       = hle.register_function("__lcs_whd_tex_lock",
        [](const uint64_t* a) -> uint64_t { return HookOps::tex_mgr_lock(a[0]); });
    const uint64_t cb_tex_unlock     = hle.register_function("__lcs_whd_tex_unlock",
        [](const uint64_t* a) -> uint64_t { return HookOps::tex_mgr_unlock(a[0]); });
    const uint64_t cb_rm_pre         = hle.register_function("__lcs_whd_remove_old_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::remove_old_pre(a[0]); });
    const uint64_t cb_rm_post        = hle.register_function("__lcs_whd_remove_old_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::remove_old_post(a[0]); });
    const uint64_t cb_swap_pre       = hle.register_function("__lcs_whd_swap_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::swap_pre(a[0]); });
    const uint64_t cb_swap_post      = hle.register_function("__lcs_whd_swap_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::swap_post(a[0]); });
    const uint64_t cb_destroy_pre    = hle.register_function("__lcs_whd_destroy_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::lgl_destroy_pre(a[0], a[1]); });

    uint8_t* base = m_impl->stub_page;
    const uint64_t base_va = reinterpret_cast<uint64_t>(base);
    size_t cursor = 0;

    // Build wrapper stub: save (x0, x1), call pre(x0, x1), restore (x0, x1), call orig(x0, x1), call post(saved_x0)
    auto emit_pre_orig_post = [&](uint64_t got_rva, const char* sym, uint64_t pre_va, uint64_t post_va,
                                  uint64_t& out_orig) -> uint64_t {
        const uint64_t stub_va = base_va + cursor;
        uint32_t* code = reinterpret_cast<uint32_t*>(base + cursor);
        out_orig = patch_got_entry(cpu, m_load_bias, got_rva, stub_va, image_game, sym);

        // Literal table placed at byte offset 0x60 from stub start
        const uint32_t insns[24] = {
            0xd100c3ffu, // 0x00: sub sp, sp, #0x30
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xa9025bf5u, // 0x10: stp x21, x22, [sp, #0x20]
            0xaa0003f3u, // 0x14: mov x19, x0
            0xaa0103f4u, // 0x18: mov x20, x1
            ldr_x16_rel(0x1c, 0x60),
            0xd63f0200u, // 0x20: blr x16 (pre_cb)
            0xaa1303e0u, // 0x24: mov x0, x19
            0xaa1403e1u, // 0x28: mov x1, x20
            ldr_x16_rel(0x2c, 0x68),
            0xd63f0200u, // 0x30: blr x16 (orig_fn)
            0xaa0003f5u, // 0x34: mov x21, x0 (save return val)
            0xaa1303e0u, // 0x38: mov x0, x19
            ldr_x16_rel(0x3c, 0x70),
            0xd63f0200u, // 0x40: blr x16 (post_cb)
            0xaa1503e0u, // 0x44: mov x0, x21
            0xa9425bf5u, // 0x48: ldp x21, x22, [sp, #0x20]
            0xa94153f3u, // 0x4c: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x50: ldp x29, x30, [sp]
            0x9100c3ffu, // 0x54: add sp, sp, #0x30
            0xd65f03c0u, // 0x58: ret
            0xd503201fu, // 0x5c: nop
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(base + cursor + 0x60ULL);
        lits[0] = pre_va;
        lits[1] = out_orig;
        lits[2] = post_va;
        cursor += 0x80ULL;
        return stub_va;
    };

    // Simple post-only stub: save (x0), call orig(x0, x1), save x0, call post(saved_x0), restore x0
    auto emit_post_only = [&](uint64_t got_rva, const char* sym, uint64_t post_va, uint64_t& out_orig) -> uint64_t {
        const uint64_t stub_va = base_va + cursor;
        uint32_t* code = reinterpret_cast<uint32_t*>(base + cursor);
        out_orig = patch_got_entry(cpu, m_load_bias, got_rva, stub_va, image_game, sym);

        const uint32_t insns[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xaa0003f3u, // 0x10: mov x19, x0
            ldr_x16_rel(0x14, 0x40),
            0xd63f0200u, // 0x18: blr x16 (orig_fn)
            0xaa0003f4u, // 0x1c: mov x20, x0
            0xaa1303e0u, // 0x20: mov x0, x19
            ldr_x16_rel(0x24, 0x48),
            0xd63f0200u, // 0x28: blr x16 (post_cb)
            0xaa1403e0u, // 0x2c: mov x0, x20
            0xa94153f3u, // 0x30: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x34: ldp x29, x30, [sp]
            0x910083ffu, // 0x38: add sp, sp, #0x20
            0xd65f03c0u, // 0x3c: ret
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(base + cursor + 0x40ULL);
        lits[0] = out_orig;
        lits[1] = post_va;
        cursor += 0x50ULL;
        return stub_va;
    };

    // Simple pre-only stub: save (x0, x1), call pre(x0, x1), restore (x0, x1), tail-branch orig
    auto emit_pre_only = [&](uint64_t got_rva, const char* sym, uint64_t pre_va, uint64_t& out_orig) -> uint64_t {
        const uint64_t stub_va = base_va + cursor;
        uint32_t* code = reinterpret_cast<uint32_t*>(base + cursor);
        out_orig = patch_got_entry(cpu, m_load_bias, got_rva, stub_va, image_game, sym);

        const uint32_t insns[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xaa0003f3u, // 0x10: mov x19, x0
            0xaa0103f4u, // 0x14: mov x20, x1
            ldr_x16_rel(0x18, 0x40),
            0xd63f0200u, // 0x1c: blr x16 (pre_cb)
            0xaa1303e0u, // 0x20: mov x0, x19
            0xaa1403e1u, // 0x24: mov x1, x20
            0xa94153f3u, // 0x28: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x2c: ldp x29, x30, [sp]
            0x910083ffu, // 0x30: add sp, sp, #0x20
            ldr_x16_rel(0x34, 0x48),
            0xd61f0200u, // 0x38: br x16 (orig_fn)
            0xd503201fu, // 0x3c: nop
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(base + cursor + 0x40ULL);
        lits[0] = pre_va;
        lits[1] = out_orig;
        cursor += 0x50ULL;
        return stub_va;
    };

    // Full argument-preserving wrapper (preserves x0..x7 across pre, orig, post and return x0)
    auto emit_wrap_args = [&](uint64_t got_rva, const char* sym, uint64_t pre_va, uint64_t post_va,
                              uint64_t& out_orig) -> uint64_t {
        const uint64_t stub_va = base_va + cursor;
        uint32_t* code = reinterpret_cast<uint32_t*>(base + cursor);
        out_orig = patch_got_entry(cpu, m_load_bias, got_rva, stub_va, image_game, sym);

        const uint32_t insns[24] = {
            0xd10183ffu, // 0x00: sub sp, sp, #0x60
            0xa9057bfdu, // 0x04: stp x29, x30, [sp, #0x50]
            0xa90007e0u, // 0x08: stp x0,  x1,  [sp, #0x00]
            0xa9010fe2u, // 0x0c: stp x2,  x3,  [sp, #0x10]
            0xa90217e4u, // 0x10: stp x4,  x5,  [sp, #0x20]
            0xa9031fe6u, // 0x14: stp x6,  x7,  [sp, #0x30]
            ldr_x16_rel(0x18, 0x60),
            0xd63f0200u, // 0x1c: blr x16 (pre_va)
            0xa9431fe6u, // 0x20: ldp x6,  x7,  [sp, #0x30]
            0xa94217e4u, // 0x24: ldp x4,  x5,  [sp, #0x20]
            0xa9410fe2u, // 0x28: ldp x2,  x3,  [sp, #0x10]
            0xa94007e0u, // 0x2c: ldp x0,  x1,  [sp, #0x00]
            ldr_x16_rel(0x30, 0x68),
            0xd63f0200u, // 0x34: blr x16 (orig_fn)
            0xf90023e0u, // 0x38: str x0,  [sp, #0x40]
            0xf94003e0u, // 0x3c: ldr x0,  [sp, #0x00]
            0xf94023e1u, // 0x40: ldr x1,  [sp, #0x40]
            ldr_x16_rel(0x44, 0x70),
            0xd63f0200u, // 0x48: blr x16 (post_va)
            0xf94023e0u, // 0x4c: ldr x0,  [sp, #0x40]
            0xa9457bfdu, // 0x50: ldp x29, x30, [sp, #0x50]
            0x910183ffu, // 0x54: add sp, sp, #0x60
            0xd65f03c0u, // 0x58: ret
            0xd503201fu, // 0x5c: nop
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(base + cursor + 0x60ULL);
        lits[0] = pre_va;
        lits[1] = out_orig;
        lits[2] = post_va;
        cursor += 0x80ULL;
        return stub_va;
    };

    emit_pre_orig_post(kGotWsRenderRva, "_ZN12cWorldStream6RenderEi",
                       cb_render_pre, cb_render_post, m_orig_render);
    emit_wrap_args(kGotWsUpdateRva, "_ZN12cWorldStream6UpdateEv",
                   cb_update_pre, cb_update_post, m_orig_update);
    emit_wrap_args(kGotWsLoadLevelRva, "_ZN12cWorldStream9LoadLevelE10eLevelName",
                   cb_load_pre, cb_load_post, m_orig_load_level);
    emit_wrap_args(kGotWsLoadSceneRva, "_ZN12cWorldStream9LoadSceneERK7CVector",
                   cb_scene_pre, cb_scene_post, m_impl->orig_load_scene);
    emit_wrap_args(kGotWsStreamRva, "_ZN12cWorldStream6StreamEPN4base12sChunkHeaderE",
                   cb_stream_call_pre, cb_stream_post, m_impl->orig_stream);
    emit_wrap_args(kGotWsPollStreamingRva, "_ZN12cWorldStream13PollStreamingEb",
                   cb_poll_pre, cb_poll_post, m_impl->orig_poll_streaming);
    emit_wrap_args(kGotWsCancelStreamRva, "_ZN12cWorldStream15CancelStreamingEb",
                   cb_cancel_pre, cb_cancel_post, m_impl->orig_cancel_streaming);
    emit_wrap_args(kGotWsStreamingCbRva, "_ZN12cWorldStream17StreamingCallbackEP11cUmdRequest",
                   cb_stream_cb_pre, cb_stream_cb_post, m_impl->orig_streaming_cb);
    emit_wrap_args(kGotWsSwapInBufRva, "_ZN12cWorldStream21SwapInStreamingBufferEv",
                   cb_swap_in_pre, cb_swap_in_post, m_impl->orig_swap_in_buf);
    emit_wrap_args(kGotLglTexGetRva, "_ZN17lglTextureManager10getTextureEPKcS1_S1_bb",
                   cb_tex_lock, cb_tex_unlock, m_impl->orig_tex_get);
    emit_wrap_args(kGotLglTexDestroyRva, "_ZN17lglTextureManager14destroyTextureEPN7Display9C_TextureE",
                   cb_tex_lock, cb_tex_unlock, m_impl->orig_tex_destroy);
    emit_wrap_args(kGotDispReleaseTexRva, "_ZN7Display14ReleaseTextureERPNS_9C_TextureE",
                   cb_tex_lock, cb_tex_unlock, m_impl->orig_disp_release_tex);
    emit_pre_orig_post(kGotWsRemoveOldRva, "_ZN12cWorldStream21RemoveOldSectorModelsEv",
                       cb_rm_pre, cb_rm_post, m_orig_remove_old_models);
    emit_pre_orig_post(kGotWsSwapSectorRva, "_ZN12cWorldStream20SwapToStreamedSectorEv",
                       cb_swap_pre, cb_swap_post, m_impl->orig_swap_sector);

    LcsBatchedFast::instance().initialize(image_game, cpu, m_load_bias);

    // Guard lglWorldManager::destroy(bool destroyAll): skip destroy(false) when WorldHD active
    {
        const uint64_t stub_va = base_va + cursor;
        uint32_t* code = reinterpret_cast<uint32_t*>(base + cursor);
        m_impl->orig_lgl_destroy = patch_got_entry(
            cpu, m_load_bias, kGotLglDestroyRva, stub_va, image_game,
            "_ZN15lglWorldManager7destroyEb");
        const uint32_t insns[20] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xaa0003f3u, // 0x10: mov x19, x0
            0xaa0103f4u, // 0x14: mov x20, x1
            ldr_x16_rel(0x18, 0x50),
            0xd63f0200u, // 0x1c: blr x16 (cb_destroy_pre)
            0xb5000100u, // 0x20: cbnz x0, +0x20 -> 0x40 (skip)
            0xaa1303e0u, // 0x24: mov x0, x19
            0xaa1403e1u, // 0x28: mov x1, x20
            0xa94153f3u, // 0x2c: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x30: ldp x29, x30, [sp]
            0x910083ffu, // 0x34: add sp, sp, #0x20
            ldr_x16_rel(0x38, 0x58),
            0xd61f0200u, // 0x3c: br x16 (orig_lgl_destroy)
            0xa94153f3u, // 0x40: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x44: ldp x29, x30, [sp]
            0x910083ffu, // 0x48: add sp, sp, #0x20
            0xd65f03c0u, // 0x4c: ret
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(base + cursor + 0x50ULL);
        lits[0] = cb_destroy_pre;
        lits[1] = m_impl->orig_lgl_destroy;
        cursor += 0x60ULL;
    }

    cpu.invalidate_cache(base_va, 0x4000);
    return true;
}

bool LcsWorldHd::ensure_level_img_open(int level) {
    if (!m_impl) return false;
    if (m_impl->img_file && m_impl->img_level == level) return true;

    close_level_img();
    const char* rel = level_img_name(level);
    if (!rel) return false;

    char resolved_buf[512]{};
    const char* path = lcs::vfs::LogicalFS::instance().resolve_bundle_path(rel, resolved_buf, sizeof(resolved_buf));
    if (!path) return false;

    FILE* fp = std::fopen(path, "rb");
    if (!fp) {
        std::string fallback = std::string("assets/ipad/") + rel;
        fp = std::fopen(fallback.c_str(), "rb");
        if (!fp) return false;
    }

    m_impl->img_file = fp;
    m_impl->img_level = level;
    return true;
}

void LcsWorldHd::close_level_img() {
    if (m_impl && m_impl->img_file) {
        std::fclose(m_impl->img_file);
        m_impl->img_file = nullptr;
    }
    if (m_impl) m_impl->img_level = 0;
}

void LcsWorldHd::flush_all_cells(uint64_t ws_ptr, bool destroy_lgl) {
    if (!m_impl) return;
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    auto* cpu = active_cpu(m_cpu);

    uint64_t dir_ptr = 0;
    sBuildingDirEntry* table = nullptr;
    int32_t table_count = 0;
    if (ws_ptr) {
        dir_ptr = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsDirOff);
        if (dir_ptr) {
            table = reinterpret_cast<sBuildingDirEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr));
            table_count = *reinterpret_cast<const int32_t*>(dir_ptr + 0x2f8ULL);
        }
    }

    const uint64_t mgr = m_got_world_mgr ? *reinterpret_cast<const uint64_t*>(m_got_world_mgr) : 0;
    const uint64_t mgr_inst = mgr ? *reinterpret_cast<const uint64_t*>(mgr) : 0;

    for (auto& [key, cell] : m_impl->cells) {
        if (table) {
            const uint64_t p_lo = reinterpret_cast<uint64_t>(cell.payload);
            const uint64_t p_hi = p_lo + cell.payload_size;
            for (uint32_t gid : cell.pair_gids) {
                if (gid < static_cast<uint32_t>(table_count)) {
                    const uint64_t gp = table[gid].geomPtr;
                    if (gp >= p_lo && gp < p_hi) {
                        table[gid].geomPtr = 0;
                        if (destroy_lgl && mgr_inst && m_fn_destroy_model && cpu) {
                            const uint64_t model_key =
                                (static_cast<uint64_t>(static_cast<uint32_t>(cell.level)) << 32) | gid;
                            cpu->call_nested(m_fn_destroy_model, {mgr_inst, model_key, 0});
                        }
                    }
                }
            }
        }
        m_impl->free_block(cell.arena_offset);
    }

    m_impl->cells.clear();
    if (table) {
        const uint64_t arena_lo = reinterpret_cast<uint64_t>(m_impl->arena_base);
        const uint64_t arena_hi = arena_lo + kCellPoolBytes;
        for (int32_t g = 0; g < table_count; ++g) {
            if (table[g].geomPtr >= arena_lo && table[g].geomPtr < arena_hi) {
                table[g].geomPtr = 0;
            }
        }
    }
    m_impl->cell_count.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
        m_impl->covered_cells.clear();
        m_impl->sync_covered_fast();
    }
    m_combined_inst_count = 0;
    m_combined_dirty = true;
    m_impl->first_scene_pending = true;
}

void LcsWorldHd::on_world_load_level_pre(uint64_t ws_ptr, int new_level) {
    if (!m_impl) return;
    m_impl->ws_mtx.lock();
    on_stream_pre(ws_ptr);
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    const int cur_level = ws_ptr ? *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurrLevelOff) : m_active_level;
    if (cur_level != new_level) {
        // Drain lglWorldCreator via remove(key) rather than destroyAll(): destroyAll() forgets
        // ResetLucidWorld(dma) so uncreated sDMAModels keep refCount==1 and stall lglWorldDestroyer.
        // Acquiring lglMutex here also waits for any active RenderThread CreateLucidWorld() while
        // ws->m_dir and arena_base remain valid.
        if (auto* cpu = active_cpu(m_cpu);
            cpu && m_got_world_creator && m_fn_creator_remove && m_fn_lgl_mutex_lock && m_fn_lgl_mutex_unlock) {
            const uint64_t wc = *reinterpret_cast<const uint64_t*>(m_got_world_creator);
            const uint64_t wc_inst = wc ? *reinterpret_cast<const uint64_t*>(wc) : 0;
            if (wc_inst) {
                std::vector<uint64_t> pending_keys;
                cpu->call_nested(m_fn_lgl_mutex_lock, {wc_inst});
                const uint64_t cnt = *reinterpret_cast<const uint64_t*>(wc_inst + 0x18ULL);
                const uint64_t root = *reinterpret_cast<const uint64_t*>(wc_inst + 0x10ULL);
                if (cnt > 0 && cnt < 65536 && root) {
                    std::vector<uint64_t> stk;
                    stk.reserve(64);
                    stk.push_back(root);
                    while (!stk.empty() && pending_keys.size() < cnt) {
                        const uint64_t node = stk.back();
                        stk.pop_back();
                        if (!node) continue;
                        pending_keys.push_back(*reinterpret_cast<const uint64_t*>(node + 0x20ULL));
                        const uint64_t l = *reinterpret_cast<const uint64_t*>(node + 0x00ULL);
                        const uint64_t r = *reinterpret_cast<const uint64_t*>(node + 0x08ULL);
                        if (r) stk.push_back(r);
                        if (l) stk.push_back(l);
                    }
                }
                cpu->call_nested(m_fn_lgl_mutex_unlock, {wc_inst});
                for (uint64_t k : pending_keys) {
                    cpu->call_nested(m_fn_creator_remove, {wc_inst, k});
                }
            }
        }
        m_impl->allow_lgl_destroy = true;
        flush_all_cells(ws_ptr, true);
        close_level_img();
        m_active_level = new_level;
    }
}

void LcsWorldHd::on_world_load_level_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        m_impl->allow_lgl_destroy = false;
    }
    on_stream_post(ws_ptr);
    m_impl->ws_mtx.unlock();
}

void LcsWorldHd::on_world_load_scene_pre(uint64_t ws_ptr) {
    if (!m_impl) return;
    m_impl->ws_mtx.lock();
    on_stream_pre(ws_ptr);
}

void LcsWorldHd::on_world_load_scene_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    on_stream_post(ws_ptr);
    m_impl->ws_mtx.unlock();
}

namespace {
thread_local std::vector<int> tl_saved_gtt_depths;
} // namespace

void LcsWorldHd::on_stream_pre(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl) return;
    const auto self = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(m_impl->stream_mtx);
    if (m_impl->gtt_stream_depth == 0) {
        m_impl->stream_cv.wait(lk, [&] {
            return (m_impl->stream_cb_depth == 0 || m_impl->stream_cb_owner == self) &&
                   (m_impl->gtt_stream_depth == 0 || m_impl->gtt_stream_owner == self);
        });
        m_impl->gtt_stream_owner = self;
    } else if (m_impl->gtt_stream_owner != self) {
        m_impl->stream_cv.wait(lk, [&] {
            return (m_impl->stream_cb_depth == 0 || m_impl->stream_cb_owner == self) &&
                   m_impl->gtt_stream_depth == 0;
        });
        m_impl->gtt_stream_owner = self;
    }
    ++m_impl->gtt_stream_depth;
}

void LcsWorldHd::on_stream_post(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->stream_mtx);
    if (m_impl->gtt_stream_depth > 0) {
        --m_impl->gtt_stream_depth;
        if (m_impl->gtt_stream_depth == 0) {
            m_impl->gtt_stream_owner = std::thread::id{};
            m_impl->stream_cv.notify_all();
        }
    }
}

void LcsWorldHd::on_poll_streaming_pre(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl) return;
    const auto self = std::this_thread::get_id();
    std::lock_guard<std::mutex> lk(m_impl->stream_mtx);
    if (m_impl->gtt_stream_depth > 0 && m_impl->gtt_stream_owner == self) {
        tl_saved_gtt_depths.push_back(m_impl->gtt_stream_depth);
        m_impl->gtt_stream_depth = 0;
        m_impl->gtt_stream_owner = std::thread::id{};
        m_impl->stream_cv.notify_all();
    } else {
        tl_saved_gtt_depths.push_back(0);
    }
}

void LcsWorldHd::on_poll_streaming_post(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl || tl_saved_gtt_depths.empty()) return;
    const int restore = tl_saved_gtt_depths.back();
    tl_saved_gtt_depths.pop_back();
    const auto self = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(m_impl->stream_mtx);
    m_impl->stream_cv.wait(lk, [&] {
        return (m_impl->stream_cb_depth == 0 || m_impl->stream_cb_owner == self) &&
               (restore == 0 || m_impl->gtt_stream_depth == 0 || m_impl->gtt_stream_owner == self);
    });
    if (restore > 0) {
        m_impl->gtt_stream_depth = restore;
        m_impl->gtt_stream_owner = self;
    }
}

void LcsWorldHd::on_cancel_streaming_pre(uint64_t ws_ptr) {
    on_stream_pre(ws_ptr);
}

void LcsWorldHd::on_cancel_streaming_post(uint64_t ws_ptr) {
    on_stream_post(ws_ptr);
}

void LcsWorldHd::on_streaming_cb_pre(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl) return;
    const auto self = std::this_thread::get_id();
    std::unique_lock<std::mutex> lk(m_impl->stream_mtx);
    if (m_impl->stream_cb_depth == 0) {
        m_impl->stream_cv.wait(lk, [&] {
            return (m_impl->gtt_stream_depth == 0 || m_impl->gtt_stream_owner == self) &&
                   (m_impl->stream_cb_depth == 0 || m_impl->stream_cb_owner == self);
        });
        m_impl->stream_cb_owner = self;
    } else if (m_impl->stream_cb_owner != self) {
        m_impl->stream_cv.wait(lk, [&] {
            return (m_impl->gtt_stream_depth == 0 || m_impl->gtt_stream_owner == self) &&
                   m_impl->stream_cb_depth == 0;
        });
        m_impl->stream_cb_owner = self;
    }
    ++m_impl->stream_cb_depth;
}

void LcsWorldHd::on_streaming_cb_post(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->stream_mtx);
    if (m_impl->stream_cb_depth > 0) {
        --m_impl->stream_cb_depth;
        if (m_impl->stream_cb_depth == 0) {
            m_impl->stream_cb_owner = std::thread::id{};
            m_impl->stream_cv.notify_all();
        }
    }
}

void LcsWorldHd::on_tex_mgr_lock() {
    if (m_impl) m_impl->tex_mgr_mtx.lock();
}

void LcsWorldHd::on_tex_mgr_unlock() {
    if (m_impl) m_impl->tex_mgr_mtx.unlock();
}

void LcsWorldHd::sanitize_stream_table(uint64_t ws_ptr, bool scrub_prev, bool scrub_stream) {
    if (!m_impl || !ws_ptr || (!scrub_prev && !scrub_stream)) return;
    const uint64_t dir_ptr = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsDirOff);
    if (!dir_ptr) return;

    auto* table = reinterpret_cast<sBuildingDirEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr));
    const int32_t table_count = *reinterpret_cast<const int32_t*>(dir_ptr + 0x2f8ULL);
    if (!table || table_count <= 0) return;

    const uint64_t ws_arena = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsArenaPtrOff);
    const uint32_t ws_arena_sz = *reinterpret_cast<const uint32_t*>(ws_ptr + kWsArenaSizeOff);
    if (!ws_arena || ws_arena_sz == 0) return;
    const uint64_t ws_arena_end = ws_arena + ws_arena_sz;

    const uint64_t b_cur    = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBaseOff);
    const uint64_t b_prev   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsPrevBaseOff);
    const uint64_t b_stream = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsStreamBaseOff);

    std::array<uint64_t, 4> bounds{};
    size_t nb = 0;
    auto push_b = [&](uint64_t p) {
        if (p >= ws_arena && p < ws_arena_end) bounds[nb++] = p;
    };
    push_b(b_cur);
    push_b(b_prev);
    push_b(b_stream);
    if (nb == 0) return;
    std::sort(bounds.begin(), bounds.begin() + nb);
    nb = static_cast<size_t>(std::unique(bounds.begin(), bounds.begin() + nb) - bounds.begin());
    bounds[nb] = ws_arena_end;

    auto span_for = [&](uint64_t b) -> std::pair<uint64_t, uint64_t> {
        for (size_t i = 0; i < nb; ++i) {
            if (bounds[i] == b) return {b, bounds[i + 1]};
        }
        return {0ULL, 0ULL};
    };

    const auto prev_span   = scrub_prev   ? span_for(b_prev)   : std::pair<uint64_t, uint64_t>{0ULL, 0ULL};
    const auto stream_span = scrub_stream ? span_for(b_stream) : std::pair<uint64_t, uint64_t>{0ULL, 0ULL};
    if (prev_span.first == 0ULL && stream_span.first == 0ULL) return;

    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    const int32_t level = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurrLevelOff);

    bool map_built = false;
    std::unordered_map<uint32_t, uint64_t> arena_repl;
    auto build_repl = [&]() {
        if (map_built) return;
        map_built = true;
        for (const auto& [key, cell] : m_impl->cells) {
            (void)key;
            if (cell.level != level || !cell.payload) continue;
            const uint64_t pairs_ptr = *reinterpret_cast<const uint64_t*>(cell.payload);
            const uint16_t npairs = *reinterpret_cast<const uint16_t*>(cell.payload + 8);
            if (!pairs_ptr || npairs == 0) continue;
            const auto* pairs = reinterpret_cast<const sPairEntry*>(pairs_ptr);
            for (uint16_t pi = 0; pi < npairs; ++pi) {
                if (pairs[pi].geomPtr != 0) {
                    arena_repl.emplace(pairs[pi].gid, pairs[pi].geomPtr);
                }
            }
        }
    };

    auto in_span = [](uint64_t p, const std::pair<uint64_t, uint64_t>& s) -> bool {
        return s.first != 0ULL && p >= s.first && p < s.second;
    };

    bool any_cleared = false;
    for (int32_t g = 0; g < table_count; ++g) {
        const uint64_t gp = table[g].geomPtr;
        if (!gp) continue;
        if (!in_span(gp, prev_span) && !in_span(gp, stream_span)) continue;
        build_repl();
        auto it = arena_repl.find(static_cast<uint32_t>(g));
        if (it != arena_repl.end()) {
            table[g].geomPtr = it->second;
        } else {
            table[g].geomPtr = 0;
            any_cleared = true;
        }
    }
    if (any_cleared) {
        m_combined_dirty = true;
    }
}

void LcsWorldHd::on_stream_call_pre(uint64_t ws_ptr, uint64_t hdr_ptr) {
    if (!m_impl) return;
    // Drain active request via PollStreaming(true) before Stream() acquires cUmdMutex (0x39c574),
    // preventing an AB-BA deadlock against StreamingCallback (0x3a1280) locking cUmdMutex on cUmdThread.
    if (ws_ptr && hdr_ptr && *reinterpret_cast<const uint8_t*>(ws_ptr + 0x1efULL) == 0) {
        auto* cpu = active_cpu(m_cpu);
        const auto self = std::this_thread::get_id();
        bool is_cb_thread = false;
        {
            std::lock_guard<std::mutex> lk(m_impl->stream_mtx);
            is_cb_thread = (m_impl->stream_cb_depth > 0 && m_impl->stream_cb_owner == self);
        }
        if (cpu && m_impl->orig_poll_streaming && !is_cb_thread) {
            int guard = 0;
            while (*reinterpret_cast<const uint64_t*>(ws_ptr + 0x218ULL) != 0 && guard++ < 4) {
                on_poll_streaming_pre(ws_ptr);
                cpu->call_nested(m_impl->orig_poll_streaming, {ws_ptr, 1ULL});
                on_poll_streaming_post(ws_ptr);
            }
        }
    }
    on_stream_pre(ws_ptr);
    if (m_initialized && ws_ptr && hdr_ptr) {
        sanitize_stream_table(ws_ptr, /*scrub_prev=*/false, /*scrub_stream=*/true);
    }
}

void LcsWorldHd::on_swap_in_buf_pre(uint64_t ws_ptr) {
    if (!m_impl) return;
    on_stream_pre(ws_ptr);
    if (m_initialized && ws_ptr) {
        sanitize_stream_table(ws_ptr, /*scrub_prev=*/true, /*scrub_stream=*/true);
    }
}

void LcsWorldHd::on_swap_in_buf_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    if (m_initialized && ws_ptr && m_radius > 0) {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        m_combined_dirty = true;
    }
    on_stream_post(ws_ptr);
}

void LcsWorldHd::on_world_update_pre(uint64_t ws_ptr) {
    if (!m_impl) return;
    m_impl->ws_mtx.lock();
    on_stream_pre(ws_ptr);
}

bool LcsWorldHd::area_allows_instance(uint64_t ws_ptr, uint64_t dir_ptr, uint16_t inst_id) const {
    if (!dir_ptr) return true;
    const int32_t num_areas = *reinterpret_cast<const int32_t*>(dir_ptr + 0x47cULL);
    if (num_areas <= 0) return true;
    const auto* areas = reinterpret_cast<const sAreaEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr + 0x480ULL));
    if (!areas) return true;

    const uint16_t clean_id = inst_id & 0x7fffu;
    const auto* area_states = reinterpret_cast<const uint32_t*>(ws_ptr + kWsAreaStatesOff);

    for (int32_t i = 0; i < num_areas; ++i) {
        if (areas[i].inst_id != clean_id) continue;
        const uint8_t flag = areas[i].flag;
        if (flag == 0xffu) return false;
        if (flag & 0x80u) {
            if (auto* cpu = active_cpu(m_cpu); m_fn_is_time_in_range && cpu) {
                const uint64_t vis = cpu->call_nested(
                    m_fn_is_time_in_range, {static_cast<uint64_t>(areas[i].val), static_cast<uint64_t>(flag & 0x7fu)});
                if ((vis & 1ULL) == 0) return false;
            }
        } else {
            if (area_states[flag] != static_cast<uint32_t>(areas[i].val)) {
                return false;
            }
        }
    }
    return true;
}

void LcsWorldHd::step_streaming(uint64_t ws_ptr, int max_loads) {
    if (!m_initialized || m_radius <= 0 || !m_impl || !ws_ptr) return;
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    auto* cpu = active_cpu(m_cpu);
    if (!cpu) return;

    const uint64_t dir_ptr = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsDirOff);
    if (!dir_ptr) return;

    const int32_t level = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurrLevelOff);
    if (level <= 0 || level > 4) return;
    if (m_active_level != level) {
        flush_all_cells(ws_ptr, false);
        m_active_level = level;
    }

    if (!ensure_level_img_open(level)) return;

    float cam_x = 0.0f, cam_y = 0.0f;
    if (m_got_camera) {
        const uint64_t cam_ptr = *reinterpret_cast<const uint64_t*>(m_got_camera);
        if (cam_ptr) {
            cam_x = *reinterpret_cast<const float*>(cam_ptr + 0x30ULL);
            cam_y = *reinterpret_cast<const float*>(cam_ptr + 0x34ULL);
        }
    }
    m_impl->last_cam_x = cam_x;
    m_impl->last_cam_y = cam_y;

    int32_t cam_cx = 0, cam_cy = 0;
    world_to_hex_cell(cam_x, cam_y, cam_cx, cam_cy);
    if (cam_cx != m_center_cx || cam_cy != m_center_cy) {
        m_center_cx = cam_cx;
        m_center_cy = cam_cy;
        m_combined_dirty = true;
    }

    const uint64_t cur_begin = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBeginOff);
    const uint64_t cur_end   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurEndOff);
    const uint64_t nat_chunk = (cur_begin && cur_end > cur_begin)
        ? *reinterpret_cast<const uint64_t*>(cur_begin) : 0;
    if (nat_chunk != m_impl->last_native_chunk) {
        m_impl->last_native_chunk = nat_chunk;
        m_combined_dirty = true;
        m_reregister_needed = true;
    }

    const int r = m_radius;
    const float max_dist_sq = cell_preload_dist_sq(r);
    const auto* rows = reinterpret_cast<const sDirRowEntry*>(dir_ptr + 0x08ULL);

    struct Candidate {
        int32_t cx;
        int32_t cy;
        float dist_sq;
        const sChunkHeader* hdr;
    };
    std::vector<Candidate> wanted;
    wanted.reserve(64);

    const int32_t cy_lo = std::max<int32_t>(0, cam_cy - r - 1);
    const int32_t cy_hi = std::min<int32_t>(44, cam_cy + r + 1);
    for (int32_t cy = cy_lo; cy <= cy_hi; ++cy) {
        const sDirRowEntry& row = rows[cy];
        const sDirRowEntry& next_row = rows[cy + 1];
        if (!row.chunks_ptr || next_row.chunks_ptr <= row.chunks_ptr) continue;
        const int32_t count = static_cast<int32_t>((next_row.chunks_ptr - row.chunks_ptr) / sizeof(sChunkHeader));
        const int32_t cx_lo = std::max<int32_t>(row.min_x, cam_cx - r - 1);
        const int32_t cx_hi = std::min<int32_t>(row.min_x + count - 1, cam_cx + r + 1);
        const auto* hdrs = reinterpret_cast<const sChunkHeader*>(row.chunks_ptr);

        for (int32_t cx = cx_lo; cx <= cx_hi; ++cx) {
            const sChunkHeader* hdr = &hdrs[cx - row.min_x];
            if (hdr->magic != kDlrwMagic || hdr->totalSize <= sizeof(sChunkHeader)) continue;
            float hx = 0.0f, hy = 0.0f;
            hex_cell_center(cx, cy, hx, hy);
            const float dx = hx - cam_x;
            const float dy = hy - cam_y;
            const float d2 = dx * dx + dy * dy;
            if (d2 <= max_dist_sq) {
                wanted.push_back({cx, cy, d2, hdr});
            }
        }
    }

    std::sort(wanted.begin(), wanted.end(), [](const Candidate& a, const Candidate& b) {
        return a.dist_sq < b.dist_sq;
    });

    const int32_t cur_cx = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurCxOff);
    const int32_t cur_cy = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurCyOff);

    // Update wanted timestamps & compute which cells need loading
    std::unordered_set<uint64_t> wanted_keys;
    wanted_keys.reserve(wanted.size());
    std::vector<Candidate> to_load;
    for (const auto& c : wanted) {
        const uint64_t key = pack_cell_key(c.cx, c.cy);
        wanted_keys.insert(key);
        auto it = m_impl->cells.find(key);
        if (it != m_impl->cells.end()) {
            it->second.last_used_frame = m_frame_index;
        } else {
            to_load.push_back(c);
        }
    }

    // Evict distant LRU cells when exceeding capacity or far beyond radius + 1 hysteresis
    // Defer eviction while Render() is reading cell payloads on another thread.
    const float evict_d = (r >= 3) ? 620.0f : (static_cast<float>(r + 1) * 115.0f + 90.0f);
    const float evict_dist_sq = evict_d * evict_d;
    if (m_impl->render_depth.load(std::memory_order_acquire) == 0 &&
        (m_impl->cells.size() > static_cast<size_t>(kMaxCachedCells) || !to_load.empty())) {
        std::vector<uint64_t> evict_list;
        std::unordered_set<uint64_t> evict_set;
        for (const auto& [key, cell] : m_impl->cells) {
            if (wanted_keys.count(key)) continue;
            if (cell.cx == cur_cx && cell.cy == cur_cy) continue;
            float hx = 0.0f, hy = 0.0f;
            hex_cell_center(cell.cx, cell.cy, hx, hy);
            const float dx = hx - cam_x;
            const float dy = hy - cam_y;
            if (dx * dx + dy * dy > evict_dist_sq || m_impl->cells.size() > static_cast<size_t>(kMaxCachedCells)) {
                evict_list.push_back(key);
            }
        }
        std::sort(evict_list.begin(), evict_list.end(), [&](uint64_t a, uint64_t b) {
            return m_impl->cells[a].last_used_frame < m_impl->cells[b].last_used_frame;
        });

        // Respect the kMaxCachedCells - 8 early-stop threshold before building replacement index.
        std::vector<uint64_t> actual_evicts;
        actual_evicts.reserve(evict_list.size());
        size_t projected_sz = m_impl->cells.size();
        for (uint64_t key : evict_list) {
            if (projected_sz <= static_cast<size_t>(kMaxCachedCells - 8)) {
                float hx = 0.0f, hy = 0.0f;
                hex_cell_center(m_impl->cells[key].cx, m_impl->cells[key].cy, hx, hy);
                const float dx = hx - cam_x;
                const float dy = hy - cam_y;
                if (dx * dx + dy * dy <= evict_dist_sq) break;
            }
            actual_evicts.push_back(key);
            evict_set.insert(key);
            --projected_sz;
        }

        if (!actual_evicts.empty()) {
            auto* table = reinterpret_cast<sBuildingDirEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr));
            const int32_t table_count = *reinterpret_cast<const int32_t*>(dir_ptr + 0x2f8ULL);
            const uint64_t mgr = m_got_world_mgr ? *reinterpret_cast<const uint64_t*>(m_got_world_mgr) : 0;
            const uint64_t mgr_inst = mgr ? *reinterpret_cast<const uint64_t*>(mgr) : 0;
            const uint64_t wc = m_got_world_creator ? *reinterpret_cast<const uint64_t*>(m_got_world_creator) : 0;
            const uint64_t wc_inst = wc ? *reinterpret_cast<const uint64_t*>(wc) : 0;

            // Build single O(cells * pairs) replacement lookup across surviving cells of this level.
            std::unordered_map<uint32_t, std::pair<uint64_t, Impl::CachedCell*>> surviving_gid_repl;
            surviving_gid_repl.reserve(2048);
            for (auto& [okey, ocell] : m_impl->cells) {
                if (evict_set.count(okey) || ocell.level != level || !ocell.payload) continue;
                const uint64_t pairs_ptr = *reinterpret_cast<const uint64_t*>(ocell.payload);
                const uint16_t npairs = *reinterpret_cast<const uint16_t*>(ocell.payload + 8);
                if (!pairs_ptr || npairs == 0) continue;
                const auto* pairs = reinterpret_cast<const sPairEntry*>(pairs_ptr);
                for (uint16_t pi = 0; pi < npairs; ++pi) {
                    if (pairs[pi].geomPtr) {
                        surviving_gid_repl.try_emplace(pairs[pi].gid, pairs[pi].geomPtr, &ocell);
                    }
                }
            }

            struct EvictRange {
                uint64_t p_lo;
                uint64_t p_hi;
                int32_t  level;
                bool     gpu_ready;
            };
            std::vector<EvictRange> evict_ranges;
            evict_ranges.reserve(actual_evicts.size());
            for (uint64_t key : actual_evicts) {
                const auto& c = m_impl->cells[key];
                const uint64_t p_lo = reinterpret_cast<uint64_t>(c.payload);
                evict_ranges.push_back({p_lo, p_lo + c.payload_size, c.level, c.gpu_ready});
            }

            // Single scan over directory table checks all evicted cells in one pass.
            double ev_dest_ms = 0.0, ev_scan_ms = 0.0;
            const auto ev_t0 = std::chrono::steady_clock::now();
            auto release_gid_if_evicted = [&](uint32_t gid) {
                if (gid >= static_cast<uint32_t>(table_count)) return;
                const uint64_t gp = table[gid].geomPtr;
                if (!gp) return;
                const EvictRange* matched = nullptr;
                for (const auto& er : evict_ranges) {
                    if (gp >= er.p_lo && gp < er.p_hi) {
                        matched = &er;
                        break;
                    }
                }
                if (!matched) return;

                auto rit = surviving_gid_repl.find(gid);
                const uint64_t replacement = (rit != surviving_gid_repl.end()) ? rit->second.first : 0ULL;
                Impl::CachedCell* repl_cell = (rit != surviving_gid_repl.end()) ? rit->second.second : nullptr;
                table[gid].geomPtr = replacement;
                if (!replacement || !matched->gpu_ready) {
                    const uint64_t model_key =
                        (static_cast<uint64_t>(static_cast<uint32_t>(matched->level)) << 32) | gid;
                    if (mgr_inst && m_fn_destroy_model) {
                        cpu->call_nested(m_fn_destroy_model, {mgr_inst, model_key, 0});
                    }
                    if (wc_inst && m_fn_creator_remove) {
                        cpu->call_nested(m_fn_creator_remove, {wc_inst, model_key});
                    }
                    if (repl_cell) {
                        repl_cell->models_registered = false;
                        repl_cell->gpu_ready = false;
                        m_reregister_needed = true;
                    }
                }
            };
            for (int32_t g = 0; g < table_count; ++g) {
                release_gid_if_evicted(static_cast<uint32_t>(g));
            }
            ev_dest_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ev_t0).count();
            if (std::getenv("LCS_DIAG_AUDIT") != nullptr && ev_dest_ms > 5.0) {
                std::cout << "  [TMP_WHD_EVICT] cells=" << actual_evicts.size() << " ms=" << ev_dest_ms << "\n" << std::flush;
            }

            for (uint64_t key : actual_evicts) {
                m_impl->free_block(m_impl->cells[key].arena_offset);
                m_impl->cells.erase(key);
                ++m_impl->stats_cells_evicted;
            }
            m_impl->cell_count.store(static_cast<int>(m_impl->cells.size()), std::memory_order_relaxed);
            m_combined_dirty = true;
        }
    }

    // Stream missing wanted cells (higher budget on initial spawn / teleport)
    const int budget = m_impl->first_scene_pending ? 160 : max_loads;
    m_impl->first_scene_pending = false;
    int loaded_now = 0;
    for (const auto& c : to_load) {
        if (loaded_now >= budget) break;
        const size_t payload_size = c.hdr->totalSize - sizeof(sChunkHeader);
        size_t aoff = 0, asz = 0;
        if (!m_impl->alloc_block(payload_size, aoff, asz)) {
            break;
        }

        uint8_t* dst = m_impl->arena_base + aoff;
        if (std::fseek(m_impl->img_file, static_cast<long>(c.hdr->offsetInImg), SEEK_SET) != 0 ||
            std::fread(dst, 1, payload_size, m_impl->img_file) != payload_size) {
            m_impl->free_block(aoff);
            continue;
        }

        // Prepare clean sChunkHeader for cRelocatableChunk::Fixup (unk1c=0, unk1e=0)
        auto* fix_hdr = reinterpret_cast<sChunkHeader*>(m_impl->temp_hdr_fixup);
        *fix_hdr = *c.hdr;
        fix_hdr->offsetInImg = 0;
        fix_hdr->unk1c = 0;
        fix_hdr->unk1e = 0;

        const uint32_t total_sz = c.hdr->totalSize;
        const uint32_t ptr_off  = fix_hdr->relocOffset;
        const uint32_t num_ptrs = fix_hdr->numRelocs;
        bool relocs_ok = (total_sz == sizeof(sChunkHeader) + payload_size);
        if (relocs_ok && num_ptrs > 0) {
            const uint64_t tab_end = static_cast<uint64_t>(ptr_off) + static_cast<uint64_t>(num_ptrs) * 4ULL;
            if (ptr_off < sizeof(sChunkHeader) || tab_end > total_sz) {
                relocs_ok = false;
            } else {
                const auto* relocs = reinterpret_cast<const uint32_t*>(dst + (ptr_off - sizeof(sChunkHeader)));
                for (uint32_t ri = 0; ri < num_ptrs; ++ri) {
                    const uint32_t rel_off = relocs[ri];
                    if (rel_off < sizeof(sChunkHeader) || static_cast<uint64_t>(rel_off) + 8ULL > total_sz) {
                        relocs_ok = false;
                        break;
                    }
                    const uint32_t target = *reinterpret_cast<const uint32_t*>(dst + (rel_off - sizeof(sChunkHeader)));
                    if (target > total_sz) {
                        relocs_ok = false;
                        break;
                    }
                }
            }
        }
        if (!relocs_ok) {
            std::cerr << "[LcsWorldHd] Skipping invalid reloc cell (" << c.cx << "," << c.cy
                      << ") total=" << total_sz << " ptrOff=" << ptr_off << " numPtrs=" << num_ptrs << "\n";
            m_impl->free_block(aoff);
            continue;
        }
        if (m_fn_fixup) {
            cpu->call_nested(m_fn_fixup, {
                0ULL,
                reinterpret_cast<uint64_t>(fix_hdr),
                reinterpret_cast<uint64_t>(dst)
            });
        }

        Impl::CachedCell cell;
        cell.level = level;
        cell.cx = c.cx;
        cell.cy = c.cy;
        cell.arena_offset = aoff;
        cell.alloc_size = asz;
        cell.payload = dst;
        cell.payload_size = payload_size;
        cell.last_used_frame = m_frame_index;

        const uint64_t pairs_ptr = *reinterpret_cast<const uint64_t*>(dst);
        const uint16_t npairs = *reinterpret_cast<const uint16_t*>(dst + 8);
        if (pairs_ptr && npairs > 0) {
            const auto* pairs = reinterpret_cast<const sPairEntry*>(pairs_ptr);
            cell.pair_gids.reserve(npairs);
            for (uint16_t pi = 0; pi < npairs; ++pi) {
                cell.pair_gids.push_back(pairs[pi].gid);
            }
        }

        const auto* bptrs = reinterpret_cast<const uint64_t*>(dst + 0x10ULL);
        std::unordered_set<uint32_t> ugids;
        for (size_t b = 0; b < kNumRenderBuckets; ++b) {
            for (uint64_t p = bptrs[b]; p < bptrs[b + 1]; p += kGeomInstSize) {
                const uint16_t gid = *reinterpret_cast<const uint16_t*>(p + 2ULL);
                ugids.insert(gid);
            }
        }
        cell.unique_gids.assign(ugids.begin(), ugids.end());

        m_impl->cells.emplace(pack_cell_key(c.cx, c.cy), std::move(cell));
        m_impl->cell_count.store(static_cast<int>(m_impl->cells.size()), std::memory_order_relaxed);
        ++m_impl->stats_cells_loaded;
        ++loaded_now;
        m_combined_dirty = true;
        m_reregister_needed = true;
    }

    if (m_reregister_needed || loaded_now > 0) {
        ensure_pairs_and_models(ws_ptr);
        m_reregister_needed = false;
    }
}

void LcsWorldHd::ensure_pairs_and_models(uint64_t ws_ptr) {
    if (!m_impl || !ws_ptr) return;
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    auto* cpu = active_cpu(m_cpu);
    if (!cpu) return;
    const uint64_t dir_ptr = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsDirOff);
    if (!dir_ptr) return;

    auto* table = reinterpret_cast<sBuildingDirEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr));
    const int32_t table_count = *reinterpret_cast<const int32_t*>(dir_ptr + 0x2f8ULL);
    if (!table || table_count <= 0) return;

    // 1. Bind dir->table[gid].geomPtr into permanent cached-cell arena whenever unset or pointing into
    //    cWorldStream's temporary native streaming ring buffers so cell swaps never leave dangling pointers.
    const int32_t level = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurrLevelOff);
    const uint64_t arena_lo = reinterpret_cast<uint64_t>(m_impl->arena_base);
    const uint64_t arena_hi = arena_lo + kCellPoolBytes;
    for (auto& [key, cell] : m_impl->cells) {
        if (cell.level != level || !cell.payload) continue;
        const uint64_t pairs_ptr = *reinterpret_cast<const uint64_t*>(cell.payload);
        const uint16_t npairs = *reinterpret_cast<const uint16_t*>(cell.payload + 8);
        if (!pairs_ptr || npairs == 0) continue;
        const auto* pairs = reinterpret_cast<const sPairEntry*>(pairs_ptr);
        for (uint16_t pi = 0; pi < npairs; ++pi) {
            const uint32_t gid = pairs[pi].gid;
            if (gid < static_cast<uint32_t>(table_count) && pairs[pi].geomPtr != 0) {
                const uint64_t cur_gp = table[gid].geomPtr;
                if (cur_gp == 0 || cur_gp < arena_lo || cur_gp >= arena_hi) {
                    table[gid].geomPtr = pairs[pi].geomPtr;
                    if (cur_gp == 0) {
                        cell.models_registered = false;
                        cell.gpu_ready = false;
                    }
                }
            }
        }
    }

    // 2. Register lglWorldManager GPU/DMA models incrementally (only for new/dirty cells, NOT all cells every frame)
    const uint64_t mgr = m_got_world_mgr ? *reinterpret_cast<const uint64_t*>(m_got_world_mgr) : 0;
    const uint64_t mgr_inst = mgr ? *reinterpret_cast<const uint64_t*>(mgr) : 0;
    if (!mgr_inst || !m_fn_get_model) return;

    for (auto& [key, cell] : m_impl->cells) {
        if (cell.level != level || cell.models_registered) continue;
        cell.dma_models.clear();
        cell.dma_models.reserve(cell.unique_gids.size());
        for (uint32_t gid : cell.unique_gids) {
            if (gid >= static_cast<uint32_t>(table_count)) continue;
            const uint64_t geom = table[gid].geomPtr;
            if (!geom) continue;
            const uint64_t model_key =
                (static_cast<uint64_t>(static_cast<uint32_t>(level)) << 32) | gid;
            const uint64_t dma = cpu->call_nested(m_fn_get_model, {mgr_inst, model_key, geom, 1ULL, 0ULL});
            if (dma) {
                cell.dma_models.push_back(dma);
            }
        }
        cell.models_registered = true;
        cell.gpu_ready = false;
    }
}

void LcsWorldHd::rebuild_combined_chunk(uint64_t ws_ptr) {
    if (!m_impl || !ws_ptr) return;
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    const uint64_t dir_ptr = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsDirOff);
    if (!dir_ptr) return;

    const uint64_t cur_begin = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBeginOff);
    const uint64_t cur_end   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurEndOff);
    const uint64_t native_chunk = (cur_begin && cur_end > cur_begin)
        ? *reinterpret_cast<const uint64_t*>(cur_begin) : 0;

    const auto* table = reinterpret_cast<const sBuildingDirEntry*>(*reinterpret_cast<const uint64_t*>(dir_ptr));
    const int32_t table_count = *reinterpret_cast<const int32_t*>(dir_ptr + 0x2f8ULL);
    const int32_t cur_cx = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurCxOff);
    const int32_t cur_cy = *reinterpret_cast<const int32_t*>(ws_ptr + kWsCurCyOff);

    auto check_cell_ready = [](Impl::CachedCell& c) -> bool {
        if (c.gpu_ready) return true;
        if (!c.models_registered) return false;
        for (uint64_t dma : c.dma_models) {
            if (!dma || *reinterpret_cast<const uint8_t*>(dma + 0x14ULL) == 0) {
                return false;
            }
        }
        c.gpu_ready = true;
        return true;
    };

    // Build distance-sorted list of active cells within m_radius for LOD twin coverage & draw ordering
    std::vector<const Impl::CachedCell*> active;
    active.reserve(m_impl->cells.size());
    const float max_dist_sq = cell_active_dist_sq(m_radius);
    std::unordered_set<uint64_t> next_covered;
    next_covered.reserve(m_impl->cells.size() + 4);
    int ready_cnt = 0;

    for (auto& [key, cell] : m_impl->cells) {
        if (cell.level != m_active_level) continue;
        float hx = 0.0f, hy = 0.0f;
        hex_cell_center(cell.cx, cell.cy, hx, hy);
        const float dx = hx - m_impl->last_cam_x;
        const float dy = hy - m_impl->last_cam_y;
        if (dx * dx + dy * dy <= max_dist_sq) {
            // Keep active render list and covered_cells strictly 1:1 so an unready HD cell
            // never draws partly-resident geometry alongside its LQ twin (or hides LQ early).
            if (check_cell_ready(cell)) {
                active.push_back(&cell);
                next_covered.insert(key);
                ++ready_cnt;
            } else if (native_chunk && cell.cx == cur_cx && cell.cy == cur_cy) {
                next_covered.insert(key);
            }
        }
    }
    if (native_chunk) {
        const uint64_t cur_key = pack_cell_key(cur_cx, cur_cy);
        auto it = m_impl->cells.find(cur_key);
        if (it == m_impl->cells.end() || it->second.gpu_ready) {
            next_covered.insert(cur_key);
        }
    }
    m_impl->ready_cell_count.store(ready_cnt, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
        m_impl->covered_cells.swap(next_covered);
        m_impl->sync_covered_fast();
    }

    // Sort active cells nearest-first so closer cells win deduplication and bucket fill order
    std::sort(active.begin(), active.end(), [&](const Impl::CachedCell* a, const Impl::CachedCell* b) {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        hex_cell_center(a->cx, a->cy, ax, ay);
        hex_cell_center(b->cx, b->cy, bx, by);
        const float da = (ax - m_impl->last_cam_x) * (ax - m_impl->last_cam_x) + (ay - m_impl->last_cam_y) * (ay - m_impl->last_cam_y);
        const float db = (bx - m_impl->last_cam_x) * (bx - m_impl->last_cam_x) + (by - m_impl->last_cam_y) * (by - m_impl->last_cam_y);
        return da < db;
    });

    // Deduplicate across overlapping adjacent hex cells using (inst_id & 0x7fff, gid) so multi-mesh buildings retain all parts
    std::unordered_set<uint32_t> seen_igids;
    seen_igids.reserve(4096);
    auto test_and_mark = [&seen_igids](uint16_t raw_id, uint16_t gid) -> bool {
        const uint32_t key = (static_cast<uint32_t>(raw_id & 0x7fffu) << 16) | gid;
        return seen_igids.insert(key).second;
    };

    float cur_hx = 0.0f, cur_hy = 0.0f;
    hex_cell_center(cur_cx, cur_cy, cur_hx, cur_hy);

    // LQ replacement: every drawn list (the game's current cell and each ready HD cell) leaves
    // out the low-detail instances its own detail replaces. Drawing the union of the lists
    // would draw those LQ shells over (and around) the HD geometry, so an LQ instance is
    // dropped when any drawn list replaces it. Bucket 0 (terrain/sea, spheres up to ~1 km) is
    // left to the game.
    std::vector<const LqIndex*> lq_lists;
    lq_lists.reserve(active.size() + 1);
    if (native_chunk) {
        // Rebuilt every time: which geometry is resident changes with streaming.
        m_impl->native_lq_index.build(reinterpret_cast<const uint64_t*>(native_chunk + 0x10ULL), cur_hx, cur_hy,
                                      true, table, table_count);
        m_impl->native_lq_chunk = native_chunk;
        lq_lists.push_back(&m_impl->native_lq_index);
    }
    for (auto* cell : active) {
        if (cell->cx == cur_cx && cell->cy == cur_cy && native_chunk) continue;
        auto& idx = const_cast<Impl::CachedCell*>(cell)->lq_index;
        float hx = 0.0f, hy = 0.0f;
        hex_cell_center(cell->cx, cell->cy, hx, hy);
        idx.build(reinterpret_cast<const uint64_t*>(cell->payload + 0x10ULL), hx, hy, false, table, table_count);
        lq_lists.push_back(&idx);
    }
    std::unordered_map<uint32_t, bool> lq_replaced_memo;
    lq_replaced_memo.reserve(512);
    int lq_replaced_count = 0;
    // `p` is an instance record in a list whose cell centre is (list_x, list_y).
    // LCS_WHD_LQ_RULE=1 enables the LQ replacement rule. Off by default: it removed whole
    // buildings at some spots (Red Light District 910,-442 / 927,-342, shots 2026-11-24).
    static const bool s_lq_rule = [] {
        const char* e = std::getenv("LCS_WHD_LQ_RULE");
        return e && e[0] == '1';
    }();
    auto lq_replaced = [&](size_t bucket, uint64_t p, float list_x, float list_y) -> bool {
        const uint16_t raw_id = *reinterpret_cast<const uint16_t*>(p);
        if (!s_lq_rule || bucket == 0 || (raw_id & 0x7fffu) < kLqInstanceIdMin) return false;
        const uint32_t key = instance_key(raw_id, *reinterpret_cast<const uint16_t*>(p + 2ULL));
        if (auto it = lq_replaced_memo.find(key); it != lq_replaced_memo.end()) return it->second;
        uint16_t hcx, hcy, hr;
        std::memcpy(&hcx, reinterpret_cast<const void*>(p + 0x04ULL), 2);
        std::memcpy(&hcy, reinterpret_cast<const void*>(p + 0x06ULL), 2);
        std::memcpy(&hr, reinterpret_cast<const void*>(p + 0x0aULL), 2);
        (void)list_x;
        (void)list_y;
        const float x = f16_to_f32(hcx);  // world coordinates, whatever list holds the record
        const float y = f16_to_f32(hcy);
        const float r = f16_to_f32(hr);
        bool replaced = false;
        for (const LqIndex* li : lq_lists) {
            if (li->replaces(key, x, y, r)) { replaced = true; break; }
        }
        lq_replaced_memo.emplace(key, replaced);
        if (replaced) ++lq_replaced_count;
        return replaced;
    };

    const int work_idx = (m_impl->render_depth.load(std::memory_order_acquire) > 0)
        ? (1 - m_impl->render_buf_idx)
        : (1 - m_impl->published_buf_idx);
    uint8_t* dst_chunk = m_impl->combined_chunk_bufs[work_idx];
    uint8_t* write_ptr = m_impl->combined_insts_bufs[work_idx];

    std::memset(dst_chunk, 0, 0x70ULL);
    auto* dst_buckets = reinterpret_cast<uint64_t*>(dst_chunk + 0x10ULL);
    size_t total_count = 0;

    const auto* nat_buckets = native_chunk
        ? reinterpret_cast<const uint64_t*>(native_chunk + 0x10ULL) : nullptr;

    // LCS_DIAG_WHD_DUMP=1: dumps every drawn cell list (up to 3 times) for tools/whd_lists_analyze.py
    {
        static const bool s_dump = (std::getenv("LCS_DIAG_WHD_DUMP") != nullptr);
        static int s_dumps = 0;
        static size_t s_last_active = 0;
        const int req = m_dump_request.exchange(-1);
        if (req >= 0 || (s_dump && s_dumps < 3 && active.size() >= 20 && active.size() >= s_last_active + 8)) {
            char path[64];
            if (req >= 0) {
                std::snprintf(path, sizeof(path), "scratch/whd_shot_%d.csv", req);
            } else {
                s_last_active = active.size();
                std::snprintf(path, sizeof(path), "scratch/whd_lists_%d.csv", s_dumps++);
            }
            if (FILE* f = std::fopen(path, "w")) {
                std::fprintf(f, "src_cx,src_cy,native,bucket,raw_id,gid,wx,wy,wz,r,cam_x,cam_y,cur_cx,cur_cy\n");
                auto dump_list = [&](const uint64_t* bk, int scx, int scy, int nat) {
                    float hx = 0.0f, hy = 0.0f;
                    hex_cell_center(scx, scy, hx, hy);
                    for (size_t b = 0; b < kNumRenderBuckets; ++b) {
                        for (uint64_t p = bk[b]; p < bk[b + 1]; p += kGeomInstSize) {
                            uint16_t rid, g, hcx, hcy, hcz, hr;
                            std::memcpy(&rid, reinterpret_cast<const void*>(p), 2);
                            std::memcpy(&g, reinterpret_cast<const void*>(p + 2), 2);
                            std::memcpy(&hcx, reinterpret_cast<const void*>(p + 4), 2);
                            std::memcpy(&hcy, reinterpret_cast<const void*>(p + 6), 2);
                            std::memcpy(&hcz, reinterpret_cast<const void*>(p + 8), 2);
                            std::memcpy(&hr, reinterpret_cast<const void*>(p + 10), 2);
                            const uint64_t m30 = *reinterpret_cast<const uint64_t*>(p + 0x30);
                            const float px = *reinterpret_cast<const float*>(p + 0x68);
                            const float py = *reinterpret_cast<const float*>(p + 0x6c);
                            std::fprintf(f, "%d,%d,%d,%zu,%u,%u,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%d,%d,%.1f,%.1f\n",
                                         scx, scy, nat, b, rid, g, f16_to_f32(hcx) + hx, f16_to_f32(hcy) + hy,
                                         f16_to_f32(hcz), f16_to_f32(hr), m_impl->last_cam_x, m_impl->last_cam_y,
                                         cur_cx, cur_cy, m30 ? 1 : 0, px, py);
                        }
                    }
                };
                if (nat_buckets) dump_list(nat_buckets, cur_cx, cur_cy, 1);
                for (const auto* cell : active) {
                    dump_list(reinterpret_cast<const uint64_t*>(cell->payload + 0x10ULL), cell->cx, cell->cy, 0);
                }
                std::fclose(f);
                std::cout << "  [WhdDump] wrote " << path << " cells=" << active.size() << "\n" << std::flush;
            }
        }
    }

    for (size_t b = 0; b < kNumRenderBuckets; ++b) {
        uint8_t* bucket_begin = write_ptr;
        dst_buckets[b] = reinterpret_cast<uint64_t>(bucket_begin);

        // 1. Native cur chunk entries go first in tie order so cur cell wins identical IDs
        if (nat_buckets && nat_buckets[b + 1] > nat_buckets[b]) {
            for (uint64_t p = nat_buckets[b]; p < nat_buckets[b + 1]; p += kGeomInstSize) {
                if (total_count >= kMaxCombinedInstances) break;
                const uint16_t raw_id = *reinterpret_cast<const uint16_t*>(p);
                const uint16_t gid = *reinterpret_cast<const uint16_t*>(p + 2ULL);
                if (gid >= static_cast<uint32_t>(table_count)) continue;
                // Mark in dedup set before skipping hidden (bit15) instances so overlapping neighbor cells
                // cannot resurrect instances explicitly disabled by cur (time-of-day / building swap / area).
                if (!test_and_mark(raw_id, gid)) continue;
                if (raw_id & 0x8000u) continue;
                if (lq_replaced(b, p, cur_hx, cur_hy)) continue;
                std::memcpy(write_ptr, reinterpret_cast<const void*>(p), kGeomInstSize);
                write_ptr += kGeomInstSize;
                ++total_count;
            }
        }

        // 2. Append deduplicated HD neighbor cell entries (clear bit15 & shift both world pos and fp16 sphere cx/cy)
        for (const auto* cell : active) {
            if (cell->cx == cur_cx && cell->cy == cur_cy && native_chunk) continue;
            float cell_hx = 0.0f, cell_hy = 0.0f;
            hex_cell_center(cell->cx, cell->cy, cell_hx, cell_hy);
            const float dx = cell_hx - cur_hx;
            const float dy = cell_hy - cur_hy;

            const auto* cbuckets = reinterpret_cast<const uint64_t*>(cell->payload + 0x10ULL);
            for (uint64_t p = cbuckets[b]; p < cbuckets[b + 1]; p += kGeomInstSize) {
                if (total_count >= kMaxCombinedInstances) break;
                const uint16_t raw_id = *reinterpret_cast<const uint16_t*>(p);
                const uint16_t gid = *reinterpret_cast<const uint16_t*>(p + 2ULL);
                if (gid >= static_cast<uint32_t>(table_count) || !table[gid].geomPtr) continue;
                if (!area_allows_instance(ws_ptr, dir_ptr, raw_id)) continue;
                if (!test_and_mark(raw_id, gid)) continue;
                if (lq_replaced(b, p, cell_hx, cell_hy)) continue;
                std::memcpy(write_ptr, reinterpret_cast<const void*>(p), kGeomInstSize);
                *reinterpret_cast<uint16_t*>(write_ptr) = raw_id & 0x7fffu;
                if ((dx != 0.0f || dy != 0.0f) &&
                    *reinterpret_cast<const uint64_t*>(write_ptr + 0x30ULL) == 0ULL) {
                    *reinterpret_cast<float*>(write_ptr + 0x68ULL) += dx;
                    *reinterpret_cast<float*>(write_ptr + 0x6cULL) += dy;
                    // The fp16 sphere is the same in every list, but Render() reads it in the
                    // current cell's frame: it moves with the position. (Not moving it lost the
                    // St Mark's safehouse door and HD shop fronts, 2026-11-24.)
                    static const bool s_shift_sphere = [] {
                        const char* e = std::getenv("LCS_WHD_SPHERE_SHIFT");
                        return !(e && e[0] == '0');
                    }();
                    if (s_shift_sphere) {
                        uint16_t hcx = 0, hcy = 0;
                        std::memcpy(&hcx, write_ptr + 0x04ULL, sizeof(uint16_t));
                        std::memcpy(&hcy, write_ptr + 0x06ULL, sizeof(uint16_t));
                        hcx = f32_to_f16(f16_to_f32(hcx) + dx);
                        hcy = f32_to_f16(f16_to_f32(hcy) + dy);
                        std::memcpy(write_ptr + 0x04ULL, &hcx, sizeof(uint16_t));
                        std::memcpy(write_ptr + 0x06ULL, &hcy, sizeof(uint16_t));
                    }
                }
                write_ptr += kGeomInstSize;
                ++total_count;
            }
        }

        // 3. Sort bucket ascending by (raw_id & 0x7fff) to satisfy cWorldStream::Render merge-walk & movable <=0x1f bound
        const size_t bucket_cnt = static_cast<size_t>(write_ptr - bucket_begin) / kGeomInstSize;
        if (bucket_cnt > 1) {
            using RawInst = std::array<uint8_t, kGeomInstSize>;
            auto* arr = reinterpret_cast<RawInst*>(bucket_begin);
            std::stable_sort(arr, arr + bucket_cnt, [](const RawInst& a, const RawInst& b) {
                const uint16_t ida = (static_cast<uint16_t>(a[0]) | (static_cast<uint16_t>(a[1]) << 8)) & 0x7fffu;
                const uint16_t idb = (static_cast<uint16_t>(b[0]) | (static_cast<uint16_t>(b[1]) << 8)) & 0x7fffu;
                return ida < idb;
            });
        }
    }
    dst_buckets[kNumRenderBuckets] = reinterpret_cast<uint64_t>(write_ptr);

    // Pre-unpack half-float spheres once per rebuild so per-pass frustum culling avoids f16 decoding.
    const uint8_t* inst_base = m_impl->combined_insts_bufs[work_idx];
    const uint64_t inst_base_addr = reinterpret_cast<uint64_t>(inst_base);
    auto& sp_vec = m_impl->cached_spheres[work_idx];
    sp_vec.resize(total_count);
    for (size_t b = 0; b <= kNumRenderBuckets; ++b) {
        m_impl->cached_bucket_idx[work_idx][b] =
            static_cast<uint32_t>((dst_buckets[b] - inst_base_addr) / kGeomInstSize);
    }
    for (size_t i = 0; i < total_count; ++i) {
        const uint8_t* sp_src = inst_base + i * kGeomInstSize;
        const uint16_t raw_id = *reinterpret_cast<const uint16_t*>(sp_src);
        uint16_t hcx = 0, hcy = 0, hcz = 0, hrad = 0;
        std::memcpy(&hcx,  sp_src + 0x04ULL, sizeof(uint16_t));
        std::memcpy(&hcy,  sp_src + 0x06ULL, sizeof(uint16_t));
        std::memcpy(&hcz,  sp_src + 0x08ULL, sizeof(uint16_t));
        std::memcpy(&hrad, sp_src + 0x0aULL, sizeof(uint16_t));
        auto& cs = sp_vec[i];
        cs.cx = f16_to_f32(hcx);
        cs.cy = f16_to_f32(hcy);
        cs.cz = f16_to_f32(hcz);
        cs.r  = f16_to_f32(hrad);
        cs.keep_always = ((raw_id & 0x7fffu) <= 0x1fu || !(cs.r > 0.0f)) ? 1 : 0;
    }

    m_impl->last_native_chunk = native_chunk;
    m_impl->published_buf_idx = work_idx;
    if (m_impl->render_depth.load(std::memory_order_acquire) == 0 && !m_impl->saved_remove.active) {
        m_impl->combined_vec_slot[0] = reinterpret_cast<uint64_t>(dst_chunk);
    }
    m_combined_inst_count = static_cast<int>(total_count);
    m_impl->last_lq_replaced = lq_replaced_count;
    m_combined_dirty = false;
}

namespace {
struct PropLodOrig {
    bool    valid       = false;
    uint8_t num_atomics = 0;
    float   d[3]        = {};
};
std::vector<PropLodOrig> s_prop_lod_orig;
int      s_last_prop_detail   = -999;
int32_t  s_last_prop_models   = -1;
int32_t  s_prop_sweep_cx      = 999999;
int32_t  s_prop_sweep_cy      = 999999;
int      s_prop_sweep_cursor  = 0;

float compute_prop_floor_dist(int detail_slider) {
    if (detail_slider < 30) return 0.0f;
    const float t = std::clamp(static_cast<float>(detail_slider - 30) / 70.0f, 0.0f, 1.0f);
    return (t <= 0.5f) ? (55.0f + (150.0f - 55.0f) * (t * 2.0f))
                       : (150.0f + (250.0f - 150.0f) * ((t - 0.5f) * 2.0f));
}

void sync_prop_lod_distances(uint64_t load_bias) {
    if (!load_bias) return;
    const uint64_t num_addr  = *reinterpret_cast<const uint64_t*>(load_bias + 0x7f8fa0ULL); // CModelInfo::msNumModelInfos
    const uint64_t ptrs_slot = *reinterpret_cast<const uint64_t*>(load_bias + 0x7f8fb0ULL); // CModelInfo::ms_modelInfoPtrs
    if (!num_addr || !ptrs_slot) return;
    const int32_t num_models = *reinterpret_cast<const int32_t*>(num_addr);
    const uint64_t ptrs_va   = *reinterpret_cast<const uint64_t*>(ptrs_slot);
    if (!ptrs_va || num_models <= 100 || num_models > 8192) return;

    const int cur_detail = lcs::settings::get().detail_distance;
    if (cur_detail == s_last_prop_detail && num_models == s_last_prop_models) return;

    if (static_cast<int32_t>(s_prop_lod_orig.size()) < num_models) {
        s_prop_lod_orig.resize(static_cast<size_t>(num_models));
    }

    const float prop_floor = compute_prop_floor_dist(cur_detail);
    const auto* ptrs = reinterpret_cast<const uint64_t*>(ptrs_va);
    for (int32_t id = 0; id < num_models; ++id) {
        const uint64_t mi = ptrs[id];
        if (!mi || mi < 0x10000ULL) continue;
        const uint8_t mtype = *reinterpret_cast<const uint8_t*>(mi + 0x28ULL);
        if (mtype != 1 && mtype != 3) continue; // CSimpleModelInfo / CTimeModelInfo
        const uint8_t na = *reinterpret_cast<const uint8_t*>(mi + 0x54ULL);
        if (na == 0 || na > 3) continue;

        auto& orig = s_prop_lod_orig[static_cast<size_t>(id)];
        if (!orig.valid) {
            orig.num_atomics = na;
            for (uint8_t k = 0; k < na; ++k) {
                orig.d[k] = *reinterpret_cast<const float*>(mi + 0x48ULL + static_cast<uint64_t>(k) * 4ULL);
            }
            orig.valid = true;
        }
        // Preserve immutable base LOD distances in CModelInfo (never overwrite mi+0x48 so
        // LcsDistance scaling hooks see the unmodified IDE distance on every query).
        (void)prop_floor;
    }
    s_last_prop_detail = cur_detail;
    s_last_prop_models = num_models;
}

bool is_readable_guest_ptr(uint64_t addr, size_t size) {
    if (addr < 0x10000ULL || (addr & 0x3ULL) != 0) return false;
#if defined(_WIN32)
    struct CachedVmRange {
        uint64_t lo = 0;
        uint64_t hi = 0;
    };
    static thread_local std::array<CachedVmRange, 8> s_vm_cache{};
    static thread_local size_t s_vm_next = 0;
    for (const auto& r : s_vm_cache) {
        if (addr >= r.lo && addr + size <= r.hi) return true;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & kReadable) == 0 || (mbi.Protect & PAGE_GUARD) != 0) return false;
    const uint64_t lo = reinterpret_cast<uint64_t>(mbi.BaseAddress);
    const uint64_t hi = lo + mbi.RegionSize;
    s_vm_cache[s_vm_next] = {lo, hi};
    s_vm_next = (s_vm_next + 1) & 7u;
    return addr + size <= hi;
#else
    (void)size;
    return true;
#endif
}

void prestream_sector_props(uint64_t load_bias, lcs::cpu::DynarmicCore* cpu,
                            int32_t center_cx, int32_t center_cy, float cam_x, float cam_y) {
    if (!load_bias || !cpu || s_prop_lod_orig.empty()) return;
    const uint64_t sectors_slot = *reinterpret_cast<const uint64_t*>(load_bias + 0x7f9838ULL); // CWorld::ms_aSectors
    const uint64_t inst_slot    = *reinterpret_cast<const uint64_t*>(load_bias + 0x7f9060ULL); // CStreaming::mspInst
    const uint64_t num_req_addr = *reinterpret_cast<const uint64_t*>(load_bias + 0x7fa2f8ULL); // CStreaming::ms_numModelsRequested
    const uint64_t fn_req_model = *reinterpret_cast<const uint64_t*>(load_bias + 0x800930ULL); // CStreaming::RequestModel
    if (!sectors_slot || !inst_slot || !num_req_addr || !fn_req_model) return;

    const uint64_t sectors_va = *reinterpret_cast<const uint64_t*>(sectors_slot);
    const uint64_t inst_va    = *reinterpret_cast<const uint64_t*>(inst_slot);
    const int32_t num_req     = *reinterpret_cast<const int32_t*>(num_req_addr);
    if (!sectors_va || !inst_va || num_req >= 96) return;

    if (center_cx != s_prop_sweep_cx || center_cy != s_prop_sweep_cy) {
        s_prop_sweep_cx     = center_cx;
        s_prop_sweep_cy     = center_cy;
        s_prop_sweep_cursor = 0;
    }

    constexpr float kSectorHalfExt = 50.0f;
    constexpr float kSectorInvSize = 1.0f / 40.0f;
    const int cam_sx = std::clamp(static_cast<int>(std::floor(cam_x * kSectorInvSize + kSectorHalfExt)), 0, 99);
    const int cam_sy = std::clamp(static_cast<int>(std::floor(cam_y * kSectorInvSize + kSectorHalfExt)), 0, 99);
    constexpr int kHalfSec = 10; // +/- 10 sectors = +/- 420m covering full expanded prop range
    const int total_sec = (kHalfSec * 2 + 1) * (kHalfSec * 2 + 1);
    if (s_prop_sweep_cursor >= total_sec) return;

    // Only walk the 6 verified CPtrListDoubleLink prop/world lists from CEntity::Add
    // (0x00/0x08 buildings, 0x18/0x20 objects, 0x48/0x50 dummies; 0x10 is NOT in CEntity::Add!)
    constexpr uint64_t kSectorPropLists[6] = {0x00ULL, 0x08ULL, 0x18ULL, 0x20ULL, 0x48ULL, 0x50ULL};
    constexpr int kMaxPrestreamPerTick = 8;
    int issued = 0;
    const int32_t max_id = std::min<int32_t>(static_cast<int32_t>(s_prop_lod_orig.size()), 0x1324);
    int sec_checked = 0;
    while (s_prop_sweep_cursor < total_sec && sec_checked < 12 && issued < kMaxPrestreamPerTick) {
        const int idx = s_prop_sweep_cursor;
        ++sec_checked;
        const int ox = (idx % (kHalfSec * 2 + 1)) - kHalfSec;
        const int oy = (idx / (kHalfSec * 2 + 1)) - kHalfSec;
        const int sx = cam_sx + ox;
        const int sy = cam_sy + oy;
        if (sx < 0 || sx > 99 || sy < 0 || sy > 99) {
            ++s_prop_sweep_cursor;
            continue;
        }

        const uint64_t sector_va = sectors_va + static_cast<uint64_t>(sy * 100 + sx) * 0x58ULL;
        bool sector_complete = true;
        for (uint64_t list_off : kSectorPropLists) {
            if (issued >= kMaxPrestreamPerTick) {
                sector_complete = false;
                break;
            }
            uint64_t node = *reinterpret_cast<const uint64_t*>(sector_va + list_off);
            int guard = 0;
            while (node && guard++ < 256) {
                if (issued >= kMaxPrestreamPerTick) {
                    sector_complete = false;
                    break;
                }
                if (!is_readable_guest_ptr(node, 0x18ULL)) break;
                const uint64_t ent = *reinterpret_cast<const uint64_t*>(node);
                node = *reinterpret_cast<const uint64_t*>(node + 0x10ULL);
                if (!is_readable_guest_ptr(ent, 0x80ULL)) continue;
                const int16_t mid = *reinterpret_cast<const int16_t*>(ent + 0x7cULL);
                if (mid <= 0 || mid >= max_id) continue;
                const auto& orig = s_prop_lod_orig[static_cast<size_t>(mid)];
                if (!orig.valid || orig.d[0] < 12.0f || orig.d[0] >= 300.0f) continue;
                const uint8_t state = *reinterpret_cast<const uint8_t*>(inst_va + static_cast<uint64_t>(mid) * 0x20ULL + 0x18ULL);
                if (state != 0) continue; // already loaded or queued
                cpu->call_nested(fn_req_model, {static_cast<uint64_t>(mid), 0ULL});
                ++issued;
            }
            if (!sector_complete) break;
        }
        // Advance only after finishing the entire sector so dense sectors are never truncated
        if (sector_complete) {
            ++s_prop_sweep_cursor;
        }
    }
}
} // namespace

void LcsWorldHd::on_world_update_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    static const bool s_whd_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    using Clk = std::chrono::steady_clock;
    const auto t0 = s_whd_audit ? Clk::now() : Clk::time_point{};
    double ms_sync = 0.0, ms_step = 0.0, ms_ens = 0.0, ms_reb = 0.0, ms_pre = 0.0;
    if (m_initialized) {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        m_impl->allow_lgl_destroy = false;
        const auto ts0 = s_whd_audit ? Clk::now() : Clk::time_point{};
        sync_prop_lod_distances(m_load_bias);
        if (s_whd_audit) ms_sync = std::chrono::duration<double, std::milli>(Clk::now() - ts0).count();
        if (m_radius > 0) {
            const auto t1 = s_whd_audit ? Clk::now() : Clk::time_point{};
            const int stream_budget = (m_impl->cells.size() >= 12) ? 3 : 8;
            step_streaming(ws_ptr, stream_budget);
            if (s_whd_audit) ms_step = std::chrono::duration<double, std::milli>(Clk::now() - t1).count();
            if (m_reregister_needed) {
                const auto t2 = s_whd_audit ? Clk::now() : Clk::time_point{};
                ensure_pairs_and_models(ws_ptr);
                if (s_whd_audit) ms_ens = std::chrono::duration<double, std::milli>(Clk::now() - t2).count();
                m_reregister_needed = false;
            }
            const uint64_t cur_begin = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBeginOff);
            const uint64_t cur_end   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurEndOff);
            if (cur_begin && cur_end > cur_begin &&
                *reinterpret_cast<const uint64_t*>(cur_begin) != m_impl->last_native_chunk) {
                m_combined_dirty = true;
            }
            if (m_combined_dirty || m_combined_inst_count == 0) {
                const auto t3 = s_whd_audit ? Clk::now() : Clk::time_point{};
                rebuild_combined_chunk(ws_ptr);
                if (s_whd_audit) ms_reb += std::chrono::duration<double, std::milli>(Clk::now() - t3).count();
            }
            if (m_active_level > 0 && !m_impl->cells.empty() &&
                (std::fabs(m_impl->last_cam_x) > 1.0f || std::fabs(m_impl->last_cam_y) > 1.0f)) {
                const auto t4 = s_whd_audit ? Clk::now() : Clk::time_point{};
                prestream_sector_props(m_load_bias, active_cpu(m_cpu), m_center_cx, m_center_cy,
                                       m_impl->last_cam_x, m_impl->last_cam_y);
                if (s_whd_audit) ms_pre = std::chrono::duration<double, std::milli>(Clk::now() - t4).count();
            }
            if (!m_combined_dirty) {
                // Re-check any unready cells so covered_cells flips true on the exact frame GPU creation finishes
                bool newly_ready = false;
                const float max_dist_sq = cell_active_dist_sq(m_radius);
                for (auto& [key, cell] : m_impl->cells) {
                    if (cell.level != m_active_level || cell.gpu_ready || !cell.models_registered) continue;
                    float hx = 0.0f, hy = 0.0f;
                    hex_cell_center(cell.cx, cell.cy, hx, hy);
                    const float dx = hx - m_impl->last_cam_x;
                    const float dy = hy - m_impl->last_cam_y;
                    if (dx * dx + dy * dy > max_dist_sq) continue;
                    bool ok = true;
                    for (uint64_t dma : cell.dma_models) {
                        if (!dma || *reinterpret_cast<const uint8_t*>(dma + 0x14ULL) == 0) {
                            ok = false;
                            break;
                        }
                    }
                    if (ok) {
                        cell.gpu_ready = true;
                        newly_ready = true;
                    }
                }
                if (newly_ready) m_combined_dirty = true;
            }
            if (m_combined_dirty) {
                const auto t5 = s_whd_audit ? Clk::now() : Clk::time_point{};
                rebuild_combined_chunk(ws_ptr);
                if (s_whd_audit) ms_reb += std::chrono::duration<double, std::milli>(Clk::now() - t5).count();
            }
        }
    }
    on_stream_post(ws_ptr);
    m_impl->ws_mtx.unlock();
    if (s_whd_audit) {
        const double ms_tot = std::chrono::duration<double, std::milli>(Clk::now() - t0).count();
        if (ms_tot > 1.2) {
            std::cout << "  [TMP_WHD] tot=" << ms_tot << " sync=" << ms_sync
                      << " step=" << ms_step << " ens=" << ms_ens
                      << " reb=" << ms_reb << " pre=" << ms_pre
                      << " cached=" << m_impl->cells.size() << "\n" << std::flush;
        }
    }
}

namespace {
bool     s_whd_fade_saved = false;
uint64_t s_whd_fade_ws    = 0;
int32_t  s_whd_fade_val   = 0;
} // namespace

uint64_t LcsWorldHd::on_world_render_pre(uint64_t ws_ptr, int pass) {
    if (!m_impl) return 0;
    m_impl->ws_mtx.lock();
    m_impl->render_depth.fetch_add(1, std::memory_order_acq_rel);
    if (!m_initialized || !ws_ptr) return 0;

    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    if (m_radius <= 0) return 0;

    // Skip extended multi-cell flood on shadow map pass so GPU shadow rendering stays fast
    if (m_got_dyn_shadows_active) {
        const uint64_t sact_ptr = *reinterpret_cast<const uint64_t*>(m_got_dyn_shadows_active);
        if (sact_ptr && *reinterpret_cast<const uint8_t*>(sact_ptr) != 0) {
            return 0;
        }
    }

    const uint64_t cur_begin = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBeginOff);
    const uint64_t cur_end   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurEndOff);
    if (!cur_begin || cur_end <= cur_begin) return 0;

    if (*reinterpret_cast<const uint64_t*>(cur_begin) != m_impl->last_native_chunk) {
        m_combined_dirty = true;
    }

    // Only swap already-published chunk buffers on RenderThread; never invoke guest calls
    // or mutate cell model pointers concurrently with gGameTickThread.
    if (m_combined_inst_count == 0) return 0;

    // Keep water fade offset aligned with native chunk so Render()'s water Z delta applies once
    if (pass == 0 && m_got_water_scroll) {
        const uint64_t wptr = *reinterpret_cast<const uint64_t*>(m_got_water_scroll);
        if (wptr) {
            const float wscroll = *reinterpret_cast<const float*>(wptr + 8ULL);
            *reinterpret_cast<float*>(ws_ptr + kWsWaterFadeOff) = -wscroll;
        }
    }

    const int ridx = m_impl->published_buf_idx;
    m_impl->render_buf_idx = ridx;
    uint64_t chunk_to_render = reinterpret_cast<uint64_t>(m_impl->combined_chunk_bufs[ridx]);

    // Host C++ frustum + pass-bucket pre-cull: shrink combined_chunk to visible pass instances
    // before entering cWorldStream::Render's Dynarmic loop.
    if (m_impl->filtered_chunk_buf && m_impl->got_cull_frustum && m_impl->got_cull_change_flag &&
        m_impl->got_cull_is_persp && m_impl->got_cull_sphere_enabled &&
        m_impl->got_clip_sphere_fudge && m_got_water_scroll) {
        const uint64_t p_frustum = *reinterpret_cast<const uint64_t*>(m_impl->got_cull_frustum);
        const uint64_t p_vmat    = m_impl->got_cull_view_mat
            ? *reinterpret_cast<const uint64_t*>(m_impl->got_cull_view_mat) : 0;
        const uint64_t p_change  = *reinterpret_cast<const uint64_t*>(m_impl->got_cull_change_flag);
        const uint64_t p_persp   = *reinterpret_cast<const uint64_t*>(m_impl->got_cull_is_persp);
        const uint64_t p_dbg     = *reinterpret_cast<const uint64_t*>(m_impl->got_cull_sphere_enabled);
        const uint64_t p_fudge   = *reinterpret_cast<const uint64_t*>(m_impl->got_clip_sphere_fudge);
        const uint64_t p_viewoff = *reinterpret_cast<const uint64_t*>(m_got_water_scroll);

        if (p_change && *reinterpret_cast<const uint32_t*>(p_change) != 0) {
            if (auto* cur_cpu = lcs::cpu::DynarmicCore::current()) {
                cur_cpu->call_nested(m_load_bias + kSymCullingUpdateRva, {1ULL});
            }
        }

        const uint8_t is_persp = p_persp ? *reinterpret_cast<const uint8_t*>(p_persp) : 0u;
        const bool cull_ready =
            p_frustum && p_change && p_persp && p_dbg && p_fudge && p_viewoff &&
            (is_persp != 0 || p_vmat != 0) &&
            (*reinterpret_cast<const uint32_t*>(p_change) == 0) &&
            (*reinterpret_cast<const uint8_t*>(p_dbg + 0x3dULL) != 0);

        if (cull_ready) {
            size_t b_start = 0, b_end = kNumRenderBuckets;
            if (pass == 0)      { b_start = 0; b_end = 3; }
            else if (pass == 1) { b_start = 3; b_end = 5; }
            else if (pass == 2) { b_start = 5; b_end = 7; }

            const float* f     = reinterpret_cast<const float*>(p_frustum);
            const float* vo    = reinterpret_cast<const float*>(p_viewoff);
            const float  fudge = (*reinterpret_cast<const float*>(p_fudge)) * 1.15f;
            const float  vox = vo[0], voy = vo[1], voz = vo[2];

            const float cam_x = f[0x168 / 4], cam_y = f[0x16c / 4], cam_z = f[0x170 / 4];
            const float nf_x  = -f[0x158 / 4], nf_y = -f[0x15c / 4], nf_z = -f[0x160 / 4];
            const float d_off = f[0x164 / 4];
            const float near_d = f[0x18c / 4], far_d = f[0x190 / 4];
            const float py_x = f[0x148 / 4], py_y = f[0x14c / 4], py_z = f[0x150 / 4], py_w = f[0x154 / 4];
            const float px_x = f[0x138 / 4], px_y = f[0x13c / 4], px_z = f[0x140 / 4], px_w = f[0x144 / 4];
            const float rad_my = f[0x17c / 4], rad_mx = f[0x178 / 4];
            const float tan_y  = f[0x180 / 4], asp_x  = f[0x188 / 4];

            const float* vm = is_persp ? nullptr : reinterpret_cast<const float*>(p_vmat);
            const float m00 = vm ? vm[0]  : 0.0f, m01 = vm ? vm[1]  : 0.0f, m02 = vm ? vm[2]  : 0.0f;
            const float m10 = vm ? vm[4]  : 0.0f, m11 = vm ? vm[5]  : 0.0f, m12 = vm ? vm[6]  : 0.0f;
            const float m20 = vm ? vm[8]  : 0.0f, m21 = vm ? vm[9]  : 0.0f, m22 = vm ? vm[10] : 0.0f;
            const float m30 = vm ? vm[12] : 0.0f, m31 = vm ? vm[13] : 0.0f, m32 = vm ? vm[14] : 0.0f;
            const float o_min_x = f[0], o_min_y = f[1], o_min_z = f[6];
            const float o_max_x = f[4], o_max_y = f[5], o_max_z = f[2];

            uint8_t* dst_hdr     = m_impl->filtered_chunk_buf;
            uint8_t* write_ptr   = m_impl->filtered_insts_buf;
            auto*    dst_buckets = reinterpret_cast<uint64_t*>(dst_hdr + 0x10ULL);
            std::memset(dst_hdr, 0, 0x70ULL);

            const auto&    sp_vec    = m_impl->cached_spheres[ridx];
            const uint8_t* src_insts = m_impl->combined_insts_bufs[ridx];
            const auto*    b_idx     = m_impl->cached_bucket_idx[ridx];
            size_t         kept      = 0;
            bool           overflow  = false;

            for (size_t b = 0; b < kNumRenderBuckets; ++b) {
                dst_buckets[b] = reinterpret_cast<uint64_t>(write_ptr);
                if (b < b_start || b >= b_end) continue;

                const uint32_t i_begin = b_idx[b];
                const uint32_t i_end   = b_idx[b + 1];
                for (uint32_t i = i_begin; i < i_end && i < sp_vec.size(); ++i) {
                    const auto& cs = sp_vec[i];
                    if (!cs.keep_always) {
                        const float wx   = cs.cx + vox;
                        const float wy   = cs.cy + voy;
                        const float wz   = cs.cz + voz;
                        const float rad  = cs.r * fudge;
                        if (is_persp) {
                            const float dist = (wx - cam_x) * nf_x + (wy - cam_y) * nf_y +
                                               (wz - cam_z) * nf_z - d_off;
                            if (dist > far_d + rad || dist < near_d - rad) continue;

                            const float dx = wx - cam_x;
                            const float dy = wy - cam_y;
                            const float dz = wz - cam_z;
                            const float dist_ty = dist * tan_y;
                            const float dot_y   = dx * py_x + dy * py_y + dz * py_z + py_w;
                            const float lim_y   = rad * rad_my + dist_ty;
                            if (dot_y > lim_y || dot_y < -lim_y) continue;

                            const float dot_x = dx * px_x + dy * px_y + dz * px_z + px_w;
                            const float lim_x = rad * rad_mx + dist_ty * asp_x;
                            if (dot_x > lim_x || dot_x < -lim_x) continue;
                        } else {
                            const float tx = wx * m00 + wy * m10 + wz * m20 + m30;
                            const float ty = wx * m01 + wy * m11 + wz * m21 + m31;
                            const float tz = wx * m02 + wy * m12 + wz * m22 + m32;
                            float d2 = 0.0f;
                            if (tx < o_min_x)      d2 += (tx - o_min_x) * (tx - o_min_x);
                            else if (tx > o_max_x) d2 += (tx - o_max_x) * (tx - o_max_x);
                            if (ty < o_min_y)      d2 += (ty - o_min_y) * (ty - o_min_y);
                            else if (ty > o_max_y) d2 += (ty - o_max_y) * (ty - o_max_y);
                            if (tz < o_min_z)      d2 += (tz - o_min_z) * (tz - o_min_z);
                            else if (tz > o_max_z) d2 += (tz - o_max_z) * (tz - o_max_z);
                            if (d2 >= 3.0f * rad * rad + 2.0f) continue;
                        }
                    }
                    if (kept >= kMaxFilteredInstances) {
                        overflow = true;
                        break;
                    }
                    std::memcpy(write_ptr, src_insts + static_cast<size_t>(i) * kGeomInstSize,
                                kGeomInstSize);
                    write_ptr += kGeomInstSize;
                    ++kept;
                }
                if (overflow) break;
            }
            dst_buckets[kNumRenderBuckets] = reinterpret_cast<uint64_t>(write_ptr);
            if (!overflow) {
                chunk_to_render = reinterpret_cast<uint64_t>(dst_hdr);
            }
        }
    }

    m_impl->combined_vec_slot[0] = chunk_to_render;

    m_impl->saved_render.active    = true;
    m_impl->saved_render.ws_ptr    = ws_ptr;
    m_impl->saved_render.cur_begin = cur_begin;
    m_impl->saved_render.cur_end   = cur_end;
    m_impl->saved_render.prev_end  = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsPrevEndOff);

    const uint64_t vec_ptr = reinterpret_cast<uint64_t>(m_impl->combined_vec_slot);
    *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurBeginOff) = vec_ptr;
    *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurEndOff)   = vec_ptr + 8ULL;
    // Empty prev slice during combined draw to avoid two-pointer sort assumptions across cells
    *reinterpret_cast<uint64_t*>(ws_ptr + kWsPrevEndOff) =
        *reinterpret_cast<const uint64_t*>(ws_ptr + kWsPrevBeginOff);
    // Keep m_fadeTimer (0x70) at 0 during combined Render() so cell-crossing crossfades never
    // branch to m_prevChunks (empty -> world vanishes) or alpha-blend 28 HD cells together.
    constexpr size_t kWsFadeTimerOff = 0x70ULL;
    if (!s_whd_fade_saved) {
        s_whd_fade_ws    = ws_ptr;
        s_whd_fade_val   = *reinterpret_cast<const int32_t*>(ws_ptr + kWsFadeTimerOff);
        s_whd_fade_saved = true;
        *reinterpret_cast<int32_t*>(ws_ptr + kWsFadeTimerOff) = 0;
    }

    ++m_impl->stats_draws_swapped;
    return 0;
}

void LcsWorldHd::on_world_render_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        if (m_impl->saved_render.active && m_impl->saved_render.ws_ptr == ws_ptr) {
            *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurBeginOff) = m_impl->saved_render.cur_begin;
            *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurEndOff)   = m_impl->saved_render.cur_end;
            *reinterpret_cast<uint64_t*>(ws_ptr + kWsPrevEndOff)  = m_impl->saved_render.prev_end;
            if (s_whd_fade_saved && s_whd_fade_ws == ws_ptr) {
                *reinterpret_cast<int32_t*>(ws_ptr + 0x70ULL) = s_whd_fade_val;
                s_whd_fade_saved = false;
            }
            m_impl->saved_render.active = false;
        }
    }
    m_impl->render_depth.fetch_sub(1, std::memory_order_acq_rel);
    m_impl->ws_mtx.unlock();
}

uint64_t LcsWorldHd::on_remove_old_models_pre(uint64_t ws_ptr) {
    if (!m_impl) return 0;
    m_impl->ws_mtx.lock();
    if (!m_initialized || !ws_ptr) return 0;
    std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
    sanitize_stream_table(ws_ptr, /*scrub_prev=*/false, /*scrub_stream=*/true);
    if (m_radius <= 0) return 0;

    const uint64_t cur_begin = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurBeginOff);
    const uint64_t cur_end   = *reinterpret_cast<const uint64_t*>(ws_ptr + kWsCurEndOff);
    if (!cur_begin || cur_end <= cur_begin) return 0;

    if (*reinterpret_cast<const uint64_t*>(cur_begin) != m_impl->last_native_chunk || m_combined_dirty) {
        rebuild_combined_chunk(ws_ptr);
    }
    if (m_combined_inst_count == 0) return 0;

    m_impl->combined_vec_slot[0] =
        reinterpret_cast<uint64_t>(m_impl->combined_chunk_bufs[m_impl->published_buf_idx]);

    m_impl->saved_remove.active    = true;
    m_impl->saved_remove.ws_ptr    = ws_ptr;
    m_impl->saved_remove.cur_begin = cur_begin;
    m_impl->saved_remove.cur_end   = cur_end;

    const uint64_t vec_ptr = reinterpret_cast<uint64_t>(m_impl->combined_vec_slot);
    *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurBeginOff) = vec_ptr;
    *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurEndOff)   = vec_ptr + 8ULL;
    // Leave m_prevChunks intact so native loop 0x39c114 zeroes any table[gid].geomPtr that still points
    // into m_prevChunks while sparing arena_base cells whose pointers do not match m_prevChunks.pairs.
    return 0;
}

void LcsWorldHd::on_remove_old_models_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        if (m_impl->saved_remove.active && m_impl->saved_remove.ws_ptr == ws_ptr) {
            *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurBeginOff) = m_impl->saved_remove.cur_begin;
            *reinterpret_cast<uint64_t*>(ws_ptr + kWsCurEndOff)   = m_impl->saved_remove.cur_end;
            m_impl->saved_remove.active = false;
            ensure_pairs_and_models(ws_ptr);
        }
    }
    m_impl->ws_mtx.unlock();
}

uint64_t LcsWorldHd::on_swap_sector_pre(uint64_t ws_ptr) {
    (void)ws_ptr;
    if (m_impl) m_impl->ws_mtx.lock();
    return 0;
}

void LcsWorldHd::on_swap_sector_post(uint64_t ws_ptr) {
    if (!m_impl) return;
    if (m_initialized && ws_ptr) {
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        if (m_radius > 0) {
            ensure_pairs_and_models(ws_ptr);
        }
        sanitize_stream_table(ws_ptr, /*scrub_prev=*/true, /*scrub_stream=*/true);
        if (m_radius > 0) {
            m_combined_dirty = true;
            rebuild_combined_chunk(ws_ptr);
        }
    }
    m_impl->ws_mtx.unlock();
}

uint64_t LcsWorldHd::on_lgl_destroy_pre(uint64_t /*mgr_ptr*/, int destroy_all) {
    if (!m_initialized || !m_impl || m_radius <= 0) return 0;
    if (destroy_all == 0 && !m_impl->allow_lgl_destroy) {
        return 1; // Preserve cached active HD sDMAModels across cell transitions
    }
    return 0;
}

void LcsWorldHd::on_frame(uint64_t frame_index) {
    m_frame_index = frame_index;
}

void LcsWorldHd::dump_diagnostics(const char* out_path) const {
    if (!out_path) return;
    std::ofstream f(out_path);
    if (!f.is_open()) return;
    size_t cov_count = 0;
    uint64_t st_loaded = 0, st_evicted = 0, st_swapped = 0;
    if (m_impl) {
        {
            std::lock_guard<std::mutex> clk(m_impl->covered_mtx);
            cov_count = m_impl->covered_cells.size();
        }
        std::lock_guard<std::recursive_mutex> lk(m_impl->mtx);
        st_loaded = m_impl->stats_cells_loaded;
        st_evicted = m_impl->stats_cells_evicted;
        st_swapped = m_impl->stats_draws_swapped;
    }
    f << "=== LcsWorldHd Diagnostics ===\n"
      << "initialized=" << (m_initialized ? 1 : 0)
      << " radius=" << m_radius
      << " level=" << m_active_level
      << " center=(" << m_center_cx << "," << m_center_cy << ")\n"
      << "loaded_cells=" << loaded_cell_count()
      << " combined_instances=" << m_combined_inst_count << "\n";
    if (m_impl) {
        f << "stats_loaded=" << st_loaded
          << " stats_evicted=" << st_evicted
          << " stats_draws_swapped=" << st_swapped
          << " covered_cells=" << cov_count << "\n";
    }
}

} // namespace lcs::game
