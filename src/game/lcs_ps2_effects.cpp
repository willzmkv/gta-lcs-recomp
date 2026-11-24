#include "game/lcs_ps2_effects.h"
#include "game/ps2_timecyc_data.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "graphics/render_bridge.h"
#include "settings/settings.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

using lcs_ps2_fx::kPs2TimeCycleTables;
constexpr size_t kPs2TimeCycleTableBytes = lcs_ps2_fx::kPs2TimeCycleTablesSize;

// Verified symbol RVAs and GOT slots in libGame.so (arm64-v8a)
constexpr uint64_t kRvaPTimeCycle         = 0x00812038ULL;
constexpr uint64_t kRvaTheCamera          = 0x00850af0ULL;
constexpr uint64_t kRvaCMBlurBlurOn       = 0x00818decULL;
constexpr uint64_t kRvaGbMinBlurOn        = 0x00818dedULL;
constexpr uint64_t kRvaGMinBlurAmt        = 0x00818deeULL;
constexpr uint64_t kRvaCDrawFadeRed       = 0x008bb148ULL;
constexpr uint64_t kRvaCDrawFadeGreen     = 0x008bb149ULL;
constexpr uint64_t kRvaCDrawFadeBlue      = 0x008bb14aULL;
constexpr uint64_t kRvaCDrawFadeValue     = 0x008bb14bULL;
constexpr uint64_t kRvaCTimeCycleForceUpd = 0x0036d570ULL;
constexpr uint64_t kRvaDisplayClearDepth  = 0x0059945cULL; // _ZN7Display14ClearDepthOnlyEff
constexpr uint64_t kRvaLgGlApiGlFlush     = 0x005a7d40ULL; // _ZN7LgGlAPI7glFlushEv (pushes opcode 0x1b into gRenderQueue)

constexpr uint64_t kGotMotionBlurRender   = 0x00801e88ULL; // _ZN6CMBlur16MotionBlurRenderEP9RslCamera
constexpr uint64_t kGotDisplayClearDepth  = 0x00807c70ULL; // _ZN7Display14ClearDepthOnlyEff (called at 0x00523fc0 in DrawGame)
constexpr uint64_t kGotPlayerIdx          = 0x007f9118ULL;
constexpr uint64_t kGotPlayersArr         = 0x007f9120ULL;

// PS2 CMBlur::GetFinalColour lookup tables at libGame.so RVA 0x0023d3c4..0x0023d483
// Used when m_nBlurType is in 1..12 and bit (m_nBlurType - 1) is set in mask 0x9b5 ({1, 3, 5, 6, 8, 9, 12}).
// Normal gameplay uses m_nBlurType == 2 (not in 0x9b5), which uses CTimeCycle BlurRGB via m_nDefaultRed/Green/Blue.
constexpr uint64_t kRvaBlurTypeBlueTbl    = 0x0023d3c4ULL;
constexpr uint64_t kRvaBlurTypeAlphaTbl   = 0x0023d3f4ULL;
constexpr uint64_t kRvaBlurTypeGreenTbl   = 0x0023d424ULL;
constexpr uint64_t kRvaBlurTypeRedTbl     = 0x0023d454ULL;

// Authentic PS2 constants from SLUS_214.23 ($gp = 0x003d6ef0):
// -0x6098($gp) (0x003d0e58): gMinBlurAmt       = 28 (0x1c)
// -0x6090($gp) (0x003d0e60): gRadiosityOffset  = 0.188f
// -0x608c($gp) (0x003d0e64): gRadiosityAlpha   = 64 (>> 1 = 32 -> 32/128 = 0.25f per tap)
constexpr uint8_t kPs2MinBlurAmt = 28u;
constexpr uint8_t kMobileMinBlurAmt = 40u;

constexpr uint32_t ldr_x16_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000010u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

} // namespace

LCSPs2Effects& LCSPs2Effects::instance() {
    static LCSPs2Effects s_instance;
    return s_instance;
}

void LCSPs2Effects::update(float dt_sec) {
    m_dt_sec = std::clamp(dt_sec, 1.0f / 500.0f, 0.125f);
    m_anim_time += m_dt_sec;
}

