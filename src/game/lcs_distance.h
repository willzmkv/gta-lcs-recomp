#pragma once

#include <cstdint>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
} // namespace lcs::cpu

namespace lcs::game {

// Native PC view-distance, detail LOD, population range, density & pool-scaling manager.
// Hooks CPools::Initialise, GetLODSettingsLevel, GetCullFadeLevel,
// C_DeviceDetailSettings::GetLODSettings, CCamera::Process, CDraw / RslCameraSetFarClipPlane,
// CSimpleModelInfo::GetElementFromDistance, CRenderer::SetupBigBuildingVisibility, and
// scales the hardcoded 330.0f mobile visibility clamp to match world.view_distance.
class LcsDistance {
public:
    static LcsDistance& instance();

    void install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    void sync_settings_now();
    void adjust_fog_parameters(float in_out[4]) const;

    float last_lod_scale() const { return m_last_lod_scale; }
    float last_far_clip() const { return m_last_far_clip; }
    bool  pools_expanded() const { return m_pools_expanded; }

private:
    LcsDistance() = default;
    ~LcsDistance();
    LcsDistance(const LcsDistance&) = delete;
    LcsDistance& operator=(const LcsDistance&) = delete;

    struct HookOps;
    friend struct HookOps;

    void on_pools_initialise_post();
    void on_vis_init_post();
    uint64_t on_camera_process_post(uint64_t cam_ptr);
    uint64_t on_get_lod_settings_pre(uint64_t dev_ptr);
    void on_get_lod_settings_post(uint64_t orig_ret_ptr);
    uint64_t on_far_clip_set_pre(uint64_t rsl_cam, uint32_t raw_far_bits);
    uint64_t on_big_building_check(uint64_t entity_ptr);
    uint64_t on_get_elem_from_dist_pre(uint64_t model_info, uint32_t dist_bits);
    uint64_t on_get_largest_lod_post(uint64_t model_info, uint32_t orig_ret_bits);
    void on_scan_world_pre();
    void on_scan_world_post();
    void update_330m_patches(float view_scale);

    lcs::cpu::DynarmicCore* m_cpu = nullptr;
    uint64_t m_load_bias = 0;
    uint8_t* m_stub_page = nullptr;

    bool  m_installed = false;
    bool  m_pools_expanded = false;
    bool  m_has_base_lod = false;
    bool  m_vw_patched = false;
    float m_saved_vw_x = 0.0f;
    bool  m_has_base_ms_lod = false;
    bool  m_has_base_draw_lod = false;
    float m_base_ms_lod_scale = 1.0f;
    float m_base_draw_lod_dist = 0.0f;
    float m_base_far_clip = 0.0f;
    float m_last_scaled_far = 0.0f;
    float m_base_gen_mul = 1.0f;
    float m_last_gen_mul = 0.0f;
    float m_last_lod_scale = 1.0f;
    float m_last_far_clip = 0.0f;
    float m_applied_330_clamp = -1.0f;

    uint64_t m_orig_pools_init = 0;
    uint64_t m_orig_vis_init = 0;
    uint64_t m_orig_cam_process = 0;
    uint64_t m_orig_get_lod_settings = 0;
    uint64_t m_orig_get_elem_dist = 0;
    uint64_t m_orig_get_largest_lod = 0;
    uint64_t m_orig_setup_big_bldg = 0;
    uint64_t m_orig_set_far_clip = 0;
    uint64_t m_orig_ws_pdyn = 0;
    uint64_t m_fn_znam = 0;
    uint64_t m_fn_zdapv = 0;
    uint32_t m_orig_scan_req_cap_insn = 0;
    float    m_applied_pop_convert = -1.0f;
    float    m_applied_300_clamp   = -1.0f;
    float    m_applied_add_dyn_sq  = -1.0f;
    int      m_applied_keep_sec    = -1;

    // Guest addresses inside stub_page for zero-trap ARM64 stubs
    uint64_t m_va_scaled_lod_struct = 0;
    uint64_t m_va_scaled_lod_ready  = 0;
    uint64_t m_va_inv_detail_scale  = 0;
    uint64_t m_va_cam_new_far_clip  = 0;
    uint64_t m_va_set_far_scaled    = 0;

    uint8_t  m_base_lod_raw[76]{};
};

} // namespace lcs::game
