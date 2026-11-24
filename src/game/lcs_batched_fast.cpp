#include "game/lcs_batched_fast.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "game/lcs_distance.h"
#include "graphics/render_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

// Verified GOT RVAs & function addresses in libGame.so (arm64-v8a)
constexpr uint64_t kGotBatchedWorldRenderRva    = 0x808BF8ULL; // BatchedWorld::C_WorldRenderManager::Render(uint)
constexpr uint64_t kGotBatchedModelRenderRva    = 0x808BD8ULL; // BatchedModel::C_DeferredBatchManager::Render()
constexpr uint64_t kGotBatchedWorldGetFreeRva   = 0x808A98ULL; // BatchedWorld::C_WorldRenderManager::GetFreeDrawCall(...)
constexpr uint64_t kGotBatchedWorldResetRva     = 0x808C00ULL; // BatchedWorld::C_WorldRenderManager::Reset(uint, bool)
constexpr uint64_t kGotBatchedModelGetFreeRva   = 0x808948ULL; // BatchedModel::C_DeferredBatchManager::GetFreeBatch(...)
constexpr uint64_t kGotBatchedModelResetRva     = 0x808C08ULL; // BatchedModel::C_DeferredBatchManager::Reset(bool)
constexpr uint64_t kGotWsRenderNoCullRva        = 0x804578ULL; // cWorldStreamEx::RenderNoCull(...)

constexpr uint64_t kGotWsPvsEnableRva           = 0x7FE3A0ULL; // dvPVSEnable (+0x3d u8)
constexpr uint64_t kGotWsPvsPrecompRva          = 0x7FE410ULL; // dvPVSEnablePrecomputedVisibilityCheck (+0x3d u8)
constexpr uint64_t kGotWsPvsCamZonePtrRva       = 0x7FE390ULL; // PVS::gCameraPVSZone (ptr->ptr)
constexpr uint64_t kGotWsPvsVisZonesRva         = 0x7FE3B0ULL; // PVS::gVisibleZones ({begin, end})
constexpr uint64_t kGotWsWorldMgrPtrRva         = 0x7FAB50ULL; // gWorldManager (ptr->ptr)
constexpr uint64_t kGotWsForceNonTransRva       = 0x7FE290ULL; // dv_forceGetNonTransient (+0x3d u8)
constexpr uint64_t kGotWsRenderTransRva         = 0x7FE298ULL; // dv_renderTransient (+0x3d u8)
constexpr uint64_t kGotWsRenderNonTransRva      = 0x7FE2A0ULL; // dv_renderNonTransient (+0x3d u8)
constexpr uint64_t kGotWsAabbCullRva            = 0x7FE2A8ULL; // gWorldStreamAABBCullingEnabled (+0x3d u8)
constexpr uint64_t kGotWsMeshAabbCullRva        = 0x7FE2C0ULL; // gWorldStreamMeshAABBCullingEnabled (+0x3d u8)
constexpr uint64_t kGotWsGlobalAabbCullRva      = 0x7FE900ULL; // gAABBCullingEnabled (+0x3d u8)
constexpr uint64_t kGotWsCullModelMatRva        = 0x7FE890ULL; // Display::C_Culling::s_modelMatrix (float[16])
constexpr uint64_t kGotWsCullChangeFlagRva      = 0x7FE898ULL; // Display::C_Culling::s_changeFlag (u32)
constexpr uint64_t kGotWsCullFrustumRva         = 0x7FE8D0ULL; // Display::C_Culling::s_frustum
constexpr uint64_t kGotWsCullFadeEnableRva      = 0x7FE2C8ULL; // dvEnableCullFadeHack (+0x3d u8)
constexpr uint64_t kGotWsCullFadeRateRva        = 0x7FE278ULL; // dvCullHackFadeRate (+0x4c float)
constexpr uint64_t kGotWsCullRetOnSmallRva      = 0x7FE2D0ULL; // dvRenderCullHackReturnOnSmall (+0x3d u8)
constexpr uint64_t kGotWsTheCameraRva           = 0x7F9190ULL; // TheCamera
constexpr uint64_t kGotWsCurViewOffsRva         = 0x7F9750ULL; // gCurrentViewOffs
constexpr uint64_t kGotWsDetailSettingsRva      = 0x7FAE88ULL; // gDeviceDetailSettings
constexpr uint64_t kGotWsPrefsDynShadowsRva     = 0x7FD280ULL; // CMenuManager::m_PrefsDynamicShadows
constexpr uint64_t kGotWsRecvShadowsRadRva      = 0x7FE2D8ULL; // dv_fRecieveShadowsRadius (+0x4c float)
constexpr uint64_t kGotWsDynShadowTargetRva     = 0x7FD288ULL; // CDynamicShadows::s_target
constexpr uint64_t kGotWsDynShadowActiveRva     = 0x7F9880ULL; // CDynamicShadows::s_bActive
constexpr uint64_t kGotWsDebugShaderRva         = 0x7FE2E0ULL; // dvDebugWorldShader (i32)
constexpr uint64_t kGotWsRenderAlphaRva         = 0x7FAC80ULL; // gbRenderingWorldAlpha (u8)
constexpr uint64_t kGotWsRenderFadeRva          = 0x7FAC60ULL; // gbRenderingWorldFade (u8)
constexpr uint64_t kGotWsCurrentPassRva         = 0x7FABB8ULL; // gCurrentPass (u32)
constexpr uint64_t kGotWsEnableDeferredRva      = 0x7FE2E8ULL; // dvEnableDeferredWorldRenderer (+0x3d u8)
constexpr uint64_t kGotWsTexZBiasRva            = 0x7FE038ULL; // dv_texZBias (+0x4c float)
constexpr uint64_t kGotWsShadowStrengthRva      = 0x7FE2F8ULL; // dv_shadowStrength (+0x4c float)
constexpr uint64_t kGotWsCurrentDepthOffsRva    = 0x7FABB0ULL; // gCurrentDepthOffset (float)
constexpr uint64_t kGotWsDrawCountRva           = 0x7FD218ULL; // gWorldDrawCount (u32)

constexpr uint64_t kSymWorldResetRva            = 0x5924D4ULL; // BatchedWorld::C_WorldRenderManager::Reset(uint, bool)
constexpr uint64_t kSymCullingUpdateRva         = 0x581784ULL; // Display::C_Culling::Update(Display::ECullSpace)
constexpr uint64_t kSymModelResetRva            = 0x5928ACULL; // BatchedModel::C_DeferredBatchManager::Reset(bool)
constexpr uint64_t kSymGetVsUniformStrIdRva     = 0x59F18CULL; // Display::GetVertexShaderUniformStringId(uint)
constexpr uint64_t kSymGetFsUniformStrIdRva     = 0x59F298ULL; // Display::GetFragmentShaderUniformStringId(uint)
constexpr uint64_t kSymWsRenderBucketStartRva   = 0x39FFBCULL; // cWorldStream::Render(int) m_fadeTimer<=0 bucket loop
constexpr uint64_t kSymWsSetListStatesRva       = 0x3A0A40ULL; // cWorldStream::SetListStates(cGuList&, int)
constexpr uint64_t kSymWsUnsetListStatesRva     = 0x3A0FD4ULL; // cWorldStream::UnsetListStates(cGuList&, int)
constexpr uint64_t kSymWsRenderAlphaRva         = 0x3A0B84ULL; // cWorldStream::RenderAlpha(const sGeomInstance*, int, int, int)
constexpr uint64_t kSymSetBlendModePassRva      = 0x39EDB4ULL; // SetBlendModeForPass(int)
constexpr uint64_t kSymPerfSphereCullRva        = 0x5824C0ULL; // Display::C_Culling::PerformSphereCullCheck
constexpr uint64_t kSymSGeomInstCtorRva         = 0x38BF68ULL; // sGeomInstance::sGeomInstance()

constexpr uint64_t kGotWsWorldStreamExRva       = 0x7FAB60ULL; // g_worldStreamEx
constexpr uint64_t kGotWsRenderNonFadingRva     = 0x7FAC50ULL; // dvRenderNonFadingWorld (+0x3d u8)
constexpr uint64_t kGotWsRenderFadingRva        = 0x7FAC58ULL; // dvRenderFadingWorld (+0x3d u8)
constexpr uint64_t kGotWsSphereCullEnRva        = 0x7FACC0ULL; // gWorldStreamSphereCullingEnabled (+0x3d u8)
constexpr uint64_t kGotWsClipSphereFudgeRva     = 0x7FACC8ULL; // gClipSphereFudge (float)
constexpr uint64_t kGotWsLastSphereStateRva     = 0x7FACD0ULL; // Display::C_Culling::s_lastSphereCullState
constexpr uint64_t kGotWsSphereCullEn2Rva       = 0x7FE8F0ULL; // gSphereCullingEnabled (+0x3d u8)
constexpr uint64_t kGotWsCullIsPerspRva         = 0x7FE8A8ULL; // Display::C_Culling::s_bIsPerspective (u8)
constexpr uint64_t kGotWsCullLastPlaneRva       = 0x7FE8F8ULL; // Display::C_Culling::s_lastSpherePlaneTest (u32)

constexpr uint64_t kGotDvEnableWorldRva         = 0x7FE2E8ULL; // dvEnableDeferredWorldRenderer (+0x3d)
constexpr uint64_t kGotDvEnableModelRva         = 0x7FAC88ULL; // dvEnableDeferredModelRenderer (+0x3d)
constexpr uint64_t kGotWorldSortKeyRva          = 0x7FEBD8ULL; // dvWorldMaterialSortKey (int32)
constexpr uint64_t kGotModelSortKeyRva          = 0x7FEBF0ULL; // dvModelMaterialSortKey (int32)

constexpr uint64_t kGotGlBlendEnableRva         = 0x7FEC50ULL; // _glBlendEnable (uint8)
constexpr uint64_t kGotGlDepthTestEnableRva     = 0x7FEC58ULL; // _glDepthTestEnable (uint8)
constexpr uint64_t kGotGlBlendEquationRva       = 0x7FEC60ULL; // _glBlendEquation (uint8)
constexpr uint64_t kGotGlDepthMaskEnableRva     = 0x7FEC68ULL; // _glDepthMaskEnable (uint8)
constexpr uint64_t kGotGlBlendSepFuncSetRva     = 0x7FEC70ULL; // _glBlendSeparateFuncSet (uint8)
constexpr uint64_t kGotGlDepthOffsetRva         = 0x7FEBE8ULL; // _glDepthOffset (float)
constexpr uint64_t kGotCurrentPassStateRva      = 0x7FAA68ULL; // gCurrentPassState (uint32)
constexpr uint64_t kGotLgGlStateRva             = 0x7FEE08ULL; // LgGlAPI::lg_glState::s_glState
constexpr uint64_t kGotColorEqRva               = 0x7FF110ULL; // LgGlAPI::colorEq
constexpr uint64_t kGotAlphaEqRva               = 0x7FF118ULL; // LgGlAPI::alphaEq
constexpr uint64_t kGotFireTvHackRva            = 0x7FD240ULL; // gDoFireTVFirstGenHack (uint8)
constexpr uint64_t kGotLastZBiasRva             = 0x7FEDE8ULL; // Display::gLastZBias (float)
constexpr uint64_t kGotDrawCallCountRva         = 0x7FD260ULL; // Display::gDrawCallCount (uint32)
constexpr uint64_t kGotPendingRenderStateRva    = 0x7FC4B8ULL; // Display::gPendingRenderState

constexpr uint64_t kGotLoadingTextureRva        = 0x7FE9E8ULL; // gLoadingTexture
constexpr uint64_t kGotDynShadowTexRva          = 0x7FE438ULL; // _pDynamicShadowTexture
constexpr uint64_t kGotModelEnvMapRva           = 0x7FE430ULL; // gpModelEnvMap

constexpr uint64_t kGotWorldAmbientLightRva     = 0x7FEC28ULL; // _worldAmbientLight (vec4)
constexpr uint64_t kGotWorldEmissiveLightRva    = 0x7FEC30ULL; // _worldEmissiveLight (vec4)
constexpr uint64_t kGotModelFogParamsRva        = 0x7FEC18ULL; // _modelFogParams (vec4)
constexpr uint64_t kGotModelFogColourRva        = 0x7FEC10ULL; // _modelFogColour (vec4)
constexpr uint64_t kGotDynShadowCentreRva       = 0x7FEBF8ULL; // _dynamicShadowCentre (vec2)
constexpr uint64_t kGotShadowInnerRadRva        = 0x7FE2B0ULL; // dv_shadowInnerRadius (+0x4c float)
constexpr uint64_t kGotShadowOuterRadRva        = 0x7FE2B8ULL; // dv_shadowOuterRadius (+0x4c float)

constexpr uint64_t kGotWorldViewMatRva          = 0x7FEC08ULL; // _worldViewMatrix (mat4)
constexpr uint64_t kGotProjectionMatRva         = 0x7FEC00ULL; // _projectionMatrix (mat4)
constexpr uint64_t kGotModelSpecPowerRva        = 0x7FE0C8ULL; // dvModelSpecPower (+0x4c float)
constexpr uint64_t kGotModelSpecIntRva          = 0x7FE0D0ULL; // dvModelSpecIntensity (+0x4c float)
constexpr uint64_t kGotModelEmissiveColRva      = 0x7FEC20ULL; // _modelEmissiveLightColour (vec4)
constexpr uint64_t kGotSkyBottomRva             = 0x7FEC38ULL; // _skyBottom (vec4)
constexpr uint64_t kGotSkyTopRva                = 0x7FEC40ULL; // _skyTop (vec4)

constexpr size_t kDrawCallStride  = 0x58ULL;  // sizeof(BatchedWorld::C_DrawCall)
constexpr size_t kBatchStride     = 0xCF8ULL; // sizeof(BatchedModel::C_DeferredBatch)
constexpr uint32_t kMaxVsUniforms = 32;
constexpr uint32_t kMaxFsUniforms = 16;
constexpr uint32_t kMaxBones      = 64;

inline uint64_t read_got_ptr(uint64_t load_bias, uint64_t rva) {
    return *reinterpret_cast<const uint64_t*>(load_bias + rva);
}

inline uint64_t tree_next(uint64_t x) {
    const uint64_t r = *reinterpret_cast<const uint64_t*>(x + 8ULL);
    if (r) {
        x = r;
        for (;;) {
            const uint64_t l = *reinterpret_cast<const uint64_t*>(x);
            if (!l) break;
            x = l;
        }
        return x;
    }
    for (;;) {
        const uint64_t p = *reinterpret_cast<const uint64_t*>(x + 16ULL);
        if (*reinterpret_cast<const uint64_t*>(p) == x) return p;
        x = p;
    }
}

inline uint32_t get_tex_gl_id(uint64_t ctex_ptr, uint32_t loading_gl_id) {
    if (!ctex_ptr) return 0;
    const uint64_t impl = *reinterpret_cast<const uint64_t*>(ctex_ptr + 0xF0ULL);
    if (impl) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(impl + 4ULL);
        return id ? id : loading_gl_id;
    }
    return *reinterpret_cast<const uint32_t*>(ctex_ptr + 0xF8ULL);
}

inline int32_t lookup_prog_uniform_loc(uint64_t prog_ptr, uint32_t string_id) {
    const uint64_t root = *reinterpret_cast<const uint64_t*>(prog_ptr + 0x60ULL);
    uint64_t node = root;
    while (node) {
        const uint32_t k = *reinterpret_cast<const uint32_t*>(node + 0x1CULL);
        if (string_id < k) {
            node = *reinterpret_cast<const uint64_t*>(node);
        } else if (string_id > k) {
            node = *reinterpret_cast<const uint64_t*>(node + 8ULL);
        } else {
            const uint32_t val = *reinterpret_cast<const uint32_t*>(node + 0x20ULL);
            return val ? static_cast<int32_t>(val - 1u) : -1;
        }
    }
    return -1;
}

struct HostShaderState {
    uint32_t gl_prog = 0;
    int32_t  vs_loc[kMaxVsUniforms]{};
    int32_t  fs_loc[kMaxFsUniforms]{};
    uint32_t vs_off[kMaxVsUniforms]{};
    uint32_t vs_sz[kMaxVsUniforms]{};
    uint32_t fs_off[kMaxFsUniforms]{};
    uint32_t fs_sz[kMaxFsUniforms]{};
    bool     vs8_is_fog = false;
};

constexpr uint64_t kSymCWorldFadeStartRva  = 0x377F74ULL; // CWorld::Process sector fade loop start
constexpr uint64_t kGotMsSectorsRva        = 0x7F9838ULL; // CWorld::ms_aSectors
constexpr uint64_t kGotCutsceneRunRva      = 0x7F9010ULL; // CCutsceneMgr::ms_running
constexpr uint64_t kGotCameraFadeRva       = 0x7F9190ULL; // TheCamera (+0x85)
constexpr uint64_t kGotFadeInRateRva       = 0x7F9890ULL; // dv_fadeInRate (+0x4c)
constexpr uint64_t kGotFadeOutRateRva      = 0x7F9898ULL; // dv_fadeOutRate (+0x4c)

