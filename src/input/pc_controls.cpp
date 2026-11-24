#include "pc_controls.h"
#include "mouse_aim.h"
#include "input_system.h"
#include "game/lcs_prompts.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::input {

namespace {

std::string trim_ws(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

bool parse_bool(const std::string& val, bool fallback) {
    std::string v = val;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
    if (v == "0" || v == "false" || v == "off" || v == "no") return false;
    return fallback;
}

int parse_vk(const std::string& val, int fallback) {
    if (val.empty()) return fallback;
    char* end = nullptr;
    long n = std::strtol(val.c_str(), &end, 10);
    if (end != val.c_str() && *end == '\0') {
        return (n >= 0 && n < 256) ? static_cast<int>(n) : fallback;
    }
    return fallback;
}

void write_default_controls_ini(const std::string& path,
                                const ModernKBOptions& opt,
                                const MouseAimConfig& mouse,
                                const KeybindSet& b) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;

    out << "# GTA: Liberty City Stories - Native PC Controls Configuration\n"
        << "# Virtual Key Codes follow ModernKBControlsSA.ini / Win32 VK conventions (0 = disabled).\n"
        << "# Common codes: 1=LMB, 2=RMB, 4=MMB, 8=Backspace, 9=Tab, 13=Enter, 16=Shift,\n"
        << "#   17=Ctrl, 20=CapsLock, 27=Esc, 32=Space, 37-40=Arrows, 65-90=A-Z, 164=LAlt\n\n"
        << "[Options]\n"
        << "IVControls=" << (opt.modern_kb_controls ? 1 : 0) << "\n"
        << "IVControls_CycleWeapon=" << (opt.cycle_weapon_wheel ? 1 : 0) << "\n"
        << "IVControls_FullDriveby=" << (opt.full_driveby ? 1 : 0) << "\n"
        << "IVControls_Walk=" << (opt.walk_toggle ? 1 : 0) << "\n"
        << "IVControls_CrouchJump=" << (opt.crouch_jump ? 1 : 0) << "\n"
        << "IVControls_CrouchSprint=" << (opt.crouch_sprint ? 1 : 0) << "\n"
        << "IVControls_AimStrafeFix=" << (opt.aim_strafe_fix ? 1 : 0) << "\n"
        << "IVControls_VehicleWepSwitch=" << (opt.vehicle_wep_switch ? 1 : 0) << "\n"
        << "IVControls_IVLookBehind=" << (opt.iv_look_behind ? 1 : 0) << "\n"
        << "FreeAim=" << (opt.free_aim ? 1 : 0) << "\n\n"
        << "[Mouse]\n"
        << "MouseSensitivity=" << mouse.sensitivity << "\n"
        << "AimSensitivity=" << mouse.aim_sensitivity << "\n"
        << "InvertY=" << (mouse.invert_y ? 1 : 0) << "\n\n"
        << "[Keybinds]\n"
        << "MoveForward=" << b.move_forward << "\n"
        << "MoveForwardAlt=" << b.move_forward_alt << "\n"
        << "MoveBackward=" << b.move_backward << "\n"
        << "MoveBackwardAlt=" << b.move_backward_alt << "\n"
        << "MoveLeft=" << b.move_left << "\n"
        << "MoveLeftAlt=" << b.move_left_alt << "\n"
        << "MoveRight=" << b.move_right << "\n"
        << "MoveRightAlt=" << b.move_right_alt << "\n"
        << "Walk=" << b.walk << "\n"
        << "Sprint=" << b.sprint << "\n"
        << "Jump=" << b.jump << "\n"
        << "Crouch=" << b.crouch << "\n"
        << "EnterVehicle=" << b.enter_vehicle << "\n"
        << "EnterVehicleAlt=" << b.enter_vehicle_alt << "\n"
        << "Fire=" << b.fire << "\n"
        << "FireAlt=" << b.fire_alt << "\n"
        << "Aim=" << b.aim << "\n"
        << "PrevWeapon=" << b.prev_weapon << "\n"
        << "NextWeapon=" << b.next_weapon << "\n"
        << "LookLeft=" << b.look_left << "\n"
        << "LookRight=" << b.look_right << "\n"
        << "LookBehind=" << b.look_behind << "\n"
        << "LookBehindAlt=" << b.look_behind_alt << "\n"
        << "CameraMode=" << b.camera_mode << "\n"
        << "RadioNext=" << b.radio_next << "\n"
        << "Horn=" << b.horn << "\n"
        << "HornAlt=" << b.horn_alt << "\n"
        << "SubMission=" << b.sub_mission << "\n"
        << "SubMissionAlt=" << b.sub_mission_alt << "\n"
        << "Pause=" << b.pause << "\n"
        << "MenuUp=" << b.menu_up << "\n"
        << "MenuDn=" << b.menu_down << "\n"
        << "MenuLeft=" << b.menu_left << "\n"
        << "MenuRight=" << b.menu_right << "\n"
        << "MenuEnter=" << b.menu_enter << "\n"
        << "MenuEnterAlt=" << b.menu_enter_alt << "\n"
        << "MenuBack=" << b.menu_back << "\n"
        << "MenuBackAlt=" << b.menu_back_alt << "\n";
}

} // namespace

