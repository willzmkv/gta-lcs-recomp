#include "game/lcs_distance.h"
#include "game/lcs_world_hd.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

// Verified libGame.so RVAs & GOT slots (arm64-v8a)
constexpr uint64_t kGotPoolsInitialiseRva     = 0x8017C0ULL; // CPools::Initialise()
constexpr uint64_t kGotCameraProcessRva       = 0x801978ULL; // CCamera::Process()
constexpr uint64_t kGotGetElemFromDistRva     = 0x803810ULL; // CSimpleModelInfo::GetElementFromDistance(float)
constexpr uint64_t kGotGetLargestLodRva       = 0x802538ULL; // CSimpleModelInfo::GetLargestLodDistance()
constexpr uint64_t kGotSetupBigBldgRva        = 0x8037E0ULL; // CRenderer::SetupBigBuildingVisibility(CEntity*)
constexpr uint64_t kGotGetLodSettingsRva      = 0x804968ULL; // C_DeviceDetailSettings::GetLODSettings()
constexpr uint64_t kGotGetLodLevelRva         = 0x8087D8ULL; // GetLODSettingsLevel()
constexpr uint64_t kGotGetCullFadeLevelRva    = 0x8087E0ULL; // GetCullFadeLevel()
constexpr uint64_t kGotSetFarClipPlaneRva     = 0x801BC8ULL; // RslCameraSetFarClipPlane(RslCamera*, float)

// Pool pointer GOT slots
constexpr uint64_t kGotPedPoolRva             = 0x7f9a38ULL; // 70  -> 140 (0xdc0 ea)
constexpr uint64_t kGotVehiclePoolRva         = 0x7f9720ULL; // 70  -> 140 (0x7b0 ea)
constexpr uint64_t kGotObjectPoolRva          = 0x7f9a40ULL; // 475 -> 950 (0x270 ea)
constexpr uint64_t kGotPtrNodePoolRva         = 0x7f9ee0ULL; // 30100 -> 60000 (0x18 ea)
constexpr uint64_t kGotEntryInfoNodePoolRva   = 0x7f9960ULL; // 4000 -> 8000 (0x28 ea)
constexpr uint64_t kGotDummyPoolRva           = 0x7f9830ULL; // 3000 -> 6000 (0x90 ea)

// Global symbols / GOTs
constexpr uint64_t kGotPrefsGraphicsDetailRva = 0x7fc0d0ULL; // int CMenuManager::m_PrefsGraphicsDetail
constexpr uint64_t kGotRendererLodScaleRva    = 0x7f96d8ULL; // float CRenderer::ms_lodDistScale
constexpr uint64_t kGotDrawLodDistRva         = 0x7f9350ULL; // float CDraw::ms_fLODDistance
constexpr uint64_t kGotDrawFarClipRva         = 0x7f96e8ULL; // float CDraw::ms_fFarClipZ
constexpr uint64_t kGotCarNumMultRva          = 0x7f9ae8ULL; // float CIniFile::CarNumberMultiplier
constexpr uint64_t kGotPedNumMultRva          = 0x7f9ae0ULL; // float CIniFile::PedNumberMultiplier
constexpr uint64_t kGotMaxCarsInUseRva        = 0x7faeb8ULL; // uint32_t CCarCtrl::MaxNumberOfCarsInUse

constexpr uint64_t kSymZnamRva                = 0x2e1d20ULL; // operator new[](size_t)
constexpr uint64_t kSymZdaPvRva               = 0x2e1e24ULL; // operator delete[](void*)

// Got/Bss slots for mobile screen-size cull-fade overrides, CVisibilityComponents & cWorldStream
constexpr uint64_t kGotGetLastElemRva              = 0x803828ULL; // CSimpleModelInfo::GetLastElement(float)
constexpr uint64_t kGotVisInitRva                  = 0x802758ULL; // CVisibilityComponents::Initialise()
constexpr uint64_t kGotVisAlphaListRva             = 0x7FC9E0ULL; // CVisibilityComponents::m_alphaList
constexpr uint64_t kGotVisAlphaEntListRva          = 0x7FA2C0ULL; // CVisibilityComponents::m_alphaEntityList
constexpr uint64_t kGotWsPDynamicRva               = 0x803800ULL; // cWorldStream::pDynamic(int, int)
constexpr uint64_t kGotDeviceDetailSettingsRva     = 0x7FAE88ULL; // gDeviceDetailSettings
constexpr uint64_t kGotDvDougalRenderRva           = 0x7FE0D8ULL; // Lucid::dvDougalRender (Render.CullFade.Model.DontRenderDougal)
constexpr uint64_t kGotDvEnableCullFadeHackRva     = 0x7FE2C8ULL; // dvEnableCullFadeHack
constexpr uint64_t kGotDvReturnOnSmallRva          = 0x7FE2D0ULL; // dvRenderCullHackReturnOnSmall
constexpr uint64_t kGotDvMaxRenderedPedsRva        = 0x7FA268ULL; // dv_maxRenderedPeds (+0x4c int32)
constexpr uint64_t kGotDvMaxRenderedVehiclesRva    = 0x7FA260ULL; // dv_maxRenderedVehicles (+0x4c int32)
constexpr uint64_t kGotDvMaxRenderedOtherRva       = 0x7FA270ULL; // dv_maxRenderedOther (+0x4c int32)
constexpr uint64_t kGotCameraRva                   = 0x7F9190ULL; // TheCamera
constexpr uint64_t kGotScanWorldRva                = 0x8036A0ULL; // CRenderer::ScanWorld()
constexpr uint64_t kGotModelCreatorReadyRva        = 0x809900ULL; // lglModelCreator::createReady()
constexpr uint64_t kGotBufferCreatorReadyRva       = 0x809948ULL; // lglBufferCreator::createReady(int&, int&, bool)
constexpr uint64_t kPatchWorldCreatorBreakRva      = 0x589130ULL; // lglWorldCreator::createReady tbnz w21, #0, exit
constexpr uint64_t kPatchCalcAspectFcselRva        = 0x46F854ULL; // CDraw::CalculateAspectRatio fcsel s0, s0, s1, eq

// Hardcoded `mov w8, #0x43a50000` (330.0f) checks in SetupBigBuildingVisibility & SetupEntityVisibility
constexpr uint64_t kPatch330BigBldgRva        = 0x360c78ULL;
constexpr uint64_t kPatch330EntityRva         = 0x361150ULL;
// Prop streaming throttle & world/population distance caps
constexpr uint64_t kPatchScanReqCapRva        = 0x3609f4ULL; // CRenderer::ScanSectorList cmp w8, #9
constexpr uint64_t kPatchScanPrioLp1Rva       = 0x360788ULL; // CRenderer::ScanSectorList_Priority mov w9, #1 (m_loadingPriority)
constexpr uint64_t kPatchPopConvertRva        = 0x462ea0ULL; // CPopulation::ManagePopulation mov w22, #0x42a00000 (80.0f)
constexpr uint64_t kPatchScanWorld300A        = 0x35e9c8ULL; // CRenderer::ScanWorld mov w9, #0x43960000 (300.0f)
constexpr uint64_t kPatchScanWorld300B        = 0x35e9f4ULL;
constexpr uint64_t kPatchScanWorld300C        = 0x35ea28ULL; // mov w11, #0x43960000
constexpr uint64_t kPatchScanWorldSplitRva    = 0x35ee2cULL; // CRenderer::ScanWorld mov w10, #0x43960000 (priority split threshold)
constexpr uint64_t kPatchReqFrustum300A       = 0x3619f8ULL; // CRenderer::RequestObjectsInFrustum mov w9, #0x43960000
constexpr uint64_t kPatchReqFrustum300B       = 0x361a5cULL;
constexpr uint64_t kPatchAddDynamicDistSqRva  = 0x3a1600ULL; // cWorldStream::AddDynamic mov/movk w9, #10000.0f (100m^2)

uint64_t s_orig_get_last_elem = 0;

#pragma pack(push, 1)
struct GuestPool {
    uint64_t m_pObjects;   // +0x00
    uint64_t m_byteMap;    // +0x08
    int32_t  m_nSize;      // +0x10
    int32_t  m_nFirstFree; // +0x14
};
static_assert(sizeof(GuestPool) == 0x18);

struct GuestLodSettings {
    float   VehicleDist0;               // +0x00
    float   VehicleDist1;               // +0x04
    float   VehicleFadeDist;            // +0x08
    float   VehicleHiddenComponentDist; // +0x0c
    float   BigVehicleDist0;            // +0x10
    float   BigVehicleDist1;            // +0x14
    float   PedDist;                    // +0x18
    float   PedFadeDist;                // +0x1c
    int32_t MaxPeds;                    // +0x20
    int32_t MaxPedsInterior;            // +0x24
    int32_t MaxRandomCars;              // +0x28
    int32_t MaxParkedCars;              // +0x2c
    float   PopulationCullRange;        // +0x30
    float   PopulationCullRangeOffScr;  // +0x34
    float   PopulationCullRangeFarAway; // +0x38
    float   PopulationAddMinRadius;     // +0x3c
    float   PopulationAddMaxRadius;     // +0x40
    float   DriverHighDetailRange;      // +0x44
    float   BoatRiderHighDetailRange;   // +0x48
};
static_assert(sizeof(GuestLodSettings) == 76);
#pragma pack(pop)

inline uint32_t ldr_x16_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000010u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

inline uint32_t ldr_x0_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000000u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

void patch_mem(lcs::cpu::DynarmicCore& cpu, uint64_t va, const void* src, size_t sz) {
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

uint64_t patch_got_slot(lcs::cpu::DynarmicCore& cpu, uint64_t load_bias, uint64_t got_rva,
                        uint64_t new_target, const lcs::cpu::ElfImage& img, const char* sym) {
    const uint64_t slot = load_bias + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot);
    if (!orig && sym) orig = img.find_symbol(sym);
    patch_mem(cpu, slot, &new_target, sizeof(uint64_t));
    return orig;
}

float map_slider_curve(int val, float min_s, float mid_s, float max_s) {
    const float t = static_cast<float>(std::clamp(val, 0, 100));
    if (t <= 50.0f) {
        return min_s + (mid_s - min_s) * (t / 50.0f);
    }
    return mid_s + (max_s - mid_s) * ((t - 50.0f) / 50.0f);
}

float compute_view_scale() {
    return map_slider_curve(lcs::settings::get().view_distance, 0.55f, 1.35f, 3.60f);
}

float compute_detail_scale() {
    return map_slider_curve(lcs::settings::get().detail_distance, 0.50f, 1.15f, 3.25f);
}

float compute_pop_dist_scale() {
    return map_slider_curve(lcs::settings::get().population_distance, 0.55f, 1.15f, 2.75f);
}

float compute_traffic_scale() {
    return map_slider_curve(lcs::settings::get().traffic_density, 0.35f, 1.10f, 2.25f);
}

float compute_ped_scale() {
    return map_slider_curve(lcs::settings::get().ped_density, 0.35f, 1.10f, 2.20f);
}

template <typename T>
T* deref_got(uint64_t load_bias, uint64_t got_rva) {
    const uint64_t ptr = *reinterpret_cast<const uint64_t*>(load_bias + got_rva);
    return reinterpret_cast<T*>(ptr);
}

// Compute consistent effective draw distance for props, storefronts, fences, trees and small
// buildings across [12m .. 300m) given their uncorrupted base LOD distance and detail_scale.
float compute_prop_effective_dist(float raw_largest, float detail_scale) {
    const int detail_dist = lcs::settings::get().detail_distance;
    const float scaled = raw_largest * detail_scale;
    if (detail_dist >= 30 && raw_largest >= 12.0f && raw_largest < 300.0f) {
        const float prop_floor = map_slider_curve(detail_dist, 55.0f, 160.0f, 550.0f);
        return std::max(scaled, prop_floor);
    }
    return scaled;
}

// Override mobile screen-size cull-fade hacks (`cWorldStreamEx::DrawPrimitiveRef` +
// `Lucid::C_Mesh::Render` + `gDeviceDetailSettings` CullFade structs) so small buildings,
// facades, cones, lampposts, and trees are never discarded on-screen by pixel-fraction culling.
void apply_cull_fade_overrides(uint64_t load_bias, float detail_scale, int detail_dist) {
    if (!load_bias) return;
    auto* dv_cull_hack   = deref_got<uint8_t>(load_bias, kGotDvEnableCullFadeHackRva);
    auto* dv_ret_small   = deref_got<uint8_t>(load_bias, kGotDvReturnOnSmallRva);
    auto* dv_dougal      = deref_got<uint8_t>(load_bias, kGotDvDougalRenderRva);
    auto* detail_struct  = deref_got<float>(load_bias, kGotDeviceDetailSettingsRva);

    // ConstructRenderList sorts visible entities closest-to-farthest and drops non-ped/non-veh props
    // beyond dv_maxRenderedOther (High preset default is only 30!). Negative values disable the cap.
    auto set_dv_i32 = [&](uint64_t rva, int32_t v) {
        if (auto* p = deref_got<uint8_t>(load_bias, rva)) {
            *reinterpret_cast<int32_t*>(p + 0x4cULL) = v;
        }
    };
    set_dv_i32(kGotDvMaxRenderedOtherRva,
               (detail_dist >= 50) ? -1 : std::max<int32_t>(30, static_cast<int32_t>(std::lround(30.0f + detail_dist * 3.0f))));
    set_dv_i32(kGotDvMaxRenderedPedsRva, (detail_dist >= 50) ? -1 : 32);
    set_dv_i32(kGotDvMaxRenderedVehiclesRva, (detail_dist >= 50) ? -1 : 32);

    if (detail_dist >= 75) {
        if (dv_cull_hack) dv_cull_hack[0x3d] = 0;
        if (dv_ret_small) dv_ret_small[0x3d] = 0;
        if (dv_dougal)    dv_dougal[0x3d]    = 0;
        if (detail_struct) {
            // WorldCullFadeSettings[0..2] at +0x00 (+0, +4, +8 floats) & ModelCullFadeSettings[0..2] at +0x34 (+13 floats)
            for (int tier = 0; tier < 3; ++tier) {
                float* w = detail_struct + tier * 4;
                w[0] = 5000.0f; w[1] = 1000.0f; w[2] = 0.0f; w[3] = 0.0f;
                float* m = detail_struct + 13 + tier * 4;
                m[0] = 5000.0f; m[1] = 1000.0f; m[2] = 0.0f; m[3] = 0.0f;
            }
        }
    } else if (detail_dist >= 30) {
        const float s = std::max(1.0f, detail_scale);
        if (dv_cull_hack) dv_cull_hack[0x3d] = 1;
        if (dv_ret_small) dv_ret_small[0x3d] = 0;
        if (dv_dougal)    dv_dougal[0x3d]    = 0;
        if (detail_struct) {
            for (int tier = 0; tier < 3; ++tier) {
                float* w = detail_struct + tier * 4;
                w[0] = 50.0f * s; w[1] = 2000.0f * s; w[2] = 0.5f / s; w[3] = 0.06f / s;
                float* m = detail_struct + 13 + tier * 4;
                m[0] = 50.0f * s; m[1] = 2000.0f * s; m[2] = 0.5f / s; m[3] = 0.06f / s;
            }
        }
    } else {
        if (dv_cull_hack) dv_cull_hack[0x3d] = 1;
        if (dv_ret_small) dv_ret_small[0x3d] = 1;
        if (dv_dougal)    dv_dougal[0x3d]    = 1;
        if (detail_struct) {
            for (int tier = 0; tier < 3; ++tier) {
                float* w = detail_struct + tier * 4;
                w[0] = 50.0f; w[1] = 2000.0f; w[2] = 0.5f; w[3] = 0.06f;
                float* m = detail_struct + 13 + tier * 4;
                m[0] = 50.0f; m[1] = 2000.0f; m[2] = 0.5f; m[3] = 0.06f;
            }
        }
    }
}