inline uint64_t run_cworld_fade_loop(uint64_t load_bias) {
    if (!load_bias) return 0;
    const uint64_t p_sec_var = *reinterpret_cast<const uint64_t*>(load_bias + kGotMsSectorsRva);
    if (!p_sec_var) return 0;
    const uint64_t sectors_base = *reinterpret_cast<const uint64_t*>(p_sec_var);
    if (sectors_base < 0x10000ULL) return 0;

    const uint64_t p_cut = *reinterpret_cast<const uint64_t*>(load_bias + kGotCutsceneRunRva);
    const uint64_t p_cam = *reinterpret_cast<const uint64_t*>(load_bias + kGotCameraFadeRva);
    const uint64_t p_fi  = *reinterpret_cast<const uint64_t*>(load_bias + kGotFadeInRateRva);
    const uint64_t p_fo  = *reinterpret_cast<const uint64_t*>(load_bias + kGotFadeOutRateRva);

    const uint8_t cut_run = p_cut ? *reinterpret_cast<const uint8_t*>(p_cut) : 0u;
    const uint8_t cam_ovr = p_cam ? *reinterpret_cast<const uint8_t*>(p_cam + 0x85ULL) : 0u;
    const uint8_t override_opaque = cut_run | cam_ovr;
    const int32_t fade_in  = p_fi ? *reinterpret_cast<const int32_t*>(p_fi + 0x4CULL) : 15;
    const int32_t fade_out = p_fo ? *reinterpret_cast<const int32_t*>(p_fo + 0x4CULL) : 15;

    static constexpr size_t kSectorLists[8] = {0x38, 0x40, 0x28, 0x30, 0x18, 0x20, 0x48, 0x50};
    for (size_t s = 0; s < 10000; ++s) {
        const uint64_t sec_ptr = sectors_base + s * 0x58ULL;
        for (size_t l = 0; l < 8; ++l) {
            uint64_t node = *reinterpret_cast<const uint64_t*>(sec_ptr + kSectorLists[l]);
            while (node >= 0x10000ULL) {
                const uint64_t entity = *reinterpret_cast<const uint64_t*>(node);
                node = *reinterpret_cast<const uint64_t*>(node + 0x10ULL);
                if (entity < 0x10000ULL) continue;

                const uint64_t rsl = *reinterpret_cast<const uint64_t*>(entity + 0x70ULL);
                int32_t cur_alpha = 0xff;
                uint8_t obj_type = 0;
                uint64_t first_atomic = 0;
                uint64_t head = 0;
                if (rsl >= 0x10000ULL) {
                    obj_type = *reinterpret_cast<const uint8_t*>(rsl);
                    if (obj_type == 2u) {
                        head = rsl + 0x10ULL;
                        first_atomic = *reinterpret_cast<const uint64_t*>(head);
                        if (first_atomic != head && first_atomic >= 0x10000ULL) {
                            cur_alpha = *reinterpret_cast<const int32_t*>(first_atomic + 0xA8ULL);
                        }
                    } else {
                        cur_alpha = *reinterpret_cast<const int32_t*>(rsl + 0xE0ULL);
                    }
                }

                int32_t new_alpha = cur_alpha;
                if (override_opaque) {
                    new_alpha = 255;
                } else if ((*reinterpret_cast<const uint8_t*>(entity + 0x5DULL) & 1u) != 0u) {
                    const int32_t sub = cur_alpha - fade_out;
                    new_alpha = (sub < 0) ? 0 : sub;
                } else if (cur_alpha <= 254) {
                    const int32_t sum = cur_alpha + fade_in;
                    new_alpha = (sum < 255) ? sum : 255;
                }

                if (rsl >= 0x10000ULL) {
                    if (obj_type == 2u) {
                        for (uint64_t it = first_atomic; it != head && it >= 0x10000ULL;
                             it = *reinterpret_cast<const uint64_t*>(it)) {
                            auto* ap = reinterpret_cast<int32_t*>(it + 0xA8ULL);
                            if (*ap != new_alpha) *ap = new_alpha;
                        }
                    } else {
                        auto* ap = reinterpret_cast<int32_t*>(rsl + 0xE0ULL);
                        if (*ap != new_alpha) *ap = new_alpha;
                    }
                }
            }
        }
    }
    return 0;
}

} // namespace

struct LcsBatchedFast::Impl {
    lcs::cpu::DynarmicCore* cpu = nullptr;
    uint64_t load_bias = 0;
    bool     initialized = false;

    uint64_t orig_world_render = 0;
    uint64_t orig_model_render = 0;
    uint64_t orig_world_get_free = 0;
    uint64_t orig_world_reset = 0;
    uint64_t orig_model_get_free = 0;
    uint64_t orig_model_reset = 0;
    uint64_t orig_ws_render_nocull = 0;
    uint64_t tramp_ws_orig = 0;
    uint64_t fn_world_reset = 0;
    uint64_t fn_model_reset = 0;
    uint64_t fn_get_vs_sid = 0;
    uint64_t fn_get_fs_sid = 0;
    uint64_t fn_culling_update = 0;
    uint64_t fn_ws_set_list_states = 0;
    uint64_t fn_ws_unset_list_states = 0;
    uint64_t fn_ws_render_alpha = 0;
    uint64_t fn_set_blend_mode_pass = 0;
    uint64_t fn_perf_sphere_cull = 0;
    uint64_t fn_sgeom_ctor = 0;
    bool     dyn_inst_ready = false;
    std::array<uint8_t, 0x78> dyn_inst_template{};
    std::array<uint8_t, 0x78> dyn_inst_scratch{};

    struct WsFadeEntry {
        uint64_t mesh = 0;
        uint32_t inst_id = 0;
        uint32_t last_seq = 0;
        float    alpha = 1.0f;
    };
    struct WsFadeMap {
        std::vector<WsFadeEntry> slots;
        size_t count = 0;

        static inline size_t hash_key(uint64_t mesh, uint32_t inst_id, size_t mask) {
            uint64_t h = mesh * 0x9E3779B97F4A7C15ULL;
            h ^= static_cast<uint64_t>(inst_id) * 0xBF58476D1CE4E5B9ULL;
            h ^= h >> 30;
            h *= 0x94D049BB133111EBULL;
            h ^= h >> 27;
            return static_cast<size_t>(h) & mask;
        }
        void rehash(size_t new_cap, uint32_t min_keep_seq) {
            std::vector<WsFadeEntry> next(new_cap);
            const size_t mask = new_cap - 1;
            size_t new_count = 0;
            for (const auto& e : slots) {
                if (!e.mesh || e.last_seq < min_keep_seq) continue;
                size_t idx = hash_key(e.mesh, e.inst_id, mask);
                while (next[idx].mesh) idx = (idx + 1) & mask;
                next[idx] = e;
                ++new_count;
            }
            slots.swap(next);
            count = new_count;
        }
        inline float& get_or_create(uint64_t mesh, uint32_t inst_id, uint32_t cur_seq) {
            if (slots.empty()) slots.resize(16384);
            if ((count + 1) * 2 >= slots.size()) {
                const uint32_t min_seq = (cur_seq > 120u) ? (cur_seq - 120u) : 0u;
                rehash(slots.size(), min_seq);
                if ((count + 1) * 2 >= slots.size()) {
                    rehash(slots.size() * 2, min_seq);
                }
            }
            const size_t mask = slots.size() - 1;
            size_t idx = hash_key(mesh, inst_id, mask);
            while (slots[idx].mesh) {
                if (slots[idx].mesh == mesh && slots[idx].inst_id == inst_id) {
                    if (cur_seq > slots[idx].last_seq + 120u) {
                        slots[idx].alpha = 1.0f;
                    }
                    slots[idx].last_seq = cur_seq;
                    return slots[idx].alpha;
                }
                idx = (idx + 1) & mask;
            }
            slots[idx] = WsFadeEntry{mesh, inst_id, cur_seq, 1.0f};
            ++count;
            return slots[idx].alpha;
        }
    } ws_fade_map;
    uint32_t ws_frame_seq = 1u;

    struct FastBatchMeta {
        uint64_t shader_ptr;
        uint64_t tex_ptr;
        uint64_t vbo_wrap;
        uint32_t pass_state;
        uint32_t pad;
    };
    struct GuestArenaCtrl {
        uint32_t world_count;
        uint32_t world_cap;
        uint32_t world_overflow;
        uint32_t pad0;
        uint64_t world_calls_ptr;
        uint64_t world_meta_ptr;
        uint32_t model_count;
        uint32_t model_cap;
        uint32_t model_overflow;
        uint32_t pad1;
        uint64_t model_batches_ptr;
        uint64_t model_meta_ptr;
    };
    struct SortEntry {
        uint64_t shader_ptr;
        uint32_t pass_state;
        uint32_t shader_key;
        uint32_t tex0;
        uint32_t vbo;
        uint32_t ibo;
        uint32_t idx;
    };
    static constexpr uint32_t kArenaWorldCap = 32768u;
    static constexpr uint32_t kArenaModelCap = 16384u;

    uint8_t*        arena_code_page = nullptr;
    GuestArenaCtrl* arena_ctrl = nullptr;
    uint8_t*        arena_world_calls = nullptr;
    FastBatchMeta*  arena_world_meta = nullptr;
    uint8_t*        arena_model_batches = nullptr;
    FastBatchMeta*  arena_model_meta = nullptr;
    bool            arena_enabled = false;
    bool            logged_arena_overflow = false;
    std::vector<SortEntry> sort_entries;

    ~Impl() {
#if defined(LCS_PLATFORM_WINDOWS)
        if (arena_code_page)     VirtualFree(arena_code_page, 0, MEM_RELEASE);
        if (arena_ctrl)          VirtualFree(arena_ctrl, 0, MEM_RELEASE);
        if (arena_world_calls)   VirtualFree(arena_world_calls, 0, MEM_RELEASE);
        if (arena_world_meta)    VirtualFree(arena_world_meta, 0, MEM_RELEASE);
        if (arena_model_batches) VirtualFree(arena_model_batches, 0, MEM_RELEASE);
        if (arena_model_meta)    VirtualFree(arena_model_meta, 0, MEM_RELEASE);
#endif
    }

    // Resolved guest global pointers
    uint64_t g_dv_enable_world = 0;
    uint64_t g_dv_enable_model = 0;
    uint64_t g_world_sort_key = 0;
    uint64_t g_model_sort_key = 0;
    uint64_t g_blend_en = 0;
    uint64_t g_depth_test_en = 0;
    uint64_t g_blend_eq = 0;
    uint64_t g_depth_mask_en = 0;
    uint64_t g_blend_sep_en = 0;
    uint64_t g_depth_offset = 0;
    uint64_t g_cur_pass_state = 0;
    uint64_t g_s_gl_state = 0;
    uint64_t g_color_eq = 0;
    uint64_t g_alpha_eq = 0;
    uint64_t g_firetv_hack = 0;
    uint64_t g_last_zbias = 0;
    uint64_t g_draw_call_count = 0;
    uint64_t g_pending_render_state = 0;
    uint64_t g_loading_tex = 0;
    uint64_t g_dyn_shadow_tex = 0;
    uint64_t g_model_env_map = 0;

    uint64_t g_world_ambient = 0;
    uint64_t g_world_emissive = 0;
    uint64_t g_fog_params = 0;
    uint64_t g_fog_colour = 0;
    uint64_t g_dyn_shadow_centre = 0;
    uint64_t g_shadow_inner_rad = 0;
    uint64_t g_shadow_outer_rad = 0;

    uint64_t g_world_view_mat = 0;
    uint64_t g_proj_mat = 0;
    uint64_t g_model_spec_pow = 0;
    uint64_t g_model_spec_int = 0;
    uint64_t g_model_emissive = 0;
    uint64_t g_sky_bottom = 0;
    uint64_t g_sky_top = 0;

    uint64_t g_ws_pvs_enable = 0;
    uint64_t g_ws_pvs_precomp = 0;
    uint64_t g_ws_pvs_cam_zone_ptr = 0;
    uint64_t g_ws_pvs_vis_zones = 0;
    uint64_t g_ws_world_mgr_ptr = 0;
    uint64_t g_ws_force_nontrans = 0;
    uint64_t g_ws_render_trans = 0;
    uint64_t g_ws_render_nontrans = 0;
    uint64_t g_ws_aabb_cull = 0;
    uint64_t g_ws_mesh_aabb_cull = 0;
    uint64_t g_ws_global_aabb_cull = 0;
    uint64_t g_ws_cull_model_mat = 0;
    uint64_t g_ws_cull_change_flag = 0;
    uint64_t g_ws_cull_frustum = 0;
    uint64_t g_ws_cull_fade_enable = 0;
    uint64_t g_ws_cull_fade_rate = 0;
    uint64_t g_ws_cull_ret_small = 0;
    uint64_t g_ws_the_camera = 0;
    uint64_t g_ws_cur_view_offs = 0;
    uint64_t g_ws_detail_settings = 0;
    uint64_t g_ws_prefs_dyn_shadows = 0;
    uint64_t g_ws_recv_shadows_rad = 0;
    uint64_t g_ws_dyn_shadow_target = 0;
    uint64_t g_ws_dyn_shadow_active = 0;
    uint64_t g_ws_dbg_shader = 0;
    uint64_t g_ws_render_alpha = 0;
    uint64_t g_ws_render_fade = 0;
    uint64_t g_ws_current_pass = 0;
    uint64_t g_ws_enable_deferred = 0;
    uint64_t g_ws_tex_zbias = 0;
    uint64_t g_ws_shadow_strength = 0;
    uint64_t g_ws_current_depth_offs = 0;
    uint64_t g_ws_draw_count = 0;
    uint64_t g_ws_ex = 0;
    uint64_t g_ws_render_nonfading = 0;
    uint64_t g_ws_render_fading = 0;
    uint64_t g_ws_sphere_cull_en = 0;
    uint64_t g_ws_clip_sphere_fudge = 0;
    uint64_t g_ws_last_sphere_state = 0;
    uint64_t g_ws_sphere_cull_en2 = 0;
    uint64_t g_ws_cull_is_persp = 0;
    uint64_t g_ws_cull_last_plane = 0;

    bool     uniform_sids_ready = false;
    uint32_t vs_sid[kMaxVsUniforms]{};
    uint32_t fs_sid[kMaxFsUniforms]{};

    std::mutex mtx;

    struct FlatShaderSlot {
        uint64_t ptr = 0;
        HostShaderState* state = nullptr;
    };
    struct FlatShaderMap {
        std::vector<FlatShaderSlot> slots;
        size_t count = 0;