PCControls& PCControls::instance() {
    static PCControls s_instance;
    return s_instance;
}

void PCControls::load_or_create(const std::string& path) {
    m_profile_path = path;
    auto& mouse = MouseAim::instance().config();
    if (!std::filesystem::exists(path)) {
        write_default_controls_ini(path, m_options, mouse, m_binds);
        std::cout << "  - Generated default PC controls profile: " << path << "\n";
        return;
    }

    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto slash = line.find("//");
        if (slash != std::string::npos) line.resize(slash);
        line = trim_ws(line);
        if (line.empty()) continue;

        if (line.front() == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim_ws(line.substr(0, eq));
        const std::string val = trim_ws(line.substr(eq + 1));

        if (section == "Options") {
            if (key == "IVControls") m_options.modern_kb_controls = parse_bool(val, m_options.modern_kb_controls);
            else if (key == "IVControls_CycleWeapon") m_options.cycle_weapon_wheel = parse_bool(val, m_options.cycle_weapon_wheel);
            else if (key == "IVControls_FullDriveby") m_options.full_driveby = parse_bool(val, m_options.full_driveby);
            else if (key == "IVControls_Walk") m_options.walk_toggle = parse_bool(val, m_options.walk_toggle);
            else if (key == "IVControls_CrouchJump") m_options.crouch_jump = parse_bool(val, m_options.crouch_jump);
            else if (key == "IVControls_CrouchSprint") m_options.crouch_sprint = parse_bool(val, m_options.crouch_sprint);
            else if (key == "IVControls_AimStrafeFix") m_options.aim_strafe_fix = parse_bool(val, m_options.aim_strafe_fix);
            else if (key == "IVControls_VehicleWepSwitch") m_options.vehicle_wep_switch = parse_bool(val, m_options.vehicle_wep_switch);
            else if (key == "IVControls_IVLookBehind") m_options.iv_look_behind = parse_bool(val, m_options.iv_look_behind);
            else if (key == "FreeAim") m_options.free_aim = parse_bool(val, m_options.free_aim);
        } else if (section == "Mouse") {
            if (key == "MouseSensitivity") {
                float s = static_cast<float>(std::atof(val.c_str()));
                if (s > 0.0f) mouse.sensitivity = std::clamp(s, 0.05f, 10.0f);
            } else if (key == "AimSensitivity") {
                float s = static_cast<float>(std::atof(val.c_str()));
                if (s > 0.0f) mouse.aim_sensitivity = std::clamp(s, 0.05f, 10.0f);
            } else if (key == "InvertY") {
                mouse.invert_y = parse_bool(val, mouse.invert_y);
            }
        } else if (section == "Keybinds") {
            if (key == "MoveForward") m_binds.move_forward = parse_vk(val, m_binds.move_forward);
            else if (key == "MoveForwardAlt") m_binds.move_forward_alt = parse_vk(val, m_binds.move_forward_alt);
            else if (key == "MoveBackward") m_binds.move_backward = parse_vk(val, m_binds.move_backward);
            else if (key == "MoveBackwardAlt") m_binds.move_backward_alt = parse_vk(val, m_binds.move_backward_alt);
            else if (key == "MoveLeft") m_binds.move_left = parse_vk(val, m_binds.move_left);
            else if (key == "MoveLeftAlt") m_binds.move_left_alt = parse_vk(val, m_binds.move_left_alt);
            else if (key == "MoveRight") m_binds.move_right = parse_vk(val, m_binds.move_right);
            else if (key == "MoveRightAlt") m_binds.move_right_alt = parse_vk(val, m_binds.move_right_alt);
            else if (key == "Walk") m_binds.walk = parse_vk(val, m_binds.walk);
            else if (key == "Sprint") m_binds.sprint = parse_vk(val, m_binds.sprint);
            else if (key == "Jump") m_binds.jump = parse_vk(val, m_binds.jump);
            else if (key == "Crouch") m_binds.crouch = parse_vk(val, m_binds.crouch);
            else if (key == "EnterVehicle") m_binds.enter_vehicle = parse_vk(val, m_binds.enter_vehicle);
            else if (key == "EnterVehicleAlt") m_binds.enter_vehicle_alt = parse_vk(val, m_binds.enter_vehicle_alt);
            else if (key == "Fire") m_binds.fire = parse_vk(val, m_binds.fire);
            else if (key == "FireAlt") m_binds.fire_alt = parse_vk(val, m_binds.fire_alt);
            else if (key == "Aim") m_binds.aim = parse_vk(val, m_binds.aim);
            else if (key == "PrevWeapon") m_binds.prev_weapon = parse_vk(val, m_binds.prev_weapon);
            else if (key == "NextWeapon") m_binds.next_weapon = parse_vk(val, m_binds.next_weapon);
            else if (key == "LookLeft") m_binds.look_left = parse_vk(val, m_binds.look_left);
            else if (key == "LookRight") m_binds.look_right = parse_vk(val, m_binds.look_right);
            else if (key == "LookBehind") m_binds.look_behind = parse_vk(val, m_binds.look_behind);
            else if (key == "LookBehindAlt") m_binds.look_behind_alt = parse_vk(val, m_binds.look_behind_alt);
            else if (key == "CameraMode") m_binds.camera_mode = parse_vk(val, m_binds.camera_mode);
            else if (key == "RadioNext") m_binds.radio_next = parse_vk(val, m_binds.radio_next);
            else if (key == "Horn") m_binds.horn = parse_vk(val, m_binds.horn);
            else if (key == "HornAlt") m_binds.horn_alt = parse_vk(val, m_binds.horn_alt);
            else if (key == "SubMission") m_binds.sub_mission = parse_vk(val, m_binds.sub_mission);
            else if (key == "SubMissionAlt") m_binds.sub_mission_alt = parse_vk(val, m_binds.sub_mission_alt);
            else if (key == "Pause") m_binds.pause = parse_vk(val, m_binds.pause);
            else if (key == "MenuUp") m_binds.menu_up = parse_vk(val, m_binds.menu_up);
            else if (key == "MenuDn") m_binds.menu_down = parse_vk(val, m_binds.menu_down);
            else if (key == "MenuLeft") m_binds.menu_left = parse_vk(val, m_binds.menu_left);
            else if (key == "MenuRight") m_binds.menu_right = parse_vk(val, m_binds.menu_right);
            else if (key == "MenuEnter") m_binds.menu_enter = parse_vk(val, m_binds.menu_enter);
            else if (key == "MenuEnterAlt") m_binds.menu_enter_alt = parse_vk(val, m_binds.menu_enter_alt);
            else if (key == "MenuBack") m_binds.menu_back = parse_vk(val, m_binds.menu_back);
            else if (key == "MenuBackAlt") m_binds.menu_back_alt = parse_vk(val, m_binds.menu_back_alt);
        }
    }
    std::cout << "  - Loaded PC controls profile: " << path << "\n";
}

