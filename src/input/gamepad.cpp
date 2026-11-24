#include "gamepad.h"
#include "input_system.h"
#include "game/lcs_prompts.h"

#include <iostream>
#include <cmath>
#include <string>
#include <array>
#include <vector>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#if defined(LCS_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace lcs::input {

namespace {

float norm_s16_raw(int16_t raw) {
    return std::clamp(static_cast<float>(raw) / 32767.0f, -1.0f, 1.0f);
}

// Smooth scaled radial deadzone preserves 360-degree analog walking/turning without cardinal snapping.
void apply_radial_deadzone(float& x, float& y, float deadzone) {
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
}

float norm_trigger(int16_t raw) {
    const float t = std::clamp(static_cast<float>(raw) / 32767.0f, 0.0f, 1.0f);
    return (t < 0.04f) ? 0.0f : t;
}

} // namespace

struct GamepadDriver::Impl {
#if defined(LCS_HAS_SDL3)
    struct PadSlot {
        SDL_Gamepad* pad = nullptr;
        SDL_Joystick* joy = nullptr; // Raw fallback if not mapped as SDL_Gamepad
        SDL_JoystickID id = 0;
        uint16_t vid = 0;
        uint16_t pid = 0;
        std::string name;
        ControllerFamily family = ControllerFamily::Unknown;
        GamepadCapabilities caps;
    };
    std::vector<PadSlot> slots;
#endif
    bool initialized = false;
    bool connected = false;
    uint64_t trigger_rumble_until_ms = 0;
    std::string device_name;
    ControllerFamily family = ControllerFamily::Unknown;
    GamepadCapabilities caps;

    std::array<bool, 16> prev_buttons{};
    std::array<float, 6> prev_axes{};

    // Snapshot & command state shared between dedicated SDL3 gamepad thread and main loop.
    // Running SDL3 on its own thread keeps SDL_HIDAPI_discovery's hidden window + PM_NOREMOVE
    // PeekMessage loop off the main Win32 UI thread, preventing DWM false hung-window ghosting.
    mutable std::mutex state_mutex;
    std::condition_variable worker_cv;
    std::thread worker_thread;
    std::atomic<bool> stop_requested{false};
    bool init_finished = false;
    bool init_ok = false;

    std::array<bool, 16> shared_buttons{};
    std::array<float, 6> shared_axes{};
    bool shared_connected = false;
    std::string shared_device_name;
    ControllerFamily shared_family = ControllerFamily::Unknown;
    GamepadCapabilities shared_caps{};

    // Main-thread cached view for stable getters returning const char* / const&
    bool main_connected = false;
    std::string main_device_name;
    ControllerFamily main_family = ControllerFamily::Unknown;
    GamepadCapabilities main_caps{};

    bool pending_led = false;
    uint8_t led_r = 0, led_g = 0, led_b = 0;
    bool pending_rumble = false;
    uint16_t rumble_low = 0, rumble_high = 0;
    uint32_t rumble_dur_ms = 0;
    bool pending_trig_rumble = false;
    uint16_t trig_left = 0, trig_right = 0;
    uint32_t trig_dur_ms = 0;

    bool has_id(uint32_t instance_id) const {
#if defined(LCS_HAS_SDL3)
        for (const auto& s : slots) {
            if (s.id == instance_id) return true;
        }
#else
        (void)instance_id;
#endif
        return false;
    }

    void refresh_primary_summary() {
#if defined(LCS_HAS_SDL3)
        if (slots.empty()) {
            connected = false;
            device_name.clear();
            family = ControllerFamily::Unknown;
            caps = {};
            return;
        }
        // Prefer PlayStation/DualSense summary if present, else first connected slot
        size_t best = 0;
        for (size_t i = 0; i < slots.size(); i++) {
            if (slots[i].caps.is_dualsense) { best = i; break; }
        }
        connected = true;
        device_name = slots[best].name;
        family = slots[best].family;
        caps = slots[best].caps;
#endif
    }

