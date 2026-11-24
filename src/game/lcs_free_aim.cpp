#include "lcs_free_aim.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kDummyDistance = 12.0f;
constexpr float kPivotHeight = 0.6f;
constexpr float kPitchLimit = 1.15f;
constexpr float kStartPitchLimit = 0.35f;
constexpr float kStickRateRadPerSec = 2.0f;

// Verified offsets in libGame.so (arm64-v8a)
constexpr uint64_t kGotTimeStepRva        = 0x7f91e8ULL; // GOT slot for CTimer::ms_fTimeStep
constexpr uint64_t kGotProcessLineOfSight = 0x801530ULL;
constexpr uint64_t kGotCameraProcess      = 0x801978ULL;
constexpr uint64_t kGotCycleCamUp         = 0x801cc8ULL;
constexpr uint64_t kGotCycleCamDown       = 0x801cd0ULL;
constexpr uint64_t kGotDoGeometryAvoidance = 0x801bf0ULL;
constexpr uint64_t kGotEnterFreeAim       = 0x803068ULL;
constexpr uint64_t kGotSetLockOnTarget    = 0x805230ULL;
constexpr uint64_t kGotProcessWeapon      = 0x806478ULL;
constexpr uint64_t kGotPlayerFreeAim      = 0x806480ULL;
constexpr uint64_t kGotPlayerZelda        = 0x806490ULL;
constexpr uint64_t kGotFindNextTarget     = 0x806508ULL;
constexpr uint64_t kGotUpdatePads         = 0x8018f8ULL;
constexpr uint64_t kVtblPlayerProcessCtrl = 0x7eeb18ULL; // _ZTV10CPlayerPed + 0x60 (ProcessControl)
constexpr uint64_t kCamUseMouse3rdRva     = 0x850ae1ULL; // CCamera::m_bUseMouse3rdPerson
constexpr uint64_t kCamPedWallFeelerRva   = 0x852724ULL; // Process_FollowPed_SA feeler beta accumulator
constexpr uint64_t kCamPedWallSkipRva     = 0x852728ULL; // Process_FollowPed_SA wall pull-in stick-path counter

constexpr uint64_t kPedPosOffset          = 0x40ULL;   // CEntity position vec3
constexpr uint64_t kPedAnimMoveDeltaOffset= 0x380ULL;  // float anim_d[2] (local right, fwd)
constexpr uint64_t kPedPedStateOffset     = 0x3dcULL;  // uint32_t ePedState
constexpr uint64_t kPedCurRotOffset       = 0x5c8ULL;  // float m_fCurrentRotation
constexpr uint64_t kPedDestRotOffset      = 0x5ccULL;  // float m_fAimingRotation (target heading)
constexpr uint64_t kPedInVehicleFlag      = 0x618ULL;  // uint8_t bInVehicle
constexpr uint64_t kPedWeaponsOffset      = 0x6a8ULL;  // CWeapon[10], stride 0x1c
constexpr uint64_t kPedWeaponStride       = 0x1cULL;
constexpr uint64_t kPedCurWeaponSlot      = 0x7ccULL;  // int8_t active slot
constexpr uint64_t kPedTargetEntity       = 0x7d0ULL;  // CEntity* lock-on target
constexpr uint64_t kPedMoveBlendRatioOffset= 0xc28ULL; // float m_fMoveBlendRatio
constexpr uint64_t kPedFreeAimActive      = 0xda1ULL;  // uint8_t m_bInFreeAim
constexpr uint64_t kPedFreeAimDummy       = 0xda8ULL;  // CEntity* dummy object

constexpr uint64_t kCamActiveCamOffset    = 0x8fULL;   // uint8_t active cam index
constexpr uint64_t kCamArrayOffset        = 0x1a0ULL;  // CCam Cams[2]
constexpr uint64_t kCamStride             = 0x298ULL;
constexpr uint64_t kCamResetStaticsOffset = 0x1aULL;   // uint8_t m_bResetStatics (instant camera aim follow)
constexpr uint64_t kCamModeOffset         = 0x1cULL;   // int16_t m_nMode (4 = FollowPed, 11 = AimWeapon)
constexpr uint64_t kCamCarLookDirOffset   = 0x38ULL;   // uint32_t direction (3 = behind, 0/1/2 = look back/L/R)
constexpr uint64_t kCamVertAngleOffset    = 0xbcULL;   // float vertical pitch angle
constexpr uint64_t kCamVertSpeedOffset    = 0xc0ULL;   // float vertical angular speed
constexpr uint64_t kCamBetaOffset         = 0xccULL;   // float horizontal yaw angle
constexpr uint64_t kCamBetaSpeedOffset    = 0xd0ULL;   // float horizontal angular speed
constexpr uint64_t kCamFrontOffset        = 0x19cULL;  // float Front[3]
constexpr uint64_t kCamSourceOffset       = 0x1acULL;  // float Source[3]
constexpr uint64_t kCrossHairOffset       = 0x9ce9a0ULL; // gCrossHair (m_bActive, m_vecPos, ..., m_pTarget)

constexpr uint32_t ldr_x16_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000010u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

constexpr uint32_t ldr_x_rel(uint32_t rt, size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000000u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5) | (rt & 0x1fu);
}

float wrap_pi(float a) {
    a = std::fmod(a + kPi, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a - kPi;
}

float wrap_2pi(float a) {
    a = std::fmod(a, kTwoPi);
    return (a < 0.0f) ? (a + kTwoPi) : a;
}

} // namespace

LCSFreeAim& LCSFreeAim::instance() {
    static LCSFreeAim s_instance;
    return s_instance;
}

uint64_t LCSFreeAim::get_player_ped() const {
    if (!m_player_idx_addr || !m_players_arr_addr) return 0;
    const uint8_t idx = *reinterpret_cast<const uint8_t*>(m_player_idx_addr);
    return *reinterpret_cast<const uint64_t*>(m_players_arr_addr + static_cast<uint64_t>(idx) * 0x1a0ULL);
}

uint64_t LCSFreeAim::get_active_cam() const {
    if (!m_the_camera) return 0;
    const uint8_t idx = *reinterpret_cast<const uint8_t*>(m_the_camera + kCamActiveCamOffset);
    if (idx > 1) return 0;
    return m_the_camera + kCamArrayOffset + static_cast<uint64_t>(idx) * kCamStride;
}

bool LCSFreeAim::is_player_on_foot_with_gun(uint64_t ped) const {
    if (!ped) return false;
    if (*reinterpret_cast<const uint8_t*>(ped + kPedInVehicleFlag) != 0) return false;
    const int8_t slot = *reinterpret_cast<const int8_t*>(ped + kPedCurWeaponSlot);
    // Slots 3..7: Handgun, Shotgun, SMG, Rifle, Heavy (excludes 0..2 fists/melee/thrown and 8 sniper/RPG).
    if (slot < 3 || slot > 7) return false;
    const uint32_t wtype = *reinterpret_cast<const uint32_t*>(
        ped + kPedWeaponsOffset + static_cast<uint64_t>(slot) * kPedWeaponStride);
    return wtype != 0;
}