void PCControls::save_profile() const {
    if (m_profile_path.empty()) return;
    write_default_controls_ini(m_profile_path, m_options, MouseAim::instance().config(), m_binds);
}

bool PCControls::matches_bind(int vk, int bind_a, int bind_b) const {
    if (vk <= 0 || vk >= 256) return false;
    auto eq = [vk](int bind) {
        if (bind == 0) return false;
        if (vk == bind) return true;
        // Treat generic modifiers (16/17/18) and side-specific codes (160..165) interchangeably.
        if (bind == 16 && (vk == 160 || vk == 161)) return true;
        if (bind == 17 && (vk == 162 || vk == 163)) return true;
        if (bind == 18 && (vk == 164 || vk == 165)) return true;
        if ((bind == 160 || bind == 161) && vk == 16) return true;
        if ((bind == 162 || bind == 163) && vk == 17) return true;
        if ((bind == 164 || bind == 165) && vk == 18) return true;
        return false;
    };
    return eq(bind_a) || eq(bind_b);
}

bool PCControls::is_bind_held(int bind_a, int bind_b) const {
    auto held_single = [this](int bind) {
        if (bind <= 0 || bind >= 256) return false;
        if (m_vk_held[bind]) return true;
        if (bind == 16) return m_vk_held[160] || m_vk_held[161];
        if (bind == 17) return m_vk_held[162] || m_vk_held[163];
        if (bind == 18) return m_vk_held[164] || m_vk_held[165];
        return false;
    };
    return held_single(bind_a) || held_single(bind_b);
}