// Cleanly expand a GTA CLinkList<T> post-Initialise without leaving stale sentinel offsets.
// CLink layout (0x20 bytes): +0x00..0x0f payload/sortKey, +0x10 m_pPrev, +0x18 m_pNext.
// CLinkList layout (0x88 bytes): +0x00 head, +0x20 tail, +0x40 freeHead, +0x60 freeTail, +0x80 m_pLinks.
void expand_vis_link_list(lcs::cpu::DynarmicCore& cpu, uint64_t fn_znam, uint64_t fn_zdapv,
                          uint64_t list_va, uint32_t new_cap) {
    if (!list_va || !fn_znam || new_cap < 4u) return;
    const uint64_t old_links = *reinterpret_cast<const uint64_t*>(list_va + 0x80ULL);
    const uint64_t new_bytes = static_cast<uint64_t>(new_cap) * 0x20ULL;
    const uint64_t new_links = cpu.call_nested(fn_znam, {new_bytes});
    if (!new_links) return;

    std::memset(reinterpret_cast<void*>(new_links), 0, static_cast<size_t>(new_bytes));
    if (old_links && fn_zdapv) {
        cpu.call_nested(fn_zdapv, {old_links});
    }

    const uint64_t act_head  = list_va + 0x00ULL;
    const uint64_t act_tail  = list_va + 0x20ULL;
    const uint64_t free_head = list_va + 0x40ULL;
    const uint64_t free_tail = list_va + 0x60ULL;

    *reinterpret_cast<uint64_t*>(act_head + 0x18ULL) = act_tail;
    *reinterpret_cast<uint64_t*>(act_tail + 0x10ULL) = act_head;
    *reinterpret_cast<uint64_t*>(free_head + 0x18ULL) = new_links;
    *reinterpret_cast<uint64_t*>(free_tail + 0x10ULL) =
        new_links + static_cast<uint64_t>(new_cap - 1u) * 0x20ULL;

    for (uint32_t i = 0; i < new_cap; ++i) {
        const uint64_t node = new_links + static_cast<uint64_t>(i) * 0x20ULL;
        const uint64_t prev = (i == 0u) ? free_head : (node - 0x20ULL);
        const uint64_t next = (i + 1u == new_cap) ? free_tail : (node + 0x20ULL);
        *reinterpret_cast<uint64_t*>(node + 0x10ULL) = prev;
        *reinterpret_cast<uint64_t*>(node + 0x18ULL) = next;
    }

    *reinterpret_cast<uint64_t*>(list_va + 0x80ULL) = new_links;
}

} // namespace

struct LcsDistance::HookOps {
    static uint64_t pools_post() {
        LcsDistance::instance().on_pools_initialise_post();
        return 0;
    }
    static uint64_t vis_init_post() {
        LcsDistance::instance().on_vis_init_post();
        return 0;
    }
    static uint64_t cam_post(uint64_t cam_ptr) {
        return LcsDistance::instance().on_camera_process_post(cam_ptr);
    }
    static uint64_t get_lod_post(uint64_t ret_ptr) {
        LcsDistance::instance().on_get_lod_settings_post(ret_ptr);
        return 0;
    }
    static uint64_t far_clip_pre(uint64_t rsl_cam, uint64_t bits) {
        return LcsDistance::instance().on_far_clip_set_pre(rsl_cam, static_cast<uint32_t>(bits));
    }
    static uint64_t big_bldg_pre(uint64_t entity_ptr) {
        return LcsDistance::instance().on_big_building_check(entity_ptr);
    }
    static uint64_t elem_dist_pre(uint64_t model_info, uint64_t dist_bits) {
        return LcsDistance::instance().on_get_elem_from_dist_pre(model_info, static_cast<uint32_t>(dist_bits));
    }
    static uint64_t largest_lod_post(uint64_t model_info, uint64_t ret_bits) {
        return LcsDistance::instance().on_get_largest_lod_post(model_info, static_cast<uint32_t>(ret_bits));
    }
    static uint64_t scan_world_pre() {
        LcsDistance::instance().on_scan_world_pre();
        return 0;
    }
    static uint64_t scan_world_post() {
        LcsDistance::instance().on_scan_world_post();
        return 0;
    }
};

constexpr uint64_t kGotRecordNodeIndexesInCircleRva = 0x804ff0ULL;
constexpr uint64_t kGotGeneratePedCreationCoorsRva  = 0x804ff8ULL;

// `mov w8, #<hi16>, lsl #16` holding `value` (low 16 mantissa bits dropped)
uint32_t mov_w8_float(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return 0x52a00008u | (((bits >> 16) & 0xffffu) << 5);
}

float env_float(const char* name, float fallback) {
    if (const char* e = std::getenv(name); e && *e) return static_cast<float>(std::atof(e));
    return fallback;
}

uint64_t record_nodes_post_sample(uint64_t total_u, uint64_t orig_out_addr, uint64_t orig_max_u, uint64_t tmp_addr) {
    const int32_t count = static_cast<int32_t>(total_u);
    const int32_t orig_max = static_cast<int32_t>(orig_max_u);
    if (count <= 0 || orig_max <= 0 || !orig_out_addr || !tmp_addr) return 0;
    auto* out = reinterpret_cast<uint16_t*>(orig_out_addr);
    const auto* tmp = reinterpret_cast<const uint16_t*>(tmp_addr);
    const int32_t n_copy = std::min(count, orig_max);
    if (count <= orig_max) {
        std::memcpy(out, tmp, static_cast<size_t>(n_copy) * sizeof(uint16_t));
    } else {
        // Uniformly stride-sample across all 360-degree scan sectors so SW cells don't starve NE cells.
        for (int32_t i = 0; i < orig_max; ++i) {
            out[i] = tmp[(static_cast<int64_t>(i) * count) / orig_max];
        }
    }
    return static_cast<uint64_t>(static_cast<uint32_t>(n_copy));
}

bool is_entity_lod_covered_by_hd(uint64_t load_bias, uint64_t entity_ptr) {
    if (!entity_ptr || LcsWorldHd::instance().radius() <= 0) return false;
    const float ex = *reinterpret_cast<const float*>(entity_ptr + 0x40ULL);
    const float ey = *reinterpret_cast<const float*>(entity_ptr + 0x44ULL);

    float wx = ex;
    float wy = ey;
    float bound_r = 75.0f;
    if (load_bias) {
        const auto* num_mi = *reinterpret_cast<const int32_t* const*>(load_bias + 0x7f8fa0ULL);
        const auto* mi_pp  = *reinterpret_cast<const uint64_t* const*>(load_bias + 0x7f8fb0ULL);
        const uint64_t* mi_ptrs = (mi_pp && *mi_pp >= 0x10000ULL)
            ? reinterpret_cast<const uint64_t*>(*mi_pp) : nullptr;
        const int16_t mid = *reinterpret_cast<const int16_t*>(entity_ptr + 0x7cULL);
        if (num_mi && mi_ptrs && mid >= 0 && mid < *num_mi) {
            const uint64_t mi = mi_ptrs[mid];
            if (mi >= 0x10000ULL) {
                const uint64_t col = *reinterpret_cast<const uint64_t*>(mi + 0x30ULL);
                if (col >= 0x10000ULL) {
                    const float csx = *reinterpret_cast<const float*>(col + 0x00ULL);
                    const float csy = *reinterpret_cast<const float*>(col + 0x04ULL);
                    const float r   = *reinterpret_cast<const float*>(col + 0x0cULL);
                    if (std::isfinite(csx) && std::isfinite(csy)) {
                        const float rx = *reinterpret_cast<const float*>(entity_ptr + 0x10ULL);
                        const float ry = *reinterpret_cast<const float*>(entity_ptr + 0x14ULL);
                        const float ux = *reinterpret_cast<const float*>(entity_ptr + 0x20ULL);
                        const float uy = *reinterpret_cast<const float*>(entity_ptr + 0x24ULL);
                        if (std::isfinite(rx) && std::isfinite(ry) && std::isfinite(ux) && std::isfinite(uy)) {
                            wx = ex + csx * rx + csy * ux;
                            wy = ey + csx * ry + csy * uy;
                        } else {
                            wx = ex + csx;
                            wy = ey + csy;
                        }
                    }
                    if (std::isfinite(r) && r > 1.0f) {
                        bound_r = std::clamp(r, 10.0f, 450.0f);
                    }
                }
            }
        }
    }
    return LcsWorldHd::instance().is_circle_covered_by_hd(wx, wy, bound_r);
}

LcsDistance::~LcsDistance() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (m_stub_page) {
        VirtualFree(m_stub_page, 0, MEM_RELEASE);
        m_stub_page = nullptr;
    }
#endif
}

LcsDistance& LcsDistance::instance() {
    static LcsDistance s_instance;
    return s_instance;
}

void LcsDistance::install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    if (m_installed) return;
    m_cpu = &cpu;
    m_load_bias = image_game.get_load_bias();

#if defined(LCS_PLATFORM_WINDOWS)
    m_stub_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    m_stub_page = static_cast<uint8_t*>(std::aligned_alloc(4096, 0x4000));
