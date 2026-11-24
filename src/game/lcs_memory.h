#pragma once

#include <cstdint>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
} // namespace lcs::cpu

namespace lcs::game {

// PC streaming retention policy for libGame.so:
// Respects world.streaming_memory (64..512 MB) and CPool occupancy to avoid
// needlessly deleting nearby/behind-camera RSL entities & models when ample RAM is configured.
class LcsMemory {
public:
    static LcsMemory& instance();

    void install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    float max_pool_pressure() const { return m_last_pressure; }

private:
    LcsMemory() = default;
    ~LcsMemory();
    LcsMemory(const LcsMemory&) = delete;
    LcsMemory& operator=(const LcsMemory&) = delete;

    struct HookOps;
    friend struct HookOps;

    float measure_pool_pressure() const;
    uint64_t should_allow_far_delete(uint64_t vec_ptr);
    uint64_t should_allow_behind_cam_delete();
    uint64_t should_allow_remove_least_used();

    uint64_t m_load_bias = 0;
    uint8_t* m_stub_page = nullptr;
    bool     m_installed = false;
    float    m_last_pressure = 0.0f;

    float    m_last_jump_x = 0.0f;
    float    m_last_jump_y = 0.0f;
    bool     m_has_last_pos = false;
    uint64_t m_tick_counter = 0;

    uint64_t m_orig_delete_far = 0;
    uint64_t m_orig_delete_behind = 0;
    uint64_t m_orig_remove_least = 0;
};

} // namespace lcs::game