bool PCControls::is_aiming() const {
    return !m_in_menu && !m_in_vehicle && is_bind_held(m_binds.aim);
}

void PCControls::press_btn(int btn) {
    if (btn >= 0 && btn < 16 && m_btn_refs[btn]++ == 0) {
        InputSystem::instance().queue_joy_button_down(0, btn);
    }
}

void PCControls::release_btn(int btn) {
    if (btn >= 0 && btn < 16 && m_btn_refs[btn] > 0 && --m_btn_refs[btn] == 0) {
        InputSystem::instance().queue_joy_button_up(0, btn);
    }
}

void PCControls::pulse_btn(int btn, int frames) {
    if (btn < 0 || btn >= 16) return;
    if (m_pulse_frames[btn] == 0) {
        press_btn(btn);
    }
    m_pulse_frames[btn] = std::max(m_pulse_frames[btn], frames);
}

void PCControls::apply_slot_mask(uint32_t& slot_mask, uint32_t new_mask) {
    uint32_t added = new_mask & ~slot_mask;
    uint32_t removed = slot_mask & ~new_mask;
    for (int b = 0; b < 16; b++) {
        if (added & (1u << b)) press_btn(b);
        if (removed & (1u << b)) release_btn(b);
    }
    slot_mask = new_mask;
}

uint32_t PCControls::resolve_btn_mask(int vk) const {
    if (vk <= 0 || vk >= 256) return 0u;

    if (matches_bind(vk, m_binds.pause)) {
        // CPad::Update reads Start (+0x22) on button 4 and Back (+0x32) on button 14.
        return m_in_menu ? (1u << 14) : (1u << 4);
    }

    if (m_in_menu) {
        if (matches_bind(vk, m_binds.menu_up)) return (1u << 8);
        if (matches_bind(vk, m_binds.menu_down)) return (1u << 9);
        if (matches_bind(vk, m_binds.menu_left)) return (1u << 10);
        if (matches_bind(vk, m_binds.menu_right)) return (1u << 11);
        if (matches_bind(vk, m_binds.menu_enter, m_binds.menu_enter_alt)) return (1u << 0);
        if (matches_bind(vk, m_binds.menu_back, m_binds.menu_back_alt)) return (1u << 14);
        return 0u;
    }

    if (m_in_cutscene && matches_bind(vk, m_binds.jump, m_binds.enter_vehicle_alt)) {
        return (1u << 0); // Skip Cutscene (Cross)
    }

    uint32_t mask = 0u;
    if (matches_bind(vk, m_binds.enter_vehicle, m_binds.enter_vehicle_alt)) {
        mask |= (1u << 3); // Triangle (Enter/Exit vehicle)
    }
    if (matches_bind(vk, m_binds.camera_mode)) {
        mask |= (1u << 5); // Select (+0x24: Cycle Camera Mode)
    }

    if (m_in_vehicle) {
        if (matches_bind(vk, m_binds.jump)) mask |= (1u << 7);       // Handbrake (R1 = Space ONLY in vehicle)
        if (matches_bind(vk, m_binds.horn, m_binds.horn_alt)) mask |= (1u << 9); // Horn / Siren
        if (matches_bind(vk, m_binds.sub_mission, m_binds.sub_mission_alt)) mask |= (1u << 6); // L1 Sub-mission
        if (matches_bind(vk, m_binds.radio_next)) mask |= (1u << 11); // Next Radio Station
        const bool looking_side = is_bind_held(m_binds.look_left) || is_bind_held(m_binds.look_right);
        if (matches_bind(vk, m_binds.fire_alt) ||
            ((m_options.full_driveby || looking_side) && matches_bind(vk, m_binds.fire))) {
            mask |= (1u << 1); // Circle (Vehicle weapon / drive-by)
        }
    } else {
        if (matches_bind(vk, m_binds.fire, m_binds.fire_alt)) {
            mask |= (1u << 1); // Circle (Attack / Fire)
        }
        if (matches_bind(vk, m_binds.aim)) {
            mask |= (1u << 7); // R1 (Target / Free Aim ONLY on foot, never handbrake in car)
        }
        if (matches_bind(vk, m_binds.jump)) mask |= (1u << 2);        // Jump (Square)
        if (matches_bind(vk, m_binds.sprint)) mask |= (1u << 0);      // Sprint (Cross)
        if (matches_bind(vk, m_binds.crouch)) mask |= (1u << 12);     // Crouch (L3)
        if (matches_bind(vk, m_binds.prev_weapon)) mask |= (1u << 10); // Prev Weapon (DPad Left)
        if (matches_bind(vk, m_binds.next_weapon)) mask |= (1u << 11); // Next Weapon (DPad Right)
    }

    return mask;
}

