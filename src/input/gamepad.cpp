#include "gamepad.h"
#include "input_system.h"

#include <iostream>
#include <cmath>
#include <string>
#include <array>
#include <algorithm>

#if defined(LCS_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace lcs::input {

struct GamepadDriver::Impl {
#if defined(LCS_HAS_SDL3)
    SDL_Gamepad* pad = nullptr;
    SDL_JoystickID pad_id = 0;
#endif
    bool connected = false;
    std::string device_name;
    ControllerFamily family = ControllerFamily::Unknown;
    GamepadCapabilities caps;

    std::array<bool, 16> prev_buttons{};
    std::array<float, 6> prev_axes{};

    void open_gamepad(uint32_t instance_id) {
#if defined(LCS_HAS_SDL3)
        if (pad) {
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }

        pad = SDL_OpenGamepad(instance_id);
        if (!pad) return;

        pad_id = instance_id;
        connected = true;

        const char* name = SDL_GetGamepadName(pad);
        device_name = name ? name : "Gamepad";

        SDL_GamepadType type = SDL_GetGamepadType(pad);
        switch (type) {
            case SDL_GAMEPAD_TYPE_PS5:
                family = ControllerFamily::PlayStation;
                caps.is_dualsense = true;
                break;
            case SDL_GAMEPAD_TYPE_PS4:
            case SDL_GAMEPAD_TYPE_PS3:
                family = ControllerFamily::PlayStation;
                caps.is_dualsense = false;
                break;
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                family = ControllerFamily::Nintendo;
                caps.is_dualsense = false;
                break;
            case SDL_GAMEPAD_TYPE_XBOX360:
            case SDL_GAMEPAD_TYPE_XBOXONE:
                family = ControllerFamily::Xbox;
                caps.is_dualsense = false;
                break;
            default:
                family = ControllerFamily::Unknown;
                caps.is_dualsense = false;
                break;
        }

        SDL_PropertiesID props = SDL_GetGamepadProperties(pad);
        caps.has_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
        caps.has_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        caps.has_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);

        std::cout << "[Gamepad] Connected: " << device_name
                  << " (Family: " << (family == ControllerFamily::PlayStation ? "PlayStation" :
                                     family == ControllerFamily::Xbox ? "Xbox" :
                                     family == ControllerFamily::Nintendo ? "Nintendo" : "Generic")
                  << (caps.is_dualsense ? ", DualSense" : "")
                  << (caps.has_led ? ", RGB LED" : "")
                  << (caps.has_rumble ? ", Haptic Rumble" : "")
                  << ")\n" << std::flush;

        // Set signature GTA Liberty City Stories cyan/blue LED for DualSense / DualShock 4
        if (caps.has_led) {
            SDL_SetGamepadLED(pad, 0, 160, 255);
        }
#endif
    }

    void close_gamepad() {
#if defined(LCS_HAS_SDL3)
        if (pad) {
            std::cout << "[Gamepad] Disconnected: " << device_name << "\n" << std::flush;
            SDL_CloseGamepad(pad);
            pad = nullptr;
        }
#endif
        connected = false;
        device_name.clear();
        family = ControllerFamily::Unknown;
        caps = {};
        prev_buttons.fill(false);
        prev_axes.fill(0.0f);
    }
};

GamepadDriver& GamepadDriver::instance() {
    static GamepadDriver s_instance;
    return s_instance;
}

GamepadDriver::GamepadDriver() : m_impl(new Impl()) {}

GamepadDriver::~GamepadDriver() {
    shutdown();
    delete m_impl;
}

bool GamepadDriver::initialize() {
#if defined(LCS_HAS_SDL3)
    // Configure SDL3 hints for full DualSense (PS5), DualShock (PS4/PS3), Switch & Xbox controller support
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        std::cerr << "[Gamepad] Failed to init SDL3 gamepad subsystem: " << SDL_GetError() << "\n";
        return false;
    }

    SDL_PumpEvents();
    SDL_UpdateGamepads();

    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads && count > 0) {
        m_impl->open_gamepad(gamepads[0]);
        SDL_free(gamepads);
    }
    return true;
#else
    return false;
#endif
}

void GamepadDriver::shutdown() {
    m_impl->close_gamepad();
#if defined(LCS_HAS_SDL3)
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
}