    void open_device(uint32_t instance_id) {
#if defined(LCS_HAS_SDL3)
        if (has_id(instance_id)) return;

        PadSlot slot;
        slot.id = instance_id;
        slot.vid = SDL_GetJoystickVendorForID(instance_id);
        slot.pid = SDL_GetJoystickProductForID(instance_id);
        const char* raw_name = SDL_GetJoystickNameForID(instance_id);
        bool is_gp = SDL_IsGamepad(instance_id);

        std::cout << "[Gamepad] Detected device id=" << instance_id
                  << " name=\"" << (raw_name ? raw_name : "?") << "\""
                  << " VID=0x" << std::hex << slot.vid << " PID=0x" << slot.pid << std::dec
                  << " is_gamepad=" << (is_gp ? "yes" : "no") << "\n" << std::flush;

        if (is_gp) {
            slot.pad = SDL_OpenGamepad(instance_id);
            if (!slot.pad) {
                std::cerr << "[Gamepad] SDL_OpenGamepad(" << instance_id << ") failed: " << SDL_GetError() << "\n";
                return;
            }
            const char* name = SDL_GetGamepadName(slot.pad);
            slot.name = name ? name : (raw_name ? raw_name : "Gamepad");

            SDL_GamepadType type = SDL_GetGamepadType(slot.pad);
            switch (type) {
                case SDL_GAMEPAD_TYPE_PS5:
                    slot.family = ControllerFamily::PlayStation;
                    slot.caps.is_dualsense = true;
                    break;
                case SDL_GAMEPAD_TYPE_PS4:
                case SDL_GAMEPAD_TYPE_PS3:
                    slot.family = ControllerFamily::PlayStation;
                    break;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                    slot.family = ControllerFamily::Nintendo;
                    break;
                case SDL_GAMEPAD_TYPE_XBOX360:
                case SDL_GAMEPAD_TYPE_XBOXONE:
                    slot.family = ControllerFamily::Xbox;
                    break;
                default:
                    if (slot.vid == 0x054C) {
                        slot.family = ControllerFamily::PlayStation;
                        slot.caps.is_dualsense = (slot.pid == 0x0CE6 || slot.pid == 0x0DF2);
                    } else if (slot.vid == 0x057E) {
                        slot.family = ControllerFamily::Nintendo;
                    } else if (slot.vid == 0x045E) {
                        slot.family = ControllerFamily::Xbox;
                    }
                    break;
            }

            SDL_PropertiesID props = SDL_GetGamepadProperties(slot.pad);
            slot.caps.has_led = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
            slot.caps.has_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            slot.caps.has_trigger_rumble = SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false);

            if (slot.caps.has_led) {
                SDL_SetGamepadLED(slot.pad, 0, 160, 255);
            }
        } else {
            slot.joy = SDL_OpenJoystick(instance_id);
            if (!slot.joy) {
                std::cerr << "[Gamepad] SDL_OpenJoystick(" << instance_id << ") failed: " << SDL_GetError() << "\n";
                return;
            }
            const char* name = SDL_GetJoystickName(slot.joy);
            slot.name = name ? name : (raw_name ? raw_name : "Joystick");
            if (slot.vid == 0x054C) {
                slot.family = ControllerFamily::PlayStation;
                slot.caps.is_dualsense = (slot.pid == 0x0CE6 || slot.pid == 0x0DF2);
            } else if (slot.vid == 0x057E) {
                slot.family = ControllerFamily::Nintendo;
            } else if (slot.vid == 0x045E) {
                slot.family = ControllerFamily::Xbox;
            }
        }

        std::cout << "[Gamepad] Connected slot #" << slots.size() << ": " << slot.name
                  << " (Family: " << (slot.family == ControllerFamily::PlayStation ? "PlayStation" :
                                     slot.family == ControllerFamily::Xbox ? "Xbox" :
                                     slot.family == ControllerFamily::Nintendo ? "Nintendo" : "Generic")
                  << (slot.caps.is_dualsense ? ", DualSense" : "")
                  << (slot.pad ? ", SDL_Gamepad" : ", Raw SDL_Joystick")
                  << (slot.caps.has_led ? ", RGB LED" : "")
                  << (slot.caps.has_rumble ? ", Haptic Rumble" : "")
                  << ")\n" << std::flush;