        ~FlatShaderMap() {
            for (auto& s : slots) {
                delete s.state;
            }
        }
        void reserve(size_t n) {
            size_t cap = 128;
            while (cap < n * 2) cap <<= 1;
            if (slots.size() < cap) rehash(cap);
        }
        void rehash(size_t new_cap) {
            std::vector<FlatShaderSlot> next(new_cap);
            const size_t mask = new_cap - 1;
            for (const auto& s : slots) {
                if (!s.ptr) continue;
                size_t idx = static_cast<size_t>(((s.ptr >> 4) * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
                while (next[idx].ptr) idx = (idx + 1) & mask;
                next[idx] = s;
            }
            slots.swap(next);
        }
        HostShaderState& get_or_create(uint64_t key, bool& inserted) {
            if (slots.empty()) reserve(64);
            if ((count + 1) * 2 >= slots.size()) rehash(slots.size() * 2);
            const size_t mask = slots.size() - 1;
            size_t idx = static_cast<size_t>(((key >> 4) * 0x9E3779B97F4A7C15ULL) >> 32) & mask;
            while (slots[idx].ptr != 0) {
                if (slots[idx].ptr == key) {
                    inserted = false;
                    return *slots[idx].state;
                }
                idx = (idx + 1) & mask;
            }
            auto* st = new HostShaderState{};
            slots[idx].ptr = key;
            slots[idx].state = st;
            ++count;
            inserted = true;
            return *st;
        }
    } shaders;

    struct VaoSlot {
        uint64_t shader_ptr = 0;
        uint32_t vbo_id     = 0;
        uint32_t ibo_id     = 0;
        uint32_t vao        = 0;
        uint32_t gen        = 0;
    };
    struct FlatVaoMap {
        std::vector<VaoSlot> slots;
        std::vector<uint32_t> active_vaos;
        size_t   count = 0;
        uint32_t gen   = 1;

        static inline size_t hash_slot(uint64_t sp, uint32_t vbo, uint32_t ibo, size_t mask) {
            uint64_t h = sp * 0x9E3779B97F4A7C15ULL;
            h ^= (static_cast<uint64_t>(vbo) << 32) | ibo;
            h ^= h >> 30;
            h *= 0xBF58476D1CE4E5B9ULL;
            h ^= h >> 27;
            return static_cast<size_t>(h) & mask;
        }
        void reserve(size_t n) {
            size_t cap = 16384;
            while (cap < n * 2) cap <<= 1;
            if (slots.size() < cap) rehash(cap);
        }
        void rehash(size_t new_cap) {
            std::vector<VaoSlot> next(new_cap);
            const size_t mask = new_cap - 1;
            for (const auto& s : slots) {
                if (s.gen != gen) continue;
                size_t idx = hash_slot(s.shader_ptr, s.vbo_id, s.ibo_id, mask);
                while (next[idx].gen == gen) idx = (idx + 1) & mask;
                next[idx] = s;
            }
            slots.swap(next);
        }
        inline uint32_t find(uint64_t sp, uint32_t vbo, uint32_t ibo) const {
            if (slots.empty()) return 0;
            const size_t mask = slots.size() - 1;
            size_t idx = hash_slot(sp, vbo, ibo, mask);
            while (slots[idx].gen == gen) {
                if (slots[idx].shader_ptr == sp && slots[idx].vbo_id == vbo && slots[idx].ibo_id == ibo) {
                    return slots[idx].vao;
                }
                idx = (idx + 1) & mask;
            }
            return 0;
        }
        void insert(uint64_t sp, uint32_t vbo, uint32_t ibo, uint32_t vao) {
            if (slots.empty()) reserve(4096);
            if ((count + 1) * 2 >= slots.size()) rehash(slots.size() * 2);
            const size_t mask = slots.size() - 1;
            size_t idx = hash_slot(sp, vbo, ibo, mask);
            while (slots[idx].gen == gen) {
                if (slots[idx].shader_ptr == sp && slots[idx].vbo_id == vbo && slots[idx].ibo_id == ibo) {
                    slots[idx].vao = vao;
                    return;
                }
                idx = (idx + 1) & mask;
            }
            slots[idx] = VaoSlot{sp, vbo, ibo, vao, gen};
            ++count;
            active_vaos.push_back(vao);
        }
        void clear(lcs::graphics::RenderBridge& rb) {
            if (!active_vaos.empty()) {
                rb.gl_delete_vertex_arrays(static_cast<int32_t>(active_vaos.size()), active_vaos.data());
                active_vaos.clear();
            }
            count = 0;
            ++gen;
            if (gen == 0) {
                std::fill(slots.begin(), slots.end(), VaoSlot{});
                gen = 1;
            }
        }
    } vaos;
    uint64_t last_buf_del_epoch = 0;

    void ensure_uniform_sids(lcs::cpu::DynarmicCore* cur_cpu) {
        if (uniform_sids_ready || !cur_cpu) return;
        for (uint32_t u = 0; u < kMaxVsUniforms; ++u) {
            vs_sid[u] = static_cast<uint32_t>(cur_cpu->call_nested(fn_get_vs_sid, {u}));
        }
        for (uint32_t u = 0; u < kMaxFsUniforms; ++u) {
            fs_sid[u] = static_cast<uint32_t>(cur_cpu->call_nested(fn_get_fs_sid, {u}));
        }
        uniform_sids_ready = true;
    }

    HostShaderState& get_shader_state(lcs::graphics::RenderBridge& rb, uint64_t shader_ptr) {
        const uint64_t prog_ptr = *reinterpret_cast<const uint64_t*>(shader_ptr + 0x18ULL);
        const uint32_t gl_prog  = prog_ptr ? *reinterpret_cast<const uint32_t*>(prog_ptr + 0x10ULL) : 0u;
        bool inserted = false;
        HostShaderState& hs = shaders.get_or_create(shader_ptr, inserted);
        if (inserted || hs.gl_prog != gl_prog || hs.gl_prog == 0) {
            hs = HostShaderState{};
            hs.gl_prog = gl_prog;
            const auto pv = gl_prog ? rb.get_batch_program_view(gl_prog)
                                    : lcs::graphics::RenderBridge::BatchProgramView{};
            for (uint32_t u = 0; u < kMaxVsUniforms; ++u) {
                hs.vs_loc[u] = (prog_ptr && vs_sid[u]) ? lookup_prog_uniform_loc(prog_ptr, vs_sid[u]) : -1;
                hs.vs_off[u] = UINT32_MAX;
                hs.vs_sz[u]  = 0u;
                if (gl_prog && hs.vs_loc[u] >= 0) {
                    rb.get_uniform_layout_offset(gl_prog, hs.vs_loc[u], hs.vs_off[u], hs.vs_sz[u]);
                }
            }
            for (uint32_t u = 0; u < kMaxFsUniforms; ++u) {
                hs.fs_loc[u] = (prog_ptr && fs_sid[u]) ? lookup_prog_uniform_loc(prog_ptr, fs_sid[u]) : -1;
                hs.fs_off[u] = UINT32_MAX;
                hs.fs_sz[u]  = 0u;
                if (gl_prog && hs.fs_loc[u] >= 0) {
                    rb.get_uniform_layout_offset(gl_prog, hs.fs_loc[u], hs.fs_off[u], hs.fs_sz[u]);
                }
            }
            hs.vs8_is_fog = (hs.vs_loc[8] >= 0 && hs.vs_loc[8] == pv.fog_params_loc);
        }
        return hs;
    }

    static inline void write_uniform_cached(const lcs::graphics::RenderBridge::BatchProgramView& pv,
                                            uint32_t off, uint32_t avail_sz, const void* src, uint32_t bytes) {
        if (!src || !pv.staging_data || off == UINT32_MAX ||
            bytes > avail_sz || off + bytes > pv.staging_size) {
            return;
        }
        uint8_t* dst = pv.staging_data + off;
        if (std::memcmp(dst, src, bytes) != 0) {
            std::memcpy(dst, src, bytes);
            ++(*pv.uniform_version);
        }
    }

    static inline void set_vs_mat4(const HostShaderState& hs,
                                   const lcs::graphics::RenderBridge::BatchProgramView& pv,
                                   uint32_t u, const float* mat) {
        if (u < kMaxVsUniforms) write_uniform_cached(pv, hs.vs_off[u], hs.vs_sz[u], mat, 64u);
    }

    static inline void set_vs_vec4(const HostShaderState& hs,
                                   const lcs::graphics::RenderBridge::BatchProgramView& pv,
                                   uint32_t u, const float* v) {
        if (u < kMaxVsUniforms) write_uniform_cached(pv, hs.vs_off[u], hs.vs_sz[u], v, 16u);
    }

    static inline void set_fs_vec4(const HostShaderState& hs,
                                   const lcs::graphics::RenderBridge::BatchProgramView& pv,
                                   uint32_t u, const float* v) {
        if (u < kMaxFsUniforms) write_uniform_cached(pv, hs.fs_off[u], hs.fs_sz[u], v, 16u);
    }

    uint32_t get_or_create_vao(lcs::graphics::RenderBridge& rb,
                               uint64_t shader_ptr, uint32_t vbo, uint32_t ibo,
                               uint32_t& cur_vao) {
        const uint64_t epoch = rb.buffer_delete_epoch();
        if (epoch != last_buf_del_epoch) {
            if (cur_vao != 0) {
                rb.gl_bind_vertex_array(0);
                cur_vao = 0;
            }
            vaos.clear(rb);
            last_buf_del_epoch = epoch;
        }

        if (const uint32_t cached = vaos.find(shader_ptr, vbo, ibo)) {
            return cached;
        }

        uint32_t vao = 0;
        rb.gl_gen_vertex_arrays(1, &vao);
        rb.gl_bind_vertex_array(vao);
        cur_vao = vao;
        rb.gl_bind_buffer(0x8892u, vbo); // GL_ARRAY_BUFFER

        const uint32_t num_attrs = *reinterpret_cast<const uint32_t*>(shader_ptr + 0x20ULL);
        const uint64_t attrs_ptr = *reinterpret_cast<const uint64_t*>(shader_ptr + 0x10ULL);
        for (uint32_t a = 0; a < num_attrs; ++a) {
            const uint8_t* ap = reinterpret_cast<const uint8_t*>(attrs_ptr + static_cast<uint64_t>(a) * 0x1CULL);
            const uint32_t idx  = *reinterpret_cast<const uint32_t*>(ap + 0x04ULL);
            const int32_t  sz   = *reinterpret_cast<const int32_t*>(ap + 0x08ULL);
            const uint32_t tp   = *reinterpret_cast<const uint32_t*>(ap + 0x0CULL);
            const uint8_t  norm = *reinterpret_cast<const uint8_t*>(ap + 0x10ULL);
            const int32_t  str  = *reinterpret_cast<const int32_t*>(ap + 0x14ULL);
            const uint32_t off  = *reinterpret_cast<const uint32_t*>(ap + 0x18ULL);
            if (idx < 16u) {
                rb.gl_enable_vertex_attrib_array(idx);
                rb.gl_vertex_attrib_pointer(idx, sz, tp, norm, str, static_cast<uint64_t>(off));
            }
        }
        rb.gl_bind_buffer(0x8893u, ibo); // GL_ELEMENT_ARRAY_BUFFER
        vaos.insert(shader_ptr, vbo, ibo, vao);
        return vao;
    }

    inline void reset_pass_caches() {
        if (g_blend_en)      *reinterpret_cast<uint8_t*>(g_blend_en)      = 0xFFu;
        if (g_depth_test_en) *reinterpret_cast<uint8_t*>(g_depth_test_en) = 0xFFu;
        if (g_blend_eq)      *reinterpret_cast<uint8_t*>(g_blend_eq)      = 0xFFu;
        if (g_depth_mask_en) *reinterpret_cast<uint8_t*>(g_depth_mask_en) = 0xFFu;
        if (g_blend_sep_en)  *reinterpret_cast<uint8_t*>(g_blend_sep_en)  = 0xFFu;
    }

    inline void apply_z_bias(lcs::graphics::RenderBridge& rb, float bias) {
        if (g_firetv_hack && *reinterpret_cast<const uint8_t*>(g_firetv_hack) != 0) return;
        if (!g_last_zbias) return;
        auto* last = reinterpret_cast<float*>(g_last_zbias);
        if (*last == bias) return;
        if (bias == 0.0f) {
            if (*last != 0.0f) rb.gl_disable(0x8037u);
        } else if (*last == 0.0f) {
            rb.gl_enable(0x8037u);
        }
        *last = bias;
    }

    void set_pass_state(lcs::graphics::RenderBridge& rb, uint32_t pass_state,
                        bool& blend_on, uint8_t& dmask_on, bool& depth_on, bool force_sync) {
        if (g_cur_pass_state) {
            *reinterpret_cast<uint32_t*>(g_cur_pass_state) = pass_state;
        }
        if (pass_state < 1 || pass_state > 7) return;

        const bool want_blend = (pass_state >= 3);
        const bool want_dmask = (pass_state <= 4);
        if (force_sync || blend_on != want_blend) {
            if (want_blend) rb.gl_enable(0x0BE2u); else rb.gl_disable(0x0BE2u);
            blend_on = want_blend;
        }
        if (want_blend) {
            rb.gl_blend_equation_separate(0x8006u, 0x8006u);
            if (g_color_eq) *reinterpret_cast<uint32_t*>(g_color_eq) = 0x8006u;
            if (g_alpha_eq) *reinterpret_cast<uint32_t*>(g_alpha_eq) = 0x8006u;
            if (pass_state == 5) {
                rb.gl_blend_func_separate(1u, 1u, 1u, 1u);
            } else if (pass_state == 7) {
                rb.gl_blend_func_separate(0x0302u, 1u, 1u, 0u);
            } else {
                rb.gl_blend_func_separate(0x0302u, 0x0303u, 1u, 0u);
            }
        }
        if (force_sync || !depth_on) {
            rb.gl_enable(0x0B71u); // GL_DEPTH_TEST
            depth_on = true;
        }
        if (force_sync || dmask_on != (want_dmask ? 1u : 0u)) {
            dmask_on = want_dmask ? 1u : 0u;
            rb.gl_depth_mask(dmask_on);
        }
    }

    inline void sync_guest_gl_state_after(uint32_t last_prog, bool blend_on,
                                          uint8_t dmask_on, bool is_model) {
        if (!g_s_gl_state) return;
        uint8_t* st = reinterpret_cast<uint8_t*>(g_s_gl_state);
        *reinterpret_cast<uint32_t*>(st + 0x04ULL) = 1u;
        *reinterpret_cast<uint32_t*>(st + 0x08ULL) = blend_on ? 1u : 0u;
        *reinterpret_cast<uint32_t*>(st + 0x0CULL) = dmask_on ? 1u : 0u;
        if (last_prog != 0) {
            *reinterpret_cast<uint32_t*>(st + 0x10ULL) = last_prog;
        }
        *reinterpret_cast<uint64_t*>(st + 0x18ULL) = 0ULL;
        *reinterpret_cast<uint64_t*>(st + 0x20ULL) = 0ULL;
        *reinterpret_cast<uint64_t*>(st + 0x28ULL) = 0ULL;
        if (is_model) {
            *(st + 0x38ULL) = 0u;
        }
        *reinterpret_cast<uint32_t*>(st + 0x44ULL) = 0u;
    }

    inline void invalidate_guest_pending_state(lcs::graphics::RenderBridge& rb) const {
        if (g_pending_render_state) {
            auto* prs = reinterpret_cast<uint8_t*>(g_pending_render_state);
            *reinterpret_cast<uint16_t*>(prs + 0x78ULL) = 0x0101u;
            *reinterpret_cast<uint64_t*>(prs + 0x38ULL) = 0ULL;
            *reinterpret_cast<uint32_t*>(prs + 0x40ULL) = 0u;
            *reinterpret_cast<uint64_t*>(prs + 0x48ULL) = 0ULL;
            *reinterpret_cast<uint32_t*>(prs + 0x50ULL) = 0u;
            *reinterpret_cast<uint64_t*>(prs + 0x58ULL) = 0ULL;
            *reinterpret_cast<uint32_t*>(prs + 0x60ULL) = 0u;
        }
        if (g_s_gl_state && *reinterpret_cast<const uint8_t*>(g_s_gl_state + 0x38ULL) != 0u) {
            rb.gl_bind_vertex_array(0);
            *reinterpret_cast<uint8_t*>(g_s_gl_state + 0x38ULL) = 0u;
        }
    }

    void reset_world_arena_entries(uint32_t mask) {
        if (!arena_enabled || !arena_ctrl) return;
        const uint32_t total = arena_ctrl->world_count;
        if (total == 0u || mask == 0u) {
            arena_ctrl->world_count = 0u;
            arena_ctrl->world_overflow = 0u;
            return;
        }
        uint32_t w = 0u;
        for (uint32_t i = 0u; i < total; ++i) {
            const uint32_t ps = arena_world_meta[i].pass_state;
            if (ps >= 1u && ps <= 32u && (((mask >> (ps - 1u)) & 1u) != 0u)) continue;
            if (w != i) {
                arena_world_meta[w] = arena_world_meta[i];
                std::memcpy(arena_world_calls + static_cast<size_t>(w) * kDrawCallStride,
                            arena_world_calls + static_cast<size_t>(i) * kDrawCallStride,
                            kDrawCallStride);
            }
            ++w;
        }
        arena_ctrl->world_count = w;
        arena_ctrl->world_overflow = 0u;
    }

    uint64_t reset_world_arena(uint32_t mask) {
        std::lock_guard<std::mutex> lk(mtx);
        reset_world_arena_entries(mask);
        invalidate_guest_pending_state(lcs::graphics::RenderBridge::instance());
        return 0ULL;
    }

    uint64_t reset_model_arena() {
        std::lock_guard<std::mutex> lk(mtx);
        if (arena_enabled && arena_ctrl) {
            arena_ctrl->model_count = 0u;
            arena_ctrl->model_overflow = 0u;
        }
        invalidate_guest_pending_state(lcs::graphics::RenderBridge::instance());
        return 0ULL;
    }

    inline void emit_world_draw_call(
        lcs::graphics::RenderBridge& rb,
        const HostShaderState& hs,
        const lcs::graphics::RenderBridge::BatchProgramView& pv,
        const uint8_t* p,
        uint64_t shader_ptr,
        uint32_t vbo,
        uint32_t ibo,
        uint32_t tex0,
        uint32_t& cur_tex0,
        uint32_t& cur_vao,
        uint32_t& active_unit) {
        if (cur_tex0 != tex0) {
            if (active_unit != 0u) {
                rb.gl_active_texture(0x84C0u);
                active_unit = 0u;
            }
            rb.gl_bind_texture(0x0DE1u, tex0);
            cur_tex0 = tex0;
        }
        set_vs_mat4(hs, pv, 3u,    *reinterpret_cast<const float* const*>(p + 0x00ULL));
        set_vs_mat4(hs, pv, 0x13u, *reinterpret_cast<const float* const*>(p + 0x18ULL));
        set_vs_mat4(hs, pv, 0u,    *reinterpret_cast<const float* const*>(p + 0x08ULL));
        set_vs_mat4(hs, pv, 2u,    *reinterpret_cast<const float* const*>(p + 0x10ULL));
        set_fs_vec4(hs, pv, 5u,    reinterpret_cast<const float*>(p + 0x40ULL));
        apply_z_bias(rb, *reinterpret_cast<const float*>(p + 0x50ULL));

        const uint32_t vao = get_or_create_vao(rb, shader_ptr, vbo, ibo, cur_vao);
        if (cur_vao != vao) {
            rb.gl_bind_vertex_array(vao);
            cur_vao = vao;
        }

        const uint32_t num_tris = *reinterpret_cast<const uint32_t*>(p + 0x38ULL);
        const uint32_t idx_off  = *reinterpret_cast<const uint32_t*>(p + 0x3CULL);
        rb.draw_elements_batched_compact(
            4u, static_cast<int32_t>(num_tris * 3u), 0x1403u,
            reinterpret_cast<const void*>(static_cast<uintptr_t>(idx_off) * 2u),
            pv.compact_bytes);
    }

    inline void emit_model_batch(
        lcs::graphics::RenderBridge& rb,
        const HostShaderState& hs,
        const lcs::graphics::RenderBridge::BatchProgramView& pv,
        const uint8_t* b,
        uint64_t shader_ptr,
        uint32_t vbo,
        uint32_t ibo,
        uint32_t tex0,
        uint32_t& cur_tex0,
        uint32_t& cur_vao,
        uint32_t& active_unit) {
        if (cur_tex0 != tex0) {
            if (active_unit != 0u) {
                rb.gl_active_texture(0x84C0u);
                active_unit = 0u;
            }
            rb.gl_bind_texture(0x0DE1u, tex0);
            cur_tex0 = tex0;
        }

        set_vs_mat4(hs, pv, 0x13u, *reinterpret_cast<const float* const*>(b + 0x38ULL));
        set_vs_vec4(hs, pv, 0x15u, reinterpret_cast<const float*>(b + 0x00ULL));

        uint32_t eff_cb_bytes = pv.compact_bytes;
        const uint32_t num_bones = *reinterpret_cast<const uint32_t*>(b + 0x60ULL);
        if (num_bones > 0 && hs.vs_off[0x1Au] != UINT32_MAX) {
            const uint32_t nb = std::min(num_bones, kMaxBones);
            const uint32_t bbytes = nb * 48u;
            const uint32_t off = hs.vs_off[0x1Au];
            write_uniform_cached(pv, off, hs.vs_sz[0x1Au], b + 0x64ULL, bbytes);
            if (off + bbytes <= pv.staging_size) {
                eff_cb_bytes = std::max(eff_cb_bytes, off + bbytes);
            }
        }

        const float uv19[4] = {
            *reinterpret_cast<const float*>(b + 0x30ULL),
            *reinterpret_cast<const float*>(b + 0x34ULL),
            0.0f, 0.0f
        };
        set_vs_vec4(hs, pv, 0x19u, uv19);

        for (uint32_t l = 0; l < 4u; ++l) {
            set_vs_vec4(hs, pv, 11u + l, reinterpret_cast<const float*>(b + 0xCB4ULL + l * 16ULL));
            set_vs_vec4(hs, pv, 15u + l, reinterpret_cast<const float*>(b + 0xC74ULL + l * 16ULL));
        }
        set_vs_vec4(hs, pv, 9u, reinterpret_cast<const float*>(b + 0xC64ULL));
        set_fs_vec4(hs, pv, 3u, reinterpret_cast<const float*>(b + 0x10ULL));
        set_fs_vec4(hs, pv, 4u, reinterpret_cast<const float*>(b + 0x20ULL));

        apply_z_bias(rb, *reinterpret_cast<const float*>(b + 0x5CULL));

        const uint32_t vao = get_or_create_vao(rb, shader_ptr, vbo, ibo, cur_vao);
        if (cur_vao != vao) {
            rb.gl_bind_vertex_array(vao);
            cur_vao = vao;
        }

        const uint32_t num_tris = *reinterpret_cast<const uint32_t*>(b + 0x58ULL);
        rb.draw_elements_batched_compact(4u, static_cast<int32_t>(num_tris * 3u), 0x1403u, nullptr, eff_cb_bytes);
    }

    static inline void sort_batch_entries(std::vector<SortEntry>& entries) {
        if (entries.size() <= 1u) return;
        std::sort(entries.begin(), entries.end(), [](const SortEntry& a, const SortEntry& b) {
            if (a.pass_state != b.pass_state) return a.pass_state < b.pass_state;
            if (a.shader_key != b.shader_key) return a.shader_key < b.shader_key;
            if (a.shader_ptr != b.shader_ptr) return a.shader_ptr < b.shader_ptr;
            if (a.tex0 != b.tex0) return a.tex0 < b.tex0;
            if (a.pass_state == 1u && a.vbo != b.vbo) return a.vbo < b.vbo;
            return a.idx < b.idx;
        });
    }

    static inline uint64_t find_tree_u64(uint64_t map_root_addr, uint64_t target_key) {
        uint64_t node = *reinterpret_cast<const uint64_t*>(map_root_addr);
        if (!node) return 0ULL;
        const uint64_t end_node = map_root_addr;
        uint64_t best = end_node;
        while (node) {
            const uint64_t key = *reinterpret_cast<const uint64_t*>(node + 0x20ULL);
            if (!(key < target_key)) {
                best = node;
                node = *reinterpret_cast<const uint64_t*>(node);
            } else {
                node = *reinterpret_cast<const uint64_t*>(node + 8ULL);
            }
        }
        if (best != end_node && *reinterpret_cast<const uint64_t*>(best + 0x20ULL) == target_key) {
            return *reinterpret_cast<const uint64_t*>(best + 0x28ULL);
        }
        return 0ULL;
    }

    inline bool ws_pvs_visible(uint64_t inst) const {
        if (!g_ws_pvs_enable || *reinterpret_cast<const uint8_t*>(g_ws_pvs_enable + 0x3DULL) == 0) return true;
        if (!g_ws_pvs_cam_zone_ptr || *reinterpret_cast<const uint64_t*>(g_ws_pvs_cam_zone_ptr) == 0) return true;
        if (!g_ws_pvs_precomp || *reinterpret_cast<const uint8_t*>(g_ws_pvs_precomp + 0x3DULL) == 0) return true;
        if (*reinterpret_cast<const int32_t*>(inst + 0x2CULL) < 0) return true;
        if (!g_ws_pvs_vis_zones) return true;
        const uint64_t begin = *reinterpret_cast<const uint64_t*>(g_ws_pvs_vis_zones);
        const uint64_t end   = *reinterpret_cast<const uint64_t*>(g_ws_pvs_vis_zones + 8ULL);
        if (begin >= end) return true;
        const auto* zones = reinterpret_cast<const uint32_t*>(begin);
        const size_t n = static_cast<size_t>((end - begin) >> 2);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t z = zones[i];
            const uint32_t w = z >> 5;
            if (w <= 7u) {
                const uint32_t mask = *reinterpret_cast<const uint32_t*>(inst + 0x10ULL + static_cast<uint64_t>(w) * 4ULL);
                if (((mask >> (z & 31u)) & 1u) != 0u) return true;
            }
        }
        return false;
    }

