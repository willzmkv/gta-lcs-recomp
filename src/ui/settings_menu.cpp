#include "ui/settings_menu.h"

#include "settings/settings.h"
#include "ui/ui_theme.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        bool g_open = false;
        int g_pageIdx = 0;
        std::string g_helpText = "Use F2 or Escape to close the Settings menu. All changes save automatically to lcs.ini.";

        constexpr const char *kPages[] = {
            "Display", "Graphics", "Audio", "Keyboard", "Gamepad", "Enhancements", "Mods"};
        constexpr int kPageCount = 7;

        void drawRowChrome(ImDrawList *dl, ImVec2 p0, ImVec2 p1, int rowIdx, bool hovered, const char *labelText)
        {
            using namespace palette;
            const ImU32 bg = hovered ? u32(kRowHover, 0.96f) : u32((rowIdx % 2 == 0) ? kRowEven : kRowOdd, 0.94f);
            dl->AddRectFilled(p0, p1, bg);
            if (hovered)
                dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f, p1.y), u32(kPink));
            dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kTealLine, 0.18f), 1.0f);

            ImGui::PushFont(hovered ? fontSemibold() : fontRegular());
            const ImVec2 ts = ImGui::CalcTextSize(labelText);
            dl->AddText(ImVec2(p0.x + 18.0f, (p0.y + p1.y - ts.y) * 0.5f),
                        u32(hovered ? kTealDark : kTeal), labelText);
            ImGui::PopFont();
        }
    } // namespace

    void openSettingsMenu(const std::string &page)
    {
        g_open = true;
        for (int i = 0; i < kPageCount; ++i)
        {
            if (page == kPages[i])
            {
                g_pageIdx = i;
                break;
            }
        }
    }

    void closeSettingsMenu() { g_open = false; }
    void toggleSettingsMenu() { g_open = !g_open; }
    bool settingsMenuOpen() { return g_open; }
    bool settingsMenuBlocksInput() { return g_open; }

    void drawSettingsMenu()
    {
        if (!g_open)
            return;

        using namespace palette;
        ImGuiIO &io = ImGui::GetIO();
        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;
        if (sw < 100.0f || sh < 100.0f)
            return;

        // Escape closes F2 menu
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        {
            g_open = false;
            return;
        }

        // Fullscreen dark noir backdrop with subtle crimson bottom vignette
        ImDrawList *bgDl = ImGui::GetBackgroundDrawList();
        bgDl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(sw, sh),
                                      u32(kSkyTop, 0.88f), u32(kSkyTop, 0.88f),
                                      u32(kSkyBot, 0.92f), u32(kSkyBot, 0.92f));

        const float winW = std::min(920.0f, sw - 48.0f);
        const float winH = std::min(620.0f, sh - 48.0f);
        const ImVec2 winPos((sw - winW) * 0.5f, (sh - winH) * 0.5f);

        ImGui::SetNextWindowPos(winPos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(kHeaderBg, 0.97f));
        ImGui::PushStyleColor(ImGuiCol_Border, rgba(kPink, 0.70f));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##LCSSettingsMenu", nullptr, flags))
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();

            // Header bar (54px) with crimson bottom line
            const float hdrH = 54.0f;
            dl->AddRectFilled(wp, ImVec2(wp.x + winW, wp.y + hdrH), u32(kHeaderBg));
            dl->AddLine(ImVec2(wp.x, wp.y + hdrH), ImVec2(wp.x + winW, wp.y + hdrH), u32(kPink), 2.0f);

            ImGui::PushFont(fontSemibold());
            dl->AddText(ImVec2(wp.x + 22.0f, wp.y + 16.0f), u32(kTealDark),
                        "GRAND THEFT AUTO: LIBERTY CITY STORIES  //  SETTINGS");
            const char *closeHint = "F2 / ESC: Close";
            const ImVec2 hs = ImGui::CalcTextSize(closeHint);
            dl->AddText(ImVec2(wp.x + winW - hs.x - 20.0f, wp.y + 18.0f), u32(kTealMuted), closeHint);
            ImGui::PopFont();

            // Left Page Rail (190px)
            const float railW = 190.0f;
            const float helpH = 54.0f;
            const float bodyY = wp.y + hdrH;
            const float bodyH = winH - hdrH - helpH;
            dl->AddRectFilled(ImVec2(wp.x, bodyY), ImVec2(wp.x + railW, bodyY + bodyH), u32(kRailBg));
            dl->AddLine(ImVec2(wp.x + railW, bodyY), ImVec2(wp.x + railW, bodyY + bodyH), u32(kTealLine, 0.30f), 1.0f);

            for (int i = 0; i < kPageCount; ++i)
            {
                const float py = bodyY + 10.0f + i * 44.0f;
                const ImVec2 p0(wp.x + 8.0f, py);
                const ImVec2 p1(wp.x + railW - 8.0f, py + 38.0f);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##page_") + kPages[i]).c_str(), ImVec2(p1.x - p0.x, p1.y - p0.y)))
                    g_pageIdx = i;
                const bool active = (g_pageIdx == i);
                const bool hov = ImGui::IsItemHovered();
                const ImU32 fill = active ? u32(kPinkSoft) : hov ? u32(kTabHover) : u32(kTabIdle);
                dl->AddRectFilled(p0, p1, fill, 4.0f);
                drawPageIcon(dl, kPages[i], ImVec2(p0.x + 22.0f, (p0.y + p1.y) * 0.5f), 16.0f,
                             u32(active ? kOnPink : kTeal));
                ImGui::PushFont(active ? fontSemibold() : fontRegular());
                dl->AddText(ImVec2(p0.x + 42.0f, (p0.y + p1.y) * 0.5f - 8.0f),
                            u32(active ? kOnPink : kTeal), kPages[i]);
                ImGui::PopFont();
            }

            // Right Content Area
            const float contentX = wp.x + railW;
            const float contentW = winW - railW;
            ImGui::SetCursorScreenPos(ImVec2(contentX, bodyY));
            ImGui::BeginChild("##LCSPageContent", ImVec2(contentW, bodyH), false);

            // Keyboard page switching (Q / E or PageUp / PageDown)
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                g_pageIdx = (g_pageIdx - 1 + kPageCount) % kPageCount;
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                g_pageIdx = (g_pageIdx + 1) % kPageCount;

            auto &s = lcs::settings::get();
            bool dirty = false;
            int rowIdx = 0;
            const float rowH = 42.0f;
            const float ctrlX = contentX + contentW - 260.0f;

            auto sectionHeader = [&](const char *title) {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + contentW, p0.y + 30.0f);
                ImDrawList *cdl = ImGui::GetWindowDrawList();
                cdl->AddRectFilled(p0, p1, u32(kSectionBg));
                cdl->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), u32(kPink));
                ImGui::PushFont(fontSemibold());
                cdl->AddText(ImVec2(p0.x + 16.0f, p0.y + 7.0f), u32(kOnPink), title);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(contentW, 30.0f));
            };

            auto toggleRow = [&](const char *id, const char *label, int &val, const char *help) {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + contentW, p0.y + rowH);
                const bool hov = ImGui::IsMouseHoveringRect(p0, p1);
                if (hov) g_helpText = help;
                drawRowChrome(ImGui::GetWindowDrawList(), p0, p1, rowIdx++, hov, label);
                bool b = (val != 0);
                ImGui::SetCursorScreenPos(ImVec2(contentX + contentW - 72.0f, p0.y));
                if (vcToggle(id, b, rowH))
                {
                    val = b ? 1 : 0;
                    dirty = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto sliderRow = [&](const char *id, const char *label, int &val, int mn, int mx, int step, const char *suf, const char *help) {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + contentW, p0.y + rowH);
                const bool hov = ImGui::IsMouseHoveringRect(p0, p1);
                if (hov) g_helpText = help;
                drawRowChrome(ImGui::GetWindowDrawList(), p0, p1, rowIdx++, hov, label);
                ImGui::SetCursorScreenPos(ImVec2(ctrlX, p0.y));
                if (vcSlider(id, val, mn, mx, step, suf, rowH))
                    dirty = true;
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto cycleRow = [&](const char *id, const char *label, int &val, const char *const *opts, const int *vals, int n, const char *help) {
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                const ImVec2 p1(p0.x + contentW, p0.y + rowH);
                const bool hov = ImGui::IsMouseHoveringRect(p0, p1);
                if (hov) g_helpText = help;
                drawRowChrome(ImGui::GetWindowDrawList(), p0, p1, rowIdx++, hov, label);
                int sel = 0;
                for (int i = 0; i < n; ++i)
                    if (vals[i] == val) sel = i;
                ImGui::SetCursorScreenPos(ImVec2(ctrlX, p0.y));
                if (vcCycle(id, sel, opts, n, rowH))
                {
                    val = vals[sel];
                    dirty = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            switch (g_pageIdx)
            {
            case 0: // Display
            {
                sectionHeader("DISPLAY & TIMING");
                static const char *kWinOpts[] = {"Borderless Fullscreen", "Windowed"};
                static const int kWinVals[] = {0, 1};
                cycleRow("##win", "Display Mode", s.display_window, kWinOpts, kWinVals, 2, "Switch between Borderless Fullscreen (Alt+Enter) and Windowed.");

                static const char *kAspOpts[] = {"Auto", "16:9", "21:9", "4:3"};
                static const int kAspVals[] = {0, 169, 219, 43};
                cycleRow("##asp", "Aspect Ratio", s.display_aspect, kAspOpts, kAspVals, 4, "Controls viewport aspect ratio.");

                static const char *kHudOpts[] = {"Stretched", "Centered 4:3", "Widescreen"};
                static const int kHudVals[] = {0, 1, 2};
                cycleRow("##hud", "HUD Layout", s.display_hud, kHudOpts, kHudVals, 3, "Widescreen keeps HUD elements properly proportioned.");

                static const char *kFpsOpts[] = {"30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS", "Unlimited"};
                static const int kFpsVals[] = {30, 60, 120, 144, 240, 0};
                cycleRow("##fps", "FPS Limit", s.display_fps, kFpsOpts, kFpsVals, 6, "Frame rate cap applied in real time.");

                toggleRow("##vsync", "VSync", s.display_vsync, "Synchronizes frame presentation with monitor refresh rate.");

                static const char *kCntOpts[] = {"Off", "Simple", "Detailed"};
                static const int kCntVals[] = {0, 1, 2};
                cycleRow("##fpscnt", "FPS Counter", s.display_fps_counter, kCntOpts, kCntVals, 3, "Displays real-time FPS overlay in the top-right corner.");
                break;
            }
            case 1: // Graphics
            {
                sectionHeader("RENDERER & IPAD HD ENHANCEMENTS");
                static const char *kRenOpts[] = {"Direct3D 12", "Vulkan 1.4"};
                static const int kRenVals[] = {0, 1};
                cycleRow("##ren", "Graphics Backend (Restart)", s.display_renderer, kRenOpts, kRenVals, 2, "Selects Direct3D 12 or Vulkan 1.4 backend on next launch.");

                static const char *kMsaaOpts[] = {"Off", "2x MSAA", "4x MSAA", "8x MSAA"};
                static const int kMsaaVals[] = {1, 2, 4, 8};
                cycleRow("##msaa", "Anti-Aliasing (MSAA)", s.display_msaa, kMsaaOpts, kMsaaVals, 4, "Hardware multisample anti-aliasing.");

                static const char *kAnisoOpts[] = {"Off", "2x", "4x", "8x", "16x"};
                static const int kAnisoVals[] = {0, 2, 4, 8, 16};
                cycleRow("##aniso", "Anisotropic Filtering", s.display_aniso, kAnisoOpts, kAnisoVals, 5, "Sharpens road and building textures at oblique angles.");

                toggleRow("##sdw", "iPad Dynamic Shadows (_SDWMAP)", s.ipad_shadows, "Enables dynamic shadow mapping from the iPad Retina shaders.");
                toggleRow("##cg", "iPad Color Grading", s.color_grading, "Applies iPad Retina brightness, contrast and saturation grading.");
                sliderRow("##bri", "Brightness", s.brightness, 0, 100, 2, "%", "Adjusts overall display brightness in real time.");
                sliderRow("##con", "Contrast", s.contrast, 0, 100, 2, "%", "Adjusts color grading contrast in real time.");
                sliderRow("##sat", "Saturation", s.saturation, 0, 100, 2, "%", "Adjusts color grading saturation in real time.");
                sliderRow("##vd", "View Distance", s.view_distance, 0, 100, 5, "%", "Controls world draw distance scaling.");
                sectionHeader("PS2 GRAPHICS PIPELINE (SLUS_214.23)");
                toggleRow("##ps2_pipe", "PS2 Effects Pipeline (CMBlur + GS)", s.ps2_pipeline, "Enables the authentic PlayStation 2 post-processing pipeline (Radiosity + BlurRGB Color Filter + Trails).");
                static const char *kPs2ModeOpts[] = {"Recomp (PC Default)", "PS2 Original (SLUS_214.23)", "Hybrid (PS2 + Recomp)"};
                static const int kPs2ModeVals[] = {0, 1, 2};
                cycleRow("##ps2_tc", "Visual Mode & TimeCycle", s.ps2_timecycle, kPs2ModeOpts, kPs2ModeVals, 3, "Select specifically Recomp, PS2 Original, or Hybrid PS2 + Recomp.");
                toggleRow("##ps2_rad", "PS2 Radiosity & Bloom", s.ps2_radiosity, "Replicates PS2 GS 4-tap sub-pixel filter and radiosity bloom.");
                sliderRow("##ps2_radi", "PS2 Radiosity Intensity", s.ps2_radiosity_intensity, 0, 200, 10, "%", "Controls PS2 sky, headlight and ambient glow strength.");
                toggleRow("##ps2_cf", "PS2 Atmospheric Color Filter", s.ps2_color_filter, "Applies PS2 CTimeCycle FinalColour/BlurRGB warm atmospheric tint.");
                sliderRow("##ps2_cfi", "PS2 Color Filter Intensity", s.ps2_color_filter_intensity, 0, 200, 10, "%", "Controls PS2 golden daylight and night contrast balance.");
                toggleRow("##ps2_tr", "PS2 Trails (Temporal Persistence)", s.ps2_trails, "Replicates PS2 frame-history trails accumulator (gMinBlurAmt = 28).");
                sliderRow("##ps2_tri", "PS2 Trails Intensity", s.ps2_trails_intensity, 0, 200, 10, "%", "Controls temporal persistence of PS2 frame-history trails.");
                toggleRow("##ps2_spd", "PS2 High-Speed Motion Blur", s.ps2_speed_blur, "Adds peripheral radial motion blur when driving vehicles fast.");
                break;
            }
            case 2: // Audio
            {
                sectionHeader("AUDIO & RADIO (DATA_MUSIC.WAD)");
                sliderRow("##vol", "Master Volume", s.audio_volume, 0, 100, 5, "%", "Controls master OpenAL audio output volume.");
                sliderRow("##mus", "Music & Radio Volume", s.audio_music, 0, 100, 5, "%", "Controls radio stations and cutscenes from data_music.wad.");
                sliderRow("##sfx", "Sound Effects Volume", s.audio_sfx, 0, 100, 5, "%", "Controls weapons, engines, ambience and voices.");
                break;
            }
            case 3: // Keyboard
            {
                sectionHeader("KEYBOARD & MOUSE");
                toggleRow("##mfa", "1:1 Mouse Free Aim", s.mouse_free_aim, "Enables direct 1:1 mouse aiming on foot (src/game/lcs_free_aim.cpp).");
                sliderRow("##msens", "Mouse Sensitivity", s.mouse_sensitivity, 10, 400, 5, "%", "Controls mouse camera and aiming speed.");
                toggleRow("##minv", "Invert Mouse Y-Axis", s.mouse_invert_y, "Inverts vertical mouse pitch.");
                break;
            }
            case 4: // Gamepad
            {
                sectionHeader("GAMEPAD (XINPUT / DUALSENSE)");
                sliderRow("##vib", "Controller Vibration", s.pad_vibration, 0, 200, 10, "%", "Scales XInput / DualSense rumble intensity.");
                static const char *kPrmOpts[] = {"Auto", "PlayStation", "Xbox", "Keyboard"};
                static const int kPrmVals[] = {1, 2, 3, 4};
                cycleRow("##prm", "Button Prompts", s.button_prompts, kPrmOpts, kPrmVals, 4, "Choose which button icons are displayed in-game.");
                break;
            }
            case 5: // Enhancements
            {
                sectionHeader("PC PORT ENHANCEMENTS");
                toggleRow("##skip", "Skip Intro Videos", s.skip_intro, "Skips startup logo movies and boots directly to the title screen.");
                sliderRow("##fcs", "Free Camera Speed (F6)", s.free_cam_speed, 25, 400, 25, "%", "Movement speed multiplier when Free Camera is active.");
                break;
            }
            case 6: // Mods
            {
                sectionHeader("MOD LOADER (MODLOADER/)");
                toggleRow("##mods", "Enable Mod Loader", s.mods_enabled, "Loads custom .wad, .txd, .dff, .gxt, .dat and HD textures from modloader/.");
                toggleRow("##tex", "HD Texture Replacements", s.texture_replacements, "Enables .png / .dds texture overrides.");
                break;
            }
            }

            if (dirty)
            {
                lcs::settings::apply_live();
                lcs::settings::save("lcs.ini");
            }
            ImGui::EndChild();

            // Bottom Help Card
            const float helpY = wp.y + winH - helpH;
            dl->AddRectFilled(ImVec2(wp.x, helpY), ImVec2(wp.x + winW, wp.y + winH), u32(kHeaderBg));
            dl->AddLine(ImVec2(wp.x, helpY), ImVec2(wp.x + winW, helpY), u32(kTealLine, 0.35f), 1.0f);
            dl->AddRectFilled(ImVec2(wp.x, helpY), ImVec2(wp.x + 4.0f, wp.y + winH), u32(kPink));
            ImGui::PushFont(fontRegular());
            dl->AddText(ImVec2(wp.x + 18.0f, helpY + 18.0f), u32(kTealMuted), g_helpText.c_str());
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}
