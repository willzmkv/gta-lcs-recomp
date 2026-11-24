// The look: hot pink for the thing to do, on a deep black (dark) or on white and blush (light); teal for done, amber
// and red for attention, and nothing else. Which of the two follows Windows' app mode unless the player picks one
// (Settings). Every colour the UI draws comes from here; sizes are logical units.
#pragma once

#include "core/math.h"

namespace theme
{
    using core::Color;
    using core::rgb;

    struct Palette
    {
        // surfaces, back to front
        Color night;      // behind everything
        Color base;       // sheets, drawers
        Color surface;    // cards
        Color surfaceHi;  // hovered cards, controls
        Color surfaceTop; // what sits on controls
        // text
        Color text, dim, faint;
        // signal colours
        Color pink;   // the primary: the thing to do, the selection
        Color pinkHi; // its gradients' lighter end
        Color onPink; // text and marks on pink
        Color teal, amber, red;
        // what is laid over surfaces at an alpha: hover tints, hairlines, dividers (its own alpha scales them)
        Color ink;
        Color shadow; // its alpha scales every shadow
        Color scrim;  // behind dialogs and sheets
        // particular fills
        Color errorFill, doneFill;                     // a failure's card, a finished task's card
        Color dangerFill, dangerFillHi, dangerText;    // the danger button
        Color logFill;                                 // the log's well
        Color well, wellBottom, wellGlow, wellGlowBottom; // the disc drop zone, idle and with a file over it
        Color sky, sea;                                // the backdrop's top and its sea
        Color horizon;                                 // the sky's glow at the horizon
        Color skyline, cloud, lights;                  // silhouettes (towers), cloud streaks, lit windows
        Color contrail;                                // jet trails, lit by the low sun
    };

inline constexpr Palette kDarkPalette{
    .night = rgb(0x070708),
    .base = rgb(0x0D0D10),
    .surface = rgb(0x141418),
    .surfaceHi = rgb(0x1E1E24),
    .surfaceTop = rgb(0x282830),
    .text = rgb(0xFFFFFF),
    .dim = rgb(0xB5B5BC),
    .faint = rgb(0x6E6E77),
    .pink = rgb(0xD91E18),
    .pinkHi = rgb(0xFF2E24),
    .onPink = rgb(0xFFFFFF),
    .teal = rgb(0x2ECC71),
    .amber = rgb(0xF39C12),
    .red = rgb(0xE74C3C),
    .ink = rgb(0xFFFFFF),
    .shadow = rgb(0x000000),
    .scrim = rgb(0x070708, 180),
    .errorFill = rgb(0x2A0F0F),
    .doneFill = rgb(0x0F2118),
    .dangerFill = rgb(0x3A1212),
    .dangerFillHi = rgb(0x4A1717),
    .dangerText = rgb(0xFFC9C9),
    .logFill = rgb(0x0A0A0C),
    .well = rgb(0x131316),
    .wellBottom = rgb(0x0E0E11),
    .wellGlow = rgb(0x2B0B0B),
    .wellGlowBottom = rgb(0x1F0808),
    .sky = rgb(0x0B0B0D),
    .sea = rgb(0x08080A),
    .horizon = rgb(0x3A0A0A),
    .skyline = rgb(0x060607),
    .cloud = rgb(0x141418),
    .lights = rgb(0xFFB8B0),
    .contrail = rgb(0xFFC9C2),
};

    inline constexpr Palette kLightPalette{
        .night = rgb(0xFFF5F4),
        .base = rgb(0xFFFFFF),
        .surface = rgb(0xFFFFFF),
        .surfaceHi = rgb(0xFFF4F3),
        .surfaceTop = rgb(0xFFE9E6),
        .text = rgb(0x1A1A1E),
        .dim = rgb(0x5E5E66),
        .faint = rgb(0x94949C),
        .pink = rgb(0xC41914),
        .pinkHi = rgb(0xE5251E),
        .onPink = rgb(0xFFFFFF),
        .teal = rgb(0x1E9E55),
        .amber = rgb(0xB36E00),
        .red = rgb(0xC92020),
        .ink = rgb(0x240B0B, 200),
        .shadow = rgb(0x7A1512, 85),
        .scrim = rgb(0xFFF6F5, 160), // light: the content behind washes out
        .errorFill = rgb(0xFFF0F0),
        .doneFill = rgb(0xEDF8F0),
        .dangerFill = rgb(0xFFE4E2),
        .dangerFillHi = rgb(0xFFD3D0),
        .dangerText = rgb(0xB02018),
        .logFill = rgb(0xFFFAF9),
        .well = rgb(0xFFFFFF),
        .wellBottom = rgb(0xFFF8F7),
        .wellGlow = rgb(0xFFE8E4),
        .wellGlowBottom = rgb(0xFFDAD5),
        .sky = rgb(0xFFE2DE),
        .sea = rgb(0xFFEEEC),
        .horizon = rgb(0xFFC9C4),
        .skyline = rgb(0xC9554E),
        .cloud = rgb(0xFFFAF9),
        .lights = rgb(0xFFF1EF),
        .contrail = rgb(0xFFFFFF),
    };

    // the palette in use: between the two while it changes (apply)
    inline Palette current = kDarkPalette;
    inline float darkness = 1; // 1 dark, 0 light