#endif
    if (!m_stub_page) {
        std::cerr << "  [Distance] WARN: Failed to allocate hook page.\n";
        return;
    }
    std::memset(m_stub_page, 0, 0x4000);

    m_fn_znam = image_game.find_symbol("_Znam");
    if (!m_fn_znam) m_fn_znam = m_load_bias + kSymZnamRva;
    m_fn_zdapv = image_game.find_symbol("_ZdaPv");
    if (!m_fn_zdapv) m_fn_zdapv = m_load_bias + kSymZdaPvRva;

    const uint64_t base_va = reinterpret_cast<uint64_t>(m_stub_page);
    // Dedicated shared data slots in upper 1 KiB of stub_page
    m_va_scaled_lod_struct = base_va + 0x1800ULL;
    m_va_scaled_lod_ready  = base_va + 0x1880ULL; // uint64_t = scaled struct VA when ready, else 0
    m_va_cam_new_far_clip  = base_va + 0x1890ULL; // float = scaled far clip for RslCameraSetFarClipPlane
    m_va_set_far_scaled    = base_va + 0x1898ULL; // float = temp scaled clip in SetFarClipPlane hook
    m_va_inv_detail_scale  = base_va + 0x18a0ULL; // float[6] = {inv_scale, scale, prop_floor, min_raw, max_raw, min_eff}

    {
        const float detail_scale = compute_detail_scale();
        const int detail_dist    = lcs::settings::get().detail_distance;
        auto* sp = reinterpret_cast<float*>(m_va_inv_detail_scale);
        sp[0] = 1.0f / std::max(0.1f, detail_scale);
        sp[1] = detail_scale;
        sp[2] = (detail_dist >= 30) ? map_slider_curve(detail_dist, 55.0f, 160.0f, 550.0f) : 0.0f;
        sp[3] = (detail_dist >= 30) ? 12.0f : 1e9f;
        sp[4] = 300.0f;
        sp[5] = 0.1f;
    }

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t cb_pools_post = hle.register_function("__lcs_dist_pools_post",
        [](const uint64_t*) -> uint64_t { return HookOps::pools_post(); });
    const uint64_t cb_vis_init_post = hle.register_function("__lcs_dist_vis_init_post",
        [](const uint64_t*) -> uint64_t { return HookOps::vis_init_post(); });
    const uint64_t cb_cam_post = hle.register_function("__lcs_dist_cam_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::cam_post(a[0]); });
    const uint64_t cb_get_lod_post = hle.register_function("__lcs_dist_get_lod_post",
        [](const uint64_t* a) -> uint64_t { return HookOps::get_lod_post(a[0]); });
    const uint64_t cb_far_clip_pre = hle.register_function("__lcs_dist_far_clip_pre",
        [](const uint64_t* a) -> uint64_t { return HookOps::far_clip_pre(a[0], a[1]); });
    const uint64_t cb_big_bldg = hle.register_function("__lcs_dist_big_bldg",
        [](const uint64_t* a) -> uint64_t { return HookOps::big_bldg_pre(a[0]); });
    const uint64_t cb_rec_nodes_post = hle.register_function("__lcs_dist_rec_nodes_post",
        [](const uint64_t* a) -> uint64_t { return record_nodes_post_sample(a[0], a[1], a[2], a[3]); });

    size_t cursor = 0;

    // 1. Force GetLODSettingsLevel() and GetCullFadeLevel() -> 2 (High preset)
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        code[0] = 0x52800040u; // mov w0, #2
        code[1] = 0xd65f03c0u; // ret
        patch_got_slot(cpu, m_load_bias, kGotGetLodLevelRva, stub_va, image_game, "_Z19GetLODSettingsLevelv");
        patch_got_slot(cpu, m_load_bias, kGotGetCullFadeLevelRva, stub_va, image_game, "_Z16GetCullFadeLevelv");
        cursor += 0x20ULL;
    }

    // 2. CPools::Initialise() post-hook (expands pools immediately after allocation)
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_pools_init = patch_got_slot(
            cpu, m_load_bias, kGotPoolsInitialiseRva, stub_va, image_game, "_ZN6CPools10InitialiseEv");
        const uint32_t insns[12] = {
            0xd10043ffu, // 0x00: sub sp, sp, #0x10
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            ldr_x16_rel(0x0c, 0x30),
            0xd63f0200u, // 0x10: blr x16 (orig CPools::Initialise)
            ldr_x16_rel(0x14, 0x38),
            0xd63f0200u, // 0x18: blr x16 (pools_post)
            0xa9407bfdu, // 0x1c: ldp x29, x30, [sp]
            0x910043ffu, // 0x20: add sp, sp, #0x10
            0xd65f03c0u, // 0x24: ret
            0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x30ULL);
        lits[0] = m_orig_pools_init;
        lits[1] = cb_pools_post;
        cursor += 0x50ULL;
    }

    // Helper: pure ARM64 tail-call stub for CSimpleModelInfo::GetElementFromDistance / GetLastElement (ZERO HLE exits!)
    auto emit_elem_dist_stub = [&](uint64_t orig_fn) {
        const uint32_t insns[38] = {
            0x580004ccu, 0xbd400182u, 0xf140401fu, 0x540002a3u,
            0x7940ac09u, 0x1200052au, 0x3400024au, 0x721d013fu,
            0x54000040u, 0x3941500au, 0x5100054au, 0x7100095fu,
            0x54000188u, 0x9101200bu, 0xbc6a5961u, 0x1e202028u,
            0x5400010du, 0xbd400583u, 0xbd400d85u, 0x1e252020u,
            0x5400008bu, 0xbd401186u, 0x1e262020u, 0x540000abu,
            0x1e220800u, 0x580001f0u, 0xd61f0200u, 0xd503201fu,
            0xbd400984u, 0xbd401587u, 0x1e230823u, 0x1e244863u,
            0x1e274863u, 0x1e231821u, 0x1e210800u, 0x580000b0u,
            0xd61f0200u, 0xd503201fu,
        };
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x98ULL);
        lits[0] = m_va_inv_detail_scale;
        lits[1] = orig_fn;
        cursor += 0xb0ULL;
    };

    // 3. CSimpleModelInfo::GetElementFromDistance(this [x0], float dist [s0])
    {
        const uint64_t stub_va = base_va + cursor;
        m_orig_get_elem_dist = patch_got_slot(
            cpu, m_load_bias, kGotGetElemFromDistRva, stub_va, image_game,
            "_ZN16CSimpleModelInfo22GetElementFromDistanceEf");
        emit_elem_dist_stub(m_orig_get_elem_dist);
    }

    // 3b. CSimpleModelInfo::GetLargestLodDistance(this [x0]) pure ARM64 stub (ZERO HLE exits!)
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_get_largest_lod = patch_got_slot(
            cpu, m_load_bias, kGotGetLargestLodRva, stub_va, image_game,
            "_ZN16CSimpleModelInfo21GetLargestLodDistanceEv");
        const uint32_t insns[44] = {
            0xd10043ffu, 0xa9007be0u, 0x58000550u, 0xd63f0200u,
            0x1e202008u, 0x5400048du, 0x1e260009u, 0x53177929u,
            0x7103fd3fu, 0x54000400u, 0xf94003e0u, 0xf140401fu,
            0x540003a3u, 0x5800042cu, 0x7940ac09u, 0x1200052au,
            0x3400032au, 0x721d013fu, 0x54000040u, 0x3941500au,
            0x5100054au, 0x7100095fu, 0x54000268u, 0x9101200bu,
            0xbc6a5961u, 0x1e202028u, 0x540001edu, 0xbd400583u,
            0xbd400d85u, 0x1e252020u, 0x5400008bu, 0xbd401186u,
            0x1e262020u, 0x5400006bu, 0x1e230800u, 0x14000006u,
            0xbd400984u, 0x1e230823u, 0x1e244863u, 0x1e211863u,
            0x1e230800u, 0xa9407be0u, 0x910043ffu, 0xd65f03c0u,
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0xb0ULL);
        lits[0] = m_orig_get_largest_lod;
        lits[1] = m_va_inv_detail_scale;
        cursor += 0xc0ULL;
    }

    // 3c. CSimpleModelInfo::GetLastElement(this [x0], float dist [s0]) - must match GetElementFromDistance
    {
        const uint64_t stub_va = base_va + cursor;
        s_orig_get_last_elem = patch_got_slot(
            cpu, m_load_bias, kGotGetLastElemRva, stub_va, image_game,
            "_ZN16CSimpleModelInfo14GetLastElementEf");
        emit_elem_dist_stub(s_orig_get_last_elem);
    }

    // 4. C_DeviceDetailSettings::GetLODSettings(): fast-path returns m_va_scaled_lod_struct once captured
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_get_lod_settings = patch_got_slot(
            cpu, m_load_bias, kGotGetLodSettingsRva, stub_va, image_game,
            "_ZN22C_DeviceDetailSettings14GetLODSettingsEv");
        const uint32_t insns[16] = {
            ldr_x16_rel(0x00, 0x40),
            0xf9400210u, // 0x04: ldr x16, [x16] (ready ptr)
            0xb4000070u, // 0x08: cbz x16, +0xc (0x14 fallback)
            0xaa1003e0u, // 0x0c: mov x0, x16
            0xd65f03c0u, // 0x10: ret
            0xd10083ffu, // 0x14: sub sp, sp, #0x20
            0xa9007bfdu, // 0x18: stp x29, x30, [sp]
            0x910003fdu, // 0x1c: mov x29, sp
            ldr_x16_rel(0x20, 0x48),
            0xd63f0200u, // 0x24: blr x16 (orig GetLODSettings)
            0xf9000be0u, // 0x28: str x0, [sp, #0x10]
            ldr_x16_rel(0x2c, 0x50),
            0xd63f0200u, // 0x30: blr x16 (get_lod_post(orig_ret))
            0xf9400be0u, // 0x34: ldr x0, [sp, #0x10]
            0xa9407bfdu, // 0x38: ldp x29, x30, [sp]
            0x910083ffu, // 0x3c: add sp, sp, #0x20  (overlaps literals if inline -> fix offsets below)
        };
        // Keep clean spacing with literals at 0x50
        const uint32_t insns_clean[20] = {
            ldr_x16_rel(0x00, 0x50),
            0xf9400210u, // 0x04: ldr x16, [x16]
            0xb4000070u, // 0x08: cbz x16, +0xc (0x14)
            0xaa1003e0u, // 0x0c: mov x0, x16
            0xd65f03c0u, // 0x10: ret
            0xd10083ffu, // 0x14: sub sp, sp, #0x20
            0xa9007bfdu, // 0x18: stp x29, x30, [sp]
            0x910003fdu, // 0x1c: mov x29, sp
            ldr_x16_rel(0x20, 0x58),
            0xd63f0200u, // 0x24: blr x16 (orig GetLODSettings)
            0xf9000be0u, // 0x28: str x0, [sp, #0x10]
            ldr_x16_rel(0x2c, 0x60),
            0xd63f0200u, // 0x30: blr x16 (cb_get_lod_post(orig_ret))
            0xf9400be0u, // 0x34: ldr x0, [sp, #0x10]
            0xa9407bfdu, // 0x38: ldp x29, x30, [sp]
            0x910083ffu, // 0x3c: add sp, sp, #0x20
            0xd65f03c0u, // 0x40: ret
            0xd503201fu, 0xd503201fu, 0xd503201fu
        };
        (void)insns;
        std::memcpy(code, insns_clean, sizeof(insns_clean));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x50ULL);
        lits[0] = m_va_scaled_lod_ready;
        lits[1] = m_orig_get_lod_settings;
        lits[2] = cb_get_lod_post;
        cursor += 0x70ULL;
    }

    // 5. RslCameraSetFarClipPlane(RslCamera* [x0], float farClip [s0])
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_set_far_clip = patch_got_slot(
            cpu, m_load_bias, kGotSetFarClipPlaneRva, stub_va, image_game,
            "_Z24RslCameraSetFarClipPlaneP9RslCameraf");
        // Calls cb_far_clip_pre(x0, (uint32_t)w_far_bits), stores float return back into s0, branches to orig.
        const uint32_t insns[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xf9000be0u, // 0x0c: str x0, [sp, #0x10]
            0x1e260001u, // 0x10: fmov w1, s0
            ldr_x16_rel(0x14, 0x40),
            0xd63f0200u, // 0x18: blr x16 (cb_far_clip_pre)
            0x1e270000u, // 0x1c: fmov s0, w0
            0xf9400be0u, // 0x20: ldr x0, [sp, #0x10]
            0xa9407bfdu, // 0x24: ldp x29, x30, [sp]
            0x910083ffu, // 0x28: add sp, sp, #0x20
            ldr_x16_rel(0x2c, 0x48),
            0xd61f0200u, // 0x30: br x16 (orig RslCameraSetFarClipPlane)
            0xd503201fu, 0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x40ULL);
        lits[0] = cb_far_clip_pre;
        lits[1] = m_orig_set_far_clip;
        cursor += 0x60ULL;
    }

    // 6. CCamera::Process() post-hook: scales LODDistScale / ms_fLODDistance / ms_fFarClipZ & updates RslCamera
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_cam_process = patch_got_slot(
            cpu, m_load_bias, kGotCameraProcessRva, stub_va, image_game, "_ZN7CCamera7ProcessEv");
        const uint32_t insns[20] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xf9000be0u, // 0x0c: str x0, [sp, #0x10] (save cam_ptr)
            ldr_x16_rel(0x10, 0x50),
            0xd63f0200u, // 0x14: blr x16 (orig CCamera::Process)
            0xf9400be0u, // 0x18: ldr x0, [sp, #0x10]
            ldr_x16_rel(0x1c, 0x58),
            0xd63f0200u, // 0x20: blr x16 (cb_cam_post(cam) -> returns rsl_cam if far clip needs update, else 0)
            0xb40000a0u, // 0x24: cbz x0, +0x14 (0x38)
            ldr_x16_rel(0x28, 0x60),
            0xbd400200u, // 0x2c: ldr s0, [x16] (*m_va_cam_new_far_clip)
            ldr_x16_rel(0x30, 0x68),
            0xd63f0200u, // 0x34: blr x16 (orig RslCameraSetFarClipPlane)
            0xa9407bfdu, // 0x38: ldp x29, x30, [sp]
            0x910083ffu, // 0x3c: add sp, sp, #0x20
            0xd65f03c0u, // 0x40: ret
            0xd503201fu, 0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x50ULL);
        lits[0] = m_orig_cam_process;
        lits[1] = cb_cam_post;
        lits[2] = m_va_cam_new_far_clip;
        lits[3] = m_orig_set_far_clip;
        cursor += 0x70ULL;
    }

    // 7. CRenderer::SetupBigBuildingVisibility(CEntity*) wrapper (hides overlapping blurry LOD* twin)
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_setup_big_bldg = patch_got_slot(
            cpu, m_load_bias, kGotSetupBigBldgRva, stub_va, image_game,
            "_ZN9CRenderer26SetupBigBuildingVisibilityEP7CEntity");
        const uint32_t insns[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xf9000be0u, // 0x0c: str x0, [sp, #0x10]
            ldr_x16_rel(0x10, 0x40),
            0xd63f0200u, // 0x14: blr x16 (cb_big_bldg(entity))
            0x340000a0u, // 0x18: cbz w0, +0x14 (0x2c)
            0x2a1f03e0u, // 0x1c: mov w0, wzr (hide LOD twin)
            0xa9407bfdu, // 0x20: ldp x29, x30, [sp]
            0x910083ffu, // 0x24: add sp, sp, #0x20
            0xd65f03c0u, // 0x28: ret
            0xf9400be0u, // 0x2c: ldr x0, [sp, #0x10]
            0xa9407bfdu, // 0x30: ldp x29, x30, [sp]
            0x910083ffu, // 0x34: add sp, sp, #0x20
            ldr_x16_rel(0x38, 0x48),
            0xd61f0200u  // 0x3c: br x16 (orig SetupBigBuildingVisibility)
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x40ULL);
        lits[0] = cb_big_bldg;
        lits[1] = m_orig_setup_big_bldg;
        cursor += 0x50ULL;
    }

    // 8. CVisibilityComponents::Initialise() post-hook (cleanly expands m_alphaList & m_alphaEntityList)
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_vis_init = patch_got_slot(
            cpu, m_load_bias, kGotVisInitRva, stub_va, image_game,
            "_ZN21CVisibilityComponents10InitialiseEv");
        const uint32_t insns[12] = {
            0xd10043ffu, // 0x00: sub sp, sp, #0x10
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            ldr_x16_rel(0x0c, 0x30),
            0xd63f0200u, // 0x10: blr x16 (orig CVisibilityComponents::Initialise)
            ldr_x16_rel(0x14, 0x38),
            0xd63f0200u, // 0x18: blr x16 (cb_vis_init_post)
            0xa9407bfdu, // 0x1c: ldp x29, x30, [sp]
            0x910043ffu, // 0x20: add sp, sp, #0x10
            0xd65f03c0u, // 0x24: ret
            0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x30ULL);
        lits[0] = m_orig_vis_init;
        lits[1] = cb_vis_init_post;
        cursor += 0x48ULL;
    }

    // 9. cWorldStream::pDynamic(ws [x0], mid [w1], lod [w2]) zero-trap overflow guard:
    // if m_numDynamics ([x0 + 0x2360]) >= 295, return nullptr so SetupEntityVisibility falls back
    // seamlessly to standard CSimpleModelInfo/C_Mesh rendering instead of silently dropping overflow props.
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        m_orig_ws_pdyn = patch_got_slot(
            cpu, m_load_bias, kGotWsPDynamicRva, stub_va, image_game,
            "_ZNK12cWorldStream8pDynamicEii");
        const uint32_t insns[8] = {
            0xb40000a0u, // 0x00: cbz x0, +0x14 (0x14 return nullptr)
            0xb9636010u, // 0x04: ldr w16, [x0, #0x2360] (m_numDynamics)
            0x71049e1fu, // 0x08: cmp w16, #295 (0x127)
            0x5400008au, // 0x0c: b.ge +0x10 (0x1c return nullptr? wait: +0x10 from 0x0c is 0x1c; +0x0c is 0x18!)
            ldr_x16_rel(0x10, 0x28),
            0xd61f0200u, // 0x14: br x16 (orig cWorldStream::pDynamic)
            0xaa1f03e0u, // 0x18: mov x0, xzr
            0xd65f03c0u  // 0x1c: ret
        };
        // Fix branch offsets carefully: 0x00 -> 0x18 (+0x18 = 6 words -> (6<<5)=0xc0 -> 0xb40000c0);
        // 0x0c -> 0x18 (+0x0c = 3 words -> (3<<5)|0xa = 0x5400006a)
        const uint32_t insns_ok[8] = {
            0xb40000c0u, // 0x00: cbz x0, 0x18
            0xb9636010u, // 0x04: ldr w16, [x0, #0x2360]
            0x71049e1fu, // 0x08: cmp w16, #295 (0x127)
            0x5400006au, // 0x0c: b.ge 0x18
            ldr_x16_rel(0x10, 0x28),
            0xd61f0200u, // 0x14: br x16 (orig pDynamic)
            0xaa1f03e0u, // 0x18: mov x0, xzr
            0xd65f03c0u  // 0x1c: ret
        };
        (void)insns;
        std::memcpy(code, insns_ok, sizeof(insns_ok));
        *reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x28ULL) = m_orig_ws_pdyn;
        cursor += 0x38ULL;
    }

    // 10. Widen CRenderer::ScanWorld() 2D sector scan cone horizontally by 1.35x so large trees/facades/props
    // straddling frustum edges still run SetupEntityVisibility instead of popping in when origins cross the viewport.
    {
        const uint64_t cb_scan_pre  = hle.register_function("__lcs_dist_scan_pre",
            [](const uint64_t*) -> uint64_t { return HookOps::scan_world_pre(); });
        const uint64_t cb_scan_post = hle.register_function("__lcs_dist_scan_post",
            [](const uint64_t*) -> uint64_t { return HookOps::scan_world_post(); });
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        const uint64_t orig_scan = patch_got_slot(
            cpu, m_load_bias, kGotScanWorldRva, stub_va, image_game,
            "_ZN9CRenderer9ScanWorldEv");
        const uint32_t insns[16] = {
            0xd10043ffu, // 0x00: sub sp, sp, #0x10
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            ldr_x16_rel(0x0c, 0x40),
            0xd63f0200u, // 0x10: blr x16 (cb_scan_pre)
            ldr_x16_rel(0x14, 0x48),
            0xd63f0200u, // 0x18: blr x16 (orig CRenderer::ScanWorld)
            ldr_x16_rel(0x1c, 0x50),
            0xd63f0200u, // 0x20: blr x16 (cb_scan_post)
            0xa9407bfdu, // 0x24: ldp x29, x30, [sp]
            0x910043ffu, // 0x28: add sp, sp, #0x10
            0xd65f03c0u, // 0x2c: ret
            0xd503201fu, 0xd503201fu, 0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns, sizeof(insns));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x40ULL);
        lits[0] = cb_scan_pre;
        lits[1] = orig_scan;
        lits[2] = cb_scan_post;
        cursor += 0x60ULL;
    }

    // 11. Remove single-item-per-frame upload bottleneck in lglWorldCreator/ModelCreator/BufferCreator::createReady.
    // On mobile, createReady() stopped after 1 mesh/model/buffer per frame, starving newly streamed cells and props.
    {
        const uint64_t wc_va = m_load_bias + kPatchWorldCreatorBreakRva;
        if (*reinterpret_cast<const uint32_t*>(wc_va) == 0x37000815u) {
            const uint32_t kNop = 0xd503201fu;
            patch_mem(cpu, wc_va, &kNop, sizeof(uint32_t));
        }

        const uint64_t asp_va = m_load_bias + kPatchCalcAspectFcselRva;
        if (*reinterpret_cast<const uint32_t*>(asp_va) == 0x1e210c00u) {
            const uint32_t kNop = 0xd503201fu;
            patch_mem(cpu, asp_va, &kNop, sizeof(uint32_t));
        }

        // Batch lglModelCreator::createReady(void* self) up to 64 items per frame
        {
            const uint64_t stub_va = base_va + cursor;
            auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
            const uint64_t orig_mc = patch_got_slot(
                cpu, m_load_bias, kGotModelCreatorReadyRva, stub_va, image_game,
                "_ZN15lglModelCreator11createReadyEv");
            const uint32_t insns[24] = {
                0xd100c3ffu, // 0x00: sub sp, sp, #0x30
                0xa9007bfdu, // 0x04: stp x29, x30, [sp]
                0x910003fdu, // 0x08: mov x29, sp
                0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
                0xf90013f5u, // 0x10: str x21, [sp, #0x20]
                0xaa0003f3u, // 0x14: mov x19, x0
                0x2a1f03f4u, // 0x18: mov w20, wzr
                0x52800815u, // 0x1c: mov w21, #64
                // loop (0x20):
                0xaa1303e0u, // 0x20: mov x0, x19
                ldr_x16_rel(0x24, 0x60),
                0xd63f0200u, // 0x28: blr x16 (orig)
                0x2a000294u, // 0x2c: orr w20, w20, w0
                0x34000060u, // 0x30: cbz w0, 0x3c
                0x710006b5u, // 0x34: subs w21, w21, #1
                0x54ffff4cu, // 0x38: b.gt 0x20
                // done (0x3c):
                0x2a1403e0u, // 0x3c: mov w0, w20
                0xf94013f5u, // 0x40: ldr x21, [sp, #0x20]
                0xa94153f3u, // 0x44: ldp x19, x20, [sp, #0x10]
                0xa9407bfdu, // 0x48: ldp x29, x30, [sp]
                0x9100c3ffu, // 0x4c: add sp, sp, #0x30
                0xd65f03c0u, // 0x50: ret
                0xd503201fu, 0xd503201fu, 0xd503201fu
            };
            std::memcpy(code, insns, sizeof(insns));
            *reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x60ULL) = orig_mc;
            cursor += 0x70ULL;
        }

        // Batch lglBufferCreator::createReady(void* self, int& a, int& b, bool c) up to 128 buffers per frame
        {
            const uint64_t stub_va = base_va + cursor;
            auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
            const uint64_t orig_bc = patch_got_slot(
                cpu, m_load_bias, kGotBufferCreatorReadyRva, stub_va, image_game,
                "_ZN16lglBufferCreator11createReadyERiS0_b");
            const uint32_t insns[30] = {
                0xd10103ffu, // 0x00: sub sp, sp, #0x40
                0xa9007bfdu, // 0x04: stp x29, x30, [sp]
                0x910003fdu, // 0x08: mov x29, sp
                0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
                0xa9025bf5u, // 0x10: stp x21, x22, [sp, #0x20]
                0xa90363f7u, // 0x14: stp x23, x24, [sp, #0x30]
                0xaa0003f3u, // 0x18: mov x19, x0
                0xaa0103f4u, // 0x1c: mov x20, x1
                0xaa0203f5u, // 0x20: mov x21, x2
                0xaa0303f6u, // 0x24: mov x22, x3
                0x2a1f03f7u, // 0x28: mov w23, wzr
                0x52801018u, // 0x2c: mov w24, #128
                // loop (0x30):
                0xaa1303e0u, // 0x30: mov x0, x19
                0xaa1403e1u, // 0x34: mov x1, x20
                0xaa1503e2u, // 0x38: mov x2, x21
                0xaa1603e3u, // 0x3c: mov x3, x22
                ldr_x16_rel(0x40, 0x80),
                0xd63f0200u, // 0x44: blr x16 (orig)
                0x2a0002f7u, // 0x48: orr w23, w23, w0
                0x34000060u, // 0x4c: cbz w0, 0x58
                0x71000718u, // 0x50: subs w24, w24, #1
                0x54fffeecu, // 0x54: b.gt 0x30 (-9 words)
                // done (0x58):
                0x2a1703e0u, // 0x58: mov w0, w23
                0xa94363f7u, // 0x5c: ldp x23, x24, [sp, #0x30]
                0xa9425bf5u, // 0x60: ldp x21, x22, [sp, #0x20]
                0xa94153f3u, // 0x64: ldp x19, x20, [sp, #0x10]
                0xa9407bfdu, // 0x68: ldp x29, x30, [sp]
                0x910103ffu, // 0x6c: add sp, sp, #0x40
                0xd65f03c0u, // 0x70: ret
                0xd503201fu
            };
            std::memcpy(code, insns, sizeof(insns));
            *reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x80ULL) = orig_bc;
            cursor += 0x90ULL;
        }
    }

    // 11. CPathFind::RecordNodeIndexesInCircle wrapper: sample up to 2048 nodes across full 360-deg circle,
    // then evenly stride-downsample into caller's 250-slot buffer so SW road corners never starve NE.
    {
        const uint64_t stub_va = base_va + cursor;
        const uint64_t tmp_nodes_va = base_va + 0x3000ULL;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        const uint64_t orig_rec_nodes = patch_got_slot(
            cpu, m_load_bias, kGotRecordNodeIndexesInCircleRva, stub_va, image_game,
            "_ZN9CPathFind25RecordNodeIndexesInCircleERK7CVectorfhiPtbbbb");
        if (orig_rec_nodes) {
            auto ldr_reg_rel = [](uint32_t rt, uint32_t at_off, uint32_t lit_off) -> uint32_t {
                const uint32_t imm19 = (lit_off - at_off) >> 2;
                return 0x58000000u | ((imm19 & 0x7ffffu) << 5) | (rt & 31u);
            };
            const uint32_t insns[28] = {
                0x7101007fu,                   // 0x00: cmp w3, #0x40 (64)
                0x54000303u,                   // 0x04: b.lo 0x64
                0x7108007fu,                   // 0x08: cmp w3, #0x200 (512)
                0x540002c8u,                   // 0x0c: b.hi 0x64
                0xb40002a4u,                   // 0x10: cbz x4, 0x64
                0xd100c3ffu,                   // 0x14: sub sp, sp, #0x30
                0xa9027bfdu,                   // 0x18: stp x29, x30, [sp, #0x20]
                0xa90153f3u,                   // 0x1c: stp x19, x20, [sp, #0x10]
                0x3940c3e8u,                   // 0x20: ldrb w8, [sp, #0x30]
                0x390003e8u,                   // 0x24: strb w8, [sp]
                0xaa0403f3u,                   // 0x28: mov x19, x4
                0x2a0303f4u,                   // 0x2c: mov w20, w3
                0x52810003u,                   // 0x30: mov w3, #0x800 (2048)
                ldr_reg_rel(4, 0x34, 0x70),    // 0x34: ldr x4, tmp_nodes_va
                ldr_reg_rel(16, 0x38, 0x78),   // 0x38: ldr x16, orig_rec_nodes
                0xd63f0200u,                   // 0x3c: blr x16
                0xaa1303e1u,                   // 0x40: mov x1, x19
                0x2a1403e2u,                   // 0x44: mov w2, w20
                ldr_reg_rel(3, 0x48, 0x70),    // 0x48: ldr x3, tmp_nodes_va
                ldr_reg_rel(16, 0x4c, 0x80),   // 0x4c: ldr x16, cb_rec_nodes_post
                0xd63f0200u,                   // 0x50: blr x16
                0xa94153f3u,                   // 0x54: ldp x19, x20, [sp, #0x10]
                0xa9427bfdu,                   // 0x58: ldp x29, x30, [sp, #0x20]
                0x9100c3ffu,                   // 0x5c: add sp, sp, #0x30
                0xd65f03c0u,                   // 0x60: ret
                ldr_reg_rel(16, 0x64, 0x78),   // 0x64: ldr x16, orig_rec_nodes
                0xd61f0200u,                   // 0x68: br x16
                0xd503201fu                    // 0x6c: nop
            };
            std::memcpy(code, insns, sizeof(insns));
            auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x70ULL);
            lits[0] = tmp_nodes_va;
            lits[1] = orig_rec_nodes;
            lits[2] = cb_rec_nodes_post;
            cursor += 0x90ULL;
        }
    }

    // 12. CPathFind::GeneratePedCreationCoors(x, y, minDist, maxDist, minDistOffScreen, maxDistOffScreen, ...):
    //     floor the in-view range (s2/s3) so peds never materialise in plain sight near the player;
    //     the off-screen range (s4/s5, behind the camera) is left to the game.
    {
#if defined(LCS_PLATFORM_WINDOWS)
        auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
        auto* page = static_cast<uint8_t*>(std::aligned_alloc(4096, 0x1000));
#endif
        if (page) {
            std::memset(page, 0, 0x1000);
            const uint64_t page_va = reinterpret_cast<uint64_t>(page);
            const uint64_t stub_va = page_va + 0x20ULL;
            const uint64_t orig = patch_got_slot(cpu, m_load_bias, kGotGeneratePedCreationCoorsRva, stub_va, image_game,
                                                 "_ZN9CPathFind24GeneratePedCreationCoorsEffffffP7CVectorPiS2_PfP7CMatrix");
            if (orig) {
                *reinterpret_cast<uint64_t*>(page) = orig;
                static constexpr uint32_t kCode[] = {
                    0x10FFFF10u, // adr  x16, page (stub - 0x20)
                    0xBD400A10u, // ldr  s16, [x16, #8]     in-view min floor
                    0x1E304842u, // fmax s2, s2, s16
                    0xBD400E11u, // ldr  s17, [x16, #12]    in-view span
                    0x1E312851u, // fadd s17, s2, s17
                    0x1E314863u, // fmax s3, s3, s17
                    0xF9400210u, // ldr  x16, [x16]         original
                    0xD61F0200u, // br   x16
                };
                std::memcpy(page + 0x20, kCode, sizeof(kCode));
                cpu.invalidate_cache(page_va, 0x40);
                m_ped_inview_floor = reinterpret_cast<float*>(page + 0x08);
                m_ped_inview_floor[0] = 0.0f;  // set by sync_spawn_distances()
                m_ped_inview_floor[1] = 0.0f;
            }
        }
    }

    // [TMP_PHASE_PROBE] Sub-frame phase profiler active ONLY under LCS_DIAG_AUDIT=1
    if (std::getenv("LCS_DIAG_AUDIT") != nullptr) {
        struct ProbeSpec { uint64_t got_rva; const char* name; };
        static constexpr ProbeSpec kProbes[] = {
            /* 0*/ {0x802b30ULL, "CGame::Process"},
            /* 1*/ {0x8036a0ULL, "ScanWorld"},
            /* 2*/ {0x8036f0ULL, "RenderRoads"},
            /* 3*/ {0x803728ULL, "RenderBarRoads"},
            /* 4*/ {0x803760ULL, "RenderVehicles"},
            /* 5*/ {0x807bb8ULL, "RenderScene"},
            /* 6*/ {0x8040c8ULL, "Water"},
            /* 7*/ {0x8040e0ULL, "TransWater"},
            /* 8*/ {0x806c78ULL, "StoredShadows"},
            /* 9*/ {0x806ca0ULL, "StaticShadows"},
            /*10*/ {0x806bc0ULL, "Particles"},
            /*11*/ {0x807bc0ULL, "RenderEffects"},
            /*12*/ {0x807c00ULL, "Render2dStuff"},
            /*13*/ {0x804ea8ULL, "DrawFonts"},
            /*14*/ {0x802b50ULL, "StreamingUpd"},
            /*15*/ {0x804978ULL, "GenOneCar"},
            /*16*/ {0x801970ULL, "CWorld::Proc"},
            /*17*/ {0x802c98ULL, "RemCars"},
            /*18*/ {0x8029b8ULL, "Scripts"},
            /*19*/ {0x802bd8ULL, "PopUpd"},
            /*20*/ {0x802bd0ULL, "PartUpd"},
            /*21*/ {0x802c88ULL, "GenCars"},
            /*22*/ {0x802b40ULL, "MenuProc"},
            /*23*/ {0x807bc8ULL, "Resolve"},
            /*24*/ {0x807bd8ULL, "SetTarget"},
            /*25*/ {0x807be8ULL, "PassThrough"},
            /*26*/ {0x8040a0ULL, "WS_Render"},
            /*27*/ {0x8040a8ULL, "DefWorld"},
            /*28*/ {0x803790ULL, "FirstPersVeh"},
            /*29*/ {0x807000ULL, "Rain"},
            /*30*/ {0x806c18ULL, "Fog"},
            /*31*/ {0x806830ULL, "Moving"},
            /*32*/ {0x806780ULL, "Coronas"},
            /*33*/ {0x806798ULL, "SunRefl"},
            /*34*/ {0x8069d0ULL, "Glass"},
            /*35*/ {0x806cf0ULL, "SpecialFX"},
            /*36*/ {0x806c30ULL, "Rubbish"},
            /*37*/ {0x806cc0ULL, "Skidmarks"},
            /*38*/ {0x804068ULL, "MattScene"},
            /*39*/ {0x806df8ULL, "ProcChan"},
            /*40*/ {0x806e20ULL, "LoadBB"},
            /*41*/ {0x800938ULL, "LoadAll"},
            /*42*/ {0x806e70ULL, "ConvObj"},
            /*43*/ {0x806e28ULL, "ReqMod"},
            /*44*/ {0x8044b8ULL, "WS_LoadLvl"},
            /*45*/ {0x8044f0ULL, "WS_Upd"},
            /*46*/ {0x8097e8ULL, "lglLoad"},
            /*47*/ {0x8097f0ULL, "lglUplTask"},
            /*48*/ {0x809ec0ULL, "uplPVR"},
        };
        constexpr size_t kNumProbes = sizeof(kProbes) / sizeof(kProbes[0]);
        struct ProbeStat {
            std::atomic<uint64_t> ns{0};
            std::atomic<uint64_t> frame_ns{0};
            std::atomic<uint32_t> calls{0};
            std::atomic<uint32_t> frame_calls{0};
        };
        static ProbeStat s_stats[kNumProbes]{};
        static uint64_t s_load_bias_probe = m_load_bias;
        static uint64_t s_last_veh_ptrs[160]{};
        static int32_t  s_last_veh_max = 0;

        auto probe_event = +[](uint32_t id, bool is_exit) {
            thread_local std::chrono::steady_clock::time_point tl_t0[kNumProbes]{};
            thread_local int tl_depth[kNumProbes]{};
            if (id >= kNumProbes) return;
            if (!is_exit) {
                if (id == 0) {
                    for (auto& p : s_stats) {
                        p.frame_ns.store(0, std::memory_order_relaxed);
                        p.frame_calls.store(0, std::memory_order_relaxed);
                    }
                }
                if (tl_depth[id]++ == 0) {
                    tl_t0[id] = std::chrono::steady_clock::now();
                }
            } else if (tl_depth[id] > 0) {
                if (--tl_depth[id] == 0) {
                    const uint64_t dt = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - tl_t0[id]).count());
                    s_stats[id].ns.fetch_add(dt, std::memory_order_relaxed);
                    s_stats[id].frame_ns.fetch_add(dt, std::memory_order_relaxed);
                }
                s_stats[id].calls.fetch_add(1, std::memory_order_relaxed);
                s_stats[id].frame_calls.fetch_add(1, std::memory_order_relaxed);
            }
        };
        static void(*s_probe_event)(uint32_t, bool) = nullptr;
        s_probe_event = probe_event;

        static void* s_probe_page = nullptr;
        if (!s_probe_page) {
#if defined(LCS_PLATFORM_WINDOWS)
            s_probe_page = VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
            s_probe_page = std::aligned_alloc(4096, 0x4000);
#endif
            if (s_probe_page) std::memset(s_probe_page, 0, 0x4000);
        }
        auto* probe_mem = static_cast<uint8_t*>(s_probe_page);
        const uint64_t probe_base_va = reinterpret_cast<uint64_t>(probe_mem);
        size_t pcursor = 0;

        const uint64_t cb_enter = hle.register_function("__lcs_tmp_probe_enter",
            [](const uint64_t* a) -> uint64_t {
                const uint32_t id = static_cast<uint32_t>(a[0]);
                if (s_probe_event) s_probe_event(id, false);
                if (id == 15 && s_load_bias_probe) { // GenOneCar pre: snapshot active VehiclePool
                    const auto* vp = *reinterpret_cast<const uint64_t* const*>(s_load_bias_probe + 0x7f9720ULL);
                    const uint64_t pool = vp ? *vp : 0;
                    if (pool) {
                        const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool);
                        const uint64_t flags = *reinterpret_cast<const uint64_t*>(pool + 8ULL);
                        const int32_t sz = std::clamp(*reinterpret_cast<const int32_t*>(pool + 0x10ULL), 0, 160);
                        s_last_veh_max = sz;
                        for (int32_t i = 0; i < sz; ++i) {
                            const uint8_t f = *reinterpret_cast<const uint8_t*>(flags + i);
                            s_last_veh_ptrs[i] = ((f & 0x80u) == 0) ? (objs + static_cast<uint64_t>(i) * 0x7b0ULL) : 0;
                        }
                    }
                }
                return 0;
            });
        const uint64_t cb_exit = hle.register_function("__lcs_tmp_probe_exit",
            [](const uint64_t* a) -> uint64_t {
                const uint32_t id = static_cast<uint32_t>(a[0]);
                if (s_probe_event) s_probe_event(id, true);
                if (id == 15 && s_load_bias_probe) { // GenOneCar post: log new spawn distance
                    const auto* vp = *reinterpret_cast<const uint64_t* const*>(s_load_bias_probe + 0x7f9720ULL);
                    const uint64_t pool = vp ? *vp : 0;
                    const uint64_t cam = *reinterpret_cast<const uint64_t*>(s_load_bias_probe + kGotCameraRva);
                    if (pool && cam) {
                        const float cx = *reinterpret_cast<const float*>(cam + 0x30ULL);
                        const float cy = *reinterpret_cast<const float*>(cam + 0x34ULL);
                        const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool);
                        const uint64_t flags = *reinterpret_cast<const uint64_t*>(pool + 8ULL);
                        const int32_t sz = std::clamp(*reinterpret_cast<const int32_t*>(pool + 0x10ULL), 0, s_last_veh_max);
                        static int s_spawn_logs = 0;
                        for (int32_t i = 0; i < sz; ++i) {
                            const uint8_t f = *reinterpret_cast<const uint8_t*>(flags + i);
                            const uint64_t v = ((f & 0x80u) == 0) ? (objs + static_cast<uint64_t>(i) * 0x7b0ULL) : 0;
                            if (v && s_last_veh_ptrs[i] == 0) {
                                const float vx = *reinterpret_cast<const float*>(v + 0x40ULL);
                                const float vy = *reinterpret_cast<const float*>(v + 0x44ULL);
                                const float d = std::sqrt((vx - cx) * (vx - cx) + (vy - cy) * (vy - cy));
                                if (d < 95.0f || ++s_spawn_logs <= 20) {
                                    std::cout << "  [TMP_CAR_SPAWN] d=" << d << " cam=(" << cx << "," << cy
                                              << ") veh=(" << vx << "," << vy << ") close=" << (d < 90.0f ? 1 : 0) << "\n";
                                }
                            }
                        }
                    }
                }
                if (id == 12) { // Render2dStuff exit ~ end of 3D+2D frame
                    static int s_pframes = 0;
                    ++s_pframes;
                    auto f_ms = [](size_t k) { return static_cast<double>(s_stats[k].frame_ns.load(std::memory_order_relaxed)) / 1e6; };
                    auto a_ms = [](size_t k, int pf) { return static_cast<double>(s_stats[k].ns.load(std::memory_order_relaxed)) / 1e6 / pf; };
                    const double game_ms = f_ms(0), str_ms = f_ms(14), eff_ms = f_ms(11), scn_ms = f_ms(5);
                    if (game_ms > 20.0 || eff_ms > 3.5 || str_ms > 4.0 || (s_pframes % 240) == 0) {
                        std::cout << "  [TMP_PHASES] pf=" << s_pframes;
                        for (size_t k = 0; k < 23; ++k) {
                            std::cout << " " << kProbes[k].name << "=" << f_ms(k) << "(avg=" << a_ms(k, s_pframes) << ")";
                        }
                        std::cout << "\n";
                        if (eff_ms > 2.5 || str_ms > 3.0 || scn_ms > 5.0 || (s_pframes % 240) == 0) {
                            std::cout << "  [TMP_PHASES_SUB] pf=" << s_pframes;
                            for (size_t k = 23; k < kNumProbes; ++k) {
                                const uint32_t fc = s_stats[k].frame_calls.load(std::memory_order_relaxed);
                                std::cout << " " << kProbes[k].name << "=" << f_ms(k)
                                          << "(c=" << fc << ",avg=" << a_ms(k, s_pframes) << ")";
                            }
                            std::cout << "\n";
                        }
                        std::cout << std::flush;
                    }
                }
                return 0;
            });

        auto ldr_reg_rel = [](uint32_t rt, uint32_t at_off, uint32_t lit_off) -> uint32_t {
            const uint32_t imm19 = (lit_off - at_off) >> 2;
            return 0x58000000u | ((imm19 & 0x7ffffu) << 5) | (rt & 31u);
        };
        for (size_t i = 0; probe_mem && i < kNumProbes; ++i) {
            if (pcursor + 0xa0ULL > 0x4000ULL) {
                std::cerr << "  [TMP_PHASE_PROBE] stub cursor overflow at i=" << i << "\n";
                break;
            }
            const uint64_t stub_va = probe_base_va + pcursor;
            auto* code = reinterpret_cast<uint32_t*>(probe_mem + pcursor);
            const uint64_t orig = patch_got_slot(cpu, m_load_bias, kProbes[i].got_rva, stub_va, image_game, kProbes[i].name);
            if (!orig) continue;
            const uint32_t mov_w0_id = 0x52800000u | (static_cast<uint32_t>(i) << 5);
            const uint32_t abi_insns[32] = {
                0xd10203ffu,                 // 0x00: sub sp, sp, #0x80
                0xa9077bfdu,                 // 0x04: stp x29, x30, [sp, #0x70]
                0xa90653f3u,                 // 0x08: stp x19, x20, [sp, #0x60]
                0xa90007e0u,                 // 0x0c: stp x0, x1, [sp, #0x00]
                0xa9010fe2u,                 // 0x10: stp x2, x3, [sp, #0x10]
                0xa90217e4u,                 // 0x14: stp x4, x5, [sp, #0x20]
                0xa9031fe6u,                 // 0x18: stp x6, x7, [sp, #0x30]
                0x6d0407e0u,                 // 0x1c: stp d0, d1, [sp, #0x40]
                0x6d050fe2u,                 // 0x20: stp d2, d3, [sp, #0x50]
                mov_w0_id,                   // 0x24: mov w0, #i
                ldr_reg_rel(16, 0x28, 0x80), // 0x28: ldr x16, cb_enter
                0xd63f0200u,                 // 0x2c: blr x16
                0xa94007e0u,                 // 0x30: ldp x0, x1, [sp, #0x00]
                0xa9410fe2u,                 // 0x34: ldp x2, x3, [sp, #0x10]
                0xa94217e4u,                 // 0x38: ldp x4, x5, [sp, #0x20]
                0xa9431fe6u,                 // 0x3c: ldp x6, x7, [sp, #0x30]
                0x6d4407e0u,                 // 0x40: ldp d0, d1, [sp, #0x40]
                0x6d450fe2u,                 // 0x44: ldp d2, d3, [sp, #0x50]
                ldr_reg_rel(16, 0x48, 0x88), // 0x48: ldr x16, orig
                0xd63f0200u,                 // 0x4c: blr x16
                0xa90007e0u,                 // 0x50: stp x0, x1, [sp, #0x00]
                0x6d0407e0u,                 // 0x54: stp d0, d1, [sp, #0x40]
                mov_w0_id,                   // 0x58: mov w0, #i
                ldr_reg_rel(16, 0x5c, 0x90), // 0x5c: ldr x16, cb_exit
                0xd63f0200u,                 // 0x60: blr x16
                0xa94007e0u,                 // 0x64: ldp x0, x1, [sp, #0x00]
                0x6d4407e0u,                 // 0x68: ldp d0, d1, [sp, #0x40]
                0xa94653f3u,                 // 0x6c: ldp x19, x20, [sp, #0x60]
                0xa9477bfdu,                 // 0x70: ldp x29, x30, [sp, #0x70]
                0x910203ffu,                 // 0x74: add sp, sp, #0x80
                0xd65f03c0u,                 // 0x78: ret
                0xd503201fu                  // 0x7c: nop
            };
            std::memcpy(code, abi_insns, sizeof(abi_insns));
            auto* lits = reinterpret_cast<uint64_t*>(probe_mem + pcursor + 0x80ULL);
            lits[0] = cb_enter;
            lits[1] = orig;
            lits[2] = cb_exit;
            pcursor += 0xa0ULL;
        }
        if (probe_base_va) cpu.invalidate_cache(probe_base_va, 0x4000);
    }


    cpu.invalidate_cache(base_va, 0x4000);
    m_installed = true;
    update_330m_patches(compute_view_scale());
    sync_spawn_distances();
    apply_cull_fade_overrides(m_load_bias, compute_detail_scale(), lcs::settings::get().detail_distance);
    std::cout << "  [Distance] Installed view/detail/population/pool hooks (330m & 100m dyn clamps lifted, CullFade unlocked).\n";
}