void LCSFreeAim::update_frame_input(bool enabled,
                                      bool want_aim,
                                      bool in_menu,
                                      bool in_cutscene,
                                      bool in_vehicle,
                                      bool mouse_look_mode,
                                      float mouse_turn_rad,
                                      float mouse_tilt_rad,
                                      float stick_rx,
                                      float stick_ry,
                                      float trigger_l2,
                                      float trigger_r2,
                                      float dt_sec) {
    m_enabled = enabled;
    m_in_menu = in_menu;
    m_in_cutscene = in_cutscene;
    m_in_vehicle = in_vehicle;
    m_mouse_look_mode = mouse_look_mode;
    m_want_aim = enabled && want_aim && !in_menu && !in_cutscene;
    m_dt_sec = std::clamp(dt_sec, 1.0f / 400.0f, 0.25f);
    m_stick_rx = stick_rx;
    m_stick_ry = stick_ry;
    m_trigger_l2 = trigger_l2;
    m_trigger_r2 = trigger_r2;

    if (m_in_menu || m_in_cutscene) {
        m_pending_turn = 0.0f;
        m_pending_tilt = 0.0f;
        m_look_turn = 0.0f;
        m_look_tilt = 0.0f;
        return;
    }

    if (m_active) {
        m_pending_turn += mouse_turn_rad;
        m_pending_tilt += mouse_tilt_rad;
        m_look_turn = 0.0f;
        m_look_tilt = 0.0f;
    } else {
        m_look_turn += mouse_turn_rad;
        m_look_tilt += mouse_tilt_rad;
        m_pending_turn = 0.0f;
        m_pending_tilt = 0.0f;
    }
}

bool LCSFreeAim::install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    if (m_installed) return true;

    m_load_bias = image_game.get_load_bias();
    m_the_camera = image_game.find_symbol("TheCamera");
    m_femm_addr = image_game.find_symbol("FrontEndMenuManager");
    const uint64_t fn_find_ped = image_game.find_symbol("_Z13FindPlayerPedv");
    const uint64_t fn_fire_hit = image_game.find_symbol("_ZN7CWeapon14FireInstantHitEP7CEntityP7CVector");
    if (!m_the_camera || !fn_find_ped || !fn_fire_hit) {
        std::cerr << "  [WARN] LCSFreeAim skipped (missing symbols).\n";
        return false;
    }

    // Resolve player index, array, and CTimer::ms_fTimeStep pointers directly from libGame.so GOT refs.
    m_player_idx_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7f9118ULL);
    m_players_arr_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7f9120ULL);
    m_timestep_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotTimeStepRva);
    m_fire_hit_start = fn_fire_hit;
    m_fire_hit_end = fn_fire_hit + 3056ULL;

    void* stub_page = nullptr;
#if defined(_WIN32)
    stub_page = VirtualAlloc(reinterpret_cast<void*>(0x7f00040000ULL), 0x1000,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub_page) {
        stub_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!stub_page) {
        std::cerr << "  [WARN] LCSFreeAim failed to allocate guest stub page.\n";
        return false;
    }

    auto* base = reinterpret_cast<uint8_t*>(stub_page);
    const uint64_t base_addr = reinterpret_cast<uint64_t>(stub_page);
    size_t cursor = 0;

    // Reserve 16-byte scratch CVector for camera-ray bullet tests inside ProcessLineOfSight.
    m_scratch_start_va = base_addr + cursor;
    std::memset(base + cursor, 0, 16);
    cursor += 16;

    // Reserve 16-byte scratch buffer (float beta, float vert) for FollowCar_SA mid-function hook.
    m_car_scratch_va = base_addr + cursor;
    std::memset(base + cursor, 0, 16);
    cursor += 16;

    auto patch_bytes = [&](uint64_t guest_va, const void* data, size_t sz) {
        auto* dst = reinterpret_cast<void*>(guest_va);
#if defined(_WIN32)
        DWORD old_prot = 0;
        VirtualProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old_prot);
#endif
        std::memcpy(dst, data, sz);
#if defined(_WIN32)
        VirtualProtect(dst, sz, old_prot, &old_prot);
