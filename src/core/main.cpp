#include <iostream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <atomic>
#include <string>
#include <cstdlib>
#include <chrono>
#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include "cpu/elf_loader.h"
#include "cpu/dynarmic_core.h"
#include "cpu/callbacks.h"
#include "os/jni_bridge.h"
#include "os/bionic_libc.h"
#include "graphics/window.h"
#include "graphics/render_bridge.h"

static lcs::cpu::DynarmicCore* s_active_cpu = nullptr;

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
    std::cerr << "\n[CRASH TRAP] Exception Code: 0x" << std::hex << rec->ExceptionCode
              << " at host address 0x" << rec->ExceptionAddress << std::dec << "\n";
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
    ExitProcess(1);
}
#endif

int main(int argc, char* argv[]) {
    bool headless = false;
    bool dump_render = false;
    uint64_t max_frames = 0;
    uint32_t win_width = 1280;
    uint32_t win_height = 720;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--dump-render") {
            dump_render = true;
        } else if (arg == "--max-frames" && i + 1 < argc) {
            max_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--width" && i + 1 < argc) {
            win_width = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--height" && i + 1 < argc) {
            win_height = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

#if defined(_WIN32)
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        // Suppress fast-fail on invalid CRT arguments (e.g. POSIX mode flags in fopen)
    });
    _set_thread_local_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        // Suppress fast-fail on invalid CRT arguments
    });
#endif

#if defined(LCS_PLATFORM_WINDOWS)
    AddVectoredExceptionHandler(1, CrashHandler);
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
        std::cout << "  [3] Invoking setIsTVDevice(false)..." << std::flush;
        cpu.call(fn_set_is_tv, {env_addr, dummy_clazz, 0});
        std::cout << " OK\n";
    }

    // 4. setHasVibrator
    uint64_t fn_set_vibrator = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_setHasVibrator");
    if (fn_set_vibrator) {
        std::cout << "  [4] Invoking setHasVibrator(false)..." << std::flush;
        cpu.call(fn_set_vibrator, {env_addr, dummy_clazz, 0});
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
        uint64_t str_pc = lcs::os::JNIBridge::instance().create_string_utf("PC");
        uint64_t str_x86 = lcs::os::JNIBridge::instance().create_string_utf("x86_64");
        std::cout << "  [6] Invoking setDeviceInfo(false, \"PC\", \"PC\", \"x86_64\")..." << std::flush;
        cpu.call(fn_set_device_info, {env_addr, dummy_clazz, 0, str_pc, str_pc, str_x86});
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

    std::cout << "\n[Step 9] Initializing Native Desktop Window & Direct3D 12 Swapchain...\n";
    void* native_window = nullptr;
    if (!headless) {
        if (!lcs::graphics::create_game_window(
                static_cast<int>(win_width),
                static_cast<int>(win_height),
                "GTA: Liberty City Stories - Native PC Port (D3D12)")) {
            std::cerr << "[ERROR] Failed to create native game window.\n";
            return 1;
        }
        lcs::graphics::get_window_size(win_width, win_height);
        native_window = lcs::graphics::get_native_window_handle();
        lcs::graphics::set_window_resize_callback([](uint32_t new_w, uint32_t new_h) {
            lcs::graphics::RenderBridge::instance().set_surface_dimensions(new_w, new_h);
        });
    }

    if (!lcs::graphics::RenderBridge::instance().initialize(
            lcs::graphics::RenderBackend::Direct3D12,
            native_window,
            win_width,
            win_height)) {
        std::cerr << "[ERROR] Failed to initialize RenderBridge.\n";
        return 1;
    }

    // 9. viewOnSurfaceCreated
    uint64_t fn_surf_created = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceCreated");
    if (fn_surf_created) {
        std::cout << "  [9] Invoking viewOnSurfaceCreated()..." << std::flush;
        cpu.call(fn_surf_created, {env_addr, dummy_clazz});
        std::cout << " OK\n";
    }

    // 10. viewOnSurfaceChanged
    uint64_t fn_surf_changed = image_game.find_symbol("Java_com_rockstargames_gtalcs_GTAJNIlib_viewOnSurfaceChanged");
    if (fn_surf_changed) {
        std::cout << "  [10] Invoking viewOnSurfaceChanged(" << win_width << ", " << win_height << ")..." << std::flush;
        cpu.call(fn_surf_changed, {env_addr, dummy_clazz, win_width, win_height});
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

    if (fn_draw_frame) {
        std::cout << "  [11] Entering main frame loop"
                  << (max_frames > 0 ? " (max_frames=" + std::to_string(max_frames) + ")" : "")
                  << "...\n" << std::flush;

        auto& bridge = lcs::graphics::RenderBridge::instance();
        uint64_t rendered_frames = 0;
        auto last_time = std::chrono::steady_clock::now();
        while (headless || lcs::graphics::poll_window_events()) {
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
            }
            if (pending.start_game && fn_start_game) {
                std::cout << "  [JNI Dispatch] RockstarJNIlib.StartGame()\n" << std::flush;
                cpu.call(fn_start_game, {env_addr, dummy_clazz});
            }
            if (pending.quit_requested) {
                std::cout << "  [JNI Dispatch] QuitApp requested by game.\n" << std::flush;
                break;
            }

            auto now = std::chrono::steady_clock::now();
            float dt = std::chrono::duration<float>(now - last_time).count();
            last_time = now;
            if (rendered_frames == 0 || dt <= 0.0f || dt > 0.1f) {
                dt = 1.0f / 60.0f;
            }

            bridge.begin_frame();
            uint64_t pre_swaps = bridge.get_frame_count();
            uint64_t pre_draws = bridge.get_draw_call_count();

            cpu.set_fp_float(0, dt);
            cpu.call(fn_draw_frame, {env_addr, dummy_clazz});

            bool guest_swapped = (bridge.get_frame_count() > pre_swaps);
            // Present if the guest did not explicitly invoke eglSwapBuffers during viewOnDrawFrame.
            if (!guest_swapped) {
                bridge.end_frame_and_present();
            }

            rendered_frames++;
            uint64_t frame_draws = bridge.get_draw_call_count() - pre_draws;
            if (rendered_frames <= 15 || rendered_frames % 60 == 0) {
                std::cout << "  - Frame " << rendered_frames
                          << " (dt=" << dt << "s, frame_draws=" << frame_draws
                          << ", guest_swap=" << (guest_swapped ? "yes" : "no")
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

        if (dump_render) {
            std::filesystem::create_directories("scratch");
            bridge.dump_diagnostics("scratch/render_dump.txt");
            std::cout << "  - Render diagnostics saved to scratch/render_dump.txt\n";
        }
    }

    lcs::graphics::RenderBridge::instance().shutdown();
    if (!headless) {
        lcs::graphics::destroy_game_window();
    }

#if defined(_WIN32)
    ExitProcess(0);
#else
    _exit(0);
#endif
    return 0;
}
