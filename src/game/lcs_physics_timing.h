#pragma once

namespace lcs::cpu {
class ElfImage;
class DynarmicCore;
} // namespace lcs::cpu

namespace lcs::game {

// Fixes high-FPS vehicle physics timing mismatches and braking lockup in libGame.so:
// 1. Scales per-frame ApplyTurnForce calls in CBike::ProcessControl (brake wobble, lean recovery)
//    and wheel/steer pushes by (ms_fTimeStep / kPs2Step) when running above 30 FPS.
// 2. Scales fixed per-frame rolling resistance in CVehicle::ProcessWheel and ProcessBikeWheel.
// 3. Clamps locked-wheel longitudinal impulse (-fwd_speed / step) in CVehicle::ProcessWheel and
//    CVehicle::ProcessBikeWheel so Square footbraking retains front car steering bite and bikes
//    slow down smoothly without rear-wheel snap oversteer or flipping.
void install_physics_timing_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
// Once per frame: advances fixed-rate (30 Hz) effects such as the LED ticker signs.
void tick_fixed_rate_effects();

} // namespace lcs::game