void PCControls::sync_continuous() {
    const bool fwd   = is_bind_held(m_binds.move_forward, m_binds.move_forward_alt);
    const bool back  = is_bind_held(m_binds.move_backward, m_binds.move_backward_alt);
    const bool left  = is_bind_held(m_binds.move_left, m_binds.move_left_alt);
    const bool right = is_bind_held(m_binds.move_right, m_binds.move_right_alt);

    float sx = 0.0f;
    float sy = 0.0f;
    if (right) sx += 1.0f;
    if (left)  sx -= 1.0f;

    if (!m_in_menu && m_in_vehicle) {
        if (fwd && !m_veh_gas_held) {
            m_veh_gas_held = true;
            press_btn(0);
        } else if (!fwd && m_veh_gas_held) {
            m_veh_gas_held = false;
            release_btn(0);
        }
        if (back && !m_veh_brake_held) {
            m_veh_brake_held = true;
            press_btn(2);
        } else if (!back && m_veh_brake_held) {
            m_veh_brake_held = false;
            release_btn(2);
        }
        sy = 0.0f;
    } else {
        if (m_veh_gas_held) {
            m_veh_gas_held = false;
            release_btn(0);
        }
        if (m_veh_brake_held) {
            m_veh_brake_held = false;
            release_btn(2);
        }
        if (back) sy += 1.0f;
        if (fwd)  sy -= 1.0f;

        if (!m_in_menu && m_options.modern_kb_controls) {
            const bool walking = m_options.walk_toggle
                ? m_walk_toggled
                : is_bind_held(m_binds.walk);
            if (walking && !is_bind_held(m_binds.sprint)) {
                constexpr float kWalkScale = 0.45f;
                sx *= kWalkScale;
                sy *= kWalkScale;
            }
        }
    }

    float l2 = 0.0f;
    float r2 = 0.0f;
    m_kb_look_left = false;
    m_kb_look_right = false;
    m_kb_look_behind = false;
    if (!m_in_menu) {
        const bool look_behind = is_bind_held(m_binds.look_behind, m_binds.look_behind_alt);
        if (m_in_vehicle) {
            const bool look_l = is_bind_held(m_binds.look_left);
            const bool look_r = is_bind_held(m_binds.look_right);
            m_kb_look_left = look_l || (look_behind && m_options.iv_look_behind);
            m_kb_look_right = look_r || (look_behind && m_options.iv_look_behind);
            m_kb_look_behind = (look_behind && m_options.iv_look_behind);
            if (m_kb_look_left) l2 = 1.0f;
            if (m_kb_look_right) r2 = 1.0f;
        } else if (look_behind && m_options.iv_look_behind) {
            m_kb_look_behind = true;
            r2 = 1.0f;
        }
    }
    m_prev_lx = sx;
    m_prev_ly = sy;
    m_prev_l2 = l2;
    m_prev_r2 = r2;
    InputSystem::instance().set_kb_axes(sx, sy, l2, r2);
}

