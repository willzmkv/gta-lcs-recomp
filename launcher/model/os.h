// What the launcher calls things on each OS, in one place: the binaries, the file manager, folder separators in text.
#pragma once

namespace launcher::os
{
#ifdef _WIN32
    constexpr bool kWindows = true;
    constexpr const char *kGameExe = "lcs.exe";
    constexpr const char *kLauncherExe = "LCSLauncher.exe";
    constexpr const char *kFileManager = "Explorer";
    constexpr const char *kShortcutName = "GTA Liberty City Stories.lnk";
    constexpr const char *kBuildGame = "lcs.exe";
    constexpr const char *kBuildIni = "lcs.ini";
    constexpr const char *kTools = "tools\\";
#else
    constexpr bool kWindows = false;
    constexpr const char *kGameExe = "lcs";
    constexpr const char *kLauncherExe = "LCSLauncher";
    constexpr const char *kFileManager = "Files";
    constexpr const char *kShortcutName = "gta-lcs-launcher.desktop";
    constexpr const char *kBuildGame = "lcs";
    constexpr const char *kBuildIni = "lcs.ini";
    constexpr const char *kTools = "tools/";
#endif
}
