#pragma once

#include <cstdint>

namespace lcs::cpu {
class ElfImage;
class DynarmicCore;
} // namespace lcs::cpu

namespace lcs::game {

// VCS-Recomp-style 1:1 mouse/right-stick Free Aim controller for GTA: Liberty City Stories.
// Installs GOT hooks on libGame.so (CPad::EnterFreeAim, CPlayerPed::PlayerControlZelda,
// CPlayerPed::PlayerControlFreeAim, CPlayerPed::FindNextWeaponLockOnTarget,
// CPed::SetWeaponLockOnTarget, CCamera::Process, CWorld::ProcessLineOfSight).
class LCSFreeAim {
public:
    static LCSFreeAim& instance();

    bool install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);

    // Called once per frame from the main loop before dispatch_events().
    // Updates enabled state, held-aim desire, gameplay context, and accumulates 1:1 mouse radians + stick rates.
    void update_frame_input(bool enabled,
                            bool want_aim,
                            bool in_menu,
                            bool in_cutscene,
                            bool in_vehicle,
                            bool mouse_look_mode,
                            float mouse_turn_rad,
                            float mouse_tilt_rad,
                            float stick_rx,
                            float stick_ry,
                            float trigger_l2,
                            float trigger_r2,
                            float dt_sec);

    // True while the player is actively in 1:1 free-aim mode on foot with a free-aim gun.
    bool is_active() const { return m_active; }
    void set_kb_look_flags(bool left, bool right, bool behind) {
        m_kb_look_left = left;
        m_kb_look_right = right;
        m_kb_look_behind = behind;
    }
    void set_classic_ps2_triggers(bool classic) { m_classic_ps2_triggers = classic; }

    // Exposed for telemetry/testing.
    float yaw() const { return m_yaw; }
    float pitch() const { return m_pitch; }
    uint32_t shot_ray_count() const { return m_shot_rays; }

private:
    LCSFreeAim() = default;

    bool m_installed = false;
    bool m_enabled = true;
    bool m_want_aim = false;
    bool m_in_menu = false;
    bool m_in_cutscene = false;
    bool m_in_vehicle = false;
    bool m_active = false;
    bool m_mouse_look_mode = true;
    bool m_zelda_suspended_free_aim = false;
    bool m_forced_mouse_3rd = false;
    bool m_has_saved_pad = false;
    bool m_kb_look_left = false;
    bool m_kb_look_right = false;
    bool m_kb_look_behind = false;
    bool m_classic_ps2_triggers = false;
    int16_t m_saved_pad_lx = 0;
    int16_t m_saved_pad_ly = 0;

    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    float m_pending_turn = 0.0f;
    float m_pending_tilt = 0.0f;
    float m_look_turn = 0.0f;
    float m_look_tilt = 0.0f;
    float m_stick_rx = 0.0f;
    float m_stick_ry = 0.0f;
    float m_trigger_l2 = 0.0f;
    float m_trigger_r2 = 0.0f;
    float m_dt_sec = 1.0f / 60.0f;

    int m_handback_frames = 0;
    float m_handback_yaw = 0.0f;
    float m_car_cam_beta = 0.0f;
    float m_car_cam_vert = 0.0f;
    float m_car_cam_hold_timer = 0.0f;
    float m_ped_wall_tail_timer = 0.0f;
    uint32_t m_shot_rays = 0;

    uint64_t m_load_bias = 0;
    uint64_t m_the_camera = 0;
    uint64_t m_player_idx_addr = 0;
    uint64_t m_players_arr_addr = 0;
    uint64_t m_timestep_addr = 0;
    uint64_t m_femm_addr = 0;
    uint64_t m_fire_hit_start = 0;
    uint64_t m_fire_hit_end = 0;
    uint64_t m_scratch_start_va = 0;
    uint64_t m_car_scratch_va = 0;

    uint64_t get_player_ped() const;
    uint64_t get_active_cam() const;
    bool is_player_on_foot_with_gun(uint64_t ped) const;
};

} // namespace lcs::game
