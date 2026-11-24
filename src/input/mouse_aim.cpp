#include "mouse_aim.h"

#include <algorithm>
#include <cmath>

namespace lcs::input {

namespace {

// libGame.so CPad::setJoyAxis filters |axis| <= 0.125f; keep non-zero moves past the deadzone.
constexpr float kLibGameDeadzoneMinOut = 0.128f;
constexpr float kRefFrameDt = 1.0f / 60.0f;
constexpr float kMouseTurnGain = 0.012f;

float map_mouse_delta(int delta, float sensitivity, float dt_sec, bool invert) {
    if (delta == 0) return 0.0f;
    float sign = (delta > 0) ? 1.0f : -1.0f;
    if (invert) sign = -sign;
    const float sens = std::clamp(sensitivity, 0.05f, 10.0f);
    const float clamped_dt = std::clamp(dt_sec, 1.0f / 400.0f, 0.25f);
    // libGame.so integrates camera rotation quadratically (angle ~ stick^2 * CTimer::ms_fTimeStep).
    // Converting raw delta into normalized pixel velocity and taking sqrt keeps mouse displacement
    // linear and sensitivity invariant across low and high framerates (10..240 FPS).
    const float norm_rate = std::abs(static_cast<float>(delta)) * (kRefFrameDt / clamped_dt);
    const float mag = std::max(kLibGameDeadzoneMinOut, std::sqrt(norm_rate * (kMouseTurnGain * sens)));
    return sign * std::min(1.0f, mag);
}

} // namespace

MouseAim& MouseAim::instance() {
    static MouseAim s_instance;
    return s_instance;
}

void MouseAim::add_delta(int dx, int dy) {
    m_accum_dx += dx;
    m_accum_dy += dy;
}

void MouseAim::reset() {
    m_accum_dx = 0;
    m_accum_dy = 0;
}

bool MouseAim::consume_axes(bool in_menu, bool is_aiming, float dt_sec, float& out_rx, float& out_ry) {
    const int dx = m_accum_dx;
    const int dy = m_accum_dy;
    m_accum_dx = 0;
    m_accum_dy = 0;

    if (!in_menu && (dx != 0 || dy != 0)) {
        const float active_sens = is_aiming ? m_config.aim_sensitivity : m_config.sensitivity;
        out_rx = map_mouse_delta(dx, active_sens, dt_sec, false);
        out_ry = map_mouse_delta(dy, active_sens, dt_sec, m_config.invert_y);
        m_last_nonzero = true;
        return true;
    }
    if (m_last_nonzero) {
        out_rx = 0.0f;
        out_ry = 0.0f;
        m_last_nonzero = false;
        return true;
    }
    return false;
}

void MouseAim::consume_look_radians(bool in_menu, bool use_aim_sens, float& out_turn_rad, float& out_tilt_rad) {
    const int dx = m_accum_dx;
    const int dy = m_accum_dy;
    m_accum_dx = 0;
    m_accum_dy = 0;
    m_last_nonzero = false;

    if (in_menu || (dx == 0 && dy == 0)) {
        out_turn_rad = 0.0f;
        out_tilt_rad = 0.0f;
        return;
    }

    constexpr float kLookRadPerCount = 0.0025f;
    const float sens = std::clamp(use_aim_sens ? m_config.aim_sensitivity : m_config.sensitivity, 0.05f, 10.0f);
    out_turn_rad = -static_cast<float>(dx) * (kLookRadPerCount * sens);
    float tilt = -static_cast<float>(dy) * (kLookRadPerCount * sens);
    if (m_config.invert_y) {
        tilt = -tilt;
    }
    out_tilt_rad = tilt;
}

void MouseAim::consume_free_aim_radians(bool in_menu, float& out_turn_rad, float& out_tilt_rad) {
    consume_look_radians(in_menu, true, out_turn_rad, out_tilt_rad);
}

} // namespace lcs::input
