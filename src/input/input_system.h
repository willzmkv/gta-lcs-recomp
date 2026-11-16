#pragma once

#include <cstdint>
#include <vector>
#include <mutex>

namespace lcs::cpu { class DynarmicCore; }

namespace lcs::input {

enum class EventType {
    TouchDown,
    TouchUp,
    TouchMove,
    JoyButtonDown,
    JoyButtonUp,
    JoyAxis,
    BackButton
};

struct RawInputEvent {
    EventType type;
    int id = 0;           // pointer id (0..3) or joy id (0)
    int index = 0;        // button index (0..15) or axis index (0..5)
    float x = 0.0f;       // normalized x (0.0 .. 1.0)
    float y = 0.0f;       // normalized y (0.0 .. 1.0)
    float value = 0.0f;   // axis value (-1.0f .. 1.0f)
};

class InputSystem {
public:
    static InputSystem& instance();

    void init(uint64_t fn_touch_start,
              uint64_t fn_touch_move,
              uint64_t fn_touch_end,
              uint64_t fn_joy_down,
              uint64_t fn_joy_up,
              uint64_t fn_joy_axis,
              uint64_t fn_no_joysticks,
              uint64_t fn_back_button);

    // Queue events from Win32 WindowProc (thread-safe)
    void queue_touch_down(int id, float norm_x, float norm_y);
    void queue_touch_up(int id, float norm_x, float norm_y);
    void queue_touch_move(int id, float norm_x, float norm_y);
    void queue_joy_button_down(int joy_id, int button);
    void queue_joy_button_up(int joy_id, int button);
    void queue_joy_axis(int joy_id, int axis, float value);
    void queue_back_button();

    // Poll XInput gamepads and queue gamepad events
    void poll_gamepad();

    // Dispatch all queued events to guest libGame.so via the main thread Dynarmic core
    void dispatch_events(lcs::cpu::DynarmicCore& cpu, uint64_t env_addr, uint64_t dummy_clazz);

    bool is_initialized() const { return m_initialized; }

private:
    InputSystem() = default;

    std::mutex m_mutex;
    std::vector<RawInputEvent> m_queue;

    uint64_t m_fn_touch_start = 0;
    uint64_t m_fn_touch_move = 0;
    uint64_t m_fn_touch_end = 0;
    uint64_t m_fn_joy_down = 0;
    uint64_t m_fn_joy_up = 0;
    uint64_t m_fn_joy_axis = 0;
    uint64_t m_fn_no_joysticks = 0;
    uint64_t m_fn_back_button = 0;

    bool m_initialized = false;
    bool m_joysticks_enabled = false;

    // Previous gamepad state for edge detection
    uint32_t m_prev_xinput_buttons = 0;
    float m_prev_axes[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

} // namespace lcs::input
