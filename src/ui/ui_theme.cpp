#include "ui/ui_theme.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        ImFont *g_fontRegular = nullptr;
        ImFont *g_fontSemibold = nullptr;
        ImFont *g_fontCondensed = nullptr;
        ImFont *g_fontOswald = nullptr;

        std::string findFirstFont(const std::vector<std::filesystem::path> &dirs, std::initializer_list<const char *> names)
        {
            std::error_code ec;
            for (const char *name : names)
            {
                for (const std::filesystem::path &dir : dirs)
                {
                    const std::filesystem::path direct = dir / name;
                    if (std::filesystem::is_regular_file(direct, ec))
                        return direct.string();
                }
            }
            return {};
        }

        std::vector<std::filesystem::path> fontSearchDirs()
        {
            std::vector<std::filesystem::path> dirs;
            std::error_code ec;
            auto addIfDir = [&](const std::filesystem::path &p) {
                if (!p.empty() && std::filesystem::is_directory(p, ec))
                    dirs.push_back(p);
            };
            addIfDir("assets/fonts/oswald");
            addIfDir("assets/fonts");
#if defined(_WIN32)
            if (const char *localApp = std::getenv("LOCALAPPDATA"))
                addIfDir(std::filesystem::path(localApp) / "Microsoft" / "Windows" / "Fonts");
            if (const char *winDir = std::getenv("WINDIR"))
                addIfDir(std::filesystem::path(winDir) / "Fonts");
            else
                addIfDir("C:/Windows/Fonts");
#else
            addIfDir("/usr/local/share/fonts");
            addIfDir("/usr/share/fonts");
#endif
            return dirs;
        }
    } // namespace

    void initTheme()
    {
        if (!ImGui::GetCurrentContext())
            return;

        ImGuiIO &io = ImGui::GetIO();
        if (io.Fonts->Fonts.empty())
            io.Fonts->AddFontDefault();

        const std::vector<std::filesystem::path> dirs = fontSearchDirs();
#if defined(_WIN32)
        const std::string regPath = findFirstFont(
            dirs, {"Inter-Regular.ttf", "Inter_18pt-Regular.ttf", "segoeui.ttf", "arial.ttf"});
        const std::string semiPath = findFirstFont(
            dirs, {"Inter-SemiBold.ttf", "Inter_18pt-SemiBold.ttf", "seguisb.ttf", "segoeuib.ttf", "arialbd.ttf"});
        const std::string condPath = findFirstFont(
            dirs, {"Oswald-SemiBold.ttf", "bahnschrift.ttf", "arialnb.ttf", "segoeuib.ttf", "arialbd.ttf"});
        const std::string oswaldPath = findFirstFont(dirs, {"Oswald-SemiBold.ttf"});
        const std::string cjkPath = findFirstFont(dirs, {"meiryo.ttc", "YuGothM.ttc", "YuGothR.ttc", "msgothic.ttc"});
#else
        const std::string regPath = findFirstFont(
            dirs, {"Inter-Regular.ttf", "DejaVuSans.ttf", "LiberationSans-Regular.ttf"});
        const std::string semiPath = findFirstFont(
            dirs, {"Inter-SemiBold.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"});
        const std::string condPath = findFirstFont(
            dirs, {"Oswald-SemiBold.ttf", "LiberationSansNarrow-Bold.ttf", "DejaVuSans-Bold.ttf"});
        const std::string oswaldPath = findFirstFont(dirs, {"Oswald-SemiBold.ttf"});
        const std::string cjkPath = findFirstFont(dirs, {"NotoSansCJKjp-Regular.otf", "NotoSansJP-Regular.ttf"});
#endif

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH = true;

        auto mergeCjkIfAny = [&](float px) {
            if (cjkPath.empty()) return;
            ImFontConfig mcfg;
            mcfg.MergeMode = true;
            mcfg.OversampleH = 1;
            mcfg.OversampleV = 1;
            io.Fonts->AddFontFromFileTTF(cjkPath.c_str(), px, &mcfg);
        };

        // Bake high-res base fonts; PushFont(font, px) scales dynamically with zero blur
        if (!regPath.empty()) {
            g_fontRegular = io.Fonts->AddFontFromFileTTF(regPath.c_str(), 26.0f, &cfg);
            mergeCjkIfAny(24.0f);
        }
        if (!semiPath.empty()) {
            g_fontSemibold = io.Fonts->AddFontFromFileTTF(semiPath.c_str(), 26.0f, &cfg);
            mergeCjkIfAny(24.0f);
        }
        if (!condPath.empty()) {
            g_fontCondensed = io.Fonts->AddFontFromFileTTF(condPath.c_str(), 30.0f, &cfg);
            mergeCjkIfAny(28.0f);
        }
        if (!oswaldPath.empty()) {
            g_fontOswald = io.Fonts->AddFontFromFileTTF(oswaldPath.c_str(), 42.0f, &cfg);
            mergeCjkIfAny(40.0f);
        }

        if (!g_fontSemibold) g_fontSemibold = g_fontRegular;
        if (!g_fontCondensed) g_fontCondensed = g_fontSemibold;
        if (!g_fontOswald) g_fontOswald = g_fontCondensed;
    }

    float uiScale()
    {
        const ImGuiIO &io = ImGui::GetIO();
        if (io.DisplaySize.y <= 0.0f)
            return 1.0f;
        return std::clamp(io.DisplaySize.y / 1080.0f, 0.68f, 2.25f);
    }

    ImFont *fontRegular() { return g_fontRegular ? g_fontRegular : ImGui::GetFont(); }
    ImFont *fontSemibold() { return g_fontSemibold ? g_fontSemibold : fontRegular(); }
    ImFont *fontCondensed() { return g_fontCondensed ? g_fontCondensed : fontSemibold(); }
    ImFont *fontOswald() { return g_fontOswald ? g_fontOswald : fontCondensed(); }

    void drawChevron(ImDrawList *dl, ImVec2 c, bool rightDir, float s, ImU32 col)
    {
        const float dx = 4.2f * s * (rightDir ? 1.0f : -1.0f);
        const float dy = 6.8f * s;
        const float thick = std::max(1.6f, 2.2f * s);
        const ImVec2 pts[3] = {{c.x - dx, c.y - dy}, {c.x + dx, c.y}, {c.x - dx, c.y + dy}};
        dl->AddPolyline(pts, 3, col, 0, thick);
    }
}