uint64_t LCSPs2Effects::get_player_vehicle() const {
    if (!m_player_idx_addr || !m_players_arr_addr) return 0;
    const uint8_t idx = *reinterpret_cast<const uint8_t*>(m_player_idx_addr);
    const uint64_t ped = *reinterpret_cast<const uint64_t*>(
        m_players_arr_addr + static_cast<uint64_t>(idx) * 0x1a0ULL);
    if (!ped) return 0;
    const uint8_t in_veh = *reinterpret_cast<const uint8_t*>(ped + 0x618ULL);
    if (!in_veh) return 0;
    return *reinterpret_cast<const uint64_t*>(ped + 0x610ULL);
}

bool LCSPs2Effects::sync_timecycle_tables(uint64_t tc_ptr) {
    if (!tc_ptr) return false;

    auto* dst_tables = reinterpret_cast<uint8_t*>(tc_ptr + 0x0280ULL);
    // Sentinel offset: Noon (12:00) ExtraSunny BlurRed at +0x2340 + 12*8 = 0x23a0.
    // Mobile TIMECYC.DAT has 0 here, whereas PS2 GAME.DTZ has 37 (0x25).
    // If CTimeCycle::Initialise() reloaded TIMECYC.DAT from disk during StartNewGame() or LoadGameFromGate(),
    // dst_tables[0x23a0] resets back to 0 even if m_applied_timecycle_mode was already > 0.
    constexpr size_t kNoonBlurRedOff = 0x2340ULL + 12ULL * 8ULL;

    if (!m_has_orig_timecycle) {
        std::memcpy(m_orig_timecycle_tables.data(), dst_tables, kPs2TimeCycleTableBytes);
        m_has_orig_timecycle = true;
        m_last_tc_ptr = tc_ptr;
        std::cout << "  [PS2Effects] Captured original Recomp TIMECYC.DAT tables (0x"
                  << std::hex << kPs2TimeCycleTableBytes << std::dec << " bytes).\n";
    }

    const auto& cfg = lcs::settings::get();
    const int pipeline_on = (cfg.ps2_pipeline != 0) ? 1 : 0;
    const int wanted_mode = pipeline_on ? std::clamp(cfg.ps2_timecycle, 0, 2) : 0;

    const bool reloaded_by_game =
        (tc_ptr != m_last_tc_ptr) ||
        (pipeline_on != 0 && dst_tables[kNoonBlurRedOff] == 0 && kPs2TimeCycleTables[kNoonBlurRedOff] != 0);

    if (!reloaded_by_game && wanted_mode == m_applied_timecycle_mode && pipeline_on == m_applied_pipeline_on) {
        return false;
    }
    m_last_tc_ptr = tc_ptr;

    if (wanted_mode == 1) {
        // Mode 1: 100% Authentic PS2 TimeCycle extracted from CHK/PS2/GAME.DTZ
        std::memcpy(dst_tables, kPs2TimeCycleTables, kPs2TimeCycleTableBytes);
    } else if (wanted_mode == 2) {
        // Mode 2: Hybrid PS2 + Recomp
        // - Blends all uint8 color/sky/ambient/directional/sun/fog tables 50/50
        // - Blends signed int8 multiplier tables (0x1440..0x167f) 50/50 as int8_t
        // - Takes max(ps2, recomp) for int16_t FarClip (0x18c0..0x1a3f) so PC draw distance is never reduced
        // - Blends int16_t FogStart (0x1a40..0x1bbf) 50/50 as int16_t
        // - Uses 100% PS2 BlurRed/BlurGreen/BlurBlue (0x2340..0x257f) for authentic PS2 Radiosity & Color Filter
        for (size_t i = 0; i < kPs2TimeCycleTableBytes; ++i) {
            if (i >= 0x18c0 && i < 0x1bc0) {
                continue; // Handled below as int16_t
            }
            if (i >= 0x2340 && i < 0x2580) {
                dst_tables[i] = kPs2TimeCycleTables[i];
            } else if (i >= 0x1440 && i < 0x1680) {
                const int a = static_cast<int>(static_cast<int8_t>(kPs2TimeCycleTables[i]));
                const int b = static_cast<int>(static_cast<int8_t>(m_orig_timecycle_tables[i]));
                dst_tables[i] = static_cast<uint8_t>(static_cast<int8_t>((a + b) / 2));
            } else {
                const int a = static_cast<int>(kPs2TimeCycleTables[i]);
                const int b = static_cast<int>(m_orig_timecycle_tables[i]);
                dst_tables[i] = static_cast<uint8_t>((a + b + 1) / 2);
            }
        }
        // int16_t FarClip [0x18c0..0x1a3f] (192 int16 entries)
        for (size_t off = 0x18c0; off < 0x1a40; off += 2) {
            int16_t ps2_v = 0, rec_v = 0;
            std::memcpy(&ps2_v, &kPs2TimeCycleTables[off], sizeof(int16_t));
            std::memcpy(&rec_v, &m_orig_timecycle_tables[off], sizeof(int16_t));
            const int16_t out_v = std::max(ps2_v, rec_v);
            std::memcpy(&dst_tables[off], &out_v, sizeof(int16_t));
        }
        // int16_t FogStart [0x1a40..0x1bbf] (192 int16 entries)
        for (size_t off = 0x1a40; off < 0x1bc0; off += 2) {
            int16_t ps2_v = 0, rec_v = 0;
            std::memcpy(&ps2_v, &kPs2TimeCycleTables[off], sizeof(int16_t));
            std::memcpy(&rec_v, &m_orig_timecycle_tables[off], sizeof(int16_t));
            const int16_t out_v = static_cast<int16_t>((static_cast<int>(ps2_v) + static_cast<int>(rec_v)) / 2);
            std::memcpy(&dst_tables[off], &out_v, sizeof(int16_t));
        }
    } else {
        // Mode 0: 100% Pure PC Recomp / iPad base tables (unmodified).
        std::memcpy(dst_tables, m_orig_timecycle_tables.data(), kPs2TimeCycleTableBytes);
    }

    m_applied_timecycle_mode = wanted_mode;
    m_applied_pipeline_on = pipeline_on;
    return true;
}

