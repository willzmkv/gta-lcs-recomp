#include "input_system.h"
#include "gamepad.h"
#include "cpu/dynarmic_core.h"
#include "game/lcs_prompts.h"

#include <algorithm>
#include <iostream>
#include <cmath>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD dwUserIndex, XINPUT_STATE* pState);
#endif

namespace lcs::input {

InputSystem& InputSystem::instance() {
    static InputSystem s_instance;
    return s_instance;
}

void InputSystem::init(uint64_t fn_touch_start,
                       uint64_t fn_touch_move,
                       uint64_t fn_touch_end,
                       uint64_t fn_joy_down,
                       uint64_t fn_joy_up,
                       uint64_t fn_joy_axis,
                       uint64_t fn_no_joysticks,
                       uint64_t fn_back_button) {
    m_fn_touch_start = fn_touch_start;
    m_fn_touch_move = fn_touch_move;
    m_fn_touch_end = fn_touch_end;
    m_fn_joy_down = fn_joy_down;
    m_fn_joy_up = fn_joy_up;
    m_fn_joy_axis = fn_joy_axis;
    m_fn_no_joysticks = fn_no_joysticks;
    m_fn_back_button = fn_back_button;
    m_initialized = true;

    GamepadDriver::instance().initialize();

    std::cout << "[Input] InputSystem initialized with native JNI entry points.\n" << std::flush;
}

void InputSystem::queue_touch_down(int id, float norm_x, float norm_y) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::TouchDown, id, 0, norm_x, norm_y, 0.0f});
}

void InputSystem::queue_touch_up(int id, float norm_x, float norm_y) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::TouchUp, id, 0, norm_x, norm_y, 0.0f});
}

void InputSystem::queue_touch_move(int id, float norm_x, float norm_y) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::TouchMove, id, 0, norm_x, norm_y, 0.0f});
}

void InputSystem::queue_joy_button_down(int joy_id, int button) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (joy_id == 0 && button >= 0 && button < 16) m_joy0_buttons[button] = true;
    m_queue.push_back({EventType::JoyButtonDown, joy_id, button, 0.0f, 0.0f, 0.0f});
}

void InputSystem::queue_joy_button_up(int joy_id, int button) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (joy_id == 0 && button >= 0 && button < 16) m_joy0_buttons[button] = false;
    m_queue.push_back({EventType::JoyButtonUp, joy_id, button, 0.0f, 0.0f, 0.0f});
}

void InputSystem::queue_joy_axis(int joy_id, int axis, float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (joy_id == 0 && axis >= 0 && axis < 6) {
        m_pad_axes[axis] = value;
    }
}

void InputSystem::set_kb_axes(float lx, float ly, float l2, float r2) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_kb_axes[0] = lx;
    m_kb_axes[1] = ly;
    m_kb_axes[4] = l2;
    m_kb_axes[5] = r2;
}

void InputSystem::clear_pad_state() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int a = 0; a < 6; ++a) {
        m_pad_axes[a] = 0.0f;
    }
}

void InputSystem::release_all_guest_inputs() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int a = 0; a < 6; ++a) {
        m_kb_axes[a] = 0.0f;
    }
    // Preserve only release events already queued and emit button-up for any held joy button.
    std::vector<RawInputEvent> filtered;
    filtered.reserve(m_queue.size() + 16);
    for (const auto& ev : m_queue) {
        if (ev.type == EventType::TouchUp || ev.type == EventType::JoyButtonUp) {
            filtered.push_back(ev);
        }
    }
    for (int b = 0; b < 16; ++b) {
        if (m_joy0_buttons[b]) {
            m_joy0_buttons[b] = false;
            filtered.push_back({EventType::JoyButtonUp, 0, b, 0.0f, 0.0f, 0.0f});
        }
    }
    m_queue.swap(filtered);
}

bool InputSystem::is_joy_button_held(int joy_id, int button) const {
    if (joy_id != 0 || button < 0 || button >= 16) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_joy0_buttons[button];
}

float InputSystem::get_joy_axis(int joy_id, int axis) const {
    if (joy_id != 0 || axis < 0 || axis >= 6) return 0.0f;
    std::lock_guard<std::mutex> lock(m_mutex);
    if ((axis < 2 || axis >= 4) && std::abs(m_kb_axes[axis]) > 0.001f) {
        return m_kb_axes[axis];
    }
    return m_pad_axes[axis];
}

float InputSystem::get_pad_axis(int joy_id, int axis) const {
    if (joy_id != 0 || axis < 0 || axis >= 6) return 0.0f;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pad_axes[axis];
}

void InputSystem::queue_back_button() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::BackButton, 0, 0, 0.0f, 0.0f, 0.0f});
}