void LcsDistance::on_pools_initialise_post() {
    auto* cur_cpu = lcs::cpu::DynarmicCore::current();
    auto* cpu = cur_cpu ? cur_cpu : m_cpu;
    if (!cpu || m_pools_expanded || !m_fn_znam) return;

    struct PoolSpec {
        const char* name;
        uint64_t got_rva;
        size_t elem_size;
        int32_t new_count;
    };
    const PoolSpec specs[] = {
        {"PedPool",           kGotPedPoolRva,           0xdc0ULL, 140},
        {"VehiclePool",       kGotVehiclePoolRva,       0x7b0ULL, 140},
        {"ObjectPool",        kGotObjectPoolRva,        0x270ULL, 950},
        {"PtrNodePool",       kGotPtrNodePoolRva,       0x18ULL,  60000},
        {"EntryInfoNodePool", kGotEntryInfoNodePoolRva, 0x28ULL,  8000},
        {"DummyPool",         kGotDummyPoolRva,         0x90ULL,  6000},
    };

    int expanded = 0;
    for (const auto& s : specs) {
        auto** pp = deref_got<GuestPool*>(m_load_bias, s.got_rva);
        if (!pp || !*pp) continue;
        GuestPool* pool = *pp;
        if (pool->m_nSize <= 0 || pool->m_nSize >= s.new_count) continue;

        const int32_t old_count = pool->m_nSize;
        const size_t new_obj_bytes = static_cast<size_t>(s.new_count) * s.elem_size;
        const uint64_t new_objs = cpu->call_nested(m_fn_znam, {new_obj_bytes});
        const uint64_t new_flags = cpu->call_nested(m_fn_znam, {static_cast<uint64_t>(s.new_count)});
        if (!new_objs || !new_flags) continue;

        std::memset(reinterpret_cast<void*>(new_objs), 0, new_obj_bytes);
        std::memcpy(reinterpret_cast<void*>(new_objs),
                    reinterpret_cast<const void*>(pool->m_pObjects),
                    static_cast<size_t>(old_count) * s.elem_size);

        auto* dst_flags = reinterpret_cast<uint8_t*>(new_flags);
        std::memcpy(dst_flags, reinterpret_cast<const void*>(pool->m_byteMap), static_cast<size_t>(old_count));
        std::memset(dst_flags + old_count, 0x80, static_cast<size_t>(s.new_count - old_count));

        if (m_fn_zdapv) {
            if (pool->m_pObjects) cpu->call_nested(m_fn_zdapv, {pool->m_pObjects});
            if (pool->m_byteMap)  cpu->call_nested(m_fn_zdapv, {pool->m_byteMap});
        }

        pool->m_pObjects = new_objs;
        pool->m_byteMap  = new_flags;
        pool->m_nSize    = s.new_count;
        ++expanded;
    }

    if (expanded > 0) {
        m_pools_expanded = true;
        std::cout << "  [Distance] Expanded " << expanded << " CPools cleanly for HD PC streaming.\n";
    }
}

