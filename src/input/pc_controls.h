#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace lcs::input {

// Options modeled after ModernKBControlsSA.ini plus desktop mouse settings.
struct ModernKBOptions {
    bool modern_kb_controls = true;
    bool cycle_weapon_wheel = true;
    bool full_driveby = false;
    bool walk_toggle = false;
    bool crouch_jump = true;
    bool crouch_sprint = true;
    bool aim_strafe_fix = true;
    bool vehicle_wep_switch = false;
    bool iv_look_behind = true;
    bool free_aim = true;
};

struct KeybindSet {
    int move_forward = 'W';
    int move_forward_alt = 38;   // VK_UP
    int move_backward = 'S';
    int move_backward_alt = 40;  // VK_DOWN
    int move_left = 'A';
    int move_left_alt = 37;      // VK_LEFT
    int move_right = 'D';
    int move_right_alt = 39;     // VK_RIGHT

    int walk = 164;              // VK_LMENU (Left Alt)
    int sprint = 16;             // VK_SHIFT
    int jump = 32;               // VK_SPACE
    int crouch = 'C';            // L3 (button 12)
    int enter_vehicle = 'F';
    int enter_vehicle_alt = 13;  // VK_RETURN

    int fire = 1;                // Left Mouse
    int fire_alt = 17;           // VK_CONTROL
    int aim = 2;                 // Right Mouse
    int prev_weapon = 'Q';
    int next_weapon = 'E';
    int look_left = 'Q';
    int look_right = 'E';
    int look_behind = 4;         // Middle Mouse
    int look_behind_alt = 'B';

    int camera_mode = 'V';
    int radio_next = 'R';
    int horn = 'H';
    int horn_alt = 20;           // VK_CAPITAL (Caps Lock)
    int sub_mission = 9;         // VK_TAB
    int sub_mission_alt = '2';

    int pause = 27;              // VK_ESCAPE
    int menu_up = 38;            // VK_UP
    int menu_down = 40;          // VK_DOWN
    int menu_left = 37;          // VK_LEFT
    int menu_right = 39;         // VK_RIGHT
    int menu_enter = 13;         // VK_RETURN
    int menu_enter_alt = 32;     // VK_SPACE
    int menu_back = 8;           // VK_BACK
    int menu_back_alt = 2;       // Right Mouse
};

// Manages configurable PC keyboard/mouse bindings and ModernKBControls behavior.
class PCControls {
public:
    static PCControls& instance();

    void load_or_create(const std::string& path);
    void save_profile() const;

    const ModernKBOptions& options() const { return m_options; }
    ModernKBOptions& options_mut() { return m_options; }
    const KeybindSet& keybinds() const { return m_binds; }

    void set_context(bool in_menu, bool in_vehicle, bool in_cutscene);
    bool in_menu() const { return m_in_menu; }
    bool is_aiming() const;

    // Raw desktop key/mouse transitions (vk follows Win32 virtual key codes: 1=LMB, 2=RMB, 4=MMB).
    void on_key_event(int vk, bool down);
    void on_mouse_button(int vk_mouse_btn, bool down);
    void on_mouse_wheel(int wheel_delta);

    // Advances timed button pulses (e.g. mouse wheel) once per frame.
    void tick_frame();
    // Reconciles held keys against real OS key state and zeroes inputs when unfocused or blocked.
    void poll_frame(bool focused, bool blocking);

    bool kb_look_left() const { return m_kb_look_left; }
    bool kb_look_right() const { return m_kb_look_right; }
    bool kb_look_behind() const { return m_kb_look_behind; }

    // Clears all active held keys and buttons (e.g. on focus loss or entering menus).
    void reset_all();

private:
    PCControls() = default;

    bool matches_bind(int vk, int bind_a, int bind_b = 0) const;
    bool is_bind_held(int bind_a, int bind_b = 0) const;
    uint32_t resolve_btn_mask(int vk) const;
    void apply_slot_mask(uint32_t& slot_mask, uint32_t new_mask);
    void press_btn(int btn);
    void release_btn(int btn);
    void pulse_btn(int btn, int frames = 2);
    void sync_continuous();

    ModernKBOptions m_options;
    KeybindSet m_binds;
    std::string m_profile_path = "gtalcs/controls.ini";

    bool m_in_menu = true;
    bool m_in_vehicle = false;
    bool m_in_cutscene = false;

    std::array<bool, 256> m_vk_held{};
    std::array<uint32_t, 256> m_vk_btn_mask{};
    std::array<int, 16> m_btn_refs{};
    std::array<int, 16> m_pulse_frames{};

    bool m_walk_toggled = false;
    bool m_crouching = false;
    bool m_veh_gas_held = false;
    bool m_veh_brake_held = false;
    bool m_kb_look_left = false;
    bool m_kb_look_right = false;
    bool m_kb_look_behind = false;
    float m_prev_lx = 0.0f;
    float m_prev_ly = 0.0f;
    float m_prev_l2 = 0.0f;
    float m_prev_r2 = 0.0f;
};

} // namespace lcs::input