    // every colour, blended: 0 is light, 1 is dark
    inline void apply(float dark)
    {
        static_assert(sizeof(Palette) % sizeof(Color) == 0);
        constexpr int n = int(sizeof(Palette) / sizeof(Color));
        const Color *a = reinterpret_cast<const Color *>(&kLightPalette);
        const Color *b = reinterpret_cast<const Color *>(&kDarkPalette);
        Color *out = reinterpret_cast<Color *>(&current);
        for (int i = 0; i < n; ++i)
            out[i] = core::mix(a[i], b[i], dark);
        darkness = dark;
    }

    // the names the UI draws with
    inline Color &kNight = current.night;
    inline Color &kBase = current.base;
    inline Color &kSurface = current.surface;
    inline Color &kSurfaceHi = current.surfaceHi;
    inline Color &kSurfaceTop = current.surfaceTop;
    inline Color &kText = current.text;
    inline Color &kDim = current.dim;
    inline Color &kFaint = current.faint;
    inline Color &kPink = current.pink;
    inline Color &kPinkHi = current.pinkHi;
    inline Color &kOnPink = current.onPink;
    inline Color &kTeal = current.teal;
    inline Color &kAmber = current.amber;
    inline Color &kRed = current.red;
    inline Color &kScrim = current.scrim;
    inline Color &kErrorFill = current.errorFill;
    inline Color &kDoneFill = current.doneFill;
    inline Color &kDangerFill = current.dangerFill;
    inline Color &kDangerFillHi = current.dangerFillHi;
    inline Color &kDangerText = current.dangerText;
    inline Color &kLogFill = current.logFill;
    inline Color &kWell = current.well;
    inline Color &kWellBottom = current.wellBottom;
    inline Color &kWellGlow = current.wellGlow;
    inline Color &kWellGlowBottom = current.wellGlowBottom;
    inline Color &kSky = current.sky;
    inline Color &kSea = current.sea;
    inline Color &kHorizon = current.horizon;
    inline Color &kSkyline = current.skyline;
    inline Color &kCloud = current.cloud;
    inline Color &kLights = current.lights;
    inline Color &kContrail = current.contrail;

    // a tint over a surface (white on dark, a deep pink on light)
    inline Color ink(float a) { return core::withAlpha(current.ink, a * core::alphaOf(current.ink)); }
    // a shadow's colour at strength a (light mode's are softer and pinker)
    inline Color shade(float a) { return core::withAlpha(current.shadow, a * core::alphaOf(current.shadow)); }

    // type sizes
    constexpr float kWordmark = 46;
    constexpr float kTitle = 30;
    constexpr float kHeading = 19;
    constexpr float kBody = 14.5f;
    constexpr float kSmall = 13;
    constexpr float kEyebrow = 11.5f;

    // shape
    constexpr float kRadius = 12;      // cards
    constexpr float kRadiusSmall = 8;  // buttons, controls
    constexpr float kTitleBar = 44;

    // the icons render/icons.cpp draws (their Segoe Fluent Icons code points, kept as identifiers)
    namespace icon
    {
        constexpr char32_t kPlay = 0xE768;
        constexpr char32_t kSettings = 0xE713;
        constexpr char32_t kHelp = 0xE897;
        constexpr char32_t kSave = 0xE74E;
        constexpr char32_t kFolder = 0xED25;   // an open folder
        constexpr char32_t kOpenFile = 0xE8E5; // a page with an arrow: choosing a file
        constexpr char32_t kDisc = 0xE958;
        constexpr char32_t kDownload = 0xE896;
        constexpr char32_t kRefresh = 0xE72C;
        constexpr char32_t kMinimize = 0xE921;
        constexpr char32_t kMaximize = 0xE922;
        constexpr char32_t kRestore = 0xE923;
        constexpr char32_t kClose = 0xE8BB;
        constexpr char32_t kCheck = 0xE73E;
        constexpr char32_t kCancel = 0xE711;
        constexpr char32_t kWarning = 0xE7BA;
        constexpr char32_t kInfo = 0xE946;
        constexpr char32_t kBack = 0xE72B;
        constexpr char32_t kChevronRight = 0xE76C;
        constexpr char32_t kChevronDown = 0xE70D;
        constexpr char32_t kDocument = 0xE8A5;
        constexpr char32_t kRepair = 0xE90F;
        constexpr char32_t kCode = 0xE943;
        constexpr char32_t kChip = 0xE950;
        constexpr char32_t kClock = 0xE823;
        constexpr char32_t kSync = 0xE895;
        constexpr char32_t kLink = 0xE71B;
        constexpr char32_t kUndo = 0xE7A7;
        constexpr char32_t kBug = 0xEBE8;
        constexpr char32_t kDrive = 0xEDA2;
        constexpr char32_t kOpen = 0xE8A7; // open in new window
        constexpr char32_t kList = 0xEA37;
        constexpr char32_t kGame = 0xE7FC;
        constexpr char32_t kStop = 0xE71A;
        constexpr char32_t kPlayFilled = 0xE769;
        constexpr char32_t kCrossedTools = 0xE910;
        constexpr char32_t kSteam = 0xE911;
        constexpr char32_t kMod = 0xEA86;
    }
}