uint64_t LCSPs2Effects::on_mblur_pre(uint64_t /*cmblur_ptr*/, uint64_t /*rsl_cam_ptr*/) {
    if (!m_p_timecycle_addr) return 0;
    const uint32_t gstate = m_game_state_addr ? *reinterpret_cast<const uint32_t*>(m_game_state_addr) : 9u;
    if (gstate != 9u) return 0;
    const uint64_t tc_ptr = *reinterpret_cast<const uint64_t*>(m_p_timecycle_addr);
    if (!tc_ptr) return 0;

    if (sync_timecycle_tables(tc_ptr)) {
        // Return tc_ptr so our AArch64 stub invokes CTimeCycle::ForceUpdate(tc_ptr) natively before on_mblur_post!
        return tc_ptr;
    }
    return 0;
}

uint64_t LCSPs2Effects::on_mblur_post(uint64_t cmblur_ptr, uint64_t /*rsl_cam_ptr*/) {
    if (!cmblur_ptr) return 0;

    // Always clear queued lens Fx count (m_nNumFx at CMBlur + 0x20) because Rockstar Leeds stubbed
    // CMBlur::CreateImmediateModeData (0x485a54) to `ret` on both PS2 and ARM64.
    *reinterpret_cast<int32_t*>(cmblur_ptr + 0x20ULL) = 0;

    const auto& cfg = lcs::settings::get();
    const int tc_mode = std::clamp(cfg.ps2_timecycle, 0, 2);
    const bool ps2_active = (cfg.ps2_pipeline != 0) && (tc_mode != 0);

    if (m_blur_on_addr) {
        *reinterpret_cast<uint8_t*>(m_blur_on_addr) = 1u;
    }
    if (m_min_blur_on_addr) {
        *reinterpret_cast<uint8_t*>(m_min_blur_on_addr) = 1u;
    }
    if (m_min_blur_amt_addr) {
        *reinterpret_cast<uint8_t*>(m_min_blur_amt_addr) =
            ps2_active ? kPs2MinBlurAmt : kMobileMinBlurAmt;
    }

    const uint32_t gstate = m_game_state_addr ? *reinterpret_cast<const uint32_t*>(m_game_state_addr) : 9u;
    if (!ps2_active || gstate != 9u) {
        m_force_reset_history = true;
        m_has_prev_cam_pos = false;
        m_has_staged_params = false;
        m_last_trails_alpha = 0.0f;
        lcs::graphics::Ps2PostProcessParams disabled_pp{};
        disabled_pp.enabled = false;
        lcs::graphics::RenderBridge::instance().queue_ps2_post_process(disabled_pp);
        return 0;
    }

    // Mode 1 = PS2 Original (100% PS2 GAME.DTZ + 100% PS2 GS sub-pixel filter, radiosity, color filter & trails)
    // Mode 2 = Híbrido PS2 + Recomp (50/50 TimeCycle + sharp PC textures + balanced 65% PS2 Radiosity, 60% Color Filter, 55% Trails)
    const float mode_rad_scale    = (tc_mode == 1) ? 1.00f : 0.65f;
    const float mode_col_scale    = (tc_mode == 1) ? 1.00f : 0.60f;
    const float mode_trails_scale = (tc_mode == 1) ? 1.00f : 0.55f;
    const float mode_soft_focus   = (tc_mode == 1) ? 1.00f : 0.20f;

    // Detect camera cuts / teleports (>15m jump in a single frame) to reset frame-history trails cleanly
    bool reset_history = m_force_reset_history;
    m_force_reset_history = false;
    if (m_the_camera_addr) {
        const float cx = *reinterpret_cast<const float*>(m_the_camera_addr + 0x30ULL);
        const float cy = *reinterpret_cast<const float*>(m_the_camera_addr + 0x34ULL);
        const float cz = *reinterpret_cast<const float*>(m_the_camera_addr + 0x38ULL);
        if (m_has_prev_cam_pos) {
            const float dx = cx - m_prev_cam_pos[0];
            const float dy = cy - m_prev_cam_pos[1];
            const float dz = cz - m_prev_cam_pos[2];
            if ((dx * dx + dy * dy + dz * dz) > (15.0f * 15.0f)) {
                reset_history = true;
            }
        } else {
            reset_history = true;
        }
        m_prev_cam_pos[0] = cx;
        m_prev_cam_pos[1] = cy;
        m_prev_cam_pos[2] = cz;
        m_has_prev_cam_pos = true;
    }

    // -------------------------------------------------------------------------
    // Exact replication of PS2 CMBlur::GetFinalColour() (SLUS_214.23 @ 0x001948c0 / ARM64 @ 0x00485f68)
    // -------------------------------------------------------------------------
    const int32_t blur_alpha = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x30ULL);
    const uint8_t min_blur_amt = m_min_blur_amt_addr ? *reinterpret_cast<const uint8_t*>(m_min_blur_amt_addr) : kPs2MinBlurAmt;
    const uint8_t min_blur_on = m_min_blur_on_addr ? *reinterpret_cast<const uint8_t*>(m_min_blur_on_addr) : 1u;

    int32_t final_a = blur_alpha;
    if (min_blur_on != 0 && final_a < static_cast<int32_t>(min_blur_amt)) {
        final_a = static_cast<int32_t>(min_blur_amt);
    }
    if (final_a < 0) final_a = 0;

    int32_t final_r = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x24ULL); // m_nDefaultRed
    int32_t final_g = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x28ULL); // m_nDefaultGreen
    int32_t final_b = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x2cULL); // m_nDefaultBlue
    if (final_r < 0) {
        final_r = 255;
        final_g = 255;
        final_b = 255;
    }

    const int32_t override_r = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x00ULL); // m_nRed
    if (override_r >= 0) {
        final_r = override_r;
        final_g = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x04ULL);
        final_b = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x08ULL);
    }

    const int32_t blur_type = *reinterpret_cast<const int32_t*>(cmblur_ptr + 0x34ULL); // m_nBlurType
    const uint32_t bt_idx = static_cast<uint32_t>(blur_type - 1);
    bool is_special_blur_type = false;
    if (bt_idx <= 11u && ((0x9b5u >> bt_idx) & 1u) != 0u) {
        is_special_blur_type = true;
        final_r = *reinterpret_cast<const int32_t*>(m_load_bias + kRvaBlurTypeRedTbl + bt_idx * 4ULL);
        final_g = *reinterpret_cast<const int32_t*>(m_load_bias + kRvaBlurTypeGreenTbl + bt_idx * 4ULL);
        final_b = *reinterpret_cast<const int32_t*>(m_load_bias + kRvaBlurTypeBlueTbl + bt_idx * 4ULL);
        final_a = *reinterpret_cast<const int32_t*>(m_load_bias + kRvaBlurTypeAlphaTbl + bt_idx * 4ULL);
    }

    // Attenuate FinalColour toward (255, 255, 255, 0) during screen fades (CDraw::FadeValue)
    const uint8_t fade_val = m_fade_val_addr ? *reinterpret_cast<const uint8_t*>(m_fade_val_addr) : 0u;
    if (fade_val != 0u) {
        const uint8_t fade_r = m_fade_red_addr ? *reinterpret_cast<const uint8_t*>(m_fade_red_addr) : 0u;
        const uint8_t fade_g = m_fade_green_addr ? *reinterpret_cast<const uint8_t*>(m_fade_green_addr) : 0u;
        const uint8_t fade_b = m_fade_blue_addr ? *reinterpret_cast<const uint8_t*>(m_fade_blue_addr) : 0u;
        if (fade_r > 9u || fade_g > 9u || fade_b >= 10u) {
            const float f = static_cast<float>(fade_val) / 255.0f;
            final_r = static_cast<int32_t>(static_cast<float>(final_r) + static_cast<float>(255 - final_r) * f);
            final_g = static_cast<int32_t>(static_cast<float>(final_g) + static_cast<float>(255 - final_g) * f);
            final_b = static_cast<int32_t>(static_cast<float>(final_b) + static_cast<float>(255 - final_b) * f);
            final_a = static_cast<int32_t>(static_cast<float>(final_a) * (1.0f - f));
        }
    }

    final_r = std::clamp(final_r, 0, 255);
    final_g = std::clamp(final_g, 0, 255);
    final_b = std::clamp(final_b, 0, 255);
    final_a = std::clamp(final_a, 0, 255);

    const float blur_r = static_cast<float>(255 - final_r) / 255.0f;
    const float blur_g = static_cast<float>(255 - final_g) / 255.0f;
    const float blur_b = static_cast<float>(255 - final_b) / 255.0f;

    m_last_blur_rgb[0] = blur_r;
    m_last_blur_rgb[1] = blur_g;
    m_last_blur_rgb[2] = blur_b;

    const float drunk_blur = std::clamp(*reinterpret_cast<const float*>(cmblur_ptr + 0x18ULL), 0.0f, 1.0f);

    float speed_blur_factor = 0.0f;
    if (cfg.ps2_speed_blur != 0) {
        const uint64_t veh = get_player_vehicle();
        if (veh != 0) {
            const float vx = *reinterpret_cast<const float*>(veh + 0x78ULL);
            const float vy = *reinterpret_cast<const float*>(veh + 0x7cULL);
            const float vz = *reinterpret_cast<const float*>(veh + 0x80ULL);
            const float speed_mps = std::sqrt(vx * vx + vy * vy + vz * vz) * 50.0f;
            if (speed_mps > 16.0f) {
                speed_blur_factor = std::clamp((speed_mps - 16.0f) / 30.0f, 0.0f, 1.0f);
            }
        }
    }

    float trails_persistence = 0.0f;
    if (cfg.ps2_trails != 0 || drunk_blur > 0.001f || is_special_blur_type) {
        const float user_trails_scale =
            (cfg.ps2_trails != 0)
                ? ((std::clamp(static_cast<float>(cfg.ps2_trails_intensity), 0.0f, 200.0f) / 100.0f) * mode_trails_scale)
                : 0.0f;
        float p_30fps = (static_cast<float>(final_a) / 255.0f) * user_trails_scale;
        if (speed_blur_factor > 0.0f && cfg.ps2_trails != 0) {
            p_30fps = std::max(p_30fps, (0.14f + 0.16f * speed_blur_factor) * user_trails_scale);
        }
        if (drunk_blur > 0.001f) {
            p_30fps = std::max(p_30fps, drunk_blur * (175.0f / 255.0f));
        }
        p_30fps = std::clamp(p_30fps, 0.0f, 0.88f);
        if (p_30fps > 0.001f) {
            const float frame_exponent = std::clamp(30.0f * m_dt_sec, 0.10f, 2.5f);
            trails_persistence = std::clamp(std::pow(p_30fps, frame_exponent), 0.0f, 0.90f);
            const float max_frame_trail = (is_special_blur_type || drunk_blur > 0.001f)
                ? 0.65f
                : ((0.12f + 0.24f * speed_blur_factor) * user_trails_scale);
            trails_persistence = std::min(trails_persistence, max_frame_trail);
        }
    }
    m_last_trails_alpha = trails_persistence;

    lcs::graphics::Ps2PostProcessParams pp{};
    pp.enabled = true;
    pp.reset_history = reset_history;
    pp.blur_rgb[0] = blur_r;
    pp.blur_rgb[1] = blur_g;
    pp.blur_rgb[2] = blur_b;
    pp.final_color_rgb[0] = static_cast<float>(final_r) / 255.0f;
    pp.final_color_rgb[1] = static_cast<float>(final_g) / 255.0f;
    pp.final_color_rgb[2] = static_cast<float>(final_b) / 255.0f;

    if (is_special_blur_type || override_r >= 0) {
        pp.trail_tint_rgb[0] = std::min(1.0f, pp.final_color_rgb[0] * 2.0f);
        pp.trail_tint_rgb[1] = std::min(1.0f, pp.final_color_rgb[1] * 2.0f);
        pp.trail_tint_rgb[2] = std::min(1.0f, pp.final_color_rgb[2] * 2.0f);
    } else {
        pp.trail_tint_rgb[0] = 1.0f;
        pp.trail_tint_rgb[1] = 1.0f;
        pp.trail_tint_rgb[2] = 1.0f;
    }

    pp.radiosity_intensity = (cfg.ps2_radiosity != 0)
        ? ((std::clamp(static_cast<float>(cfg.ps2_radiosity_intensity), 0.0f, 200.0f) / 100.0f) * mode_rad_scale)
        : 0.0f;
    pp.color_filter_intensity = (cfg.ps2_color_filter != 0)
        ? ((std::clamp(static_cast<float>(cfg.ps2_color_filter_intensity), 0.0f, 200.0f) / 100.0f) * mode_col_scale)
        : 0.0f;
    pp.trails_persistence = trails_persistence;
    pp.speed_blur_factor = speed_blur_factor;
    pp.drunk_blur_factor = drunk_blur;
    pp.anim_phase = m_anim_time;
    pp.soft_focus_weight = mode_soft_focus;

    // Stage parameters for Display::ClearDepthOnly (0x00523fc0), which runs right AFTER
    // Services::SetBackBuffer (0x00523f00) and Display::RenderPassThrough (0x00523fa0)
    // and right BEFORE Render2dStuff (0x00523fc4).
    m_staged_params = pp;
    m_has_staged_params = true;
    return 0;
}