#endif
        cpu.invalidate_cache(guest_va, sz);
    };

    auto patch_got = [&](uint64_t got_rva, uint64_t new_target, const char* sym) -> uint64_t {
        const uint64_t slot_va = m_load_bias + got_rva;
        auto* slot = reinterpret_cast<uint64_t*>(slot_va);
        uint64_t orig = *slot;
        if (!orig && sym) orig = image_game.find_symbol(sym);
        patch_bytes(slot_va, &new_target, sizeof(uint64_t));
        return orig;
    };

    auto& hle = lcs::cpu::HLEManager::instance();

    // Helper A: pre_override stub (returns 0 -> invoke orig; returns N!=0 -> return N-1 directly).
    auto install_pre_override = [&](uint64_t got_rva, const char* sym, const char* svc_name,
                                    lcs::cpu::HLEFunction pre_cb) {
        const uint64_t cb_va = hle.register_function(svc_name, std::move(pre_cb));
        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_va = patch_got(got_rva, stub_va, sym);
        const uint32_t words[18] = {
            0xd100c3ffu, // 0x00: sub sp, sp, #0x30
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90107e0u, // 0x0c: stp x0, x1, [sp, #0x10]
            0xa9020fe2u, // 0x10: stp x2, x3, [sp, #0x20]
            ldr_x16_rel(0x14, 0x50),
            0xd63f0200u, // 0x18: blr x16
            0xb40000a0u, // 0x1c: cbz x0, +0x14 (0x30)
            0xd1000400u, // 0x20: sub x0, x0, #1
            0xa9407bfdu, // 0x24: ldp x29, x30, [sp]
            0x9100c3ffu, // 0x28: add sp, sp, #0x30
            0xd65f03c0u, // 0x2c: ret
            0xa94107e0u, // 0x30: ldp x0, x1, [sp, #0x10]
            0xa9420fe2u, // 0x34: ldp x2, x3, [sp, #0x20]
            0xa9407bfdu, // 0x38: ldp x29, x30, [sp]
            0x9100c3ffu, // 0x3c: add sp, sp, #0x30
            ldr_x16_rel(0x40, 0x48),
            0xd61f0200u, // 0x44: br x16
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 18);
        lits[0] = orig_va;
        lits[1] = cb_va;
        cursor += sizeof(words) + 16;
        cursor = (cursor + 15) & ~size_t(15);
    };

    // Helper B: pre_post stub (pre returns 1 -> skip orig & return; 0 -> call orig + post).
    auto install_pre_post = [&](uint64_t got_rva, const char* sym, const char* pre_svc,
                                lcs::cpu::HLEFunction pre_cb, const char* post_svc,
                                lcs::cpu::HLEFunction post_cb) {
        const uint64_t pre_va = hle.register_function(pre_svc, std::move(pre_cb));
        const uint64_t post_va = hle.register_function(post_svc, std::move(post_cb));
        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_va = patch_got(got_rva, stub_va, sym);
        const uint32_t words[24] = {
            0xd10103ffu, // 0x00: sub sp, sp, #0x40
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xa90207e0u, // 0x10: stp x0, x1, [sp, #0x20]
            0xaa0003f3u, // 0x14: mov x19, x0
            0xaa0103f4u, // 0x18: mov x20, x1
            ldr_x16_rel(0x1c, 0x68),
            0xd63f0200u, // 0x20: blr x16
            0xb5000160u, // 0x24: cbnz x0, +0x2c (0x50)
            0xa94207e0u, // 0x28: ldp x0, x1, [sp, #0x20]
            ldr_x16_rel(0x2c, 0x60),
            0xd63f0200u, // 0x30: blr x16
            0xf9001be0u, // 0x34: str x0, [sp, #0x30]
            0xaa1303e0u, // 0x38: mov x0, x19
            0xaa1403e1u, // 0x3c: mov x1, x20
            0xf9401be2u, // 0x40: ldr x2, [sp, #0x30]
            ldr_x16_rel(0x44, 0x70),
            0xd63f0200u, // 0x48: blr x16
            0xf9401be0u, // 0x4c: ldr x0, [sp, #0x30]
            0xa94153f3u, // 0x50: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x54: ldp x29, x30, [sp]
            0x910103ffu, // 0x58: add sp, sp, #0x40
            0xd65f03c0u, // 0x5c: ret
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 24);
        lits[0] = orig_va;
        lits[1] = pre_va;
        lits[2] = post_va;
        cursor += sizeof(words) + 24;
        cursor = (cursor + 15) & ~size_t(15);
    };

    auto exit_free_aim = [this](uint64_t ped) {
        if (m_active) {
            // FollowPed_SA uses Front = -cos/sin(Beta), which is pi radians opposite AimWeapon m_yaw.
            m_handback_yaw = wrap_2pi(m_yaw + kPi);
            m_handback_frames = 4;
            m_active = false;
        }
        m_pending_turn = 0.0f;
        m_pending_tilt = 0.0f;
        if (ped) {
            *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 0;
            *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = 0;
        }
    };

    auto apply_active_free_aim = [this](uint64_t ped) {
        if (!ped) return;
        const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
        if (!dummy) return;

        const uint64_t cam = get_active_cam();
        if (!m_active) {
            // Start aiming straight where the active camera currently looks (level-clamped pitch).
            if (cam) {
                const float fx = *reinterpret_cast<const float*>(cam + kCamFrontOffset);
                const float fy = *reinterpret_cast<const float*>(cam + kCamFrontOffset + 4);
                const float fz = *reinterpret_cast<const float*>(cam + kCamFrontOffset + 8);
                m_yaw = std::atan2(fy, fx);
                m_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kStartPitchLimit, kStartPitchLimit);
            } else {
                const float rot = *reinterpret_cast<const float*>(ped + kPedCurRotOffset);
                m_yaw = wrap_pi(rot + (kPi * 0.5f));
                m_pitch = 0.0f;
            }
            m_active = true;
            m_handback_frames = 0;
        }

        const float turn = m_pending_turn - (m_stick_rx * kStickRateRadPerSec * m_dt_sec);
        const float tilt = m_pending_tilt - (m_stick_ry * kStickRateRadPerSec * m_dt_sec);
        m_pending_turn = 0.0f;
        m_pending_tilt = 0.0f;

        m_yaw = wrap_pi(m_yaw + turn);
        m_pitch = std::clamp(m_pitch + tilt, -kPitchLimit, kPitchLimit);

        const float px = *reinterpret_cast<const float*>(ped + kPedPosOffset);
        const float py = *reinterpret_cast<const float*>(ped + kPedPosOffset + 4);
        const float pz = *reinterpret_cast<const float*>(ped + kPedPosOffset + 8);
        const float cp = std::cos(m_pitch);
        const float dx = cp * std::cos(m_yaw);
        const float dy = cp * std::sin(m_yaw);
        const float dz = std::sin(m_pitch);

        auto* dpos = reinterpret_cast<float*>(dummy + kPedPosOffset);
        dpos[0] = px + dx * kDummyDistance;
        dpos[1] = py + dy * kDummyDistance;
        dpos[2] = pz + kPivotHeight + dz * kDummyDistance;

        // Keep player stationary while Free Aim is active and lock facing to reticle yaw.
        *reinterpret_cast<float*>(ped + kPedMoveBlendRatioOffset) = 0.0f;
        *reinterpret_cast<float*>(ped + kPedAnimMoveDeltaOffset) = 0.0f;
        *reinterpret_cast<float*>(ped + kPedAnimMoveDeltaOffset + 4ULL) = 0.0f;
        *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 1;
        *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = dummy;
        const float gta_heading = wrap_pi(m_yaw - (kPi * 0.5f));
        *reinterpret_cast<float*>(ped + kPedCurRotOffset) = gta_heading;
        *reinterpret_cast<float*>(ped + kPedDestRotOffset) = gta_heading;

        // Keep gCrossHair active on the dummy so CWeaponEffects::Render draws the reticle.
        const uint64_t ch = m_load_bias + kCrossHairOffset;
        *reinterpret_cast<uint8_t*>(ch) = 1;
        auto* ch_pos = reinterpret_cast<float*>(ch + 0x4ULL);
        ch_pos[0] = dpos[0];
        ch_pos[1] = dpos[1];
        ch_pos[2] = dpos[2];
        *reinterpret_cast<float*>(ch + 0x10ULL) = 1.0f;
        *reinterpret_cast<float*>(ch + 0x14ULL) = 0.8f;
        *reinterpret_cast<uint32_t*>(ch + 0x20ULL) = 0xffffffffu;
        *reinterpret_cast<uint64_t*>(ch + 0x28ULL) = dummy;

        if (cam) {
            *reinterpret_cast<uint8_t*>(cam + kCamResetStaticsOffset) = 1;
            *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = m_pitch;
            *reinterpret_cast<float*>(cam + kCamBetaOffset) = wrap_2pi(m_yaw);
        }
    };

    // 1. CPad::EnterFreeAim(): auto-enter LCS's free-aim dummy whenever Free Aim is enabled and aiming a gun.
    install_pre_override(kGotEnterFreeAim, "_ZN4CPad12EnterFreeAimEv", "__aim_enter",
        [this](const uint64_t*) -> uint64_t {
            if (!m_enabled || !m_want_aim) return 0;
            const uint64_t ped = get_player_ped();
            if (!is_player_on_foot_with_gun(ped)) return 0;
            return 2; // override return = 1 (true)
        });

    // 1b. Classic PS2 & PC GTA camera switching: V (KB) / SELECT (Controller) ONLY, zero D-Pad interference.
    //     Note from CamControl (0x324284 / 0x324604 / 0x32469c):
    //       - In vehicles (x10==4): CamControl only checks CycleCameraModeUpJustDown (steps m_fCarZoomIndicator).
    //       - On foot     (x10==6): UpJustDown decrements m_fPedZoomIndicator and clamps at 1.0, whereas
    //                               CycleCameraModeDownJustDown increments cleanly and wraps 1 -> 2 -> 3 -> 1.
    const uint64_t the_camera_addr = m_load_bias + 0x850af0ULL;
    auto check_select_cam_just_down = [this, the_camera_addr](const uint64_t pad, bool want_in_vehicle) -> uint64_t {
        if (!pad || m_in_menu || m_in_cutscene || m_active) return 1; // return false
        if (*reinterpret_cast<const uint16_t*>(pad + 0xa2ULL) != 0) return 1; // DisablePlayerControls
        if (*reinterpret_cast<const uint16_t*>(pad + 0x9eULL) > 3) return 1;
        const uint64_t ped = get_player_ped();
        const bool in_veh = (ped != 0 && *reinterpret_cast<const uint8_t*>(ped + kPedInVehicleFlag) != 0);
        if (in_veh != want_in_vehicle) return 1;
        const bool cur_sel = (*reinterpret_cast<const uint16_t*>(pad + 0x24ULL) != 0);
        const bool old_sel = (*reinterpret_cast<const uint16_t*>(pad + 0x58ULL) != 0);
        if (cur_sel && !old_sel) {
            m_car_cam_hold_timer = 0.0f;
            m_handback_frames = 0;
            if (!want_in_vehicle) {
                // CCamera::CamControl at 0x3246d0..0x324720 increments [cam+0x164] by +1 and clamps >3.0f to 3.0f
                // instead of wrapping; reset >=2.5f to 0.0f so +1.0f cleanly cycles 1 -> 2 -> 3 -> 1.
                float* ped_zoom = reinterpret_cast<float*>(the_camera_addr + 0x164ULL);
                if (*ped_zoom >= 2.5f) {
                    *ped_zoom = 0.0f;
                }
            }
            return 2; // return true
        }
        return 1; // return false
    };
    install_pre_override(kGotCycleCamUp, "_ZN4CPad25CycleCameraModeUpJustDownEv", "__pc_cycle_cam_up",
        [check_select_cam_just_down](const uint64_t* args) -> uint64_t {
            return check_select_cam_just_down(args[0], true);
        });
    install_pre_override(kGotCycleCamDown, "_ZN4CPad27CycleCameraModeDownJustDownEv", "__pc_cycle_cam_down",
        [check_select_cam_just_down](const uint64_t* args) -> uint64_t {
            return check_select_cam_just_down(args[0], false);
        });

    // 2. CPlayerPed::FindNextWeaponLockOnTarget(): suppress ped auto-lock when Free Aim is enabled with a gun.
    install_pre_override(kGotFindNextTarget, "_ZN10CPlayerPed26FindNextWeaponLockOnTargetEP7CEntityb", "__aim_no_lock",
        [this](const uint64_t* args) -> uint64_t {
            if (!m_enabled) return 0;
            const uint64_t ped = get_player_ped();
            if (args[0] == ped && is_player_on_foot_with_gun(ped)) {
                return 1; // override return = 0 (nullptr)
            }
            return 0;
        });

    // 3. CPed::SetWeaponLockOnTarget(): keep target locked to the free-aim dummy instead of locking onto peds.
    install_pre_override(kGotSetLockOnTarget, "_ZN4CPed21SetWeaponLockOnTargetEP7CEntity", "__aim_set_tgt",
        [this](const uint64_t* args) -> uint64_t {
            if (!m_enabled) return 0;
            const uint64_t ped = get_player_ped();
            if (args[0] != ped || !is_player_on_foot_with_gun(ped)) return 0;
            const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
            if (args[1] != 0 && args[1] != dummy) {
                return 1; // drop non-dummy ped target
            }
            return 0;
        });

    // 4. CPlayerPed::PlayerControlZelda(): keep player stationary while Free Aim is held (including
    //    during PEDSTATE_ATTACK when firing with LMB + RMB held), and apply 1:1 reticle aim afterward.
    install_pre_post(kGotPlayerZelda, "_ZN10CPlayerPed18PlayerControlZeldaEP4CPad",
        "__aim_zelda_pre",
        [this, exit_free_aim](const uint64_t* args) -> uint64_t {
            m_zelda_suspended_free_aim = false;
            m_has_saved_pad = false;
            const uint64_t ped = get_player_ped();
            if (!ped || args[0] != ped) return 0;

            if (!m_enabled || !m_want_aim || !is_player_on_foot_with_gun(ped)) {
                if (m_active || *reinterpret_cast<const uint8_t*>(ped + kPedFreeAimActive) != 0) {
                    exit_free_aim(ped);
                }
                return 0;
            }
            const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
            if (dummy && (*reinterpret_cast<const uint8_t*>(ped + kPedFreeAimActive) != 0 || m_active)) {
                *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 0;
                *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = 0;
                m_zelda_suspended_free_aim = true;
                if (args[1]) {
                    m_saved_pad_lx = *reinterpret_cast<const int16_t*>(args[1] + 0x2ULL);
                    m_saved_pad_ly = *reinterpret_cast<const int16_t*>(args[1] + 0x4ULL);
                    *reinterpret_cast<int16_t*>(args[1] + 0x2ULL) = 0;
                    *reinterpret_cast<int16_t*>(args[1] + 0x4ULL) = 0;
                    m_has_saved_pad = true;
                }
            }
            return 0;
        },
        "__aim_zelda_post",
        [this, exit_free_aim, apply_active_free_aim](const uint64_t* args) -> uint64_t {
            if (m_has_saved_pad && args[1]) {
                *reinterpret_cast<int16_t*>(args[1] + 0x2ULL) = m_saved_pad_lx;
                *reinterpret_cast<int16_t*>(args[1] + 0x4ULL) = m_saved_pad_ly;
            }
            m_has_saved_pad = false;
            if (!m_zelda_suspended_free_aim) return 0;
            m_zelda_suspended_free_aim = false;
            const uint64_t ped = args[0];
            if (!ped) return 0;
            if (!m_enabled || !m_want_aim || !is_player_on_foot_with_gun(ped)) {
                exit_free_aim(ped);
                return 0;
            }
            apply_active_free_aim(ped);
            return 0;
        });

    // 4b. Restore Free Aim state before CPlayerPed::ProcessPlayerWeapon() runs (0x45bab0 -> 0x45db54) so targeting/firing
    //     sees m_bInFreeAim == 1 & dummy intact cleanly across frames.
    install_pre_post(kGotProcessWeapon, "_ZN10CPlayerPed19ProcessPlayerWeaponEP4CPad",
        "__aim_weap_pre",
        [this, exit_free_aim](const uint64_t* args) -> uint64_t {
            if (!m_zelda_suspended_free_aim) return 0;
            m_zelda_suspended_free_aim = false;
            const uint64_t ped = args[0];
            if (!ped) return 0;
            if (!m_enabled || !m_want_aim || !is_player_on_foot_with_gun(ped)) {
                exit_free_aim(ped);
                return 0;
            }
            const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
            *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 1;
            *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = dummy;
            return 0;
        },
        "__aim_weap_post",
        [](const uint64_t*) -> uint64_t { return 0; });

    // 5. CPlayerPed::PlayerControlFreeAim(): run native PlayerControlFreeAim so the player stands still,
    //    then drive 1:1 mouse/right-stick dummy placement, heading, camera angles & crosshair reticle.
    {
        const uint64_t orig_zelda_va = image_game.find_symbol("_ZN10CPlayerPed18PlayerControlZeldaEP4CPad");
        const uint64_t pre_va = hle.register_function("__aim_ctrl_pre",
            [this, exit_free_aim](const uint64_t* args) -> uint64_t {
                if (!m_enabled) {
                    if (m_active) exit_free_aim(get_player_ped());
                    return 0; // pass through to orig_PlayerControlFreeAim
                }
                const uint64_t ped = get_player_ped();
                if (args[0] != ped || !ped) return 0;

                const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
                if (!m_want_aim || !is_player_on_foot_with_gun(ped) || !dummy) {
                    exit_free_aim(ped);
                    return 1; // exit free aim cleanly
                }

                // Keep Free Aim active so native PlayerControlFreeAim keeps the ped standing still.
                *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 1;
                *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = dummy;
                return 0; // invoke orig_PlayerControlFreeAim -> then __aim_ctrl_post
            });

        const uint64_t post_va = hle.register_function("__aim_ctrl_post",
            [apply_active_free_aim](const uint64_t* args) -> uint64_t {
                apply_active_free_aim(args[0]);
                return 0;
            });

        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_free_aim = patch_got(kGotPlayerFreeAim, stub_va, "_ZN10CPlayerPed20PlayerControlFreeAimEP4CPad");
        const uint32_t words[30] = {
            0xd10103ffu, // 0x00: sub sp, sp, #0x40
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90153f3u, // 0x0c: stp x19, x20, [sp, #0x10]
            0xa90207e0u, // 0x10: stp x0, x1, [sp, #0x20]
            0xaa0003f3u, // 0x14: mov x19, x0
            0xaa0103f4u, // 0x18: mov x20, x1
            ldr_x16_rel(0x1c, 0x80),
            0xd63f0200u, // 0x20: blr x16 (pre_cb)
            0xf100041fu, // 0x24: cmp x0, #1
            0x54000200u, // 0x28: b.eq +0x40 (0x68)
            0xf100081fu, // 0x2c: cmp x0, #2
            0x54000080u, // 0x30: b.eq +0x10 (0x40)
            0xa94207e0u, // 0x34: ldp x0, x1, [sp, #0x20]
            ldr_x16_rel(0x38, 0x78),
            0x14000003u, // 0x3c: b +0xc (0x48)
            0xa94207e0u, // 0x40: ldp x0, x1, [sp, #0x20]
            ldr_x16_rel(0x44, 0x90),
            0xd63f0200u, // 0x48: blr x16 (orig_free_aim or orig_zelda)
            0xf9001be0u, // 0x4c: str x0, [sp, #0x30]
            0xaa1303e0u, // 0x50: mov x0, x19
            0xaa1403e1u, // 0x54: mov x1, x20
            0xf9401be2u, // 0x58: ldr x2, [sp, #0x30]
            ldr_x16_rel(0x5c, 0x88),
            0xd63f0200u, // 0x60: blr x16 (post_cb)
            0xf9401be0u, // 0x64: ldr x0, [sp, #0x30]
            0xa94153f3u, // 0x68: ldp x19, x20, [sp, #0x10]
            0xa9407bfdu, // 0x6c: ldp x29, x30, [sp]
            0x910103ffu, // 0x70: add sp, sp, #0x40
            0xd65f03c0u, // 0x74: ret
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 30);
        lits[0] = orig_free_aim;
        lits[1] = pre_va;
        lits[2] = post_va;
        lits[3] = orig_zelda_va;
        cursor += sizeof(words) + 32;
        cursor = (cursor + 15) & ~size_t(15);
    }

    // 5b. CPlayerPed::ProcessControl(): keep player stationary and facing the reticle while Free Aim is active.
    install_pre_override(kVtblPlayerProcessCtrl, "_ZN10CPlayerPed14ProcessControlEv", "__aim_ped_ctrl",
        [this](const uint64_t* args) -> uint64_t {
            if (!m_enabled || !m_active) return 0;
            const uint64_t ped = get_player_ped();
            if (!ped || args[0] != ped) return 0;

            *reinterpret_cast<float*>(ped + kPedMoveBlendRatioOffset) = 0.0f;
            *reinterpret_cast<float*>(ped + kPedAnimMoveDeltaOffset) = 0.0f;
            *reinterpret_cast<float*>(ped + kPedAnimMoveDeltaOffset + 4ULL) = 0.0f;
            const float gta_heading = wrap_pi(m_yaw - (kPi * 0.5f));
            *reinterpret_cast<float*>(ped + kPedCurRotOffset) = gta_heading;
            *reinterpret_cast<float*>(ped + kPedDestRotOffset) = gta_heading;
            return 0; // continue to orig_ProcessControl
        });

    // 6. CCamera::Process():
    //    - In 1:1 Free Aim: keep AimWeapon cam locked to reticle pitch/yaw and hand back smoothly on exit.
    //    - Outside Free Aim: transiently set m_bUseMouse3rdPerson=1 during CCamera::Process (stops WASD FollowPed rotation)
    //      and clear it right after CCamera::Process so PlayerControlZelda keeps full 360-degree turning (no moonwalk).
    install_pre_post(kGotCameraProcess, "_ZN7CCamera7ProcessEv",
        "__aim_cam_pre",
        [this, exit_free_aim](const uint64_t*) -> uint64_t {
            m_forced_mouse_3rd = false;
            const uint64_t cam = get_active_cam();
            if (!cam) return 0;

            const uint64_t ped = get_player_ped();
            if (m_active) {
                if (!m_enabled || !m_want_aim || !is_player_on_foot_with_gun(ped)) {
                    exit_free_aim(ped);
                } else {
                    if (ped) {
                        const uint64_t dummy = *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy);
                        if (dummy) {
                            *reinterpret_cast<uint8_t*>(ped + kPedFreeAimActive) = 1;
                            *reinterpret_cast<uint64_t*>(ped + kPedTargetEntity) = dummy;
                        }
                    }
                    *reinterpret_cast<uint8_t*>(cam + kCamResetStaticsOffset) = 1;
                    *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = m_pitch;
                    *reinterpret_cast<float*>(cam + kCamBetaOffset) = wrap_2pi(m_yaw);
                    return 0;
                }
            } else if (ped && (!m_enabled || !m_want_aim) &&
                       *reinterpret_cast<const uint8_t*>(ped + kPedFreeAimActive) != 0) {
                exit_free_aim(ped);
            }

            if (m_in_menu || m_in_cutscene) {
                m_look_turn = 0.0f;
                m_look_tilt = 0.0f;
                m_car_cam_hold_timer = 0.0f;
                m_ped_wall_tail_timer = 0.0f;
                return 0;
            }

            const int16_t mode = *reinterpret_cast<const int16_t*>(cam + kCamModeOffset);
            const bool is_follow_ped = (mode == 4);
            const bool is_follow_car = (mode == 3 || mode == 18 || mode == 22);
            const bool is_first_person = (mode == 7 || mode == 8 || mode == 16 || mode == 45 || mode == 46);
            const bool using_right_stick = (std::abs(m_stick_rx) > 0.125f || std::abs(m_stick_ry) > 0.125f);

            // Force m_bUseMouse3rdPerson=1 ONLY when Mouse+KB is active (prevents FollowPed_SA camera
            // rotation while WASD strafing). Leave 0 when playing with a controller so native console
            // behind-ped follow camera and Left Stick movement feel completely natural!
            if (is_follow_ped && m_mouse_look_mode && !using_right_stick) {
                *reinterpret_cast<uint8_t*>(m_load_bias + kCamUseMouse3rdRva) = 1;
                m_forced_mouse_3rd = true;
            } else if (!is_follow_ped) {
                m_ped_wall_tail_timer = 0.0f;
            }

            if (m_handback_frames > 0 && is_follow_ped) {
                *reinterpret_cast<float*>(cam + kCamBetaOffset) = wrap_2pi(m_handback_yaw);
                *reinterpret_cast<float*>(cam + kCamBetaSpeedOffset) = 0.0f;
            }

            const float turn = m_look_turn;
            const float tilt = m_look_tilt;
            m_look_turn = 0.0f;
            m_look_tilt = 0.0f;

            if (is_follow_ped) {
                // Keep FollowPed_SA on its stick wall pull-in path (0x852728 >= 1) and clear
                // feeler beta accumulation (0x852724 = 0) so walking against a wall never skips
                // ProcessLineOfSight or pushes Source inside wall geometry.
                // Set LCS_FOOT_WALL=0 to disable for A/B diagnostics.
                static const bool s_foot_wall_fix = [] {
                    const char* e = std::getenv("LCS_FOOT_WALL");
                    return !(e && e[0] == '0');
                }();
                if (turn != 0.0f || tilt != 0.0f) {
                    m_ped_wall_tail_timer = 0.30f;
                } else if (m_ped_wall_tail_timer > 0.0f) {
                    m_ped_wall_tail_timer = std::max(0.0f, m_ped_wall_tail_timer - m_dt_sec);
                }
                if (s_foot_wall_fix) {
                    auto* wall_skip = reinterpret_cast<uint32_t*>(m_load_bias + kCamPedWallSkipRva);
                    if (*wall_skip == 0) {
                        *wall_skip = 1;
                    }
                    *reinterpret_cast<float*>(m_load_bias + kCamPedWallFeelerRva) = 0.0f;
                }
            }

            if (!is_follow_car ||
                using_right_stick ||
                *reinterpret_cast<const uint8_t*>(cam + kCamResetStaticsOffset) != 0 ||
                *reinterpret_cast<const uint32_t*>(cam + kCamCarLookDirOffset) != 3) {
                m_car_cam_hold_timer = 0.0f;
                if (is_follow_car) {
                    return 0;
                }
            } else {
                if (m_car_cam_hold_timer > 0.0f) {
                    m_car_cam_hold_timer = std::max(0.0f, m_car_cam_hold_timer - m_dt_sec);
                }
                if (turn != 0.0f || tilt != 0.0f) {
                    if (m_car_cam_hold_timer <= 0.0f) {
                        m_car_cam_beta = *reinterpret_cast<const float*>(cam + kCamBetaOffset);
                        m_car_cam_vert = *reinterpret_cast<const float*>(cam + kCamVertAngleOffset);
                    }
                    m_car_cam_beta = wrap_2pi(m_car_cam_beta + turn);
                    m_car_cam_vert = std::clamp(m_car_cam_vert + tilt, -0.35f, 0.45f);
                    m_car_cam_hold_timer = 1.25f;
                }
                if (m_car_cam_hold_timer > 0.0f) {
                    *reinterpret_cast<float*>(cam + kCamBetaOffset) = m_car_cam_beta;
                    *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = m_car_cam_vert;
                    *reinterpret_cast<float*>(cam + kCamBetaSpeedOffset) = 0.0f;
                    *reinterpret_cast<float*>(cam + kCamVertSpeedOffset) = 0.0f;
                }
                return 0;
            }

            if ((is_follow_ped || is_first_person) && (turn != 0.0f || tilt != 0.0f)) {
                const float vmin = is_first_person ? -1.15f : -0.75f;
                const float vmax = is_first_person ?  1.15f :  0.85f;
                const float new_beta = wrap_2pi(*reinterpret_cast<const float*>(cam + kCamBetaOffset) + turn);
                const float new_vert = std::clamp(*reinterpret_cast<const float*>(cam + kCamVertAngleOffset) + tilt, vmin, vmax);

                *reinterpret_cast<float*>(cam + kCamBetaOffset) = new_beta;
                *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = new_vert;
                *reinterpret_cast<float*>(cam + kCamBetaSpeedOffset) = 0.0f;
                *reinterpret_cast<float*>(cam + kCamVertSpeedOffset) = 0.0f;
            }
            return 0;
        },
        "__aim_cam_post",
        [this](const uint64_t*) -> uint64_t {
            if (m_forced_mouse_3rd) {
                *reinterpret_cast<uint8_t*>(m_load_bias + kCamUseMouse3rdRva) = 0;
                m_forced_mouse_3rd = false;
            }
            const uint64_t cam = get_active_cam();
            if (!cam) return 0;

            // Keep CTimeCycle's indoor CamNoRain/PlayerNoRain FarClip ramp (+0x2bdc) and
            // ExtraColour state consistent between FollowPed and AimWeapon inside interiors/safehouses.
            if (const uint64_t p_tc_ptr = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7f90a0ULL)) {
                if (const uint64_t tc = *reinterpret_cast<const uint64_t*>(p_tc_ptr)) {
                    const int32_t extra_on = *reinterpret_cast<const int32_t*>(tc + 0x2bccULL);
                    const uint32_t cull_player = *reinterpret_cast<const uint32_t*>(m_load_bias + 0x865d68ULL);
                    const uint32_t cull_cam = *reinterpret_cast<const uint32_t*>(m_load_bias + 0x865d64ULL);
                    if (extra_on != 0 || ((cull_player | cull_cam) & 0x88u) != 0) {
                        *reinterpret_cast<int32_t*>(tc + 0x2bdcULL) = 0x40;
                    }
                }
            }

            const int16_t mode = *reinterpret_cast<const int16_t*>(cam + kCamModeOffset);
            if (m_active && mode == 11) {
                *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = m_pitch;
                *reinterpret_cast<float*>(cam + kCamBetaOffset) = wrap_2pi(m_yaw);
                return 0;
            }
            if (m_handback_frames > 0) {
                if (mode == 4) {
                    *reinterpret_cast<float*>(cam + kCamBetaOffset) = wrap_2pi(m_handback_yaw);
                    *reinterpret_cast<float*>(cam + kCamBetaSpeedOffset) = 0.0f;
                    m_handback_frames = 0;
                } else {
                    --m_handback_frames;
                }
            }
            return 0;
        });

    // 7. CWorld::ProcessLineOfSight(): trace FireInstantHit bullets straight from the camera crosshair ray.
    {
        const uint64_t cb_va = hle.register_function("__aim_los_pre",
            [this](const uint64_t* args) -> uint64_t {
                if (!m_enabled || !m_active) return 0;
                const uint64_t lr = args[2];
                if (lr < m_fire_hit_start || lr >= m_fire_hit_end) return 0;

                const uint64_t ped = get_player_ped();
                const uint64_t dummy = ped ? *reinterpret_cast<const uint64_t*>(ped + kPedFreeAimDummy) : 0;
                const uint64_t cam = get_active_cam();
                if (!dummy || !cam || !args[0] || !args[1]) return 0;

                const auto* src = reinterpret_cast<const float*>(cam + kCamSourceOffset);
                const auto* dpos = reinterpret_cast<const float*>(dummy + kPedPosOffset);
                const auto* gun = reinterpret_cast<const float*>(args[0]);
                auto* end = reinterpret_cast<float*>(args[1]);

                float dir[3] = {dpos[0] - src[0], dpos[1] - src[1], dpos[2] - src[2]};
                const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const float range = std::sqrt(
                    (end[0] - gun[0]) * (end[0] - gun[0]) +
                    (end[1] - gun[1]) * (end[1] - gun[1]) +
                    (end[2] - gun[2]) * (end[2] - gun[2]));
                if (len <= 0.01f || range <= 0.01f) return 0;

                dir[0] /= len;
                dir[1] /= len;
                dir[2] /= len;
                const float along = std::max(0.0f,
                    (gun[0] - src[0]) * dir[0] +
                    (gun[1] - src[1]) * dir[1] +
                    (gun[2] - src[2]) * dir[2]);

                auto* new_start = reinterpret_cast<float*>(m_scratch_start_va);
                new_start[0] = src[0] + dir[0] * along;
                new_start[1] = src[1] + dir[1] * along;
                new_start[2] = src[2] + dir[2] * along;

                end[0] = new_start[0] + dir[0] * range;
                end[1] = new_start[1] + dir[1] * range;
                end[2] = new_start[2] + dir[2] * range;

                ++m_shot_rays;
                return m_scratch_start_va;
            });

        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_los = patch_got(kGotProcessLineOfSight, stub_va,
            "_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbbb");
        const uint32_t words[20] = {
            0xd10143ffu, // 0x00: sub sp, sp, #0x50
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90107e0u, // 0x0c: stp x0, x1, [sp, #0x10]
            0xa9020fe2u, // 0x10: stp x2, x3, [sp, #0x20]
            0xa90317e4u, // 0x14: stp x4, x5, [sp, #0x30]
            0xa9041fe6u, // 0x18: stp x6, x7, [sp, #0x40]
            0xaa1e03e2u, // 0x1c: mov x2, x30
            ldr_x16_rel(0x20, 0x58),
            0xd63f0200u, // 0x24: blr x16
            0xb4000040u, // 0x28: cbz x0, +8 (0x30)
            0xf9000be0u, // 0x2c: str x0, [sp, #0x10]
            0xa94107e0u, // 0x30: ldp x0, x1, [sp, #0x10]
            0xa9420fe2u, // 0x34: ldp x2, x3, [sp, #0x20]
            0xa94317e4u, // 0x38: ldp x4, x5, [sp, #0x30]
            0xa9441fe6u, // 0x3c: ldp x6, x7, [sp, #0x40]
            0xa9407bfdu, // 0x40: ldp x29, x30, [sp]
            0x910143ffu, // 0x44: add sp, sp, #0x50
            ldr_x16_rel(0x48, 0x50),
            0xd61f0200u, // 0x4c: br x16
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 20);
        lits[0] = orig_los;
        lits[1] = cb_va;
        cursor += sizeof(words) + 16;
        cursor = (cursor + 15) & ~size_t(15);
    }

    // 7b. CCam::DoGeometryAvoidance(): in FollowPed_SA (mode == 4), upgrade EFrontResolve from 1 to 2
    //     (the exact right-stick path at 0x31a04c) so pressing against a wall/corner never freezes
    //     Front to m_cvecLastGoodPos or fights mouse yaw.
    {
        const uint64_t cb_va = hle.register_function("__aim_geom_avoid",
            [this](const uint64_t* args) -> uint64_t {
                static const bool s_foot_wall_fix = [] {
                    const char* e = std::getenv("LCS_FOOT_WALL");
                    return !(e && e[0] == '0');
                }();
                const uint64_t resolve = args[2];
                if (!s_foot_wall_fix || resolve != 1) return resolve;
                const uint64_t cam = args[0];
                if (cam && cam == get_active_cam() &&
                    *reinterpret_cast<const int16_t*>(cam + kCamModeOffset) == 4) {
                    return 2; // EFrontResolve::LookAtTargetFromOriginal (stick path)
                }
                return resolve;
            });

        const uint64_t stub_va = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        const uint64_t orig_avoid = patch_got(kGotDoGeometryAvoidance, stub_va,
            "_ZN4CCam19DoGeometryAvoidanceERK7CVector13EFrontResolve");
        const uint32_t words[16] = {
            0xd100c3ffu, // 0x00: sub sp, sp, #0x30
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90107e0u, // 0x0c: stp x0, x1, [sp, #0x10]
            0xf90013e2u, // 0x10: str x2, [sp, #0x20]
            ldr_x16_rel(0x14, 0x48),
            0xd63f0200u, // 0x18: blr x16 (__aim_geom_avoid)
            0xaa0003e2u, // 0x1c: mov x2, x0
            0xa94107e0u, // 0x20: ldp x0, x1, [sp, #0x10]
            0xa9407bfdu, // 0x24: ldp x29, x30, [sp]
            0x9100c3ffu, // 0x28: add sp, sp, #0x30
            ldr_x16_rel(0x2c, 0x40),
            0xd61f0200u, // 0x30: br x16 (orig_DoGeometryAvoidance)
            0xd503201fu, // 0x34: nop
            0xd503201fu, // 0x38: nop
            0xd503201fu, // 0x3c: nop
        };
        std::memcpy(code, words, sizeof(words));
        auto* lits = reinterpret_cast<uint64_t*>(code + 16);
        lits[0] = orig_avoid;
        lits[1] = cb_va;
        cursor += sizeof(words) + 16;
        cursor = (cursor + 15) & ~size_t(15);
    }

    // 8. CCam::Process_FollowCar_SA(): inject held mouse Beta/VertAngle right at 0x318464
    //    before sincosf/GetVectorsReadyForRsl/Source/Front/ProcessLineOfSight run so FollowCar's
    //    auto-centering spring never fights or vibrates the mouse-aimed camera.
    {
        constexpr uint64_t kFollowCarPatchRva = 0x318464ULL;
        constexpr uint64_t kFollowCarRetRva   = 0x318474ULL;
        const uint64_t patch_va = m_load_bias + kFollowCarPatchRva;
        const uint32_t expected_insns[4] = {0x9102f3e0u, 0x9102e3e1u, 0xbd0aed20u, 0x9102f3f4u};
        uint32_t actual_insns[4]{};
        std::memcpy(actual_insns, reinterpret_cast<const void*>(patch_va), sizeof(actual_insns));
        if (std::memcmp(actual_insns, expected_insns, sizeof(expected_insns)) == 0) {
            const uint64_t cb_va = hle.register_function("__aim_car_cam_mid",
                [this](const uint64_t* args) -> uint64_t {
                    if (!m_enabled || m_in_menu || m_in_cutscene || m_car_cam_hold_timer <= 0.0f) {
                        return 0;
                    }
                    const uint64_t cam = args[0];
                    if (!cam || cam != get_active_cam()) return 0;
                    if (*reinterpret_cast<const uint32_t*>(cam + kCamCarLookDirOffset) != 3) return 0;

                    *reinterpret_cast<float*>(cam + kCamBetaOffset) = m_car_cam_beta;
                    *reinterpret_cast<float*>(cam + kCamVertAngleOffset) = m_car_cam_vert;
                    *reinterpret_cast<float*>(cam + kCamBetaSpeedOffset) = 0.0f;
                    *reinterpret_cast<float*>(cam + kCamVertSpeedOffset) = 0.0f;
                    *reinterpret_cast<float*>(m_load_bias + 0x811ae8ULL) = m_car_cam_vert;

                    auto* out_f = reinterpret_cast<float*>(m_car_scratch_va);
                    out_f[0] = m_car_cam_beta;
                    out_f[1] = m_car_cam_vert;
                    return 1;
                });

            const uint64_t stub_va = base_addr + cursor;
            auto* code = reinterpret_cast<uint32_t*>(base + cursor);
            const uint32_t words[24] = {
                0xd100c3ffu, // 0x00: sub sp, sp, #0x30
                0xa9007be9u, // 0x04: stp x9, x30, [sp]
                0xf9000beau, // 0x08: str x10, [sp, #0x10]
                0xfd000fe0u, // 0x0c: str d0, [sp, #0x18]
                0xaa1303e0u, // 0x10: mov x0, x19 (cam)
                ldr_x_rel(16, 0x14, 0x60),
                0xd63f0200u, // 0x18: blr x16 (__aim_car_cam_mid)
                0xb40000a0u, // 0x1c: cbz x0, +0x14 (0x30)
                ldr_x_rel(10, 0x20, 0x70),
                0xbd400140u, // 0x24: ldr s0, [x10]
                0xbd400548u, // 0x28: ldr s8, [x10, #4]
                0x14000002u, // 0x2c: b +8 (0x34)
                0xfd400fe0u, // 0x30: ldr d0, [sp, #0x18]
                0xa9407be9u, // 0x34: ldp x9, x30, [sp]
                0xf9400beau, // 0x38: ldr x10, [sp, #0x10]
                0x9100c3ffu, // 0x3c: add sp, sp, #0x30
                expected_insns[0], // 0x40: add x0, sp, #0xbc
                expected_insns[1], // 0x44: add x1, sp, #0xb8
                expected_insns[2], // 0x48: str s0, [x9, #0xaec]
                expected_insns[3], // 0x4c: add x20, sp, #0xbc
                ldr_x_rel(16, 0x50, 0x68),
                0xd61f0200u, // 0x54: br x16
                0xd503201fu, // 0x58: nop
                0xd503201fu, // 0x5c: nop
            };
            std::memcpy(code, words, sizeof(words));
            auto* lits = reinterpret_cast<uint64_t*>(code + 24);
            lits[0] = cb_va;
            lits[1] = m_load_bias + kFollowCarRetRva;
            lits[2] = m_car_scratch_va;
            cursor += sizeof(words) + 24;
            cursor = (cursor + 15) & ~size_t(15);

            struct {
                uint32_t ldr_x16;
                uint32_t br_x16;
                uint64_t target;
            } detour{0x58000050u, 0xd61f0200u, stub_va};
            patch_bytes(patch_va, &detour, sizeof(detour));
        } else {
            std::cerr << "  [WARN] FollowCar_SA patch signature mismatch; vehicle mouse hold hook skipped.\n";
        }
    }

    cpu.invalidate_cache(base_addr, 0x1000);

    // 9. CPad::UpdatePads() post-hook:
    //    - Enforce modern trigger isolation: controller L2/R2 NEVER write CPad LeftShoulder2 (+0x0c)
    //      or RightShoulder2 (+0x10) (LookLeft / LookRight / LookBehind) unless PS2 classic is selected.
    //      Keyboard Q / E / LookBehind keep +0x0c / +0x10 working cleanly.
    //    - Map modern LT/RT (L2/R2) to R1/Target (+0x0e) & Circle/Fire (+0x2c) on foot,
    //      and Cross/Accelerate (+0x2a) & Square/Brake (+0x26) in vehicles.
    if (const uint64_t pad0_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7f9280ULL)) {
        install_pre_post(kGotUpdatePads, "_ZN4CPad10UpdatePadsEv",
            "__pad_trig_pre", [](const uint64_t*) -> uint64_t { return 0; },
            "__pad_trig_post",
            [this, pad0_addr](const uint64_t*) -> uint64_t {
                const uint16_t disabled = *reinterpret_cast<const uint16_t*>(pad0_addr + 0xa2ULL);
                if (!m_classic_ps2_triggers) {
                    const bool allow = (!m_in_menu && !m_in_cutscene && disabled == 0);
                    const uint16_t l2_val = (allow && (m_kb_look_left || (m_in_vehicle && m_kb_look_behind))) ? 0xffu : 0u;
                    const uint16_t r2_val = (allow && (m_kb_look_right || m_kb_look_behind)) ? 0xffu : 0u;
                    *reinterpret_cast<uint16_t*>(pad0_addr + 0x0cULL) = l2_val;
                    *reinterpret_cast<uint16_t*>(pad0_addr + 0x10ULL) = r2_val;
                }
                if (m_in_menu || m_in_cutscene || disabled != 0) return 0;
                if (m_classic_ps2_triggers) return 0;
                const bool lt = (m_trigger_l2 > 0.35f);
                const bool rt = (m_trigger_r2 > 0.35f);
                if (!m_in_vehicle) {
                    if (lt) *reinterpret_cast<uint16_t*>(pad0_addr + 0x0eULL) = 0xffu; // Aim / Lock-on / Free Aim
                    if (rt) *reinterpret_cast<uint16_t*>(pad0_addr + 0x2cULL) = 0xffu; // Attack / Fire
                } else {
                    if (rt) *reinterpret_cast<uint16_t*>(pad0_addr + 0x2aULL) = 0xffu; // Accelerate
                    if (lt) *reinterpret_cast<uint16_t*>(pad0_addr + 0x26ULL) = 0xffu; // Brake / Reverse
                }
                return 0;
            });
    }

    cpu.invalidate_cache(base_addr, 0x1000);
    m_installed = true;
    std::cout << "  - Installed VCS-style Free Aim hooks (1:1 reticle aim, strafe, camera-ray shots).\n";
    return true;
}

} // namespace lcs::game
