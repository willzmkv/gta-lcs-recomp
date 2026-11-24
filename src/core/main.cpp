#include <iostream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <atomic>
#include <string>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>
#include <memory>
#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#endif
#include "core/config.h"
#include "cpu/elf_loader.h"
#include "cpu/dynarmic_core.h"
#include "cpu/callbacks.h"
#include "os/jni_bridge.h"
#include "os/bionic_libc.h"
#include "graphics/window.h"
#include "graphics/render_bridge.h"
#if defined(LCS_ENABLE_D3D12)
#include "graphics/r_d3d12/d3d12_backend.h"
#endif
#include "input/input_system.h"
#include "input/pc_controls.h"
#include "input/mouse_aim.h"
#include "input/gamepad.h"
#include "game/lcs_free_aim.h"
#include "game/lcs_distance.h"
#include "game/lcs_memory.h"
#include "game/lcs_physics_timing.h"
#include "game/lcs_subtitles.h"
#include "game/lcs_prompts.h"
#include "game/lcs_ps2_effects.h"
#include "game/lcs_trainer.h"
#include "game/lcs_world_hd.h"
#include "settings/settings.h"
#include "ui/global_menu.h"
#include "ui/imgui_overlay.h"
#include "audio/audio_system.h"
#include "audio/openal_bridge.h"
#include "vfs/logical_fs.h"
#include "vfs/mod_loader.h"
#include "vfs/wad_archive.h"

static lcs::cpu::DynarmicCore* s_active_cpu = nullptr;
static uint64_t s_logical_fs_dot_va = 0;

#if defined(LCS_PLATFORM_WINDOWS)
static bool IsFatalHardwareException(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_IN_PAGE_ERROR:
            return true;
        default:
            return false;
    }
}

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    auto rec = ep->ExceptionRecord;
    // Allow internal OS / DXGI / driver try-catch C++ exceptions and debug signals to propagate normally.
    if (!IsFatalHardwareException(rec->ExceptionCode)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::cout << "\n[CRASH TRAP] Exception Code: 0x" << std::hex << rec->ExceptionCode
              << " at host address 0x" << rec->ExceptionAddress << std::dec << "\n" << std::flush;
    std::cerr << "\n[CRASH TRAP] Exception Code: 0x" << std::hex << rec->ExceptionCode
              << " at host address 0x" << rec->ExceptionAddress << std::dec << "\n" << std::flush;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(rec->ExceptionAddress, &mbi, sizeof(mbi))) {
        char mod_name[MAX_PATH] = {0};
        GetModuleFileNameA(reinterpret_cast<HMODULE>(mbi.AllocationBase), mod_name, sizeof(mod_name));
        uintptr_t rva = reinterpret_cast<uintptr_t>(rec->ExceptionAddress) - reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        std::cerr << "  - Fault Module: " << mod_name << " (RVA: 0x" << std::hex << rva << std::dec << ")\n";
    }
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::cerr << "  - Access Type: " << (rec->ExceptionInformation[0] == 0 ? "READ" :
                                            rec->ExceptionInformation[0] == 1 ? "WRITE" : "EXECUTE")
                  << " at address 0x" << std::hex << rec->ExceptionInformation[1] << std::dec << "\n";
    }
    if (s_active_cpu) {
        std::cerr << "  - Guest PC: 0x" << std::hex << s_active_cpu->get_pc()
                  << " (SP: 0x" << s_active_cpu->get_sp() << ")\n";
        if (s_active_cpu->get_pc() >= 0x7f00200000ULL && s_active_cpu->get_pc() < 0x7f00300000ULL) {
            uint32_t id = static_cast<uint32_t>((s_active_cpu->get_pc() - 0x7f00200000ULL) / 8);
            std::cerr << "  - Trampoline: ID " << id << " [" << lcs::cpu::HLEManager::instance().get_symbol_name(id) << "]\n";
        }
        for (size_t i = 0; i < 31; i += 4) {
            std::cerr << "    X" << i << ": 0x" << s_active_cpu->get_reg(i) << " ";
            if (i + 1 < 31) std::cerr << "X" << (i+1) << ": 0x" << s_active_cpu->get_reg(i+1) << " ";
            if (i + 2 < 31) std::cerr << "X" << (i+2) << ": 0x" << s_active_cpu->get_reg(i+2) << " ";
            if (i + 3 < 31) std::cerr << "X" << (i+3) << ": 0x" << s_active_cpu->get_reg(i+3) << " ";
            std::cerr << "\n";
        }
        std::cerr << std::dec << std::flush;
        std::cout << std::flush;
    }

    if (!std::getenv("LCS_DIAG_VERIFY")) {
        char crash_msg[512];
        snprintf(crash_msg, sizeof(crash_msg),
                 "Fatal hardware exception (0x%08X) at address 0x%p.\n\nPlease check the console or log for details.",
                 static_cast<unsigned int>(rec->ExceptionCode), rec->ExceptionAddress);
        MessageBoxA(nullptr, crash_msg, "GTA: Liberty City Stories - Fatal Error", MB_ICONERROR | MB_OK);
    }

    ExitProcess(1);
}
#endif

// Adds PC Display options (FULLSCREEN, V-SYNC, MAX FPS, RESOLUTION) into FEB_DIS and syncs with host & lcs.conf.
static void setup_pc_display_menu_options(const lcs::cpu::ElfImage& image_game,
                                          lcs::cpu::DynarmicCore& cpu,
                                          bool headless) {
    struct GuestMenuEntry {
        int32_t action;
        char gxt[8];
        int32_t target;
        int32_t align;
    };
    static_assert(sizeof(GuestMenuEntry) == 20);

    const uint64_t load_bias = image_game.get_load_bias();
    const uint64_t a_screens = image_game.find_symbol("aScreens");
    const uint64_t femm = image_game.find_symbol("FrontEndMenuManager");
    const uint64_t vsd = image_game.find_symbol("_ZN12CMenuManager16m_PrefsVsyncDispE");
    const uint64_t vsync_sym = image_game.find_symbol("_ZN12CMenuManager12m_PrefsVsyncE");
    const uint64_t frame_lim = image_game.find_symbol("_ZN12CMenuManager19m_PrefsFrameLimiterE");
    const uint64_t bri_sym = image_game.find_symbol("_ZN12CMenuManager17m_PrefsBrightnessE");
    if (!a_screens || !femm || !vsd || !vsync_sym || !frame_lim || !bri_sym) {
        std::cerr << "  [WARN] Display menu PC injection skipped (missing symbols).\n";
        return;
    }

    // Validate expected FEB_DIS (aScreens[6]) layout before inserting before Restore Defaults / Back.
    const uint64_t scr6_entries_addr = a_screens + 0x34c8 + 0x34;
    auto* entries = reinterpret_cast<GuestMenuEntry*>(scr6_entries_addr);
    if (entries[6].action != 102 || std::strncmp(entries[6].gxt, "FEMP_R", 6) != 0 ||
        entries[7].action != 2 || std::strncmp(entries[7].gxt, "FEDS_TB", 7) != 0 ||
        entries[8].action != 0) {
        std::cerr << "  [WARN] Unexpected FEB_DIS menu layout; PC Display options not injected.\n";
        return;
    }

    // Shift Restore Defaults [6] -> [12], Back [7] -> [13], and existing options [1..5] -> [3..7].
    // Insert CONTRAST [1] (action 88) and SATURATION [2] (action 140) right below BRIGHTNESS [0] (action 11),
    // and insert 4 PC display entries [8..11] (Fullscreen, V-Sync, Max FPS, Resolution).
    entries[13] = entries[7];
    entries[12] = entries[6];
    for (int i = 5; i >= 1; --i) {
        entries[i + 2] = entries[i];
    }
    auto set_entry_on = [&](GuestMenuEntry* target_entries, size_t idx, int32_t action, const char* gxt_key, int32_t align) {
        std::memset(&target_entries[idx], 0, sizeof(GuestMenuEntry));
        target_entries[idx].action = action;
        std::memcpy(target_entries[idx].gxt, gxt_key, std::min<size_t>(7, std::strlen(gxt_key)));
        target_entries[idx].align = align;
    };
    set_entry_on(entries, 1, 88,  "FED_CON", 6); // Contrast (native graphical slider)
    set_entry_on(entries, 2, 140, "FED_SAT", 6); // Saturation (native graphical slider)
    set_entry_on(entries, 8, 6,   "FED_FUL", 6); // Fullscreen (m_PrefsVsyncDisp)
    set_entry_on(entries, 9, 7,   "FED_VSY", 6); // V-Sync (m_PrefsFrameLimiter)
    set_entry_on(entries, 10, 142, "FED_FPS", 6); // Max FPS (custom multi-option)
    set_entry_on(entries, 11, 143, "FED_RES", 6); // Resolution (custom multi-option)
    std::memset(&entries[14], 0, sizeof(GuestMenuEntry));

    // Inject PC Mouse/Aim/FreeAim options before Restore Defaults / Back on both FEB_CON (scr[4]) and GAMSET (scr[70]).
    auto inject_pc_controls_entries = [&](GuestMenuEntry* target_entries, int32_t page_align) -> bool {
        if (target_entries[9].action == 0 &&
            target_entries[3].action == 102 && std::strncmp(target_entries[3].gxt, "FEMP_R", 6) == 0 &&
            target_entries[4].action == 2 && std::strncmp(target_entries[4].gxt, "FEDS_TB", 7) == 0 &&
            target_entries[5].action == 0) {
            // Compact layout (scr[70] GAMSET): FEMP_R at [3], FEDS_TB at [4] -> shift to [7],[8].
            // Actions 125, 126, 130, 142 skip sliders (0x52aae4 / >141), hit Draw @ 0x52a294, and route ProcessMultiMenuOptions cleanly.
            target_entries[8] = target_entries[4];
            target_entries[7] = target_entries[3];
            set_entry_on(target_entries, 3, 125, "FEC_MSE", page_align);
            set_entry_on(target_entries, 4, 126, "FEC_AMS", page_align);
            set_entry_on(target_entries, 5, 130, "FEC_INV", page_align);
            set_entry_on(target_entries, 6, 142, "FEC_AIM", page_align);
            std::memset(&target_entries[9], 0, sizeof(GuestMenuEntry));
            return true;
        }
        if (target_entries[8].action == 102 && std::strncmp(target_entries[8].gxt, "FEMP_R", 6) == 0 &&
            target_entries[9].action == 2 && std::strncmp(target_entries[9].gxt, "FEDS_TB", 7) == 0 &&
            target_entries[10].action == 0) {
            // Extended layout (scr[4] FEB_CON): FEMP_R at [8], FEDS_TB at [9] -> shift to [12],[13].
            target_entries[13] = target_entries[9];
            target_entries[12] = target_entries[8];
            set_entry_on(target_entries, 8, 125, "FEC_MSE", page_align);
            set_entry_on(target_entries, 9, 126, "FEC_AMS", page_align);
            set_entry_on(target_entries, 10, 130, "FEC_INV", page_align);
            set_entry_on(target_entries, 11, 142, "FEC_AIM", page_align);
            std::memset(&target_entries[14], 0, sizeof(GuestMenuEntry));
            return true;
        }
        return false;
    };

    const uint64_t scr4_entries_addr = a_screens + 4ULL * 0x8ccULL + 0x34ULL;
    const uint64_t scr70_entries_addr = a_screens + 70ULL * 0x8ccULL + 0x34ULL;
    const bool has_feb_con = inject_pc_controls_entries(reinterpret_cast<GuestMenuEntry*>(scr4_entries_addr), 4);
    const bool has_gamset = inject_pc_controls_entries(reinterpret_cast<GuestMenuEntry*>(scr70_entries_addr), 70);

    void* stub_page = nullptr;
#if defined(LCS_PLATFORM_WINDOWS)
    stub_page = VirtualAlloc(reinterpret_cast<void*>(0x7f00020000ULL), 0x1000,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub_page) {
        stub_page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!stub_page) {
        std::cerr << "  [WARN] Failed to allocate guest menu hook page.\n";
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(stub_page);
    const uint64_t base_addr = reinterpret_cast<uint64_t>(stub_page);
    size_t cursor = 0;

    auto alloc_utf16_const = [&](const char* ascii) -> uint64_t {
        const size_t len = std::strlen(ascii);
        const uint64_t addr = base_addr + cursor;
        auto* dst = reinterpret_cast<uint16_t*>(base + cursor);
        for (size_t i = 0; i < len; ++i) {
            dst[i] = static_cast<uint8_t>(ascii[i]);
        }
        dst[len] = 0;
        cursor = (cursor + (len + 1) * sizeof(uint16_t) + 15) & ~size_t(15);
        return addr;
    };

    const uint64_t lbl_con_addr = alloc_utf16_const("CONTRAST");
    const uint64_t lbl_sat_addr = alloc_utf16_const("SATURATION");
    const uint64_t lbl_ful_addr = alloc_utf16_const("FULLSCREEN");
    const uint64_t lbl_vsy_addr = alloc_utf16_const("V-SYNC");
    const uint64_t lbl_fps_addr = alloc_utf16_const("MAX FPS");
    const uint64_t lbl_res_addr = alloc_utf16_const("RESOLUTION");
    const uint64_t lbl_mse_addr = alloc_utf16_const("MOUSE SENSITIVITY");
    const uint64_t lbl_ams_addr = alloc_utf16_const("AIM SENSITIVITY");
    const uint64_t lbl_inv_addr = alloc_utf16_const("INVERT MOUSE Y");
    const uint64_t lbl_aim_addr = alloc_utf16_const("FREE AIM");

    // Store Contrast [0.0..1.0] and Saturation [0.0..1.0] floats in unused tail of aScreens[6] (0x81fd00, 0x81fd04).
    auto* con_slider_ptr = reinterpret_cast<float*>(load_bias + 0x81fd00ULL);
    auto* sat_slider_ptr = reinterpret_cast<float*>(load_bias + 0x81fd04ULL);
    auto* bri_slider_ptr = reinterpret_cast<int32_t*>(bri_sym);

    auto apply_color_grading_from_sliders = [bri_slider_ptr, con_slider_ptr, sat_slider_ptr]() {
        const float bri_raw = static_cast<float>(std::clamp(*bri_slider_ptr, 51, 511));
        const float con_norm = std::clamp(*con_slider_ptr, 0.0f, 1.0f);
        const float sat_norm = std::clamp(*sat_slider_ptr, 0.0f, 1.0f);
        // At default brightness 336 (62%), GPU brightness multiplier is 1.0f; range [51..511] maps smoothly to [0.68f..1.28f].
        const float bri_mul = 1.0f + ((bri_raw - 336.0f) / 460.0f) * 0.60f;
        // At default contrast 0.50 (50%), GPU contrast multiplier is 1.12f; range [0.0..1.0] maps to [0.80f..1.44f].
        const float con_mul = 0.80f + con_norm * 0.64f;
        // At default saturation 0.50 (50%), GPU saturation multiplier is 1.24f; range [0.0..1.0] maps to [0.60f..1.88f].
        const float sat_mul = 0.60f + sat_norm * 1.28f;
        lcs::graphics::RenderBridge::instance().set_color_grading(bri_mul, con_mul, sat_mul);
    };

    auto sync_color_sliders_from_config = [bri_slider_ptr, con_slider_ptr, sat_slider_ptr, apply_color_grading_from_sliders]() {
        const int bri_pct = std::clamp(lcs::core::g_config.brightness, 0, 100);
        *bri_slider_ptr = 51 + static_cast<int32_t>(std::lround((static_cast<float>(bri_pct) / 100.0f) * 460.0f));
        *con_slider_ptr = std::clamp(static_cast<float>(lcs::core::g_config.contrast) / 100.0f, 0.0f, 1.0f);
        *sat_slider_ptr = std::clamp(static_cast<float>(lcs::core::g_config.saturation) / 100.0f, 0.0f, 1.0f);
        apply_color_grading_from_sliders();
    };
    sync_color_sliders_from_config();

    constexpr size_t kValBufUnits = 32;
    auto alloc_val_buf = [&](uint64_t& out_addr) -> uint16_t* {
        out_addr = base_addr + cursor;
        auto* ptr = reinterpret_cast<uint16_t*>(base + cursor);
        cursor += kValBufUnits * sizeof(uint16_t);
        return ptr;
    };
    uint64_t fps_val_addr = 0, res_val_addr = 0;
    uint64_t mse_val_addr = 0, ams_val_addr = 0, inv_val_addr = 0, aim_val_addr = 0;
    auto* fps_val_buf = alloc_val_buf(fps_val_addr);
    auto* res_val_buf = alloc_val_buf(res_val_addr);
    auto* mse_val_buf = alloc_val_buf(mse_val_addr);
    auto* ams_val_buf = alloc_val_buf(ams_val_addr);
    auto* inv_val_buf = alloc_val_buf(inv_val_addr);
    auto* aim_val_buf = alloc_val_buf(aim_val_addr);

    auto write_utf16_buf = [](uint16_t* dst, const std::string& text) {
        const size_t n = std::min(text.size(), kValBufUnits - 1);
        for (size_t i = 0; i < n; ++i) {
            dst[i] = static_cast<uint8_t>(text[i]);
        }
        dst[n] = 0;
    };

    // Build available resolution choices bounded by current monitor dimensions.
    uint32_t max_mon_w = 1920;
    uint32_t max_mon_h = 1080;
    if (!headless) {
        lcs::graphics::get_desktop_resolution(max_mon_w, max_mon_h);
    }
    struct ResPreset { uint32_t w; uint32_t h; };
    std::vector<ResPreset> res_choices;
    const ResPreset kStandardPresets[] = {
        {1024, 768}, {1280, 720}, {1366, 768}, {1600, 900},
        {1920, 810}, {1920, 1080}, {2560, 1080}, {2560, 1440},
        {3440, 1440}, {3840, 1620}, {3840, 2160}
    };
    auto add_res = [&](uint32_t w, uint32_t h) {
        for (const auto& r : res_choices) {
            if (r.w == w && r.h == h) return;
        }
        res_choices.push_back({w, h});
    };
    for (const auto& p : kStandardPresets) {
        if (p.w <= max_mon_w && p.h <= max_mon_h) {
            add_res(p.w, p.h);
        }
    }
    if (max_mon_w >= 640 && max_mon_h >= 480) {
        add_res(max_mon_w, max_mon_h);
    }
    if (res_choices.empty()) {
        add_res(1280, 720);
    }
    add_res(static_cast<uint32_t>(std::max(1, lcs::core::g_config.width)),
            static_cast<uint32_t>(std::max(1, lcs::core::g_config.height)));
    std::sort(res_choices.begin(), res_choices.end(), [](const ResPreset& a, const ResPreset& b) {
        return (a.w != b.w) ? (a.w < b.w) : (a.h < b.h);
    });

    static constexpr int kFpsPresets[] = {0, 30, 60, 75, 120, 144, 165, 240};

    auto refresh_value_strings = [fps_val_buf, res_val_buf, mse_val_buf, ams_val_buf, inv_val_buf, aim_val_buf, write_utf16_buf]() {
        if (lcs::core::g_config.max_fps <= 0) {
            write_utf16_buf(fps_val_buf, "UNLIMITED");
        } else {
            write_utf16_buf(fps_val_buf, std::to_string(lcs::core::g_config.max_fps) + " FPS");
        }
        write_utf16_buf(res_val_buf,
            std::to_string(lcs::core::g_config.width) + "X" + std::to_string(lcs::core::g_config.height));

        const auto& mcfg = lcs::input::MouseAim::instance().config();
        const auto& copt = lcs::input::PCControls::instance().options();
        auto fmt_sens = [](float v) -> std::string {
            int tenths = static_cast<int>(std::lround(v * 10.0f));
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "X";
        };
        write_utf16_buf(mse_val_buf, fmt_sens(mcfg.sensitivity));
        write_utf16_buf(ams_val_buf, fmt_sens(mcfg.aim_sensitivity));
        write_utf16_buf(inv_val_buf, mcfg.invert_y ? "ON" : "OFF");
        write_utf16_buf(aim_val_buf, copt.free_aim ? "ON" : "OFF");
    };

    auto sync_menu_prefs = [vsd, vsync_sym, frame_lim, refresh_value_strings, sync_color_sliders_from_config](bool fs, bool /*vs*/) {
        const uint8_t fsv = fs ? 1 : 0;
        *reinterpret_cast<uint8_t*>(vsd) = fsv;
        *reinterpret_cast<uint8_t*>(vsync_sym) = fsv;
        // Keep CMenuManager::m_PrefsFrameLimiter permanently disabled (0) so guest never throttles to 30/50 FPS on PC.
        *reinterpret_cast<uint8_t*>(frame_lim) = 0;
        refresh_value_strings();
        sync_color_sliders_from_config();
    };
    sync_menu_prefs(lcs::core::g_config.fullscreen, lcs::core::g_config.vsync);

    lcs::graphics::set_fullscreen_callback([vsd, vsync_sym](bool fs) {
        lcs::core::g_config.fullscreen = fs;
        const uint8_t v = fs ? 1 : 0;
        *reinterpret_cast<uint8_t*>(vsd) = v;
        *reinterpret_cast<uint8_t*>(vsync_sym) = v;
        lcs::core::save_config_setting("gtalcs/lcs.conf", "fullscreen", fs ? "on" : "off");
    });

    auto patch_bytes = [&](uint64_t guest_va, const void* data, size_t sz) {
        auto* dst = reinterpret_cast<void*>(guest_va);
#if defined(LCS_PLATFORM_WINDOWS)
        DWORD old_prot = 0;
        VirtualProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old_prot);
#endif
        std::memcpy(dst, data, sz);
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualProtect(dst, sz, old_prot, &old_prot);
#endif
        cpu.invalidate_cache(guest_va, sz);
    };

    auto patch_got_slot = [&](uint64_t got_rva, uint64_t new_target, uint64_t fallback_sym) -> uint64_t {
        uint64_t slot_addr = load_bias + got_rva;
        auto* slot = reinterpret_cast<uint64_t*>(slot_addr);
        uint64_t orig = *slot;
        if (!orig) orig = fallback_sym;
        patch_bytes(slot_addr, &new_target, sizeof(uint64_t));
        return orig;
    };

    // Use 14-row compact spacing (0x43 -> 0x5297b0, matching FEB_CON scr=4) for FEB_DIS (scr=6) in CMenuManager::Draw.
    {
        const uint8_t compact_row_layout = 0x43u;
        patch_bytes(load_bias + 0x23eab5ULL + 5ULL, &compact_row_layout, 1);

        // Redirect action 88 (Contrast) -> 0x81fd00 and action 140 (Saturation) -> 0x81fd04 in Draw, CheckSliderMovement, and GetCurrentLabelValueSlider.
        const uint32_t draw_con_adrp = 0xb00017b8u; // adrp x24, #0x81f000
        const uint32_t draw_con_add  = 0x91340318u; // add  x24, x24, #0xd00
        const uint32_t draw_sat_adrp = 0xb00017b8u; // adrp x24, #0x81f000
        const uint32_t draw_sat_add  = 0x91341318u; // add  x24, x24, #0xd04
        patch_bytes(load_bias + 0x52a89cULL, &draw_con_adrp, 4);
        patch_bytes(load_bias + 0x52a8a4ULL, &draw_con_add,  4);
        patch_bytes(load_bias + 0x52a8bcULL, &draw_sat_adrp, 4);
        patch_bytes(load_bias + 0x52a8c4ULL, &draw_sat_add,  4);

        const uint32_t chk_con[2] = {0xb0001768u, 0x91340108u}; // adrp x8, #0x81f000; add x8, x8, #0xd00
        const uint32_t chk_sat[2] = {0xb0001768u, 0x91341108u}; // adrp x8, #0x81f000; add x8, x8, #0xd04
        patch_bytes(load_bias + 0x532778ULL, chk_con, sizeof(chk_con));
        patch_bytes(load_bias + 0x5327d0ULL, chk_sat, sizeof(chk_sat));

        const uint32_t lbl_cmp_140 = 0x7102311fu; // cmp w8, #0x8c (140)
        const uint32_t lbl_sat[3]  = {0xf0001748u, 0x91341108u, 0x1400000eu}; // adrp x8, #0x81f000; add x8, x8, #0xd04; b 0x53460c
        const uint32_t lbl_con[2]  = {0xf0001748u, 0x91340108u};              // adrp x8, #0x81f000; add x8, x8, #0xd00
        patch_bytes(load_bias + 0x5345c4ULL, &lbl_cmp_140, 4);
        patch_bytes(load_bias + 0x5345ccULL, lbl_sat, sizeof(lbl_sat));
        patch_bytes(load_bias + 0x534604ULL, lbl_con, sizeof(lbl_con));
    }

    // 1. CText::Get(this, key) hook: return custom labels for FED_* and FEC_* PC menu options.
    constexpr uint64_t kGotCTextGet = 0x800bd0ULL;
    const uint64_t ctext_cb_addr = lcs::cpu::HLEManager::instance().register_function("__pc_hook_ctext",
        [lbl_con_addr, lbl_sat_addr, lbl_ful_addr, lbl_vsy_addr, lbl_fps_addr, lbl_res_addr,
         lbl_mse_addr, lbl_ams_addr, lbl_inv_addr, lbl_aim_addr](const uint64_t* args) -> uint64_t {
            const char* key = reinterpret_cast<const char*>(args[0]);
            if (!key) return 0;
            if (std::strcmp(key, "FED_CON") == 0) return lbl_con_addr;
            if (std::strcmp(key, "FED_SAT") == 0) return lbl_sat_addr;
            if (std::strcmp(key, "FED_FUL") == 0) return lbl_ful_addr;
            if (std::strcmp(key, "FED_VSY") == 0) return lbl_vsy_addr;
            if (std::strcmp(key, "FED_FPS") == 0) return lbl_fps_addr;
            if (std::strcmp(key, "FED_RES") == 0) return lbl_res_addr;
            if (std::strcmp(key, "FEC_MSE") == 0) return lbl_mse_addr;
            if (std::strcmp(key, "FEC_AMS") == 0) return lbl_ams_addr;
            if (std::strcmp(key, "FEC_INV") == 0) return lbl_inv_addr;
            if (std::strcmp(key, "FEC_AIM") == 0) return lbl_aim_addr;
            return 0;
        });
    const uint64_t ctext_hook_addr = base_addr + cursor;
    auto* ctext_code = reinterpret_cast<uint32_t*>(base + cursor);
    uint64_t orig_ctext = patch_got_slot(kGotCTextGet, ctext_hook_addr, image_game.find_symbol("_ZN5CText3GetEPKc"));
    static constexpr uint32_t kCTextHookInsns[] = {
        0xd10083ffu, 0xa9007bfdu, 0x910003fdu, 0xa90107e0u,
        0xaa0103e0u, 0x580001b0u, 0xd63f0200u, 0xb50000c0u,
        0xa94107e0u, 0xa9407bfdu, 0x910083ffu, 0x580000b0u,
        0xd61f0200u, 0xa9407bfdu, 0x910083ffu, 0xd65f03c0u,
    };
    static_assert(sizeof(kCTextHookInsns) == 16 * sizeof(uint32_t));
    std::memcpy(ctext_code, kCTextHookInsns, sizeof(kCTextHookInsns));
    auto* ctext_lits = reinterpret_cast<uint64_t*>(ctext_code + 16);
    ctext_lits[0] = orig_ctext;
    ctext_lits[1] = ctext_cb_addr;
    cursor += 16 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
    cursor = (cursor + 15) & ~size_t(15);

    // 2. Patch CMenuManager::Draw @ 0x52a294 to supply right-column text (x24) for Controls (125/126/130/142) & Display (142/143).
    const uint64_t draw_patch_va = load_bias + 0x52a294ULL;
    const uint32_t orig_draw_words[4] = {0x2a1f03e0u, 0x2a1f03e1u, 0x940aae21u, 0x2a0003fau};
    if (std::memcmp(reinterpret_cast<const void*>(draw_patch_va), orig_draw_words, sizeof(orig_draw_words)) == 0) {
        const uint64_t draw_cb_addr = lcs::cpu::HLEManager::instance().register_function("__pc_hook_draw_val",
            [femm, fps_val_addr, res_val_addr, mse_val_addr, ams_val_addr, inv_val_addr, aim_val_addr](const uint64_t* args) -> uint64_t {
                const int32_t act = static_cast<int32_t>(args[0]);
                const int32_t page = *reinterpret_cast<const int32_t*>(femm + 0x110);
                if (page == 70 || page == 4) {
                    switch (act) {
                        case 125: return mse_val_addr;
                        case 126: return ams_val_addr;
                        case 130: return inv_val_addr;
                        case 142: return aim_val_addr;
                        default:  return 0;
                    }
                }
                if (page == 6) {
                    switch (act) {
                        case 142: return fps_val_addr;
                        case 143: return res_val_addr;
                        default:  return 0;
                    }
                }
                return 0;
            });
        const uint64_t draw_stub_addr = base_addr + cursor;
        auto* draw_code = reinterpret_cast<uint32_t*>(base + cursor);
        static constexpr uint32_t kDrawX24StubInsns[] = {
            0xb5000138u, // [ 0] cbnz x24, +0x24 ([9])
            0xb98002c8u, 0x9b176508u, 0x8b140108u, 0xb9403500u,
            0x58000170u, 0xd63f0200u, 0xb4000040u, 0xaa0003f8u,
            0x2a1f03e0u, 0x2a1f03e1u, 0x58000130u, 0xd63f0200u,
            0x2a0003fau, 0x58000110u, 0xd61f0200u,
        };
        static_assert(sizeof(kDrawX24StubInsns) == 16 * sizeof(uint32_t));
        std::memcpy(draw_code, kDrawX24StubInsns, sizeof(kDrawX24StubInsns));
        auto* draw_lits = reinterpret_cast<uint64_t*>(draw_code + 16);
        draw_lits[0] = draw_cb_addr;
        draw_lits[1] = load_bias + 0x7d5b20ULL;
        draw_lits[2] = load_bias + 0x52a2a4ULL;
        cursor += 16 * sizeof(uint32_t) + 3 * sizeof(uint64_t);
        cursor = (cursor + 15) & ~size_t(15);

        struct DrawTrampoline {
            uint32_t ldr_x16;
            uint32_t br_x16;
            uint64_t target;
        } tramp{0x58000050u, 0xd61f0200u, draw_stub_addr};
        patch_bytes(draw_patch_va, &tramp, sizeof(tramp));
    }

    // Route actions 125/126/130 (Controls) & 142/143 (Controls FreeAim + Display) in ProcessMultiMenuOptions jump table (0x23ef79) to frontend click sound (0x53263c).
    const uint64_t multi_tbl_base_va = load_bias + 0x23ef79ULL;
    for (int act : {125, 126, 130, 142, 143}) {
        const uint64_t slot_va = multi_tbl_base_va + static_cast<uint64_t>(act - 110);
        if (*reinterpret_cast<const uint8_t*>(slot_va) == 14) {
            const uint8_t click = 59;
            patch_bytes(slot_va, &click, 1);
        }
    }

    // Helper generating a zero-reentrancy AArch64 post-hook: runs orig_fn natively, then notifies SVC(x0=this, x1=orig_ret, x2=orig_w1).
    auto install_post_hook = [&](uint64_t got_rva, const char* sym_name, const char* svc_name,
                                 lcs::cpu::HLEFunction notify_cb) {
        uint64_t notify_tramp = lcs::cpu::HLEManager::instance().register_function(svc_name, std::move(notify_cb));
        uint64_t stub_addr = base_addr + cursor;
        auto* code = reinterpret_cast<uint32_t*>(base + cursor);
        uint64_t orig_fn = patch_got_slot(got_rva, stub_addr, image_game.find_symbol(sym_name));
        static constexpr uint32_t kPostHookInsns[] = {
            0xa9be53f3u, 0xf9000bfeu, 0xaa0003f3u, 0xaa0103f4u,
            0x58000190u, 0xd63f0200u, 0xaa0003e1u, 0xaa1403e2u,
            0xaa1303e0u, 0xf9000fe1u, 0x58000110u, 0xd63f0200u,
            0xf9400fe0u, 0xf9400bfeu, 0xa8c253f3u, 0xd65f03c0u,
        };
        static_assert(sizeof(kPostHookInsns) == 16 * sizeof(uint32_t));
        std::memcpy(code, kPostHookInsns, sizeof(kPostHookInsns));
        auto* lits = reinterpret_cast<uint64_t*>(code + 16);
        lits[0] = orig_fn;
        lits[1] = notify_tramp;
        cursor += 16 * sizeof(uint32_t) + 2 * sizeof(uint64_t);
        cursor = (cursor + 15) & ~size_t(15);
    };

    // 3. ProcessOnOffMenuOptions post-hook -> apply Fullscreen (6) and V-Sync (7).
    install_post_hook(0x807ff8ULL, "_ZN12CMenuManager23ProcessOnOffMenuOptionsEv", "__pc_hook_onoff",
        [headless, femm, scr6_entries_addr, vsd, frame_lim, refresh_value_strings](const uint64_t* args) -> uint64_t {
            if (args[1] == 0) return 0;
            const int32_t page = *reinterpret_cast<const int32_t*>(femm + 0x110);
            const int32_t entry = *reinterpret_cast<const int32_t*>(femm + 0xc);
            if (page == 6 && entry >= 0 && entry < 14) {
                const auto* e = reinterpret_cast<const GuestMenuEntry*>(scr6_entries_addr);
                if (e[entry].action == 6 && !headless) {
                    const bool want_fs = (*reinterpret_cast<const uint8_t*>(vsd) != 0);
                    lcs::graphics::set_fullscreen(want_fs);
                } else if (e[entry].action == 7) {
                    const bool want_vsy = !lcs::core::g_config.vsync;
                    *reinterpret_cast<uint8_t*>(frame_lim) = 0;
                    lcs::core::g_config.vsync = want_vsy;
                    lcs::graphics::RenderBridge::instance().set_vsync_enabled(want_vsy);
                    lcs::core::save_config_setting("gtalcs/lcs.conf", "vsync", want_vsy ? "on" : "off");
                    refresh_value_strings();
                }
            }
            return 0;
        });

    // 4. ProcessMultiMenuOptions post-hook -> cycle Display (142/143) & Controls (136..139).
    install_post_hook(0x808000ULL, "_ZN12CMenuManager23ProcessMultiMenuOptionsEa", "__pc_hook_multi",
        [headless, femm, scr6_entries_addr, scr4_entries_addr, scr70_entries_addr, has_feb_con, has_gamset, res_choices, refresh_value_strings](const uint64_t* args) -> uint64_t {
            const int32_t page = *reinterpret_cast<const int32_t*>(femm + 0x110);
            const int32_t entry = *reinterpret_cast<const int32_t*>(femm + 0xc);
            const int dir = (static_cast<int8_t>(args[2] & 0xff) < 0) ? -1 : 1;

            const uint64_t ctrl_entries_addr =
                (page == 70 && has_gamset && entry >= 0 && entry < 9) ? scr70_entries_addr :
                (page == 4 && has_feb_con && entry >= 0 && entry < 14) ? scr4_entries_addr : 0;
            if (ctrl_entries_addr != 0) {
                const auto* e = reinterpret_cast<const GuestMenuEntry*>(ctrl_entries_addr);
                auto& mcfg = lcs::input::MouseAim::instance().config();
                auto& copt = lcs::input::PCControls::instance().options_mut();
                const int32_t act = e[entry].action;
                auto step_sens = [dir](float v) -> float {
                    int tenths = static_cast<int>(std::lround(v * 10.0f));
                    tenths = std::clamp(tenths + dir, 2, 30);
                    return static_cast<float>(tenths) * 0.1f;
                };
                if (act == 125) {
                    mcfg.sensitivity = step_sens(mcfg.sensitivity);
                    refresh_value_strings();
                    lcs::input::PCControls::instance().save_profile();
                } else if (act == 126) {
                    mcfg.aim_sensitivity = step_sens(mcfg.aim_sensitivity);
                    refresh_value_strings();
                    lcs::input::PCControls::instance().save_profile();
                } else if (act == 130) {
                    mcfg.invert_y = !mcfg.invert_y;
                    refresh_value_strings();
                    lcs::input::PCControls::instance().save_profile();
                } else if (act == 142) {
                    copt.free_aim = !copt.free_aim;
                    refresh_value_strings();
                    lcs::input::PCControls::instance().save_profile();
                }
                return 0;
            }

            if (page != 6 || entry < 0 || entry >= 14) return 0;
            const auto* e = reinterpret_cast<const GuestMenuEntry*>(scr6_entries_addr);
            if (e[entry].action == 142) {
                constexpr int kCount = static_cast<int>(std::size(kFpsPresets));
                int idx = 0;
                int best_diff = std::abs(kFpsPresets[0] - lcs::core::g_config.max_fps);
                for (int i = 1; i < kCount; ++i) {
                    int diff = std::abs(kFpsPresets[i] - lcs::core::g_config.max_fps);
                    if (diff < best_diff) {
                        best_diff = diff;
                        idx = i;
                    }
                }
                idx = (idx + dir + kCount) % kCount;
                lcs::core::g_config.max_fps = kFpsPresets[idx];
                refresh_value_strings();
                lcs::core::save_config_setting("gtalcs/lcs.conf", "max_fps",
                                               std::to_string(lcs::core::g_config.max_fps));
            } else if (e[entry].action == 143 && !res_choices.empty()) {
                const int count = static_cast<int>(res_choices.size());
                int idx = 0;
                for (int i = 0; i < count; ++i) {
                    if (static_cast<int>(res_choices[i].w) == lcs::core::g_config.width &&
                        static_cast<int>(res_choices[i].h) == lcs::core::g_config.height) {
                        idx = i;
                        break;
                    }
                }
                idx = (idx + dir + count) % count;
                const uint32_t nw = res_choices[idx].w;
                const uint32_t nh = res_choices[idx].h;
                lcs::core::g_config.width = static_cast<int>(nw);
                lcs::core::g_config.height = static_cast<int>(nh);
                if (!headless) {
                    lcs::graphics::set_window_size(nw, nh);
                }
                refresh_value_strings();
                lcs::core::save_config_setting("gtalcs/lcs.conf", "width", std::to_string(nw));
                lcs::core::save_config_setting("gtalcs/lcs.conf", "height", std::to_string(nh));
            }
            return 0;
        });

    // 7. Touchscreen::RestoreDefaults post-hook -> reset PC Controls options when user clicks Restore Defaults in FEB_CON.
    if (has_feb_con) {
        install_post_hook(0x808038ULL, "_ZN11Touchscreen15RestoreDefaultsEv", "__pc_hook_restore_con",
            [femm, scr4_entries_addr, refresh_value_strings](const uint64_t*) -> uint64_t {
                const int32_t page = *reinterpret_cast<const int32_t*>(femm + 0x110);
                const int32_t entry = *reinterpret_cast<const int32_t*>(femm + 0xc);
                if (page == 4 && entry >= 0 && entry < 14) {
                    const auto* e = reinterpret_cast<const GuestMenuEntry*>(scr4_entries_addr);
                    if (e[entry].action == 102) {
                        auto& mcfg = lcs::input::MouseAim::instance().config();
                        mcfg.sensitivity = 1.0f;
                        mcfg.aim_sensitivity = 0.8f;
                        mcfg.invert_y = false;
                        lcs::input::PCControls::instance().options_mut().free_aim = true;
                        refresh_value_strings();
                        lcs::input::PCControls::instance().save_profile();
                    }
                }
                return 0;
            });
    }

    // 5. LoadSettings post-hook -> keep menu Display & InvertLook options synced with host/lcs.conf.
    const uint64_t invert_look_sym = image_game.find_symbol("_ZN12CMenuManager17m_PrefsInvertLookE");
    install_post_hook(0x808080ULL, "_ZN12CMenuManager12LoadSettingsEv", "__pc_hook_load_settings",
        [headless, sync_menu_prefs, invert_look_sym](const uint64_t*) -> uint64_t {
            const bool active_fs = headless ? lcs::core::g_config.fullscreen : lcs::graphics::is_fullscreen();
            sync_menu_prefs(active_fs, lcs::core::g_config.vsync);
            if (invert_look_sym) {
                *reinterpret_cast<uint8_t*>(invert_look_sym) =
                    lcs::input::MouseAim::instance().config().invert_y ? 1 : 0;
            }
            return 0;
        });

    // 6. RestoreDisplayDefaults post-hook -> reset PC display prefs only when user clicks Restore Defaults in FEB_DIS.
    install_post_hook(0x808030ULL, "_Z22RestoreDisplayDefaultsv", "__pc_hook_restore_disp",
        [headless, femm, scr6_entries_addr, sync_menu_prefs](const uint64_t*) -> uint64_t {
            bool clicked_restore = false;
            const int32_t page = *reinterpret_cast<const int32_t*>(femm + 0x110);
            const int32_t entry = *reinterpret_cast<const int32_t*>(femm + 0xc);
            if (page == 6 && entry >= 0 && entry < 14) {
                const auto* e = reinterpret_cast<const GuestMenuEntry*>(scr6_entries_addr);
                clicked_restore = (e[entry].action == 102);
            }
            if (clicked_restore) {
                lcs::core::g_config.vsync = false;
                lcs::core::g_config.max_fps = 0;
                lcs::core::g_config.width = 1280;
                lcs::core::g_config.height = 720;
                lcs::core::g_config.brightness = 62;
                lcs::core::g_config.contrast = 50;
                lcs::core::g_config.saturation = 50;
                lcs::graphics::RenderBridge::instance().set_vsync_enabled(false);
                if (!headless) {
                    lcs::graphics::set_fullscreen(false);
                    lcs::graphics::set_window_size(1280, 720);
                } else {
                    lcs::core::g_config.fullscreen = false;
                    lcs::core::save_config_setting("gtalcs/lcs.conf", "fullscreen", "off");
                }
                lcs::core::save_config_setting("gtalcs/lcs.conf", "vsync", "off");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "max_fps", "0");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "width", "1280");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "height", "720");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "brightness", "62");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "contrast", "50");
                lcs::core::save_config_setting("gtalcs/lcs.conf", "saturation", "50");
                sync_menu_prefs(false, false);
            } else {
                const bool active_fs = headless ? lcs::core::g_config.fullscreen : lcs::graphics::is_fullscreen();
                sync_menu_prefs(active_fs, lcs::core::g_config.vsync);
            }
            return 0;
        });

    cpu.invalidate_cache(base_addr, 0x1000);
    std::cout << "  - Injected PC Display options (Fullscreen, V-Sync, Max FPS, Resolution) into FEB_DIS.\n";
}

