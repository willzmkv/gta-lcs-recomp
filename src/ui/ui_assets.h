// Real game & launcher texture assets uploaded to D3D12 SRV heap for the Global Menu
// Uses authentic game/launcher PNGs (liberty_city_hd_map, menu_sub_1..4, menu_LCSLogo,
// radar sprites from touch_fe/hud, controller PNGs, and Kenney 1.5 input prompt PNGs).
#pragma once

#include "imgui.h"

namespace ui::assets
{
    struct RealTexture
    {
        ImTextureID id = 0;
        int w = 0;
        int h = 0;
        bool valid() const { return id != 0 && w > 0 && h > 0; }
    };

    const RealTexture &mapTexture();              // Real 2000x2000 HD Liberty City map
    const RealTexture &characterTexture(int idx); // 0..3 -> menu_sub_1..4.png
    const RealTexture &controllerTexture(int idx);// 0=xbox, 1=ps5, 2=ps2, 3=switch, 4=kb_mouse
    const RealTexture &logoTexture();             // menu_LCSLogo.png
    const RealTexture &mainMenuBgTexture();       // GTA IV-style LCS main menu background comic grid
    const RealTexture &mainMenuToniTexture();     // Toni Cipriani AK-47 transparent foreground art
    const RealTexture &mainMenuLogoTexture();     // GTA Liberty City Stories full title logo PNG
    const RealTexture &radarSprite(int spriteId); // Real 1-indexed CRadar::RadarSprites PNGs (touch_fe + hud)
    // 0=Xbox, 1=PS5, 2=PS2, 3=Keyboard & Mouse, 4=Nintendo Switch -> Kenney PNG for token
    const RealTexture &inputPromptTexture(int platformPreset, const char *token);
}