        slots.push_back(std::move(slot));
        refresh_primary_summary();
#else
        (void)instance_id;
#endif
    }

    void close_device(uint32_t instance_id) {
#if defined(LCS_HAS_SDL3)
        for (size_t i = 0; i < slots.size(); i++) {
            if (slots[i].id == instance_id) {
                std::cout << "[Gamepad] Disconnected: " << slots[i].name << "\n" << std::flush;
                if (slots[i].pad) SDL_CloseGamepad(slots[i].pad);
                if (slots[i].joy) SDL_CloseJoystick(slots[i].joy);
                slots.erase(slots.begin() + i);
                break;
            }
        }
        refresh_primary_summary();
#else
        (void)instance_id;
#endif
    }

    void close_all() {
#if defined(LCS_HAS_SDL3)
        for (auto& s : slots) {
            if (s.pad) SDL_CloseGamepad(s.pad);
            if (s.joy) SDL_CloseJoystick(s.joy);
        }
        slots.clear();
#endif
        connected = false;
        device_name.clear();
        family = ControllerFamily::Unknown;
        caps = {};
        prev_buttons.fill(false);
        prev_axes.fill(0.0f);
    }

    void sync_all_devices() {
#if defined(LCS_HAS_SDL3)
        int count = 0;
        SDL_JoystickID* joys = SDL_GetJoysticks(&count);
        if (joys) {
            for (int i = 0; i < count; i++) {
                if (!has_id(joys[i])) {
                    open_device(joys[i]);
                }
            }
            SDL_free(joys);
        }
#endif
    }
    void sample_devices_on_worker(std::array<bool, 16>& cur_buttons, std::array<float, 6>& cur_axes) {
#if defined(LCS_HAS_SDL3)
        // Exact War Drum / GTA LCS button table verified from libGame.so (0x3448f4..0x344abc):
        // 0: Cross/A, 1: Circle/B, 2: Square/X, 3: Triangle/Y, 4: Select, 5: Start,
        // 6: L1, 7: R1, 8: DPadUp, 9: DPadDown, 10: DPadLeft, 11: DPadRight, 12: L3, 13: R3
        static constexpr struct {
            SDL_GamepadButton sdl_btn;
            int war_btn;
        } kButtons[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH,          0},  // Cross / A
            {SDL_GAMEPAD_BUTTON_EAST,           1},  // Circle / B
            {SDL_GAMEPAD_BUTTON_WEST,           2},  // Square / X
            {SDL_GAMEPAD_BUTTON_NORTH,          3},  // Triangle / Y
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  6},  // L1
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7},  // R1
            {SDL_GAMEPAD_BUTTON_DPAD_UP,        8},  // D-Pad Up
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN,      9},  // D-Pad Down
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT,      10}, // D-Pad Left
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     11}, // D-Pad Right
            {SDL_GAMEPAD_BUTTON_LEFT_STICK,     12}, // L3
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK,    13}, // R3
            {SDL_GAMEPAD_BUTTON_BACK,           5},  // Select / Camera Mode (CPad::Update #5 -> +0x24)
            {SDL_GAMEPAD_BUTTON_TOUCHPAD,       5},  // DualSense Touchpad (Select)
            {SDL_GAMEPAD_BUTTON_START,          4},  // Start / Pause (CPad::Update #4 -> +0x22)
        };

        for (const auto& s : slots) {
            bool slot_active = false;
            if (s.pad) {
                for (const auto& mapping : kButtons) {
                    if (SDL_GetGamepadButton(s.pad, mapping.sdl_btn)) {
                        cur_buttons[mapping.war_btn] = true;
                        slot_active = true;
                    }
                }
                float lx = norm_s16_raw(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_LEFTX));
                float ly = norm_s16_raw(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_LEFTY));
                float rx = norm_s16_raw(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHTX));
                float ry = norm_s16_raw(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHTY));
                apply_radial_deadzone(lx, ly, 0.14f);
                apply_radial_deadzone(rx, ry, 0.14f);
                float pad_axes[6] = {
                    lx,
                    ly,
                    rx,
                    ry,
                    norm_trigger(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
                    norm_trigger(SDL_GetGamepadAxis(s.pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)),
                };
                for (int a = 0; a < 6; a++) {
                    if (std::abs(pad_axes[a]) > 0.25f) {
                        slot_active = true;
                    }
                    if (std::abs(pad_axes[a]) > std::abs(cur_axes[a])) {
                        cur_axes[a] = pad_axes[a];
                    }
                }
            } else if (s.joy) {
                // Raw DualSense / HID fallback mapping (Windows DirectInput / HID)
                // Indices: 0:Square(2), 1:Cross(0), 2:Circle(1), 3:Triangle(3), 4:L1(6), 5:R1(7), 8:Select(5), 9:Start(4), 10:L3(12), 11:R3(13), 13:Touchpad(5)
                static constexpr int kRawBtnToWar[14] = {2, 0, 1, 3, 6, 7, -1, -1, 5, 4, 12, 13, -1, 5};
                int nb = SDL_GetNumJoystickButtons(s.joy);
                for (int b = 0; b < std::min(nb, 14); b++) {
                    if (kRawBtnToWar[b] >= 0 && SDL_GetJoystickButton(s.joy, b)) {
                        cur_buttons[kRawBtnToWar[b]] = true;
                        slot_active = true;
                    }
                }
                if (SDL_GetNumJoystickHats(s.joy) > 0) {
                    uint8_t hat = SDL_GetJoystickHat(s.joy, 0);
                    if (hat & SDL_HAT_UP)    { cur_buttons[8]  = true; slot_active = true; }
                    if (hat & SDL_HAT_DOWN)  { cur_buttons[9]  = true; slot_active = true; }
                    if (hat & SDL_HAT_LEFT)  { cur_buttons[10] = true; slot_active = true; }
                    if (hat & SDL_HAT_RIGHT) { cur_buttons[11] = true; slot_active = true; }
                }
                int na = SDL_GetNumJoystickAxes(s.joy);
                bool is_sony = (SDL_GetJoystickVendor(s.joy) == 0x054C);
                if (is_sony && na >= 6) {
                    float lx = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 0));
                    float ly = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 1));
                    float rx = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 2));
                    float ry = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 5));
                    apply_radial_deadzone(lx, ly, 0.14f);
                    apply_radial_deadzone(rx, ry, 0.14f);
                    float lt = std::clamp((static_cast<float>(SDL_GetJoystickAxis(s.joy, 3)) + 32768.0f) / 65535.0f, 0.0f, 1.0f);
                    float rt = std::clamp((static_cast<float>(SDL_GetJoystickAxis(s.joy, 4)) + 32768.0f) / 65535.0f, 0.0f, 1.0f);
                    if (std::abs(lx) > 0.25f || std::abs(ly) > 0.25f || std::abs(rx) > 0.25f ||
                        std::abs(ry) > 0.25f || lt > 0.25f || rt > 0.25f) {
                        slot_active = true;
                    }
                    if (std::abs(lx) > std::abs(cur_axes[0])) cur_axes[0] = lx;
                    if (std::abs(ly) > std::abs(cur_axes[1])) cur_axes[1] = ly;
                    if (std::abs(rx) > std::abs(cur_axes[2])) cur_axes[2] = rx;
                    if (std::abs(ry) > std::abs(cur_axes[3])) cur_axes[3] = ry;
                    if (lt > cur_axes[4]) cur_axes[4] = lt;
                    if (rt > cur_axes[5]) cur_axes[5] = rt;
                } else {
                    if (na >= 4) {
                        float lx = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 0));
                        float ly = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 1));
                        float rx = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 2));
                        float ry = norm_s16_raw(SDL_GetJoystickAxis(s.joy, 3));
                        apply_radial_deadzone(lx, ly, 0.14f);
                        apply_radial_deadzone(rx, ry, 0.14f);
                        if (std::abs(lx) > 0.25f || std::abs(ly) > 0.25f || std::abs(rx) > 0.25f || std::abs(ry) > 0.25f) {
                            slot_active = true;
                        }
                        if (std::abs(lx) > std::abs(cur_axes[0])) cur_axes[0] = lx;
                        if (std::abs(ly) > std::abs(cur_axes[1])) cur_axes[1] = ly;
                        if (std::abs(rx) > std::abs(cur_axes[2])) cur_axes[2] = rx;
                        if (std::abs(ry) > std::abs(cur_axes[3])) cur_axes[3] = ry;
                    }
                    if (na >= 6) {
                        float lt = std::clamp((static_cast<float>(SDL_GetJoystickAxis(s.joy, 4)) + 32768.0f) / 65535.0f, 0.0f, 1.0f);
                        float rt = std::clamp((static_cast<float>(SDL_GetJoystickAxis(s.joy, 5)) + 32768.0f) / 65535.0f, 0.0f, 1.0f);
                        if (lt > 0.25f || rt > 0.25f) slot_active = true;
                        if (lt > cur_axes[4]) cur_axes[4] = lt;
                        if (rt > cur_axes[5]) cur_axes[5] = rt;
                    }
                }
            }

            // Track the most recently operated physical controller so multi-controller setups switch prompts live
            if (slot_active) {
                connected = true;
                device_name = s.name;
                family = s.family;
                caps = s.caps;
            }

            // DualSense / PlayStation exclusive LED illumination & subtle trigger resistance when not overridden by gameplay haptics
            if (s.pad) {
                SDL_SetGamepadLED(s.pad, 0, 80, 255);
                if ((cur_axes[4] > 0.05f || cur_axes[5] > 0.05f) &&
                    SDL_GetTicks() >= trigger_rumble_until_ms) {
                    uint16_t lt_rumble = static_cast<uint16_t>(std::clamp(cur_axes[4] * 32767.0f, 0.0f, 32767.0f));
                    uint16_t rt_rumble = static_cast<uint16_t>(std::clamp(cur_axes[5] * 38000.0f, 0.0f, 65535.0f));
                    SDL_RumbleGamepadTriggers(s.pad, lt_rumble, rt_rumble, 50);
                }
            }
        }