// Restricts CMenuManager 2D layout/fonts to a centered 16:9 viewport on ultrawide (>16:9) displays
// Keeps menus edge-anchored across the full ultrawide frame while fixing stretched menu text
// (CMenuManager::DrawFrontEnd) and cutscene/gameplay subtitles & HUD fade text (CHud::DrawAfterFade).
static uint8_t* alloc_rwx_hook_page() {
#if defined(LCS_PLATFORM_WINDOWS)
    void* ptr = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!ptr) return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(ptr);
    std::memset(base, 0, 0x1000);
    return base;
#else
    return nullptr;
#endif
}

static uint64_t patch_got_slot_entry(const lcs::cpu::ElfImage& image_game,
                                     uint64_t got_rva,
                                     uint64_t new_target,
                                     const char* fallback_sym) {
#if defined(LCS_PLATFORM_WINDOWS)
    const uint64_t slot_addr = image_game.get_load_bias() + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot_addr);
    if (!orig && fallback_sym) orig = image_game.find_symbol(fallback_sym);
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(slot_addr), sizeof(uint64_t), PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(reinterpret_cast<void*>(slot_addr), &new_target, sizeof(uint64_t));
    VirtualProtect(reinterpret_cast<void*>(slot_addr), sizeof(uint64_t), old_prot, &old_prot);
    return orig;
#else
    (void)image_game;
    (void)got_rva;
    (void)new_target;
    (void)fallback_sym;
    return 0;
#endif
}

// Caps logical guest dimensions to <=1920x1080 (and >=720px tall when aspect fits)
// so libGame.so renders directly to FBO 0 while matching the host surface aspect ratio.
static void compute_guest_dimensions(uint32_t win_w, uint32_t win_h,
                                     uint32_t& out_w, uint32_t& out_h) {
    constexpr uint32_t kMaxGuestWidth = 1920;
    constexpr uint32_t kMaxGuestHeight = 1080;
    constexpr uint32_t kMinGuestHeight = 720;

    out_w = std::max(1u, win_w);
    out_h = std::max(1u, win_h);
    if (out_w > kMaxGuestWidth || out_h > kMaxGuestHeight) {
        const double down = std::min(
            static_cast<double>(kMaxGuestWidth) / static_cast<double>(out_w),
            static_cast<double>(kMaxGuestHeight) / static_cast<double>(out_h));
        out_w = std::max(1u, static_cast<uint32_t>(std::llround(static_cast<double>(out_w) * down)));
        out_h = std::max(1u, static_cast<uint32_t>(std::llround(static_cast<double>(out_h) * down)));
    }
    if (out_h < kMinGuestHeight && out_w < kMaxGuestWidth) {
        const double up = std::min(
            static_cast<double>(kMaxGuestWidth) / static_cast<double>(out_w),
            static_cast<double>(kMinGuestHeight) / static_cast<double>(out_h));
        out_w = std::max(1u, static_cast<uint32_t>(std::llround(static_cast<double>(out_w) * up)));
        out_h = std::max(1u, static_cast<uint32_t>(std::llround(static_cast<double>(out_h) * up)));
    }
}

struct UltrawideScaleState {
    uint32_t menu_depth;
    uint32_t hud_depth;
    float menu_scale_x;
    float hud_scale_x;
};

static void update_pc_ultrawide_menu_scales(UltrawideScaleState* state,
                                            uint32_t guest_width,
                                            uint32_t guest_height) {
    if (!state || guest_width == 0 || guest_height == 0) return;
    const float aspect = static_cast<float>(guest_width) / static_cast<float>(guest_height);
    const float uw_menu_scale = (16.0f / 9.0f) / aspect;
    // Use ~0.995 threshold so standard 16:9 variants like 1366x768 keep unscaled 1.0f metrics.
    if (uw_menu_scale < 0.995f) {
        state->menu_scale_x = uw_menu_scale;
        state->hud_scale_x = uw_menu_scale;
    } else {
        state->menu_scale_x = 1.0f;
        state->hud_scale_x = 1.0f;
    }
}

static UltrawideScaleState* setup_pc_ultrawide_menu_hooks(const lcs::cpu::ElfImage& image_game,
                                                          lcs::cpu::DynarmicCore& cpu,
                                                          uint32_t guest_width,
                                                          uint32_t guest_height) {
    if (guest_width == 0 || guest_height == 0) return nullptr;

    uint8_t* base = alloc_rwx_hook_page();
    if (!base) {
        std::cerr << "  [WARN] Failed to allocate ultrawide font hook page.\n";
        return nullptr;
    }

    const uint64_t base_addr = reinterpret_cast<uint64_t>(base);
    const uint64_t state_addr = base_addr + 0xF00ULL;

    // Shared state at +0xF00: +0 menu_depth, +4 hud_depth, +8 menu_scale_x, +12 hud_scale_x.
    auto* state = reinterpret_cast<UltrawideScaleState*>(base + 0xF00);
    state->menu_depth = 0;
    state->hud_depth = 0;
    update_pc_ultrawide_menu_scales(state, guest_width, guest_height);

    // Stub template: increments *depth_ptr before calling orig_fn, decrements after returning.
    static constexpr uint32_t kDepthWrapInsns[16] = {
        0xa9bf7bfdu, 0x910003fdu, 0x580001c8u, 0xb9400109u,
        0x11000529u, 0xb9000109u, 0x58000190u, 0xd63f0200u,
        0x58000108u, 0xb9400109u, 0x51000529u, 0xb9000109u,
        0xa8c17bfdu, 0xd65f03c0u, 0xd503201fu, 0xd503201fu,
    };
    auto emit_depth_wrapper = [&](size_t off, uint64_t got_rva, const char* sym, uint64_t depth_ptr) {
        const uint64_t orig_fn = patch_got_slot_entry(image_game, got_rva, base_addr + off, sym);
        std::memcpy(base + off, kDepthWrapInsns, sizeof(kDepthWrapInsns));
        auto* lit = reinterpret_cast<uint64_t*>(base + off + 0x40);
        lit[0] = depth_ptr;
        lit[1] = orig_fn;
    };

    // Menus draw in 16:9 metrics; CHud::Draw & CHud::DrawAfterFade emit 4:3 font X metrics by default (ws=0).
    // Suppress Rockstar's native front-end rendering once menu_page is initialized so it never flashes behind the ImGui UI.
    {
        const uint64_t femm_addr = image_game.find_symbol("FrontEndMenuManager");
        const uint64_t gstate_addr = image_game.find_symbol("gGameState");
        const uint64_t a_screens = image_game.find_symbol("aScreens");
        const uint64_t use_resume_addr = image_game.find_symbol("_ZN12CMenuManager16m_bUseResumeMenuE");
        const uint64_t shown_legal_addr = image_game.find_symbol("shownLegalScreen");
        const uint64_t fn_is_playing = image_game.find_symbol("_Z13IsPlayingGamev");
        const uint64_t orig_draw_fe =
            *reinterpret_cast<const uint64_t*>(image_game.get_load_bias() + 0x807c20ULL)
                ? *reinterpret_cast<const uint64_t*>(image_game.get_load_bias() + 0x807c20ULL)
                : image_game.find_symbol("_ZN12CMenuManager12DrawFrontEndEv");
        const uint64_t tramp = lcs::cpu::HLEManager::instance().register_function(
            "__pc_draw_frontend_suppress",
            [&cpu, state, orig_draw_fe, femm_addr, gstate_addr, use_resume_addr, shown_legal_addr, fn_is_playing, a_screens](const uint64_t* args) -> uint64_t {
                const int32_t page = femm_addr ? *reinterpret_cast<const int32_t*>(femm_addr + 0x110ULL) : 0;
                const bool menu_active = femm_addr ? (*reinterpret_cast<const uint8_t*>(femm_addr + 0x14ULL) != 0) : false;
                const uint32_t gstate = gstate_addr ? *reinterpret_cast<const uint32_t*>(gstate_addr) : 0;
                if (ui::globalMenuOpen() ||
                    lcs::game::LCSTrainer::instance().is_in_main_menu() ||
                    (gstate == 7 && menu_active)) {
                    if (femm_addr && page == 0) {
                        int32_t init_page = 51;
                        const bool has_res_flag = (*reinterpret_cast<const uint8_t*>(femm_addr + 0x2aULL) != 0);
                        const bool use_res = use_resume_addr && (*reinterpret_cast<const uint8_t*>(use_resume_addr) != 0);
                        if (has_res_flag && use_res) {
                            init_page = 69;
                        } else if (!has_res_flag && fn_is_playing && cpu.call(fn_is_playing, {}) != 0) {
                            init_page = 52;
                            if (shown_legal_addr) *reinterpret_cast<uint8_t*>(shown_legal_addr) = 1;
                        }
                        *reinterpret_cast<int32_t*>(femm_addr + 0x110ULL) = init_page;
                        *reinterpret_cast<int32_t*>(femm_addr + 0x114ULL) = init_page;
                        if (*reinterpret_cast<const int32_t*>(femm_addr + 0xcULL) == 0 && a_screens) {
                            const uint8_t* scr = reinterpret_cast<const uint8_t*>(a_screens) + static_cast<size_t>(init_page) * 0x8ccULL;
                            if (*reinterpret_cast<const int32_t*>(scr + 0x34ULL) == 1) {
                                *reinterpret_cast<int32_t*>(femm_addr + 0xcULL) = 1;
                            }
                        }
                    }
                    return 0;
                }
                if (!orig_draw_fe) return 0;
                state->menu_depth++;
                cpu.call(orig_draw_fe, {args[0]});
                state->menu_depth--;
                return 0;
            });
        patch_got_slot_entry(image_game, 0x807c20ULL, tramp, "_ZN12CMenuManager12DrawFrontEndEv");
    }
    emit_depth_wrapper(0x100, 0x806ad8ULL, "_ZN4CHud13DrawAfterFadeEv", state_addr + 4ULL);
    emit_depth_wrapper(0x300, 0x806a48ULL, "_ZN4CHud4DrawEv", state_addr + 4ULL);

    // Stub 3 (+0x200): wraps CFont::SetScale(s0, s1) (GOT 0x803078) with 16:9 ultrawide X scale,
    // skipping CHud::Draw ammo (0x47ee6c) which already divides s0 by Display::aspect in libGame.so.
    constexpr size_t kStub3Off = 0x200;
    const uint64_t orig_set_scale =
        patch_got_slot_entry(image_game, 0x803078ULL, base_addr + kStub3Off, "_ZN5CFont8SetScaleEff");
    static constexpr uint32_t kStub3Insns[16] = {
        0x58000208u, 0xb9400509u, 0x340000e9u, 0x5800022au,
        0xeb0a03dfu, 0x54000100u, 0xbd400d02u, 0x1e220800u,
        0x14000005u, 0xb9400109u, 0x34000069u, 0xbd400902u,
        0x1e220800u, 0x580000b0u, 0xd61f0200u, 0xd503201fu,
    };
    std::memcpy(base + kStub3Off, kStub3Insns, sizeof(kStub3Insns));
    auto* s3_u64 = reinterpret_cast<uint64_t*>(base + kStub3Off + 0x40);
    s3_u64[0] = state_addr;
    s3_u64[1] = orig_set_scale;
    s3_u64[2] = image_game.get_load_bias() + 0x47ee6cULL;

    cpu.invalidate_cache(base_addr, 0x1000);
    std::cout << "  - Applied dynamic ultrawide menu & HUD aspect fix (menu X=" << state->menu_scale_x
              << ", HUD/sub X=" << state->hud_scale_x << " at " << guest_width << "x" << guest_height << ").\n";
    return state;
}

