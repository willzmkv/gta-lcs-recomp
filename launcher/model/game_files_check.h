// Dual-source "Frankenstein" validator for GTA: Liberty City Stories (PC):
//   1. Android Source: .apk / .obb / .zip OR extracted gtalcs/gamefiles/android-libs/libGame.so (ARM64 ELF64)
//      + gtalcs/gamefiles/assets/data_main.wad (+ optional data_music.wad).
//   2. iPad Source: GTALcs.ipa (ZIP with Payload/GTALcs.app/WorldPs.glsl & Data/TIMECYC.DAT)
//      OR extracted assets/ipad/.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace launcher
{
    enum class SourceKind
    {
        Unknown = 0,
        Android = 1,
        Ipad = 2,
    };

    struct AndroidSourceResult
    {
        bool ok = false;
        bool needsExtract = false; // true if user picked a valid .apk/.obb that hasn't been extracted yet
        std::string code;          // ok, not_found, invalid_elf, wrong_arch, missing_wad, not_android, archive_ready
        std::string message;
        std::filesystem::path libGamePath;
        std::filesystem::path dataMainPath;
        std::filesystem::path dataMusicPath;
        std::filesystem::path archivePath;
    };

    struct IpadSourceResult
    {
        bool ok = false;
        bool needsExtract = false;
        std::string code; // ok, not_found, not_ipa, missing_assets
        std::string message;
        std::filesystem::path ipaPath;
        std::filesystem::path extractedDir;
        int shaderCount = 0;
        bool hasTimecyc = false;
    };

    struct GameFilesCheck
    {
        AndroidSourceResult android;
        IpadSourceResult ipad;

        bool ready() const { return android.ok && ipad.ok; }
    };

    // Validates a single file (either .apk, .obb, .zip, .so, .wad) or checks the extracted gtalcs/ folder.
    AndroidSourceResult checkAndroidSource(const std::filesystem::path &root, const std::filesystem::path &picked = {});

    // Validates an iPad .ipa file or the extracted assets/ipad/ folder.
    IpadSourceResult checkIpadSource(const std::filesystem::path &root, const std::filesystem::path &picked = {});

    // Inspects a file dropped onto the launcher window to route it to the Android or iPad slot automatically.
    SourceKind classifyDroppedFile(const std::filesystem::path &path);

    // Searches common folders (root, Downloads, Desktop, Documents) for candidate .apk/.obb and .ipa files.
    std::vector<std::filesystem::path> androidCandidates(const std::filesystem::path &root);
    std::vector<std::filesystem::path> ipadCandidates(const std::filesystem::path &root);
}
