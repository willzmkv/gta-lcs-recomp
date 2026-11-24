#pragma once

#include <cstdint>

namespace lcs::cpu {
class ElfImage;
}

namespace lcs::game {

enum class PromptDevice {
    KeyboardMouse,
    PlayStation,
    Xbox,
    Switch,
};

// Classic procedural SDF button/keycap/mouse prompt system (ported from VCS Recomp Classic style,
// zero external Kenney PNG dependencies). Automatically switches all 20 GTA: LCS tut_controller_*
// HUD/tutorial prompt sprites between Keyboard+Mouse (reflecting live controls.ini keybinds and
// on-foot / vehicle / menu context) and PlayStation / Xbox / Switch controller icons.
class LCSPrompts {
public:
    static LCSPrompts& instance();

    void init(const lcs::cpu::ElfImage& image_game);

    // Notify active input device (called from keyboard/mouse events and gamepad driver).
    void notify_keyboard_mouse();
    void notify_gamepad();
    void set_active_device(PromptDevice dev) { m_device = dev; }

    PromptDevice active_device() const { return m_device; }

    // Called once per frame in the render loop. Uploads/updates any loaded tut_controller_*
    // textures whenever a sprite loads for the first time or device/context changes.
    void update(bool in_menu, bool in_vehicle);

private:
    LCSPrompts() = default;

    struct Slot {
        const char* symbol = nullptr;
        uint64_t global_addr = 0;
        uint32_t last_tex_id = 0;
        uint64_t last_state_hash = 0;
    };

    bool m_initialized = false;
    PromptDevice m_device = PromptDevice::KeyboardMouse;
    Slot m_slots[20]{};
    int m_slot_count = 0;
};

} // namespace lcs::game