uint64_t LCSPs2Effects::on_pre_hud_flush() {
    if (m_blur_on_addr) {
        *reinterpret_cast<uint8_t*>(m_blur_on_addr) = 1u;
    }
    if (!m_has_staged_params) {
        return 0;
    }
    m_has_staged_params = false;
    lcs::graphics::RenderBridge::instance().queue_ps2_post_process(m_staged_params);
    ++m_render_pass_count;
    return 1;
}

bool LCSPs2Effects::install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    if (m_installed) return true;

    m_load_bias = image_game.get_load_bias();
    m_cpu = &cpu;

    m_p_timecycle_addr    = m_load_bias + kRvaPTimeCycle;
    m_the_camera_addr     = m_load_bias + kRvaTheCamera;
    m_blur_on_addr        = m_load_bias + kRvaCMBlurBlurOn;
    m_min_blur_on_addr    = m_load_bias + kRvaGbMinBlurOn;
    m_min_blur_amt_addr   = m_load_bias + kRvaGMinBlurAmt;
    m_fade_red_addr       = m_load_bias + kRvaCDrawFadeRed;
    m_fade_green_addr     = m_load_bias + kRvaCDrawFadeGreen;
    m_fade_blue_addr      = m_load_bias + kRvaCDrawFadeBlue;
    m_fade_val_addr       = m_load_bias + kRvaCDrawFadeValue;
    m_fn_tc_force_update  = m_load_bias + kRvaCTimeCycleForceUpd;
    m_fn_clear_depth_only = m_load_bias + kRvaDisplayClearDepth;
    m_fn_lgl_gl_flush     = m_load_bias + kRvaLgGlApiGlFlush;
    m_game_state_addr     = image_game.find_symbol("gGameState");
    m_player_idx_addr     = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotPlayerIdx);
    m_players_arr_addr    = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotPlayersArr);

    void* stub_page = nullptr;