void InputSystem::poll_gamepad() {
    GamepadDriver::instance().poll();
    if (GamepadDriver::instance().is_connected()) {
        return;
    }

#if defined(LCS_PLATFORM_WINDOWS)
    static HMODULE s_xinput = nullptr;
    static XInputGetStateFn s_get_state = nullptr;
    static bool s_loaded = false;

    if (!s_loaded) {
        s_xinput = LoadLibraryA("xinput1_4.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput9_1_0.dll");
        if (!s_xinput) s_xinput = LoadLibraryA("xinput1_3.dll");
        if (s_xinput) {
            s_get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(s_xinput, "XInputGetState"));
        }
        s_loaded = true;
    }

    if (!s_get_state) return;

    XINPUT_STATE state{};
    DWORD dw = s_get_state(0, &state);
    if (dw != ERROR_SUCCESS) {
        if (m_prev_xinput_buttons != 0) {
            for (int b = 0; b < 16; ++b) {
                if (is_joy_button_held(0, b)) queue_joy_button_up(0, b);
            }
            m_prev_xinput_buttons = 0;
        }
        for (int a = 0; a < 6; ++a) {
            if (m_prev_axes[a] != 0.0f) {
                m_prev_axes[a] = 0.0f;
                queue_joy_axis(0, a, 0.0f);
            }
        }
        return;
    }

    uint32_t btns = state.Gamepad.wButtons;

    // Mapping table verified against libGame.so CPad::Update (0x3448f4..0x344abc):
    // 0: Cross/A, 1: Circle/B, 2: Square/X, 3: Triangle/Y, 4: Start, 5: Select/Back,
    // 6: L1, 7: R1, 8: DPadUp, 9: DPadDown, 10: DPadLeft, 11: DPadRight, 12: L3, 13: R3
    struct ButtonMap {
        WORD xinput_bit;
        int war_button;
    };
    static const ButtonMap kMaps[] = {
        {XINPUT_GAMEPAD_A,              0},
        {XINPUT_GAMEPAD_B,              1},
        {XINPUT_GAMEPAD_X,              2},
        {XINPUT_GAMEPAD_Y,              3},
        {XINPUT_GAMEPAD_BACK,           5},
        {XINPUT_GAMEPAD_START,          4},
        {XINPUT_GAMEPAD_LEFT_SHOULDER,  6},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, 7},
        {XINPUT_GAMEPAD_DPAD_UP,        8},
        {XINPUT_GAMEPAD_DPAD_DOWN,      9},
        {XINPUT_GAMEPAD_DPAD_LEFT,      10},
        {XINPUT_GAMEPAD_DPAD_RIGHT,     11},
        {XINPUT_GAMEPAD_LEFT_THUMB,     12},
        {XINPUT_GAMEPAD_RIGHT_THUMB,    13},
    };

    bool any_xinput = false;
    for (const auto& bm : kMaps) {
        bool now = (btns & bm.xinput_bit) != 0;
        bool prev = (m_prev_xinput_buttons & bm.xinput_bit) != 0;
        if (now && !prev) {
            queue_joy_button_down(0, bm.war_button);
            any_xinput = true;
        } else if (!now && prev) {
            queue_joy_button_up(0, bm.war_button);
        }
    }
    m_prev_xinput_buttons = btns;

    // Sticks with smooth scaled radial deadzone (prevents cardinal snapping)
    auto norm_thumb = [](SHORT val) -> float {
        return std::clamp(static_cast<float>(val) / 32767.0f, -1.0f, 1.0f);
    };
    auto apply_radial_dz = [](float& x, float& y, float deadzone) {
        const float mag = std::hypot(x, y);
        if (mag <= deadzone || mag < 1e-6f) {
            x = 0.0f;
            y = 0.0f;
            return;
        }
        const float scaled = std::clamp((mag - deadzone) / (1.0f - deadzone), 0.0f, 1.0f);
        const float k = scaled / mag;
        x *= k;
        y *= k;
    };

    float lx = norm_thumb(state.Gamepad.sThumbLX);
    float ly = -norm_thumb(state.Gamepad.sThumbLY);
    float rx = norm_thumb(state.Gamepad.sThumbRX);
    float ry = -norm_thumb(state.Gamepad.sThumbRY);
    apply_radial_dz(lx, ly, 0.14f);
    apply_radial_dz(rx, ry, 0.14f);
    float lt = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
    float rt = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
    if (lt < 0.04f) lt = 0.0f;
    if (rt < 0.04f) rt = 0.0f;

    float curr_axes[6] = {lx, ly, rx, ry, lt, rt};
    for (int a = 0; a < 6; a++) {
        if (std::abs(curr_axes[a] - m_prev_axes[a]) > 0.01f) {
            if (std::abs(curr_axes[a]) > 0.25f) any_xinput = true;
            m_prev_axes[a] = curr_axes[a];
            queue_joy_axis(0, a, curr_axes[a]);
        }
    }
    if (any_xinput) {
        lcs::game::LCSPrompts::instance().notify_gamepad();
    }
