#include "game/lcs_physics_timing.h"

#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "cpu/callbacks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {
namespace {

constexpr float kPs2TimeStep = 50.0f / 30.0f; // 1.66667 in RW 1/50s time-step units

constexpr uint64_t kGotTimeStepRva        = 0x7f91e8ULL; // CTimer::ms_fTimeStep*
constexpr uint64_t kGotHandlingMgrPtrRva  = 0x7f9340ULL; // pmod_HandlingManager (CHandlingDataMgr**)
constexpr uint64_t kGotApplyTurnForceRva  = 0x803140ULL; // CPhysical::ApplyTurnForce(float...)
constexpr uint64_t kGotProcessWheelRva    = 0x807328ULL; // CVehicle::ProcessWheel
constexpr uint64_t kGotProcessBikeWheelRva= 0x807380ULL; // CVehicle::ProcessBikeWheel

// Return addresses (RVA) of per-frame ApplyTurnForce calls lacking CTimer::ms_fTimeStep scaling:
constexpr uint64_t kRaBikeWobble   = 0x4deab8ULL; // CBike::ProcessControl brake wobble (0.007*sin*turnMass)

// Exact locked-brake instructions where unclamped s1 = -fwd_speed / step overwrote longitudinal impulse:
constexpr uint64_t kWheelLockPatchRva = 0x4f770cULL;
constexpr uint64_t kWheelLockRetRva   = 0x4f7750ULL;
constexpr uint64_t kBikeLockPatchRva  = 0x4f7db8ULL;
constexpr uint64_t kBikeLockRetRva    = 0x4f7dfcULL;

inline uint32_t ldr_x_rel(uint8_t rt, size_t cur_off, size_t lit_off) {
    const int32_t imm = static_cast<int32_t>(lit_off - cur_off) >> 2;
    return 0x58000000u | ((static_cast<uint32_t>(imm) & 0x7ffffu) << 5) | (rt & 0x1fu);
}

void patch_bytes(lcs::cpu::DynarmicCore& cpu, uint64_t guest_va, const void* src, size_t sz) {
#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    auto* dst = reinterpret_cast<void*>(guest_va);
    VirtualProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(dst, src, sz);
    VirtualProtect(dst, sz, old_prot, &old_prot);
#else
    std::memcpy(reinterpret_cast<void*>(guest_va), src, sz);
#endif
    cpu.invalidate_cache(guest_va, sz);
}

// A zeroed RWX page for code stubs; they jump back with absolute branches, so it can live anywhere.
uint8_t* alloc_stub_page() {
#if defined(LCS_PLATFORM_WINDOWS)
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    auto* page = static_cast<uint8_t*>(std::aligned_alloc(4096, 0x1000));
#endif
    if (page) std::memset(page, 0, 0x1000);
    return page;
}

uint64_t patch_got(lcs::cpu::DynarmicCore& cpu, uint64_t load_bias, uint64_t got_rva, uint64_t new_target,
                   const lcs::cpu::ElfImage& image, const char* sym) {
    const uint64_t slot_va = load_bias + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot_va);
    if (!orig && sym) orig = image.find_symbol(sym);
    patch_bytes(cpu, slot_va, &new_target, sizeof(uint64_t));
    return orig;
}

float compute_frame_scale(uint64_t timestep_ptr_va) {
    if (!timestep_ptr_va) return 1.0f;
    const auto* ts_ptr = reinterpret_cast<const float* const*>(timestep_ptr_va);
    if (!ts_ptr || !*ts_ptr) return 1.0f;
    const float step = **ts_ptr;
    if (step > 0.0f && step < kPs2TimeStep) {
        return std::clamp(step / kPs2TimeStep, 0.02f, 1.0f);
    }
    return 1.0f;
}

} // namespace

namespace {
volatile uint32_t* g_scrollbar_tick = nullptr;
const float* g_cloud_rotation = nullptr;     // CClouds::CloudRotation (LCS_DIAG_CLOUDS)
const uint32_t* g_individual_rotation = nullptr;
const float* g_wind = nullptr;
const uint8_t* g_pads = nullptr;             // CPad Pads[]: horn history ring at +0xa6, index at +0xae
const uint32_t* g_time_ms = nullptr;         // CTimer::m_snTimeInMilliseconds

// Horn button on a siren vehicle (CAutomobile::ProcessControl): the game decides from an
// 8-frame horn history (released within 7 frames = toggle the siren, held 8 frames = horn), so
// at 144 FPS a tap had to be shorter than ~49 ms. Same decision on the game clock, 7 frames at
// 30 FPS = 233 ms. Returns 0 = no horn, 1 = horn, 2 = toggle the siren.
uint64_t siren_horn_decision() {
    constexpr uint32_t kTapMs = 233;
    static bool s_down = false;
    static uint32_t s_press_ms = 0;
    const bool down = g_pads[0xa6 + (g_pads[0xae] & 7)] != 0;
    const uint32_t now = *g_time_ms;
    uint64_t result = 0;
    if (down) {
        if (!s_down) s_press_ms = now;
        result = (now - s_press_ms >= kTapMs) ? 1 : 0;
    } else if (s_down) {
        result = (now - s_press_ms < kTapMs) ? 2 : 0;
    }
    s_down = down;
    return result;
}
} // namespace

