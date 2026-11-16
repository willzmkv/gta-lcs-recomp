#include "input_system.h"
#include "gamepad.h"
#include "cpu/dynarmic_core.h"

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
    m_queue.push_back({EventType::JoyButtonDown, joy_id, button, 0.0f, 0.0f, 0.0f});
}

void InputSystem::queue_joy_button_up(int joy_id, int button) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::JoyButtonUp, joy_id, button, 0.0f, 0.0f, 0.0f});
}

void InputSystem::queue_joy_axis(int joy_id, int axis, float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back({EventType::JoyAxis, joy_id, axis, 0.0f, 0.0f, value});
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
        m_prev_xinput_buttons = 0;
        return;
    }

    uint32_t btns = state.Gamepad.wButtons;

    // Mapping table: XInput mask -> War Drum button index (0..15)
    // 0: A, 1: B, 2: X, 3: Y, 4: LB, 5: RB, 6: BACK, 7: START, 8: L3, 9: R3
    // 10: DPAD_UP, 11: DPAD_DOWN, 12: DPAD_LEFT, 13: DPAD_RIGHT
    struct ButtonMap {
        WORD xinput_bit;
        int war_button;
    };
    static const ButtonMap kMaps[] = {
        {XINPUT_GAMEPAD_A,              0},
        {XINPUT_GAMEPAD_B,              1},
        {XINPUT_GAMEPAD_X,              2},
        {XINPUT_GAMEPAD_Y,              3},
        {XINPUT_GAMEPAD_LEFT_SHOULDER,  4},
        {XINPUT_GAMEPAD_RIGHT_SHOULDER, 5},
        {XINPUT_GAMEPAD_BACK,           6},
        {XINPUT_GAMEPAD_START,          7},
        {XINPUT_GAMEPAD_LEFT_THUMB,     8},
        {XINPUT_GAMEPAD_RIGHT_THUMB,    9},
        {XINPUT_GAMEPAD_DPAD_UP,        10},
        {XINPUT_GAMEPAD_DPAD_DOWN,      11},
        {XINPUT_GAMEPAD_DPAD_LEFT,      12},
        {XINPUT_GAMEPAD_DPAD_RIGHT,     13},
    };

    for (const auto& bm : kMaps) {
        bool now = (btns & bm.xinput_bit) != 0;
        bool prev = (m_prev_xinput_buttons & bm.xinput_bit) != 0;
        if (now && !prev) {
            queue_joy_button_down(0, bm.war_button);
        } else if (!now && prev) {
            queue_joy_button_up(0, bm.war_button);
        }
    }
    m_prev_xinput_buttons = btns;

    // Sticks
    auto normalize_thumb = [](SHORT val, SHORT deadzone) -> float {
        if (std::abs(val) < deadzone) return 0.0f;
        float f = static_cast<float>(val) / 32767.0f;
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return f;
    };

    float lx = normalize_thumb(state.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    float ly = normalize_thumb(state.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
    float rx = normalize_thumb(state.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    float ry = normalize_thumb(state.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
    float lt = static_cast<float>(state.Gamepad.bLeftTrigger) / 255.0f;
    float rt = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;

    float curr_axes[6] = {lx, ly, rx, ry, lt, rt};
    for (int a = 0; a < 6; a++) {
        if (std::abs(curr_axes[a] - m_prev_axes[a]) > 0.01f) {
            m_prev_axes[a] = curr_axes[a];
            queue_joy_axis(0, a, curr_axes[a]);
        }
    }
#endif
}

void InputSystem::dispatch_events(lcs::cpu::DynarmicCore& cpu, uint64_t env_addr, uint64_t dummy_clazz) {
    if (!m_initialized) return;

    if (!m_joysticks_enabled && m_fn_no_joysticks) {
        cpu.call(m_fn_no_joysticks, {env_addr, dummy_clazz, 1});
        m_joysticks_enabled = true;
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
                if (m_fn_touch_start) {
                    cpu.set_fp_float(0, ev.x);
                    cpu.set_fp_float(1, ev.y);
                    cpu.call(m_fn_touch_start, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id)});
                }
                break;
            case EventType::TouchMove:
                if (m_fn_touch_move) {
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
                if (m_fn_joy_down) {
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
                    cpu.set_fp_float(0, ev.value);
                    cpu.call(m_fn_joy_axis, {env_addr, dummy_clazz, static_cast<uint64_t>(ev.id), static_cast<uint64_t>(ev.index)});
                }
                break;
            case EventType::BackButton:
                if (m_fn_back_button) {
                    cpu.call(m_fn_back_button, {env_addr, dummy_clazz});
                }
                break;
        }
    }
}

} // namespace lcs::input