void LcsDistance::on_vis_init_post() {
    auto* cur_cpu = lcs::cpu::DynarmicCore::current();
    auto* cpu = cur_cpu ? cur_cpu : m_cpu;
    if (!cpu || !m_fn_znam) return;
    const uint64_t alpha_list = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotVisAlphaListRva);
    const uint64_t alpha_ent  = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotVisAlphaEntListRva);
    expand_vis_link_list(*cpu, m_fn_znam, m_fn_zdapv, alpha_list, 256u);
    expand_vis_link_list(*cpu, m_fn_znam, m_fn_zdapv, alpha_ent,  2048u);
    std::cout << "  [Distance] Expanded CVisibilityComponents alpha lists (256 / 2048).\n";
}

void LcsDistance::on_get_lod_settings_post(uint64_t orig_ret_ptr) {
    if (!orig_ret_ptr || m_has_base_lod) return;
    std::memcpy(m_base_lod_raw, reinterpret_cast<const void*>(orig_ret_ptr), sizeof(m_base_lod_raw));
    m_has_base_lod = true;
    sync_settings_now();
}

void LcsDistance::sync_settings_now() {
    if (!m_installed) return;

    const float view_scale    = compute_view_scale();
    const float detail_scale  = compute_detail_scale();
    const float pop_scale     = compute_pop_dist_scale();
    const float traffic_scale = compute_traffic_scale();
    const float ped_scale     = compute_ped_scale();

    {
        const int detail_dist = lcs::settings::get().detail_distance;
        auto* sp = reinterpret_cast<float*>(m_va_inv_detail_scale);
        sp[0] = 1.0f / std::max(0.1f, detail_scale);
        sp[1] = detail_scale;
        sp[2] = (detail_dist >= 30) ? map_slider_curve(detail_dist, 55.0f, 160.0f, 550.0f) : 0.0f;
        sp[3] = (detail_dist >= 30) ? 12.0f : 1e9f;
        sp[4] = 300.0f;
        sp[5] = 0.1f;
    }
    update_330m_patches(view_scale);
    sync_spawn_distances();
    apply_cull_fade_overrides(m_load_bias, detail_scale, lcs::settings::get().detail_distance);

    // Keep game's built-in Graphics Detail preference pinned to High (2) so GetDrawDistance() stays 1.0f
    if (auto* p_detail = deref_got<int32_t>(m_load_bias, kGotPrefsGraphicsDetailRva)) {
        if (*p_detail < 2) *p_detail = 2;
    }

    if (auto* p_car_mul = deref_got<float>(m_load_bias, kGotCarNumMultRva)) {
        *p_car_mul = traffic_scale;
    }
    if (auto* p_ped_mul = deref_got<float>(m_load_bias, kGotPedNumMultRva)) {
        *p_ped_mul = ped_scale;
    }
    if (auto* p_max_cars = deref_got<uint32_t>(m_load_bias, kGotMaxCarsInUseRva)) {
        const uint32_t base_max_cars = 24;
        *p_max_cars = static_cast<uint32_t>(std::clamp(std::lround(base_max_cars * traffic_scale), 8L, 72L));
    }

    if (m_has_base_lod) {
        GuestLodSettings base{};
        std::memcpy(&base, m_base_lod_raw, sizeof(base));

        // Note CVisibilityComponents::SetRslCamera reads linear distances that are compared against dist^2
        base.VehicleDist0               *= detail_scale;
        base.VehicleDist1               *= detail_scale;
        base.VehicleFadeDist            *= detail_scale;
        base.VehicleHiddenComponentDist *= detail_scale;
        base.BigVehicleDist0            *= detail_scale;
        base.BigVehicleDist1            *= detail_scale;
        base.PedDist                    *= detail_scale;
        base.PedFadeDist                *= detail_scale;

        base.MaxPeds       = static_cast<int32_t>(std::clamp(std::lround(base.MaxPeds * ped_scale * 1.40f), 8L, 84L));
        base.MaxRandomCars = static_cast<int32_t>(std::clamp(std::lround(base.MaxRandomCars * traffic_scale * 1.50f), 4L, 56L));
        base.MaxParkedCars = static_cast<int32_t>(std::clamp(std::lround(base.MaxParkedCars * traffic_scale * 1.25f), 2L, 32L));

        // Keep cull range tight enough (<=360m) so old traffic behind the player/across islands
        // despawns promptly instead of holding all vehicle slots hostage across the city.
        base.PopulationAddMinRadius     *= std::clamp(0.80f + 0.14f * pop_scale, 0.70f, 1.22f);
        base.PopulationAddMaxRadius     *= std::clamp(pop_scale, 0.65f, 1.75f);
        const float cull_scale           = std::clamp(pop_scale * 1.15f, 0.75f, 1.85f);
        base.PopulationCullRange         = std::clamp(base.PopulationCullRange * cull_scale, 140.0f, 340.0f);
        base.PopulationCullRangeOffScr   = std::min(base.PopulationCullRange * 0.85f, 290.0f);
        base.PopulationCullRangeFarAway  = std::min(base.PopulationCullRange + 50.0f, 390.0f);
        base.DriverHighDetailRange      *= std::max(detail_scale, pop_scale);
        base.BoatRiderHighDetailRange   *= std::max(detail_scale, pop_scale);

        std::memcpy(reinterpret_cast<void*>(m_va_scaled_lod_struct), &base, sizeof(base));
        *reinterpret_cast<uint64_t*>(m_va_scaled_lod_ready) = m_va_scaled_lod_struct;
    }

    if (auto* p_ms_lod = deref_got<float>(m_load_bias, kGotRendererLodScaleRva)) {
        if (!m_has_base_ms_lod && std::isfinite(*p_ms_lod) && *p_ms_lod > 0.05f && *p_ms_lod < 10.0f) {
            m_base_ms_lod_scale = *p_ms_lod;
            m_has_base_ms_lod = true;
        }
        *p_ms_lod = m_base_ms_lod_scale * view_scale;
    }
    if (auto* p_draw_lod = deref_got<float>(m_load_bias, kGotDrawLodDistRva)) {
        if (!m_has_base_draw_lod && std::isfinite(*p_draw_lod) && *p_draw_lod > 1.0f && *p_draw_lod < 2000.0f) {
            m_base_draw_lod_dist = *p_draw_lod;
            m_has_base_draw_lod = true;
        }
        if (m_has_base_draw_lod) {
            *p_draw_lod = m_base_draw_lod_dist * view_scale;
        }
    }
}

