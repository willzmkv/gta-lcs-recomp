#pragma once

namespace lcs::input {

struct MouseAimConfig {
    float sensitivity = 1.0f;
    float aim_sensitivity = 0.8f;
    bool invert_y = false;
};

// Translates raw desktop mouse deltas into normalized right-stick camera/aim axes.
class MouseAim {
public:
    static MouseAim& instance();

    MouseAimConfig& config() { return m_config; }
    const MouseAimConfig& config() const { return m_config; }

    void add_delta(int dx, int dy);
    void reset();

    // Consumes accumulated mouse movement scaled by frame dt; returns true when joy-axis updates should be queued.
    bool consume_axes(bool in_menu, bool is_aiming, float dt_sec, float& out_rx, float& out_ry);

    // Consumes raw mouse movement directly as camera yaw (left+) / pitch (up+) radians (1:1 framerate-independent).
    void consume_look_radians(bool in_menu, bool use_aim_sens, float& out_turn_rad, float& out_tilt_rad);

    // Convenience alias for Free Aim (always uses AimSensitivity).
    void consume_free_aim_radians(bool in_menu, float& out_turn_rad, float& out_tilt_rad);

private:
    MouseAim() = default;

    MouseAimConfig m_config;
    int m_accum_dx = 0;
    int m_accum_dy = 0;
    bool m_last_nonzero = false;
};

} // namespace lcs::input