void GamepadDriver::poll() {
#if defined(LCS_HAS_SDL3)
    SDL_PumpEvents();
    SDL_UpdateGamepads();

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
            if (!m_impl->connected) {
                m_impl->open_gamepad(ev.gdevice.which);
            }
        } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
            if (m_impl->pad_id == ev.gdevice.which) {
                m_impl->close_gamepad();
                // Check if another gamepad is still connected
                int count = 0;
                SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
                if (gamepads && count > 0) {
                    m_impl->open_gamepad(gamepads[0]);
                    SDL_free(gamepads);
                }
            }
        }
    }

    if (!m_impl->connected) {
        int count = 0;
        SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
        if (gamepads && count > 0) {
            m_impl->open_gamepad(gamepads[0]);
            SDL_free(gamepads);
        }
    }

    if (!m_impl->connected || !m_impl->pad) return;

    auto& input = InputSystem::instance();

    // Map of SDL3 Gamepad buttons to War Drum / GTA LCS button IDs (0..13)
    static constexpr struct {
        SDL_GamepadButton sdl_btn;
        int war_btn;
    } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH,          0},  // Cross / A
        {SDL_GAMEPAD_BUTTON_EAST,           1},  // Circle / B
        {SDL_GAMEPAD_BUTTON_WEST,           2},  // Square / X
        {SDL_GAMEPAD_BUTTON_NORTH,          3},  // Triangle / Y
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  4},  // L1
        {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5},  // R1
        {SDL_GAMEPAD_BUTTON_BACK,           6},  // Select / Back
        {SDL_GAMEPAD_BUTTON_START,          7},  // Start
        {SDL_GAMEPAD_BUTTON_LEFT_STICK,     8},  // L3
        {SDL_GAMEPAD_BUTTON_RIGHT_STICK,    9},  // R3
        {SDL_GAMEPAD_BUTTON_DPAD_UP,        10}, // Up
        {SDL_GAMEPAD_BUTTON_DPAD_DOWN,      11}, // Down
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT,      12}, // Left
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     13}, // Right
    };

    for (const auto& mapping : kButtons) {
        bool down = SDL_GetGamepadButton(m_impl->pad, mapping.sdl_btn);
        bool prev = m_impl->prev_buttons[mapping.war_btn];
        if (down && !prev) {
            input.queue_joy_button_down(0, mapping.war_btn);
        } else if (!down && prev) {
            input.queue_joy_button_up(0, mapping.war_btn);
        }
        m_impl->prev_buttons[mapping.war_btn] = down;
    }

    // Axes
    auto get_axis_norm = [this](SDL_GamepadAxis axis, float deadzone) -> float {
        int16_t raw = SDL_GetGamepadAxis(m_impl->pad, axis);
        float norm = static_cast<float>(raw) / 32767.0f;
        if (std::abs(norm) < deadzone) return 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        if (norm < -1.0f) norm = -1.0f;
        return norm;
    };

    auto get_trigger_norm = [this](SDL_GamepadAxis axis) -> float {
        int16_t raw = SDL_GetGamepadAxis(m_impl->pad, axis);
        float norm = static_cast<float>(raw) / 32767.0f;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;
        return norm;
    };

    float axes[6];
    axes[0] = get_axis_norm(SDL_GAMEPAD_AXIS_LEFTX, 0.15f);
    axes[1] = get_axis_norm(SDL_GAMEPAD_AXIS_LEFTY, 0.15f);
    axes[2] = get_axis_norm(SDL_GAMEPAD_AXIS_RIGHTX, 0.15f);
    axes[3] = get_axis_norm(SDL_GAMEPAD_AXIS_RIGHTY, 0.15f);
    axes[4] = get_trigger_norm(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    axes[5] = get_trigger_norm(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    for (int a = 0; a < 6; a++) {
        if (std::abs(axes[a] - m_impl->prev_axes[a]) > 0.01f) {
            m_impl->prev_axes[a] = axes[a];
            input.queue_joy_axis(0, a, axes[a]);
        }
    }

    // Map triggers to digital buttons 14 (L2) and 15 (R2)
    bool l2_down = (axes[4] > 0.5f);
    bool r2_down = (axes[5] > 0.5f);
    if (l2_down && !m_impl->prev_buttons[14]) input.queue_joy_button_down(0, 14);
    else if (!l2_down && m_impl->prev_buttons[14]) input.queue_joy_button_up(0, 14);
    m_impl->prev_buttons[14] = l2_down;

    if (r2_down && !m_impl->prev_buttons[15]) input.queue_joy_button_down(0, 15);
    else if (!r2_down && m_impl->prev_buttons[15]) input.queue_joy_button_up(0, 15);
    m_impl->prev_buttons[15] = r2_down;
#endif
}

bool GamepadDriver::is_connected() const {
    return m_impl->connected;
}

const char* GamepadDriver::get_device_name() const {
    return m_impl->device_name.c_str();
}

ControllerFamily GamepadDriver::get_family() const {
    return m_impl->family;
}

const GamepadCapabilities& GamepadDriver::get_caps() const {
    return m_impl->caps;
}

void GamepadDriver::set_led_color(uint8_t r, uint8_t g, uint8_t b) {
#if defined(LCS_HAS_SDL3)
    if (m_impl->pad && m_impl->caps.has_led) {
        SDL_SetGamepadLED(m_impl->pad, r, g, b);
    }
#endif
}

void GamepadDriver::set_rumble(float low_freq, float high_freq, uint32_t duration_ms) {
#if defined(LCS_HAS_SDL3)
    if (m_impl->pad && m_impl->caps.has_rumble) {
        uint16_t low = static_cast<uint16_t>(std::clamp(low_freq, 0.0f, 1.0f) * 65535.0f);
        uint16_t high = static_cast<uint16_t>(std::clamp(high_freq, 0.0f, 1.0f) * 65535.0f);
        SDL_RumbleGamepad(m_impl->pad, low, high, duration_ms);
    }
#endif
}

void GamepadDriver::set_trigger_rumble(float left, float right, uint32_t duration_ms) {
#if defined(LCS_HAS_SDL3)
    if (m_impl->pad && m_impl->caps.has_trigger_rumble) {
        uint16_t l = static_cast<uint16_t>(std::clamp(left, 0.0f, 1.0f) * 65535.0f);
        uint16_t r = static_cast<uint16_t>(std::clamp(right, 0.0f, 1.0f) * 65535.0f);
        SDL_RumbleGamepadTriggers(m_impl->pad, l, r, duration_ms);
    }
#endif
}

} // namespace lcs::input