void LcsDistance::adjust_fog_parameters(float in_out[4]) const {
    if (!in_out) return;
    const float raw_start = in_out[0];
    const float raw_range = in_out[1];
    const float raw_far   = in_out[3];

    // Identify whether RslCameraBeginUpdate uploaded our stretched world farClip
    // (or unstretched interior/frontend farClip). Recover base TimeCycle farClip.
    float base_far = (raw_far > 1.0f && std::isfinite(raw_far)) ? raw_far : (raw_start + raw_range);
    if (m_base_far_clip > 10.0f && m_last_scaled_far > 10.0f &&
        std::fabs(raw_far - m_last_scaled_far) < 25.0f) {
        base_far = m_base_far_clip;
    }
    base_far = std::max(10.0f, base_far);

    const auto& cfg = lcs::settings::get();
    const float dist_mul  = static_cast<float>(std::clamp(cfg.fog_distance, 10, 100)) / 100.0f;
    const float start_mul = static_cast<float>(std::clamp(cfg.fog_start, 0, 300)) / 100.0f;

    const float fog_end = std::max(10.0f, base_far * dist_mul);
    float fog_start = 0.0f;
    if (raw_start > 0.0f) {
        fog_start = std::min(raw_start * dist_mul * start_mul, fog_end * 0.95f);
        fog_start = std::max(0.0f, fog_start);
    }
    const float fog_range = std::max(1.0f, fog_end - fog_start);

    in_out[0] = fog_start;
    in_out[1] = fog_range;
}

uint64_t LcsDistance::on_far_clip_set_pre(uint64_t /*rsl_cam*/, uint32_t raw_far_bits) {
    float far_clip = 0.0f;
    std::memcpy(&far_clip, &raw_far_bits, sizeof(float));

    const float view_scale = compute_view_scale();
    if (far_clip > 1.0f && std::isfinite(far_clip) && view_scale > 0.1f) {
        if (m_base_far_clip <= 1.0f || std::fabs(far_clip - m_last_scaled_far) > 0.5f) {
            m_base_far_clip = far_clip;
        }
        far_clip = std::clamp(m_base_far_clip * view_scale, 150.0f, 3200.0f);
        m_last_scaled_far = far_clip;
        m_last_far_clip = far_clip;
    }

    uint32_t out_bits = 0;
    std::memcpy(&out_bits, &far_clip, sizeof(float));
    return out_bits;
}

uint64_t LcsDistance::on_camera_process_post(uint64_t cam_ptr) {
    sync_settings_now();
    if (!cam_ptr) return 0;

    const float view_scale = compute_view_scale();
    float* lod_mul = reinterpret_cast<float*>(cam_ptr + 0xf0ULL);
    float* gen_mul = reinterpret_cast<float*>(cam_ptr + 0xf4ULL);
    // CCamera::Process() multiplies cam+0xf0 by CRenderer::ms_lodDistScale already.
    if (!std::isfinite(*lod_mul) || *lod_mul <= 0.01f) {
        *lod_mul = m_base_ms_lod_scale * view_scale;
    }
    m_last_lod_scale = *lod_mul;

    if (std::isfinite(*gen_mul) && *gen_mul > 0.05f) {
        if (std::fabs(*gen_mul - m_last_gen_mul) > 0.01f) {
            m_base_gen_mul = std::clamp(*gen_mul, 0.25f, 1.50f);
        }
        // Keep CCamera::gen_mul in a controlled band so 185m driving spawn stays inside 320m circle.
        *gen_mul = std::clamp(m_base_gen_mul, 0.75f, 1.35f);
        m_last_gen_mul = *gen_mul;
    }

    uint64_t rsl_cam = *reinterpret_cast<const uint64_t*>(cam_ptr + 0xae0ULL);
    if (auto* p_far_clip = deref_got<float>(m_load_bias, kGotDrawFarClipRva)) {
        if (m_base_far_clip <= 1.0f ||
            (std::isfinite(*p_far_clip) && *p_far_clip > 1.0f && std::fabs(*p_far_clip - m_last_scaled_far) > 0.5f)) {
            if (std::isfinite(*p_far_clip) && *p_far_clip > 1.0f) {
                m_base_far_clip = *p_far_clip;
            }
        }
        if (m_base_far_clip > 1.0f) {
            const float scaled_far = std::clamp(m_base_far_clip * view_scale, 180.0f, 3200.0f);
            *p_far_clip = scaled_far;
            m_last_scaled_far = scaled_far;
            m_last_far_clip = scaled_far;
            *reinterpret_cast<float*>(m_va_cam_new_far_clip) = scaled_far;
            return rsl_cam;
        }
    }
    return 0;
}