    inline int ws_cull_aabb(const float* m, float min_x, float min_y, float min_z,
                            float max_x, float max_y, float max_z) const {
        if (!g_ws_cull_frustum) return 2;
        const float cx = 0.5f * (min_x + max_x);
        const float cy = 0.5f * (min_y + max_y);
        const float cz = 0.5f * (min_z + max_z);
        const float ex = 0.5f * (max_x - min_x);
        const float ey = 0.5f * (max_y - min_y);
        const float ez = 0.5f * (max_z - min_z);
        const float wx = cx * m[0] + cy * m[4] + cz * m[8]  + m[12];
        const float wy = cx * m[1] + cy * m[5] + cz * m[9]  + m[13];
        const float wz = cx * m[2] + cy * m[6] + cz * m[10] + m[14];

        const uint8_t* fr = reinterpret_cast<const uint8_t*>(g_ws_cull_frustum);
        int state = 2;
        for (int i = 0; i < 6; ++i) {
            const float* pt = reinterpret_cast<const float*>(fr + 0x20ULL + static_cast<size_t>(i) * 0x20ULL);
            const float* nm = reinterpret_cast<const float*>(fr + 0x30ULL + static_cast<size_t>(i) * 0x20ULL);
            const float nx = nm[0], ny = nm[1], nz = nm[2];
            const float mx = m[0] * nx + m[1] * ny + m[2] * nz;
            const float my = m[4] * nx + m[5] * ny + m[6] * nz;
            const float mz = m[8] * nx + m[9] * ny + m[10] * nz;
            const float radius = ex * std::fabs(mx) + ey * std::fabs(my) + ez * std::fabs(mz);
            const float cdist = (wx - pt[0]) * nx + (wy - pt[1]) * ny + (wz - pt[2]) * nz;
            if (cdist - radius > 0.0f) return 0;
            if (cdist + radius >= 0.0f) state = 1;
        }
        return state;
    }

    inline bool ws_calc_screen_size(const float* m, int16_t min_x, int16_t min_y, int16_t min_z,
                                    int16_t max_x, int16_t max_y, int16_t max_z) const {
        if (!g_ws_the_camera || !g_ws_cur_view_offs || !g_ws_detail_settings) return true;
        constexpr float kInv32767 = 1.0f / 32767.0f;
        const float lx0 = static_cast<float>(min_x) * kInv32767;
        const float ly0 = static_cast<float>(min_y) * kInv32767;
        const float lz0 = static_cast<float>(min_z) * kInv32767;
        const float lx1 = static_cast<float>(max_x) * kInv32767;
        const float ly1 = static_cast<float>(max_y) * kInv32767;
        const float lz1 = static_cast<float>(max_z) * kInv32767;

        const float tx0 = m[0] * lx0 + m[4] * ly0 + m[8]  * lz0 + m[12];
        const float ty0 = m[1] * lx0 + m[5] * ly0 + m[9]  * lz0 + m[13];
        const float tz0 = m[2] * lx0 + m[6] * ly0 + m[10] * lz0 + m[14];
        const float tx1 = m[0] * lx1 + m[4] * ly1 + m[8]  * lz1 + m[12];
        const float ty1 = m[1] * lx1 + m[5] * ly1 + m[9]  * lz1 + m[13];
        const float tz1 = m[2] * lx1 + m[6] * ly1 + m[10] * lz1 + m[14];

        const float rad = std::max(std::fabs(tx0 - tx1), std::max(std::fabs(ty0 - ty1), std::fabs(tz0 - tz1)));
        const float cx = 0.5f * (tx0 + tx1);
        const float cy = 0.5f * (ty0 + ty1);
        const float cz = 0.5f * (tz0 + tz1);

        const uint8_t* cam = reinterpret_cast<const uint8_t*>(g_ws_the_camera);
        const uint8_t act_cam = *(cam + 0x8FULL);
        const float* cam_front = reinterpret_cast<const float*>(cam + static_cast<size_t>(act_cam) * 0x298ULL + 0x33CULL);
        const float* cam_pos   = reinterpret_cast<const float*>(cam + 0x30ULL);
        const float* view_offs = reinterpret_cast<const float*>(g_ws_cur_view_offs);

        const float vx = (cam_pos[0] - (cx - cam_front[0] * rad)) + view_offs[0];
        const float vy = (cam_pos[1] - (cy - cam_front[1] * rad)) + view_offs[1];
        const float vz = (cam_pos[2] - (cz - cam_front[2] * rad)) + view_offs[2];
        float dist = std::sqrt(vx * vx + vy * vy + vz * vz);
        if (dist == 0.0f) dist = 0.001f;

        const uint8_t* dds = reinterpret_cast<const uint8_t*>(g_ws_detail_settings);
        const int32_t level = *reinterpret_cast<const int32_t*>(dds + 0x30ULL);
        const float* cfg = reinterpret_cast<const float*>(dds + (static_cast<int64_t>(level) << 4));
        if (dist < cfg[0]) return true;
        const float scr_sz = rad / dist;
        const float thresh = cfg[3] + (cfg[2] - cfg[3]) * ((dist - cfg[0]) / cfg[1]);
        return scr_sz > thresh;
    }

    inline uint64_t call_ws_orig(uint64_t ws_ex, uint64_t ws, uint64_t inst,
                                 uint64_t check_aabb, uint64_t alpha_bits) const {
        if (!tramp_ws_orig) return 0ULL;
        if (auto* cur_cpu = lcs::cpu::DynarmicCore::current()) {
            cur_cpu->call_nested(tramp_ws_orig, {ws_ex, ws, inst, check_aabb, alpha_bits});
        }
        return 0ULL;
    }

    uint64_t ws_render_nocull(uint64_t ws_ex, uint64_t ws, uint64_t inst,
                              uint32_t check_aabb, uint32_t alpha_bits) {
        if (*reinterpret_cast<const uint8_t*>(ws_ex + 0x28ULL) == 0u) return 0ULL;
        if (!arena_enabled || !arena_ctrl || !g_ws_enable_deferred ||
            *reinterpret_cast<const uint8_t*>(g_ws_enable_deferred + 0x3DULL) == 0u) {
            return call_ws_orig(ws_ex, ws, inst, check_aabb, alpha_bits);
        }
        if (!ws_pvs_visible(inst)) return 0ULL;

        const uint64_t world_mgr = g_ws_world_mgr_ptr
            ? *reinterpret_cast<const uint64_t*>(g_ws_world_mgr_ptr) : 0ULL;
        if (!world_mgr) return call_ws_orig(ws_ex, ws, inst, check_aabb, alpha_bits);

        const uint32_t world_id = *reinterpret_cast<const uint32_t*>(ws + 0x14ULL);
        const uint16_t sec_idx  = *reinterpret_cast<const uint16_t*>(inst + 0x02ULL);
        const uint64_t key      = (static_cast<uint64_t>(world_id) << 32) | sec_idx;

        uint64_t model = 0ULL;
        const bool force_nt = g_ws_force_nontrans &&
                              (*reinterpret_cast<const uint8_t*>(g_ws_force_nontrans + 0x3DULL) != 0u);
        if (!force_nt) {
            const uint64_t trans = find_tree_u64(world_mgr + 8ULL, key);
            if (trans) {
                if (*reinterpret_cast<const uint8_t*>(trans + 0x14ULL) != 0u) {
                    model = trans;
                } else {
                    return call_ws_orig(ws_ex, ws, inst, check_aabb, alpha_bits);
                }
            }
        }
        if (!model) {
            model = find_tree_u64(world_mgr + 8ULL, key | (1ULL << 63));
        }
        if (!model) return call_ws_orig(ws_ex, ws, inst, check_aabb, alpha_bits);

        const uint8_t is_trans = *reinterpret_cast<const uint8_t*>(model);
        if (is_trans != 0u) {
            if (!g_ws_render_trans || *reinterpret_cast<const uint8_t*>(g_ws_render_trans + 0x3DULL) == 0u) {
                return 0ULL;
            }
        } else {
            if (!g_ws_render_nontrans || *reinterpret_cast<const uint8_t*>(g_ws_render_nontrans + 0x3DULL) == 0u) {
                return 0ULL;
            }
        }
        if (*reinterpret_cast<const uint32_t*>(model + 0x10ULL) != 0u) return 0ULL;

        const uint32_t num_meshes = *reinterpret_cast<const uint32_t*>(model + 0x04ULL);
        if (num_meshes == 0u) return 0ULL;
        const uint64_t* const meshes = *reinterpret_cast<const uint64_t* const*>(model + 0x08ULL);
        if (!meshes) return 0ULL;

        const uint64_t vb_wrap = *reinterpret_cast<const uint64_t*>(model + 0x20ULL);
        const uint64_t ib_wrap = *reinterpret_cast<const uint64_t*>(model + 0x28ULL);

        // If any submesh needs cold fallback texture allocation, run original path cleanly
        for (uint32_t i = 0; i < num_meshes; ++i) {
            const uint64_t mesh = meshes[i];
            if (!mesh) continue;
            const uint64_t* const tp = *reinterpret_cast<const uint64_t* const*>(mesh + 0x10ULL);
            const uint64_t tex = tp ? tp[0] : 0ULL;
            if (tex) {
                const uint64_t img = *reinterpret_cast<const uint64_t*>(tex + 0xF0ULL);
                if (img && *reinterpret_cast<const uint32_t*>(img + 4ULL) == 0u && tp[1] == 0ULL) {
                    return call_ws_orig(ws_ex, ws, inst, check_aabb, alpha_bits);
                }
            }
        }

        const uint64_t p_mat = *reinterpret_cast<const uint64_t*>(inst + 0x30ULL);
        const uint64_t inst_mat_addr = p_mat ? p_mat : (inst + 0x38ULL);
        const float* m = reinterpret_cast<const float*>(inst_mat_addr);

        const bool global_aabb = g_ws_global_aabb_cull &&
                                 (*reinterpret_cast<const uint8_t*>(g_ws_global_aabb_cull + 0x3DULL) != 0u);
        const bool do_model_aabb = global_aabb && ((check_aabb & 1u) != 0u) &&
                                   g_ws_aabb_cull && (*reinterpret_cast<const uint8_t*>(g_ws_aabb_cull + 0x3DULL) != 0u);
        if (do_model_aabb && g_ws_cull_change_flag &&
            (*reinterpret_cast<const uint32_t*>(g_ws_cull_change_flag) & 0xCu) != 0u) {
            if (g_ws_cull_model_mat && (*reinterpret_cast<const uint32_t*>(g_ws_cull_change_flag) & 2u) != 0u) {
                std::memcpy(reinterpret_cast<void*>(g_ws_cull_model_mat), m, 64);
            }
            if (auto* cur_cpu = lcs::cpu::DynarmicCore::current()) {
                cur_cpu->call_nested(fn_culling_update, {0ULL});
            }
        }

        int model_cull_state = 2;
        if (do_model_aabb) {
            model_cull_state = ws_cull_aabb(m, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f);
            if (model_cull_state == 0) return 0ULL;
        }
        const bool do_mesh_aabb = global_aabb && (model_cull_state == 1) &&
                                  g_ws_mesh_aabb_cull && (*reinterpret_cast<const uint8_t*>(g_ws_mesh_aabb_cull + 0x3DULL) != 0u);

        float base_alpha = 1.0f;
        std::memcpy(&base_alpha, &alpha_bits, sizeof(float));

        const bool cull_fade_en = g_ws_cull_fade_enable &&
                                  (*reinterpret_cast<const uint8_t*>(g_ws_cull_fade_enable + 0x3DULL) != 0u);
        const bool ret_on_small = g_ws_cull_ret_small &&
                                  (*reinterpret_cast<const uint8_t*>(g_ws_cull_ret_small + 0x3DULL) != 0u);
        const float fade_rate = g_ws_cull_fade_rate
            ? *reinterpret_cast<const float*>(g_ws_cull_fade_rate + 0x4CULL) : 0.1f;
        const uint32_t inst_id = static_cast<uint32_t>(*reinterpret_cast<const uint16_t*>(inst) & 0x7FFFu);

        uint32_t sh_idx = 0u;
        if (g_ws_prefs_dyn_shadows && *reinterpret_cast<const uint8_t*>(g_ws_prefs_dyn_shadows) != 0u) {
            const float recv_rad = g_ws_recv_shadows_rad
                ? *reinterpret_cast<const float*>(g_ws_recv_shadows_rad + 0x4CULL) : 0.0f;
            if (recv_rad <= 0.0f) {
                sh_idx = 1u;
            } else if (g_ws_dyn_shadow_target && g_ws_cur_view_offs) {
                const float* st = reinterpret_cast<const float*>(g_ws_dyn_shadow_target);
                const float* vo = reinterpret_cast<const float*>(g_ws_cur_view_offs);
                const float dx = st[0] + vo[0] - m[12];
                const float dy = st[1] + vo[1] - m[13];
                const float dz = st[2] + vo[2] - m[14];
                sh_idx = ((dx * dx + dy * dy + dz * dz) < (recv_rad * recv_rad)) ? 1u : 0u;
            }
        }

        const bool shadow_active = g_ws_dyn_shadow_active &&
                                   (*reinterpret_cast<const uint8_t*>(g_ws_dyn_shadow_active) != 0u);
        const int32_t dbg_sh = g_ws_dbg_shader ? *reinterpret_cast<const int32_t*>(g_ws_dbg_shader) : 0;
        const uint8_t rnd_alpha = g_ws_render_alpha ? *reinterpret_cast<const uint8_t*>(g_ws_render_alpha) : 0u;
        const uint32_t cur_pass = g_ws_current_pass ? *reinterpret_cast<const uint32_t*>(g_ws_current_pass) : 0u;
        const uint8_t rnd_fade = g_ws_render_fade ? *reinterpret_cast<const uint8_t*>(g_ws_render_fade) : 0u;
        const float cfg_zbias = g_ws_tex_zbias ? *reinterpret_cast<const float*>(g_ws_tex_zbias + 0x4CULL) : 0.0f;
        const float cur_depth_offs = g_ws_current_depth_offs ? *reinterpret_cast<const float*>(g_ws_current_depth_offs) : 0.0f;
        const float shadow_str = g_ws_shadow_strength ? *reinterpret_cast<const float*>(g_ws_shadow_strength + 0x4CULL) : 1.0f;
        const uint64_t mat_0 = *reinterpret_cast<const uint64_t*>(ws_ex + 0x290ULL);
        const uint64_t mat_1 = *reinterpret_cast<const uint64_t*>(ws_ex + 0x0F0ULL);
        const uint64_t mat_2 = *reinterpret_cast<const uint64_t*>(ws_ex + 0x1C0ULL);
        const uint64_t sh_shadow = *reinterpret_cast<const uint64_t*>(ws_ex + 0x20ULL);
        const uint64_t sh_unlit  = *reinterpret_cast<const uint64_t*>(ws_ex + static_cast<uint64_t>(sh_idx) * 8ULL);
        const uint64_t sh_lit_0  = *reinterpret_cast<const uint64_t*>(ws_ex + 0x10ULL);
        const uint64_t sh_lit_1  = *reinterpret_cast<const uint64_t*>(ws_ex + 0x18ULL);
        const uint64_t sh_lit    = (sh_idx == 0u) ? sh_lit_0 : sh_lit_1;

        for (uint32_t i = 0; i < num_meshes; ++i) {
            const uint64_t mesh = meshes[i];
            if (!mesh) continue;

            const auto* bbox = reinterpret_cast<const int16_t*>(mesh + 0x18ULL);
            if (do_mesh_aabb) {
                constexpr float kInv32768 = 1.0f / 32768.0f;
                if (ws_cull_aabb(m,
                        static_cast<float>(bbox[0]) * kInv32768,
                        static_cast<float>(bbox[1]) * kInv32768,
                        static_cast<float>(bbox[2]) * kInv32768,
                        static_cast<float>(bbox[3]) * kInv32768,
                        static_cast<float>(bbox[4]) * kInv32768,
                        static_cast<float>(bbox[5]) * kInv32768) == 0) {
                    continue;
                }
            }

            float fade_alpha = 1.0f;
            if (cull_fade_en) {
                const bool vis = ws_calc_screen_size(m, bbox[0], bbox[1], bbox[2], bbox[3], bbox[4], bbox[5]);
                if (ret_on_small) {
                    float& stored = ws_fade_map.get_or_create(mesh, inst_id, ws_frame_seq);
                    stored = std::min(1.0f, stored + (vis ? 1.0f : -1.0f) * fade_rate);
                    if (stored <= 0.0f) {
                        stored = 0.0f;
                        continue;
                    }
                    fade_alpha = stored;
                }
            }

            const uint64_t* const tp = *reinterpret_cast<const uint64_t* const*>(mesh + 0x10ULL);
            uint64_t tex = tp ? tp[0] : 0ULL;
            const uint64_t raw_tp = tex ? *reinterpret_cast<const uint64_t*>(tex + 0x128ULL) : 0ULL;

            uint64_t shader = shadow_active ? sh_shadow : sh_unlit;
            if (!shadow_active && dbg_sh == 0) {
                if (raw_tp) {
                    if (*reinterpret_cast<const uint8_t*>(raw_tp + 1ULL) == 0u &&
                        (rnd_alpha | *reinterpret_cast<const uint8_t*>(raw_tp)) != 0u) {
                        shader = sh_lit;
                    }
                } else if (rnd_alpha != 0u) {
                    shader = sh_lit;
                }
            }

            if (tex) {
                const uint64_t img = *reinterpret_cast<const uint64_t*>(tex + 0xF0ULL);
                if (img && *reinterpret_cast<const uint32_t*>(img + 4ULL) == 0u) {
                    if (const uint64_t fb = tp[1]) {
                        const uint64_t fb_img = *reinterpret_cast<const uint64_t*>(fb + 0xF0ULL);
                        if (fb_img && *reinterpret_cast<const uint32_t*>(fb_img + 4ULL) != 0u) {
                            tex = fb;
                        }
                    }
                }
            }
            const uint64_t eff_tp = tex ? *reinterpret_cast<const uint64_t*>(tex + 0x128ULL) : 0ULL;

            const bool is_lit_sh = (shader == sh_lit_0 || shader == sh_lit_1);
            uint32_t pass_mode = is_lit_sh ? (rnd_fade ? 8u : 3u) : cur_pass;
            float eff_alpha = base_alpha;
            uint32_t pass_state = 1u;
            if (fade_alpha != 1.0f) {
                if (fade_alpha < eff_alpha) eff_alpha = fade_alpha;
                pass_state = 3u;
            } else if (pass_mode == 4u || pass_mode == 6u) {
                pass_state = (eff_tp && *reinterpret_cast<const uint8_t*>(eff_tp + 2ULL) != 0u) ? 6u : 4u;
            } else if (pass_mode == 5u) {
                pass_state = 5u;
            } else if (pass_mode == 8u) {
                pass_state = 3u;
            }

            if (!vb_wrap) continue;
            const uint32_t slot = arena_ctrl->world_count;
            if (slot >= arena_ctrl->world_cap) {
                arena_ctrl->world_overflow = 1u;
                continue;
            }
            arena_ctrl->world_count = slot + 1u;
            FastBatchMeta& meta = arena_world_meta[slot];
            meta.shader_ptr = shader;
            meta.tex_ptr    = tex;
            meta.vbo_wrap   = vb_wrap;
            meta.pass_state = pass_state;
            meta.pad        = 0u;

            float zb = 0.0f;
            if (eff_tp) {
                const float tz = *reinterpret_cast<const float*>(eff_tp + 4ULL);
                if (tz != 0.0f) {
                    zb = (cfg_zbias != 0.0f) ? cfg_zbias : tz;
                }
            }
            zb += cur_depth_offs;

            uint8_t* dc = arena_world_calls + static_cast<size_t>(slot) * kDrawCallStride;
            *reinterpret_cast<uint64_t*>(dc + 0x00ULL) = mat_0;
            *reinterpret_cast<uint64_t*>(dc + 0x08ULL) = mat_1;
            *reinterpret_cast<uint64_t*>(dc + 0x10ULL) = mat_2;
            *reinterpret_cast<uint64_t*>(dc + 0x18ULL) = inst_mat_addr;
            *reinterpret_cast<uint64_t*>(dc + 0x20ULL) = vb_wrap;
            *reinterpret_cast<uint64_t*>(dc + 0x28ULL) = ib_wrap;
            *reinterpret_cast<uint64_t*>(dc + 0x30ULL) = tex;
            *reinterpret_cast<uint32_t*>(dc + 0x38ULL) = *reinterpret_cast<const uint32_t*>(mesh + 0x04ULL);
            *reinterpret_cast<uint32_t*>(dc + 0x3CULL) = *reinterpret_cast<const uint32_t*>(mesh + 0x3CULL);
            *reinterpret_cast<float*>(dc + 0x40ULL)    = eff_alpha;
            *reinterpret_cast<float*>(dc + 0x44ULL)    = shadow_str;
            *reinterpret_cast<uint64_t*>(dc + 0x48ULL) = 0ULL;
            *reinterpret_cast<float*>(dc + 0x50ULL)    = zb;
            *reinterpret_cast<uint32_t*>(dc + 0x54ULL) = 0u;

            if (g_ws_draw_count) {
                ++(*reinterpret_cast<uint32_t*>(g_ws_draw_count));
            }
        }
        return 0ULL;
    }

