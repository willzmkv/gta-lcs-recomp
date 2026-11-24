// Pop-in probe (verification rig, LCS_DIAG_POPIN=1): watches the ped and vehicle pools for new
// entities and records how far from the player each one appeared and whether it was on screen
// (CEntity::GetIsOnScreen, the game's own visibility test). What matters to the player is what
// appears in view nearby; spawns out of view, behind the camera, are invisible.
#pragma once

#include <cstdint>
#include <string>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
} // namespace lcs::cpu

namespace lcs::game {

class PopinProbe {
public:
    static PopinProbe& instance();

    void init(const lcs::cpu::ElfImage& image_game);
    // Must run on the game thread (it calls guest code).
    void tick(lcs::cpu::DynarmicCore& cpu);
    // Prints the counts gathered since the last report (and resets them).
    void report(const std::string& label, double seconds);

private:
    struct Counts {
        int in_view[4] = {};   // < 50, 50-100, 100-150, >= 150 m
        int out_view[3] = {};  // < 30, 30-60, >= 60 m
        float min_in_view = 1e9f;
        double near100 = 0.0, near200 = 0.0;  // live entities within 100 / 200 m, summed per tick
        int ticks = 0;
    };
    void scan_pool(lcs::cpu::DynarmicCore& cpu, uint64_t got_rva, uint64_t stride, uint8_t* prev, Counts& out,
                   const float* player, uint64_t skip_a, uint64_t skip_b, bool is_ped);

    uint64_t m_load_bias = 0;
    uint64_t m_fn_on_screen = 0;
    uint64_t m_fn_player_ped = 0;
    uint64_t m_fn_player_vehicle = 0;
    bool m_primed = false;
    uint8_t m_prev_peds[1024]{};
    uint8_t m_prev_vehicles[1024]{};
    Counts m_peds;
    Counts m_vehicles;
};

} // namespace lcs::game