uint64_t LcsDistance::on_big_building_check(uint64_t entity_ptr) {
    const bool cov = is_entity_lod_covered_by_hd(m_load_bias, entity_ptr);
    if (cov) {
        // Clear bit 19 (0x80000) and bit 51 so SetupEntityVisibility in sector scans
        // cannot resurrect a hidden BigBuilding LOD* mesh on top of HD geometry.
        auto* flags_ptr = reinterpret_cast<uint64_t*>(entity_ptr + 0x58ULL);
        *flags_ptr &= ~((1ULL << 19) | (1ULL << 51));
    }
    return cov ? 1ULL : 0ULL;
}

static inline float read_simple_model_raw_largest_lod(uint64_t model_info) {
    if (model_info < 0x10000ULL) return 0.0f;
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(model_info + 0x56ULL);
    const uint32_t n_sub = flags & 3u;
    if (n_sub == 0u) return 0.0f;
    int idx = (flags & 8u)
        ? (static_cast<int>(*reinterpret_cast<const uint8_t*>(model_info + 0x54ULL)) - 1)
        : (static_cast<int>(n_sub) - 1);
    if (idx < 0 || idx > 2) return 0.0f;
    const float v = *reinterpret_cast<const float*>(model_info + 0x48ULL + static_cast<uint64_t>(idx) * 4ULL);
    return (std::isfinite(v) && v > 0.0f) ? v : 0.0f;
}

// Large objects stay drawn out to 30 times their bounding radius (at most 1500 m): Portland's
// harbour cranes (raw 170-299 m, no LOD model) popped in ~550 m away, inside the view at the
// longest draw distances (the player's report, 2026-11-24). 0 when the rule does not apply.
static inline float large_object_far(uint64_t model_info) {
    static const bool s_off = [] { const char* e = std::getenv("LCS_LARGE_OBJ_FAR"); return e && e[0] == '0'; }();
    if (s_off || model_info < 0x10000ULL || lcs::settings::get().detail_distance < 30) return 0.0f;
    const uint64_t col = *reinterpret_cast<const uint64_t*>(model_info + 0x30ULL);  // CColModel*
    if (col < 0x10000ULL) return 0.0f;
    const float r = *reinterpret_cast<const float*>(col + 0x0cULL);                // bounding radius
    if (!std::isfinite(r) || r < 15.0f) return 0.0f;
    return std::min(1500.0f, r * 30.0f);
}

uint64_t LcsDistance::on_get_elem_from_dist_pre(uint64_t model_info, uint32_t dist_bits) {
    float dist = 0.0f;
    std::memcpy(&dist, &dist_bits, sizeof(dist));
    const float detail_scale = compute_detail_scale();
    float out_dist = dist / std::max(0.1f, detail_scale);

    const float raw_largest = read_simple_model_raw_largest_lod(model_info);
    if (lcs::settings::get().detail_distance >= 30 && raw_largest >= 12.0f && raw_largest < 300.0f) {
        const float eff_dist = compute_prop_effective_dist(raw_largest, detail_scale);
        out_dist = dist * (raw_largest / std::max(0.1f, eff_dist));
    }
    if (const float far_dist = large_object_far(model_info); far_dist > 0.0f && raw_largest >= 12.0f) {
        out_dist = std::min(out_dist, dist * (raw_largest / far_dist));
    }

    uint32_t out_bits = 0;
    std::memcpy(&out_bits, &out_dist, sizeof(out_bits));
    return static_cast<uint64_t>(out_bits);
}

uint64_t LcsDistance::on_get_largest_lod_post(uint64_t model_info, uint32_t orig_ret_bits) {
    float orig_ret = 0.0f;
    std::memcpy(&orig_ret, &orig_ret_bits, sizeof(orig_ret));
    float out_val = orig_ret;

    if (std::isfinite(orig_ret) && orig_ret > 0.0f) {
        const float detail_scale = compute_detail_scale();
        const float raw_largest = read_simple_model_raw_largest_lod(model_info);
        if (raw_largest >= 12.0f && raw_largest < 300.0f && lcs::settings::get().detail_distance >= 30) {
            const float eff_dist = compute_prop_effective_dist(raw_largest, detail_scale);
            out_val = orig_ret * (eff_dist / raw_largest);
        } else if (raw_largest > 0.0f) {
            out_val = orig_ret * detail_scale;
        }
        if (const float far_dist = large_object_far(model_info); far_dist > 0.0f && raw_largest >= 12.0f) {
            out_val = std::max(out_val, orig_ret * (far_dist / raw_largest));
        }
    }

    uint32_t out_bits = 0;
    std::memcpy(&out_bits, &out_val, sizeof(out_bits));
    return static_cast<uint64_t>(out_bits);
}

void LcsDistance::on_scan_world_pre() {
    m_vw_patched = false;
    if (!m_load_bias) return;
    const uint64_t cam_ptr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotCameraRva);
    const uint64_t rsl_cam = cam_ptr ? *reinterpret_cast<const uint64_t*>(cam_ptr + 0xae0ULL) : 0ULL;
    if (!rsl_cam) return;
    auto* vw_x = reinterpret_cast<float*>(rsl_cam + 0x70ULL);
    const float cur = *vw_x;
    if (cur > 0.05f && cur < 2.0f) {
        m_saved_vw_x = cur;
        *vw_x = cur * 1.35f;
        m_vw_patched = true;
    }
}

void LcsDistance::on_scan_world_post() {
    if (m_vw_patched && m_load_bias) {
        const uint64_t cam_ptr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotCameraRva);
        const uint64_t rsl_cam = cam_ptr ? *reinterpret_cast<const uint64_t*>(cam_ptr + 0xae0ULL) : 0ULL;
        if (rsl_cam) {
            *reinterpret_cast<float*>(rsl_cam + 0x70ULL) = m_saved_vw_x;
        }
    }
    m_vw_patched = false;

    // Strip any BigBuilding/LOD entities that slipped into ms_aVisibleEntityPtrs via
    // ScanBigBuildingList's 3-of-4 frame cached bit-43 fast path despite HD cell coverage.
    if (m_load_bias && LcsWorldHd::instance().radius() > 0) {
        auto* n_vis = *reinterpret_cast<int32_t* const*>(m_load_bias + 0x7fa208ULL);
        auto* p_vis = *reinterpret_cast<uint64_t* const*>(m_load_bias + 0x7fa220ULL);
        const auto* num_mi = *reinterpret_cast<const int32_t* const*>(m_load_bias + 0x7f8fa0ULL);
        const auto* mi_pp  = *reinterpret_cast<const uint64_t* const*>(m_load_bias + 0x7f8fb0ULL);
        const uint64_t* mi_ptrs = (mi_pp && *mi_pp >= 0x10000ULL)
            ? reinterpret_cast<const uint64_t*>(*mi_pp) : nullptr;
        if (n_vis && p_vis && num_mi && mi_ptrs && *n_vis > 0) {
            const int32_t n = std::clamp(*n_vis, 0, 8192);
            int32_t write_i = 0;
            for (int32_t i = 0; i < n; ++i) {
                const uint64_t ent = p_vis[i];
                bool hide = false;
                if (ent >= 0x10000ULL) {
                    const int16_t mid = *reinterpret_cast<const int16_t*>(ent + 0x7cULL);
                    if (mid >= 0 && mid < *num_mi) {
                        const uint64_t mi = mi_ptrs[mid];
                        if (mi >= 0x10000ULL) {
                            const uint8_t mtype = *reinterpret_cast<const uint8_t*>(mi + 0x28ULL);
                            if (mtype == 1 || mtype == 3) {
                                const uint16_t mflags = *reinterpret_cast<const uint16_t*>(mi + 0x56ULL);
                                const float lodd = *reinterpret_cast<const float*>(mi + 0x48ULL);
                                const uint64_t rel = *reinterpret_cast<const uint64_t*>(mi + 0x58ULL);
                                const uint64_t eflags = *reinterpret_cast<const uint64_t*>(ent + 0x58ULL);
                                const bool is_bb = (eflags & (1ULL << 24)) != 0 ||
                                                   (eflags & (1ULL << 45)) != 0 ||
                                                   (mflags & 0x10u) != 0 ||
                                                   (rel != 0) ||
                                                   (std::isfinite(lodd) && lodd >= 295.0f);
                                if (is_bb && is_entity_lod_covered_by_hd(m_load_bias, ent)) {
                                    hide = true;
                                }
                            }
                        }
                    }
                }
                if (!hide) {
                    p_vis[write_i++] = ent;
                }
            }
            *n_vis = write_i;
        }
    }

    static const bool s_lod_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    if (s_lod_audit && m_load_bias && LcsWorldHd::instance().radius() > 0) {
        static int s_scan_cnt = 0;
        ++s_scan_cnt;
        if (s_scan_cnt >= 240 && (s_scan_cnt % 120) == 0) {
            const uint64_t cam_ptr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotCameraRva);
            const float cx = cam_ptr ? *reinterpret_cast<const float*>(cam_ptr + 0x30ULL) : 0.0f;
            const float cy = cam_ptr ? *reinterpret_cast<const float*>(cam_ptr + 0x34ULL) : 0.0f;
            const auto* n_vis = *reinterpret_cast<const int32_t* const*>(m_load_bias + 0x7fa208ULL);
            const auto* p_vis = *reinterpret_cast<const uint64_t* const*>(m_load_bias + 0x7fa220ULL);
            const auto* num_mi = *reinterpret_cast<const int32_t* const*>(m_load_bias + 0x7f8fa0ULL);
            const auto* mi_pp  = *reinterpret_cast<const uint64_t* const*>(m_load_bias + 0x7f8fb0ULL);
            const uint64_t* mi_ptrs = (mi_pp && *mi_pp >= 0x10000ULL)
                ? reinterpret_cast<const uint64_t*>(*mi_pp) : nullptr;

            int bldg_high_lod = 0, bldg_cov_missed = 0;
            if (n_vis && p_vis) {
                const int32_t n = std::clamp(*n_vis, 0, 8192);
                for (int32_t i = 0; i < n; ++i) {
                    const uint64_t ent = p_vis[i];
                    if (ent < 0x10000ULL) continue;
                    const int16_t mid = *reinterpret_cast<const int16_t*>(ent + 0x7cULL);
                    if (!num_mi || !mi_ptrs || mid < 0 || mid >= *num_mi) continue;
                    const uint64_t mi = mi_ptrs[mid];
                    if (mi < 0x10000ULL) continue;
                    const uint8_t mtype = *reinterpret_cast<const uint8_t*>(mi + 0x28ULL);
                    if (mtype != 1 && mtype != 3) continue;
                    const uint16_t mflags = *reinterpret_cast<const uint16_t*>(mi + 0x56ULL);
                    const float lodd = *reinterpret_cast<const float*>(mi + 0x48ULL);
                    const float neard = *reinterpret_cast<const float*>(mi + 0x50ULL);
                    const uint64_t rel = *reinterpret_cast<const uint64_t*>(mi + 0x58ULL);
                    const uint64_t eflags = *reinterpret_cast<const uint64_t*>(ent + 0x58ULL);
                    const bool is_bb = (eflags & (1ULL << 24)) != 0 || (eflags & (1ULL << 45)) != 0 ||
                                       (rel != 0) || (mflags & 0x10u) != 0 || lodd >= 295.0f;
                    if (!is_bb) continue;
                    ++bldg_high_lod;
                    const float ex = *reinterpret_cast<const float*>(ent + 0x40ULL);
                    const float ey = *reinterpret_cast<const float*>(ent + 0x44ULL);
                    const uint64_t col = *reinterpret_cast<const uint64_t*>(mi + 0x30ULL);
                    float csx = 0.0f, csy = 0.0f, csr = 75.0f;
                    if (col >= 0x10000ULL) {
                        csx = *reinterpret_cast<const float*>(col + 0x00ULL);
                        csy = *reinterpret_cast<const float*>(col + 0x04ULL);
                        csr = *reinterpret_cast<const float*>(col + 0x0cULL);
                    }
                    const float dist = std::sqrt((ex - cx) * (ex - cx) + (ey - cy) * (ey - cy));
                    const bool cov_full = is_entity_lod_covered_by_hd(m_load_bias, ent);
                    const bool cov_ctr = LcsWorldHd::instance().is_circle_covered_by_hd(ex, ey, 10.0f);
                    if (cov_full || cov_ctr || dist < 600.0f) {
                        ++bldg_cov_missed;
                        if (bldg_cov_missed <= 12) {
                            std::cout << "  [TMP_LOD_AUDIT] ENT mid=" << mid
                                      << " mtype=" << static_cast<int>(mtype)
                                      << " mfl=0x" << std::hex << mflags << " efl=0x" << eflags << std::dec
                                      << " pos=(" << ex << "," << ey << ") dist=" << dist
                                      << " col=(" << csx << "," << csy << ",r=" << csr << ",p=" << (col ? 1 : 0) << ")"
                                      << " lodd=" << lodd << " near=" << neard << " rel=" << (rel ? 1 : 0)
                                      << " covFull=" << cov_full << " covCtr=" << cov_ctr << "\n";
                        }
                    }
                }
            }
            if (bldg_high_lod > 0 || bldg_cov_missed > 0) {
                std::cout << "  [TMP_LOD_SUM] scan=" << s_scan_cnt << " cam=(" << cx << "," << cy << ")"
                          << " nVis=" << (n_vis ? *n_vis : -1)
                          << " entHighLod=" << bldg_high_lod
                          << " nearOrCov=" << bldg_cov_missed << "\n" << std::flush;
            }
        }
    }
}