#else
        (void)cur_buttons;
        (void)cur_axes;
#endif
    }

    void worker_loop() {
#if defined(LCS_HAS_SDL3)
        // Configure SDL3 hints for full DualSense (PS5), DualShock (PS4/PS3), Switch & Xbox support over USB & BT
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");

        if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK)) {
            std::cerr << "[Gamepad] Failed to init SDL3 gamepad subsystem: " << SDL_GetError() << "\n";
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                init_ok = false;
                init_finished = true;
            }
            worker_cv.notify_all();
            return;
        }

        SDL_PumpEvents();
        SDL_UpdateGamepads();
        SDL_UpdateJoysticks();
        sync_all_devices();
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            shared_connected = connected;
            shared_device_name = device_name;
            shared_family = family;
            shared_caps = caps;
            init_ok = true;
            init_finished = true;
        }
        worker_cv.notify_all();

        while (!stop_requested.load(std::memory_order_acquire)) {
            SDL_PumpEvents();
            SDL_UpdateGamepads();
            SDL_UpdateJoysticks();

            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_EVENT_GAMEPAD_ADDED || ev.type == SDL_EVENT_JOYSTICK_ADDED) {
                    open_device(ev.jdevice.which);
                } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED || ev.type == SDL_EVENT_JOYSTICK_REMOVED) {
                    close_device(ev.jdevice.which);
                }
            }

            static uint32_t s_rescan_ticks = 0;
            if (slots.empty() || (++s_rescan_ticks % 250u == 0u)) {
                sync_all_devices();
            }

            std::array<bool, 16> cur_buttons{};
            std::array<float, 6> cur_axes{};
            if (!slots.empty()) {
                sample_devices_on_worker(cur_buttons, cur_axes);
            }

            bool do_led = false;
            uint8_t lr = 0, lg = 0, lb = 0;
            bool do_rumble = false;
            uint16_t rlow = 0, rhigh = 0;
            uint32_t rdur = 0;
            bool do_trig = false;
            uint16_t tl = 0, tr = 0;
            uint32_t tdur = 0;

            {
                std::lock_guard<std::mutex> lock(state_mutex);
                shared_buttons = cur_buttons;
                shared_axes = cur_axes;
                shared_connected = connected;
                shared_device_name = device_name;
                shared_family = family;
                shared_caps = caps;

                if (pending_led) {
                    do_led = true;
                    lr = led_r; lg = led_g; lb = led_b;
                    pending_led = false;
                }
                if (pending_rumble) {
                    do_rumble = true;
                    rlow = rumble_low; rhigh = rumble_high; rdur = rumble_dur_ms;
                    pending_rumble = false;
                }
                if (pending_trig_rumble) {
                    do_trig = true;
                    tl = trig_left; tr = trig_right; tdur = trig_dur_ms;
                    pending_trig_rumble = false;
                }
            }

            if (do_led) {
                for (auto& s : slots) {
                    if (s.pad && s.caps.has_led) {
                        SDL_SetGamepadLED(s.pad, lr, lg, lb);
                    }
                }
            }
            if (do_rumble) {
                for (auto& s : slots) {
                    if (s.pad && s.caps.has_rumble) {
                        SDL_RumbleGamepad(s.pad, rlow, rhigh, rdur);
                    }
                }
            }
            if (do_trig) {
                trigger_rumble_until_ms = (tdur > 0) ? (SDL_GetTicks() + tdur) : 0;
                for (auto& s : slots) {
                    if (s.pad && s.caps.has_trigger_rumble) {
                        SDL_RumbleGamepadTriggers(s.pad, tl, tr, tdur);
                    }
                }
            }

            const auto wait_dur = !slots.empty()
                ? std::chrono::milliseconds(2)
                : std::chrono::milliseconds(8);
            std::unique_lock<std::mutex> lk(state_mutex);
            worker_cv.wait_for(lk, wait_dur, [&]() {
                return stop_requested.load(std::memory_order_acquire) ||
                       pending_led || pending_rumble || pending_trig_rumble;
            });
        }

        close_all();
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK);
#endif
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
    if (m_impl->initialized) return true;
