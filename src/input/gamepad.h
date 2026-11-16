#pragma once

#include <cstdint>

namespace lcs::input {

enum class ControllerFamily {
    Unknown,
    Xbox,
    PlayStation,
    Nintendo,
};

struct GamepadCapabilities {
    bool has_led = false;
    bool has_rumble = false;
    bool has_trigger_rumble = false;
    bool is_dualsense = false;
};

class GamepadDriver {
public:
    static GamepadDriver& instance();

    bool initialize();
    void shutdown();

    void poll();

    bool is_connected() const;
    const char* get_device_name() const;
    ControllerFamily get_family() const;
    const GamepadCapabilities& get_caps() const;

    void set_led_color(uint8_t r, uint8_t g, uint8_t b);
    void set_rumble(float low_freq, float high_freq, uint32_t duration_ms = 1000);
    void set_trigger_rumble(float left, float right, uint32_t duration_ms = 1000);

private:
    GamepadDriver();
    ~GamepadDriver();

    GamepadDriver(const GamepadDriver&) = delete;
    GamepadDriver& operator=(const GamepadDriver&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace lcs::input