    static inline float decode_fp16(uint16_t h) {
        const uint32_t bits = ((h & 0x7FFFu) == 0u)
            ? (static_cast<uint32_t>(h) << 16)
            : ((((static_cast<uint32_t>(h & 0x8000u) << 16) |
                 (static_cast<uint32_t>((h >> 10) & 0x1Fu) << 23) |
                 (static_cast<uint32_t>(h & 0x3FFu) << 13))) + 0x38000000u);
        float f = 0.0f;
        std::memcpy(&f, &bits, sizeof(f));
        return f;
    }

    uint32_t ws_check_sphere_frustum(lcs::cpu::DynarmicCore* cur_cpu,
                                     float sx, float sy, float sz, float rad) const {
        if (!g_ws_sphere_cull_en2 || *reinterpret_cast<const uint8_t*>(g_ws_sphere_cull_en2 + 0x3DULL) == 0u) {
            return 2u;
        }
        if (g_ws_cull_change_flag && *reinterpret_cast<const uint32_t*>(g_ws_cull_change_flag) != 0u) {
            cur_cpu->call_nested(fn_culling_update, {1ULL});
        }
        const float* vo = g_ws_cur_view_offs ? reinterpret_cast<const float*>(g_ws_cur_view_offs) : nullptr;
        const float cx = sx + (vo ? vo[0] : 0.0f);
        const float cy = sy + (vo ? vo[1] : 0.0f);
        const float cz = sz + (vo ? vo[2] : 0.0f);
        const bool persp = g_ws_cull_is_persp && *reinterpret_cast<const uint8_t*>(g_ws_cull_is_persp) != 0u;
        if (!persp) {
            float sp[5] = {cx, cy, cz, 1.0f, rad};
            return static_cast<uint32_t>(
                cur_cpu->call_nested(fn_perf_sphere_cull, {reinterpret_cast<uint64_t>(sp), 1ULL}));
        }

        const float* fr = reinterpret_cast<const float*>(g_ws_cull_frustum);
        const float dx = cx - fr[90];
        const float dy = cy - fr[91];
        const float dz = cz - fr[92];

        const float z = (-fr[89]) + dx * (-fr[86]) + dy * (-fr[87]) + dz * (-fr[88]);
        const float near_c = fr[99];
        const float far_c  = fr[100];
        uint32_t res = 2u;
        uint32_t plane = 6u;
        if (z > far_c + rad) { res = 0u; plane = 1u; goto done; }
        if (z < near_c - rad) { res = 0u; plane = 0u; goto done; }
        if (z > far_c - rad) { res = 1u; plane = 1u; }
        else if (z < near_c + rad) { res = 1u; plane = 0u; }

        {
            const float x    = fr[85] + dx * fr[82] + dy * fr[83] + dz * fr[84];
            const float xrad = rad * fr[95];
            const float xlim = z * fr[96];
            if (x > xlim + xrad) { res = 0u; plane = 4u; goto done; }
            if (x < -xlim - xrad) { res = 0u; plane = 5u; goto done; }
            if (x > xlim - xrad) { res = 1u; plane = 4u; }
            else if (x < xrad - xlim) { res = 1u; plane = 5u; }

            const float y    = fr[81] + dx * fr[78] + dy * fr[79] + dz * fr[80];
            const float yrad = rad * fr[94];
            const float ylim = xlim * fr[98];
            if (y > ylim + yrad) { res = 0u; plane = 3u; goto done; }
            if (y < -ylim - yrad) { res = 0u; plane = 2u; goto done; }
            if (y > ylim - yrad) { res = 1u; plane = 3u; }
            else if (y < yrad - ylim) { res = 1u; plane = 2u; }
        }
    done:
        if (g_ws_cull_last_plane) *reinterpret_cast<uint32_t*>(g_ws_cull_last_plane) = plane;
        if (g_ws_last_sphere_state) *reinterpret_cast<uint32_t*>(g_ws_last_sphere_state) = res;
        return res;
    }

    inline void ensure_dyn_inst_template(lcs::cpu::DynarmicCore* cur_cpu) {
        if (dyn_inst_ready) return;
        std::memset(dyn_inst_template.data(), 0, dyn_inst_template.size());
        cur_cpu->call_nested(fn_sgeom_ctor, {reinterpret_cast<uint64_t>(dyn_inst_template.data())});
        dyn_inst_ready = true;
    }

    inline void push_alpha_entry(lcs::cpu::DynarmicCore* cur_cpu, uint64_t ws, uint64_t inst,
                                 uint32_t bucket, uint32_t sph_vis, bool sph_en,
                                 float sx, float sy, float sz) const {
        const uint64_t p_cur = *reinterpret_cast<const uint64_t*>(ws + 0x8AA0ULL);
        const uint64_t p_cap = *reinterpret_cast<const uint64_t*>(ws + 0x8AA8ULL);
        if (!p_cur || p_cur + 0x18ULL > p_cap) {
            cur_cpu->call_nested(fn_ws_render_alpha, {ws, inst, static_cast<uint64_t>(bucket), 0xFFULL, 0ULL});
            return;
        }
        const float* cam_pos = g_ws_the_camera ? reinterpret_cast<const float*>(g_ws_the_camera + 0x30ULL) : nullptr;
        const float dx = cam_pos ? (sx - cam_pos[0]) : sx;
        const float dy = cam_pos ? (sy - cam_pos[1]) : sy;
        const float dz = cam_pos ? (sz - cam_pos[2]) : sz;
        uint8_t* dst = reinterpret_cast<uint8_t*>(p_cur);
        *reinterpret_cast<float*>(dst + 0x00ULL)    = dx * dx + dy * dy + dz * dz;
        *reinterpret_cast<uint64_t*>(dst + 0x08ULL) = inst;
        dst[0x10] = static_cast<uint8_t>(((bucket & 0xFu) << 4) | (sph_en ? (sph_vis & 0xFu) : 1u));
        dst[0x11] = 0xFFu;
        dst[0x12] = 0u;
        dst[0x13] = 0u;
        dst[0x14] = g_ws_render_fade ? *reinterpret_cast<const uint8_t*>(g_ws_render_fade) : 0u;
        *reinterpret_cast<uint64_t*>(ws + 0x8AA0ULL) = p_cur + 0x18ULL;
    }

    uint64_t run_cworld_bucket_loop(uint64_t ws, uint32_t start_b, uint32_t end_b, uint64_t gu_list) {
        static const bool s_bf_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
        const auto bt0 = s_bf_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        auto* cur_cpu = lcs::cpu::DynarmicCore::current();
        if (!cur_cpu || !ws || !g_ws_ex) return 0ULL;
        const uint64_t vec_hdr = *reinterpret_cast<const uint64_t*>(ws + 0x38ULL);
        if (!vec_hdr) return 0ULL;
        const uint64_t hdr = *reinterpret_cast<const uint64_t*>(vec_hdr);
        if (!hdr) return 0ULL;

        const bool sph_en = g_ws_sphere_cull_en && (*reinterpret_cast<const uint8_t*>(g_ws_sphere_cull_en + 0x3DULL) != 0u);
        const float fudge = g_ws_clip_sphere_fudge ? *reinterpret_cast<const float*>(g_ws_clip_sphere_fudge) : 1.0f;
        const bool rnd_nonfade = g_ws_render_nonfading && (*reinterpret_cast<const uint8_t*>(g_ws_render_nonfading + 0x3DULL) != 0u);
        const bool rnd_fading  = g_ws_render_fading && (*reinterpret_cast<const uint8_t*>(g_ws_render_fading + 0x3DULL) != 0u);

        uint64_t cur_ptr = *reinterpret_cast<const uint64_t*>(hdr + 0x10ULL + static_cast<uint64_t>(start_b) * 8ULL);
        for (uint32_t b = start_b; b < end_b; ++b) {
            const uint64_t next_ptr = *reinterpret_cast<const uint64_t*>(hdr + 0x10ULL + static_cast<uint64_t>(b + 1u) * 8ULL);
            if (b <= 3u) {
                cur_cpu->call_nested(fn_ws_set_list_states, {ws, gu_list, static_cast<uint64_t>(b)});
                if (rnd_nonfade) {
                    if (cur_ptr < next_ptr) {
                        cur_cpu->call_nested(fn_set_blend_mode_pass, {static_cast<uint64_t>(b)});
                        for (uint64_t inst = cur_ptr; inst < next_ptr; inst += 0x78ULL) {
                            if (*reinterpret_cast<const int16_t*>(inst) < 0) continue;
                            uint32_t chk_aabb = 1u;
                            if (sph_en) {
                                const uint16_t* sp16 = reinterpret_cast<const uint16_t*>(inst + 4ULL);
                                const uint32_t vis = ws_check_sphere_frustum(
                                    cur_cpu, decode_fp16(sp16[0]), decode_fp16(sp16[1]),
                                    decode_fp16(sp16[2]), decode_fp16(sp16[3]) * fudge);
                                if (vis == 0u) continue;
                                chk_aabb = (vis == 1u) ? 1u : 0u;
                            }
                            ws_render_nocull(g_ws_ex, ws, inst, chk_aabb, 0x3F800000u);
                        }
                    }
                    int16_t dyn_idx = *reinterpret_cast<const int16_t*>(ws + 0x8A88ULL + static_cast<uint64_t>(b) * 2ULL);
                    while (dyn_idx >= 0) {
                        const uint64_t dyn_base = ws + static_cast<uint64_t>(dyn_idx) * 0x58ULL;
                        dyn_idx = *reinterpret_cast<const int16_t*>(dyn_base + 0x236CULL);
                        const uint16_t mod_id = *reinterpret_cast<const uint16_t*>(dyn_base + 0x236EULL);
                        const uint64_t models_hdr = *reinterpret_cast<const uint64_t*>(ws + 0x18ULL);
                        const uint64_t models_arr = models_hdr ? *reinterpret_cast<const uint64_t*>(models_hdr) : 0ULL;
                        if (!models_arr || *reinterpret_cast<const uint64_t*>(models_arr + static_cast<uint64_t>(mod_id) * 0x18ULL) == 0ULL) {
                            continue;
                        }
                        ensure_dyn_inst_template(cur_cpu);
                        std::memcpy(dyn_inst_scratch.data(), dyn_inst_template.data(), 0x78u);
                        *reinterpret_cast<uint16_t*>(dyn_inst_scratch.data() + 0x02ULL) = mod_id;
                        *reinterpret_cast<uint64_t*>(dyn_inst_scratch.data() + 0x30ULL) =
                            *reinterpret_cast<const uint64_t*>(dyn_base + 0x2370ULL);
                        const uint32_t chk = (*reinterpret_cast<const uint32_t*>(dyn_base + 0x2378ULL) == 1u) ? 1u : 0u;
                        ws_render_nocull(g_ws_ex, ws, reinterpret_cast<uint64_t>(dyn_inst_scratch.data()), chk, 0x3F800000u);
                    }
                }
                cur_cpu->call_nested(fn_ws_unset_list_states, {ws, gu_list, static_cast<uint64_t>(b)});
            } else if (rnd_fading) {
                for (uint64_t inst = cur_ptr; inst < next_ptr; inst += 0x78ULL) {
                    if (*reinterpret_cast<const int16_t*>(inst) < 0) continue;
                    const uint16_t* sp16 = reinterpret_cast<const uint16_t*>(inst + 4ULL);
                    const float sx = decode_fp16(sp16[0]);
                    const float sy = decode_fp16(sp16[1]);
                    const float sz = decode_fp16(sp16[2]);
                    uint32_t vis = 1u;
                    if (sph_en) {
                        vis = ws_check_sphere_frustum(cur_cpu, sx, sy, sz, decode_fp16(sp16[3]) * fudge);
                        if (vis == 0u) continue;
                    }
                    push_alpha_entry(cur_cpu, ws, inst, b, vis, sph_en, sx, sy, sz);
                }
                int16_t dyn_idx = *reinterpret_cast<const int16_t*>(ws + 0x8A88ULL + static_cast<uint64_t>(b) * 2ULL);
                while (dyn_idx >= 0) {
                    const uint64_t dyn_base = ws + static_cast<uint64_t>(dyn_idx) * 0x58ULL;
                    const uint64_t p_cur = *reinterpret_cast<const uint64_t*>(ws + 0x8AA0ULL);
                    const uint64_t p_cap = *reinterpret_cast<const uint64_t*>(ws + 0x8AA8ULL);
                    if (p_cur && p_cur + 0x18ULL <= p_cap) {
                        uint8_t* dst = reinterpret_cast<uint8_t*>(p_cur);
                        *reinterpret_cast<float*>(dst + 0x00ULL)    = *reinterpret_cast<const float*>(dyn_base + 0x2368ULL);
                        *reinterpret_cast<uint64_t*>(dst + 0x08ULL) = dyn_base + 0x2368ULL;
                        dst[0x10] = static_cast<uint8_t>(0x40u | (*reinterpret_cast<const uint8_t*>(dyn_base + 0x2378ULL) & 0xFu));
                        dst[0x11] = 0xFFu;
                        dst[0x12] = 0u;
                        dst[0x13] = 1u;
                        *reinterpret_cast<uint64_t*>(ws + 0x8AA0ULL) = p_cur + 0x18ULL;
                    }
                    dyn_idx = *reinterpret_cast<const int16_t*>(dyn_base + 0x236CULL);
                }
            }
            cur_ptr = next_ptr;
        }
        if (s_bf_audit) {
            const double bms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bt0).count();
            static int bc = 0; ++bc;
            if (bms > 3.0 || (bc % 400 == 0)) {
                std::cout << "  [TMP_BUCKET] c=" << bc << " ms=" << bms << "\n";
            }
        }
        return 0ULL;
    }