#if defined(LCS_HAS_SDL3)
    m_impl->stop_requested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        m_impl->init_finished = false;
        m_impl->init_ok = false;
    }
    m_impl->worker_thread = std::thread([this]() {
        m_impl->worker_loop();
    });

    bool ok = false;
    {
        std::unique_lock<std::mutex> lock(m_impl->state_mutex);
        m_impl->worker_cv.wait(lock, [&]() { return m_impl->init_finished; });
        ok = m_impl->init_ok;
        m_impl->main_connected = m_impl->shared_connected;
        m_impl->main_device_name = m_impl->shared_device_name;
        m_impl->main_family = m_impl->shared_family;
        m_impl->main_caps = m_impl->shared_caps;
    }
    if (!ok) {
        if (m_impl->worker_thread.joinable()) {
            m_impl->worker_thread.join();
        }
        return false;
    }
    m_impl->initialized = true;
    return true;
#else
    return false;
#endif
}

void GamepadDriver::shutdown() {
#if defined(LCS_HAS_SDL3)
    m_impl->stop_requested.store(true, std::memory_order_release);
    m_impl->worker_cv.notify_all();
    if (m_impl->worker_thread.joinable()) {
        m_impl->worker_thread.join();
    }
#endif
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        m_impl->shared_buttons.fill(false);
        m_impl->shared_axes.fill(0.0f);
        m_impl->shared_connected = false;
        m_impl->shared_device_name.clear();
        m_impl->shared_family = ControllerFamily::Unknown;
        m_impl->shared_caps = {};
    }
    m_impl->main_connected = false;
    m_impl->main_device_name.clear();
    m_impl->main_family = ControllerFamily::Unknown;
    m_impl->main_caps = {};
    m_impl->prev_buttons.fill(false);
    m_impl->prev_axes.fill(0.0f);
    m_impl->initialized = false;
}