#endif
}

void InputSystem::dispatch_events(lcs::cpu::DynarmicCore& cpu, uint64_t env_addr, uint64_t dummy_clazz) {
    if (!m_initialized) return;

    if (!m_joysticks_enabled && m_fn_no_joysticks) {
        // setNoJoysticks(1) sets GetJoypadsConnected()=1 -> WarGamepad_GetGamepadType()=0 -> OS_GamepadIsConnected()=true
        cpu.call(m_fn_no_joysticks, {env_addr, dummy_clazz, 1});
        m_joysticks_enabled = true;
    }

    float want_axes[6] = {};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_block_guest_presses) {
            want_axes[0] = (std::abs(m_kb_axes[0]) > 0.001f) ? m_kb_axes[0] : m_pad_axes[0];
            want_axes[1] = (std::abs(m_kb_axes[1]) > 0.001f) ? m_kb_axes[1] : m_pad_axes[1];
            want_axes[2] = m_suppress_right_stick ? 0.0f : m_pad_axes[2];
            want_axes[3] = m_suppress_right_stick ? 0.0f : m_pad_axes[3];
            // Only forward controller triggers to guest Look axes (4, 5) in Classic PS2 mode.
            // In modern schemes (Xbox / PS5 / Switch), only keyboard look (Q/E/MMB) writes axes 4/5.
            want_axes[4] = m_classic_ps2_triggers ? std::max(m_kb_axes[4], m_pad_axes[4]) : m_kb_axes[4];
            want_axes[5] = m_classic_ps2_triggers ? std::max(m_kb_axes[5], m_pad_axes[5]) : m_kb_axes[5];
        }
    }
    if (m_fn_joy_axis) {
        for (int a = 0; a < 6; ++a) {
            if (std::abs(want_axes[a] - m_sent_guest_axes[a]) > 1e-4f) {
                m_sent_guest_axes[a] = want_axes[a];
                cpu.set_fp_float(0, want_axes[a]);
                cpu.call(m_fn_joy_axis, {env_addr, dummy_clazz, 0, static_cast<uint64_t>(a)});
            }
        }
    }

    std::vector<RawInputEvent> pending;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return;
        pending.swap(m_queue);
    }

    for (const auto& ev : pending) {
        switch (ev.type) {
            case EventType::TouchDown:
                if (!m_block_guest_presses && m_fn_touch_start) {
                    cpu.set_fp_float(0, ev.x);
                    cpu.set_fp_float(1, ev.y);
                    cpu.call(m_fn_touch_start, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id)});
                }
                break;
            case EventType::TouchMove:
                if (!m_block_guest_presses && m_fn_touch_move) {
                    cpu.set_fp_float(0, ev.x);
                    cpu.set_fp_float(1, ev.y);
                    cpu.call(m_fn_touch_move, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id)});
                }
                break;
            case EventType::TouchUp:
                if (m_fn_touch_end) {
                    cpu.set_fp_float(0, ev.x);
                    cpu.set_fp_float(1, ev.y);
                    cpu.call(m_fn_touch_end, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id)});
                }
                break;
            case EventType::JoyButtonDown:
                if (!m_block_guest_presses && m_fn_joy_down) {
                    cpu.call(m_fn_joy_down, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id), static_cast<uint64_t>(ev.index)});
                }
                break;
            case EventType::JoyButtonUp:
                if (m_fn_joy_up) {
                    cpu.call(m_fn_joy_up, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id), static_cast<uint64_t>(ev.index)});
                }
                break;
            case EventType::JoyAxis:
                if (m_fn_joy_axis) {
                    if (m_suppress_right_stick && ev.id == 0 && (ev.index == 2 || ev.index == 3)) {
                        break;
                    }
                    const float v = m_block_guest_presses ? 0.0f : ev.value;
                    cpu.set_fp_float(0, v);
                    cpu.call(m_fn_joy_axis, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id), static_cast<uint64_t>(ev.index)});
                }
                break;
            case EventType::BackButton:
                if (!m_block_guest_presses && m_fn_back_button) {
                    cpu.call(m_fn_back_button, {env_addr, dummy_clazz});
                }
                break;
        }
    }
}

} // namespace lcs::input