    uint64_t render_world(uint64_t mgr_ptr, uint32_t mask) {
        static const bool s_bf_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
        const auto wt0 = s_bf_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!g_dv_enable_world || *reinterpret_cast<const uint8_t*>(g_dv_enable_world + 0x3DULL) == 0) {
            return 0;
        }
        auto* cur_cpu = lcs::cpu::DynarmicCore::current();
        if (!cur_cpu) return 0;

        std::lock_guard<std::mutex> lk(mtx);
        ++ws_frame_seq;
        ensure_uniform_sids(cur_cpu);

        auto& rb = lcs::graphics::RenderBridge::instance();
        reset_pass_caches();
        if (g_depth_offset) *reinterpret_cast<float*>(g_depth_offset) = 0.0f;
        apply_z_bias(rb, 0.0f);

        if (arena_enabled && arena_ctrl) {
            if ((arena_ctrl->world_overflow || arena_ctrl->model_overflow) && !logged_arena_overflow) {
                logged_arena_overflow = true;
                std::cerr << "[LcsBatchedFast] Arena capacity overflow (world="
                          << arena_ctrl->world_count << "/" << arena_ctrl->world_cap
                          << ", model=" << arena_ctrl->model_count << "/" << arena_ctrl->model_cap << ").\n";
            }
            const uint32_t total = arena_ctrl->world_count;
            if (total > 0) {
                const int32_t sort_key = g_world_sort_key ? *reinterpret_cast<const int32_t*>(g_world_sort_key) : 2;
                const bool sort_by_gl_prog = (sort_key == 1 || sort_key == 2);
                const uint32_t loading_id = g_loading_tex
                    ? *reinterpret_cast<const uint32_t*>(g_loading_tex + 4ULL) : 0u;
                const uint64_t env_ctex = g_dyn_shadow_tex
                    ? *reinterpret_cast<const uint64_t*>(g_dyn_shadow_tex) : 0ULL;
                const uint32_t env_tex_id = get_tex_gl_id(env_ctex, loading_id);

                sort_entries.clear();
                if (sort_entries.capacity() < total) sort_entries.reserve(total);
                uint32_t matched_count = 0;

                for (uint32_t i = 0; i < total; ++i) {
                    const auto& m = arena_world_meta[i];
                    if (mask != 0 && ((mask >> (m.pass_state - 1u)) & 1u) == 0) continue;
                    ++matched_count;
                    if (!m.shader_ptr || !m.vbo_wrap) continue;
                    const uint8_t* p = arena_world_calls + static_cast<size_t>(i) * kDrawCallStride;
                    const uint64_t ib_wrap = *reinterpret_cast<const uint64_t*>(p + 0x28ULL);
                    if (!ib_wrap) continue;
                    const uint64_t vb_ptr = *reinterpret_cast<const uint64_t*>(m.vbo_wrap + 0x18ULL);
                    const uint64_t ib_ptr = *reinterpret_cast<const uint64_t*>(ib_wrap + 0x10ULL);
                    if (!vb_ptr || !ib_ptr) continue;
                    const uint32_t vbo = *reinterpret_cast<const uint32_t*>(vb_ptr);
                    const uint32_t ibo = *reinterpret_cast<const uint32_t*>(ib_ptr);
                    if (!vbo || !ibo) continue;

                    const uint32_t shader_key = sort_by_gl_prog
                        ? *reinterpret_cast<const uint32_t*>(m.shader_ptr + 0xA0ULL) : 0u;
                    const uint32_t tex0 = get_tex_gl_id(m.tex_ptr, loading_id);
                    sort_entries.push_back(SortEntry{m.shader_ptr, m.pass_state, shader_key, tex0, vbo, ibo, i});
                }

                if (!sort_entries.empty()) {
                    sort_batch_entries(sort_entries);

                    const float* amb_v = reinterpret_cast<const float*>(g_world_ambient);
                    const float* emi_v = reinterpret_cast<const float*>(g_world_emissive);
                    const float* fogp  = reinterpret_cast<const float*>(g_fog_params);
                    float fogp_adj[4]{};
                    if (fogp) {
                        std::memcpy(fogp_adj, fogp, sizeof(fogp_adj));
                        lcs::game::LcsDistance::instance().adjust_fog_parameters(fogp_adj);
                    }
                    const float* fogc  = reinterpret_cast<const float*>(g_fog_colour);
                    const float* sc    = reinterpret_cast<const float*>(g_dyn_shadow_centre);
                    const float  s_in  = g_shadow_inner_rad ? *reinterpret_cast<const float*>(g_shadow_inner_rad + 0x4CULL) : 0.0f;
                    const float  s_out = g_shadow_outer_rad ? *reinterpret_cast<const float*>(g_shadow_outer_rad + 0x4CULL) : 0.0f;
                    const float  sh_vec4[4] = { sc ? sc[0] : 0.0f, sc ? sc[1] : 0.0f, s_in, s_out };

                    uint32_t cur_pass = UINT32_MAX;
                    uint64_t cur_shader_ptr = 0ULL;
                    HostShaderState* cur_hs = nullptr;
                    lcs::graphics::RenderBridge::BatchProgramView cur_pv{};
                    uint32_t cur_prog = 0;
                    uint32_t cur_tex0 = 0xFFFFFFFFu;
                    uint32_t cur_tex1 = 0xFFFFFFFFu;
                    uint32_t cur_vao  = 0;
                    uint32_t active_unit = 0xFFFFFFFFu;
                    bool     blend_on = false;
                    uint8_t  dmask_on = 1;
                    bool     depth_on = false;
                    bool     first_pass = true;
                    uint32_t draws_issued = 0;

                    for (const SortEntry& se : sort_entries) {
                        if (se.pass_state != cur_pass) {
                            set_pass_state(rb, se.pass_state, blend_on, dmask_on, depth_on, first_pass);
                            first_pass = false;
                            cur_pass = se.pass_state;
                        }
                        if (se.shader_ptr != cur_shader_ptr) {
                            cur_shader_ptr = se.shader_ptr;
                            cur_hs = &get_shader_state(rb, cur_shader_ptr);
                            if (cur_hs->gl_prog != cur_prog) {
                                rb.gl_use_program(cur_hs->gl_prog);
                                cur_prog = cur_hs->gl_prog;
                            }
                            cur_pv = rb.get_batch_program_view(cur_prog);
                            set_fs_vec4(*cur_hs, cur_pv, 7u, sh_vec4);
                            set_fs_vec4(*cur_hs, cur_pv, 0u, amb_v);
                            set_fs_vec4(*cur_hs, cur_pv, 1u, emi_v);
                            set_vs_vec4(*cur_hs, cur_pv, 8u, (fogp && cur_hs->vs8_is_fog) ? fogp_adj : fogp);
                            set_vs_vec4(*cur_hs, cur_pv, 7u, fogc);
                            if (cur_tex1 != env_tex_id) {
                                if (active_unit != 1u) {
                                    rb.gl_active_texture(0x84C1u);
                                    active_unit = 1u;
                                }
                                rb.gl_bind_texture(0x0DE1u, env_tex_id);
                                cur_tex1 = env_tex_id;
                            }
                        }
                        const uint8_t* p = arena_world_calls + static_cast<size_t>(se.idx) * kDrawCallStride;
                        emit_world_draw_call(rb, *cur_hs, cur_pv, p, cur_shader_ptr, se.vbo, se.ibo, se.tex0,
                                             cur_tex0, cur_vao, active_unit);
                        ++draws_issued;
                    }

                    if (cur_vao != 0) rb.gl_bind_vertex_array(0);
                    if (active_unit != 0u) rb.gl_active_texture(0x84C0u);
                    if (g_draw_call_count) *reinterpret_cast<uint32_t*>(g_draw_call_count) += draws_issued;
                    if (!first_pass) {
                        sync_guest_gl_state_after(cur_prog, blend_on, dmask_on, false);
                    }
                }

                if (mask == 0 || matched_count == total) {
                    arena_ctrl->world_count = 0u;
                    arena_ctrl->world_overflow = 0u;
                } else if (matched_count > 0) {
                    reset_world_arena_entries(mask);
                }
            }
            reset_pass_caches();
            invalidate_guest_pending_state(rb);
            if (s_bf_audit) {
                const double wms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wt0).count();
                static int wc = 0; ++wc;
                if (wms > 5.0 || (wc % 400 == 0)) {
                    std::cout << "  [TMP_WORLD_FAST] c=" << wc << " mask=" << mask << " ms=" << wms << "\n";
                }
            }
            return 0;
        }

        const uint64_t end_p = mgr_ptr + 8ULL;
        uint64_t node_p = *reinterpret_cast<const uint64_t*>(mgr_ptr);
        if (node_p != end_p) {
            int32_t sort_key = g_world_sort_key ? *reinterpret_cast<const int32_t*>(g_world_sort_key) : 2;
            size_t sh_map_off = (sort_key == 0) ? 0x30ULL : 0x48ULL;
            if (sort_key < 0 || sort_key > 2) sh_map_off = 0x48ULL;

            const uint32_t loading_id = g_loading_tex
                ? *reinterpret_cast<const uint32_t*>(g_loading_tex + 4ULL) : 0u;
            const uint64_t env_ctex = g_dyn_shadow_tex
                ? *reinterpret_cast<const uint64_t*>(g_dyn_shadow_tex) : 0ULL;
            const uint32_t env_tex_id = get_tex_gl_id(env_ctex, loading_id);

            const float* amb_v = reinterpret_cast<const float*>(g_world_ambient);
            const float* emi_v = reinterpret_cast<const float*>(g_world_emissive);
            const float* fogp  = reinterpret_cast<const float*>(g_fog_params);
            const float* fogc  = reinterpret_cast<const float*>(g_fog_colour);
            const float* sc    = reinterpret_cast<const float*>(g_dyn_shadow_centre);
            const float  s_in  = g_shadow_inner_rad ? *reinterpret_cast<const float*>(g_shadow_inner_rad + 0x4CULL) : 0.0f;
            const float  s_out = g_shadow_outer_rad ? *reinterpret_cast<const float*>(g_shadow_outer_rad + 0x4CULL) : 0.0f;
            const float  sh_vec4[4] = { sc ? sc[0] : 0.0f, sc ? sc[1] : 0.0f, s_in, s_out };

            uint32_t cur_prog = 0;
            uint32_t cur_tex0 = 0xFFFFFFFFu;
            uint32_t cur_tex1 = 0xFFFFFFFFu;
            uint32_t cur_vao  = 0;
            uint32_t active_unit = 0xFFFFFFFFu;
            bool     blend_on = false;
            uint8_t  dmask_on = 1;
            bool     depth_on = false;
            bool     first_pass = true;
            uint32_t draws_issued = 0;

            for (; node_p != end_p; node_p = tree_next(node_p)) {
                if (*reinterpret_cast<const uint32_t*>(node_p + 0x28ULL) == 0) continue;
                const uint32_t pass_state = *reinterpret_cast<const uint32_t*>(node_p + 0x20ULL);
                if (mask != 0 && ((mask >> (pass_state - 1u)) & 1u) == 0) continue;

                set_pass_state(rb, pass_state, blend_on, dmask_on, depth_on, first_pass);
                first_pass = false;

                const uint64_t end_s = node_p + sh_map_off + 8ULL;
                uint64_t node_s = *reinterpret_cast<const uint64_t*>(node_p + sh_map_off);
                for (; node_s != end_s; node_s = tree_next(node_s)) {
                    if (*reinterpret_cast<const uint32_t*>(node_s + 0x28ULL) == 0) continue;
                    const uint64_t shader_ptr = *reinterpret_cast<const uint64_t*>(node_s + 0x20ULL);
                    if (!shader_ptr) continue;

                    HostShaderState& hs = get_shader_state(rb, shader_ptr);
                    if (hs.gl_prog != cur_prog) {
                        rb.gl_use_program(hs.gl_prog);
                        cur_prog = hs.gl_prog;
                    }
                    const auto pv = rb.get_batch_program_view(cur_prog);
                    float fogp_adj[4]{};
                    if (fogp) {
                        std::memcpy(fogp_adj, fogp, sizeof(fogp_adj));
                        lcs::game::LcsDistance::instance().adjust_fog_parameters(fogp_adj);
                    }

                    set_fs_vec4(hs, pv, 7u, sh_vec4);
                    set_fs_vec4(hs, pv, 0u, amb_v);
                    set_fs_vec4(hs, pv, 1u, emi_v);
                    set_vs_vec4(hs, pv, 8u, (fogp && hs.vs8_is_fog) ? fogp_adj : fogp);
                    set_vs_vec4(hs, pv, 7u, fogc);

                    if (cur_tex1 != env_tex_id) {
                        if (active_unit != 1u) {
                            rb.gl_active_texture(0x84C1u);
                            active_unit = 1u;
                        }
                        rb.gl_bind_texture(0x0DE1u, env_tex_id);
                        cur_tex1 = env_tex_id;
                    }

                    const uint64_t end_t = node_s + 0x38ULL;
                    uint64_t node_t = *reinterpret_cast<const uint64_t*>(node_s + 0x30ULL);
                    for (; node_t != end_t; node_t = tree_next(node_t)) {
                        const uint64_t vec_begin = *reinterpret_cast<const uint64_t*>(node_t + 0x38ULL);
                        const uint64_t vec_end   = *reinterpret_cast<const uint64_t*>(node_t + 0x40ULL);
                        if (vec_begin >= vec_end) continue;
                        if (sh_map_off == 0x30ULL && *reinterpret_cast<const uint64_t*>(node_t + 0x30ULL) == 0ULL) continue;

                        for (uint64_t p = vec_begin; p < vec_end; p += kDrawCallStride) {
                            const uint64_t vb_wrap = *reinterpret_cast<const uint64_t*>(p + 0x20ULL);
                            const uint64_t ib_wrap = *reinterpret_cast<const uint64_t*>(p + 0x28ULL);
                            if (!vb_wrap || !ib_wrap) continue;
                            const uint64_t vb_ptr = *reinterpret_cast<const uint64_t*>(vb_wrap + 0x18ULL);
                            const uint64_t ib_ptr = *reinterpret_cast<const uint64_t*>(ib_wrap + 0x10ULL);
                            if (!vb_ptr || !ib_ptr) continue;
                            const uint32_t vbo = *reinterpret_cast<const uint32_t*>(vb_ptr);
                            const uint32_t ibo = *reinterpret_cast<const uint32_t*>(ib_ptr);
                            if (!vbo || !ibo) continue;

                            const uint32_t tex0 = get_tex_gl_id(*reinterpret_cast<const uint64_t*>(p + 0x30ULL), loading_id);
                            emit_world_draw_call(rb, hs, pv, reinterpret_cast<const uint8_t*>(p), shader_ptr, vbo, ibo, tex0,
                                                 cur_tex0, cur_vao, active_unit);
                            ++draws_issued;
                        }
                    }
                }
            }

            if (cur_vao != 0) rb.gl_bind_vertex_array(0);
            if (active_unit != 0u) rb.gl_active_texture(0x84C0u);
            if (g_draw_call_count) *reinterpret_cast<uint32_t*>(g_draw_call_count) += draws_issued;
            if (!first_pass) {
                sync_guest_gl_state_after(cur_prog, blend_on, dmask_on, false);
            }
        }

        reset_pass_caches();
        cur_cpu->call_nested(fn_world_reset, {mgr_ptr, static_cast<uint64_t>(mask), 0ULL});
        if (s_bf_audit) {
            const double wms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wt0).count();
            static int wc = 0; ++wc;
            if (wms > 5.0 || (wc % 400 == 0)) {
                std::cout << "  [TMP_WORLD_FAST] c=" << wc << " mask=" << mask << " ms=" << wms << "\n";
            }
        }
        return 0;
    }

    uint64_t render_model(uint64_t mgr_ptr) {
        static const bool s_bf_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
        const auto mt0 = s_bf_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!g_dv_enable_model || *reinterpret_cast<const uint8_t*>(g_dv_enable_model + 0x3DULL) == 0) {
            return 0;
        }
        auto* cur_cpu = lcs::cpu::DynarmicCore::current();
        if (!cur_cpu) return 0;

        std::lock_guard<std::mutex> lk(mtx);
        ensure_uniform_sids(cur_cpu);

        auto& rb = lcs::graphics::RenderBridge::instance();
        reset_pass_caches();
        rb.gl_disable(0x0B44u); // GL_CULL_FACE

        if (arena_enabled && arena_ctrl) {
            const uint32_t total = arena_ctrl->model_count;
            if (total > 0) {
                const int32_t sort_key = g_model_sort_key ? *reinterpret_cast<const int32_t*>(g_model_sort_key) : 2;
                const bool sort_by_gl_prog = (sort_key == 1 || sort_key == 2);
                const uint32_t loading_id = g_loading_tex
                    ? *reinterpret_cast<const uint32_t*>(g_loading_tex + 4ULL) : 0u;
                const uint64_t env_ctex = g_model_env_map
                    ? *reinterpret_cast<const uint64_t*>(g_model_env_map) : 0ULL;
                const uint32_t env_tex_id = get_tex_gl_id(env_ctex, loading_id);

                sort_entries.clear();
                if (sort_entries.capacity() < total) sort_entries.reserve(total);

                for (uint32_t i = 0; i < total; ++i) {
                    const auto& m = arena_model_meta[i];
                    if (!m.shader_ptr || !m.vbo_wrap) continue;
                    const uint8_t* b = arena_model_batches + static_cast<size_t>(i) * kBatchStride;
                    const uint64_t ib_wrap = *reinterpret_cast<const uint64_t*>(b + 0x48ULL);
                    if (!ib_wrap) continue;
                    const uint64_t vb_ptr = *reinterpret_cast<const uint64_t*>(m.vbo_wrap + 0x18ULL);
                    const uint64_t ib_ptr = *reinterpret_cast<const uint64_t*>(ib_wrap + 0x10ULL);
                    if (!vb_ptr || !ib_ptr) continue;
                    const uint32_t vbo = *reinterpret_cast<const uint32_t*>(vb_ptr);
                    const uint32_t ibo = *reinterpret_cast<const uint32_t*>(ib_ptr);
                    if (!vbo || !ibo) continue;

                    const uint32_t shader_key = sort_by_gl_prog
                        ? *reinterpret_cast<const uint32_t*>(m.shader_ptr + 0xA0ULL) : 0u;
                    const uint32_t tex0 = get_tex_gl_id(m.tex_ptr, loading_id);
                    sort_entries.push_back(SortEntry{m.shader_ptr, m.pass_state, shader_key, tex0, vbo, ibo, i});
                }

                bool first_pass = true;
                uint32_t cur_prog = 0;
                bool blend_on = false;
                uint8_t dmask_on = 1;

                if (!sort_entries.empty()) {
                    sort_batch_entries(sort_entries);

                    const float* wv_mat   = reinterpret_cast<const float*>(g_world_view_mat);
                    const float* pr_mat   = reinterpret_cast<const float*>(g_proj_mat);
                    const float  sp_pow   = g_model_spec_pow ? *reinterpret_cast<const float*>(g_model_spec_pow + 0x4CULL) : 0.0f;
                    const float  sp_int   = g_model_spec_int ? *reinterpret_cast<const float*>(g_model_spec_int + 0x4CULL) : 0.0f;
                    const float  spec4[4] = { sp_pow, sp_int, 0.0f, 0.0f };
                    const float* fogp     = reinterpret_cast<const float*>(g_fog_params);
                    float fogp_adj[4]{};
                    if (fogp) {
                        std::memcpy(fogp_adj, fogp, sizeof(fogp_adj));
                        lcs::game::LcsDistance::instance().adjust_fog_parameters(fogp_adj);
                    }
                    const float* fogc     = reinterpret_cast<const float*>(g_fog_colour);
                    const float* emi      = reinterpret_cast<const float*>(g_model_emissive);
                    const float* sky_bot  = reinterpret_cast<const float*>(g_sky_bottom);
                    const float* sky_top  = reinterpret_cast<const float*>(g_sky_top);

                    uint32_t cur_pass = UINT32_MAX;
                    uint64_t cur_shader_ptr = 0ULL;
                    HostShaderState* cur_hs = nullptr;
                    lcs::graphics::RenderBridge::BatchProgramView cur_pv{};
                    uint32_t cur_tex0 = 0xFFFFFFFFu;
                    uint32_t cur_tex1 = 0xFFFFFFFFu;
                    uint32_t cur_vao  = 0;
                    uint32_t active_unit = 0xFFFFFFFFu;
                    bool     depth_on = false;
                    uint32_t draws_issued = 0;

                    for (const SortEntry& se : sort_entries) {
                        if (se.pass_state != cur_pass) {
                            set_pass_state(rb, se.pass_state, blend_on, dmask_on, depth_on, first_pass);
                            first_pass = false;
                            cur_pass = se.pass_state;
                        }
                        if (se.shader_ptr != cur_shader_ptr) {
                            cur_shader_ptr = se.shader_ptr;
                            cur_hs = &get_shader_state(rb, cur_shader_ptr);
                            if (cur_hs->gl_prog != cur_prog) {
                                rb.gl_use_program(cur_hs->gl_prog);
                                cur_prog = cur_hs->gl_prog;
                            }
                            cur_pv = rb.get_batch_program_view(cur_prog);
                            set_vs_mat4(*cur_hs, cur_pv, 0u,    wv_mat);
                            set_vs_mat4(*cur_hs, cur_pv, 1u,    pr_mat);
                            set_vs_vec4(*cur_hs, cur_pv, 0x16u, spec4);
                            set_vs_vec4(*cur_hs, cur_pv, 8u,    (fogp && cur_hs->vs8_is_fog) ? fogp_adj : fogp);
                            set_vs_vec4(*cur_hs, cur_pv, 7u,    fogc);
                            set_vs_vec4(*cur_hs, cur_pv, 0xAu,  emi);
                            set_fs_vec4(*cur_hs, cur_pv, 8u,    sky_bot);
                            set_fs_vec4(*cur_hs, cur_pv, 9u,    sky_top);
                            if (cur_tex1 != env_tex_id) {
                                if (active_unit != 1u) {
                                    rb.gl_active_texture(0x84C1u);
                                    active_unit = 1u;
                                }
                                rb.gl_bind_texture(0x0DE1u, env_tex_id);
                                cur_tex1 = env_tex_id;
                            }
                        }
                        const uint8_t* b = arena_model_batches + static_cast<size_t>(se.idx) * kBatchStride;
                        emit_model_batch(rb, *cur_hs, cur_pv, b, cur_shader_ptr, se.vbo, se.ibo, se.tex0,
                                         cur_tex0, cur_vao, active_unit);
                        ++draws_issued;
                    }

                    if (cur_vao != 0) rb.gl_bind_vertex_array(0);
                    if (active_unit != 0u) rb.gl_active_texture(0x84C0u);
                    if (g_draw_call_count) *reinterpret_cast<uint32_t*>(g_draw_call_count) += draws_issued;
                }

                if (!first_pass) {
                    sync_guest_gl_state_after(cur_prog, blend_on, dmask_on, true);
                } else if (g_s_gl_state) {
                    *reinterpret_cast<uint8_t*>(g_s_gl_state + 0x38ULL) = 0u;
                }
                arena_ctrl->model_count = 0;
                arena_ctrl->model_overflow = 0;
            }
            reset_pass_caches();
            invalidate_guest_pending_state(rb);
            if (s_bf_audit) {
                const double mms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mt0).count();
                static int mc = 0; ++mc;
                if (mms > 5.0 || (mc % 400 == 0)) {
                    std::cout << "  [TMP_MODEL_FAST] c=" << mc << " ms=" << mms << "\n";
                }
            }
            return 0;
        }

        const uint64_t end_p = mgr_ptr + 8ULL;
        uint64_t node_p = *reinterpret_cast<const uint64_t*>(mgr_ptr);
        if (node_p != end_p) {
            int32_t sort_key = g_model_sort_key ? *reinterpret_cast<const int32_t*>(g_model_sort_key) : 2;
            size_t sh_map_off = (sort_key == 0) ? 0x30ULL : 0x48ULL;
            if (sort_key < 0 || sort_key > 2) sh_map_off = 0x48ULL;

            const uint32_t loading_id = g_loading_tex
                ? *reinterpret_cast<const uint32_t*>(g_loading_tex + 4ULL) : 0u;
            const uint64_t env_ctex = g_model_env_map
                ? *reinterpret_cast<const uint64_t*>(g_model_env_map) : 0ULL;
            const uint32_t env_tex_id = get_tex_gl_id(env_ctex, loading_id);

            const float* wv_mat   = reinterpret_cast<const float*>(g_world_view_mat);
            const float* pr_mat   = reinterpret_cast<const float*>(g_proj_mat);
            const float  sp_pow   = g_model_spec_pow ? *reinterpret_cast<const float*>(g_model_spec_pow + 0x4CULL) : 0.0f;
            const float  sp_int   = g_model_spec_int ? *reinterpret_cast<const float*>(g_model_spec_int + 0x4CULL) : 0.0f;
            const float  spec4[4] = { sp_pow, sp_int, 0.0f, 0.0f };
            const float* fogp     = reinterpret_cast<const float*>(g_fog_params);
            const float* fogc     = reinterpret_cast<const float*>(g_fog_colour);
            const float* emi      = reinterpret_cast<const float*>(g_model_emissive);
            const float* sky_bot  = reinterpret_cast<const float*>(g_sky_bottom);
            const float* sky_top  = reinterpret_cast<const float*>(g_sky_top);

            uint32_t cur_prog = 0;
            uint32_t cur_tex0 = 0xFFFFFFFFu;
            uint32_t cur_tex1 = 0xFFFFFFFFu;
            uint32_t cur_vao  = 0;
            uint32_t active_unit = 0xFFFFFFFFu;
            bool     blend_on = false;
            uint8_t  dmask_on = 1;
            bool     depth_on = false;
            bool     first_pass = true;
            uint32_t draws_issued = 0;

            for (; node_p != end_p; node_p = tree_next(node_p)) {
                if (*reinterpret_cast<const uint32_t*>(node_p + 0x28ULL) == 0) continue;
                const uint32_t pass_state = *reinterpret_cast<const uint32_t*>(node_p + 0x20ULL);

                set_pass_state(rb, pass_state, blend_on, dmask_on, depth_on, first_pass);
                first_pass = false;

                const uint64_t end_s = node_p + sh_map_off + 8ULL;
                uint64_t node_s = *reinterpret_cast<const uint64_t*>(node_p + sh_map_off);
                for (; node_s != end_s; node_s = tree_next(node_s)) {
                    if (*reinterpret_cast<const uint32_t*>(node_s + 0x28ULL) == 0) continue;
                    const uint64_t shader_ptr = *reinterpret_cast<const uint64_t*>(node_s + 0x20ULL);
                    if (!shader_ptr) continue;

                    HostShaderState& hs = get_shader_state(rb, shader_ptr);
                    if (hs.gl_prog != cur_prog) {
                        rb.gl_use_program(hs.gl_prog);
                        cur_prog = hs.gl_prog;
                    }
                    const auto pv = rb.get_batch_program_view(cur_prog);
                    float fogp_adj[4]{};
                    if (fogp) {
                        std::memcpy(fogp_adj, fogp, sizeof(fogp_adj));
                        lcs::game::LcsDistance::instance().adjust_fog_parameters(fogp_adj);
                    }

                    set_vs_mat4(hs, pv, 0u,    wv_mat);
                    set_vs_mat4(hs, pv, 1u,    pr_mat);
                    set_vs_vec4(hs, pv, 0x16u, spec4);
                    set_vs_vec4(hs, pv, 8u,    (fogp && hs.vs8_is_fog) ? fogp_adj : fogp);
                    set_vs_vec4(hs, pv, 7u,    fogc);
                    set_vs_vec4(hs, pv, 0xAu,  emi);
                    set_fs_vec4(hs, pv, 8u,    sky_bot);
                    set_fs_vec4(hs, pv, 9u,    sky_top);

                    if (cur_tex1 != env_tex_id) {
                        if (active_unit != 1u) {
                            rb.gl_active_texture(0x84C1u);
                            active_unit = 1u;
                        }
                        rb.gl_bind_texture(0x0DE1u, env_tex_id);
                        cur_tex1 = env_tex_id;
                    }

                    const uint64_t end_t = node_s + 0x38ULL;
                    uint64_t node_t = *reinterpret_cast<const uint64_t*>(node_s + 0x30ULL);
                    for (; node_t != end_t; node_t = tree_next(node_t)) {
                        const uint64_t vec_begin = *reinterpret_cast<const uint64_t*>(node_t + 0x38ULL);
                        const uint64_t vec_end   = *reinterpret_cast<const uint64_t*>(node_t + 0x40ULL);
                        if (vec_begin >= vec_end) continue;
                        if (*reinterpret_cast<const uint64_t*>(node_t + 0x30ULL) == 0ULL) continue;

                        for (uint64_t b = vec_begin; b < vec_end; b += kBatchStride) {
                            const uint64_t vb_wrap = *reinterpret_cast<const uint64_t*>(b + 0x40ULL);
                            const uint64_t ib_wrap = *reinterpret_cast<const uint64_t*>(b + 0x48ULL);
                            if (!vb_wrap || !ib_wrap) continue;
                            const uint64_t vb_ptr = *reinterpret_cast<const uint64_t*>(vb_wrap + 0x18ULL);
                            const uint64_t ib_ptr = *reinterpret_cast<const uint64_t*>(ib_wrap + 0x10ULL);
                            if (!vb_ptr || !ib_ptr) continue;
                            const uint32_t vbo = *reinterpret_cast<const uint32_t*>(vb_ptr);
                            const uint32_t ibo = *reinterpret_cast<const uint32_t*>(ib_ptr);
                            if (!vbo || !ibo) continue;

                            const uint32_t tex0 = get_tex_gl_id(*reinterpret_cast<const uint64_t*>(b + 0x50ULL), loading_id);
                            emit_model_batch(rb, hs, pv, reinterpret_cast<const uint8_t*>(b), shader_ptr, vbo, ibo, tex0,
                                             cur_tex0, cur_vao, active_unit);
                            ++draws_issued;
                        }
                    }
                }
            }

            if (cur_vao != 0) rb.gl_bind_vertex_array(0);
            if (active_unit != 0u) rb.gl_active_texture(0x84C0u);
            if (g_draw_call_count) *reinterpret_cast<uint32_t*>(g_draw_call_count) += draws_issued;
            if (!first_pass) {
                sync_guest_gl_state_after(cur_prog, blend_on, dmask_on, true);
            } else if (g_s_gl_state) {
                *reinterpret_cast<uint8_t*>(g_s_gl_state + 0x38ULL) = 0u;
            }
        }

        reset_pass_caches();
        cur_cpu->call_nested(fn_model_reset, {mgr_ptr, 0ULL});
        return 0;
    }
};