void GamepadDriver::poll() {
#if defined(LCS_HAS_SDL3)
    if (!m_impl->initialized) return;

    std::array<bool, 16> cur_buttons{};
    std::array<float, 6> cur_axes{};
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        cur_buttons = m_impl->shared_buttons;
        cur_axes = m_impl->shared_axes;
        m_impl->main_connected = m_impl->shared_connected;
        m_impl->main_device_name = m_impl->shared_device_name;
        m_impl->main_family = m_impl->shared_family;
        m_impl->main_caps = m_impl->shared_caps;
    }

    if (!m_impl->main_connected) {
        auto& input = InputSystem::instance();
        for (int b = 0; b < 16; b++) {
            if (m_impl->prev_buttons[b]) {
                m_impl->prev_buttons[b] = false;
                input.queue_joy_button_up(0, b);
            }
        }
        bool had_axes = false;
        for (int a = 0; a < 6; a++) {
            if (m_impl->prev_axes[a] != 0.0f) {
                m_impl->prev_axes[a] = 0.0f;
                had_axes = true;
            }
        }
        if (had_axes) {
            input.clear_pad_state();
        }
        return;
    }

    auto& input = InputSystem::instance();
    bool any_gamepad_activity = false;
    for (int b = 0; b < 16; b++) {
        if (cur_buttons[b]) {
            any_gamepad_activity = true;
        }
        if (cur_buttons[b] && !m_impl->prev_buttons[b]) {
            input.queue_joy_button_down(0, b);
        } else if (!cur_buttons[b] && m_impl->prev_buttons[b]) {
            input.queue_joy_button_up(0, b);
        }
        m_impl->prev_buttons[b] = cur_buttons[b];
    }

    for (int a = 0; a < 6; a++) {
        if (std::abs(cur_axes[a] - m_impl->prev_axes[a]) > 0.01f) {
            if ((a < 4 && std::abs(cur_axes[a]) > 0.35f) || (a >= 4 && cur_axes[a] > 0.35f)) {
                any_gamepad_activity = true;
            }
            m_impl->prev_axes[a] = cur_axes[a];
            input.queue_joy_axis(0, a, cur_axes[a]);
        }
    }
    if (any_gamepad_activity) {
        lcs::game::LCSPrompts::instance().notify_gamepad();
    }