// Pace cAudioManager::ServiceSoundEffects() to the original 30 Hz step rate so fixed per-tick
// engine ramps (0.6^N accel cutoff, -8/127 fades, shift timers) do not collapse at 60-144 FPS.
static volatile uint32_t* setup_pc_audio_timing_hooks(const lcs::cpu::ElfImage& image_game,
                                                      lcs::cpu::DynarmicCore& cpu) {
    const uint64_t sample_mgr = image_game.find_symbol("SampleManager");
    const uint64_t orig_sample_service = image_game.find_symbol("_ZN14cSampleManager7ServiceEb");
    const uint64_t slot_addr = image_game.get_load_bias() + 0x801448ULL;
    uint64_t orig_sfx_service = 0;
    if (sample_mgr && orig_sample_service) {
        orig_sfx_service = *reinterpret_cast<const uint64_t*>(slot_addr);
        if (!orig_sfx_service) orig_sfx_service = image_game.find_symbol("_ZN13cAudioManager19ServiceSoundEffectsEv");
    }
    if (!orig_sfx_service || !orig_sample_service || !sample_mgr) {
        return nullptr;
    }

    uint8_t* base = alloc_rwx_hook_page();
    if (!base) {
        return nullptr;
    }
    const uint64_t base_addr = reinterpret_cast<uint64_t>(base);
    const uint64_t state_addr = base_addr + 0x100ULL;

    *reinterpret_cast<uint32_t*>(base + 0x100) = 1; // allow_sfx_tick
    *reinterpret_cast<uint64_t*>(base + 0x108) = sample_mgr;
    *reinterpret_cast<uint64_t*>(base + 0x110) = orig_sample_service;
    *reinterpret_cast<uint64_t*>(base + 0x118) =
        patch_got_slot_entry(image_game, 0x801448ULL, base_addr, "_ZN13cAudioManager19ServiceSoundEffectsEv");

    static constexpr uint32_t kAudio30HzInsns[10] = {
        0x58000188u, 0xb9400109u, 0x34000089u, 0xb900011fu,
        0xf9400d10u, 0xd61f0200u, 0xf9400500u, 0x2a1f03e1u,
        0xf9400910u, 0xd61f0200u,
    };
    std::memcpy(base, kAudio30HzInsns, sizeof(kAudio30HzInsns));
    *reinterpret_cast<uint64_t*>(base + 0x30) = state_addr;

    cpu.invalidate_cache(base_addr, 0x1000);
    std::cout << "  - Applied 30 Hz cAudioManager SFX pacing hook.\n";
    return reinterpret_cast<volatile uint32_t*>(base + 0x100);
}

// Bridges guest CPad::StartShake*, TouchSense::*, and OS_HasVibrator calls to GamepadDriver SDL3 haptics.
static void setup_pc_gamepad_rumble_hooks(const lcs::cpu::ElfImage& image_game, bool headless) {
    if (headless) return;

    const uint64_t load_bias = image_game.get_load_bias();
    const uint64_t pref_vib = image_game.find_symbol("_ZN12CMenuManager19m_PrefsUseVibrationE");

    auto patch_got = [&](uint64_t got_rva, const char* svc_name, lcs::cpu::HLEFunction fn) {
        const uint64_t tramp = lcs::cpu::HLEManager::instance().register_function(svc_name, std::move(fn));
        auto* slot = reinterpret_cast<uint64_t*>(load_bias + got_rva);
#if defined(LCS_PLATFORM_WINDOWS)
        DWORD old_prot = 0;
        VirtualProtect(slot, sizeof(uint64_t), PAGE_READWRITE, &old_prot);
#endif
        *slot = tramp;
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualProtect(slot, sizeof(uint64_t), old_prot, &old_prot);
#endif
    };

    auto vibration_allowed = [pref_vib]() -> bool {
        auto& gp = lcs::input::GamepadDriver::instance();
        if (!gp.is_connected() || !gp.get_caps().has_rumble) return false;
        if (pref_vib && *reinterpret_cast<const uint8_t*>(pref_vib) == 0) return false;
        return true;
    };

    // CPad::StartShake(this, short duration, uint8_t freq)
    patch_got(0x802eb0ULL, "__pc_pad_start_shake", [vibration_allowed](const uint64_t* args) -> uint64_t {
        if (!vibration_allowed()) return 0;
        const int16_t dur_raw = static_cast<int16_t>(args[1] & 0xffffu);
        const uint8_t freq = static_cast<uint8_t>(args[2] & 0xffu);
        if (dur_raw <= 0 || freq == 0) return 0;
        const float amp = std::clamp(static_cast<float>(freq) / 255.0f, 0.1f, 1.0f);
        const uint32_t dur_ms = static_cast<uint32_t>(std::clamp<int>(dur_raw, 40, 2000));
        lcs::input::GamepadDriver::instance().set_rumble(amp, amp * 0.75f, dur_ms);
        return 0;
    });

    // CPad::StartShake_Distance(this, short duration, uint8_t freq, float x, float y, float z)
    patch_got(0x802eb8ULL, "__pc_pad_start_shake_dist", [vibration_allowed](const uint64_t* args) -> uint64_t {
        if (!vibration_allowed()) return 0;
        const int16_t dur_raw = static_cast<int16_t>(args[1] & 0xffffu);
        const uint8_t freq = static_cast<uint8_t>(args[2] & 0xffu);
        if (dur_raw <= 0 || freq == 0) return 0;
        const float amp = std::clamp(static_cast<float>(freq) / 255.0f, 0.1f, 1.0f) * 0.7f;
        const uint32_t dur_ms = static_cast<uint32_t>(std::clamp<int>(dur_raw, 40, 1500));
        lcs::input::GamepadDriver::instance().set_rumble(amp, amp * 0.65f, dur_ms);
        return 0;
    });

    // CPad::StartShake_Train(this, float x, float y)
    patch_got(0x802ec0ULL, "__pc_pad_start_shake_train", [vibration_allowed](const uint64_t*) -> uint64_t {
        if (!vibration_allowed()) return 0;
        lcs::input::GamepadDriver::instance().set_rumble(0.22f, 0.12f, 120);
        return 0;
    });

    // CPad::StopPadsShaking() & CPad::StopShaking(short)
    auto stop_cb = [](const uint64_t*) -> uint64_t {
        lcs::input::GamepadDriver::instance().set_rumble(0.0f, 0.0f, 0);
        lcs::input::GamepadDriver::instance().set_trigger_rumble(0.0f, 0.0f, 0);
        return 0;
    };
    patch_got(0x802f18ULL, "__pc_pad_stop_shakes", stop_cb);
    patch_got(0x802f20ULL, "__pc_pad_stop_shake", stop_cb);

    // TouchSense::playBuiltinEffect(this, type, min_mag, max_mag, param4, dur) -> weapons/collisions/impacts
    patch_got(0x805100ULL, "__pc_ts_play_builtin", [vibration_allowed](const uint64_t* args) -> uint64_t {
        if (!vibration_allowed() || lcs::input::PCControls::instance().in_menu()) return 1;
        const int min_mag = static_cast<int32_t>(args[2]);
        const int max_mag = static_cast<int32_t>(args[3]);
        const int dur_units = static_cast<int32_t>(args[5]);
        if (min_mag < 20 && max_mag < 20) return 1; // Ignore subtle UI/cursor ticks
        const float low = std::clamp(static_cast<float>(min_mag) / 127.0f, 0.12f, 1.0f);
        const float high = std::clamp(static_cast<float>(max_mag) / 127.0f, 0.12f, 1.0f);
        const uint32_t dur_ms = static_cast<uint32_t>(std::clamp(dur_units * 4, 45, 250));
        auto& gp = lcs::input::GamepadDriver::instance();
        gp.set_rumble(low, high, dur_ms);
        gp.set_trigger_rumble(low * 0.2f, high * 0.55f, std::min<uint32_t>(dur_ms, 120));
        return 1;
    });

    // TouchSense::startContinuousBuiltinEffect / stopContinuousEffect
    patch_got(0x806350ULL, "__pc_ts_start_cont", [vibration_allowed](const uint64_t*) -> uint64_t {
        if (vibration_allowed() && !lcs::input::PCControls::instance().in_menu()) {
            lcs::input::GamepadDriver::instance().set_rumble(0.35f, 0.25f, 1000);
        }
        return 1;
    });
    patch_got(0x806330ULL, "__pc_ts_stop_cont", [](const uint64_t*) -> uint64_t {
        lcs::input::GamepadDriver::instance().set_rumble(0.0f, 0.0f, 0);
        return 0;
    });

    // OS_HasVibrator() dynamic check reflecting live connected controller rumble capability
    patch_got(0x807e08ULL, "__pc_os_has_vibrator", [](const uint64_t*) -> uint64_t {
        auto& gp = lcs::input::GamepadDriver::instance();
        return (gp.is_connected() && gp.get_caps().has_rumble) ? 1ULL : 0ULL;
    });

    std::cout << "  - Bridged CPad::StartShake / TouchSense haptics to SDL3 GamepadDriver rumble.\n";
}

// Replaces PLT calls to CTimer::Update() (GOT 0x800bc0) with a sub-millisecond steady_clock
// HLE implementation carrying fractional milliseconds across frames so simulation speed remains
// locked to exact 1.0x real-time (50 Hz Leeds step base) from 8 FPS up to 400+ FPS.
static void setup_pc_ctimer_hook(const lcs::cpu::ElfImage& image_game) {
    struct TimerSyms {
        uint32_t* time_ms = nullptr;
        uint32_t* prev_ms = nullptr;
        uint32_t* pause_ms = nullptr;
        uint32_t* nonclip_ms = nullptr;
        uint32_t* frame_cycles = nullptr;
        uint32_t* frame_counter = nullptr;
        float* step = nullptr;
        float* step_nonclip = nullptr;
        float* step_noscale = nullptr;
        float* time_scale = nullptr;
        uint8_t* user_pause = nullptr;
        uint8_t* code_pause = nullptr;
        uint8_t* snapshot = nullptr;
        uint16_t* rec_state = nullptr;
        uint8_t* chase_status = nullptr;
        uint8_t* m_timer = nullptr;
    };

    auto syms = std::make_shared<TimerSyms>();
    syms->time_ms       = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer22m_snTimeInMillisecondsE"));
    syms->prev_ms       = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer30m_snPreviousTimeInMillisecondsE"));
    syms->pause_ms      = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer31m_snTimeInMillisecondsPauseModeE"));
    syms->nonclip_ms    = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer32m_snTimeInMillisecondsNonClippedE"));
    syms->frame_cycles  = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer21m_snFrameTimeInCyclesE"));
    syms->frame_counter = reinterpret_cast<uint32_t*>(image_game.find_symbol("_ZN6CTimer14m_FrameCounterE"));
    syms->step          = reinterpret_cast<float*>(image_game.find_symbol("_ZN6CTimer12ms_fTimeStepE"));
    syms->step_nonclip  = reinterpret_cast<float*>(image_game.find_symbol("_ZN6CTimer22ms_fTimeStepNonClippedE"));
    syms->step_noscale  = reinterpret_cast<float*>(image_game.find_symbol("_ZN6CTimer19ms_fTimeStepNoScaleE"));
    syms->time_scale    = reinterpret_cast<float*>(image_game.find_symbol("_ZN6CTimer13ms_fTimeScaleE"));
    syms->user_pause    = reinterpret_cast<uint8_t*>(image_game.find_symbol("_ZN6CTimer11m_UserPauseE"));
    syms->code_pause    = reinterpret_cast<uint8_t*>(image_game.find_symbol("_ZN6CTimer11m_CodePauseE"));
    syms->snapshot      = reinterpret_cast<uint8_t*>(image_game.find_symbol("_ZN10CSpecialFX15bSnapShotActiveE"));
    syms->rec_state     = reinterpret_cast<uint16_t*>(image_game.find_symbol("_ZN18CRecordDataForGame14RecordingStateE"));
    syms->chase_status  = reinterpret_cast<uint8_t*>(image_game.find_symbol("_ZN19CRecordDataForChase6StatusE"));
    syms->m_timer       = reinterpret_cast<uint8_t*>(image_game.find_symbol("_ZN6CTimer7m_TimerE"));

    if (!syms->time_ms || !syms->step) {
        std::cerr << "  [WARN] CTimer::Update HLE hook skipped (missing symbols).\n";
        return;
    }

    struct TimerState {
        bool init = false;
        std::chrono::steady_clock::time_point origin{};
        std::chrono::steady_clock::time_point prev{};
        double pause_ms_frac = 0.0;
        double game_ms_frac = 0.0;
    };
    auto st = std::make_shared<TimerState>();

    const uint64_t tramp = lcs::cpu::HLEManager::instance().register_function(
        "__pc_ctimer_update",
        [syms, st](const uint64_t*) -> uint64_t {
            const uint32_t old_ms = *syms->time_ms;
            if (syms->prev_ms) *syms->prev_ms = old_ms;

            const auto now = std::chrono::steady_clock::now();
            if (!st->init) {
                st->init = true;
                st->origin = now;
                st->prev = now - std::chrono::microseconds(16667);
            }
            double dt_sec = std::chrono::duration<double>(now - st->prev).count();
            st->prev = now;
            dt_sec = std::clamp(dt_sec, 0.0002, 0.125); // 8 FPS..5000 FPS safety window

            if (syms->m_timer) {
                const uint32_t wall_ms = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - st->origin).count());
                *reinterpret_cast<uint32_t*>(syms->m_timer + 0x00) = wall_ms;
                *reinterpret_cast<uint32_t*>(syms->m_timer + 0x08) = wall_ms;
                const float tm = *reinterpret_cast<const float*>(syms->m_timer + 0x10);
                *reinterpret_cast<float*>(syms->m_timer + 0x14) = static_cast<float>(dt_sec * tm);
                *reinterpret_cast<float*>(syms->m_timer + 0x18) = static_cast<float>(dt_sec * 1000.0 * tm);
                *reinterpret_cast<float*>(syms->m_timer + 0x1c) = static_cast<float>(dt_sec);
                *reinterpret_cast<float*>(syms->m_timer + 0x20) = static_cast<float>(dt_sec * 1000.0);
            }

            const uint32_t raw_cycles = static_cast<uint32_t>(std::llround(dt_sec * 147456000.0));
            st->pause_ms_frac += dt_sec * 1000.0;
            const uint32_t pause_inc = static_cast<uint32_t>(st->pause_ms_frac);
            st->pause_ms_frac -= static_cast<double>(pause_inc);
            if (syms->pause_ms) *syms->pause_ms += pause_inc;

            const bool paused =
                (syms->user_pause && *syms->user_pause != 0) ||
                (syms->code_pause && *syms->code_pause != 0);
            const double active_dt = paused ? 0.0 : dt_sec;
            if (syms->frame_cycles) *syms->frame_cycles = paused ? 0u : raw_cycles;

            const double scale = syms->time_scale ? static_cast<double>(*syms->time_scale) : 1.0;
            const double step_noscale = active_dt * 50.0;
            double step = active_dt * 50.0 * scale;

            if (!paused) {
                st->game_ms_frac += active_dt * 1000.0 * scale;
                const uint32_t ms_inc = static_cast<uint32_t>(st->game_ms_frac);
                st->game_ms_frac -= static_cast<double>(ms_inc);
                *syms->time_ms += ms_inc;
                if (syms->nonclip_ms) *syms->nonclip_ms += ms_inc;
            } else {
                st->game_ms_frac = 0.0;
            }

            const bool snapshot = (syms->snapshot && *syms->snapshot != 0);
            if (!paused && !snapshot && step > 0.0 && step < 0.05) {
                step = 0.05; // only prevent div-by-0 above ~1000 FPS
            }

            float final_step = static_cast<float>(step);
            *syms->step = final_step;
            if (syms->step_nonclip) *syms->step_nonclip = final_step;
            if (syms->step_noscale) *syms->step_noscale = static_cast<float>(step_noscale);

            if (!syms->rec_state || *syms->rec_state != 2) {
                if (final_step > 6.0f) {
                    final_step = 6.0f;
                    *syms->step = 6.0f;
                }
                if (*syms->time_ms > old_ms + 120u) {
                    *syms->time_ms = old_ms + 120u;
                }
            }
            if (syms->chase_status && *syms->chase_status == 1) {
                *syms->step = 1.0f;
                *syms->time_ms = old_ms + 16u;
            }
            if (syms->frame_counter) {
                (*syms->frame_counter) += 1u;
            }
            return 0;
        });

    patch_got_slot_entry(image_game, 0x800bc0ULL, tramp, "_ZN6CTimer6UpdateEv");
    std::cout << "  - Installed high-precision fractional CTimer::Update() HLE hook (GOT 0x800bc0).\n";
}

// Replaces PLT calls to CdStreamSync(int) (GOT 0x806df0) with an atomic completion wait.
// Rockstar's ARM64 CdStreamSync reads ch->nSectorsToRead non-atomically before setting bLocked=1
// and calling Semaphore::Down; StreamThread::ThreadMain can finish and read bLocked==0 in between,
// leaving CdStreamSync permanently deadlocked on modern fast storage + multi-core hosts.
static void setup_pc_cdstream_hook(const lcs::cpu::ElfImage& image_game) {
    const uint64_t load_bias = image_game.get_load_bias();
    const uint64_t tramp = lcs::cpu::HLEManager::instance().register_function(
        "__pc_cdstream_sync",
        [load_bias](const uint64_t* args) -> uint64_t {
            const int32_t channel = static_cast<int32_t>(args[0]);
            const auto* cd_init = reinterpret_cast<const uint8_t*>(load_bias + 0x9ec3dcULL);
            if (*cd_init != 1) return 0;

            const int32_t num_ch = *reinterpret_cast<const int32_t*>(load_bias + 0x9ec3a8ULL);
            auto* channels = *reinterpret_cast<uint8_t* const*>(load_bias + 0x9ec3b0ULL);
            if (!channels || channel < 0 || channel >= num_ch) return 0;

            uint8_t* ch = channels + static_cast<size_t>(channel) * 0x28ULL;
            auto is_busy = [ch]() -> bool {
                std::atomic_thread_fence(std::memory_order_seq_cst);
                const uint32_t nsec = std::atomic_ref<uint32_t>(
                    *reinterpret_cast<uint32_t*>(ch + 0x04)).load(std::memory_order_acquire);
                const uint8_t bread = std::atomic_ref<uint8_t>(
                    *reinterpret_cast<uint8_t*>(ch + 0x12)).load(std::memory_order_acquire);
                return (nsec != 0) || (bread != 0);
            };

            uint32_t spins = 0;
            while (is_busy()) {
                if (++spins < 64) {
                    std::this_thread::yield();
                } else {
                    std::this_thread::sleep_for(std::chrono::microseconds(250));
                }
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const uint32_t status = *reinterpret_cast<const uint32_t*>(ch + 0x14);
            return static_cast<uint64_t>(status);
        });

    patch_got_slot_entry(image_game, 0x806df0ULL, tramp, "CdStreamSync");
    std::cout << "  - Installed deadlock-free CdStreamSync() HLE hook (GOT 0x806df0).\n";
}

// Routes LogicalFS_OpenBundleFile calls through modloader/ and assets/ipad/ Retina overrides via AArch64 pre-hook.
static void setup_pc_logical_fs_hooks(const lcs::cpu::ElfImage& image_game,
                                      lcs::cpu::DynarmicCore& cpu) {
    auto& vfs = lcs::vfs::LogicalFS::instance();
    vfs.initialize("modloader", "assets/ipad");

    const uint64_t load_bias = image_game.get_load_bias();
    constexpr uint64_t kOpenBundleGotRva = 0x800198ULL;
    auto* got_slot = reinterpret_cast<uint64_t*>(load_bias + kOpenBundleGotRva);
    const uint64_t sym_open = image_game.find_symbol("_Z24LogicalFS_OpenBundleFilePKci");
    if (!sym_open) {
        std::cerr << "  [WARN] LogicalFS_OpenBundleFile symbol not found; skipping iPad/modloader VFS hook.\n";
        return;
    }

    void* vfs_page = nullptr;
#if defined(LCS_PLATFORM_WINDOWS)
    vfs_page = VirtualAlloc(reinterpret_cast<void*>(0x7f00030000ULL), 0x3000,
                            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!vfs_page) {
        vfs_page = VirtualAlloc(nullptr, 0x3000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
#endif
    if (!vfs_page) {
        std::cerr << "  [WARN] Failed to allocate guest VFS hook page.\n";
        return;
    }

    auto* base = reinterpret_cast<uint8_t*>(vfs_page);
    const uint64_t page_va = reinterpret_cast<uint64_t>(vfs_page);

    // 16 x 512-byte lock-free ring buffer starting at offset 0x100 for resolved guest path strings.
    constexpr size_t kSlotCount = 16;
    constexpr size_t kSlotStride = 512;
    constexpr size_t kRingOffset = 0x100;
    auto* ring_buf = reinterpret_cast<char*>(base + kRingOffset);
    const uint64_t ring_va = page_va + kRingOffset;
    auto slot_idx = std::make_shared<std::atomic<uint32_t>>(0);

    const uint64_t resolve_tramp = lcs::cpu::HLEManager::instance().register_function(
        "__pc_logical_fs_resolve",
        [&vfs, ring_buf, ring_va, slot_idx](const uint64_t* args) -> uint64_t {
            const char* guest_path = reinterpret_cast<const char*>(args[0]);
            if (!guest_path || !*guest_path) return args[0];

            const uint32_t idx = slot_idx->fetch_add(1, std::memory_order_relaxed) & (kSlotCount - 1);
            char* dst = ring_buf + idx * kSlotStride;
            const char* resolved = vfs.resolve_bundle_path(guest_path, dst, kSlotStride);
            if (resolved == dst) {
                return ring_va + idx * kSlotStride;
            }
            return args[0];
        });

#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    VirtualProtect(got_slot, sizeof(uint64_t), PAGE_READWRITE, &old_prot);
#endif
    const uint64_t orig_open = (*got_slot != 0) ? *got_slot : sym_open;
    *got_slot = page_va;
#if defined(LCS_PLATFORM_WINDOWS)
    VirtualProtect(got_slot, sizeof(uint64_t), old_prot, &old_prot);
#endif

    // AArch64 pre-hook: x0 = resolve_bundle_path(x0), w1 = preserved mode, then tail-branch/call orig_open.
    static constexpr uint32_t kPreHookInsns[] = {
        0xa9be53f3u, // stp x19, x20, [sp, #-32]!
        0xf9000bfeu, // str x30, [sp, #16]
        0xaa0003f3u, // mov x19, x0
        0x2a0103f4u, // mov w20, w1
        0x58000110u, // ldr x16, .+32 (resolve_tramp)
        0xd63f0200u, // blr x16
        0x2a1403e1u, // mov w1, w20
        0x580000f0u, // ldr x16, .+28 (orig_open)
        0xd63f0200u, // blr x16
        0xf9400bfeu, // ldr x30, [sp, #16]
        0xa8c253f3u, // ldp x19, x20, [sp], #32
        0xd65f03c0u, // ret
    };
    static_assert(sizeof(kPreHookInsns) == 48);
    std::memcpy(base, kPreHookInsns, sizeof(kPreHookInsns));
    auto* lits = reinterpret_cast<uint64_t*>(base + 48);
    lits[0] = resolve_tramp;
    lits[1] = orig_open;

    // Also reserve a persistent "." string at offset 0x80 for LogicalFS_AddBundleRoot(".").
    char* dot_str = reinterpret_cast<char*>(base + 0x80);
    dot_str[0] = '.';
    dot_str[1] = '\0';
    s_logical_fs_dot_va = page_va + 0x80;

    cpu.invalidate_cache(page_va, 0x100);
    std::cout << "  - Hooked LogicalFS_OpenBundleFile (iPad Retina PVRTC assets: "
              << vfs.ipad_asset_count() << ").\n";
}

int main(int argc, char* argv[]) {
    // Automatically detect project root directory (handles launching from build/bin/Release, Desktop shortcut, etc.)
    auto resolve_project_root = []() {
        if (std::filesystem::exists("gtalcs/gamefiles/android-libs/libGame.so")) {
            return;
        }
#if defined(LCS_PLATFORM_WINDOWS)
        wchar_t exe_buf[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
            std::filesystem::path cur = std::filesystem::path(exe_buf).parent_path();
            for (int i = 0; i < 5; i++) {
                if (std::filesystem::exists(cur / "gtalcs/gamefiles/android-libs/libGame.so")) {
                    std::filesystem::current_path(cur);
                    std::wcout << L"[Init] Switched working directory to: " << cur.wstring() << L"\n";
                    return;
                }
                if (!cur.has_parent_path() || cur == cur.parent_path()) break;
                cur = cur.parent_path();
            }
        }
#endif
    };
    resolve_project_root();

    lcs::core::load_config("gtalcs/lcs.conf");
    lcs::input::PCControls::instance().load_or_create("gtalcs/controls.ini");

    bool headless = false;
    bool dump_render = false;
    bool smoke_test = false;
    uint64_t max_frames = 0;
    uint32_t win_width = static_cast<uint32_t>(lcs::core::g_config.width);
    uint32_t win_height = static_cast<uint32_t>(lcs::core::g_config.height);

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--dump-render") {
            dump_render = true;
        } else if (arg == "--list-mods") {
            auto& vfs = lcs::vfs::LogicalFS::instance();
            vfs.initialize("modloader", "assets/ipad");
            const auto& mods = lcs::vfs::ModLoader::instance().mods();
            std::cout << "[VFS] ModLoader active packs (highest priority first): " << mods.size() << "\n";
            for (size_t m = mods.size(); m > 0; --m) {
                std::cout << "  #" << (mods.size() - m + 1) << " " << mods[m - 1].name
                          << " (priority=" << mods[m - 1].priority
                          << ", files=" << mods[m - 1].file_count << ")\n";
            }
            std::cout << "[VFS] iPad Retina assets indexed: " << vfs.ipad_asset_count() << "\n";
            lcs::vfs::mount_wad_archive("gtalcs/gamefiles/assets/data_main.wad");
            lcs::vfs::mount_wad_archive("gtalcs/gamefiles/assets/data_music.wad");
            return 0;
        } else if (arg == "--smoke-test") {
            smoke_test = true;
        } else if ((arg == "--max-frames" || arg == "--frames") && i + 1 < argc) {
            max_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if ((arg == "--fps" || arg == "--max-fps") && i + 1 < argc) {
            lcs::core::g_config.max_fps = std::atoi(argv[++i]);
        } else if (arg == "--vsync") {
            lcs::core::g_config.vsync = true;
        } else if (arg == "--no-vsync") {
            lcs::core::g_config.vsync = false;
        } else if (arg == "--fullscreen") {
            lcs::core::g_config.fullscreen = true;
        } else if (arg == "--windowed") {
            lcs::core::g_config.fullscreen = false;
        } else if (arg == "--width" && i + 1 < argc) {
            win_width = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            lcs::core::g_config.width = static_cast<int>(win_width);
        } else if (arg == "--height" && i + 1 < argc) {
            win_height = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            lcs::core::g_config.height = static_cast<int>(win_height);
        }
    }

#if defined(LCS_PLATFORM_WINDOWS)
    timeBeginPeriod(1);
#endif

#if defined(_WIN32)
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        // Suppress fast-fail on invalid CRT arguments (e.g. POSIX mode flags in fopen)
    });
    _set_thread_local_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        // Suppress fast-fail on invalid CRT arguments
    });