LcsBatchedFast& LcsBatchedFast::instance() {
    static LcsBatchedFast inst;
    return inst;
}

LcsBatchedFast::~LcsBatchedFast() {
    delete m_impl;
    m_impl = nullptr;
}

void LcsBatchedFast::initialize(const lcs::cpu::ElfImage& image_game,
                                lcs::cpu::DynarmicCore& cpu,
                                uint64_t load_bias) {
    if (!m_impl) m_impl = new Impl();
    if (m_impl->initialized) return;

    m_impl->cpu = &cpu;
    m_impl->load_bias = load_bias;

    auto resolve_sym = [&](uint64_t rva, const char* sym) -> uint64_t {
        if (uint64_t s = image_game.find_symbol(sym)) return s;
        return load_bias + rva;
    };

    m_impl->fn_world_reset  = resolve_sym(kSymWorldResetRva, "_ZN12BatchedWorld20C_WorldRenderManager5ResetEjb");
    m_impl->fn_model_reset  = resolve_sym(kSymModelResetRva, "_ZN12BatchedModel22C_DeferredBatchManager5ResetEb");
    m_impl->fn_get_vs_sid   = resolve_sym(kSymGetVsUniformStrIdRva, "_ZN7Display30GetVertexShaderUniformStringIdEj");
    m_impl->fn_get_fs_sid   = resolve_sym(kSymGetFsUniformStrIdRva, "_ZN7Display32GetFragmentShaderUniformStringIdEj");
    m_impl->fn_culling_update = resolve_sym(kSymCullingUpdateRva, "_ZN7Display9C_Culling6UpdateENS_10ECullSpaceE");
    m_impl->fn_ws_set_list_states   = resolve_sym(kSymWsSetListStatesRva, "_ZN12cWorldStream13SetListStatesER7cGuListi");
    m_impl->fn_ws_unset_list_states = resolve_sym(kSymWsUnsetListStatesRva, "_ZN12cWorldStream15UnsetListStatesER7cGuListi");
    m_impl->fn_ws_render_alpha      = resolve_sym(kSymWsRenderAlphaRva, "_ZN12cWorldStream11RenderAlphaEPK13sGeomInstanceiii");
    m_impl->fn_set_blend_mode_pass  = resolve_sym(kSymSetBlendModePassRva, "_Z19SetBlendModeForPassi");
    m_impl->fn_perf_sphere_cull     = resolve_sym(kSymPerfSphereCullRva, "_ZN7Display9C_Culling22PerformSphereCullCheckEN5Maths6SphereENS_10ECullSpaceE");
    m_impl->fn_sgeom_ctor           = resolve_sym(kSymSGeomInstCtorRva, "_ZN13sGeomInstanceC2Ev");

    m_impl->g_dv_enable_world   = read_got_ptr(load_bias, kGotDvEnableWorldRva);
    m_impl->g_dv_enable_model   = read_got_ptr(load_bias, kGotDvEnableModelRva);
    m_impl->g_world_sort_key    = read_got_ptr(load_bias, kGotWorldSortKeyRva);
    m_impl->g_model_sort_key    = read_got_ptr(load_bias, kGotModelSortKeyRva);
    m_impl->g_blend_en          = read_got_ptr(load_bias, kGotGlBlendEnableRva);
    m_impl->g_depth_test_en     = read_got_ptr(load_bias, kGotGlDepthTestEnableRva);
    m_impl->g_blend_eq          = read_got_ptr(load_bias, kGotGlBlendEquationRva);
    m_impl->g_depth_mask_en     = read_got_ptr(load_bias, kGotGlDepthMaskEnableRva);
    m_impl->g_blend_sep_en      = read_got_ptr(load_bias, kGotGlBlendSepFuncSetRva);
    m_impl->g_depth_offset      = read_got_ptr(load_bias, kGotGlDepthOffsetRva);
    m_impl->g_cur_pass_state    = read_got_ptr(load_bias, kGotCurrentPassStateRva);
    m_impl->g_s_gl_state        = read_got_ptr(load_bias, kGotLgGlStateRva);
    m_impl->g_color_eq          = read_got_ptr(load_bias, kGotColorEqRva);
    m_impl->g_alpha_eq          = read_got_ptr(load_bias, kGotAlphaEqRva);
    m_impl->g_firetv_hack       = read_got_ptr(load_bias, kGotFireTvHackRva);
    m_impl->g_last_zbias        = read_got_ptr(load_bias, kGotLastZBiasRva);
    m_impl->g_draw_call_count       = read_got_ptr(load_bias, kGotDrawCallCountRva);
    m_impl->g_pending_render_state  = read_got_ptr(load_bias, kGotPendingRenderStateRva);
    m_impl->g_loading_tex       = read_got_ptr(load_bias, kGotLoadingTextureRva);
    m_impl->g_dyn_shadow_tex    = read_got_ptr(load_bias, kGotDynShadowTexRva);
    m_impl->g_model_env_map     = read_got_ptr(load_bias, kGotModelEnvMapRva);

    m_impl->g_world_ambient     = read_got_ptr(load_bias, kGotWorldAmbientLightRva);
    m_impl->g_world_emissive    = read_got_ptr(load_bias, kGotWorldEmissiveLightRva);
    m_impl->g_fog_params        = read_got_ptr(load_bias, kGotModelFogParamsRva);
    m_impl->g_fog_colour        = read_got_ptr(load_bias, kGotModelFogColourRva);
    m_impl->g_dyn_shadow_centre = read_got_ptr(load_bias, kGotDynShadowCentreRva);
    m_impl->g_shadow_inner_rad  = read_got_ptr(load_bias, kGotShadowInnerRadRva);
    m_impl->g_shadow_outer_rad  = read_got_ptr(load_bias, kGotShadowOuterRadRva);

    m_impl->g_world_view_mat    = read_got_ptr(load_bias, kGotWorldViewMatRva);
    m_impl->g_proj_mat          = read_got_ptr(load_bias, kGotProjectionMatRva);
    m_impl->g_model_spec_pow    = read_got_ptr(load_bias, kGotModelSpecPowerRva);
    m_impl->g_model_spec_int    = read_got_ptr(load_bias, kGotModelSpecIntRva);
    m_impl->g_model_emissive    = read_got_ptr(load_bias, kGotModelEmissiveColRva);
    m_impl->g_sky_bottom        = read_got_ptr(load_bias, kGotSkyBottomRva);
    m_impl->g_sky_top           = read_got_ptr(load_bias, kGotSkyTopRva);

    m_impl->g_ws_pvs_enable         = read_got_ptr(load_bias, kGotWsPvsEnableRva);
    m_impl->g_ws_pvs_precomp        = read_got_ptr(load_bias, kGotWsPvsPrecompRva);
    m_impl->g_ws_pvs_cam_zone_ptr   = read_got_ptr(load_bias, kGotWsPvsCamZonePtrRva);
    m_impl->g_ws_pvs_vis_zones      = read_got_ptr(load_bias, kGotWsPvsVisZonesRva);
    m_impl->g_ws_world_mgr_ptr      = read_got_ptr(load_bias, kGotWsWorldMgrPtrRva);
    m_impl->g_ws_force_nontrans     = read_got_ptr(load_bias, kGotWsForceNonTransRva);
    m_impl->g_ws_render_trans       = read_got_ptr(load_bias, kGotWsRenderTransRva);
    m_impl->g_ws_render_nontrans    = read_got_ptr(load_bias, kGotWsRenderNonTransRva);
    m_impl->g_ws_aabb_cull          = read_got_ptr(load_bias, kGotWsAabbCullRva);
    m_impl->g_ws_mesh_aabb_cull     = read_got_ptr(load_bias, kGotWsMeshAabbCullRva);
    m_impl->g_ws_global_aabb_cull   = read_got_ptr(load_bias, kGotWsGlobalAabbCullRva);
    m_impl->g_ws_cull_model_mat     = read_got_ptr(load_bias, kGotWsCullModelMatRva);
    m_impl->g_ws_cull_change_flag   = read_got_ptr(load_bias, kGotWsCullChangeFlagRva);
    m_impl->g_ws_cull_frustum       = read_got_ptr(load_bias, kGotWsCullFrustumRva);
    m_impl->g_ws_cull_fade_enable   = read_got_ptr(load_bias, kGotWsCullFadeEnableRva);
    m_impl->g_ws_cull_fade_rate     = read_got_ptr(load_bias, kGotWsCullFadeRateRva);
    m_impl->g_ws_cull_ret_small     = read_got_ptr(load_bias, kGotWsCullRetOnSmallRva);
    m_impl->g_ws_the_camera         = read_got_ptr(load_bias, kGotWsTheCameraRva);
    m_impl->g_ws_cur_view_offs      = read_got_ptr(load_bias, kGotWsCurViewOffsRva);
    m_impl->g_ws_detail_settings    = read_got_ptr(load_bias, kGotWsDetailSettingsRva);
    m_impl->g_ws_prefs_dyn_shadows  = read_got_ptr(load_bias, kGotWsPrefsDynShadowsRva);
    m_impl->g_ws_recv_shadows_rad   = read_got_ptr(load_bias, kGotWsRecvShadowsRadRva);
    m_impl->g_ws_dyn_shadow_target  = read_got_ptr(load_bias, kGotWsDynShadowTargetRva);
    m_impl->g_ws_dyn_shadow_active  = read_got_ptr(load_bias, kGotWsDynShadowActiveRva);
    m_impl->g_ws_dbg_shader         = read_got_ptr(load_bias, kGotWsDebugShaderRva);
    m_impl->g_ws_render_alpha       = read_got_ptr(load_bias, kGotWsRenderAlphaRva);
    m_impl->g_ws_render_fade        = read_got_ptr(load_bias, kGotWsRenderFadeRva);
    m_impl->g_ws_current_pass       = read_got_ptr(load_bias, kGotWsCurrentPassRva);
    m_impl->g_ws_enable_deferred    = read_got_ptr(load_bias, kGotWsEnableDeferredRva);
    m_impl->g_ws_tex_zbias          = read_got_ptr(load_bias, kGotWsTexZBiasRva);
    m_impl->g_ws_shadow_strength    = read_got_ptr(load_bias, kGotWsShadowStrengthRva);
    m_impl->g_ws_current_depth_offs = read_got_ptr(load_bias, kGotWsCurrentDepthOffsRva);
    m_impl->g_ws_draw_count         = read_got_ptr(load_bias, kGotWsDrawCountRva);
    m_impl->g_ws_ex                 = read_got_ptr(load_bias, kGotWsWorldStreamExRva);
    m_impl->g_ws_render_nonfading   = read_got_ptr(load_bias, kGotWsRenderNonFadingRva);
    m_impl->g_ws_render_fading      = read_got_ptr(load_bias, kGotWsRenderFadingRva);
    m_impl->g_ws_sphere_cull_en     = read_got_ptr(load_bias, kGotWsSphereCullEnRva);
    m_impl->g_ws_clip_sphere_fudge  = read_got_ptr(load_bias, kGotWsClipSphereFudgeRva);
    m_impl->g_ws_last_sphere_state  = read_got_ptr(load_bias, kGotWsLastSphereStateRva);
    m_impl->g_ws_sphere_cull_en2    = read_got_ptr(load_bias, kGotWsSphereCullEn2Rva);
    m_impl->g_ws_cull_is_persp      = read_got_ptr(load_bias, kGotWsCullIsPerspRva);
    m_impl->g_ws_cull_last_plane    = read_got_ptr(load_bias, kGotWsCullLastPlaneRva);

    m_impl->shaders.reserve(64);
    m_impl->vaos.reserve(4096);

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t world_hle = hle.register_function(
        "__lcs_batched_world_render",
        [this](const uint64_t* args) -> uint64_t {
            return m_impl->render_world(args[0], static_cast<uint32_t>(args[1]));
        });
    const uint64_t model_hle = hle.register_function(
        "__lcs_batched_model_render",
        [this](const uint64_t* args) -> uint64_t {
            return m_impl->render_model(args[0]);
        });
    const uint64_t ws_nocull_hle = hle.register_function(
        "__lcs_ws_render_nocull",
        [this](const uint64_t* args) -> uint64_t {
            return m_impl->ws_render_nocull(args[0], args[1], args[2],
                                            static_cast<uint32_t>(args[3]),
                                            static_cast<uint32_t>(args[4]));
        });
    const uint64_t world_reset_hle = hle.register_function(
        "__lcs_batched_world_reset",
        [this](const uint64_t* args) -> uint64_t {
            return m_impl->reset_world_arena(static_cast<uint32_t>(args[1]));
        });
    const uint64_t model_reset_hle = hle.register_function(
        "__lcs_batched_model_reset",
        [this](const uint64_t*) -> uint64_t {
            return m_impl->reset_model_arena();
        });

    auto* world_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedWorldRenderRva);
    auto* model_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedModelRenderRva);
    m_impl->orig_world_render = *world_got;
    m_impl->orig_model_render = *model_got;
    *world_got = world_hle;
    *model_got = model_hle;