#endif
}

bool GamepadDriver::is_connected() const {
    return m_impl->main_connected;
}

const char* GamepadDriver::get_device_name() const {
    return m_impl->main_device_name.c_str();
}

ControllerFamily GamepadDriver::get_family() const {
    return m_impl->main_family;
}

const GamepadCapabilities& GamepadDriver::get_caps() const {
    return m_impl->main_caps;
}

void GamepadDriver::set_led_color(uint8_t r, uint8_t g, uint8_t b) {
#if defined(LCS_HAS_SDL3)
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        m_impl->led_r = r;
        m_impl->led_g = g;
        m_impl->led_b = b;
        m_impl->pending_led = true;
    }
    m_impl->worker_cv.notify_one();
#else
    (void)r; (void)g; (void)b;
#endif
}

void GamepadDriver::set_rumble(float low_freq, float high_freq, uint32_t duration_ms) {
#if defined(LCS_HAS_SDL3)
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        m_impl->rumble_low = static_cast<uint16_t>(std::clamp(low_freq, 0.0f, 1.0f) * 65535.0f);
        m_impl->rumble_high = static_cast<uint16_t>(std::clamp(high_freq, 0.0f, 1.0f) * 65535.0f);
        m_impl->rumble_dur_ms = duration_ms;
        m_impl->pending_rumble = true;
    }
    m_impl->worker_cv.notify_one();
#else
    (void)low_freq; (void)high_freq; (void)duration_ms;
#endif
}

void GamepadDriver::set_trigger_rumble(float left, float right, uint32_t duration_ms) {
#if defined(LCS_HAS_SDL3)
    {
        std::lock_guard<std::mutex> lock(m_impl->state_mutex);
        m_impl->trig_left = static_cast<uint16_t>(std::clamp(left, 0.0f, 1.0f) * 65535.0f);
        m_impl->trig_right = static_cast<uint16_t>(std::clamp(right, 0.0f, 1.0f) * 65535.0f);
        m_impl->trig_dur_ms = duration_ms;
        m_impl->pending_trig_rumble = true;
    }
    m_impl->worker_cv.notify_one();
#else
    (void)left; (void)right; (void)duration_ms;
#endif
}

} // namespace lcs::input