void PCControls::set_context(bool in_menu, bool in_vehicle, bool in_cutscene) {
    const bool menu_changed = (m_in_menu != in_menu);
    const bool veh_changed = (m_in_vehicle != in_vehicle);
    const bool cut_changed = (m_in_cutscene != in_cutscene);
    m_in_menu = in_menu;
    m_in_vehicle = in_vehicle;
    m_in_cutscene = in_cutscene;

    if (menu_changed && m_in_menu) {
        reset_all();
        return;
    }
    if (veh_changed && m_in_vehicle) {
        m_crouching = false;
    }
    if (veh_changed) {
        for (int vk = 1; vk < 256; ++vk) {
            if (m_vk_held[vk]) {
                apply_slot_mask(m_vk_btn_mask[vk], resolve_btn_mask(vk));
            }
        }
    }
    if (menu_changed || veh_changed || cut_changed) {
        sync_continuous();
    }
}

void PCControls::on_key_event(int vk, bool down) {
    if (vk <= 0 || vk >= 256) return;
    if (down) {
        lcs::game::LCSPrompts::instance().notify_keyboard_mouse();
    }
    if (m_vk_held[vk] == down) return;
    m_vk_held[vk] = down;

    if (down && !m_in_menu && !m_in_vehicle && m_options.modern_kb_controls) {
        if (m_options.walk_toggle && matches_bind(vk, m_binds.walk)) {
            m_walk_toggled = !m_walk_toggled;
        }
        if (matches_bind(vk, m_binds.crouch)) {
            m_crouching = !m_crouching;
        } else if (m_crouching &&
                   ((m_options.crouch_jump && matches_bind(vk, m_binds.jump)) ||
                    (m_options.crouch_sprint && matches_bind(vk, m_binds.sprint)))) {
            // Stand out of crouch before jump/sprint executes so player never gets stuck crouched.
            m_crouching = false;
            pulse_btn(12, 2);
        }
    }

    if (down) {
        uint32_t mask = resolve_btn_mask(vk);
        if (mask) apply_slot_mask(m_vk_btn_mask[vk], mask);
    } else if (m_vk_btn_mask[vk]) {
        apply_slot_mask(m_vk_btn_mask[vk], 0u);
    }

    sync_continuous();
}

void PCControls::on_mouse_button(int vk_mouse_btn, bool down) {
    on_key_event(vk_mouse_btn, down);
}

void PCControls::on_mouse_wheel(int wheel_delta) {
    if (m_in_menu || wheel_delta == 0) return;

    const bool allow = m_in_vehicle || m_options.cycle_weapon_wheel;
    if (allow) {
        pulse_btn(wheel_delta > 0 ? 11 : 10, 2);
    }
}

void PCControls::tick_frame() {
    for (int b = 0; b < 16; b++) {
        if (m_pulse_frames[b] > 0 && --m_pulse_frames[b] == 0) {
            release_btn(b);
        }
    }
}

void PCControls::poll_frame(bool focused, bool blocking) {
    if (!focused || blocking) {
        bool any = (m_prev_lx != 0.0f || m_prev_ly != 0.0f || m_prev_l2 != 0.0f || m_prev_r2 != 0.0f ||
                    m_veh_gas_held || m_veh_brake_held || m_kb_look_left || m_kb_look_right || m_kb_look_behind);
        if (!any) {
            for (int vk = 1; vk < 256; ++vk) {
                if (m_vk_held[vk] || m_vk_btn_mask[vk]) { any = true; break; }
            }
        }
        if (any) {
            reset_all();
        } else {
            InputSystem::instance().set_kb_axes(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return;
    }

#if defined(LCS_PLATFORM_WINDOWS)
    // Reconcile held Win32 keys with real physical state so missed WM_KEYUP / WM_SYSKEYUP can never latch A/D/WASD.
    bool released_any = false;
    for (int vk = 1; vk < 255; ++vk) {
        if (!m_vk_held[vk]) continue;
        if ((GetAsyncKeyState(vk) & 0x8000) == 0) {
            m_vk_held[vk] = false;
            if (m_vk_btn_mask[vk]) {
                apply_slot_mask(m_vk_btn_mask[vk], 0u);
            }
            released_any = true;
        }
    }
    if (released_any) {
        sync_continuous();
    }
#endif
}

void PCControls::reset_all() {
    m_vk_held.fill(false);
    for (uint32_t& m : m_vk_btn_mask) {
        if (m) apply_slot_mask(m, 0u);
    }
    for (int b = 0; b < 16; b++) {
        if (m_pulse_frames[b] > 0) {
            m_pulse_frames[b] = 0;
            release_btn(b);
        }
    }
    m_crouching = false;
    sync_continuous();
}

} // namespace lcs::input