void tick_fixed_rate_effects() {
    using Clock = std::chrono::steady_clock;
    // LCS_DIAG_CLOUDS=1: cloud rotation rates per second (frame-rate independent when the
    // IndividualRotation rate is 3000 * (Wind * 0.5 + 0.18) at any fps).
    static const bool s_diag_clouds = std::getenv("LCS_DIAG_CLOUDS") != nullptr;
    if (s_diag_clouds && g_cloud_rotation && g_individual_rotation && g_wind) {
        static Clock::time_point s_t0 = Clock::now();
        static float s_rot0 = *g_cloud_rotation;
        static uint32_t s_ind0 = *g_individual_rotation;
        static int s_frames = 0;
        ++s_frames;
        const double dt = std::chrono::duration<double>(Clock::now() - s_t0).count();
        if (dt >= 2.0) {
            const uint32_t ind = *g_individual_rotation;
            std::printf("  [Clouds] fps %.0f wind %.3f CloudRotation/s %.5f IndividualRotation/s %.0f (expected %.0f)\n",
                        s_frames / dt, *g_wind, (*g_cloud_rotation - s_rot0) / dt,
                        static_cast<double>(static_cast<uint16_t>(ind - s_ind0)) / dt,
                        3000.0 * (*g_wind * 0.5 + 0.18));
            std::fflush(stdout);
            s_t0 = Clock::now();
            s_rot0 = *g_cloud_rotation;
            s_ind0 = ind;
            s_frames = 0;
        }
    }
    // Sets the CScrollBar tick on frames where a 1/30 s step elapsed (at most one per frame).
    if (!g_scrollbar_tick) return;
    static Clock::time_point s_next = Clock::now();
    const auto now = Clock::now();
    if (now >= s_next) {
        *g_scrollbar_tick = 1;
        s_next += std::chrono::microseconds(33333);
        if (now - s_next > std::chrono::milliseconds(200)) s_next = now;  // after a stall
    } else {
        *g_scrollbar_tick = 0;
    }
}

void install_physics_timing_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    static const bool s_enabled = [] {
        const char* env = std::getenv("LCS_PHYSICS_TIMESTEP");
        return !(env && env[0] == '0');
    }();
    if (!s_enabled) {
        std::cout << "  [PhysicsTiming] Disabled via LCS_PHYSICS_TIMESTEP=0.\n";
        return;
    }

