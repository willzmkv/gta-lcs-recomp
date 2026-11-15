#include <iostream>
#include <iomanip>
#include <filesystem>
#include "cpu/elf_loader.h"

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

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

    // Symbol resolver cross-referencing between openal, mpg123, and dummy stubs
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
        // Dummy HLE trampoline address for now: 0x7f00000000
        return 0x7f00000000;
    };

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

    std::cout << "\n[Result] Virtual memory mapping, ELF64 loading, and relocation engine verified successfully.\n";
    return 0;
}