#if defined(LCS_PLATFORM_WINDOWS)
    {
        m_impl->arena_code_page = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        m_impl->arena_ctrl = static_cast<Impl::GuestArenaCtrl*>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        m_impl->arena_world_calls = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, static_cast<size_t>(Impl::kArenaWorldCap) * kDrawCallStride,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        m_impl->arena_world_meta = static_cast<Impl::FastBatchMeta*>(
            VirtualAlloc(nullptr, static_cast<size_t>(Impl::kArenaWorldCap) * sizeof(Impl::FastBatchMeta),
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        m_impl->arena_model_batches = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, static_cast<size_t>(Impl::kArenaModelCap) * kBatchStride,
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        m_impl->arena_model_meta = static_cast<Impl::FastBatchMeta*>(
            VirtualAlloc(nullptr, static_cast<size_t>(Impl::kArenaModelCap) * sizeof(Impl::FastBatchMeta),
                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));

        if (m_impl->arena_code_page && m_impl->arena_ctrl &&
            m_impl->arena_world_calls && m_impl->arena_world_meta &&
            m_impl->arena_model_batches && m_impl->arena_model_meta) {
            std::memset(m_impl->arena_ctrl, 0, sizeof(Impl::GuestArenaCtrl));
            m_impl->arena_ctrl->world_cap = Impl::kArenaWorldCap;
            m_impl->arena_ctrl->world_calls_ptr = reinterpret_cast<uint64_t>(m_impl->arena_world_calls);
            m_impl->arena_ctrl->world_meta_ptr  = reinterpret_cast<uint64_t>(m_impl->arena_world_meta);
            m_impl->arena_ctrl->model_cap = Impl::kArenaModelCap;
            m_impl->arena_ctrl->model_batches_ptr = reinterpret_cast<uint64_t>(m_impl->arena_model_batches);
            m_impl->arena_ctrl->model_meta_ptr    = reinterpret_cast<uint64_t>(m_impl->arena_model_meta);

            // Capstone-verified ARM64 arena allocator stubs (+0x000 / +0x200) and cWorldStreamEx::RenderNoCull trampolines (+0x480 / +0x4A0):
            static constexpr uint32_t kWorldGetFreeInsns[33] = {
                0xB40003E4u, 0xF9407888u, 0xB40003A8u, 0xF9401108u, 0xB4000368u,
                0x58001F69u, 0xB940012Au, 0xB940052Bu, 0x6B0B015Fu, 0x54000282u,
                0x1100054Bu, 0xB900012Bu, 0xF9400D2Bu, 0x8B0A156Bu, 0xA9001162u,
                0xF9000963u, 0xB9001961u, 0xF940092Bu, 0x52800B0Cu, 0x9BAC7D4Cu,
                0x8B0C0160u, 0xA9007C1Fu, 0xA9017C1Fu, 0xA9027C1Fu, 0xF9001804u,
                0xF9001C1Fu, 0xA9047C1Fu, 0xF900281Fu, 0xD65F03C0u, 0x5280002Au,
                0xB900092Au, 0xAA1F03E0u, 0xD65F03C0u
            };
            static constexpr uint32_t kModelGetFreeInsns[29] = {
                0xB4000364u, 0xF9407888u, 0xB4000328u, 0xF9401108u, 0xB40002E8u,
                0x58000F69u, 0xB940212Au, 0xB940252Bu, 0x6B0B015Fu, 0x54000202u,
                0x1100054Bu, 0xB900212Bu, 0xF9401D2Bu, 0x8B0A156Bu, 0xA9001162u,
                0xF9000963u, 0xB9001961u, 0xF940192Bu, 0x52819F0Cu, 0x9BAC7D4Cu,
                0x8B0C0160u, 0xF9002804u, 0xB9005C1Fu, 0xB900601Fu, 0xD65F03C0u,
                0x5280002Au, 0xB900292Au, 0xAA1F03E0u, 0xD65F03C0u
            };

            std::memcpy(m_impl->arena_code_page + 0x000, kWorldGetFreeInsns, sizeof(kWorldGetFreeInsns));
            std::memcpy(m_impl->arena_code_page + 0x200, kModelGetFreeInsns, sizeof(kModelGetFreeInsns));
            *reinterpret_cast<uint64_t*>(m_impl->arena_code_page + 0x400) =
                reinterpret_cast<uint64_t>(m_impl->arena_ctrl);

            auto* ws_nc_got = reinterpret_cast<uint64_t*>(load_bias + kGotWsRenderNoCullRva);
            m_impl->orig_ws_render_nocull = *ws_nc_got;

            #pragma pack(push, 1)
            struct FloatArgTramp {
                uint32_t insns[4];
                uint64_t target;
            };
            #pragma pack(pop)
            const FloatArgTramp tramp_to_hle{
                {0x1E260004u, 0x58000070u, 0xD61F0200u, 0xD503201Fu},
                ws_nocull_hle
            };
            const FloatArgTramp tramp_to_orig{
                {0x1E270080u, 0x58000070u, 0xD61F0200u, 0xD503201Fu},
                m_impl->orig_ws_render_nocull
            };
            std::memcpy(m_impl->arena_code_page + 0x480, &tramp_to_hle,  sizeof(tramp_to_hle));
            std::memcpy(m_impl->arena_code_page + 0x4A0, &tramp_to_orig, sizeof(tramp_to_orig));

            DWORD old_code_prot = 0;
            VirtualProtect(m_impl->arena_code_page, 4096, PAGE_EXECUTE_READ, &old_code_prot);
            FlushInstructionCache(GetCurrentProcess(), m_impl->arena_code_page, 4096);

            auto* wg_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedWorldGetFreeRva);
            auto* wr_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedWorldResetRva);
            auto* mg_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedModelGetFreeRva);
            auto* mr_got = reinterpret_cast<uint64_t*>(load_bias + kGotBatchedModelResetRva);
            m_impl->orig_world_get_free = *wg_got;
            m_impl->orig_world_reset    = *wr_got;
            m_impl->orig_model_get_free = *mg_got;
            m_impl->orig_model_reset    = *mr_got;
            m_impl->tramp_ws_orig       = reinterpret_cast<uint64_t>(m_impl->arena_code_page + 0x4A0);
            *wg_got    = reinterpret_cast<uint64_t>(m_impl->arena_code_page + 0x000);
            *wr_got    = world_reset_hle;
            *mg_got    = reinterpret_cast<uint64_t>(m_impl->arena_code_page + 0x200);
            *mr_got    = model_reset_hle;
            *ws_nc_got = reinterpret_cast<uint64_t>(m_impl->arena_code_page + 0x480);
            m_impl->arena_enabled = true;
            m_impl->sort_entries.reserve(8192);
        }
    }
    auto patch_arm64_hle_bridge = [&cpu](uint64_t addr, const void* bytes, size_t size) -> bool {
        DWORD old_prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_EXECUTE_READWRITE, &old_prot)) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(addr), bytes, size);
        DWORD dummy = 0;
        VirtualProtect(reinterpret_cast<void*>(addr), size, old_prot, &dummy);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(addr), size);
        cpu.invalidate_cache(addr, size);
        if (auto* cur = lcs::cpu::DynarmicCore::current(); cur && cur != &cpu) {
            cur->invalidate_cache(addr, size);
        }
        return true;
    };
    {
        const uint64_t fade_hle = hle.register_function(
            "__lcs_cworld_fade_loop",
            [load_bias](const uint64_t*) -> uint64_t {
                return run_cworld_fade_loop(load_bias);
            });
        #pragma pack(push, 1)
        struct FadePatch {
            uint32_t insns[7];
            uint64_t target;
        } patch = {
            {
                0xD10043FFu, // 0x00: sub sp, sp, #0x10
                0xA9007BFDu, // 0x04: stp x29, x30, [sp]
                0x580000B0u, // 0x08: ldr x16, +0x14 (-> 0x1c)
                0xD63F0200u, // 0x0C: blr x16
                0xA8C17BFDu, // 0x10: ldp x29, x30, [sp], #0x10
                0x14000045u, // 0x14: b +0x114 (0x377f88 -> 0x37809c)
                0xD503201Fu, // 0x18: nop
            },
            fade_hle
        };
        #pragma pack(pop)
        patch_arm64_hle_bridge(load_bias + kSymCWorldFadeStartRva, &patch, sizeof(patch));
    }
    if (m_impl->g_ws_ex != 0 && m_impl->fn_ws_set_list_states != 0) {
        const uint64_t ws_bucket_hle = hle.register_function(
            "__lcs_cworld_bucket_loop",
            [this](const uint64_t* args) -> uint64_t {
                return m_impl->run_cworld_bucket_loop(
                    args[0],
                    static_cast<uint32_t>(args[1]),
                    static_cast<uint32_t>(args[2]),
                    args[3]);
            });
        #pragma pack(push, 1)
        struct WsBucketPatch {
            uint32_t insns[10];
            uint64_t target;
        } ws_patch = {
            {
                0xA9BF7BFDu, // 0x00: stp x29, x30, [sp, #-0x10]!
                0xAA1303E0u, // 0x04: mov x0, x19
                0x2A1A03E1u, // 0x08: mov w1, w26
                0xB9404BE2u, // 0x0C: ldr w2, [sp, #0x48]
                0x910223E3u, // 0x10: add x3, sp, #0x88
                0x580000B0u, // 0x14: ldr x16, #0x39ffe4 (+0x28)
                0xD63F0200u, // 0x18: blr x16
                0xA8C17BFDu, // 0x1C: ldp x29, x30, [sp], #0x10
                0x14000096u, // 0x20: b #0x3a0234 (0x39ffdc + 0x258)
                0xD503201Fu, // 0x24: nop
            },
            ws_bucket_hle
        };
        #pragma pack(pop)
        patch_arm64_hle_bridge(load_bias + kSymWsRenderBucketStartRva, &ws_patch, sizeof(ws_patch));
    }
#endif

    m_impl->initialized = true;
    std::cout << "[LcsBatchedFast] Installed native C++ BatchedWorld, BatchedModel, cWorldStream & CWorld fade fast paths.\n";
}

} // namespace lcs::game