#if defined(_WIN32)
    stub_page = VirtualAlloc(reinterpret_cast<void*>(0x7f00060000ULL), 0x1000,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub_page) {
        stub_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!stub_page) {
        std::cerr << "  [WARN] LCSPs2Effects failed to allocate guest stub page.\n";
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(stub_page);
    const uint64_t base_addr = reinterpret_cast<uint64_t>(stub_page);
    std::memset(base, 0, 0x1000);

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t pre_cb_va = hle.register_function("__ps2_mblur_pre",
        [this](const uint64_t* args) -> uint64_t {
            return on_mblur_pre(args[0], args[1]);
        });
    const uint64_t post_cb_va = hle.register_function("__ps2_mblur_post",
        [this](const uint64_t* args) -> uint64_t {
            return on_mblur_post(args[0], args[1]);
        });
    const uint64_t pre_hud_cb_va = hle.register_function("__ps2_pre_hud_flush",
        [this](const uint64_t* /*args*/) -> uint64_t {
            return on_pre_hud_flush();
        });

    // Stub 1 (at base + 0x00) for CMBlur::MotionBlurRender (GOT 0x00801e88, called at 0x00523ec4 in DrawGame):
    //   1. Calls __ps2_mblur_pre(this, cam).
    //   2. If __ps2_mblur_pre returns non-zero (tc_ptr), invokes CTimeCycle::ForceUpdate(tc_ptr) natively in AArch64.
    //   3. Calls __ps2_mblur_post(this, cam), which stages Ps2PostProcessParams for this frame.
    //   (Does NOT call glFlush here because gpSceneTextureB is still bound and hasn't been blitted to FBO 0 yet!)
    auto* code1 = reinterpret_cast<uint32_t*>(base);
    const uint32_t stub1_insns[20] = {
        0xd10083ffu, // 0x00: sub sp, sp, #0x20
        0xa9007bfdu, // 0x04: stp x29, x30, [sp]
        0x910003fdu, // 0x08: mov x29, sp
        0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
        0xaa0003f3u, // 0x10: mov x19, x0
        0xaa0103f4u, // 0x14: mov x20, x1
        ldr_x16_rel(0x18, 0x50),
        0xd63f0200u, // 0x1c: blr x16 (__ps2_mblur_pre)
        0xb4000060u, // 0x20: cbz x0, +0xc (0x2c)
        ldr_x16_rel(0x24, 0x58),
        0xd63f0200u, // 0x28: blr x16 (CTimeCycle::ForceUpdate)
        0xaa1303e0u, // 0x2c: mov x0, x19
        0xaa1403e1u, // 0x30: mov x1, x20
        ldr_x16_rel(0x34, 0x60),
        0xd63f0200u, // 0x38: blr x16 (__ps2_mblur_post)
        0xa94153f3u, // 0x3c: ldp x19, x20, [sp, #0x10]
        0xa9407bfdu, // 0x40: ldp x29, x30, [sp]
        0x910083ffu, // 0x44: add sp, sp, #0x20
        0xd65f03c0u, // 0x48: ret
        0xd503201fu, // 0x4c: nop
    };
    std::memcpy(code1, stub1_insns, sizeof(stub1_insns));
    auto* lits1 = reinterpret_cast<uint64_t*>(base + 0x50);
    lits1[0] = pre_cb_va;
    lits1[1] = m_fn_tc_force_update;
    lits1[2] = post_cb_va;

    // Stub 2 (at base + 0x80) for Display::ClearDepthOnly(float, float) (GOT 0x00807c70, called at 0x00523fc0 in DrawGame):
    //   Called right AFTER Services::SetBackBuffer() (0x00523f00) and Display::RenderPassThrough(gpSceneTextureB) (0x00523fa0),
    //   and immediately BEFORE Render2dStuff() (0x00523fc4)!
    //   1. Calls original Display::ClearDepthOnly(s0, s1) (0x0059945c).
    //   2. Calls __ps2_pre_hud_flush(), which queues m_staged_params into RenderBridge.
    //   3. If __ps2_pre_hud_flush() returns 1, calls LgGlAPI::glFlush() (0x005a7d40) to insert opcode 0x1b (lglFlush)
    //      into gRenderQueue at the exact 3D -> 2D boundary on FBO 0!
    const uint64_t stub2_addr = base_addr + 0x80ULL;
    auto* code2 = reinterpret_cast<uint32_t*>(base + 0x80);
    const uint32_t stub2_insns[14] = {
        0xd10043ffu, // 0x80: sub sp, sp, #0x10
        0xa9007bfdu, // 0x84: stp x29, x30, [sp]
        0x910003fdu, // 0x88: mov x29, sp
        ldr_x16_rel(0x8c, 0xb8),
        0xd63f0200u, // 0x90: blr x16 (Display::ClearDepthOnly)
        ldr_x16_rel(0x94, 0xc0),
        0xd63f0200u, // 0x98: blr x16 (__ps2_pre_hud_flush)
        0xb4000060u, // 0x9c: cbz x0, +0xc (0xa8)
        ldr_x16_rel(0xa0, 0xc8),
        0xd63f0200u, // 0xa4: blr x16 (LgGlAPI::glFlush)
        0xa9407bfdu, // 0xa8: ldp x29, x30, [sp]
        0x910043ffu, // 0xac: add sp, sp, #0x10
        0xd65f03c0u, // 0xb0: ret
        0xd503201fu, // 0xb4: nop
    };
    std::memcpy(code2, stub2_insns, sizeof(stub2_insns));
    auto* lits2 = reinterpret_cast<uint64_t*>(base + 0xb8);
    lits2[0] = m_fn_clear_depth_only;
    lits2[1] = pre_hud_cb_va;
    lits2[2] = m_fn_lgl_gl_flush;

    auto patch_got = [&](uint64_t got_rva, uint64_t target_va) {
        const uint64_t slot_va = m_load_bias + got_rva;
#if defined(_WIN32)
        DWORD old_prot = 0;
        VirtualProtect(reinterpret_cast<void*>(slot_va), sizeof(uint64_t), PAGE_EXECUTE_READWRITE, &old_prot);
#endif
        std::memcpy(reinterpret_cast<void*>(slot_va), &target_va, sizeof(uint64_t));
#if defined(_WIN32)
        VirtualProtect(reinterpret_cast<void*>(slot_va), sizeof(uint64_t), old_prot, &old_prot);
#endif
        cpu.invalidate_cache(slot_va, sizeof(uint64_t));
    };

    patch_got(kGotMotionBlurRender, base_addr);
    patch_got(kGotDisplayClearDepth, stub2_addr);
    cpu.invalidate_cache(base_addr, 0x1000);

    m_installed = true;
    std::cout << "  - Installed Authentic PS2 Graphical Effects Pipeline (TimeCycle + Radiosity + BlurRGB Color Filter + Trails).\n";
    return true;
}

} // namespace lcs::game
