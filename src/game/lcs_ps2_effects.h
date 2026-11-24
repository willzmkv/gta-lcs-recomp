// Authentic PS2 Graphical Effects Pipeline for GTA: Liberty City Stories (PC Recomp)
// Extracted and reverse-engineered from PS2 SLUS_214.23 & CHK/PS2/GAME.DTZ:
//   - PS2 CTimeCycle lookup tables (0x2880 bytes from GAME.DTZ @ 0x000008c0 + 0x0280)
//   - PS2 CMBlur::GetFinalColour (SLUS_214.23 @ 0x001948c0 / libGame.so @ 0x00485f68)
//   - PS2 Radiosity & BlurRGB Self-Modulated Bloom/Color Filter (SLUS_214.23 @ 0x00194178 -> 0x001d17d8)
//   - PS2 Frame-History Trails & Drunk Blur (SLUS_214.23 @ 0x00272348 & 0x00274910)
//   - PS2 High-Speed Vehicle Motion Blur
// Executed at the exact 3D -> 2D boundary (CMBlur::MotionBlurRender, GOT 0x801e88)
// before Render2dStuff() so the 2D HUD, Radar, Crosshair, Subtitles, and Menus stay razor-sharp.
#pragma once

#include "graphics/render_bridge.h"

#include <array>
#include <cstdint>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
}

namespace lcs::game {

class LCSPs2Effects {
public:
    static LCSPs2Effects& instance();

    bool install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    void update(float dt_sec);

    // Live diagnostic state for telemetry / verification
    bool is_installed() const { return m_installed; }
    bool is_timecycle_captured() const { return m_has_orig_timecycle; }
    int active_timecycle_mode() const { return m_applied_timecycle_mode; }
    void get_last_blur_rgb(float& out_r, float& out_g, float& out_b) const {
        out_r = m_last_blur_rgb[0];
        out_g = m_last_blur_rgb[1];
        out_b = m_last_blur_rgb[2];
    }
    float get_last_trails_alpha() const { return m_last_trails_alpha; }
    uint64_t get_render_pass_count() const { return m_render_pass_count; }

private:
    LCSPs2Effects() = default;

    uint64_t on_mblur_pre(uint64_t cmblur_ptr, uint64_t rsl_cam_ptr);
    uint64_t on_mblur_post(uint64_t cmblur_ptr, uint64_t rsl_cam_ptr);
    uint64_t on_pre_hud_flush();
    bool sync_timecycle_tables(uint64_t tc_ptr);
    uint64_t get_player_vehicle() const;

    bool m_installed = false;
    uint64_t m_load_bias = 0;
    lcs::cpu::DynarmicCore* m_cpu = nullptr;

    // Resolved guest addresses in libGame.so
    uint64_t m_p_timecycle_addr = 0;      // RVA 0x00812038 (CTimeCycle* pTimeCycle)
    uint64_t m_the_camera_addr = 0;       // RVA 0x00850af0 (CCamera TheCamera; m_Blur at +0x1aa8)
    uint64_t m_blur_on_addr = 0;          // RVA 0x00818dec (bool CMBlur::BlurOn)
    uint64_t m_min_blur_on_addr = 0;      // RVA 0x00818ded (bool gbMinBlurOn)
    uint64_t m_min_blur_amt_addr = 0;     // RVA 0x00818dee (uint8_t gMinBlurAmt; PS2=28, Mobile=40)
    uint64_t m_fade_red_addr = 0;         // RVA 0x008bb148 (uint8_t CDraw::FadeRed)
    uint64_t m_fade_green_addr = 0;       // RVA 0x008bb149 (uint8_t CDraw::FadeGreen)
    uint64_t m_fade_blue_addr = 0;        // RVA 0x008bb14a (uint8_t CDraw::FadeBlue)
    uint64_t m_fade_val_addr = 0;         // RVA 0x008bb14b (uint8_t CDraw::FadeValue)
    uint64_t m_game_state_addr = 0;       // gGameState (9 = active gameplay)
    uint64_t m_player_idx_addr = 0;       // CWorld::PlayerInFocus
    uint64_t m_players_arr_addr = 0;      // CWorld::Players
    uint64_t m_fn_tc_force_update = 0;    // RVA 0x0036d570 (CTimeCycle::ForceUpdate)
    uint64_t m_fn_clear_depth_only = 0;   // RVA 0x0059945c (Display::ClearDepthOnly)
    uint64_t m_fn_lgl_gl_flush = 0;       // RVA 0x005a7d40 (LgGlAPI::glFlush -> queues opcode 0x1b in gRenderQueue)

    // Backed-up original Recomp/iPad TIMECYC.DAT tables (+0x0280..+0x2aff, 0x2880 bytes)
    bool m_has_orig_timecycle = false;
    uint64_t m_last_tc_ptr = 0;
    int m_applied_timecycle_mode = -1;
    int m_applied_pipeline_on = -1;
    std::array<uint8_t, 0x2880> m_orig_timecycle_tables{};

    // Staged per-frame PS2 post-process parameters computed in CMBlur::MotionBlurRender (0x00523ec4)
    // and flushed into gRenderQueue at Display::ClearDepthOnly (0x00523fc0), right after
    // Services::SetBackBuffer (0x00523f00) and Display::RenderPassThrough (0x00523fa0)
    // and immediately before Render2dStuff (0x00523fc4).
    lcs::graphics::Ps2PostProcessParams m_staged_params{};
    bool m_has_staged_params = false;

    // Frame timing & camera cut detection
    float m_dt_sec = 1.0f / 60.0f;
    float m_anim_time = 0.0f;
    float m_prev_cam_pos[3] = {0.0f, 0.0f, 0.0f};
    bool m_has_prev_cam_pos = false;
    bool m_force_reset_history = true;

    // Telemetry state
    float m_last_blur_rgb[3] = {0.0f, 0.0f, 0.0f};
    float m_last_trails_alpha = 0.0f;
    uint64_t m_render_pass_count = 0;
};

} // namespace lcs::game