#if defined(LCS_PLATFORM_WINDOWS)
    void* page = VirtualAlloc(nullptr, 0x2000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page) {
        std::cerr << "  [WARN] Failed to allocate physics timing RWX hook page.\n";
        return;
    }
    auto* base = reinterpret_cast<uint8_t*>(page);
    std::memset(base, 0, 0x2000);
    const uint64_t base_addr = reinterpret_cast<uint64_t>(base);
    size_t cursor = 0;

    const uint64_t load_bias = image_game.get_load_bias();
    const uint64_t ts_ptr_va = load_bias + kGotTimeStepRva;
    const uint64_t handling_ptr_va = load_bias + kGotHandlingMgrPtrRva;

    auto& hle = lcs::cpu::HLEManager::instance();

    // Scratch state for rolling resistance save/restore across ProcessWheel / ProcessBikeWheel
    struct RollResState {
        float saved_val;
        uint32_t active;
        float steer_lim_car;
        float straight_lim_car;
        float handbrake_lim_car;
        float steer_thresh;
        float bike_lim;
    };
    const uint64_t phys_state_va = base_addr + 0x1e00ULL;
    auto* phys_state = reinterpret_cast<RollResState*>(base + 0x1e00ULL);
    phys_state->saved_val = 0.0f;
    phys_state->active = 0;
    phys_state->steer_lim_car = 0.80f;     // Front & Square footbraking cornering cap (78% lateral steering retained!)
    phys_state->straight_lim_car = 1.00f;  // Straight-line max braking = 100% traction
    phys_state->handbrake_lim_car = 1.28f; // Rear R1 handbrake drift allows breaking traction smoothly
    phys_state->steer_thresh = 0.005f;
    phys_state->bike_lim = 0.82f;          // Motorcycle wheel brake lock cap (prevents rear spinout/endo flip)

    // 1. ApplyTurnForce(this [x0], fx [s0], fy [s1], fz [s2], px [s3], py [s4], pz [s5])
    {
        const uint64_t cb_va = hle.register_function("__phys_turn_scale",
            [load_bias, ts_ptr_va](const uint64_t* args) -> uint64_t {
                const uint64_t ra = args[0] - load_bias;
                float scale = 1.0f;
                // Only scale per-frame brake speed wobble (matching VCS Recomp);
                // keep Lean1/2 and Steer1/2 unscaled so bikes retain full upright & steering authority.
                if (ra == kRaBikeWobble) {
                    scale = compute_frame_scale(ts_ptr_va);
                }
                uint32_t bits = 0;
                std::memcpy(&bits, &scale, 4);
                return bits;
            });

        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_atf = patch_got(
            cpu, load_bias, kGotApplyTurnForceRva, stub_va, image_game, "_ZN9CPhysical14ApplyTurnForceEffffff");

        // Save x0, x30 and d0..d5 across HLE call, scale s0..s2 by returned float bits in w0, tail-branch to orig.
        const uint32_t words[24] = {
            0xd10143ffu, // 0x00: sub sp, sp, #0x50
            0xa9007be0u, // 0x04: stp x0, x30, [sp, #0x00]
            0x6d0107e0u, // 0x08: stp d0, d1, [sp, #0x10]
            0x6d020fe2u, // 0x0c: stp d2, d3, [sp, #0x20]
            0x6d0317e4u, // 0x10: stp d4, d5, [sp, #0x30]
            0xaa1e03e0u, // 0x14: mov x0, x30
            ldr_x_rel(16, 0x18, 0x68),
            0xd63f0200u, // 0x1c: blr x16 (__phys_turn_scale)
            0x1e270010u, // 0x20: fmov s16, w0 (caller-saved; never clobber callee-saved d8/s8 used by CBike!)
            0xa9407be0u, // 0x24: ldp x0, x30, [sp, #0x00]
            0x6d4107e0u, // 0x28: ldp d0, d1, [sp, #0x10]
            0x6d420fe2u, // 0x2c: ldp d2, d3, [sp, #0x20]
            0x6d4317e4u, // 0x30: ldp d4, d5, [sp, #0x30]
            0x910143ffu, // 0x34: add sp, sp, #0x50
            0x1e300800u, // 0x38: fmul s0, s0, s16
            0x1e300821u, // 0x3c: fmul s1, s1, s16
            0x1e300842u, // 0x40: fmul s2, s2, s16
            ldr_x_rel(16, 0x44, 0x60),
            0xd61f0200u, // 0x48: br x16 (orig_ApplyTurnForce)
            0xd503201fu, // 0x4c: nop
            0xd503201fu, // 0x50: nop
            0xd503201fu, // 0x54: nop
            0xd503201fu, // 0x58: nop
            0xd503201fu, // 0x5c: nop
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 24);
        lits[0] = orig_atf;
        lits[1] = cb_va;
        cursor += sizeof(words) + 16;
        cursor = (cursor + 15) & ~size_t(15);
    }

    // 2. Rolling resistance scaling wrapper for ProcessWheel (GOT 0x807328) & ProcessBikeWheel (GOT 0x807380)
    //    No instruction inside either function accesses GOT 0x7f9340 except rolling resistance ([*pmod_HandlingMgr + 4]).
    auto install_roll_wrapper = [&](uint64_t got_rva, const char* sym, const char* name_pre, const char* name_post) {
        const uint64_t pre_va = hle.register_function(name_pre,
            [ts_ptr_va, handling_ptr_va, phys_state](const uint64_t*) -> uint64_t {
                phys_state->active = 0;
                const float scale = compute_frame_scale(ts_ptr_va);
                if (scale < 0.999f) {
                    // GOT[0x7f9340] points to pmod_HandlingManager (float**), whose pointee +4 is fRollingResistance.
                    auto* const* p_mgr_ptr = reinterpret_cast<float* const* const*>(handling_ptr_va);
                    float* const* mgr_ptr = p_mgr_ptr ? *p_mgr_ptr : nullptr;
                    float* mgr_obj = (mgr_ptr && *mgr_ptr) ? *mgr_ptr : nullptr;
                    if (mgr_obj) {
                        phys_state->saved_val = mgr_obj[1];
                        mgr_obj[1] = phys_state->saved_val * scale;
                        phys_state->active = 1;
                    }
                }
                return 0;
            });
        const uint64_t post_va = hle.register_function(name_post,
            [handling_ptr_va, phys_state](const uint64_t*) -> uint64_t {
                if (phys_state->active) {
                    auto* const* p_mgr_ptr = reinterpret_cast<float* const* const*>(handling_ptr_va);
                    float* const* mgr_ptr = p_mgr_ptr ? *p_mgr_ptr : nullptr;
                    float* mgr_obj = (mgr_ptr && *mgr_ptr) ? *mgr_ptr : nullptr;
                    if (mgr_obj) {
                        mgr_obj[1] = phys_state->saved_val;
                    }
                    phys_state->active = 0;
                }
                return 0;
            });

        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_fn = patch_got(cpu, load_bias, got_rva, stub_va, image_game, sym);

        // Clean frame layout: sub sp, sp, #0xd0 (stack args from caller sit at [sp+0xd0 .. sp+0xe8])
        const uint32_t wrap_words[40] = {
            0xd10343ffu, // 0x00: sub sp, sp, #0xd0
            0xa9027bfdu, // 0x04: stp x29, x30, [sp, #0x20]
            0xa90307e0u, // 0x08: stp x0, x1, [sp, #0x30]
            0xa9040fe2u, // 0x0c: stp x2, x3, [sp, #0x40]
            0xa90517e4u, // 0x10: stp x4, x5, [sp, #0x50]
            0xa9061fe6u, // 0x14: stp x6, x7, [sp, #0x60]
            0x6d0707e0u, // 0x18: stp d0, d1, [sp, #0x70]
            0x6d080fe2u, // 0x1c: stp d2, d3, [sp, #0x80]
            0x6d0917e4u, // 0x20: stp d4, d5, [sp, #0x90]
            0x6d0a1fe6u, // 0x24: stp d6, d7, [sp, #0xa0]
            0xa94d2feau, // 0x28: ldp x10, x11, [sp, #0xd0] (copy 32 bytes stack args to [sp, #0])
            0xa94e37ecu, // 0x2c: ldp x12, x13, [sp, #0xe0]
            0xa9002feau, // 0x30: stp x10, x11, [sp, #0x00]
            0xa90137ecu, // 0x34: stp x12, x13, [sp, #0x10]
            ldr_x_rel(16, 0x38, 0xa8), // pre_va
            0xd63f0200u, // 0x3c: blr x16
            0xa94307e0u, // 0x40: ldp x0, x1, [sp, #0x30]
            0xa9440fe2u, // 0x44: ldp x2, x3, [sp, #0x40]
            0xa94517e4u, // 0x48: ldp x4, x5, [sp, #0x50]
            0xa9461fe6u, // 0x4c: ldp x6, x7, [sp, #0x60]
            0x6d4707e0u, // 0x50: ldp d0, d1, [sp, #0x70]
            0x6d480fe2u, // 0x54: ldp d2, d3, [sp, #0x80]
            0x6d4917e4u, // 0x58: ldp d4, d5, [sp, #0x90]
            0x6d4a1fe6u, // 0x5c: ldp d6, d7, [sp, #0xa0]
            ldr_x_rel(16, 0x60, 0xa0), // orig_fn
            0xd63f0200u, // 0x64: blr x16
            ldr_x_rel(16, 0x68, 0xb0), // post_va
            0xd63f0200u, // 0x6c: blr x16
            0xa9427bfdu, // 0x70: ldp x29, x30, [sp, #0x20]
            0x910343ffu, // 0x74: add sp, sp, #0xd0
            0xd65f03c0u, // 0x78: ret
            0xd503201fu, // 0x7c..0x9c padding
            0xd503201fu, 0xd503201fu, 0xd503201fu, 0xd503201fu,
            0xd503201fu, 0xd503201fu, 0xd503201fu, 0xd503201fu,
        };
        std::memcpy(code, wrap_words, sizeof(wrap_words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 40);
        lits[0] = orig_fn;
        lits[1] = pre_va;
        lits[2] = post_va;
        cursor += sizeof(wrap_words) + 24;
        cursor = (cursor + 15) & ~size_t(15);
    };

    install_roll_wrapper(kGotProcessWheelRva,
                         "_ZN8CVehicle12ProcessWheelER7CVectorS1_S1_S1_ifffcPfP11tWheelStatet",
                         "__phys_car_wheel_pre", "__phys_car_wheel_post");
    install_roll_wrapper(kGotProcessBikeWheelRva,
                         "_ZN8CVehicle16ProcessBikeWheelER7CVectorS1_S1_S1_iffffcPfP11tWheelState17eBikeWheelSpecialt",
                         "__phys_bike_wheel_pre", "__phys_bike_wheel_post");

    // 3. Locked-wheel longitudinal brake clamp in CVehicle::ProcessWheel (0x4f770c):
    //    Replaces unclamped `s16 = s1 (-fwd_speed / step)` with `clamp(s1, -lim, +lim)` so steering under Square
    //    retains lateral grip while straight-line braking and R1 handbrake drift remain full strength.
    {
        const uint64_t patch_va = load_bias + kWheelLockPatchRva;
        static constexpr uint32_t kExpected[4] = {0x1e204160u, 0x1e204030u, 0x52800068u, 0xb9000388u};
        uint32_t actual[4]{};
        std::memcpy(actual, reinterpret_cast<const void*>(patch_va), sizeof(actual));
        if (std::memcmp(actual, kExpected, sizeof(kExpected)) == 0) {
            const uint64_t stub_va = base_addr + cursor;
            auto* code = reinterpret_cast<uint32_t*>(base + cursor);
            // Re-checked branch offsets explicitly:
            const uint32_t car_words[24] = {
                0x1e204160u, // 0x00: fmov s0, s11
                0x52800068u, // 0x04: mov w8, #3
                0xb9000388u, // 0x08: str w8, [x28]
                ldr_x_rel(9, 0x0c, 0x60), // 0x0c: ldr x9, =phys_state_va
                0x394b3688u, // 0x10: ldrb w8, [x20, #0x2cd]
                0x362800a8u, // 0x14: tbz w8, #5, +0x14 (0x28 not handbrake)
                0x36000097u, // 0x18: tbz w23, #0, +0x10 (0x28 front wheel)
                0xbd401123u, // 0x1c: ldr s3, [x9, #0x10] (handbrake_lim_car = 1.28f)
                0x14000009u, // 0x20: b +0x24 (0x44)
                0xd503201fu, // 0x24: nop
                0xbd42c282u, // 0x28: ldr s2, [x20, #0x2c0] (m_fSteerAngle)
                0x1e20c042u, // 0x2c: fabs s2, s2
                0xbd401523u, // 0x30: ldr s3, [x9, #0x14] (steer_thresh = 0.005f)
                0x1e232040u, // 0x34: fcmp s2, s3
                0xbd400d22u, // 0x38: ldr s2, [x9, #0x0c] (straight = 1.00f)
                0xbd400923u, // 0x3c: ldr s3, [x9, #0x08] (steer = 0.80f)
                0x1e22cc63u, // 0x40: fcsel s3, s3, s2, gt
                0x1e290863u, // 0x44: fmul s3, s3, s9 (cap = factor * s9)
                0x1e235902u, // 0x48: fmin s2, s8, s3 (lim = min(s8, cap))
                0x1e214043u, // 0x4c: fneg s3, s2
                0x1e225830u, // 0x50: fmin s16, s1, s2
                0x1e234a10u, // 0x54: fmax s16, s16, s3
                ldr_x_rel(16, 0x58, 0x68),
                0xd61f0200u, // 0x5c: br x16 -> kWheelLockRetRva
            };
            std::memcpy(code, car_words, sizeof(car_words));
            auto* lits = reinterpret_cast<uint64_t*>(code + 24);
            lits[0] = phys_state_va;
            lits[1] = load_bias + kWheelLockRetRva;
            cursor += sizeof(car_words) + 16;
            cursor = (cursor + 15) & ~size_t(15);

            struct { uint32_t ldr_x16, br_x16; uint64_t target; } detour{0x58000050u, 0xd61f0200u, stub_va};
            patch_bytes(cpu, patch_va, &detour, sizeof(detour));
        } else {
            std::cerr << "  [WARN] ProcessWheel lock signature mismatch; car braking grip patch skipped.\n";
        }
    }

    // 4. Locked-wheel longitudinal brake clamp in CVehicle::ProcessBikeWheel (0x4f7db8):
    //    Replaces unclamped `s6 = s1 (-fwd_speed / step)` with `clamp(s1, -lim, +lim)` (`lim = min(s9, 0.82*s10)`).
    {
        const uint64_t patch_va = load_bias + kBikeLockPatchRva;
        static constexpr uint32_t kExpected[4] = {0x1e204180u, 0x1e204026u, 0x52800068u, 0xb9000388u};
        uint32_t actual[4]{};
        std::memcpy(actual, reinterpret_cast<const void*>(patch_va), sizeof(actual));
        if (std::memcmp(actual, kExpected, sizeof(kExpected)) == 0) {
            const uint64_t stub_va = base_addr + cursor;
            auto* code = reinterpret_cast<uint32_t*>(base + cursor);
            // Inputs live: s1=-fwd_speed/step, s9=brake_imp, s10=grip_lim, s12=lat_imp, x28=&state.
            // Scratch: x9, w8, s2, s3. Exits to kBikeLockRetRva with s0=s12, s6=clamped, [x28]=3.
            const uint32_t bike_words[14] = {
                0x1e204180u, // 0x00: fmov s0, s12
                0x52800068u, // 0x04: mov w8, #3
                0xb9000388u, // 0x08: str w8, [x28]
                ldr_x_rel(9, 0x0c, 0x38), // 0x0c: ldr x9, =phys_state_va
                0xbd401923u, // 0x10: ldr s3, [x9, #0x18] (bike_lim = 0.82f)
                0x1e2a0863u, // 0x14: fmul s3, s3, s10 (cap = 0.82 * grip)
                0x1e235922u, // 0x18: fmin s2, s9, s3  (lim = min(brake, cap))
                0x1e214043u, // 0x1c: fneg s3, s2
                0x1e225826u, // 0x20: fmin s6, s1, s2
                0x1e2348c6u, // 0x24: fmax s6, s6, s3
                ldr_x_rel(16, 0x28, 0x40),
                0xd61f0200u, // 0x2c: br x16 -> kBikeLockRetRva
                0xd503201fu, // 0x30: nop
                0xd503201fu, // 0x34: nop
            };
            std::memcpy(code, bike_words, sizeof(bike_words));
            auto* lits = reinterpret_cast<uint64_t*>(code + 14);
            lits[0] = phys_state_va;
            lits[1] = load_bias + kBikeLockRetRva;
            cursor += sizeof(bike_words) + 16;
            cursor = (cursor + 15) & ~size_t(15);

            struct { uint32_t ldr_x16, br_x16; uint64_t target; } detour{0x58000050u, 0xd61f0200u, stub_va};
            patch_bytes(cpu, patch_va, &detour, sizeof(detour));
        } else {
            std::cerr << "  [WARN] ProcessBikeWheel lock signature mismatch; bike braking grip patch skipped.\n";
        }
    }

    cpu.invalidate_cache(base_addr, 0x2000);
    std::cout << "  - Installed vehicle physics timing & braking lateral-grip hooks.\n";

    // Blinking HUD: CHud::Draw tests bits 2/3 of CTimer::m_FrameCounter 8 times (flashing health,
    // wanted stars, timers...), CDarkel::DrawMessages once (rampage texts): 4x too fast at 120 fps.
    // Their GOT slots point to wrappers that set m_FrameCounter to the 30 fps count of the game
    // clock (m_snTimeInMilliseconds * 3 / 100) for the call and restore it afterwards.
    {
        const uint64_t fc_va = image_game.find_symbol("_ZN6CTimer14m_FrameCounterE");
        const uint64_t ms_va = image_game.find_symbol("_ZN6CTimer22m_snTimeInMillisecondsE");
        struct Wrapped { uint64_t got_rva; const char* sym; };
        static constexpr Wrapped kBlinkers[] = {
            {0x806a48ULL, "_ZN4CHud4DrawEv"},
            {0x804be8ULL, "_ZN7CDarkel12DrawMessagesEv"},
        };
        uint8_t* page = (fc_va && ms_va) ? alloc_stub_page() : nullptr;
        int wrapped = 0;
        for (size_t i = 0; page && i < std::size(kBlinkers); ++i) {
            uint8_t* stub = page + i * 0x60;
            const uint64_t stub_va = reinterpret_cast<uint64_t>(stub);
            const uint64_t orig = patch_got(cpu, image_game.get_load_bias(), kBlinkers[i].got_rva, stub_va,
                                            image_game, kBlinkers[i].sym);
            if (!orig) continue;
            static constexpr uint32_t kWrap[] = {
                0xa9be7bfdu, // stp   x29, x30, [sp, #-32]!
                0xf9000bf3u, // str   x19, [sp, #16]
                0x58000210u, // ldr   x16, fc_ptr
                0xb9400213u, // ldr   w19, [x16]          the real frame counter
                0x58000211u, // ldr   x17, ms_ptr
                0xb9400231u, // ldr   w17, [x17]
                0x52800069u, // mov   w9, #3
                0x9ba97e31u, // umull x17, w17, w9
                0xd2800c89u, // mov   x9, #100
                0x9ac90a31u, // udiv  x17, x17, x9        ms * 3 / 100: frames at 30 fps
                0xb9000211u, // str   w17, [x16]
                0x58000170u, // ldr   x16, orig
                0xd63f0200u, // blr   x16
                0x580000b0u, // ldr   x16, fc_ptr
                0xb9000213u, // str   w19, [x16]          restored
                0xf9400bf3u, // ldr   x19, [sp, #16]
                0xa8c27bfdu, // ldp   x29, x30, [sp], #32
                0xd65f03c0u, // ret
            };
            const uint64_t lits[3] = {fc_va, ms_va, orig};
            std::memcpy(stub, kWrap, sizeof(kWrap));
            std::memcpy(stub + 0x48, lits, sizeof(lits));
            cpu.invalidate_cache(stub_va, 0x60);
            ++wrapped;
        }
        std::cout << "  - Applied 30 fps blink rate to " << wrapped << " HUD draw functions.\n";
    }

    // Peds pushing cars: CPhysical::ApplyCollision gives a ped (A) a mass factor (s9: 10, or about
    // 1 for the player against a vehicle) and every frame of contact moves the vehicle by an
    // impulse of that size - 4x as many pushes per second at 120 fps, so Toni could shove cars
    // while running into them. Where A's factor is final (+0x34e604), a vehicle B scales it by
    // min(1, timestep * 0.6): the same at 30 fps, the 30 fps impulse per second above it.
    // LCS_PED_PUSH_FIX=0 leaves it alone.
    if (const char* e = std::getenv("LCS_PED_PUSH_FIX"); !(e && e[0] == '0')) {
        const uint64_t lb = image_game.get_load_bias();
        const uint64_t ts_va = image_game.find_symbol("_ZN6CTimer12ms_fTimeStepE");
        const auto* site = reinterpret_cast<const uint32_t*>(lb + 0x34e604ULL);
        const bool layout_ok = site[0] == 0xf9402e88u &&  // ldr x8, [x20, #0x58]   A's flags
                               site[1] == 0x927f0908u &&  // and x8, x8, #0xe        entity type
                               site[2] == 0xf100191fu &&  // cmp x8, #6              ped
                               site[3] == 0x540001c1u;    // b.ne +0x44
        uint8_t* page = (layout_ok && ts_va) ? alloc_stub_page() : nullptr;
        if (page) {
            static constexpr uint32_t kPushStub[] = {
                0xadbf07e0u, // stp   q0, q1, [sp, #-32]!
                0xf9402e68u, // ldr   x8, [x19, #0x58]     B's flags
                0x927f0908u, // and   x8, x8, #0xe
                0xf100111fu, // cmp   x8, #4               vehicle
                0x54000101u, // b.ne  restore
                0x58000230u, // ldr   x16, ts_ptr
                0xbd400200u, // ldr   s0, [x16]            CTimer::ms_fTimeStep
                0x1c0002a1u, // ldr   s1, k                0.6
                0x1e210800u, // fmul  s0, s0, s1
                0x1e2e1001u, // fmov  s1, #1.0
                0x1e215800u, // fmin  s0, s0, s1
                0x1e200929u, // fmul  s9, s9, s0           A's mass factor
                0xacc107e0u, // restore: ldp q0, q1, [sp], #32
                0xf9402e88u, // ldr   x8, [x20, #0x58]     (the replaced instructions)
                0x927f0908u, // and   x8, x8, #0xe
                0xf100191fu, // cmp   x8, #6
                0x54000061u, // b.ne  not_ped
                0x580000f0u, // ldr   x16, cont            +0x34e614
                0xd61f0200u, // br    x16
                0x580000f0u, // not_ped: ldr x16, not_ped_va   +0x34e648
                0xd61f0200u, // br    x16
                0xd503201fu, // nop
            };
            const uint64_t page_va = reinterpret_cast<uint64_t>(page);
            const uint64_t lits[3] = {ts_va, lb + 0x34e614ULL, lb + 0x34e648ULL};
            const float k = 0.6f;
            std::memcpy(page, kPushStub, sizeof(kPushStub));
            std::memcpy(page + 0x58, lits, sizeof(lits));
            std::memcpy(page + 0x70, &k, sizeof(k));
            cpu.invalidate_cache(page_va, 0x80);
            const uint32_t jump[4] = {0x58000050u, 0xd61f0200u,  // ldr x16, #8 ; br x16
                                      static_cast<uint32_t>(page_va), static_cast<uint32_t>(page_va >> 32)};
            patch_bytes(cpu, lb + 0x34e604ULL, jump, sizeof(jump));
            std::cout << "  - Applied frame-rate independent ped -> vehicle push (ApplyCollision).\n";
        } else {
            std::cerr << "  [PhysicsTiming] ApplyCollision layout differs; ped push fix not applied.\n";
        }
    }

    // CScrollBar::Update (LED ticker signs): a per-instance frame counter scrolls the text one
    // column every 8 calls, tuned for 30 fps (4x too fast at 120 fps). Its GOT slot points to a
    // stub that runs the original only on frames where the 30 Hz tick (scrollbar_tick) is set.
    {
        if (auto* page = alloc_stub_page()) {
            const uint64_t page_va = reinterpret_cast<uint64_t>(page);
            const uint64_t load_bias = image_game.get_load_bias();
            const uint64_t orig = patch_got(cpu, load_bias, 0x806828ULL, page_va + 0x10, image_game,
                                            "_ZN10CScrollBar6UpdateEv");
            if (orig) {
                *reinterpret_cast<uint64_t*>(page) = orig;
                static constexpr uint32_t kStub[] = {
                    0x10FFFF90u, // adr  x16, page
                    0xB9400A11u, // ldr  w17, [x16, #8]   tick flag
                    0x34000071u, // cbz  w17, ret
                    0xF9400210u, // ldr  x16, [x16]       original
                    0xD61F0200u, // br   x16
                    0xD65F03C0u, // ret
                };
                std::memcpy(page + 0x10, kStub, sizeof(kStub));
                cpu.invalidate_cache(page_va, 0x40);
                g_scrollbar_tick = reinterpret_cast<volatile uint32_t*>(page + 0x08);
                *g_scrollbar_tick = 1;
                std::cout << "  - Applied 30 Hz CScrollBar::Update (LED ticker signs).\n";
            }
        }
    }

    // CClouds::Update, tuned for 30 fps (timestep 1.667) and applied every frame:
    //   CloudRotation      += sin(camera heading - 0.85) * 0.001 * Wind           (clouds orbit)
    //   IndividualRotation += 60 * (Wind * timestep * 0.5 + 0.3)                  (sprites spin)
    // Both had per-frame terms, ~4x too fast at 120 fps. Rewritten as
    //   CloudRotation      += sin(...) * 0.0006 * timestep * Wind
    //   IndividualRotation += 60 * timestep * (Wind * 0.5 + 0.18)
    // which give the same values at 30 fps. Here s1 = Wind and s2 = timestep.
    const uint64_t upd = image_game.find_symbol("_ZN7CClouds6UpdateEv");
    const uint64_t ts_va = image_game.find_symbol("_ZN6CTimer12ms_fTimeStepE");
    const uint64_t wind_va = image_game.find_symbol("_ZN8CWeather4WindE");
    if (upd && ts_va && wind_va) {
        auto* code = reinterpret_cast<uint32_t*>(upd);
        const bool layout_ok = code[0x90 / 4] == 0x52824de8u &&                      // mov  w8, #0x126f (0.001)
                               code[0xa8 / 4] == 0x1e220800u &&                      // fmul s0, s0, s2
                               code[0xac / 4] == 0xbd400262u &&                      // ldr  s2, [x19]
                               code[0xb0 / 4] == 0x1f010800u &&                      // fmadd s0, s0, s1, s2
                               code[0xb8 / 4] == 0x52933349u && code[0xbc / 4] == 0x72a7d329u &&  // w9 = 0.3
                               code[0xd4 / 4] == 0x1e210841u &&                      // fmul s1, s2, s1
                               code[0xd8 / 4] == 0x1e270122u &&                      // fmov s2, w9
                               code[0xe4 / 4] == 0x1f030821u;                        // fmadd s1, s1, s3, s2
        uint8_t* page = layout_ok ? alloc_stub_page() : nullptr;
        if (page) {
            // CloudRotation: +0x90..+0xb0 computes it after sinf; jump to a stub that returns to +0xb4
            // with s0 = new CloudRotation and s1 = Wind, as the original. x16 is free after the call.
            static constexpr uint32_t kCloudStub[] = {
                0x58000190u, // ldr   x16, ts_ptr
                0xbd400201u, // ldr   s1, [x16]        timestep
                0x1c000202u, // ldr   s2, k            0.0006
                0x1e220800u, // fmul  s0, s0, s2
                0x1e210800u, // fmul  s0, s0, s1
                0x58000130u, // ldr   x16, wind_ptr
                0xbd400201u, // ldr   s1, [x16]        Wind
                0xbd400262u, // ldr   s2, [x19]        CloudRotation
                0x1f010800u, // fmadd s0, s0, s1, s2
                0x580000f0u, // ldr   x16, back
                0xd61f0200u, // br    x16
                0xd503201fu, // nop
            };
            const uint64_t page_va = reinterpret_cast<uint64_t>(page);
            const uint64_t lits[3] = {ts_va, wind_va, upd + 0xb4};
            const float k = 0.0006f;
            std::memcpy(page, kCloudStub, sizeof(kCloudStub));
            std::memcpy(page + 0x30, lits, sizeof(lits));
            std::memcpy(page + 0x48, &k, sizeof(k));
            cpu.invalidate_cache(page_va, 0x50);
            const uint32_t jump[4] = {
                0x58000050u, // ldr x16, #8
                0xd61f0200u, // br  x16
                static_cast<uint32_t>(page_va), static_cast<uint32_t>(page_va >> 32),
            };
            patch_bytes(cpu, upd + 0x90, jump, sizeof(jump));

            // IndividualRotation: w9 = 0.18 at +0xb8, and +0xd4..+0xe4 combine with s3 = 0.5.
            const uint32_t mov_w9 = code[0xdc / 4];   // mov w9, #60.0
            const uint32_t ldr_s4 = code[0xe0 / 4];   // ldr s4, [x8]
            const uint32_t consts[2] = {0x528a3d89u, 0x72a7c709u};  // w9 = 0.18f
            const uint32_t combine[5] = {
                0x1e270124u, // fmov  s4, w9          s4 = 0.18
                0x1f031021u, // fmadd s1, s1, s3, s4  s1 = Wind * 0.5 + 0.18
                mov_w9,      // mov   w9, #60.0
                0x1e220821u, // fmul  s1, s1, s2      s1 *= timestep
                ldr_s4,      // ldr   s4, [x8]        IndividualRotation (moved after its use)
            };
            patch_bytes(cpu, upd + 0xb8, consts, sizeof(consts));
            patch_bytes(cpu, upd + 0xd4, combine, sizeof(combine));
            g_cloud_rotation = reinterpret_cast<const float*>(image_game.find_symbol("_ZN7CClouds13CloudRotationE"));
            g_individual_rotation = reinterpret_cast<const uint32_t*>(image_game.find_symbol("_ZN7CClouds18IndividualRotationE"));
            g_wind = reinterpret_cast<const float*>(wind_va);
            std::cout << "  - Applied frame-rate independent CClouds::Update rotation.\n";
        } else {
            std::cerr << "  [PhysicsTiming] CClouds::Update layout differs; clouds fix not applied.\n";
        }
    }

    // Siren toggle vs. horn (FramerateVigilante's SirenTurnOnFix for GTA III, same logic in LCS):
    // the 8-frame horn history check after CVehicle::UsesSiren() at +0x4cba3c is replaced by
    // siren_horn_decision(). LCS_SIREN_TAP_FIX=0 leaves it alone.
    if (const char* e = std::getenv("LCS_SIREN_TAP_FIX"); !(e && e[0] == '0')) {
        const uint64_t lb = image_game.get_load_bias();
        const auto* site = reinterpret_cast<const uint32_t*>(lb + 0x4cba3cULL);
        static constexpr uint32_t kExpect[4] = {0xd0001968u, 0xf9414108u, 0x3942b909u, 0x9102990bu};
        g_pads = reinterpret_cast<const uint8_t*>(image_game.find_symbol("Pads"));
        g_time_ms = reinterpret_cast<const uint32_t*>(image_game.find_symbol("_ZN6CTimer22m_snTimeInMillisecondsE"));
        uint8_t* page = nullptr;
        if (g_pads && g_time_ms && std::memcmp(site, kExpect, sizeof(kExpect)) == 0 && (page = alloc_stub_page())) {
            const uint64_t hle_va = lcs::cpu::HLEManager::instance().register_function(
                "__lcs_siren_horn_decision", [](const uint64_t*) -> uint64_t { return siren_horn_decision(); });
            const uint32_t stub[16] = {
                0xaa1303e0u,             // mov  x0, x19 (this)
                ldr_x_rel(16, 0x04, 0x40),
                0xd63f0200u,             // blr  x16 -> siren_horn_decision
                0x7100041fu,             // cmp  w0, #1
                0x540000a0u,             // b.eq horn
                0x7100081fu,             // cmp  w0, #2
                0x540000a0u,             // b.eq toggle
                ldr_x_rel(16, 0x1c, 0x48),
                0xd61f0200u,             // br   x16 -> no horn (m_nCarHornTimer = 0)
                ldr_x_rel(16, 0x24, 0x50),
                0xd61f0200u,             // br   x16 -> horn (m_nCarHornTimer = 1)
                ldr_x_rel(16, 0x2c, 0x58),
                0xd61f0200u,             // br   x16 -> toggle m_bSirenOrAlarm
                0xd503201fu, 0xd503201fu, 0xd503201fu,
            };
            const uint64_t lits[4] = {hle_va, lb + 0x4cbaecULL, lb + 0x4cbac8ULL, lb + 0x4cd190ULL};
            const uint64_t page_va = reinterpret_cast<uint64_t>(page);
            std::memcpy(page, stub, sizeof(stub));
            std::memcpy(page + 0x40, lits, sizeof(lits));
            cpu.invalidate_cache(page_va, 0x60);
            const uint32_t jump[4] = {
                0x58000050u, // ldr x16, #8
                0xd61f0200u, // br  x16
                static_cast<uint32_t>(page_va), static_cast<uint32_t>(page_va >> 32),
            };
            patch_bytes(cpu, lb + 0x4cba3cULL, jump, sizeof(jump));
            std::cout << "  - Applied time-based siren toggle / horn (tap < 233 ms).\n";
        } else {
            std::cerr << "  [PhysicsTiming] Siren horn-history layout differs; siren fix not applied.\n";
        }
    }
#else
    (void)image_game; (void)cpu;
#endif
}

} // namespace lcs::game
