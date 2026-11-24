// Shared visual theme for the GTA 6-style Global Menu adapted to GTA: Liberty City Stories
// (Crimson Red #E5251E, Deep Obsidian Black #0B0B0E, Dark Graphite, Pure White #F5F5F7).
#pragma once

#include "imgui.h"

#include <cstdint>
#include <string>

namespace ui
{
    namespace palette
    {
        // GTA 6 Layout x Liberty City Stories Color Palette
        constexpr uint32_t kBgTopLeft = 0x0B0B0Eu;   // Charcoal / obsidian top-left backdrop
        constexpr uint32_t kBgMid = 0x160A0Cu;       // Deep graphite-wine center
        constexpr uint32_t kBgBotRight = 0x3B0A0Au;  // Deep nocturnal crimson bottom-right
        constexpr uint32_t kTopBarBg = 0x0A0A0Du;    // Matte black global top bar
        constexpr uint32_t kPanelGlass = 0x0A0A0Eu;  // Left hub panel glassmorphism
        constexpr uint32_t kSilhouette = 0x080303u;  // Liberty City skyline silhouette

        constexpr uint32_t kSelectBar = 0xF5F5F7u;   // GTA 6 high-contrast pure white selection bar
        constexpr uint32_t kSelectText = 0x0A0A0Cu;  // Deep black text on selection bar
        constexpr uint32_t kCrimson = 0xE5251Eu;     // Primary LCS Crimson accent
        constexpr uint32_t kCrimsonHi = 0xFF3B30u;   // Hover / bright LCS Crimson
        constexpr uint32_t kCrimsonDark = 0x9E110Cu; // Border / deep crimson

        constexpr uint32_t kWhite = 0xFFFFFFu;       // Pure white text
        constexpr uint32_t kDim = 0xC8C8D0u;         // Secondary gray text
        constexpr uint32_t kMuted = 0x8E8E9Au;       // Muted hint / counter text
        constexpr uint32_t kHairline = 0xFFFFFFu;    // Hairline divider (use with alpha 0.14..0.22)

        constexpr uint32_t kPillOff = 0x2A2A32u;     // Inactive toggle track
        constexpr uint32_t kStateOn = 0x2ECC71u;     // Clean green ON badge
        constexpr uint32_t kAmber = 0xF39C12u;

        // Legacy aliases for shared helpers (FPS counter, notices)
        constexpr uint32_t kHeaderBg = kTopBarBg;
        constexpr uint32_t kPink = kCrimson;
        constexpr uint32_t kTealDark = kWhite;

        inline ImVec4 rgba(uint32_t hex, float a = 1.0f)
        {
            return ImVec4(((hex >> 16) & 0xFFu) / 255.0f, ((hex >> 8) & 0xFFu) / 255.0f, (hex & 0xFFu) / 255.0f, a);
        }

        inline ImU32 u32(uint32_t hex, float a = 1.0f)
        {
            const int alpha = a <= 0.0f ? 0 : (a >= 1.0f ? 255 : static_cast<int>(a * 255.0f + 0.5f));
            return IM_COL32((hex >> 16) & 0xFFu, (hex >> 8) & 0xFFu, hex & 0xFFu, alpha);
        }
    }

    void initTheme();
    float uiScale();
    ImFont *fontRegular();
    ImFont *fontSemibold();
    ImFont *fontCondensed();
    ImFont *fontOswald();
    // Pricedown Bl.otf from fonts/ when present (nullptr otherwise): HD stand-in for the game's
    // Pricedown bitmap font.
    ImFont *fontPricedown();
    void drawChevron(ImDrawList *dl, ImVec2 center, bool rightDir, float scale, ImU32 col);
}