#endif

#if defined(LCS_PLATFORM_WINDOWS)
    // Use SetUnhandledExceptionFilter (not AddVectoredExceptionHandler) so Dynarmic SEH fastmem recovery runs first.
    SetUnhandledExceptionFilter(CrashHandler);
#endif

    std::cout << "====================================================\n";
    std::cout << " GTA: Liberty City Stories Native PC Port\n";
    std::cout << " ARM64 ELF64 Virtual Memory & Relocation Harness\n";
    std::cout << "====================================================\n\n";

    std::string base_dir = "gtalcs/gamefiles/android-libs";
    std::string game_so = base_dir + "/libGame.so";
    std::string openal_so = base_dir + "/libopenal.so";
    std::string mpg123_so = base_dir + "/libVendor_mpg123.so";

    if (!std::filesystem::exists(game_so)) {
        std::cerr << "[ERROR] Game binary not found: " << game_so << "\n";
#if defined(LCS_PLATFORM_WINDOWS)
        if (!headless) {
            std::wstring msg = L"Game binary not found:\n" + std::filesystem::absolute(game_so).wstring() +
                               L"\n\nPlease ensure game assets are extracted into gtalcs/gamefiles/.";
            MessageBoxW(nullptr, msg.c_str(), L"GTA: Liberty City Stories", MB_ICONERROR | MB_OK);
        }
#endif
        return 1;
    }

    std::cout << "[Step 1] Loading AArch64 ELF images into virtual memory...\n";

    lcs::cpu::ElfImage image_openal;
    lcs::cpu::ElfImage image_mpg123;
    lcs::cpu::ElfImage image_game;

    // Load OpenAL
    if (std::filesystem::exists(openal_so)) {
        if (image_openal.load(openal_so, 0x7f7a000000)) {
            std::cout << "  - Mapped libopenal.so at 0x" << std::hex << image_openal.get_load_bias()
                      << " (size: " << std::dec << (image_openal.get_image_size() / 1024) << " KB)\n";
        }
    }

    // Load mpg123
    if (std::filesystem::exists(mpg123_so)) {
        if (image_mpg123.load(mpg123_so, 0x7f81800000)) {
            std::cout << "  - Mapped libVendor_mpg123.so at 0x" << std::hex << image_mpg123.get_load_bias()
                      << " (size: " << std::dec << (image_mpg123.get_image_size() / 1024) << " KB)\n";
        }
    }

    // Load main game
    if (!image_game.load(game_so, 0x7f80000000)) {
        std::cerr << "[ERROR] Failed to map libGame.so into virtual memory.\n";
        return 1;
    }
    std::cout << "  - Mapped libGame.so at 0x" << std::hex << image_game.get_load_bias()
              << " (size: " << std::dec << (image_game.get_image_size() / (1024 * 1024)) << " MB)\n\n";

    std::cout << "[Step 2] Testing exported symbol resolution in libGame.so...\n";
    const char* test_exports[] = {
        "JNI_OnLoad",
        "Java_com_rockstargames_gtalcs_GTAJNIlib_setAssetPacksInfo",
        "Java_com_rockstargames_gtalcs_GTAJNIlib_setPrivateFilesDir",
        "Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceCreated",
        "Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceChanged",
        "Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnDrawFrame",
        "_Z15_LogicalFS_InitRKN8Platform16C_GameConfigBaseE",
        "_Z38LogicalFS_MountWadFromPhysicalFilePathRKNSt6__ndk112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEEPKc"
    };

    for (const char* sym_name : test_exports) {
        uint64_t addr = image_game.find_symbol(sym_name);
        if (addr != 0) {
            std::cout << "  [FOUND] 0x" << std::hex << addr << " -> " << sym_name << "\n";
        } else {
            std::cout << "  [FAIL]  Could not locate " << sym_name << "\n";
        }
    }

    std::cout << "\n[Step 3] Processing ELF relocations...\n";
    lcs::os::init_bionic_libc();
    lcs::cpu::HLEManager::instance().initialize();

    // Symbol resolver cross-referencing between openal, mpg123, and HLE trampoline table
    auto resolver = [&](const char* name, uint8_t binding) -> uint64_t {
        (void)binding;
        uint64_t addr = 0;
        if (lcs::audio::is_openal_symbol(name)) {
            return lcs::cpu::HLEManager::instance().resolve(name);
        }
        if (image_openal.is_loaded()) {
            addr = image_openal.find_symbol(name);
            if (addr) return addr;
        }
        if (image_mpg123.is_loaded()) {
            addr = image_mpg123.find_symbol(name);
            if (addr) return addr;
        }
        uint64_t bionic_data = lcs::os::resolve_bionic_libc_data(name);
        if (bionic_data) return bionic_data;

        return lcs::cpu::HLEManager::instance().resolve(name);
    };

    lcs::cpu::RelocationStats openal_stats;
    if (image_openal.is_loaded()) {
        image_openal.relocate(resolver, &openal_stats);
        std::cout << "  - Relocated libopenal.so: relative=" << openal_stats.relative
                  << ", jmps=" << openal_stats.jump_slots << "\n";
    }

    lcs::cpu::RelocationStats mpg123_stats;
    if (image_mpg123.is_loaded()) {
        image_mpg123.relocate(resolver, &mpg123_stats);
        std::cout << "  - Relocated libVendor_mpg123.so: relative=" << mpg123_stats.relative
                  << ", jmps=" << mpg123_stats.jump_slots << "\n";
    }

    lcs::cpu::RelocationStats game_stats;
    image_game.relocate(resolver, &game_stats);

    std::cout << std::dec;
    std::cout << "  libGame.so Relocations:\n";
    std::cout << "    - Relative (R_AARCH64_RELATIVE):  " << game_stats.relative << "\n";
    std::cout << "    - Absolute (R_AARCH64_ABS64):     " << game_stats.absolute << "\n";
    std::cout << "    - Global Data (R_AARCH64_GLOB_DAT): " << game_stats.global_data << "\n";
    std::cout << "    - Jump Slots (R_AARCH64_JUMP_SLOT):  " << game_stats.jump_slots << "\n";
    std::cout << "    - Unresolved:                     " << game_stats.unresolved << "\n\n";

    std::cout << "[Step 4] Checking constructor initializers (.init_array)...\n";
    auto inits = image_game.get_init_array();
    std::cout << "  Found " << inits.size() << " constructor functions in libGame.so.\n";
    if (!inits.empty()) {
        std::cout << "  First constructor at: 0x" << std::hex << inits[0] << std::dec << "\n";
        std::cout << "  Last constructor at:  0x" << std::hex << inits.back() << std::dec << "\n";
    }

    std::cout << "\n[Step 5] Initializing Dynarmic ARM64 JIT CPU Core...\n";
    lcs::cpu::DynarmicCore cpu;
    if (!cpu.initialize()) {
        std::cerr << "[ERROR] Failed to initialize Dynarmic ARM64 JIT.\n";
        return 1;
    }
    std::cout << "  - Dynarmic ARM64 JIT engine online.\n";
    s_active_cpu = &cpu;
    lcs::cpu::HLEManager::instance().wire_to_cpu(cpu);

    // Allocate a test page for guest code execution verification
    void* test_page = VirtualAlloc(reinterpret_cast<void*>(0x7f00010000ULL), 0x1000,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (test_page) {
        cpu.invalidate_cache(0x7f00010000ULL, 0x1000);
        uint32_t* test_code = reinterpret_cast<uint32_t*>(test_page);
        // ARM64:
        // mov w0, #42       (0x52800540)
        // add w0, w0, #58   (0x1100e800)  -> 42 + 58 = 100
        // ret               (0xd65f03c0)
        test_code[0] = 0x52800540;
        test_code[1] = 0x1100e800;
        test_code[2] = 0xd65f03c0;

        uint64_t result = cpu.call(0x7f00010000ULL);
        std::cout << "  - ARM64 JIT Execution Test (42 + 58): Result = " << result
                  << (result == 100 ? " [PASS]" : " [FAIL]") << "\n";
    }

    // Test SVC Dispatch
    bool svc_handled = false;
    cpu.register_svc_handler(0x42, [&](uint32_t swi, const uint64_t* args) -> uint64_t {
        svc_handled = true;
        return args[0] * 2; // return x0 * 2
    });

    if (test_page) {
        cpu.invalidate_cache(0x7f00010000ULL, 0x1000);
        uint32_t* test_code = reinterpret_cast<uint32_t*>(test_page);
        // ARM64:
        // mov x0, #21       (0xd28002a0)
        // svc #0x42         (0xd4000841)
        // ret               (0xd65f03c0)
        test_code[0] = 0xd28002a0;
        test_code[1] = 0xd4000001 | (0x42 << 5);
        test_code[2] = 0xd65f03c0;

        uint64_t result = cpu.call(0x7f00010000ULL);
        std::cout << "  - ARM64 SVC HLE Trap Test (SVC #0x42, 21 * 2): Result = " << result
                  << (result == 42 && svc_handled ? " [PASS]" : " [FAIL]") << "\n";
    }

    auto patch_guest_word = [&](const char* sym, size_t off, uint32_t expected, uint32_t replacement, const char* desc) -> bool {
        uint64_t fn = image_game.find_symbol(sym);
        if (!fn) return false;
        auto* insn = reinterpret_cast<uint32_t*>(fn + off);
        if (*insn != expected) return false;
#if defined(LCS_PLATFORM_WINDOWS)
        DWORD old_prot = 0;
        VirtualProtect(insn, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_prot);
#endif
        *insn = replacement;
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualProtect(insn, sizeof(uint32_t), old_prot, &old_prot);
#endif
        cpu.invalidate_cache(fn + off, sizeof(uint32_t));
        if (desc) {
            std::cout << "  - " << desc << "\n";
        }
        return true;
    };

    // Relax CTimer::Update() minimum frame-step clamp from 0.5f (100 FPS ceiling) to 0.125f (400 FPS)
    // so 120/240 FPS runs keep exact 1.0x simulation speed instead of running sped-up.
    if (uint64_t fn_timer_upd = image_game.find_symbol("_ZN6CTimer6UpdateEv")) {
        auto* insn_cmp = reinterpret_cast<uint32_t*>(fn_timer_upd + 0xec);
        auto* insn_mov = reinterpret_cast<uint32_t*>(fn_timer_upd + 0x134);
        auto* insn_fmov = reinterpret_cast<uint32_t*>(fn_timer_upd + 0x138);
        if (*insn_cmp == 0x1e2c1004u && *insn_mov == 0x52a7e00au && *insn_fmov == 0x1e2c1000u) {
#if defined(LCS_PLATFORM_WINDOWS)
            DWORD old_prot = 0;
            VirtualProtect(insn_cmp, 0x60, PAGE_EXECUTE_READWRITE, &old_prot);
#endif
            *insn_cmp = 0x1e281004u;  // fmov s4, #0.125
            *insn_mov = 0x52a7c00au;  // mov  w10, #0x3e000000 (0.125f)
            *insn_fmov = 0x1e281000u; // fmov s0, #0.125
#if defined(LCS_PLATFORM_WINDOWS)
            VirtualProtect(insn_cmp, 0x60, old_prot, &old_prot);
#endif
            cpu.invalidate_cache(fn_timer_upd, 0x200);
            std::cout << "  - Applied high-FPS CTimer::Update floor relaxation (0.5f -> 0.125f, up to 400 FPS).\n";
        }
    }

    // Relax CTimer::Update() low-FPS ceiling (3.0f / +60ms -> 6.0f / +120ms, down to 8.33 FPS)
    // and cTime::Refresh() ceiling (0.1f -> 0.125f) so <16.67 FPS frames keep 1.0x real-time speed.
    patch_guest_word("_ZN6CTimer6UpdateEv", 0x164, 0x1e211001u, 0x1e231001u,
                     "Applied low-FPS CTimer::Update step ceiling relaxation (3.0f -> 6.0f, 120ms).");
    patch_guest_word("_ZN6CTimer6UpdateEv", 0x168, 0x1100f14au, 0x1101e14au,
                     "Applied low-FPS CTimer::Update ms ceiling relaxation (+60ms -> +120ms).");
    patch_guest_word("_ZN5cTime7RefreshEv", 0x30, 0x529999a8u, 0x52800008u,
                     nullptr);
    patch_guest_word("_ZN5cTime7RefreshEv", 0x34, 0x72a7b988u, 0x72a7c008u,
                     "Applied low-FPS cTime::Refresh ceiling relaxation (0.1f -> 0.125f, 8 FPS).");

    // Fix CCutsceneObject::ProcessControl broken low-timeStep fallback branch at >100 FPS (including >1000 FPS fade-in frames):
    // Convert `if (ms_fTimeStep < 0.5f)` into `if (ms_fTimeStep <= 0.0f)` so positive timesteps always integrate m_vecMoveSpeed * ms_fTimeStep,
    // and zero displacement cleanly when ms_fTimeStep <= 0.0f instead of doubling velocity on high-FPS frames.
    patch_guest_word("_ZN15CCutsceneObject14ProcessControlEv", 0x184, 0x1e212000u, 0x1e202008u,
                     "Applied high-FPS CCutsceneObject::ProcessControl fix (1/4: fcmp s0, #0.0).");
    patch_guest_word("_ZN15CCutsceneObject14ProcessControlEv", 0x188, 0x54000845u, 0x5400084cu,
                     "Applied high-FPS CCutsceneObject::ProcessControl fix (2/4: b.gt integrate).");
    patch_guest_word("_ZN15CCutsceneObject14ProcessControlEv", 0x194, 0x0e21d422u, 0x4f00e402u,
                     "Applied high-FPS CCutsceneObject::ProcessControl fix (3/4: movi v2.16b, #0).");
    patch_guest_word("_ZN15CCutsceneObject14ProcessControlEv", 0x198, 0x1e232861u, 0x1e2703e1u,
                     "Applied high-FPS CCutsceneObject::ProcessControl fix (4/4: fmov s1, wzr).");
    // Fix CPed::CalculateNewVelocity broken low-timeStep (<0.5f, i.e. >100 FPS) branch that multiplied ped displacement
    // by 0.5 instead of (1.0f / ms_fTimeStep), causing peds to walk/run at half speed above 100 FPS.
    // +0x124: fmov s3, #0.5 -> movi d3, #0 (0.0f); +0x144: b.pl -> b.gt (only take 0 branch when ms_fTimeStep <= 0.0f).
    patch_guest_word("_ZN4CPed20CalculateNewVelocityEv", 0x124, 0x1e2c1003u, 0x2f00e403u,
                     "Applied high-FPS CPed::CalculateNewVelocity step threshold fix (0.5f -> 0.0f).");
    patch_guest_word("_ZN4CPed20CalculateNewVelocityEv", 0x144, 0x54000085u, 0x5400008cu,
                     "Applied high-FPS CPed::CalculateNewVelocity branch fix (b.pl -> b.gt).");

    // Remove the 500 ms AL_SOURCE_STATE polling cache in cSampleManager::GetChannelUsedFlag() so
    // gear-shift/engine samples on channel 23 (0x1593+x) and ActiveQueues detect completion immediately
    // instead of leaving 250-500 ms silent gaps between engine intensity stages.
    patch_guest_word("_ZN14cSampleManager18GetChannelUsedFlagEj", 0x6c, 0x540001a2u, 0xd503201fu,
                     "Removed 500 ms GetChannelUsedFlag cache for immediate engine/SFX state transitions.");

    // Remove the 0.125f per-axis axial chop in Java_..._setJoyAxis (0x56c06c: fmov s2, #0.125 -> movi d2, #0)
    // because host GamepadDriver & InputSystem already apply a smooth scaled radial deadzone (eliminates cardinal snapping).
    patch_guest_word("Java_com_rockstargames_gtalcs_GTAJNIlib_setJoyAxis", 0x4, 0x1e281002u, 0x2f00e402u,
                     "Replaced setJoyAxis per-axis axial threshold with smooth host radial deadzone.");

    setup_pc_display_menu_options(image_game, cpu, headless);
    setup_pc_ctimer_hook(image_game);
    setup_pc_cdstream_hook(image_game);
    lcs::game::LCSFreeAim::instance().install(image_game, cpu);
    lcs::game::LCSPs2Effects::instance().install(image_game, cpu);
    lcs::game::LCSPrompts::instance().init(image_game);
    setup_pc_gamepad_rumble_hooks(image_game, headless);
    setup_pc_logical_fs_hooks(image_game, cpu);

    lcs::settings::load("lcs.ini");
    if (const char* diag_sub = std::getenv("LCS_DIAG_SUB_LANG"); diag_sub && *diag_sub) {
        lcs::settings::get().game_language = std::clamp(std::atoi(diag_sub), 0, 7);
    }
    lcs::game::LcsDistance::instance().install(image_game, cpu);
    lcs::game::LcsMemory::instance().install(image_game, cpu);
    lcs::game::LcsWorldHd::instance().initialize(image_game, cpu, false);

    std::cout << "\n[Step 6] Running guest constructors in libGame.so (.init_array)...\n";
    size_t executed = 0;
    for (size_t i = 0; i < inits.size(); i++) {
        std::cout << "  [" << i << "] Constructor at 0x" << std::hex << inits[i] << "..." << std::flush;
        cpu.call(inits[i]);
        std::cout << " OK\n";
        executed++;
    }
    std::cout << "  - All " << executed << " constructors executed successfully! [PASS]\n\n";

    std::cout << "[Step 7] Initializing JNI 1.6 Bridge & Invoking JNI_OnLoad...\n";
    // Initialize TLS slots for JNIEnv
    *reinterpret_cast<uint64_t*>(0x7f60200130ULL) = lcs::os::JNI_GUEST_ENV_ADDR;
    *reinterpret_cast<uint32_t*>(0x7f60200140ULL) = 1;

    if (!lcs::os::JNIBridge::instance().initialize(cpu, lcs::cpu::HLEManager::instance())) {
        std::cerr << "[ERROR] Failed to initialize JNI Bridge.\n";
        return 1;
    }
    // Re-wire CPU to capture newly registered JNI HLE trampolines
    lcs::cpu::HLEManager::instance().wire_to_cpu(cpu);

    uint64_t jni_onload = image_game.find_symbol("JNI_OnLoad");
    if (!jni_onload) {
        std::cerr << "[ERROR] JNI_OnLoad symbol not found in libGame.so.\n";
        return 1;
    }

    uint64_t vm_addr = lcs::os::JNIBridge::instance().get_vm_address();
    std::cout << "  - Calling JNI_OnLoad(vm=0x" << std::hex << vm_addr << ", reserved=0) at 0x" << jni_onload << "...\n" << std::dec;
    uint64_t jni_ver = cpu.call(jni_onload, {vm_addr, 0});
    std::cout << "  - JNI_OnLoad returned: 0x" << std::hex << jni_ver
              << (jni_ver == 0x00010006 ? " (JNI_VERSION_1_6) [PASS]" : " [UNEXPECTED]")
              << std::dec << "\n\n";

    std::cout << "[Step 8] Executing Android JNI Game Bootstrap Sequence...\n";
    uint64_t env_addr = lcs::os::JNIBridge::instance().get_env_address();
    uint64_t dummy_clazz = lcs::os::JNI_GUEST_CLASS_BASE;

    // 1. setPrivateFilesDir
    std::filesystem::create_directories("gtalcs/userdata");
    uint64_t fn_set_private_dir = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setPrivateFilesDir");
    if (fn_set_private_dir) {
        uint64_t str_dir = lcs::os::JNIBridge::instance().create_string_utf("gtalcs/userdata/");
        std::cout << "  [1] Invoking setPrivateFilesDir(\"gtalcs/userdata/\")..." << std::flush;
        cpu.call(fn_set_private_dir, {env_addr, dummy_clazz, str_dir});
        std::cout << " OK\n";
    }

    // 1.2 Register project root "." ahead of asset packs so modloader/ and assets/ipad/ override base WADs.
    if (s_logical_fs_dot_va != 0) {
        if (uint64_t fn_add_root = image_game.find_symbol("_Z23LogicalFS_AddBundleRootPKc")) {
            std::cout << "  [1.2] Invoking LogicalFS_AddBundleRoot(\".\")..." << std::flush;
            cpu.call(fn_add_root, {s_logical_fs_dot_va});
            std::cout << " OK\n";
        }
    }

    // 1.5 setAssetPacksInfo
    uint64_t fn_set_asset_packs = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setAssetPacksInfo");
    if (fn_set_asset_packs) {
        auto& jni = lcs::os::JNIBridge::instance();
        uint64_t name_main = jni.create_string_utf("data_main");
        uint64_t name_music = jni.create_string_utf("data_music");
        uint64_t path_base = jni.create_string_utf("gtalcs/gamefiles");

        uint64_t arr_names = jni.create_object_array({name_main, name_music});
        uint64_t arr_paths = jni.create_object_array({path_base, path_base});

        std::cout << "  [1.5] Invoking setAssetPacksInfo([\"data_main\", \"data_music\"])..." << std::flush;
        cpu.call(fn_set_asset_packs, {env_addr, dummy_clazz, arr_names, arr_paths});
        std::cout << " OK\n";
    }

    // 2. initCloud
    uint64_t fn_init_cloud = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_initCloud");
    if (fn_init_cloud) {
        uint64_t str_prod = lcs::os::JNIBridge::instance().create_string_utf("prod");
        uint64_t str_empty = lcs::os::JNIBridge::instance().create_string_utf("");
        std::cout << "  [2] Invoking initCloud(\"prod\", \"\")..." << std::flush;
        cpu.call(fn_init_cloud, {env_addr, dummy_clazz, str_prod, str_empty});
        std::cout << " OK\n";
    }

    // 3. setIsTVDevice
    uint64_t fn_set_is_tv = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setIsTVDevice");
    if (fn_set_is_tv) {
        std::cout << "  [3] Invoking setIsTVDevice(true)..." << std::flush;
        cpu.call(fn_set_is_tv, {env_addr, dummy_clazz, 1});
        std::cout << " OK\n";
    }

    // 4. setHasVibrator
    if (!headless) {
        lcs::input::GamepadDriver::instance().initialize();
    }
    const bool has_vibrator = !headless && lcs::input::GamepadDriver::instance().get_caps().has_rumble;
    uint64_t fn_set_vibrator = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setHasVibrator");
    if (fn_set_vibrator) {
        std::cout << "  [4] Invoking setHasVibrator(" << (has_vibrator ? "true" : "false") << ")..." << std::flush;
        cpu.call(fn_set_vibrator, {env_addr, dummy_clazz, has_vibrator ? 1ULL : 0ULL});
        std::cout << " OK\n";
    }

    // 5. setOSVersion
    uint64_t fn_set_os_ver = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setOSVersion");
    if (fn_set_os_ver) {
        std::cout << "  [5] Invoking setOSVersion(28)..." << std::flush;
        cpu.call(fn_set_os_ver, {env_addr, dummy_clazz, 28});
        std::cout << " OK\n";
    }

    // 6. setDeviceInfo
    uint64_t fn_set_device_info = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setDeviceInfo");
    if (fn_set_device_info) {
        uint64_t str_shield = lcs::os::JNIBridge::instance().create_string_utf("SHIELD");
        uint64_t str_x86 = lcs::os::JNIBridge::instance().create_string_utf("x86_64");
        std::cout << "  [6] Invoking setDeviceInfo(4096000, \"SHIELD\", \"SHIELD\", \"x86_64\")..." << std::flush;
        cpu.call(fn_set_device_info, {env_addr, dummy_clazz, 4096000, str_shield, str_shield, str_x86});
        std::cout << " OK\n";
    }

    // 7. markInitialized
    uint64_t fn_mark_init = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_markInitialized");
    if (fn_mark_init) {
        std::cout << "  [7] Invoking markInitialized()..." << std::flush;
        cpu.call(fn_mark_init, {env_addr, dummy_clazz});
        std::cout << " OK\n";
    }

    // 8. isInitialized check
    uint64_t fn_is_init = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_isInitialized");
    if (fn_is_init) {
        uint64_t is_init = cpu.call(fn_is_init, {env_addr, dummy_clazz});
        std::cout << "  [8] isInitialized() -> " << (is_init ? "TRUE [PASS]" : "FALSE [FAIL]") << "\n";
    }

    std::cout << "\n[Step 9] Initializing Native Desktop Window, Direct3D 12 Swapchain & Audio...\n";
    lcs::audio::init_audio_system();
    void* native_window = nullptr;
    if (!headless) {
        if (!lcs::graphics::create_game_window(
                static_cast<int>(win_width),
                static_cast<int>(win_height),
                "GTA: Liberty City Stories - Native PC Port (D3D12)",
                lcs::core::g_config.fullscreen)) {
            std::cerr << "[ERROR] Failed to create native game window.\n";
            return 1;
        }
        lcs::graphics::get_window_size(win_width, win_height);
        native_window = lcs::graphics::get_native_window_handle();
    }

    if (!lcs::graphics::RenderBridge::instance().initialize(
            lcs::graphics::RenderBackend::Direct3D12,
            native_window,
            win_width,
            win_height)) {
        std::cerr << "[ERROR] Failed to initialize RenderBridge.\n";
        return 1;
    }
    lcs::graphics::RenderBridge::instance().set_vsync_enabled(lcs::core::g_config.vsync);
    std::cout << "  - Display pacing: vsync=" << (lcs::core::g_config.vsync ? "on" : "off")
              << ", max_fps=" << lcs::core::g_config.max_fps << "\n";

    // 9. viewOnSurfaceCreated
    uint64_t fn_surf_created = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceCreated");
    if (fn_surf_created) {
        std::cout << "  [9] Invoking viewOnSurfaceCreated()..." << std::flush;
        cpu.call(fn_surf_created, {env_addr, dummy_clazz});
        std::cout << " OK\n";
    }

    // Cap logical guest surface to <=1920x1080 (and >=720px tall when aspect fits) so libGame.so
    // renders directly to FBO 0 while RenderBridge scales viewports/scissors to native swapchain size.
    uint32_t guest_width = win_width;
    uint32_t guest_height = win_height;
    compute_guest_dimensions(win_width, win_height, guest_width, guest_height);
    lcs::graphics::RenderBridge::instance().set_guest_dimensions(guest_width, guest_height);
    UltrawideScaleState* uw_scale_state =
        setup_pc_ultrawide_menu_hooks(image_game, cpu, guest_width, guest_height);
    volatile uint32_t* audio_sfx_allow = setup_pc_audio_timing_hooks(image_game, cpu);
    lcs::game::install_physics_timing_hooks(image_game, cpu);
    lcs::subtitles::install_hooks(image_game, cpu);
    lcs::game::LCSTrainer::instance().install_brief_hooks(image_game, cpu);

    const uint64_t sym_width = image_game.find_symbol("Width");
    const uint64_t sym_height = image_game.find_symbol("Height");
    const uint64_t sym_video_w = image_game.find_symbol("_ZN7Display11gVideoWidthE");
    const uint64_t sym_video_h = image_game.find_symbol("_ZN7Display12gVideoHeightE");
    const uint64_t sym_aspect = image_game.find_symbol("_ZN7Display7gAspectE");
    const uint64_t sym_backbuf_target = image_game.find_symbol("_ZN7Display17gBackBufferTargetE");
    const uint64_t sym_touchscreen = image_game.find_symbol("GTouchscreen");
    const uint64_t sym_prefs_show_pad_input = image_game.find_symbol("_ZN12CMenuManager19m_PrefsShowPadInputE");
    const uint64_t sym_prefs_brightness = image_game.find_symbol("_ZN12CMenuManager17m_PrefsBrightnessE");
    const uint64_t sym_prefs_invert_look = image_game.find_symbol("_ZN12CMenuManager17m_PrefsInvertLookE");
    const uint64_t sym_con_slider = image_game.get_load_bias() + 0x81fd00ULL;
    const uint64_t sym_sat_slider = image_game.get_load_bias() + 0x81fd04ULL;
    const uint64_t sym_btn_defaults = image_game.find_symbol("ButtonDefaults");
    const uint64_t sym_btn_tablet = image_game.find_symbol("ButtonDefaultsTablet");

    // Native PC port: never display Android on-screen touch buttons (even on initial load or after menu mouse clicks).
    // Touchscreen::Render() skips all 54 mobile touch buttons when GTouchscreen+0x8 != 0 and m_PrefsShowPadInput == 0.
    // NOP the three stores in Touchscreen::Update() that clear GTouchscreen+0x8 back to 0.
    {
        const uint64_t lb = image_game.get_load_bias();
        struct TouchPatch { uint64_t rva; uint32_t expected; };
        constexpr TouchPatch kTouchHidePatches[] = {
            {0x547088ULL, 0x3900227fu}, // strb wzr, [x19, #0x8]
            {0x5470a4ULL, 0x39002268u}, // strb w8,  [x19, #0x8]
            {0x547ab4ULL, 0x3900227fu}, // strb wzr, [x19, #0x8]
        };
        int patched = 0;
        for (const auto& p : kTouchHidePatches) {
            const uint64_t va = lb + p.rva;
            uint32_t cur = 0;
            std::memcpy(&cur, reinterpret_cast<const void*>(va), sizeof(cur));
            if (cur == p.expected) {
                constexpr uint32_t kArm64Nop = 0xd503201fu;
#if defined(_WIN32)
                DWORD old_prot = 0;
                VirtualProtect(reinterpret_cast<void*>(va), sizeof(kArm64Nop), PAGE_EXECUTE_READWRITE, &old_prot);
#endif
                std::memcpy(reinterpret_cast<void*>(va), &kArm64Nop, sizeof(kArm64Nop));
#if defined(_WIN32)
                VirtualProtect(reinterpret_cast<void*>(va), sizeof(kArm64Nop), old_prot, &old_prot);
#endif
                cpu.invalidate_cache(va, sizeof(kArm64Nop));
                ++patched;
            }
        }
        if (patched == 3) {
            std::cout << "  - Disabled Android touchscreen button overlay permanently for native PC UI.\n";
        }
    }

    // Native PC/PS2 HUD: let CHud::GetRadarPosY() and CHud::Draw() radar ring honor btn24->y ([x9, #0xc])
    // even when OS_IsTVDevice() is true (otherwise TV overscan code overrides Y from btn24->x).
    {
        const uint64_t lb = image_game.get_load_bias();
        struct RadarYPatch { uint64_t rva; uint32_t expected; uint32_t replacement; };
        constexpr RadarYPatch kRadarYPatches[] = {
            {0x484188ULL, 0x36000160u, 0x1400000bu}, // CHud::GetRadarPosY: b 0x4841b4
            {0x48119cULL, 0x36000120u, 0x14000009u}, // CHud::Draw ring top Y: b 0x4811c0
            {0x481254ULL, 0x36000120u, 0x14000009u}, // CHud::Draw ring bottom Y: b 0x481278
        };
        int patched = 0;
        for (const auto& p : kRadarYPatches) {
            const uint64_t va = lb + p.rva;
            uint32_t cur = 0;
            std::memcpy(&cur, reinterpret_cast<const void*>(va), sizeof(cur));
            if (cur == p.expected) {
#if defined(_WIN32)
                DWORD old_prot = 0;
                VirtualProtect(reinterpret_cast<void*>(va), sizeof(p.replacement), PAGE_EXECUTE_READWRITE, &old_prot);
#endif
                std::memcpy(reinterpret_cast<void*>(va), &p.replacement, sizeof(p.replacement));
#if defined(_WIN32)
                VirtualProtect(reinterpret_cast<void*>(va), sizeof(p.replacement), old_prot, &old_prot);
#endif
                cpu.invalidate_cache(va, sizeof(p.replacement));
                ++patched;
            }
        }
        if (patched == 3) {
            std::cout << "  - Repositioned HUD radar to classic PC/PS2 bottom-left corner.\n";
        }
    }

    uint32_t current_guest_w = guest_width;
    uint32_t current_guest_h = guest_height;
    uint64_t last_ts_obj = 0;
    uint32_t last_ts_gw = 0;
    uint32_t last_ts_gh = 0;

    auto sync_touchscreen_hud_layout = [&](uint32_t gw, uint32_t gh, bool force) {
        if (sym_touchscreen && sym_btn_defaults && sym_btn_tablet && gw > 0 && gh > 0) {
            const uint64_t ts_obj = *reinterpret_cast<const uint64_t*>(sym_touchscreen);
            auto* ts_base = reinterpret_cast<uint8_t*>(ts_obj);
            if (ts_base && *reinterpret_cast<const uint64_t*>(ts_base + 0xe8) != 0) {
                if (!force && ts_obj == last_ts_obj && gw == last_ts_gw && gh == last_ts_gh) {
                    return;
                }
                last_ts_obj = ts_obj;
                last_ts_gw = gw;
                last_ts_gh = gh;

                const float aspect = static_cast<float>(gw) / static_cast<float>(gh);
                const float sx = static_cast<float>(gw) / 1280.0f;
                const float sy = static_cast<float>(gh) / 720.0f;
                *reinterpret_cast<float*>(ts_base + 0x4b0) = sx;
                *reinterpret_cast<float*>(ts_base + 0x4b4) = sy;
                *reinterpret_cast<float*>(ts_base + 0x4b8) = 0.5f * (sx + sy);

                auto* tbl = reinterpret_cast<const uint8_t*>(sym_btn_tablet);
                auto* def = reinterpret_cast<uint8_t*>(sym_btn_defaults);
                auto tbl_f = [tbl](size_t off) -> float {
                    return *reinterpret_cast<const float*>(tbl + off);
                };
                auto update_btn_x = [ts_base, def](size_t idx, float new_x) {
                    *reinterpret_cast<float*>(def + idx * 0x28 + 0x08) = new_x;
                    for (size_t arr_off : {size_t(0x20), size_t(0x1d0)}) {
                        const uint64_t btn = *reinterpret_cast<const uint64_t*>(ts_base + arr_off + idx * 8);
                        if (btn) {
                            *reinterpret_cast<float*>(btn + 0x08) = new_x;
                        }
                    }
                };
                constexpr float kAspectNorm = 1.4285715f; // 10.0f / 7.0f
                constexpr float kPcRadarY = 525.0f;       // Bottom-left PC/PS2 radar placement (720p ref space)
                const float summary_x = 1280.0f - (342.85715f / aspect) - 12.8f;
                const float timer_x = summary_x - (414.28574f / aspect);
                const float btn21_x = tbl_f(0x198) + ((tbl_f(0x1a0) - tbl_f(0x358)) * 0.5f * kAspectNorm) / aspect;
                const float btn22_x = tbl_f(0x1c0) + ((tbl_f(0x1c8) - tbl_f(0x380)) * 0.5f * kAspectNorm) / aspect;
                *reinterpret_cast<float*>(def + 24 * 0x28 + 0x0c) = kPcRadarY;
                for (size_t arr_off : {size_t(0x20), size_t(0x1d0)}) {
                    const uint64_t rbtn = *reinterpret_cast<const uint64_t*>(ts_base + arr_off + 24 * 8);
                    if (rbtn) {
                        *reinterpret_cast<float*>(rbtn + 0x0c) = kPcRadarY;
                    }
                }
                update_btn_x(25, summary_x);
                update_btn_x(48, timer_x);
                update_btn_x(21, btn21_x);
                update_btn_x(22, btn22_x);
            }
        }
    };

    auto apply_runtime_resolution = [&](uint32_t surface_w, uint32_t surface_h) {
        if (surface_w == 0 || surface_h == 0) return;
        uint32_t gw = surface_w;
        uint32_t gh = surface_h;
        compute_guest_dimensions(surface_w, surface_h, gw, gh);
        current_guest_w = gw;
        current_guest_h = gh;

        auto& rb = lcs::graphics::RenderBridge::instance();
        rb.set_guest_dimensions(gw, gh);
        rb.set_surface_dimensions(surface_w, surface_h);

        const float aspect = static_cast<float>(gw) / static_cast<float>(gh);
        if (sym_width) *reinterpret_cast<int32_t*>(sym_width) = static_cast<int32_t>(gw);
        if (sym_height) *reinterpret_cast<int32_t*>(sym_height) = static_cast<int32_t>(gh);
        if (sym_video_w) *reinterpret_cast<int32_t*>(sym_video_w) = static_cast<int32_t>(gw);
        if (sym_video_h) *reinterpret_cast<int32_t*>(sym_video_h) = static_cast<int32_t>(gh);
        if (sym_aspect) {
            *reinterpret_cast<float*>(sym_aspect) = aspect;
        }
        if (sym_backbuf_target) {
            const uint64_t target = *reinterpret_cast<const uint64_t*>(sym_backbuf_target);
            if (target) {
                // C_RenderTarget starts with C_RenderTargetDesc: +0 wRef, +8 wVal, +0x10 hRef, +0x18 hVal.
                if (*reinterpret_cast<const uint64_t*>(target + 0x00) == 0) {
                    *reinterpret_cast<float*>(target + 0x08) = static_cast<float>(gw);
                }
                if (*reinterpret_cast<const uint64_t*>(target + 0x10) == 0) {
                    *reinterpret_cast<float*>(target + 0x18) = static_cast<float>(gh);
                }
            }
        }
        // Refresh aspect-anchored HUD/Touchscreen layout (Summary, Timer, Weapon) cached at Touchscreen::Initialize.
        sync_touchscreen_hud_layout(gw, gh, true);
        update_pc_ultrawide_menu_scales(uw_scale_state, gw, gh);
        std::cout << "[Resize] Surface=" << surface_w << "x" << surface_h
                  << " Guest=" << gw << "x" << gh
                  << " (menuX=" << (uw_scale_state ? uw_scale_state->menu_scale_x : 1.0f)
                  << ", hudX=" << (uw_scale_state ? uw_scale_state->hud_scale_x : 1.0f) << ")\n"
                  << std::flush;
    };

    // 10. viewOnSurfaceChanged
    uint64_t fn_surf_changed = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceChanged");
    if (fn_surf_changed) {
        std::cout << "  [10] Invoking viewOnSurfaceChanged(" << guest_width << ", " << guest_height << ")"
                  << " [swapchain=" << win_width << "x" << win_height << "]..." << std::flush;
        cpu.call(fn_surf_changed, {env_addr, dummy_clazz, guest_width, guest_height});
        std::cout << " OK\n";
    }

    // 10.5 viewOnResume
    uint64_t fn_resume = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnResume");
    if (fn_resume) {
        std::cout << "  [10.5] Invoking viewOnResume()..." << std::flush;
        cpu.call(fn_resume, {env_addr, dummy_clazz});
        std::cout << " OK\n";
    }

    // 11. Continuous Render & Game Loop (viewOnDrawFrame)
    uint64_t fn_draw_frame = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnDrawFrame");
    uint64_t fn_playlist_finish = image_game.find_symbol("Java_com_rockstargames_gtalcs_CommonAPI_HandlePlaylistFinishInit");
    uint64_t fn_game_loaded = image_game.find_symbol("Java_com_rockstargames_gtalcs_RockstarJNIlib_GameLoaded");
    uint64_t fn_start_before_load = image_game.find_symbol("Java_com_rockstargames_gtalcs_RockstarJNIlib_StartGameBeforeLoad");
    uint64_t fn_start_game = image_game.find_symbol("Java_com_rockstargames_gtalcs_RockstarJNIlib_StartGame");
    uint64_t fn_video_finished = image_game.find_symbol("Java_com_rockstargames_hal_andVideo_VideoFinishedCB");
    uint64_t fn_touch_start = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onTouchStart");
    uint64_t fn_touch_move = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onTouchMove");
    uint64_t fn_touch_end = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onTouchEnd");
    uint64_t fn_joy_down = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onJoyButtonDown");
    uint64_t fn_joy_up = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onJoyButtonUp");
    uint64_t fn_joy_axis = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setJoyAxis");
    uint64_t fn_no_joysticks = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setNoJoysticks");
    uint64_t fn_back_button = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_onBackButtonPressed");
    uint64_t fn_is_main_menu = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_isOnMainMenuScreen");

    if (!headless) {
        lcs::input::InputSystem::instance().init(
            fn_touch_start,
            fn_touch_move,
            fn_touch_end,
            fn_joy_down,
            fn_joy_up,
            fn_joy_axis,
            fn_no_joysticks,
            fn_back_button
        );
    }

    if (fn_draw_frame) {
        lcs::settings::load("lcs.ini");
        // Sync runtime settings from the game's already-loaded lcs.conf & controls.ini without clobbering them
        {
            auto& st = lcs::settings::get();
            st.display_window = lcs::core::g_config.fullscreen ? 0 : 1;
            st.display_vsync = lcs::core::g_config.vsync ? 1 : 0;
            st.display_fps = lcs::core::g_config.max_fps;
            st.brightness = lcs::core::g_config.brightness;
            st.contrast = lcs::core::g_config.contrast;
            st.saturation = lcs::core::g_config.saturation;
            st.mouse_free_aim = lcs::input::PCControls::instance().options().free_aim ? 1 : 0;
            st.mouse_invert_y = lcs::input::MouseAim::instance().config().invert_y ? 1 : 0;
            st.mouse_sensitivity = static_cast<int>(std::lround(lcs::input::MouseAim::instance().config().sensitivity * 100.0f));
            if (const char* diag_sub = std::getenv("LCS_DIAG_SUB_LANG"); diag_sub && *diag_sub) {
                st.game_language = std::clamp(std::atoi(diag_sub), 0, 7);
            }
        }
        lcs::settings::apply_live();
        if (sym_prefs_invert_look) {
            *reinterpret_cast<uint8_t*>(sym_prefs_invert_look) =
                lcs::input::MouseAim::instance().config().invert_y ? 1 : 0;
        }
        lcs::game::LCSTrainer::instance().attach(image_game, cpu);
#if defined(_WIN32)
        lcs::cpu::HLEManager::set_render_thread_id(GetCurrentThreadId());
#endif

        std::cout << "  [11] Entering main frame loop"
                  << (max_frames > 0 ? " (max_frames=" + std::to_string(max_frames) + ")" : "")
                  << "...\n" << std::flush;

        auto& bridge = lcs::graphics::RenderBridge::instance();
        uint64_t rendered_frames = 0;
        auto last_time = std::chrono::steady_clock::now();
        auto next_frame_deadline = last_time;

        constexpr float kAudioSfxStep = 1.0f / 30.0f;
        float audio_sfx_accum = kAudioSfxStep;
        while (headless || lcs::graphics::is_window_open()) {
            // Wait for DXGI swapchain slot BEFORE polling window/mouse/gamepad events so sampled
            // inputs drive the immediate next frame instead of aging behind queued frames.
            if (!headless) {
                bridge.wait_for_present_slot();
                if (!lcs::graphics::poll_window_events()) {
                    break;
                }
            }
            const auto frame_start = std::chrono::steady_clock::now();
            // Advance any active full-motion video playback and pass newly decoded frames to renderer.
            lcs::os::JNIBridge::instance().update_video(smoke_test);
            // Drain any asynchronous Java -> Native callbacks queued during the previous frame.
            auto pending = lcs::os::JNIBridge::instance().consume_pending_callbacks();
            if (pending.playlist_finish_init && fn_playlist_finish) {
                std::cout << "  [JNI Dispatch] CommonAPI.HandlePlaylistFinishInit(false)\n" << std::flush;
                cpu.call(fn_playlist_finish, {env_addr, dummy_clazz, 0});
            }
            if (pending.game_loaded && fn_game_loaded) {
                std::cout << "  [JNI Dispatch] RockstarJNIlib.GameLoaded()\n" << std::flush;
                cpu.call(fn_game_loaded, {env_addr, dummy_clazz});
            }
            if (pending.start_game_before_load && fn_start_before_load) {
                std::cout << "  [JNI Dispatch] RockstarJNIlib.StartGameBeforeLoad()\n" << std::flush;
                cpu.call(fn_start_before_load, {env_addr, dummy_clazz});
                if (uint64_t femm = image_game.find_symbol("FrontEndMenuManager")) {
                    *reinterpret_cast<uint8_t*>(femm + 0x2b) = 0;
                }
            }
            if (pending.start_game && fn_start_game) {
                std::cout << "  [JNI Dispatch] RockstarJNIlib.StartGame()\n" << std::flush;
                cpu.call(fn_start_game, {env_addr, dummy_clazz});
                if (uint64_t femm = image_game.find_symbol("FrontEndMenuManager")) {
                    *reinterpret_cast<uint8_t*>(femm + 0x2b) = 0;
                }
            }
            if (pending.video_finished && fn_video_finished) {
                std::cout << "  [JNI Dispatch] andVideo.VideoFinishedCB()\n" << std::flush;
                cpu.call(fn_video_finished, {env_addr, dummy_clazz});
            }
            if (pending.quit_requested) {
                std::cout << "  [JNI Dispatch] QuitApp requested by game.\n" << std::flush;
                break;
            }

            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last_time).count();
            last_time = now;
            if (rendered_frames == 0 || dt <= 0.0f) {
                dt = 1.0f / 60.0f;
            } else if (dt > 0.125f) {
                dt = 0.125f;
            }
            if (audio_sfx_allow) {
                audio_sfx_accum += dt;
                if (audio_sfx_accum >= kAudioSfxStep) {
                    *audio_sfx_allow = 1;
                    audio_sfx_accum -= kAudioSfxStep;
                    if (audio_sfx_accum > kAudioSfxStep) {
                        audio_sfx_accum = kAudioSfxStep;
                    }
                }
            }

            if (!headless) {
                // Safely handle window resize between frames when GPU and command lists are idle.
                // Do not re-call viewOnSurfaceChanged because libGame.so tears down EGL and calls InitGame().
                uint32_t resize_w = 0, resize_h = 0;
                if (lcs::graphics::get_pending_resize(resize_w, resize_h)) {
                    apply_runtime_resolution(resize_w, resize_h);
                }
                // If Touchscreen::Initialize() completed AFTER an opening/boot resize, sync its HUD coordinates immediately
                sync_touchscreen_hud_layout(current_guest_w, current_guest_h, false);
                if (sym_width) *reinterpret_cast<int32_t*>(sym_width) = static_cast<int32_t>(current_guest_w);
                if (sym_height) *reinterpret_cast<int32_t*>(sym_height) = static_cast<int32_t>(current_guest_h);
                if (sym_aspect && current_guest_h > 0) {
                    *reinterpret_cast<float*>(sym_aspect) = static_cast<float>(current_guest_w) / static_cast<float>(current_guest_h);
                }

                static bool s_in_vehicle_ctx = false;
                static bool s_in_cutscene_ctx = false;
                static bool s_prev_blocking_ctx = true;
                static bool s_prev_cutscene_ctx = false;
                bool blocking_ctrl_ctx = false;
                bool in_veh_ctrl_ctx = false;
                {
                    static uint64_t s_femm_addr = image_game.find_symbol("FrontEndMenuManager");
                    static uint64_t s_find_veh = image_game.find_symbol("_Z17FindPlayerVehiclev");
                    static uint64_t s_cut_running = image_game.find_symbol("_ZN12CCutsceneMgr10ms_runningE");
                    static uint64_t s_game_state = image_game.find_symbol("gGameState");

                    bool in_main_menu = fn_is_main_menu ? (cpu.call(fn_is_main_menu, {env_addr, dummy_clazz}) != 0) : false;
                    bool menu_active = s_femm_addr ? (*reinterpret_cast<const uint8_t*>(s_femm_addr + 0x14) != 0) : false;
                    bool video_active = lcs::os::JNIBridge::instance().is_video_playing();
                    lcs::game::LCSTrainer::instance().set_video_playing(video_active);
                    bool in_cutscene = s_cut_running ? (*reinterpret_cast<const uint8_t*>(s_cut_running) != 0) : false;
                    s_in_cutscene_ctx = in_cutscene;
                    uint32_t gstate = s_game_state ? *reinterpret_cast<const uint32_t*>(s_game_state) : 0;
                    s_in_vehicle_ctx = (gstate == 9 && s_find_veh && !in_cutscene && !menu_active && !in_main_menu)
                        ? (cpu.call(s_find_veh) != 0)
                        : false;

                    const bool blocking_ctx =
                        (gstate != 9) || in_main_menu || menu_active || video_active || lcs::ui_overlay::is_blocking_game_input();
                    blocking_ctrl_ctx = blocking_ctx;
                    in_veh_ctrl_ctx = s_in_vehicle_ctx;
                    lcs::graphics::set_gameplay_context(blocking_ctx, s_in_vehicle_ctx, in_cutscene);
                    // Discard buffered cursor movement whenever transitioning out of loading/menu/cutscene into active gameplay
                    if ((s_prev_blocking_ctx && !blocking_ctx) || (s_prev_cutscene_ctx && !in_cutscene)) {
                        lcs::input::MouseAim::instance().reset();
                    }
                    s_prev_blocking_ctx = blocking_ctx;
                    s_prev_cutscene_ctx = in_cutscene;
                }

                auto& input = lcs::input::InputSystem::instance();
                input.poll_gamepad();
                {
                    static bool s_prev_pad_start = false;
                    const bool pad_start = input.is_joy_button_held(0, 4);
                    if (pad_start && !s_prev_pad_start && lcs::game::LCSTrainer::instance().is_in_gameplay() && !ui::globalMenuOpen()) {
                        ui::openGlobalMenu(ui::GlobalTab::Jogo);
                        lcs::graphics::set_menu_active(true);
                    }
                    s_prev_pad_start = pad_start;
                }
                const bool overlay_blocking = lcs::ui_overlay::is_blocking_game_input();
                input.set_block_guest_presses(overlay_blocking && !smoke_test);
                auto& pc_controls = lcs::input::PCControls::instance();
#if defined(LCS_PLATFORM_WINDOWS)
                HWND hwnd_focus = static_cast<HWND>(lcs::graphics::get_native_window_handle());
                const bool diag_verify = (std::getenv("LCS_DIAG_VERIFY") != nullptr);
                const bool win_focused = diag_verify || (hwnd_focus && GetForegroundWindow() == hwnd_focus);
#else
                const bool win_focused = true;
#endif
                pc_controls.poll_frame(win_focused, blocking_ctrl_ctx);
                auto& free_aim = lcs::game::LCSFreeAim::instance();
                free_aim.set_kb_look_flags(pc_controls.kb_look_left(), pc_controls.kb_look_right(), pc_controls.kb_look_behind());
                free_aim.set_classic_ps2_triggers(input.classic_ps2_triggers());
                const float pad_l2 = input.get_pad_axis(0, 4);
                const float pad_r2 = input.get_pad_axis(0, 5);
                const bool want_free_aim =
                    !pc_controls.in_menu() &&
                    !s_in_vehicle_ctx &&
                    pc_controls.options().free_aim &&
                    (pc_controls.is_aiming() || input.is_joy_button_held(0, 7) || pad_l2 > 0.35f);
                const float pad_rx = input.get_pad_axis(0, 2);
                const float pad_ry = input.get_pad_axis(0, 3);

                float look_turn = 0.0f, look_tilt = 0.0f;
                lcs::input::MouseAim::instance().consume_look_radians(
                    pc_controls.in_menu(), free_aim.is_active() || pc_controls.is_aiming(), look_turn, look_tilt);
                if (look_turn != 0.0f || look_tilt != 0.0f) {
                    lcs::game::LCSPrompts::instance().notify_keyboard_mouse();
                }
                const bool mouse_look_mode =
                    (lcs::game::LCSPrompts::instance().active_device() == lcs::game::PromptDevice::KeyboardMouse);
                free_aim.update_frame_input(
                    pc_controls.options().free_aim, want_free_aim,
                    pc_controls.in_menu(), s_in_cutscene_ctx, s_in_vehicle_ctx,
                    mouse_look_mode, look_turn, look_tilt, pad_rx, pad_ry, pad_l2, pad_r2, dt);

                if (sym_prefs_invert_look) {
                    *reinterpret_cast<uint8_t*>(sym_prefs_invert_look) =
                        lcs::input::MouseAim::instance().config().invert_y ? 1 : 0;
                }

                // Suppress forwarding right-stick to guest while 1:1 free-aim drives CCam directly,
                // without overwriting physical m_joy0_axes[2/3] so held DualSense Free Aim keeps rotating.
                input.set_suppress_right_stick(free_aim.is_active());
                if (smoke_test) {
                    static bool s_smoke_entered = false;
                    const bool in_mm = lcs::game::LCSTrainer::instance().is_in_main_menu();
                    if (!in_mm && (rendered_frames == 40 || rendered_frames == 100 ||
                                   rendered_frames == 165 || rendered_frames == 230)) {
                        lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                    } else if (!in_mm && (rendered_frames == 43 || rendered_frames == 103 ||
                                          rendered_frames == 168 || rendered_frames == 233)) {
                        lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                    } else if (in_mm && !s_smoke_entered) {
                        s_smoke_entered = true;
                        HWND hwnd_smoke = static_cast<HWND>(lcs::graphics::get_native_window_handle());
                        PostMessageW(hwnd_smoke, WM_KEYDOWN, VK_RETURN, 1);
                        PostMessageW(hwnd_smoke, WM_KEYUP, VK_RETURN, 0xC0000001);
                    } else if (rendered_frames == 260) {
                        std::cout << "  [SmokeTest] Testing mid-game resize 1024x600...\n" << std::flush;
                        apply_runtime_resolution(1024, 600);
                    } else if (rendered_frames == 290) {
                        lcs::input::InputSystem::instance().queue_joy_axis(0, 1, -1.0f);
                    } else if (rendered_frames == 320) {
                        lcs::input::InputSystem::instance().queue_joy_axis(0, 1, 0.0f);
                    }
                }
                if (const char* diag_env = std::getenv("LCS_DIAG_CUTSCENE"); diag_env && *diag_env == '1') {
                    static bool s_entered_mm = false;
                    static int s_cut_stage = 0; // 0..4 INTRO, 10..14 HOMSWET, 20 done
                    static float s_stage_t = 0.0f;
                    static std::string s_last_cname;
                    const uint64_t lb = image_game.get_load_bias();

                    struct CutObjStat {
                        uint32_t render_calls = 0;
                        uint32_t render_draws = 0;
                    };
                    static std::unordered_map<uint64_t, CutObjStat> s_cut_stats;
                    static std::vector<uint64_t> s_render_stack;
                    static std::vector<uint64_t> s_draw_stack;
                    static bool s_diag_hooks = false;
                    if (!s_diag_hooks) {
                        s_diag_hooks = true;
                        if (const char* nps2 = std::getenv("LCS_DIAG_NO_PS2"); nps2 && *nps2 == '1') {
                            lcs::settings::get().ps2_pipeline = 0;
                            std::cout << "  [DiagCutscene] LCS_DIAG_NO_PS2=1 -> forced ps2_pipeline=0\n";
                        }
                        if (const char* dfps = std::getenv("LCS_DIAG_FPS"); dfps && std::atoi(dfps) > 0) {
                            lcs::settings::get().display_fps = std::atoi(dfps);
                            lcs::core::g_config.max_fps = lcs::settings::get().display_fps;
                            std::cout << "  [DiagCutscene] LCS_DIAG_FPS -> forced max_fps=" << lcs::core::g_config.max_fps << "\n";
                        }
                        if (const char* rvp = std::getenv("LCS_DIAG_REVERT_PC"); rvp && *rvp == '1') {
                            auto* insn = reinterpret_cast<uint32_t*>(lb + 0x40b2e4ULL + 0x178ULL);
                            DWORD oldp = 0;
                            VirtualProtect(insn, 4, PAGE_EXECUTE_READWRITE, &oldp);
                            *insn = 0x1e2c1001u;
                            VirtualProtect(insn, 4, oldp, &oldp);
                            cpu.invalidate_cache(reinterpret_cast<uint64_t>(insn), 4);
                            std::cout << "  [DiagCutscene] LCS_DIAG_REVERT_PC=1 -> reverted CCutsceneObject::ProcessControl to 0.5f\n";
                        }
                        if (void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                            auto* got_slot = reinterpret_cast<uint64_t*>(lb + 0x801998ULL);
                            const uint64_t orig_fn = *got_slot ? *got_slot : (lb + 0x35f344ULL);
                            const uint64_t pre_tramp = lcs::cpu::HLEManager::instance().register_function(
                                "__diag_ronr_pre", [&](const uint64_t* a) -> uint64_t {
                                    s_render_stack.push_back(a[0]);
                                    s_draw_stack.push_back(bridge.get_draw_call_count());
                                    return 0;
                                });
                            const uint64_t post_tramp = lcs::cpu::HLEManager::instance().register_function(
                                "__diag_ronr_post", [&](const uint64_t*) -> uint64_t {
                                    if (!s_render_stack.empty()) {
                                        const uint64_t ent = s_render_stack.back();
                                        const uint64_t d0 = s_draw_stack.back();
                                        s_render_stack.pop_back();
                                        s_draw_stack.pop_back();
                                        const uint64_t d1 = bridge.get_draw_call_count();
                                        auto& st = s_cut_stats[ent];
                                        st.render_calls++;
                                        st.render_draws += static_cast<uint32_t>(d1 - d0);
                                    }
                                    return 0;
                                });
                            auto* code = reinterpret_cast<uint32_t*>(page);
                            // Stack: [sp,#0]=x19, [sp,#8]=x30; frame=16B; literals at code[12..14] (+48)
                            uint32_t insns[] = {
                                0xa9bf7bf3u, // stp x19, x30, [sp, #-16]!
                                0xaa0003f3u, // mov x19, x0
                                0x58000150u, // ldr x16, +40 (code[12] = pre_tramp)
                                0xd63f0200u, // blr x16
                                0xaa1303e0u, // mov x0, x19
                                0x58000130u, // ldr x16, +36 (code[14] = orig_fn)
                                0xd63f0200u, // blr x16
                                0x58000130u, // ldr x16, +36 (code[16] = post_tramp)
                                0xd63f0200u, // blr x16
                                0xa8c17bf3u, // ldp x19, x30, [sp], #16
                                0xd65f03c0u, // ret
                                0xd503201fu, // nop (offset 44 -> code[12] is 48, 8-byte aligned!)
                            };
                            std::memcpy(code, insns, sizeof(insns));
                            // insns idx 2 (byte +8)  -> byte 48: delta=40  (imm19 = 10 -> 0x140)
                            // insns idx 5 (byte +20) -> byte 56: delta=36  (imm19 = 9  -> 0x120)
                            // insns idx 7 (byte +28) -> byte 64: delta=36  (imm19 = 9  -> 0x120)
                            code[2] = 0x58000150u;
                            code[5] = 0x58000130u;
                            code[7] = 0x58000130u;
                            auto* lits = reinterpret_cast<uint64_t*>(code + 12);
                            lits[0] = pre_tramp;
                            lits[1] = orig_fn;
                            lits[2] = post_tramp;
                            DWORD oldp = 0;
                            VirtualProtect(got_slot, 8, PAGE_READWRITE, &oldp);
                            *got_slot = reinterpret_cast<uint64_t>(page);
                            VirtualProtect(got_slot, 8, oldp, &oldp);
                            cpu.invalidate_cache(reinterpret_cast<uint64_t>(page), 128);
                            std::cout << "  [DiagCutscene] Hooked GOT RenderOneNonRoad @ 0x" << std::hex << lb + 0x801998ULL << std::dec << "\n";
                        }
                    }

                    const bool in_mm = lcs::game::LCSTrainer::instance().is_in_main_menu();
                    const char* cname = reinterpret_cast<const char*>(lb + 0x848a24ULL);
                    const int32_t cobj_n = *reinterpret_cast<const int32_t*>(lb + 0x848a68ULL);
                    const float ctimer = *reinterpret_cast<const float*>(lb + 0x848a54ULL);

                    auto dump_cut_objs = [&](const char* label) {
                        std::filesystem::create_directories("scratch");
                        const auto* objs = reinterpret_cast<const uint64_t*>(lb + 0x848a70ULL);
                        const int32_t curr_area = *reinterpret_cast<const int32_t*>(lb + 0x855024ULL);
                        const float* cam_p = reinterpret_cast<const float*>(lb + 0x85ad5cULL);
                        const float* cam_f = reinterpret_cast<const float*>(lb + 0x850af0ULL + 0x19cULL);
                        const float* cut_off = reinterpret_cast<const float*>(lb + 0x848a58ULL);
                        const int32_t vis_n = *reinterpret_cast<const int32_t*>(lb + 0x85ad6cULL);
                        const auto* vis_arr = reinterpret_cast<const uint64_t*>(lb + 0x85ad80ULL);
                        std::cout << "  [DiagCutscene] " << label << " name=\"" << (cname ? cname : "")
                                  << "\" timer=" << ctimer << " numObjs=" << cobj_n
                                  << " area=" << curr_area
                                  << " off=(" << cut_off[0] << "," << cut_off[1] << "," << cut_off[2] << ")"
                                  << " cam=(" << cam_p[0] << "," << cam_p[1] << "," << cam_p[2] << ")"
                                  << " front=(" << cam_f[0] << "," << cam_f[1] << "," << cam_f[2] << ")"
                                  << " visN=" << vis_n
                                  << " ps2_pipe=" << lcs::settings::get().ps2_pipeline
                                  << " draws=" << bridge.get_draw_call_count() << "\n";
                        for (int i = 0; i < std::min(cobj_n, 24); ++i) {
                            const uint64_t obj = objs[i];
                            if (!obj) continue;
                            const uint16_t mid = *reinterpret_cast<const uint16_t*>(obj + 0x7cULL);
                            const uint64_t flags = *reinterpret_cast<const uint64_t*>(obj + 0x58ULL);
                            const uint64_t rw70 = *reinterpret_cast<const uint64_t*>(obj + 0x70ULL);
                            const uint32_t rwtype = rw70 ? *reinterpret_cast<const uint8_t*>(rw70) : 0;
                            const float px = *reinterpret_cast<const float*>(obj + 0x40ULL);
                            const float py = *reinterpret_cast<const float*>(obj + 0x44ULL);
                            const float pz = *reinterpret_cast<const float*>(obj + 0x48ULL);
                            const float* vel = reinterpret_cast<const float*>(obj + 0x90ULL);
                            const float* startp = reinterpret_cast<const float*>(obj + 0x250ULL);
                            const float* accump = reinterpret_cast<const float*>(obj + 0x260ULL);
                            const int32_t cid = *reinterpret_cast<const int32_t*>(obj + 0x240ULL);
                            const char* mname = reinterpret_cast<const char*>(lb + 0x848c04ULL + i * 32ULL);
                            bool in_vis = false;
                            for (int v = 0; v < std::min(vis_n, 2000); ++v) {
                                if (vis_arr[v] == obj) { in_vis = true; break; }
                            }
                            const auto st = s_cut_stats[obj];
                            std::cout << "    [" << i << "] ptr=0x" << std::hex << obj
                                      << " rw70=0x" << rw70 << "(t=" << std::dec << rwtype << ")"
                                      << " flags=0x" << std::hex << flags << std::dec
                                      << " mid=" << mid << " cid=" << cid
                                      << " vis=" << (in_vis ? 1 : 0)
                                      << " rCalls=" << st.render_calls << " rDraws=" << st.render_draws
                                      << " name=\"" << (mname ? mname : "") << "\""
                                      << " pos=(" << px << "," << py << "," << pz << ")"
                                      << " vel=(" << vel[0] << "," << vel[1] << "," << vel[2] << ")"
                                      << " start=(" << startp[0] << "," << startp[1] << "," << startp[2] << ")"
                                      << " acc=(" << accump[0] << "," << accump[1] << "," << accump[2] << ")\n";
                        }
                        s_cut_stats.clear();
                        std::cout << std::flush;
                    };

                    if (lcs::os::JNIBridge::instance().is_video_playing()) {
                        lcs::os::JNIBridge::instance().update_video(true);
                    } else if (in_mm && !s_entered_mm) {
                        static int s_mm_wait = 0;
                        if (++s_mm_wait == 30) {
                            lcs::graphics::capture_window_bmp("scratch/diag_mm.bmp");
                        }
                        if (s_mm_wait >= 90) {
                            s_entered_mm = true;
                            lcs::game::LCSTrainer::instance().request_new_game();
                            std::cout << "  [DiagCutscene] Requested New Game on Main Menu after 90 frames.\n" << std::flush;
                        }
                    } else if (s_in_cutscene_ctx) {
                        static int s_cut_frames = 0;
                        s_stage_t += dt;
                        ++s_cut_frames;
                        const std::string cur_n = cname ? cname : "";
                        if (cur_n != s_last_cname) {
                            s_last_cname = cur_n;
                            s_stage_t = 0.0f;
                            s_cut_frames = 0;
                            s_cut_stage = (cur_n == "HOMSWET") ? 10 : 0;
                            dump_cut_objs("NEW_SCENE");
                        }
                        if (std::getenv("LCS_DIAG_EARLY")) {
                            if (s_cut_frames == 1 || s_cut_frames == 2 || s_cut_frames == 5 || s_cut_frames == 15) {
                                dump_cut_objs((cur_n + "_F" + std::to_string(s_cut_frames)).c_str());
                            }
                            if (cur_n == "INTRO" && s_cut_stage == 0 && s_stage_t >= 2.0f) {
                                s_cut_stage = 1;
                                dump_cut_objs("INTRO_2S");
                                lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                            } else if (cur_n == "INTRO" && s_cut_stage == 1 && s_stage_t >= 2.3f) {
                                lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                                s_cut_stage = 2;
                            } else if (s_cut_stage == 10 && s_stage_t >= 2.5f) {
                                s_cut_stage = 11;
                                dump_cut_objs("HOMSWET_2S");
                                lcs::graphics::capture_window_bmp("scratch/diag_homswet_2s.bmp");
                                break;
                            }
                        } else if (s_cut_stage == 0 && s_stage_t >= 24.0f) {
                            s_cut_stage = 1;
                            dump_cut_objs("INTRO_24S");
                            lcs::graphics::capture_window_bmp("scratch/diag_intro_24s.bmp");
                        } else if (s_cut_stage == 1 && s_stage_t >= 29.0f) {
                            s_cut_stage = 2;
                            dump_cut_objs("INTRO_29S");
                            lcs::graphics::capture_window_bmp("scratch/diag_intro_29s.bmp");
                        } else if (s_cut_stage == 2 && s_stage_t >= 34.0f) {
                            s_cut_stage = 3;
                            dump_cut_objs("INTRO_34S");
                            lcs::graphics::capture_window_bmp("scratch/diag_intro_34s.bmp");
                            lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                        } else if (s_cut_stage == 3 && s_stage_t >= 34.25f) {
                            lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                            s_cut_stage = 4;
                        } else if (s_cut_stage == 10 && s_stage_t >= 2.0f) {
                            s_cut_stage = 11;
                            dump_cut_objs("HOMSWET_2S");
                            lcs::graphics::capture_window_bmp("scratch/diag_homswet_2s.bmp");
                        } else if (s_cut_stage == 11 && s_stage_t >= 5.0f) {
                            s_cut_stage = 12;
                            dump_cut_objs("HOMSWET_5S");
                            lcs::graphics::capture_window_bmp("scratch/diag_homswet_5s.bmp");
                        } else if (s_cut_stage == 12 && s_stage_t >= 8.0f) {
                            s_cut_stage = 13;
                            dump_cut_objs("HOMSWET_8S");
                            lcs::graphics::capture_window_bmp("scratch/diag_homswet_8s.bmp");
                            lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                        } else if (s_cut_stage == 13 && s_stage_t >= 8.25f) {
                            lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                            s_cut_stage = 14;
                            s_stage_t = 0.0f;
                        }
                    } else if (s_cut_stage >= 14 && s_cut_stage < 20 && lcs::game::LCSTrainer::instance().is_in_gameplay()) {
                        s_stage_t += dt;
                        if (s_stage_t >= 4.0f) {
                            s_cut_stage = 20;
                            std::cout << "  [DiagCutscene] Reached open gameplay! draws=" << bridge.get_draw_call_count() << "\n" << std::flush;
                            lcs::graphics::capture_window_bmp("scratch/diag_gameplay.bmp");
                            break;
                        }
                    }
                }
                if (const char* dv_env = std::getenv("LCS_DIAG_VERIFY"); dv_env && *dv_env == '1') {
                    static uint64_t s_prev_tot_draws = 0;
                    const uint64_t cur_tot_draws = bridge.get_draw_call_count();
                    const uint64_t s_last_frame_draws = cur_tot_draws - s_prev_tot_draws;
                    s_prev_tot_draws = cur_tot_draws;
                    static int s_vstage = 0;
                    static int s_vframes = 0;
                    static int s_saved_lang = -1;
                    static HWND s_hwnd = nullptr;
                    static float s_cam_p0 = -1.0f, s_cam_p1 = -1.0f, s_cam_p2 = -1.0f, s_cam_p3 = -1.0f;
                    static float s_car_c0 = -1.0f, s_car_c1 = -1.0f;
                    const uint64_t lb = image_game.get_load_bias();
                    auto& tr = lcs::game::LCSTrainer::instance();
                    if (!s_hwnd) {
                        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                            DWORD pid = 0;
                            GetWindowThreadProcessId(h, &pid);
                            if (pid == GetCurrentProcessId() && IsWindowVisible(h)) {
                                char title[128]{};
                                GetWindowTextA(h, title, sizeof(title));
                                if (title[0] != '\0') { *reinterpret_cast<HWND*>(lp) = h; return FALSE; }
                            }
                            return TRUE;
                        }, reinterpret_cast<LPARAM>(&s_hwnd));
                    }
                    auto cap_frame = [&](const char* path) {
#if defined(LCS_ENABLE_D3D12)
                        if (lcs::graphics::d3d12::is_d3d12_initialized()) {
                            lcs::graphics::d3d12::set_d3d12_screenshot_request(path);
                            return;
                        }
#endif
                        lcs::graphics::capture_window_bmp(path);
                    };
                    auto send_vk = [&](WPARAM vk, bool down, bool extended = false) {
                        if (!s_hwnd) return;
                        const UINT sc = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
                        LPARAM lp = 1 | (static_cast<LPARAM>(sc) << 16);
                        if (extended) lp |= (1LL << 24);
                        if (!down) lp |= (1LL << 30) | (1LL << 31);
                        SendMessageA(s_hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
                    };
                    static bool s_vhold[256]{};
                    auto set_hold_vk = [&](int vk, bool down) {
                        if (vk > 0 && vk < 256) {
                            s_vhold[vk] = down;
                            pc_controls.on_key_event(vk, down);
                        }
                    };
                    for (int hvk = 1; hvk < 256; ++hvk) {
                        if (s_vhold[hvk]) pc_controls.on_key_event(hvk, true);
                    }
                    if (s_saved_lang < 0) s_saved_lang = lcs::settings::get().game_language;
                    if (lcs::os::JNIBridge::instance().is_video_playing()) {
                        lcs::os::JNIBridge::instance().update_video(true);
                    } else {
                        ++s_vframes;
                        std::filesystem::create_directories("scratch");
                        if (s_vstage >= 10 && s_last_frame_draws < 50 && (s_vframes % 60 == 0)) {
                            const uint64_t femm_a = image_game.find_symbol("FrontEndMenuManager");
                            const uint64_t gs_a = image_game.find_symbol("gGameState");
                            const uint64_t up_a = image_game.find_symbol("_ZN6CTimer11m_UserPauseE");
                            const uint32_t gsv = gs_a ? *reinterpret_cast<const uint32_t*>(gs_a) : 0;
                            const int f14 = femm_a ? *reinterpret_cast<const uint8_t*>(femm_a + 0x14ULL) : -1;
                            const int f15 = femm_a ? *reinterpret_cast<const uint8_t*>(femm_a + 0x15ULL) : -1;
                            const int pg = femm_a ? *reinterpret_cast<const int32_t*>(femm_a + 0x110ULL) : -1;
                            const int up = up_a ? *reinterpret_cast<const uint8_t*>(up_a) : -1;
                            std::cout << "  [VerifyWait] st=" << s_vstage << " f=" << s_vframes
                                      << " draws=" << s_last_frame_draws << " gs=" << gsv
                                      << " femm14=" << f14 << " femm15=" << f15 << " pg=" << pg
                                      << " upause=" << up << " cut=" << s_in_cutscene_ctx
                                      << " gp=" << tr.is_in_gameplay() << "\n" << std::flush;
                        }
                        if (s_vstage == 0) {
                            const uint64_t femm_a = image_game.find_symbol("FrontEndMenuManager");
                            const uint64_t gs_a = image_game.find_symbol("gGameState");
                            const uint32_t gsv = gs_a ? *reinterpret_cast<const uint32_t*>(gs_a) : 0;
                            const int f14 = femm_a ? *reinterpret_cast<const uint8_t*>(femm_a + 0x14ULL) : -1;
                            const int f2b = femm_a ? *reinterpret_cast<const uint8_t*>(femm_a + 0x2bULL) : -1;
                            const int pg = femm_a ? *reinterpret_cast<const int32_t*>(femm_a + 0x110ULL) : -1;
                            static bool s_boot_cap = false;
                            if (gsv == 7 || s_last_frame_draws > 2) {
                                std::cout << "  [VerifyBoot] f=" << s_vframes << " draws=" << s_last_frame_draws
                                          << " gs=" << gsv << " f14=" << f14 << " f2b=" << f2b << " pg=" << pg
                                          << " in_mm=" << tr.is_in_main_menu() << "\n" << std::flush;
                                if (!s_boot_cap && gsv == 7) {
                                    s_boot_cap = true;
                                    cap_frame("scratch/verify_boot_f1.bmp");
                                }
                            }
                        }
                        if (s_vstage == 0 && tr.is_in_main_menu() && s_vframes >= 25) {
                            if (const char* diag_sub = std::getenv("LCS_DIAG_SUB_LANG"); diag_sub && *diag_sub) {
                                lcs::settings::get().game_language = std::clamp(std::atoi(diag_sub), 0, 7);
                                tr.request_new_game();
                                s_vstage = 10; s_vframes = 0;
                            } else if (std::getenv("LCS_DIAG_CRUISE")) {
                                s_vstage = 100; s_vframes = 0;
                            } else {
                                cap_frame("scratch/verify_mm_pt.bmp");
                                ui::openGlobalMenu(ui::GlobalTab::Configuracoes);
                                lcs::settings::get().game_language = 0;
                                s_vstage = 1; s_vframes = 0;
                            }
                        } else if (s_vstage == 1 && s_vframes == 8) {
                            cap_frame("scratch/verify_cfg_en.bmp");
                            lcs::settings::get().game_language = 5;
                            s_vstage = 2; s_vframes = 0;
                        } else if (s_vstage == 2 && s_vframes == 8) {
                            cap_frame("scratch/verify_cfg_ru.bmp");
                            lcs::settings::get().game_language = 6;
                            s_vstage = 3; s_vframes = 0;
                        } else if (s_vstage == 3 && s_vframes == 8) {
                            cap_frame("scratch/verify_cfg_ja.bmp");
                            lcs::settings::get().game_language = s_saved_lang;
                            s_vstage = 4; s_vframes = 0;
                        } else if (s_vstage == 4 && s_vframes == 8) {
                            cap_frame("scratch/verify_cfg_pt.bmp");
                            lcs::input::InputSystem::instance().queue_joy_button_down(0, 12);
                            s_vstage = 5; s_vframes = 0;
                        } else if (s_vstage == 5 && s_vframes == 4) {
                            lcs::input::InputSystem::instance().queue_joy_button_up(0, 12);
                            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
                            GetCursorInfo(&ci);
                            std::cout << "  [VerifyMenuPadCursor] ci.flags=" << ci.flags
                                      << " dev=" << static_cast<int>(lcs::game::LCSPrompts::instance().active_device()) << "\n" << std::flush;
                            send_vk(VK_ESCAPE, true);
                            s_vstage = 6; s_vframes = 0;
                        } else if (s_vstage == 6) {
                            if (s_vframes == 1) { send_vk(VK_ESCAPE, false); cap_frame("scratch/verify_esc_f1.bmp"); }
                            if (s_vframes == 3) {
                                cap_frame("scratch/verify_esc_f3.bmp");
                                std::cout << "  [VerifyEsc] globalMenuOpen=" << ui::globalMenuOpen()
                                          << " in_mm=" << tr.is_in_main_menu() << "\n" << std::flush;
                                if (std::getenv("LCS_DIAG_BOOT_ONLY")) {
                                    std::cout << "  [VerifyDone] Boot + ESC check complete.\n" << std::flush;
                                    break;
                                }
                                if (std::getenv("LCS_DIAG_VERIFY_HD")) {
                                    ui::openGlobalMenu(ui::GlobalTab::Configuracoes);
                                    s_vstage = 100; s_vframes = 0;
                                } else {
                                    tr.request_new_game();
                                    s_vstage = 10; s_vframes = 0;
                                }
                            }
                        } else if (s_vstage == 100) {
                            // Navigate Settings -> Gráficos & Mundo (4 tabs right), wait full fade-in, capture PT & EN
                            if (s_vframes == 1) {
                                lcs::settings::get().game_language = 7;
                            } else if (s_vframes == 2 || s_vframes == 5 || s_vframes == 8 || s_vframes == 11) {
                                send_vk(VK_TAB, true);
                            } else if (s_vframes == 3 || s_vframes == 6 || s_vframes == 9 || s_vframes == 12) {
                                send_vk(VK_TAB, false);
                            } else if (s_vframes == 30) {
                                cap_frame("scratch/verify_cfg_gfx_pt.bmp");
                            } else if (s_vframes == 33) {
                                lcs::settings::get().game_language = 0;
                            } else if (s_vframes == 40) {
                                cap_frame("scratch/verify_cfg_gfx_en.bmp");
                            } else if (s_vframes == 43) {
                                lcs::settings::get().game_language = s_saved_lang;
                                send_vk(VK_ESCAPE, true);
                            } else if (s_vframes == 45) {
                                send_vk(VK_ESCAPE, false);
                                const int rslot = tr.get_recent_save_slot();
                                const int load_slot = (rslot >= 0) ? rslot : 0;
                                std::cout << "  [VerifyHD] Loading read-only save slot " << load_slot << "...\n" << std::flush;
                                tr.request_load_slot(load_slot);
                                s_vstage = 101; s_vframes = 0;
                            }
                        } else if (s_vstage == 101) {
                            const uint64_t gp = image_game.find_symbol("_ZN4CPad6GetPadEi");
                            const uint64_t pad0 = gp ? cpu.call(gp, {0}) : 0;
                            const uint16_t dis = pad0 ? *reinterpret_cast<const uint16_t*>(pad0 + 0xa2ULL) : 0;
                            if (!s_in_cutscene_ctx && tr.is_in_gameplay() && s_last_frame_draws >= 50 && dis == 0) {
                                static int s_ready_cnt = 0;
                                ++s_ready_cnt;
                                if (s_ready_cnt == 40) {
                                    const auto snap0 = tr.read_snapshot();
                                    std::cout << "  [VerifyHD] Save loaded pos=(" << snap0.pos[0] << "," << snap0.pos[1] << "," << snap0.pos[2] << ")\n" << std::flush;
                                    // Position on open waterfront promenade looking South toward Callahan Bridge & Harbor skyline (no save writes)
                                    tr.set_time_of_day(13);
                                    tr.set_weather(0);
                                    tr.teleport_to(785.0f, -645.0f, 15.2f, "Waterfront Skyline");
                                    lcs::settings::get().view_distance = 85;
                                    lcs::settings::get().detail_distance = 85;
                                    lcs::game::LcsWorldHd::instance().set_radius(0);
                                    lcs::game::LcsDistance::instance().sync_settings_now();
                                } else if (s_ready_cnt == 52) {
                                    set_hold_vk('S', true);
                                } else if (s_ready_cnt == 74) {
                                    set_hold_vk('S', false);
                                    set_hold_vk('W', true);
                                } else if (s_ready_cnt >= 120) {
                                    set_hold_vk('W', false);
                                    s_ready_cnt = 0;
                                    s_vstage = 102; s_vframes = 0;
                                }
                            }
                        } else if (s_vstage == 102 && std::getenv("LCS_DIAG_CRUISE")) {
                            s_vstage = 200; s_vframes = 0;
                        } else if (s_vstage == 200) {
                            // A/B Spot 1: Saint Mark's Street (1195.8, -348.8, 24.8) at 19:20, HD=0
                            if (s_vframes == 1) {
                                tr.set_time_of_day(19);
                                tr.set_weather(0);
                                tr.teleport_to(1195.8f, -348.8f, 24.8f, "StMarks");
                                lcs::settings::get().view_distance = 100;
                                lcs::settings::get().detail_distance = 100;
                                lcs::game::LcsWorldHd::instance().set_radius(0);
                                lcs::game::LcsDistance::instance().sync_settings_now();
                            }
                            if (s_vframes == 55) cap_frame("scratch/cruise_stmarks_hd0.bmp");
                            if (s_vframes == 58) {
                                std::cout << "  [CruiseAB] spot=stmarks rad=0 draws=" << s_last_frame_draws << "\n" << std::flush;
                                lcs::game::LcsWorldHd::instance().set_radius(3);
                                s_vstage = 201; s_vframes = 0;
                            }
                        } else if (s_vstage == 201) {
                            // A/B Spot 1: Saint Mark's Street (19:20), HD=3
                            if (s_vframes == 80) cap_frame("scratch/cruise_stmarks_hd3.bmp");
                            if (s_vframes == 83) {
                                std::cout << "  [CruiseAB] spot=stmarks rad=3 draws=" << s_last_frame_draws << "\n" << std::flush;
                                tr.set_time_of_day(21);
                                tr.teleport_to(900.8f, -740.0f, 14.8f, "Chinatown");
                                lcs::game::LcsWorldHd::instance().set_radius(0);
                                s_vstage = 202; s_vframes = 0;
                            }
                        } else if (s_vstage == 202) {
                            // A/B Spot 2: Chinatown Street (900.8, -740.0, 14.8) at 21:42, HD=0
                            if (s_vframes == 55) cap_frame("scratch/cruise_china_hd0.bmp");
                            if (s_vframes == 58) {
                                std::cout << "  [CruiseAB] spot=china rad=0 draws=" << s_last_frame_draws << "\n" << std::flush;
                                lcs::game::LcsWorldHd::instance().set_radius(3);
                                s_vstage = 203; s_vframes = 0;
                            }
                        } else if (s_vstage == 203) {
                            // A/B Spot 2: Chinatown Street (21:42), HD=3
                            if (s_vframes == 80) cap_frame("scratch/cruise_china_hd3.bmp");
                            if (s_vframes == 83) {
                                std::cout << "  [CruiseAB] spot=china rad=3 draws=" << s_last_frame_draws << "\n" << std::flush;
                                tr.set_time_of_day(14);
                                tr.teleport_to(1050.8f, -420.0f, 14.8f, "PortlandBlvdBank");
                                lcs::game::LcsWorldHd::instance().set_radius(0);
                                s_vstage = 210; s_vframes = 0;
                            }
                        } else if (s_vstage == 210) {
                            // A/B Spot 3: Liberty Bank / Portland Blvd (1050.8, -420.0, 14.8) at 14:00, HD=0 vs HD=3
                            if (s_vframes == 55) cap_frame("scratch/cruise_bank_hd0.bmp");
                            if (s_vframes == 58) {
                                std::cout << "  [CruiseAB] spot=bank rad=0 draws=" << s_last_frame_draws << "\n" << std::flush;
                                lcs::game::LcsWorldHd::instance().set_radius(3);
                            }
                            if (s_vframes == 135) cap_frame("scratch/cruise_bank_hd3.bmp");
                            if (s_vframes == 138) {
                                std::cout << "  [CruiseAB] spot=bank rad=3 draws=" << s_last_frame_draws << "\n" << std::flush;
                                s_vstage = 204; s_vframes = 0;
                            }
                        } else if (s_vstage == 204) {
                            // Spawn Kuruma (152) on flat Portland Blvd Southbound, wait to settle, board, align South
                            auto& inp = lcs::input::InputSystem::instance();
                            if (s_vframes == 15) tr.spawn_vehicle(152);
                            if (s_vframes == 55) {
                                const bool ok = tr.teleport_beside_nearest_vehicle(152);
                                const auto ps = tr.read_snapshot();
                                const uint64_t gp = image_game.find_symbol("_ZN4CPad6GetPadEi");
                                const uint64_t pad0 = gp ? cpu.call(gp, {0}) : 0;
                                const uint16_t dis = pad0 ? *reinterpret_cast<const uint16_t*>(pad0 + 0xa2ULL) : 0;
                                std::cout << "  [CruiseWarpKuruma] ok=" << (ok ? 1 : 0)
                                          << " pos=(" << ps.pos[0] << "," << ps.pos[1] << "," << ps.pos[2] << ")"
                                          << " dis=" << dis << "\n" << std::flush;
                            }
                            if (s_vframes == 58) cap_frame("scratch/cruise_spawn.bmp");
                            if (s_vframes == 60 || s_vframes == 115) {
                                if (s_vframes > 60) tr.teleport_beside_nearest_vehicle(152);
                                send_vk('F', true);
                                inp.queue_joy_button_down(0, 3);
                            }
                            if (s_vframes == 66 || s_vframes == 121) {
                                send_vk('F', false);
                                inp.queue_joy_button_up(0, 3);
                            }
                            static bool s_board_queued = false;
                            static bool s_board_done = false;
                            if (s_vframes <= 1) { s_board_queued = false; s_board_done = false; }
                            if ((s_vframes >= 75 && !s_board_queued) && (!s_in_vehicle_ctx || s_vframes == 75)) {
                                s_board_queued = true;
                                send_vk('F', false);
                                inp.queue_joy_button_up(0, 3);
                                const uint64_t fn_ped     = image_game.find_symbol("_Z13FindPlayerPedv");
                                const uint64_t fn_warp    = image_game.find_symbol("_ZN4CPed14WarpPedIntoCarEP8CVehicle");
                                const uint64_t fn_auto_tp = image_game.find_symbol("_ZN11CAutomobile8TeleportE7CVector");
                                const uint64_t vp_addr    = image_game.find_symbol("_ZN6CPools15ms_pVehiclePoolE");
                                tr.enqueue_cpu_action([&, fn_ped, fn_warp, fn_auto_tp, vp_addr]() {
                                    auto* gcpu = lcs::cpu::DynarmicCore::current();
                                    if (!gcpu) return;
                                    const uint64_t ped = fn_ped ? gcpu->call_nested(fn_ped, {}) : 0;
                                    const uint64_t pool_va = vp_addr ? *reinterpret_cast<const uint64_t*>(vp_addr) : 0;
                                    uint64_t best_veh = ped ? *reinterpret_cast<const uint64_t*>(ped + 0x610ULL) : 0;
                                    if (!best_veh && ped && pool_va) {
                                        const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool_va + 0x00ULL);
                                        const uint64_t bmap = *reinterpret_cast<const uint64_t*>(pool_va + 0x08ULL);
                                        const int32_t  size = *reinterpret_cast<const int32_t*>(pool_va + 0x10ULL);
                                        const auto ps = tr.read_snapshot();
                                        float best_d2 = 1e9f;
                                        for (int32_t vi = 0; vi < size && objs && bmap; ++vi) {
                                            if (*reinterpret_cast<const int8_t*>(bmap + vi) < 0) continue;
                                            const uint64_t v = objs + static_cast<uint64_t>(vi) * 0x7b0ULL;
                                            if (*reinterpret_cast<const int16_t*>(v + 0x7cULL) != 152) continue;
                                            const float* p = reinterpret_cast<const float*>(v + 0x40ULL);
                                            const float dx = p[0] - ps.pos[0], dy = p[1] - ps.pos[1], dz = p[2] - ps.pos[2];
                                            const float d2 = dx * dx + dy * dy + dz * dz;
                                            if (d2 < best_d2) { best_d2 = d2; best_veh = v; }
                                        }
                                    }
                                    if (ped && best_veh && fn_warp) {
                                        if (*reinterpret_cast<const uint64_t*>(best_veh + 0x248ULL) != ped) {
                                            *reinterpret_cast<uint32_t*>(ped + 0x270ULL) = 18; // OBJECTIVE_ENTER_CAR_AS_DRIVER
                                            gcpu->call_nested(fn_warp, {ped, best_veh});
                                        }
                                        alignas(16) static float s_tp_vec[4];
                                        s_tp_vec[0] = 1046.5f; s_tp_vec[1] = -430.0f; s_tp_vec[2] = 15.0f; s_tp_vec[3] = 0.0f;
                                        if (fn_auto_tp) {
                                            gcpu->call_nested(fn_auto_tp, {best_veh, reinterpret_cast<uint64_t>(s_tp_vec)});
                                        }
                                        float* mat = reinterpret_cast<float*>(best_veh + 0x10ULL);
                                        mat[0] = -1.0f; mat[1] = 0.0f;  mat[2] = 0.0f;
                                        mat[4] = 0.0f;  mat[5] = -1.0f; mat[6] = 0.0f;
                                        mat[8] = 0.0f;  mat[9] = 0.0f;  mat[10] = 1.0f;
                                        float* vel = reinterpret_cast<float*>(best_veh + 0x90ULL);
                                        vel[0] = 0.0f; vel[1] = 0.0f; vel[2] = 0.0f;
                                        std::cout << "  [CruiseWarpIntoCar] ped=0x" << std::hex << ped
                                                  << " veh=0x" << best_veh
                                                  << " drv=0x" << *reinterpret_cast<const uint64_t*>(best_veh + 0x248ULL)
                                                  << std::dec << "\n" << std::flush;
                                        s_board_done = true;
                                    }
                                });
                            }
                            if (s_board_done && s_in_vehicle_ctx) {
                                send_vk('F', false);
                                inp.queue_joy_button_up(0, 3);
                                std::cout << "  [CruiseBoarded] f=" << s_vframes << " aligned_veh=(1046.5,-430,15.0) facing=South\n" << std::flush;
                                s_vstage = 205; s_vframes = 0;
                            } else if (s_vframes > 260) {
                                send_vk('F', false);
                                std::cout << "  [CruiseAbort] could not board Kuruma\n" << std::flush;
                                break;
                            }
                        } else if (s_vstage == 205) {
                            // Multi-cell route: South on Portland Blvd (1046.5,-430..-498) -> West to Waterfront (795,-498) -> South along Coast (785,-780)
                            static auto s_prev_tp = std::chrono::steady_clock::now();
                            static float s_prev_x = 0.0f, s_prev_y = 0.0f;
                            static bool s_has_prev = false;
                            static int s_last_cx = -999, s_last_cy = -999;
                            static int s_stuck_f = 0, s_rev_f = 0, s_cap_idx = 0, s_leg = 0, s_skip_ms = 0;
                            static double s_peak_ms = 0.0;
                            static float s_min_ms = 9999.0f;
                            if (s_vframes <= 1) {
                                s_has_prev = false; s_stuck_f = 0; s_rev_f = 0; s_cap_idx = 0; s_leg = 0; s_skip_ms = 2;
                                s_peak_ms = 0.0; s_min_ms = 9999.0f;
                            }
                            auto& inp = lcs::input::InputSystem::instance();
                            const auto cur_tp = std::chrono::steady_clock::now();
                            const double ms = std::chrono::duration<double, std::milli>(cur_tp - s_prev_tp).count();
                            s_prev_tp = cur_tp;
                            if (s_skip_ms > 0) {
                                --s_skip_ms;
                            } else if (s_vframes > 3) {
                                if (ms > s_peak_ms) s_peak_ms = ms;
                                if (ms < s_min_ms) s_min_ms = static_cast<float>(ms);
                            }

                            const auto cs = tr.read_snapshot();
                            const int cx = static_cast<int>(std::floor((cs.pos[0] + 3000.0f) / 150.0f));
                            const int cy = static_cast<int>(std::floor((cs.pos[1] + 3000.0f) / 150.0f));
                            const float dx = s_has_prev ? (cs.pos[0] - s_prev_x) : 0.0f;
                            const float dy = s_has_prev ? (cs.pos[1] - s_prev_y) : 0.0f;
                            const float spd = std::sqrt(dx * dx + dy * dy);

                            if (!s_has_prev) {
                                s_has_prev = true;
                                s_prev_x = cs.pos[0]; s_prev_y = cs.pos[1];
                                s_last_cx = cx; s_last_cy = cy;
                            }

                            // Multi-district continuous driving stress route along open avenues
                            const int target_cruise_frames = []() {
                                if (const char* env = std::getenv("LCS_DIAG_CRUISE_FRAMES")) {
                                    const int v = std::atoi(env);
                                    if (v > 60) return v;
                                }
                                return 620;
                            }();
                            static const float kWaypoints[][2] = {
                                {1046.5f, -502.0f},  // Portland Blvd South to Chinatown intersection
                                { 825.0f, -498.0f},  // West to Waterfront Blvd
                                { 825.0f, -805.0f},  // South down Waterfront / Atlantic Quays
                                {1190.0f, -805.0f},  // East through Trenton / Portland Harbor
                                {1195.0f, -495.0f},  // North along Portland Harbor / Portland View
                                {1046.5f, -495.0f},  // West past Bank to Portland Blvd
                                {1046.5f, -210.0f},  // North past Saint Mark's to Harwood
                                { 875.0f, -210.0f},  // West through Hepburn Heights
                                { 870.0f, -495.0f},  // South through Red Light District
                            };
                            constexpr int kNumWaypoints = static_cast<int>(sizeof(kWaypoints) / sizeof(kWaypoints[0]));
                            if (target_cruise_frames > 1000) {
                                // Exercise level transitions & live WorldHD radius toggles at extended stress intervals
                                if (s_vframes == 1400) {
                                    tr.teleport_to(260.0f, -680.0f, 26.2f, "StauntonStress");
                                } else if (s_vframes == 1900) {
                                    lcs::game::LcsWorldHd::instance().set_radius(0);
                                } else if (s_vframes == 2050) {
                                    lcs::game::LcsWorldHd::instance().set_radius(2);
                                } else if (s_vframes == 2200) {
                                    tr.teleport_to(-580.0f, -100.0f, 26.2f, "ShoresideStress");
                                    lcs::game::LcsWorldHd::instance().set_radius(3);
                                } else if (s_vframes == 2900) {
                                    tr.teleport_to(1046.5f, -430.0f, 15.0f, "PortlandReturn");
                                    s_leg = 0;
                                }
                            }
                            const int wp_idx = s_leg % kNumWaypoints;
                            const float gx = kWaypoints[wp_idx][0], gy = kWaypoints[wp_idx][1];
                            const float rdx = gx - cs.pos[0], rdy = gy - cs.pos[1];
                            const float rdist = std::sqrt(rdx * rdx + rdy * rdy);
                            if (rdist < 30.0f || (wp_idx == 0 && cs.pos[1] <= -476.0f) || (wp_idx == 1 && cs.pos[0] <= 835.0f)) {
                                ++s_leg;
                            }
                            const float nx = kWaypoints[s_leg % kNumWaypoints][0], ny = kWaypoints[s_leg % kNumWaypoints][1];
                            const float ndx = nx - cs.pos[0], ndy = ny - cs.pos[1];
                            const float rlen = std::max(1.0f, std::sqrt(ndx * ndx + ndy * ndy));
                            const float tx = ndx / rlen, ty = ndy / rlen;

                            if (s_rev_f > 0) {
                                set_hold_vk('W', false);
                                set_hold_vk('D', false);
                                set_hold_vk('S', true);
                                set_hold_vk('A', true);
                                inp.queue_joy_button_up(0, 0);
                                inp.queue_joy_button_down(0, 2);
                                --s_rev_f;
                                if (s_rev_f == 0) {
                                    set_hold_vk('S', false);
                                    set_hold_vk('A', false);
                                    inp.queue_joy_button_up(0, 2);
                                }
                            } else {
                                set_hold_vk('S', false);
                                set_hold_vk('W', true);
                                inp.queue_joy_button_up(0, 2);
                                inp.queue_joy_button_down(0, 0);
                                float steer = 0.0f;
                                if (spd > 0.04f) {
                                    const float vx = dx / spd, vy = dy / spd;
                                    const float cross = vx * ty - vy * tx;
                                    const float dot = vx * tx + vy * ty;
                                    if (dot < 0.1f) {
                                        steer = (cross >= 0.0f) ? -1.0f : 1.0f;
                                    } else {
                                        steer = std::clamp(-cross * 2.2f, -0.95f, 0.95f);
                                    }
                                }
                                set_hold_vk('A', steer < -0.18f);
                                set_hold_vk('D', steer > 0.18f);
                                inp.set_kb_axes(steer, 0.0f, 0.0f, 1.0f);
                                if (s_vframes > 35 && spd < 0.02f) ++s_stuck_f;
                                else s_stuck_f = 0;
                                if (s_stuck_f >= 28) {
                                    s_rev_f = 24;
                                    s_stuck_f = 0;
                                }
                            }

                            const bool cell_changed = (cx != s_last_cx || cy != s_last_cy);
                            if (std::getenv("LCS_DIAG_AUDIT") && (s_vframes % 40 == 0)) {
                                const uint64_t vp_got = *reinterpret_cast<const uint64_t*>(image_game.get_load_bias() + 0x7f9720ULL);
                                const uint64_t pool_va = vp_got ? *reinterpret_cast<const uint64_t*>(vp_got) : 0ULL;
                                const int32_t cnodes = *reinterpret_cast<const int32_t*>(image_game.get_load_bias() + 0x89031cULL);
                                int n_cars = 0, b_lt60 = 0, b_60_90 = 0, b_90_140 = 0, b_gt140 = 0;
                                float dmin = 9999.0f, dmax = 0.0f;
                                struct StuckCarInfo {
                                    int32_t idx = -1;
                                    int16_t mid = -1;
                                    float vd = 0.0f;
                                    float x = 0.0f, y = 0.0f;
                                    uint64_t eflags = 0, drv = 0;
                                    uint8_t b291 = 0, b293 = 0, b2cc = 0, b2cd = 0;
                                };
                                std::vector<StuckCarInfo> stuck_cars;
                                if (pool_va) {
                                    const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool_va + 0x00ULL);
                                    const uint64_t bmap = *reinterpret_cast<const uint64_t*>(pool_va + 0x08ULL);
                                    const int32_t psz   = *reinterpret_cast<const int32_t*>(pool_va + 0x10ULL);
                                    for (int32_t vi = 0; vi < psz && objs && bmap; ++vi) {
                                        if (*reinterpret_cast<const int8_t*>(bmap + vi) < 0) continue;
                                        const uint64_t v = objs + static_cast<uint64_t>(vi) * 0x7b0ULL;
                                        const float* vp = reinterpret_cast<const float*>(v + 0x40ULL);
                                        const float vdx = vp[0] - cs.pos[0], vdy = vp[1] - cs.pos[1];
                                        const float vd = std::sqrt(vdx * vdx + vdy * vdy);
                                        if (vd < 1.0f) continue; // skip player car
                                        ++n_cars;
                                        if (vd < dmin) dmin = vd;
                                        if (vd > dmax) dmax = vd;
                                        if (vd < 60.0f) ++b_lt60;
                                        else if (vd < 90.0f) ++b_60_90;
                                        else if (vd < 140.0f) ++b_90_140;
                                        else ++b_gt140;
                                        if (vd > 500.0f && stuck_cars.size() < 12) {
                                            StuckCarInfo sc{};
                                            sc.idx = vi;
                                            sc.mid = *reinterpret_cast<const int16_t*>(v + 0x7cULL);
                                            sc.vd = vd;
                                            sc.x = vp[0]; sc.y = vp[1];
                                            sc.eflags = *reinterpret_cast<const uint64_t*>(v + 0x58ULL);
                                            sc.drv = *reinterpret_cast<const uint64_t*>(v + 0x248ULL);
                                            sc.b291 = *reinterpret_cast<const uint8_t*>(v + 0x291ULL);
                                            sc.b293 = *reinterpret_cast<const uint8_t*>(v + 0x293ULL);
                                            sc.b2cc = *reinterpret_cast<const uint8_t*>(v + 0x2ccULL);
                                            sc.b2cd = *reinterpret_cast<const uint8_t*>(v + 0x2cdULL);
                                            stuck_cars.push_back(sc);
                                        }
                                    }
                                }
                                std::cout << "  [TMP_CAR_AUDIT] f=" << s_vframes << " nodes=" << cnodes
                                          << " cars=" << n_cars << " minD=" << dmin << " maxD=" << dmax
                                          << " (<60:" << b_lt60 << " 60-90:" << b_60_90
                                          << " 90-140:" << b_90_140 << " >140:" << b_gt140 << ")\n";
                                if (!stuck_cars.empty()) {
                                    std::cout << "  [TMP_CAR_STUCK] gt500:";
                                    for (const auto& sc : stuck_cars) {
                                        std::cout << " [#" << sc.idx << " mid=" << sc.mid << " d=" << sc.vd
                                                  << " cr=" << static_cast<int>(sc.b2cc) << " cd=" << static_cast<int>(sc.b2cd)
                                                  << " b291=" << static_cast<int>(sc.b291) << " b293=" << static_cast<int>(sc.b293)
                                                  << " drv=" << (sc.drv ? 1 : 0) << " efl=0x" << std::hex << sc.eflags << std::dec << "]";
                                    }
                                    std::cout << "\n";
                                }
                            }
                            if (cell_changed || (s_vframes % 40 == 0)) {
                                const auto whd = lcs::game::LcsWorldHd::instance().get_stats();
                                std::cout << "  [Cruise] f=" << s_vframes
                                          << " leg=" << s_leg
                                          << " pos=(" << cs.pos[0] << "," << cs.pos[1] << "," << cs.pos[2] << ")"
                                          << " hex=(" << cx << "," << cy << ")"
                                          << (cell_changed ? " CELL_CHANGE" : "")
                                          << " hd=(" << whd.cached_cells << " cached," << whd.ready_cells << " ready," << whd.combined_instances << " inst)"
                                          << " spd=" << (spd * 60.0f)
                                          << " ms=" << ms << " peak=" << s_peak_ms
                                          << " draws=" << s_last_frame_draws << "\n" << std::flush;
                                if (cell_changed && s_cap_idx < 6) {
                                    char pbuf[64];
                                    std::snprintf(pbuf, sizeof(pbuf), "scratch/cruise_cell_%d.bmp", s_cap_idx++);
                                    cap_frame(pbuf);
                                    s_skip_ms = 2;
                                }
                                s_last_cx = cx; s_last_cy = cy;
                            }
                            if (s_vframes == 90 || s_vframes == 220 || s_vframes == 380 || s_vframes == 500) {
                                char pbuf[64];
                                std::snprintf(pbuf, sizeof(pbuf), "scratch/cruise_f%d.bmp", s_vframes);
                                cap_frame(pbuf);
                                s_skip_ms = 2;
                            }
                            if (ms > 35.0 && s_skip_ms == 0 && s_vframes > 5) {
                                const auto whd = lcs::game::LcsWorldHd::instance().get_stats();
                                std::cout << "  [CruiseSLOW] f=" << s_vframes << " ms=" << ms
                                          << " pos=(" << cs.pos[0] << "," << cs.pos[1] << "," << cs.pos[2] << ")"
                                          << " hd=(" << whd.cached_cells << "," << whd.ready_cells << "," << whd.combined_instances << ")"
                                          << " draws=" << s_last_frame_draws << "\n" << std::flush;
                            }
                            s_prev_x = cs.pos[0]; s_prev_y = cs.pos[1];
                            if (s_vframes >= target_cruise_frames) {
                                set_hold_vk('W', false);
                                set_hold_vk('S', false);
                                inp.queue_joy_button_up(0, 0);
                                inp.queue_joy_button_up(0, 2);
                                inp.set_kb_axes(0.0f, 0.0f, 0.0f, 0.0f);
                                std::cout << "  [VerifyDone] Cruise complete. peak_ms=" << s_peak_ms
                                          << " min_ms=" << s_min_ms << " cell_caps=" << s_cap_idx << "\n" << std::flush;
                                break;
                            }
                        } else if (s_vstage == 102 || s_vstage == 103 || s_vstage == 104) {
                            // A/B/C skyline benchmark: hd_radius 0 (Original) vs 2 (19 cells) vs 3 (37 cells)
                            constexpr int kWarmup = 60;
                            constexpr int kMeasure = 120;
                            static std::chrono::steady_clock::time_point s_t0;
                            static uint64_t s_draw_sum = 0;
                            static uint64_t s_draw_max = 0;
                            static int s_measured = 0;
                            const int rad = (s_vstage == 102) ? 0 : (s_vstage == 103 ? 2 : 3);
                            if (s_vframes == kWarmup) {
                                s_t0 = std::chrono::steady_clock::now();
                                s_draw_sum = 0;
                                s_draw_max = 0;
                                s_measured = 0;
                            } else if (s_vframes > kWarmup && s_vframes <= kWarmup + kMeasure) {
                                s_draw_sum += s_last_frame_draws;
                                if (s_last_frame_draws > s_draw_max) s_draw_max = s_last_frame_draws;
                                ++s_measured;
                                if (s_vframes == kWarmup + kMeasure) {
                                    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_t0).count();
                                    const double fps = (sec > 0.0) ? (static_cast<double>(s_measured) / sec) : 0.0;
                                    const double ms = (s_measured > 0) ? (sec * 1000.0 / static_cast<double>(s_measured)) : 0.0;
                                    const double avg_draws = (s_measured > 0) ? (static_cast<double>(s_draw_sum) / static_cast<double>(s_measured)) : 0.0;
                                    auto& whd = lcs::game::LcsWorldHd::instance();
                                    auto& dst = lcs::game::LcsDistance::instance();
                                    auto& mem = lcs::game::LcsMemory::instance();
                                    std::cout << "  [BenchHD] radius=" << rad
                                              << " fps=" << fps << " ms=" << ms
                                              << " avgDraws=" << avg_draws << " maxDraws=" << s_draw_max
                                              << " cells=" << whd.loaded_cell_count()
                                              << " hdInsts=" << whd.rendered_instance_count()
                                              << " lodScale=" << dst.last_lod_scale()
                                              << " farClip=" << dst.last_far_clip()
                                              << " poolsExp=" << dst.pools_expanded()
                                              << " poolPress=" << mem.max_pool_pressure()
                                              << "\n" << std::flush;
                                    if (rad == 0) {
                                        cap_frame("scratch/verify_skyline_hd0.bmp");
                                    } else if (rad == 2) {
                                        cap_frame("scratch/verify_skyline_hd2.bmp");
                                    } else {
                                        cap_frame("scratch/verify_skyline_hd3.bmp");
                                    }
                                }
                            } else if (s_vframes == kWarmup + kMeasure + 2) {
                                auto& whd = lcs::game::LcsWorldHd::instance();
                                if (rad == 0) {
                                    whd.set_radius(2);
                                    s_vstage = 103; s_vframes = 0;
                                } else if (rad == 2) {
                                    whd.set_radius(3);
                                    s_vstage = 104; s_vframes = 0;
                                } else {
                                    // Run quick V-camera + PCJ-600 bike sanity check right in open roadway
                                    s_vstage = 13; s_vframes = 0;
                                }
                            }
                        } else if (s_vstage == 10 && s_in_cutscene_ctx) {
                            if ((s_last_frame_draws >= 50 && lcs::subtitles::has_active_items()) || s_vframes >= 1800) {
                                std::cout << "  [VerifyIntro] draws=" << s_last_frame_draws << " hasSub=" << lcs::subtitles::has_active_items() << " f=" << s_vframes << "\n" << std::flush;
                                cap_frame("scratch/verify_cut_sub.bmp");
                                lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                                s_vstage = 11; s_vframes = 0;
                            }
                        } else if (s_vstage == 11) {
                            static int s_hom_f = 0;
                            static int s_hom_skip = 0;
                            static int s_dis_zero_f = 0;
                            static bool s_hom_shot = false;
                            static uint64_t s_pad0 = 0;
                            if (s_vframes == 12) lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                            if (s_hom_skip > 0 && s_hom_skip < 14) {
                                if (++s_hom_skip == 14) lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                            }
                            if (!s_pad0) {
                                if (const uint64_t gp = image_game.find_symbol("_ZN4CPad6GetPadEi")) {
                                    s_pad0 = cpu.call(gp, {0});
                                }
                            }
                            const uint16_t dis = s_pad0 ? *reinterpret_cast<const uint16_t*>(s_pad0 + 0xa2ULL) : 0;
                            const char* cname = reinterpret_cast<const char*>(lb + 0x848a24ULL);
                            const uint8_t ws_active = *reinterpret_cast<const uint8_t*>(lb + 0x850af0ULL + 0x73ULL);
                            if (s_in_cutscene_ctx && cname && std::strcmp(cname, "HOMSWET") == 0) {
                                if (s_last_frame_draws >= 50) ++s_hom_f;
                                if (!s_hom_shot && ((s_hom_f >= 20 && lcs::subtitles::has_active_items()) || s_hom_f >= 200)) {
                                    s_hom_shot = true;
                                    std::cout << "  [VerifyHomswet] draws=" << s_last_frame_draws << " hasSub=" << lcs::subtitles::has_active_items() << " hom_f=" << s_hom_f << "\n" << std::flush;
                                    cap_frame("scratch/verify_cut_homswet.bmp");
                                }
                                if (s_hom_f >= 180 && s_hom_skip == 0) {
                                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                                    s_hom_skip = 1;
                                }
                            } else if (!s_in_cutscene_ctx && tr.is_in_gameplay() && s_hom_f > 0 && s_last_frame_draws >= 50 && dis == 0) {
                                ++s_dis_zero_f;
                                if (s_dis_zero_f == 25) {
                                    tr.give_weapon(33, 50);
                                    std::cout << "  [VerifyUnlocked] f=" << s_vframes << " inVeh=" << s_in_vehicle_ctx
                                              << " ws=" << static_cast<int>(ws_active)
                                              << " dev=" << static_cast<int>(lcs::game::LCSPrompts::instance().active_device()) << "\n" << std::flush;
                                    s_vstage = 115; s_vframes = 0;
                                }
                            } else {
                                s_dis_zero_f = 0;
                                if (s_hom_f > 0 && (s_vframes % 120 == 0)) {
                                    std::cout << "  [VerifyWaitCtrl] f=" << s_vframes << " cut=" << s_in_cutscene_ctx
                                              << " ws=" << static_cast<int>(ws_active) << " gp=" << tr.is_in_gameplay()
                                              << " dis=" << dis << " draws=" << s_last_frame_draws << "\n" << std::flush;
                                }
                            }
                        } else if (s_vstage == 115) {
                            static bool s_got_help_shot = false;
                            static bool s_brief_ready = false;
                            if (!s_got_help_shot && lcs::subtitles::has_active_help_box()) {
                                s_got_help_shot = true;
                                std::cout << "  [VerifyHelpBox] f=" << s_vframes
                                          << " dev=" << static_cast<int>(lcs::game::LCSPrompts::instance().active_device()) << "\n" << std::flush;
                                cap_frame("scratch/verify_helpbox_kb.bmp");
                            }
                            if ((s_vframes % 20 == 0) || s_vframes >= 1500) {
                                const auto briefs = tr.read_real_briefs();
                                bool has_help = false, has_obj = false;
                                for (const auto& b : briefs) {
                                    if (b.category == 1) has_help = true;
                                    if (b.category == 2) has_obj = true;
                                }
                                if ((has_help && has_obj && s_got_help_shot) || s_vframes >= 1500) {
                                    std::cout << "  [VerifyBriefs] total=" << briefs.size()
                                              << " hasHelp=" << has_help << " hasObj=" << has_obj
                                              << " helpShot=" << s_got_help_shot << "\n";
                                    for (size_t i = 0; i < briefs.size(); ++i) {
                                        std::cout << "    [" << i << "] cat=" << briefs[i].category
                                                  << " spk=\"" << briefs[i].speaker << "\" txt=\"" << briefs[i].text << "\"\n";
                                    }
                                    std::cout << std::flush;
                                    s_brief_ready = true;
                                }
                            }
                            if (s_brief_ready) {
                                ui::openGlobalMenu(ui::GlobalTab::Registro);
                                s_vstage = 12; s_vframes = 0;
                            }
                        } else if (s_vstage == 12) {
                            if (s_vframes == 6) { cap_frame("scratch/verify_brief_all.bmp"); }
                            else if (s_vframes == 10 && std::getenv("LCS_DIAG_SUB_LANG")) {
                                std::cout << "  [VerifyDone] Subtitle & Briefs check complete.\n" << std::flush;
                                break;
                            }
                            else if (s_vframes == 8) { send_vk(VK_DOWN, true, true); }
                            else if (s_vframes == 10) { send_vk(VK_DOWN, false, true); }
                            else if (s_vframes == 26) { cap_frame("scratch/verify_brief_dlg.bmp"); }
                            else if (s_vframes == 28) { send_vk(VK_DOWN, true, true); }
                            else if (s_vframes == 30) { send_vk(VK_DOWN, false, true); }
                            else if (s_vframes == 46) { cap_frame("scratch/verify_brief_help.bmp"); }
                            else if (s_vframes == 48) { send_vk(VK_DOWN, true, true); }
                            else if (s_vframes == 50) { send_vk(VK_DOWN, false, true); }
                            else if (s_vframes == 66) { cap_frame("scratch/verify_brief_obj.bmp"); }
                            else if (s_vframes == 70) { ui::closeGlobalMenu(); s_vstage = 13; s_vframes = 0; }
                        } else if (s_vstage == 13) {
                            const float* ped_zoom = reinterpret_cast<const float*>(lb + 0x850af0ULL + 0x164ULL);
                            static int s_exit_tried = 0;
                            static int s_foot_ok = 0;
                            if (s_in_vehicle_ctx) {
                                if (s_exit_tried == 0) { send_vk('F', true); s_exit_tried = 1; }
                                else if (s_exit_tried == 4) { send_vk('F', false); }
                                else if (s_exit_tried == 90) { s_exit_tried = 0; }
                                ++s_exit_tried;
                                s_foot_ok = 0;
                                s_vframes = 0;
                            } else {
                                const uint64_t gp = image_game.find_symbol("_ZN4CPad6GetPadEi");
                                const uint64_t pad0 = gp ? cpu.call(gp, {0}) : 0;
                                const uint16_t dis = pad0 ? *reinterpret_cast<const uint16_t*>(pad0 + 0xa2ULL) : 0;
                                if (dis != 0) { s_foot_ok = 0; s_vframes = 0; }
                                else {
                                    ++s_foot_ok;
                                    if (s_foot_ok == 25) {
                                        cap_frame("scratch/verify_hud_uw.bmp");
                                        s_cam_p0 = *ped_zoom;
                                        send_vk('V', true);
                                    } else if (s_foot_ok == 28) send_vk('V', false);
                                    else if (s_foot_ok == 42) { s_cam_p1 = *ped_zoom; send_vk('V', true); }
                                    else if (s_foot_ok == 45) send_vk('V', false);
                                    else if (s_foot_ok == 59) { s_cam_p2 = *ped_zoom; send_vk('V', true); }
                                    else if (s_foot_ok == 62) send_vk('V', false);
                                    else if (s_foot_ok == 76) {
                                        s_cam_p3 = *ped_zoom;
                                        std::cout << "  [VerifyCamFoot] V sequence: " << s_cam_p0 << " -> " << s_cam_p1
                                                  << " -> " << s_cam_p2 << " -> " << s_cam_p3 << "\n" << std::flush;
                                        lcs::input::InputSystem::instance().queue_joy_button_down(0, 12);
                                    } else if (s_foot_ok == 79) lcs::input::InputSystem::instance().queue_joy_button_up(0, 12);
                                    else if (s_foot_ok == 88) {
                                        std::cout << "  [VerifyCamDpad] after DPadUp zoom=" << *ped_zoom << "\n" << std::flush;
                                        set_hold_vk('S', true);
                                    } else if (s_foot_ok == 260) {
                                        set_hold_vk('S', false);
                                        set_hold_vk('D', true);
                                    } else if (s_foot_ok == 290) {
                                        set_hold_vk('D', false);
                                        set_hold_vk('W', true);
                                    } else if (s_foot_ok == 310) {
                                        set_hold_vk('W', false);
                                    } else if (s_foot_ok == 330) {
                                        const auto ps = tr.read_snapshot();
                                        std::cout << "  [VerifyWalkedAway] pos=(" << ps.pos[0] << "," << ps.pos[1] << "," << ps.pos[2] << ")\n" << std::flush;
                                        tr.spawn_vehicle(178); // PCJ-600
                                        s_vstage = 14; s_vframes = 0;
                                    }
                                }
                            }
                        } else if (s_vstage == 14) {
                            if (s_vframes == 50) set_hold_vk('W', true);
                            else if (s_vframes == 260) set_hold_vk('W', false);
                            if (s_vframes == 65 || s_vframes == 100 || s_vframes == 135 || s_vframes == 170 ||
                                s_vframes == 210 || s_vframes == 250 || s_vframes == 290) {
                                send_vk('F', true);
                            } else if (s_vframes == 69 || s_vframes == 104 || s_vframes == 139 || s_vframes == 174 ||
                                       s_vframes == 214 || s_vframes == 254 || s_vframes == 294) {
                                send_vk('F', false);
                            }
                            if (s_in_vehicle_ctx || s_vframes >= 420) {
                                set_hold_vk('W', false);
                                send_vk('F', false);
                                const uint64_t gp = image_game.find_symbol("_ZN4CPad6GetPadEi");
                                const uint64_t pad0 = gp ? cpu.call(gp, {0}) : 0;
                                const uint16_t dis = pad0 ? *reinterpret_cast<const uint16_t*>(pad0 + 0xa2ULL) : 0;
                                static int s_veh_ready = 0;
                                if (s_in_vehicle_ctx && dis != 0 && s_vframes < 420) {
                                    s_veh_ready = 0;
                                } else if (++s_veh_ready == 25 || s_vframes >= 420) {
                                    const float* car_zoom = reinterpret_cast<const float*>(lb + 0x850af0ULL + 0xdcULL);
                                    s_car_c0 = *car_zoom;
                                    const uint64_t fv = image_game.find_symbol("_Z17FindPlayerVehiclev");
                                    const uint64_t veh = fv ? cpu.call(fv, {}) : 0;
                                    const uint16_t mid = veh ? *reinterpret_cast<const uint16_t*>(veh + 0x7cULL) : 0;
                                    std::cout << "  [VerifyBikeMounted] at frame=" << s_vframes << " inVeh=" << s_in_vehicle_ctx
                                              << " model=" << mid << " dis=" << dis << " carZoom0=" << s_car_c0 << "\n" << std::flush;
                                    send_vk('V', true);
                                    s_vstage = 15; s_vframes = 0;
                                }
                            }
                        } else if (s_vstage == 15) {
                            const float* car_zoom = reinterpret_cast<const float*>(lb + 0x850af0ULL + 0xdcULL);
                            static float s_car_c2 = -1.0f;
                            auto log_bike = [&](const char* tag) {
                                const auto snap = tr.read_snapshot();
                                const uint64_t fv = image_game.find_symbol("_Z17FindPlayerVehiclev");
                                const uint64_t veh = fv ? cpu.call(fv, {}) : 0;
                                float fx = 0, fy = 0, uz = 0;
                                uint16_t mid = 0;
                                if (veh) {
                                    mid = *reinterpret_cast<const uint16_t*>(veh + 0x7cULL);
                                    fx = *reinterpret_cast<const float*>(veh + 0x20ULL);
                                    fy = *reinterpret_cast<const float*>(veh + 0x24ULL);
                                    uz = *reinterpret_cast<const float*>(veh + 0x38ULL);
                                }
                                std::cout << "  [" << tag << "] f=" << s_vframes << " inVeh=" << s_in_vehicle_ctx
                                          << " model=" << mid << " pos=(" << snap.pos[0] << "," << snap.pos[1] << "," << snap.pos[2] << ")"
                                          << " fwd=(" << fx << "," << fy << ") upZ=" << uz << "\n" << std::flush;
                            };
                            if (s_vframes == 3) send_vk('V', false);
                            else if (s_vframes == 14) { s_car_c1 = *car_zoom; send_vk('V', true); }
                            else if (s_vframes == 17) send_vk('V', false);
                            else if (s_vframes == 28) {
                                s_car_c2 = *car_zoom;
                                std::cout << "  [VerifyCamVeh] V: " << s_car_c0 << " -> " << s_car_c1 << " -> " << s_car_c2 << "\n" << std::flush;
                                set_hold_vk('W', true);
                            }
                            if (s_vframes == 30 || s_vframes == 80 || s_vframes == 135) log_bike("VerifyBikeDrive");
                            if (s_vframes == 130) cap_frame("scratch/verify_bike_drive.bmp");
                            if (s_vframes == 140) {
                                set_hold_vk('W', false);
                                set_hold_vk('S', true);
                            } else if (s_vframes == 200) {
                                log_bike("VerifyBikeBrake");
                                cap_frame("scratch/verify_bike_brake.bmp");
                            } else if (s_vframes == 280) {
                                log_bike("VerifyBikeReverse");
                                set_hold_vk('S', false);
                                set_hold_vk('W', true); set_hold_vk('A', true);
                            } else if (s_vframes == 350) {
                                set_hold_vk('W', false); set_hold_vk('A', false);
                                log_bike("VerifyBikeTurnL");
                                cap_frame("scratch/verify_bike_turn.bmp");
                                set_hold_vk('W', true); set_hold_vk('D', true);
                            } else if (s_vframes == 410) {
                                set_hold_vk('W', false); set_hold_vk('D', false);
                                log_bike("VerifyBikeTurnR");
                            } else if (s_vframes == 415) {
                                std::cout << "  [VerifyDone] All automated checks complete.\n" << std::flush;
                                break;
                            }
                        }
                    }
                }
                lcs::input::InputSystem::instance().dispatch_events(cpu, env_addr, dummy_clazz);
                lcs::game::LCSPrompts::instance().update(pc_controls.in_menu(), s_in_vehicle_ctx);
                lcs::game::LCSPs2Effects::instance().update(dt);
                lcs::game::LcsWorldHd::instance().on_frame(rendered_frames);
                lcs::game::LCSTrainer::instance().tick(dt);
                if (lcs::game::LCSTrainer::instance().consume_quit_request()) {
                    std::cout << "  [GlobalMenu] Quit to Windows requested.\n" << std::flush;
                    break;
                }
            }

            bridge.begin_frame();
            lcs::subtitles::begin_frame();
            uint64_t pre_swaps = bridge.get_frame_count();
            uint64_t pre_draws = bridge.get_draw_call_count();

            if (sym_touchscreen) {
                const uint64_t ts_obj = *reinterpret_cast<const uint64_t*>(sym_touchscreen);
                if (ts_obj) {
                    *reinterpret_cast<uint8_t*>(ts_obj + 0x8) = 1;
                    // Keep radar in classic PC/PS2 bottom-left position (button index 24 at GTouchscreen+0xe0).
                    constexpr float kPcRadarY = 525.0f;
                    const uint64_t rbtn = *reinterpret_cast<const uint64_t*>(ts_obj + 0xe0);
                    if (rbtn) {
                        *reinterpret_cast<float*>(rbtn + 0x0c) = kPcRadarY;
                    }
                }
            }
            if (sym_prefs_show_pad_input) {
                *reinterpret_cast<uint8_t*>(sym_prefs_show_pad_input) = 0;
            }
            if (sym_prefs_brightness) {
                static int32_t s_last_bri = -1;
                static float s_last_con = -1.0f;
                static float s_last_sat = -1.0f;
                const int32_t cur_bri = std::clamp(*reinterpret_cast<const int32_t*>(sym_prefs_brightness), 51, 511);
                const float cur_con = std::clamp(*reinterpret_cast<const float*>(sym_con_slider), 0.0f, 1.0f);
                const float cur_sat = std::clamp(*reinterpret_cast<const float*>(sym_sat_slider), 0.0f, 1.0f);
                if (cur_bri != s_last_bri || std::fabs(cur_con - s_last_con) > 1e-4f || std::fabs(cur_sat - s_last_sat) > 1e-4f) {
                    const bool had_prev = (s_last_bri >= 0);
                    s_last_bri = cur_bri;
                    s_last_con = cur_con;
                    s_last_sat = cur_sat;
                    const float bri_mul = 1.0f + ((static_cast<float>(cur_bri) - 336.0f) / 460.0f) * 0.60f;
                    const float con_mul = 0.80f + cur_con * 0.64f;
                    const float sat_mul = 0.60f + cur_sat * 1.28f;
                    bridge.set_color_grading(bri_mul, con_mul, sat_mul);
                    if (had_prev) {
                        const int bri_pct = std::clamp(static_cast<int>(std::lround(((static_cast<float>(cur_bri) - 51.0f) / 460.0f) * 100.0f)), 0, 100);
                        const int con_pct = std::clamp(static_cast<int>(std::lround(cur_con * 100.0f)), 0, 100);
                        const int sat_pct = std::clamp(static_cast<int>(std::lround(cur_sat * 100.0f)), 0, 100);
                        if (bri_pct != lcs::core::g_config.brightness) {
                            lcs::core::g_config.brightness = bri_pct;
                            lcs::core::save_config_setting("gtalcs/lcs.conf", "brightness", std::to_string(bri_pct));
                        }
                        if (con_pct != lcs::core::g_config.contrast) {
                            lcs::core::g_config.contrast = con_pct;
                            lcs::core::save_config_setting("gtalcs/lcs.conf", "contrast", std::to_string(con_pct));
                        }
                        if (sat_pct != lcs::core::g_config.saturation) {
                            lcs::core::g_config.saturation = sat_pct;
                            lcs::core::save_config_setting("gtalcs/lcs.conf", "saturation", std::to_string(sat_pct));
                        }
                    }
                }
            }

            cpu.set_fp_float(0, dt);
            cpu.call(fn_draw_frame, {env_addr, dummy_clazz});

            bool guest_swapped = (bridge.get_frame_count() > pre_swaps);
            // Present if the guest did not explicitly invoke eglSwapBuffers during viewOnDrawFrame.
            if (!guest_swapped) {
                bridge.end_frame_and_present();
            }
            int active_cap_fps = lcs::core::g_config.max_fps;
            if (lcs::core::g_config.vsync && active_cap_fps > 0) {
                static int s_cached_hz = 0;
                static auto s_hz_checked = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                const auto hz_now = std::chrono::steady_clock::now();
                if (hz_now - s_hz_checked >= std::chrono::seconds(1)) {
                    s_hz_checked = hz_now;
#if defined(LCS_PLATFORM_WINDOWS)
                    DEVMODEW dm{};
                    dm.dmSize = sizeof(dm);
                    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24) {
                        s_cached_hz = static_cast<int>(dm.dmDisplayFrequency);
                    }
#endif
                }
                // Let DXGI Present(1) pace cleanly to monitor VBlank unless cap is an intentional divisor (e.g. 30 on 60/75Hz)
                if (s_cached_hz <= 0 || active_cap_fps > (s_cached_hz / 2) + 2) {
                    active_cap_fps = 0;
                }
            }
            if (active_cap_fps > 0) {
                const auto target_dur = std::chrono::nanoseconds(1000000000LL / active_cap_fps);
                if (rendered_frames == 0) {
                    next_frame_deadline = frame_start + target_dur;
                } else {
                    next_frame_deadline += target_dur;
                }
                const auto now_pace = std::chrono::steady_clock::now();
                if (now_pace > next_frame_deadline + target_dur) {
                    next_frame_deadline = now_pace;
                }
#if defined(LCS_PLATFORM_WINDOWS)
                typedef HANDLE (WINAPI *CreateWaitableTimerExWFn)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
                static auto pCreateTimerEx = reinterpret_cast<CreateWaitableTimerExWFn>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW"));
                static HANDLE s_pace_timer = pCreateTimerEx ? pCreateTimerEx(nullptr, nullptr, 0x00000002u, TIMER_ALL_ACCESS) : nullptr;
                const auto rem_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(next_frame_deadline - now_pace).count();
                if (rem_ns > 800000LL && s_pace_timer) {
                    LARGE_INTEGER li;
                    li.QuadPart = -static_cast<LONGLONG>((rem_ns - 650000LL) / 100LL);
                    if (li.QuadPart < 0 && SetWaitableTimerEx(s_pace_timer, &li, 0, nullptr, nullptr, nullptr, 0)) {
                        WaitForSingleObject(s_pace_timer, INFINITE);
                    }
                } else if (now_pace + std::chrono::milliseconds(2) < next_frame_deadline) {
                    std::this_thread::sleep_until(next_frame_deadline - std::chrono::microseconds(1500));
                }
#else
                if (now_pace + std::chrono::milliseconds(2) < next_frame_deadline) {
                    std::this_thread::sleep_until(next_frame_deadline - std::chrono::microseconds(1500));
                }
#endif
                while (std::chrono::steady_clock::now() < next_frame_deadline) {
                    std::this_thread::yield();
                }
            }

            rendered_frames++;
            uint64_t frame_draws = bridge.get_draw_call_count() - pre_draws;
            if (rendered_frames <= 15 || rendered_frames % 60 == 0) {
                bool in_menu = fn_is_main_menu ? (cpu.call(fn_is_main_menu, {env_addr, dummy_clazz}) != 0) : false;
                std::cout << "  - Frame " << rendered_frames
                          << " (dt=" << dt << "s, frame_draws=" << frame_draws
                          << ", guest_swap=" << (guest_swapped ? "yes" : "no")
                          << ", main_menu=" << (in_menu ? "yes" : "no")
                          << ", total_draws=" << bridge.get_draw_call_count()
                          << ", shaders=" << bridge.get_shader_count()
                          << ", programs=" << bridge.get_program_count()
                          << ", textures=" << bridge.get_texture_count()
                          << ", buffers=" << bridge.get_buffer_count() << ").\n" << std::flush;
            }

            if (max_frames > 0 && rendered_frames >= max_frames) {
                break;
            }
        }

        std::cout << "\n[Result] Render loop exited after " << rendered_frames << " frames ("
                  << "draws=" << bridge.get_draw_call_count()
                  << ", shaders=" << bridge.get_shader_count()
                  << ", programs=" << bridge.get_program_count()
                  << ", textures=" << bridge.get_texture_count()
                  << ", buffers=" << bridge.get_buffer_count() << ").\n";
        std::cout << "[LogicalFS] Session asset hits: modloader="
                  << lcs::vfs::LogicalFS::instance().mod_hit_count()
                  << ", ipad_retina=" << lcs::vfs::LogicalFS::instance().ipad_hit_count() << "\n";

        if (dump_render) {
            std::filesystem::create_directories("scratch");
            bridge.dump_diagnostics("scratch/render_dump.txt");
            std::cout << "  - Render diagnostics saved to scratch/render_dump.txt\n";
        }
    }

    lcs::audio::shutdown_audio_system();
    lcs::graphics::RenderBridge::instance().shutdown();
    if (!headless) {
        lcs::graphics::destroy_game_window();
    }

#if defined(_WIN32)
    timeEndPeriod(1);
    ExitProcess(0);
#else
    _exit(0);
#endif
    return 0;
}