// Where new peds and cars may appear. In view they must appear far enough not to be seen
// materialising (with the extended draw distance a car popping up 120 m ahead is obvious);
// out of view (behind the camera) the game's own short distances stay. Population distance
// below 50 keeps the game's values.
void LcsDistance::sync_spawn_distances() {
    if (!m_cpu || !m_load_bias) return;
    const int pop = lcs::settings::get().population_distance;
    const float t = std::clamp((pop - 50) / 50.0f, 0.0f, 1.0f);
    const bool extended = pop >= 50;
    // GenerateOneRandomCar's in-view spawn distance = speed factor (0.65..1.33) x gen x base;
    // GenerateCarCreationCoors only finds road nodes inside the search radius.
    // GenerateOneRandomCar keeps a car spawned in view only inside [130, 190] x f from the player
    // (f = speed factor x gen; 0x3b3af0 / 0x3b3ac0) and >= 105 x f from the camera (0x3b3b08):
    // the in-view base (140, 185 before) must move with that window or every car is discarded.
    const float car_base = env_float("LCS_CAR_INVIEW_BASE", 185.0f);
    const float car_radius = env_float("LCS_CAR_NODE_RADIUS", 380.0f);
    // No floor by default: the game already spawns peds in view >= ~90 m, and a floor (even 100 m)
    // emptied the pavements near the player by 35-60% (pop-in probe, 2026-11-24).
    const float ped_min = env_float("LCS_PED_INVIEW_MIN", 0.0f);
    if (std::fabs(car_base - m_applied_car_inview) >= 1.0f) {
        const uint32_t insn = mov_w8_float(car_base);
        patch_mem(*m_cpu, m_load_bias + 0x3b2a0cULL, &insn, sizeof(uint32_t));
        patch_mem(*m_cpu, m_load_bias + 0x3b2aa8ULL, &insn, sizeof(uint32_t));
        patch_mem(*m_cpu, m_load_bias + 0x3b2af0ULL, &insn, sizeof(uint32_t));
        const uint32_t keep_max = mov_w8_float(car_base * (190.0f / 185.0f));
        const uint32_t keep_min = mov_w8_float(car_base * (130.0f / 185.0f));
        const uint32_t cam_min = mov_w8_float(car_base * (105.0f / 185.0f));
        patch_mem(*m_cpu, m_load_bias + 0x3b3ac0ULL, &keep_max, sizeof(uint32_t));
        patch_mem(*m_cpu, m_load_bias + 0x3b3af0ULL, &keep_min, sizeof(uint32_t));
        patch_mem(*m_cpu, m_load_bias + 0x3b3b08ULL, &cam_min, sizeof(uint32_t));
        m_applied_car_inview = car_base;
    }
    if (std::fabs(car_radius - m_applied_car_node_radius) >= 1.0f) {
        const uint32_t insn = mov_w8_float(car_radius);
        patch_mem(*m_cpu, m_load_bias + 0x3cea00ULL, &insn, sizeof(uint32_t));
        m_applied_car_node_radius = car_radius;
    }
    if (m_ped_inview_floor) {
        m_ped_inview_floor[0] = ped_min;
        m_ped_inview_floor[1] = ped_min > 0.0f ? 60.0f : 0.0f;
    }
}

void LcsDistance::update_330m_patches(float view_scale) {
    auto* cur_cpu = lcs::cpu::DynarmicCore::current();
    auto* cpu = cur_cpu ? cur_cpu : m_cpu;
    if (!cpu) return;

    // 1. Lift 330m visibility clamps (SetupBigBuildingVisibility & SetupEntityVisibility) up to full 3200m far clip
    const float target_clamp = std::clamp(330.0f * std::max(1.0f, view_scale), 330.0f, 3200.0f);
    if (std::fabs(target_clamp - m_applied_330_clamp) >= 1.0f) {
        uint32_t fbits = 0;
        std::memcpy(&fbits, &target_clamp, sizeof(float));
        const uint32_t hi16 = (fbits >> 16) & 0xffffu;
        const uint32_t insn = 0x52a00008u | (hi16 << 5); // mov w8, #<hi16>, lsl #16
        patch_mem(*cpu, m_load_bias + kPatch330BigBldgRva, &insn, sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatch330EntityRva, &insn, sizeof(uint32_t));
        m_applied_330_clamp = target_clamp;
    }

    // 2. Lift ScanSectorList request-queue throttle (cmp w8, #9 -> cmp w8, #128) and disable false m_loadingPriority=1
    if (m_orig_scan_req_cap_insn == 0) {
        m_orig_scan_req_cap_insn = 0x7100251fu; // cmp w8, #0x9
        const uint32_t lifted_cap = 0x7102011fu; // cmp w8, #0x80 (128)
        patch_mem(*cpu, m_load_bias + kPatchScanReqCapRva, &lifted_cap, sizeof(uint32_t));
        const uint32_t no_prio_lp1 = 0x52800009u; // mov w9, #0 (prevent close unstreamed props from starving far frustum)
        patch_mem(*cpu, m_load_bias + kPatchScanPrioLp1Rva, &no_prio_lp1, sizeof(uint32_t));

        // Expand GenerateCarCreationCoors (340m->320m circle with 360-deg strided sampling,
        // 100->240 candidate nodes) and GeneratePedCreationCoors (+120m->+220m circle margin,
        // 150->236 candidate nodes), and raise CCarCtrl::GenerateOneRandomCar close-spawn mins
        // (40m/55m -> 100..115m, 140m*s10 -> 185m*s10) so cars never pop in nearby.
        const uint32_t car_cmp_240    = 0x7103c11fu; // 0x3ceae0: cmp w8, #0xf0
        const uint32_t car_mov_240    = 0x52801e09u; // 0x3ceae4: mov w9, #0xf0
        const uint32_t ped_margin_220 = 0x52a86b88u; // 0x3cf19c: mov w8, #0x435c, lsl #16 (220.0f)
        const uint32_t ped_cmp_236    = 0x7103b11fu; // 0x3cf270: cmp w8, #0xec
        const uint32_t ped_mov_236    = 0x52801d8au; // 0x3cf274: mov w10, #0xec
        patch_mem(*cpu, m_load_bias + 0x3ceae0ULL, &car_cmp_240,    sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3ceae4ULL, &car_mov_240,    sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3cf19cULL, &ped_margin_220, sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3cf270ULL, &ped_cmp_236,    sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3cf274ULL, &ped_mov_236,    sizeof(uint32_t));

        const uint32_t car_pitch_100  = 0x52a85909u; // 0x3b2940: mov w9, #0x42c8, lsl #16 (100.0f vs 55.0f)
        const uint32_t car_slow_105   = 0x52a85a49u; // 0x3b2b28: mov w9, #0x42d2, lsl #16 (105.0f vs 40.0f)
        const uint32_t car_fast_115   = 0x52a85cc9u; // 0x3b2b48: mov w9, #0x42e6, lsl #16 (115.0f vs 40.0f)
        patch_mem(*cpu, m_load_bias + 0x3b2940ULL, &car_pitch_100, sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3b2b28ULL, &car_slow_105,  sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + 0x3b2b48ULL, &car_fast_115,  sizeof(uint32_t));

        // Direct-GL single-thread execution on RenderThread: bypass lglRenderQueue ring buffer
        // encode/decode + lglSleep/lglThreadSync overhead by routing requestEnableGameTickThread
        // through its disable branch cleanly.
        const uint32_t orig_gtt_req = *reinterpret_cast<const uint32_t*>(m_load_bias + 0x534cb0ULL);
        if (orig_gtt_req == 0x360001a0u) {
            const uint32_t b_disable = 0x1400000du; // b 0x534ce4
            patch_mem(*cpu, m_load_bias + 0x534cb0ULL, &b_disable, sizeof(uint32_t));
        }
    }

    // 3. Scale CPopulation::ManagePopulation ConvertToRealObject radius (80.0f -> up to 180.0f)
    const float pop_convert = std::clamp(80.0f * std::max(1.0f, compute_pop_dist_scale()), 80.0f, 180.0f);
    if (std::fabs(pop_convert - m_applied_pop_convert) >= 1.0f) {
        uint32_t fbits = 0;
        std::memcpy(&fbits, &pop_convert, sizeof(float));
        const uint32_t hi16 = (fbits >> 16) & 0xffffu;
        const uint32_t insn = 0x52a00016u | (hi16 << 5); // mov w22, #<hi16>, lsl #16
        patch_mem(*cpu, m_load_bias + kPatchPopConvertRva, &insn, sizeof(uint32_t));
        m_applied_pop_convert = pop_convert;
    }

    // 4. Scale CRenderer::ScanWorld (including 0x35ee2c split threshold) & RequestObjectsInFrustum up to 3200m farZ
    const float far_goal = (m_last_scaled_far > 10.0f) ? m_last_scaled_far : (888.89f * view_scale);
    const float frustum_clamp = std::clamp(std::max(300.0f * std::max(1.0f, view_scale), far_goal), 300.0f, 3200.0f);
    if (std::fabs(frustum_clamp - m_applied_300_clamp) >= 1.0f) {
        uint32_t fbits = 0;
        std::memcpy(&fbits, &frustum_clamp, sizeof(float));
        const uint32_t hi16 = (fbits >> 16) & 0xffffu;
        const uint32_t insn_w9  = 0x52a00009u | (hi16 << 5);
        const uint32_t insn_w10 = 0x52a0000au | (hi16 << 5);
        const uint32_t insn_w11 = 0x52a0000bu | (hi16 << 5);
        patch_mem(*cpu, m_load_bias + kPatchScanWorld300A,  &insn_w9,  sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatchScanWorld300B,  &insn_w9,  sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatchScanWorld300C,  &insn_w11, sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatchScanWorldSplitRva, &insn_w10, sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatchReqFrustum300A, &insn_w9,  sizeof(uint32_t));
        patch_mem(*cpu, m_load_bias + kPatchReqFrustum300B, &insn_w9,  sizeof(uint32_t));
        m_applied_300_clamp = frustum_clamp;
    }

    // 5. Lift cWorldStream::AddDynamic 100.0m (10000.0f sq) prop culling distance up to 500.0m (250000.0f sq)
    const int detail_dist = lcs::settings::get().detail_distance;
    const float detail_scale = compute_detail_scale();
    const float max_dyn_dist = (detail_dist >= 30)
        ? std::clamp(compute_prop_effective_dist(250.0f, detail_scale), 100.0f, 500.0f)
        : 100.0f;
    const float target_dyn_sq = max_dyn_dist * max_dyn_dist;
    if (std::fabs(target_dyn_sq - m_applied_add_dyn_sq) >= 1.0f) {
        uint32_t fbits = 0;
        std::memcpy(&fbits, &target_dyn_sq, sizeof(float));
        const uint32_t lo16 = fbits & 0xffffu;
        const uint32_t hi16 = (fbits >> 16) & 0xffffu;
        const uint32_t pair[2] = {
            0x52800009u | (lo16 << 5), // mov  w9, #<lo16>
            0x72a00009u | (hi16 << 5), // movk w9, #<hi16>, lsl #16
        };
        patch_mem(*cpu, m_load_bias + kPatchAddDynamicDistSqRva, pair, sizeof(pair));
        m_applied_add_dyn_sq = target_dyn_sq;
    }

    // 6. Scale CStreaming::DeleteFarAwayRslObjects keep radius (default 2..11 sectors = 80..440m -> 14..23 = 560..920m)
    // so crossing 40m sector boundaries while driving never destroys RwObjects still inside visible distance.
    const int keep_sec = (detail_dist >= 50) ? 14 : std::clamp(2 + (detail_dist * 12) / 50, 2, 14);
    if (keep_sec != m_applied_keep_sec) {
        enum class KeepKind { MovStart, MovStartP1, SubsImm, CmpImm, CmpEnd };
        struct KeepSite { uint64_t rva; KeepKind kind; };
        constexpr KeepSite kKeepSites[] = {
            {0x4a73acULL, KeepKind::MovStart},  {0x4a73b0ULL, KeepKind::MovStartP1},
            {0x4a74c8ULL, KeepKind::CmpImm},    {0x4a74f0ULL, KeepKind::CmpImm},
            {0x4a75bcULL, KeepKind::CmpEnd},    {0x4a75dcULL, KeepKind::SubsImm},
            {0x4a75e4ULL, KeepKind::MovStartP1},{0x4a75e8ULL, KeepKind::MovStart},
            {0x4a7600ULL, KeepKind::CmpEnd},    {0x4a7728ULL, KeepKind::CmpImm},
            {0x4a7750ULL, KeepKind::CmpImm},    {0x4a7870ULL, KeepKind::MovStart},
            {0x4a7874ULL, KeepKind::MovStartP1},{0x4a7990ULL, KeepKind::CmpImm},
            {0x4a79b8ULL, KeepKind::CmpImm},    {0x4a7a68ULL, KeepKind::CmpEnd},
            {0x4a7a88ULL, KeepKind::SubsImm},   {0x4a7a90ULL, KeepKind::MovStartP1},
            {0x4a7a94ULL, KeepKind::MovStart},  {0x4a7aacULL, KeepKind::CmpEnd},
            {0x4a7bd8ULL, KeepKind::CmpImm},    {0x4a7c00ULL, KeepKind::CmpImm},
        };
        for (const auto& ks : kKeepSites) {
            const uint64_t va = m_load_bias + ks.rva;
            const uint32_t cur = *reinterpret_cast<const uint32_t*>(va);
            uint32_t patched = cur;
            const uint32_t u_r   = static_cast<uint32_t>(keep_sec);
            const uint32_t u_rp1 = static_cast<uint32_t>(keep_sec + 1);
            const uint32_t u_end = static_cast<uint32_t>(keep_sec + 10);
            if (ks.kind == KeepKind::MovStart || ks.kind == KeepKind::MovStartP1) {
                const uint32_t v = (ks.kind == KeepKind::MovStart) ? u_r : u_rp1;
                if ((cur & 0xffe00000u) == 0x52800000u) {
                    patched = (cur & 0xffe0001fu) | ((v & 0xffffu) << 5);
                }
            } else {
                const uint32_t v = (ks.kind == KeepKind::CmpEnd) ? u_end : u_r;
                if ((cur & 0xffc00000u) == 0x71000000u) {
                    patched = (cur & ~0x3ffc00u) | ((v & 0xfffu) << 10);
                }
            }
            if (patched != cur) {
                patch_mem(*cpu, va, &patched, sizeof(uint32_t));
            }
        }
        m_applied_keep_sec = keep_sec;
    }
}

} // namespace lcs::game
