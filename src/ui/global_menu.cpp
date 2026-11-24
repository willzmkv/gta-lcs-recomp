#include "ui/global_menu.h"

#include "core/config.h"
#include "game/lcs_prompts.h"
#include "game/lcs_trainer.h"
#include "graphics/render_bridge.h"
#include "graphics/window.h"
#include "input/input_system.h"
#include "input/mouse_aim.h"
#include "input/pc_controls.h"
#include "settings/settings.h"
#include "ui/ui_assets.h"
#include "ui/ui_loc.h"
#include "ui/ui_theme.h"
#include "vfs/logical_fs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        bool g_open = false;
        bool g_justOpened = false;
        GlobalTab g_tab = GlobalTab::Jogo;

        // Selection indices per screen
        int g_hubSel = 0;
        int g_jogoSubView = 0;      // 0=Hub, 1=Salvar Jogo, 2=Carregar Jogo, 3=Confirmar Novo Jogo
        int g_jogoSlotSel = 0;      // 0..8 (Slots 1..8 + Slot 9 AutoSave)
        int g_jogoConfirmSlot = -1; // >=0 when confirming overwrite or load of a slot
        int g_jogoConfirmBtn = 0;   // 0=Confirmar, 1=Cancelar
        int g_mmSel = 0;            // Main Menu bottom-bar index
        int g_mmSubView = 0;        // 0=Main bar, 1=Load Slot picker, 2=Confirm New Game, 3=Confirm Quit
        int g_mmSlotSel = 0;
        int g_mmConfirmSlot = -1;
        int g_mmConfirmBtn = 0;     // 0=Confirmar, 1=Cancelar
        float g_mmAnimTime = 0.0f;
        int g_briefFilter = 0; // 0=Tudo, 1=Dialogo, 2=Ajuda, 3=Objetivo
        int g_cfgSubTab = 0;   // 0=Esquema, 1=Controle, 2=Jogabilidade, 3=Tela, 4=Graficos, 5=Interface, 6=Camera, 7=Audio
        int g_cfgRow = 0;
        int g_cfgLastRows = 0;
        int g_schemePreset = 4;  // 0=Xbox, 1=PS5, 2=PS2, 3=Switch, 4=Teclado & Mouse
        int g_schemePreview = 0; // 0=A pe, 1=Mira, 2=Corpo a corpo, 3=Veiculos
        bool g_mapShowLegend = true;
        int g_modsRow = 0;
        int g_trainerCat = 0; // 0=Jogador, 1=Armas, 2=Veiculos, 3=Teleporte, 4=Camera, 5=Mundo
        int g_trainerRow = 0;
        int g_trainerLastRows = 0;

        // Trainer selector states
        int g_wantedLevel = 0;
        int g_moneyIdx = 1;
        int g_weaponIdx = 0;
        int g_vehicleIdx = 0;
        int g_weatherIdx = 0;
        int g_timeHour = 12;

        // Interactive Liberty City Map state
        float g_mapPanX = 0.0f;
        float g_mapPanY = 0.0f;
        float g_mapZoom = 1.0f;
        bool g_mapInit = false;

        // Smooth UI transition & gliding selection bar animation state
        float g_uiDt = 1.0f / 60.0f;
        float g_menuGlowTime = 0.0f;
        float g_tabFade = 1.0f;
        float g_tabSlide = 0.0f;
        float g_subViewFade = 1.0f;
        float g_hubBarY = -1.0f;
        float g_cfgBarY = -1.0f;
        float g_trainerBarY = -1.0f;
        float g_modsBarY = -1.0f;
        float g_slotBarY = -1.0f;
        float g_briefBarY = -1.0f;
        float g_topTabX = -1.0f;
        float g_topTabW = 0.0f;
        float g_mmBarX = -1.0f;
        float g_mmBarW = 0.0f;
        bool g_justClosedToMain = false;
        double g_lastSliderSfxTime = -10.0;

        inline void playMenuSfx(lcs::game::MenuSfx sfx)
        {
            lcs::game::LCSTrainer::instance().play_menu_sfx(sfx);
        }

        inline void triggerViewTransition(float slideDir = 1.0f)
        {
            g_tabFade = 0.0f;
            g_tabSlide = slideDir * 16.0f;
            g_hubBarY = -1.0f;
            g_cfgBarY = -1.0f;
            g_trainerBarY = -1.0f;
            g_modsBarY = -1.0f;
            g_slotBarY = -1.0f;
            g_briefBarY = -1.0f;
        }

        inline float smoothStepFactor(float speed)
        {
            return std::clamp(1.0f - std::exp(-speed * std::clamp(g_uiDt, 0.001f, 0.05f)), 0.04f, 1.0f);
        }

        inline float glideFloat(float &cur, float target, float speed = 22.0f)
        {
            if (cur < -0.5f)
                cur = target;
            else
                cur += (target - cur) * smoothStepFactor(speed);
            return cur;
        }

        // Layout unit helpers (1920x1080 reference space + user font accessibility scale)
        struct Layout
        {
            float sw = 1920.0f;
            float sh = 1080.0f;
            float unit = 1.0f;
            float ox = 0.0f;
            float contentW = 1920.0f;
            float fontScale = 1.0f;

            float u(float v) const { return v * unit; }
            float ux(float v) const { return ox + v * unit; }
            float uf(float v) const { return v * unit * fontScale; }
            float rf(float v) const { return v * unit * (1.0f + std::max(0.0f, fontScale - 1.0f) * 0.72f); }
        };

        Layout computeLayout(const ImGuiIO &io)
        {
            Layout L;
            L.sw = std::max(320.0f, io.DisplaySize.x);
            L.sh = std::max(240.0f, io.DisplaySize.y);
            // Reference 1920x1080; on ultrawide (>16:9), keep comfortable centered 16:9 stage while backdrop fills 100%
            L.unit = std::min(L.sw / 1920.0f, L.sh / 1080.0f);
            L.contentW = 1920.0f * L.unit;
            L.ox = (L.sw - L.contentW) * 0.5f;
            L.fontScale = std::clamp(static_cast<float>(lcs::settings::get().ui_font_scale) / 100.0f, 0.85f, 1.45f);
            return L;
        }

        // ---- Shared GTA 6 x LCS visual primitives ------------------------------------------------------------------

        // REAL official GTA: Liberty City Stories character PNG (menu_sub_1..4.png) flush to the LEFT edge.
        // Note: R* stored ~3:4 portraits horizontally stretched in 512x512 POT textures; kPortraitAspectFix
        // restores the true un-squashed proportion and scales tall to ~90% screen height.
        void drawRealCharacterArt(ImDrawList *dl, const Layout &L, int charIdx,
                                  float heightFrac = 0.89f, float alphaFrac = 0.72f, float offsetX = 0.0f)
        {
            const auto &ch = ui::assets::characterTexture(charIdx);
            if (!ch.valid())
                return;
            const float aspectFix = (ch.w == ch.h) ? 0.74f : 1.0f;
            const float topSafe = L.u(70.0f);
            const float artH = std::min(L.sh - topSafe, L.sh * heightFrac);
            const float artW = artH * (static_cast<float>(ch.w) / static_cast<float>(std::max(1, ch.h))) * aspectFix;
            const ImVec2 p0(offsetX, L.sh - artH);
            const ImVec2 p1(offsetX + artW, L.sh);
            const int a = static_cast<int>(std::clamp(alphaFrac, 0.0f, 1.0f) * 255.0f);
            dl->AddImage(ch.id, p0, p1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
        }

        // GTA 6 diagonal gradient backdrop adapted to LCS Charcoal -> Nocturnal Crimson + Real LCS Character PNG
        void drawBackdrop(ImDrawList *dl, const Layout &L, int charIdx = -1)
        {
            using namespace palette;
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(L.sw, L.sh),
                                        u32(kBgTopLeft, 1.0f), u32(kBgMid, 1.0f),
                                        u32(kBgBotRight, 1.0f), u32(kBgTopLeft, 1.0f));
            if (charIdx >= 0)
                drawRealCharacterArt(dl, L, charIdx, 0.89f, 0.72f, 0.0f);
        }

        struct PromptPart
        {
            const ui::assets::RealTexture *tex = nullptr;
            std::string sep;
            float w = 0.0f;
            float h = 0.0f;
        };

        bool buildPromptCluster(const Layout &L, int platPreset, const char *keyTxt,
                                std::vector<PromptPart> &outParts, float &outTotalW, float &outMaxH)
        {
            outParts.clear();
            outTotalW = 0.0f;
            outMaxH = 0.0f;
            if (!keyTxt || !keyTxt[0])
                return false;

            auto pushIcon = [&](const char *tok, float iconH) -> bool {
                const auto &tex = ui::assets::inputPromptTexture(platPreset, tok);
                if (!tex.valid())
                    return false;
                const float iconW = iconH * (static_cast<float>(tex.w) / static_cast<float>(std::max(1, tex.h)));
                outParts.push_back({&tex, {}, iconW, iconH});
                outTotalW += iconW;
                outMaxH = std::max(outMaxH, iconH);
                return true;
            };

            const std::string s(keyTxt);
            if (platPreset == 3 && s == "WASD")
            {
                static constexpr const char *kWasd[] = {"W", "A", "S", "D"};
                const float iconH = L.u(25.0f);
                const float gap = L.u(2.0f);
                for (int i = 0; i < 4; ++i)
                {
                    if (!pushIcon(kWasd[i], iconH))
                    {
                        outParts.clear();
                        return false;
                    }
                    if (i < 3)
                    {
                        outParts.push_back({nullptr, "", gap, iconH});
                        outTotalW += gap;
                    }
                }
                return true;
            }

            const size_t slashPos = s.find(" / ");
            if (slashPos != std::string::npos)
            {
                const std::string leftTok = s.substr(0, slashPos);
                const std::string rightTok = s.substr(slashPos + 3);
                const float iconH = L.u(28.0f);
                if (!pushIcon(leftTok.c_str(), iconH))
                {
                    outParts.clear();
                    return false;
                }
                ImGui::PushFont(fontSemibold(), L.u(15.0f));
                const ImVec2 slashSz = ImGui::CalcTextSize(" / ");
                ImGui::PopFont();
                outParts.push_back({nullptr, " / ", slashSz.x, slashSz.y});
                outTotalW += slashSz.x;
                outMaxH = std::max(outMaxH, slashSz.y);
                if (!pushIcon(rightTok.c_str(), iconH))
                {
                    outParts.clear();
                    return false;
                }
                return true;
            }

            if (pushIcon(s.c_str(), L.u(28.0f)))
                return true;
            outParts.clear();
            return false;
        }

        // Renders Kenney PNG prompt icons (with crisp pill fallback) and returns total width consumed.
        float drawPromptCluster(ImDrawList *dl, const Layout &L, int platPreset, const char *keyTxt,
                                float anchorX, float cy, bool rightAlign)
        {
            using namespace palette;
            std::vector<PromptPart> parts;
            float totalW = 0.0f;
            float maxH = 0.0f;
            if (buildPromptCluster(L, platPreset, keyTxt, parts, totalW, maxH))
            {
                float curX = rightAlign ? (anchorX - totalW) : anchorX;
                for (const auto &p : parts)
                {
                    if (p.tex)
                    {
                        dl->AddImage(p.tex->id, ImVec2(curX, cy - p.h * 0.5f), ImVec2(curX + p.w, cy + p.h * 0.5f));
                    }
                    else if (!p.sep.empty())
                    {
                        ImGui::PushFont(fontSemibold(), L.u(15.0f));
                        dl->AddText(ImVec2(curX, cy - p.h * 0.5f), u32(kDim), p.sep.c_str());
                        ImGui::PopFont();
                    }
                    curX += p.w;
                }
                return totalW;
            }

            ImGui::PushFont(fontSemibold(), L.u(14.5f));
            const ImVec2 ks = ImGui::CalcTextSize(keyTxt ? keyTxt : "");
            const float bw = std::max(L.u(28.0f), ks.x + L.u(14.0f));
            const float bh = L.u(24.0f);
            const float bx0 = rightAlign ? (anchorX - bw) : anchorX;
            dl->AddRectFilled(ImVec2(bx0, cy - bh * 0.5f), ImVec2(bx0 + bw, cy + bh * 0.5f), u32(kSelectBar), bh * 0.5f);
            dl->AddText(ImVec2(bx0 + (bw - ks.x) * 0.5f, cy - ks.y * 0.5f), u32(kSelectText), keyTxt ? keyTxt : "");
            ImGui::PopFont();
            return bw;
        }

        // Bidirectional mapping between 5-scheme UI selector (0=Xbox, 1=PS5, 2=PS2, 3=Switch, 4=KB+Mouse)
        // and inputPromptTexture presets (0=Xbox, 1=PS5, 2=PS2, 3=KB+Mouse, 4=Switch) + live LCSPrompts device.
        int schemeIndexToPromptPreset(int schemeIdx)
        {
            switch (schemeIdx)
            {
            case 0: return 0; // Xbox
            case 1: return 1; // PlayStation 5
            case 2: return 2; // PlayStation 2
            case 3: return 4; // Nintendo Switch
            case 4: return 3; // Keyboard & Mouse
            default: return 3;
            }
        }

        int promptDeviceToSchemeIndex(lcs::game::PromptDevice dev, int currentScheme)
        {
            using PD = lcs::game::PromptDevice;
            switch (dev)
            {
            case PD::Xbox: return 0;
            case PD::PlayStation: return (currentScheme == 2) ? 2 : 1;
            case PD::Switch: return 3;
            case PD::KeyboardMouse:
            default: return 4;
            }
        }

        lcs::game::PromptDevice schemeIndexToPromptDevice(int schemeIdx)
        {
            using PD = lcs::game::PromptDevice;
            switch (schemeIdx)
            {
            case 0: return PD::Xbox;
            case 1:
            case 2: return PD::PlayStation;
            case 3: return PD::Switch;
            case 4:
            default: return PD::KeyboardMouse;
            }
        }

        lcs::game::PromptDevice g_lastSyncedPromptDev = lcs::game::PromptDevice::KeyboardMouse;
        bool g_promptDevSyncedOnce = false;

        void syncSchemeWithActiveDevice(bool allowKbReset = true)
        {
            const auto dev = lcs::game::LCSPrompts::instance().active_device();
            if (!g_promptDevSyncedOnce || dev != g_lastSyncedPromptDev)
            {
                if (!allowKbReset && dev == lcs::game::PromptDevice::KeyboardMouse && g_promptDevSyncedOnce)
                {
                    // Do not snap the preview diagram back to KB/Mouse while the user is actively cycling presets here.
                    g_lastSyncedPromptDev = dev;
                    return;
                }
                g_promptDevSyncedOnce = true;
                g_lastSyncedPromptDev = dev;
                g_schemePreset = promptDeviceToSchemeIndex(dev, g_schemePreset);
                lcs::input::InputSystem::instance().set_classic_ps2_triggers(g_schemePreset == 2);
            }
        }

        void applyManualSchemePreset(int newScheme)
        {
            g_schemePreset = ((newScheme % 5) + 5) % 5;
            const auto newDev = schemeIndexToPromptDevice(g_schemePreset);
            lcs::game::LCSPrompts::instance().set_active_device(newDev);
            g_lastSyncedPromptDev = newDev;
            g_promptDevSyncedOnce = true;
            lcs::input::InputSystem::instance().set_classic_ps2_triggers(g_schemePreset == 2);
        }

        inline bool isControllerPromptPreset(int platPreset) { return platPreset != 3; }

        int activePromptPreset()
        {
            const int manual = lcs::settings::get().button_prompts;
            if (manual == 2) return 1; // PlayStation
            if (manual == 3) return 0; // Xbox
            if (manual == 4) return 3; // Teclado & Mouse
            const bool browsingScheme = (g_tab == GlobalTab::Configuracoes && g_cfgSubTab == 0 && g_open);
            syncSchemeWithActiveDevice(!browsingScheme);
            return schemeIndexToPromptPreset(g_schemePreset);
        }

        const char *deviceTokenForAction(int platPreset, const char *kbToken)
        {
            if (!kbToken)
                return "";
            const std::string s(kbToken);
            if (platPreset == 0) // Xbox
            {
                if (s == "Q") return "LB";
                if (s == "E") return "RB";
                if (s == "ENTER") return "A";
                if (s == "ESC") return "B";
                if (s == "TAB") return "Y";
                if (s == "X") return "X";
                if (s == "R") return "R3";
                if (s == "SCROLL") return "LT / RT";
                if (s == "SUB_BAR") return "LT / RT";
                if (s == "SUB_PREV") return "LT";
                if (s == "SUB_NEXT") return "RT";
                if (s == "MOUSE") return "LS";
            }
            else if (platPreset == 1 || platPreset == 2) // PlayStation 5 / PlayStation 2
            {
                if (s == "Q") return "L1";
                if (s == "E") return "R1";
                if (s == "ENTER") return "X";
                if (s == "ESC") return "CIRCLE";
                if (s == "TAB") return "TRIANGLE";
                if (s == "X") return "SQUARE";
                if (s == "R") return "R3";
                if (s == "SCROLL") return "L2 / R2";
                if (s == "SUB_BAR") return "L2 / R2";
                if (s == "SUB_PREV") return "L2";
                if (s == "SUB_NEXT") return "R2";
                if (s == "MOUSE") return "L";
            }
            else if (platPreset == 4) // Nintendo Switch
            {
                if (s == "Q") return "L";
                if (s == "E") return "R";
                if (s == "ENTER") return "A";
                if (s == "ESC") return "B";
                if (s == "TAB") return "X";
                if (s == "X") return "Y";
                if (s == "R") return "R3";
                if (s == "SCROLL") return "ZL / ZR";
                if (s == "SUB_BAR") return "ZL / ZR";
                if (s == "SUB_PREV") return "ZL";
                if (s == "SUB_NEXT") return "ZR";
                if (s == "MOUSE") return "LS";
            }
            if (s == "SUB_BAR") return "TAB";
            return kbToken;
        }

        // Round button prompt badge / Kenney icon that automatically tracks the live active device
        float drawFooterPrompt(ImDrawList *dl, const Layout &L, float rightX, float cy, const char *label, const char *keyTxt)
        {
            using namespace palette;
            label = ui::loc::tx(label);
            ImGui::PushFont(fontCondensed(), L.uf(19.0f));
            const ImVec2 ls = ImGui::CalcTextSize(label);
            ImGui::PopFont();

            const int plat = activePromptPreset();
            const char *tok = deviceTokenForAction(plat, keyTxt);
            const float badgeW = drawPromptCluster(dl, L, plat, tok, rightX, cy, true);
            const float bx0 = rightX - badgeW;

            const float lx = bx0 - L.u(10.0f) - ls.x;
            ImGui::PushFont(fontCondensed(), L.uf(19.0f));
            dl->AddText(ImVec2(lx, cy - ls.y * 0.5f), u32(kWhite), label);
            ImGui::PopFont();

            return lx - L.u(26.0f);
        }

        // Global top bar: [Q/L1/LB] JOGO | MAPA | RESUMO | PROGRESSO | CONFIGURACOES | MODS | TRAINER [E/R1/RB]
        // When opened from the Startup Main Menu (!in_gameplay), restricts top tabs to CONFIGURACOES | MODS.
        void drawGlobalTopBar(ImDrawList *dl, const Layout &L, bool inGameplay)
        {
            using namespace palette;
            const float barH = L.rf(62.0f);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(L.sw, barH), u32(kTopBarBg, 0.96f));
            dl->AddLine(ImVec2(0, barH), ImVec2(L.sw, barH), u32(kHairline, 0.14f), 1.0f);

            struct TopTabEntry { GlobalTab tab; const char *name; };
            const TopTabEntry kAllTabs[] = {
                {GlobalTab::Jogo,          ui::loc::get(ui::loc::Text::TabGame)},
                {GlobalTab::Mapa,          ui::loc::get(ui::loc::Text::TabMap)},
                {GlobalTab::Registro,      ui::loc::get(ui::loc::Text::TabBrief)},
                {GlobalTab::Progresso,     ui::loc::get(ui::loc::Text::TabStats)},
                {GlobalTab::Configuracoes, ui::loc::get(ui::loc::Text::TabSettings)},
                {GlobalTab::Mods,          ui::loc::get(ui::loc::Text::TabMods)},
                {GlobalTab::Trainer,       ui::loc::get(ui::loc::Text::TabTrainer)},
            };
            const TopTabEntry kFrontEndTabs[] = {
                {GlobalTab::Configuracoes, ui::loc::get(ui::loc::Text::TabSettings)},
                {GlobalTab::Mods,          ui::loc::get(ui::loc::Text::TabMods)},
            };
            const TopTabEntry *tabs = inGameplay ? kAllTabs : kFrontEndTabs;
            const int kN = inGameplay ? 7 : 2;

            ImGui::PushFont(fontCondensed(), std::min(L.uf(23.0f), L.sw * 0.014f * 1.15f));
            float totalW = 0.0f;
            float widths[7]{};
            const float gap = L.u(32.0f);
            for (int i = 0; i < kN; ++i)
            {
                widths[i] = ImGui::CalcTextSize(tabs[i].name).x;
                totalW += widths[i];
            }
            totalW += gap * (kN - 1);

            float x = (L.sw - totalW) * 0.5f;
            const float cy = barH * 0.5f;
            const int plat = activePromptPreset();

            // Left tab switch icon (Q / LB / L1 / L)
            drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "Q"), x - L.u(20.0f), cy, true);

            float targetX = x, targetW = widths[0];
            for (int i = 0; i < kN; ++i)
            {
                const bool active = (g_tab == tabs[i].tab);
                const float tw = widths[i];
                const ImVec2 p0(x - L.u(8.0f), 0.0f);
                const ImVec2 p1(x + tw + L.u(8.0f), barH);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##gtab_") + std::to_string(i)).c_str(), ImVec2(p1.x - p0.x, p1.y - p0.y)))
                {
                    if (g_tab != tabs[i].tab)
                    {
                        const float dir = (static_cast<int>(tabs[i].tab) > static_cast<int>(g_tab)) ? 1.0f : -1.0f;
                        g_tab = tabs[i].tab;
                        triggerViewTransition(dir);
                        playMenuSfx(lcs::game::MenuSfx::Confirm);
                    }
                }
                const bool hov = ImGui::IsItemHovered();
                const ImVec2 ts = ImGui::CalcTextSize(tabs[i].name);
                dl->AddText(ImVec2(x, cy - ts.y * 0.5f),
                            active ? u32(kWhite) : (hov ? u32(kCrimsonHi) : u32(kDim, 0.85f)),
                            tabs[i].name);
                if (active)
                {
                    targetX = x - L.u(2.0f);
                    targetW = tw + L.u(4.0f);
                }
                x += tw + gap;
            }

            // Smoothly gliding GTA crimson underline under the active top tab
            glideFloat(g_topTabX, targetX, 20.0f);
            glideFloat(g_topTabW, targetW, 20.0f);
            dl->AddRectFilled(ImVec2(g_topTabX, barH - L.u(4.0f)),
                              ImVec2(g_topTabX + g_topTabW, barH),
                              u32(kCrimson));

            // Right tab switch icon (E / RB / R1 / R)
            drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "E"), x - gap + L.u(20.0f), cy, false);
            ImGui::PopFont();
        }

        // GTA 6 Toggle Pill ("Sim" / "Não" + oval switch with crimson active fill)
        bool drawGta6Toggle(ImDrawList *dl, const Layout &L, const char *id, ImVec2 rightEdge, float rowH, bool &val, bool selected)
        {
            using namespace palette;
            const float pw = L.rf(50.0f);
            const float ph = L.rf(24.0f);
            const ImVec2 p0(rightEdge.x - pw, rightEdge.y + (rowH - ph) * 0.5f);
            const ImVec2 p1(rightEdge.x, p0.y + ph);

            ImGui::SetCursorScreenPos(ImVec2(p0.x - L.u(52.0f), rightEdge.y));
            const bool clicked = ImGui::InvisibleButton(id, ImVec2(pw + L.u(52.0f), rowH));
            if (clicked)
            {
                val = !val;
                playMenuSfx(lcs::game::MenuSfx::Confirm);
            }

            const char *txt = ui::loc::tx(val ? "Sim" : "Não");
            ImGui::PushFont(fontRegular(), L.uf(20.0f));
            const ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText(ImVec2(p0.x - L.u(14.0f) - ts.x, rightEdge.y + (rowH - ts.y) * 0.5f),
                        selected ? u32(kSelectText) : u32(kWhite), txt);
            ImGui::PopFont();

            const ImU32 trackCol = val ? u32(kCrimson) : (selected ? u32(0xBDBDC6u) : u32(kPillOff, 0.92f));
            const ImU32 borderCol = val ? u32(kCrimsonHi) : (selected ? u32(0x70707Au) : u32(kWhite, 0.55f));
            dl->AddRectFilled(p0, p1, trackCol, ph * 0.5f);
            dl->AddRect(p0, p1, borderCol, ph * 0.5f, 0, L.u(1.4f));

            const float r = ph * 0.36f;
            const float cx = val ? (p1.x - ph * 0.5f) : (p0.x + ph * 0.5f);
            const float cy = (p0.y + p1.y) * 0.5f;
            dl->AddCircleFilled(ImVec2(cx, cy), r, u32(kWhite));
            return clicked;
        }

        // GTA 6 Cycle Control ("Valor   i / N   < >")
        bool drawGta6Cycle(ImDrawList *dl, const Layout &L, const char *id, ImVec2 rightEdge, float rowH,
                           int &sel, const char *const *opts, int count, bool selected)
        {
            using namespace palette;
            if (count <= 0) return false;
            sel = std::clamp(sel, 0, count - 1);
            bool changed = false;

            const float cy = rightEdge.y + rowH * 0.5f;
            const float chevW = L.u(22.0f);
            const float rx = rightEdge.x - chevW * 0.5f;
            const float lx = rx - L.u(26.0f);

            ImGui::PushID(id);
            ImGui::SetCursorScreenPos(ImVec2(lx - chevW * 0.5f, rightEdge.y));
            if (ImGui::InvisibleButton("##prev", ImVec2(chevW, rowH)))
            {
                sel = (sel - 1 + count) % count;
                changed = true;
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
            const bool hovL = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(ImVec2(rx - chevW * 0.5f, rightEdge.y));
            if (ImGui::InvisibleButton("##next", ImVec2(chevW, rowH)))
            {
                sel = (sel + 1) % count;
                changed = true;
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
            const bool hovR = ImGui::IsItemHovered();

            const ImU32 textCol = selected ? u32(kSelectText) : u32(kWhite);
            const ImU32 subCol = selected ? u32(0x40404Au) : u32(kDim, 0.85f);
            drawChevron(dl, ImVec2(lx, cy), false, L.u(0.75f), hovL ? u32(kCrimson) : textCol);
            drawChevron(dl, ImVec2(rx, cy), true, L.u(0.75f), hovR ? u32(kCrimson) : textCol);

            char idxBuf[24];
            std::snprintf(idxBuf, sizeof(idxBuf), "%d / %d", sel + 1, count);
            ImGui::PushFont(fontRegular(), L.uf(17.5f));
            const ImVec2 is = ImGui::CalcTextSize(idxBuf);
            const float ix = lx - L.u(18.0f) - is.x;
            dl->AddText(ImVec2(ix, cy - is.y * 0.5f), subCol, idxBuf);
            ImGui::PopFont();

            const char *optTxt = ui::loc::tx(opts[sel]);
            ImGui::PushFont(fontSemibold(), L.uf(19.5f));
            const ImVec2 vs = ImGui::CalcTextSize(optTxt);
            dl->AddText(ImVec2(ix - L.u(14.0f) - vs.x, cy - vs.y * 0.5f), textCol, optTxt);
            ImGui::PopFont();
            ImGui::PopID();
            return changed;
        }

        // GTA 6 Slider Bar (thin horizontal bar + vertical "|" marker + integer number)
        bool drawGta6Slider(ImDrawList *dl, const Layout &L, const char *id, ImVec2 rightEdge, float rowH,
                            int &val, int mn, int mx, int step, const char *suffix, bool selected)
        {
            using namespace palette;
            bool changed = false;
            const float cy = rightEdge.y + rowH * 0.5f;
            const float numW = L.rf(58.0f);
            const float trackW = L.u(165.0f);
            const float trackH = L.u(9.0f);
            const float tx1 = rightEdge.x - numW;
            const float tx0 = tx1 - trackW;

            ImGui::PushID(id);
            ImGui::SetCursorScreenPos(ImVec2(tx0 - L.u(6.0f), rightEdge.y));
            ImGui::InvisibleButton("##sld", ImVec2(trackW + L.u(12.0f), rowH));
            if (ImGui::IsItemActive())
            {
                const float t = std::clamp((ImGui::GetIO().MousePos.x - tx0) / std::max(1.0f, trackW), 0.0f, 1.0f);
                int raw = mn + static_cast<int>(std::round(t * static_cast<float>(mx - mn)));
                if (step > 1)
                    raw = mn + ((raw - mn + step / 2) / step) * step;
                raw = std::clamp(raw, mn, mx);
                if (raw != val)
                {
                    val = raw;
                    changed = true;
                    const double now = ImGui::GetTime();
                    if ((now - g_lastSliderSfxTime) >= 0.065)
                    {
                        g_lastSliderSfxTime = now;
                        playMenuSfx(lcs::game::MenuSfx::Move);
                    }
                }
            }

            const float frac = std::clamp(static_cast<float>(val - mn) / std::max(1.0f, static_cast<float>(mx - mn)), 0.0f, 1.0f);
            const float fillX = tx0 + trackW * frac;
            dl->AddRectFilled(ImVec2(tx0, cy - trackH * 0.5f), ImVec2(tx1, cy + trackH * 0.5f),
                              selected ? u32(0xC6C6CEu) : u32(kWhite, 0.18f), L.u(2.0f));
            dl->AddRect(ImVec2(tx0, cy - trackH * 0.5f), ImVec2(tx1, cy + trackH * 0.5f),
                        selected ? u32(0x7A7A86u) : u32(kWhite, 0.35f), L.u(2.0f), 0, 1.0f);
            if (frac > 0.002f)
            {
                dl->AddRectFilled(ImVec2(tx0, cy - trackH * 0.5f), ImVec2(fillX, cy + trackH * 0.5f),
                                  u32(kCrimson), L.u(2.0f));
            }
            // Crisp vertical tick marker "|"
            dl->AddRectFilled(ImVec2(fillX - L.u(1.5f), cy - L.u(9.0f)),
                              ImVec2(fillX + L.u(1.5f), cy + L.u(9.0f)),
                              selected ? u32(kSelectText) : u32(kWhite));

            char buf[32];
            std::snprintf(buf, sizeof(buf), "%d%s", val, suffix ? suffix : "");
            ImGui::PushFont(fontSemibold(), L.uf(19.5f));
            const ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(rightEdge.x - ts.x, cy - ts.y * 0.5f),
                        selected ? u32(kSelectText) : u32(kWhite), buf);
            ImGui::PopFont();
            ImGui::PopID();
            return changed;
        }

        // ---- Save settings to lcs.ini + lcs.conf + controls.ini ---------------------------------------------------
        void persistAllSettings()
        {
            lcs::settings::apply_live();
            lcs::settings::save("lcs.ini");
            auto &s = lcs::settings::get();
            lcs::core::save_config_setting("gtalcs/lcs.conf", "fullscreen", s.display_window == 0 ? "on" : "off");
            lcs::core::save_config_setting("gtalcs/lcs.conf", "vsync", s.display_vsync ? "on" : "off");
            lcs::core::save_config_setting("gtalcs/lcs.conf", "max_fps", std::to_string(s.display_fps));
            lcs::core::save_config_setting("gtalcs/lcs.conf", "brightness", std::to_string(s.brightness));
            lcs::core::save_config_setting("gtalcs/lcs.conf", "contrast", std::to_string(s.contrast));
            lcs::core::save_config_setting("gtalcs/lcs.conf", "saturation", std::to_string(s.saturation));
            lcs::input::PCControls::instance().save_profile();
        }

        // ---- Shared GTA 6-inspired Load/Save Slot Screen & Fullscreen Confirm (Art on Left, UI on Right) ---------
        void drawGta6SlotScreen(ImDrawList *dl, const Layout &L, bool mouseMoved,
                                int charIdx, const char *eyebrow, const char *title,
                                const char *subtitle, bool warnSubtitle,
                                int maxSlots, const std::vector<lcs::game::SaveSlotInfo> &slots,
                                int &slotSel, const char *confirmVerb,
                                const std::function<void(int)> &onSelectSlot,
                                const std::function<void()> &onBack)
        {
            using namespace palette;
            // Fullscreen LCS crimson/charcoal GTA 6-inspired gradient + tall un-squashed character art on the left
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(L.sw, L.sh),
                                        u32(kBgTopLeft, 1.0f), u32(kBgMid, 1.0f),
                                        u32(kBgBotRight, 1.0f), u32(kBgTopLeft, 1.0f));
            drawRealCharacterArt(dl, L, charIdx, 0.91f, 0.96f, L.ox * 0.25f);
            eyebrow = ui::loc::tx(eyebrow);
            title = ui::loc::tx(title);
            subtitle = ui::loc::tx(subtitle);
            confirmVerb = ui::loc::tx(confirmVerb);

            const float slideOff = L.u(g_tabSlide);
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float rx0 = std::max(L.sw * 0.43f, L.ux(760.0f)) + slideOff;
            const float rx1 = L.sw - L.u(84.0f) + slideOff;
            const float topY = L.u(68.0f);

            eyebrow = ui::loc::tx(eyebrow);
            title = ui::loc::tx(title);
            subtitle = ui::loc::tx(subtitle);
            confirmVerb = ui::loc::tx(confirmVerb);

            ImGui::PushFont(fontCondensed(), L.uf(21.0f));
            dl->AddText(ImVec2(rx0, topY), u32(kDim, 0.90f * fadeA), eyebrow);
            ImGui::PopFont();

            ImGui::PushFont(fontCondensed(), L.uf(42.0f));
            dl->AddText(ImVec2(rx0, topY + L.rf(24.0f)), u32(kWhite, fadeA), title);
            ImGui::PopFont();

            ImGui::PushFont(fontRegular(), L.uf(19.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(rx0, topY + L.rf(76.0f)),
                        warnSubtitle ? u32(kCrimsonHi, fadeA) : u32(kDim, fadeA), subtitle, nullptr, rx1 - rx0);
            ImGui::PopFont();

            const int prevSlotSel = slotSel;
            slotSel = std::clamp(slotSel, 0, maxSlots - 1);
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                slotSel = (slotSel - 1 + maxSlots) % maxSlots;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                slotSel = (slotSel + 1) % maxSlots;
            if (slotSel != prevSlotSel)
                playMenuSfx(lcs::game::MenuSfx::Move);
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            {
                if (onSelectSlot)
                    onSelectSlot(slotSel);
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
            {
                playMenuSfx(lcs::game::MenuSfx::Back);
                if (onBack)
                    onBack();
            }

            const float listTop = topY + L.rf(122.0f);
            const float listBot = L.sh - L.rf(120.0f);
            const float rowH = std::min(L.rf(62.0f), (listBot - listTop) / static_cast<float>(maxSlots));

            // Smoothly gliding slot highlight bar
            glideFloat(g_slotBarY, listTop + slotSel * rowH, 24.0f);
            dl->AddRectFilled(ImVec2(rx0, g_slotBarY), ImVec2(rx1, g_slotBarY + rowH - L.u(4.0f)),
                              u32(kSelectBar, fadeA), L.u(3.0f));
            dl->AddRectFilled(ImVec2(rx0, g_slotBarY), ImVec2(rx0 + L.u(5.0f), g_slotBarY + rowH - L.u(4.0f)),
                              u32(kCrimson, fadeA), L.u(2.0f));

            for (int i = 0; i < maxSlots; ++i)
            {
                const ImVec2 p0(rx0, listTop + i * rowH);
                const ImVec2 p1(rx1, p0.y + rowH - L.u(4.0f));
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##gta6slot_") + std::to_string(i)).c_str(),
                                           ImVec2(p1.x - p0.x, p1.y - p0.y)))
                {
                    slotSel = i;
                    if (onSelectSlot)
                        onSelectSlot(i);
                }
                if (ImGui::IsItemHovered() && mouseMoved && slotSel != i)
                {
                    slotSel = i;
                    playMenuSfx(lcs::game::MenuSfx::Move);
                }

                const bool sel = (i == slotSel);
                const int st = (i < static_cast<int>(slots.size())) ? slots[i].state : 1;
                std::string nameTxt;
                if (st == 0 && !slots[i].name.empty())
                    nameTxt = slots[i].name;
                else if (st == 2)
                    nameTxt = ui::loc::tx("Arquivo corrompido");
                else if (st == 3)
                    nameTxt = ui::loc::tx("Compartimento indisponível");
                else
                    nameTxt = ui::loc::tx("Compartimento vazio");

                const std::string dateTxt = (st == 0) ? slots[i].date : "";

                if (!sel)
                {
                    dl->AddRectFilled(p0, p1, u32(kTopBarBg, 0.25f * fadeA), L.u(3.0f));
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.16f * fadeA), 1.0f);
                }

                char slotTag[32];
                if (i == 8) std::snprintf(slotTag, sizeof(slotTag), "%s", ui::loc::tx("AUTOSAVE"));
                else std::snprintf(slotTag, sizeof(slotTag), "SLOT %d", i + 1);

                const float midY = p0.y + (p1.y - p0.y) * 0.5f;
                ImGui::PushFont(sel ? fontSemibold() : fontRegular(), L.uf(17.0f));
                const ImVec2 stSz = ImGui::CalcTextSize(slotTag);
                dl->AddText(ImVec2(p0.x + L.u(18.0f), midY - stSz.y * 0.5f),
                            sel ? u32(kCrimson, fadeA) : u32(kCrimsonHi, fadeA), slotTag);
                ImGui::PopFont();

                ImGui::PushFont(sel ? fontSemibold() : fontRegular(), L.uf(21.0f));
                const ImVec2 nSz = ImGui::CalcTextSize(nameTxt.c_str());
                dl->AddText(ImVec2(p0.x + L.rf(148.0f), midY - nSz.y * 0.5f),
                            sel ? u32(kSelectText, fadeA) : (st == 0 ? u32(kWhite, fadeA) : u32(kDim, 0.78f * fadeA)),
                            nameTxt.c_str());
                ImGui::PopFont();

                if (!dateTxt.empty())
                {
                    ImGui::PushFont(fontRegular(), L.uf(18.0f));
                    const ImVec2 dSz = ImGui::CalcTextSize(dateTxt.c_str());
                    dl->AddText(ImVec2(p1.x - L.u(18.0f) - dSz.x, midY - dSz.y * 0.5f),
                                sel ? u32(kSelectText, fadeA) : u32(kDim, fadeA), dateTxt.c_str());
                    ImGui::PopFont();
                }
            }

            // Counter ("1 / 9") matching GTA 6 Image 5
            char countBuf[24];
            std::snprintf(countBuf, sizeof(countBuf), "%d / %d", slotSel + 1, maxSlots);
            ImGui::PushFont(fontSemibold(), L.uf(19.0f));
            dl->AddText(ImVec2(rx0, listTop + maxSlots * rowH + L.u(12.0f)), u32(kDim, 0.90f * fadeA), countBuf);
            ImGui::PopFont();

            float fx = L.sw - L.u(56.0f);
            const float fy = L.sh - L.u(46.0f);
            fx = drawFooterPrompt(dl, L, fx, fy, "VOLTAR", "ESC");
            drawFooterPrompt(dl, L, fx, fy, confirmVerb, "ENTER");
        }

        void drawGta6ConfirmScreen(ImDrawList *dl, const Layout &L, bool mouseMoved,
                                   int charIdx, const char *question, const char *subtitle,
                                   const char *yesLabel, const char *noLabel,
                                   int &confirmBtn,
                                   const std::function<void()> &onConfirm,
                                   const std::function<void()> &onCancel)
        {
            using namespace palette;
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(L.sw, L.sh),
                                        u32(kBgTopLeft, 1.0f), u32(kBgMid, 1.0f),
                                        u32(kBgBotRight, 1.0f), u32(kBgTopLeft, 1.0f));
            drawRealCharacterArt(dl, L, charIdx, 0.91f, 0.96f, L.ox * 0.25f);
            question = ui::loc::tx(question);
            subtitle = ui::loc::tx(subtitle);
            yesLabel = ui::loc::tx(yesLabel);
            noLabel = ui::loc::tx(noLabel);

            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float rx0 = std::max(L.sw * 0.44f, L.ux(780.0f)) + L.u(g_tabSlide);
            const float rx1 = L.sw - L.u(96.0f) + L.u(g_tabSlide);
            const float midY = L.sh * 0.44f;

            const int prevBtn = confirmBtn;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A) ||
                ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                confirmBtn = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D) ||
                ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                confirmBtn = 1;
            if (confirmBtn != prevBtn)
                playMenuSfx(lcs::game::MenuSfx::Move);

            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            {
                playMenuSfx(confirmBtn == 0 ? lcs::game::MenuSfx::Confirm : lcs::game::MenuSfx::Back);
                if (confirmBtn == 0 && onConfirm) onConfirm();
                else if (confirmBtn == 1 && onCancel) onCancel();
                return;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
            {
                playMenuSfx(lcs::game::MenuSfx::Back);
                if (onCancel) onCancel();
                return;
            }

            ImGui::PushFont(fontCondensed(), L.uf(48.0f));
            dl->AddText(ImVec2(rx0, midY - L.rf(78.0f)), u32(kWhite, fadeA), question);
            ImGui::PopFont();

            dl->AddLine(ImVec2(rx0, midY - L.u(12.0f)), ImVec2(rx1, midY - L.u(12.0f)),
                        u32(kHairline, 0.35f * fadeA), L.u(1.5f));

            ImGui::PushFont(fontRegular(), L.uf(22.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(rx0, midY + L.u(10.0f)), u32(kDim, 0.94f * fadeA),
                        subtitle, nullptr, rx1 - rx0);
            ImGui::PopFont();

            const float btnY = midY + L.rf(108.0f);
            const float btnH = L.rf(52.0f);
            const ImVec2 yesP0(rx0, btnY);
            const ImVec2 yesP1(rx0 + L.rf(230.0f), btnY + btnH);
            const ImVec2 noP0(yesP1.x + L.u(20.0f), btnY);
            const ImVec2 noP1(noP0.x + L.rf(200.0f), btnY + btnH);

            ImGui::SetCursorScreenPos(yesP0);
            if (ImGui::InvisibleButton("##gta6_conf_yes", ImVec2(yesP1.x - yesP0.x, btnH)))
            {
                confirmBtn = 0;
                playMenuSfx(lcs::game::MenuSfx::Confirm);
                if (onConfirm) onConfirm();
                return;
            }
            if (ImGui::IsItemHovered() && mouseMoved && confirmBtn != 0)
            {
                confirmBtn = 0;
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            ImGui::SetCursorScreenPos(noP0);
            if (ImGui::InvisibleButton("##gta6_conf_no", ImVec2(noP1.x - noP0.x, btnH)))
            {
                confirmBtn = 1;
                playMenuSfx(lcs::game::MenuSfx::Back);
                if (onCancel) onCancel();
                return;
            }
            if (ImGui::IsItemHovered() && mouseMoved && confirmBtn != 1)
            {
                confirmBtn = 1;
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            const bool selYes = (confirmBtn == 0);
            dl->AddRectFilled(yesP0, yesP1, selYes ? u32(kSelectBar, fadeA) : u32(kTopBarBg, 0.72f * fadeA), L.u(3.0f));
            if (selYes)
                dl->AddRectFilled(yesP0, ImVec2(yesP0.x + L.u(5.0f), yesP1.y), u32(kCrimson, fadeA));
            else
                dl->AddRect(yesP0, yesP1, u32(kHairline, 0.28f * fadeA), L.u(3.0f));
            ImGui::PushFont(fontSemibold(), L.uf(20.5f));
            const ImVec2 ySz = ImGui::CalcTextSize(yesLabel);
            dl->AddText(ImVec2(yesP0.x + (yesP1.x - yesP0.x - ySz.x) * 0.5f, yesP0.y + (btnH - ySz.y) * 0.5f),
                        selYes ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), yesLabel);
            ImGui::PopFont();

            const bool selNo = (confirmBtn == 1);
            dl->AddRectFilled(noP0, noP1, selNo ? u32(kSelectBar, fadeA) : u32(kTopBarBg, 0.72f * fadeA), L.u(3.0f));
            if (selNo)
                dl->AddRectFilled(noP0, ImVec2(noP0.x + L.u(5.0f), noP1.y), u32(kCrimson, fadeA));
            else
                dl->AddRect(noP0, noP1, u32(kHairline, 0.28f * fadeA), L.u(3.0f));
            ImGui::PushFont(fontSemibold(), L.uf(20.5f));
            const ImVec2 nSz = ImGui::CalcTextSize(noLabel);
            dl->AddText(ImVec2(noP0.x + (noP1.x - noP0.x - nSz.x) * 0.5f, noP0.y + (btnH - nSz.y) * 0.5f),
                        selNo ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), noLabel);
            ImGui::PopFont();

            float fx = L.sw - L.u(56.0f);
            const float fy = L.sh - L.u(46.0f);
            fx = drawFooterPrompt(dl, L, fx, fy, "NÃO", "ESC");
            drawFooterPrompt(dl, L, fx, fy, "SIM", "ENTER");
        }

        // ---- Screen 1: JOGO (Hub Principal + Save / Load / New Game nativos) -----------------------------------------
        void drawTabJogo(ImDrawList *dl, const Layout &L, bool mouseMoved)
        {
            using namespace palette;
            auto &trainer = lcs::game::LCSTrainer::instance();

            if (g_jogoSubView == 1 || g_jogoSubView == 2)
            {
                const bool isSave = (g_jogoSubView == 1);
                const auto snap = trainer.read_snapshot();
                const auto slots = trainer.read_save_slots();
                if (g_jogoConfirmSlot >= 0)
                {
                    char qTitle[96];
                    if (g_jogoConfirmSlot == 8)
                        std::snprintf(qTitle, sizeof(qTitle), "%s", ui::loc::tx("CARREGAR AUTOSAVE?"));
                    else
                        std::snprintf(qTitle, sizeof(qTitle), "%s %d?",
                                      ui::loc::tx(isSave ? "SOBRESCREVER SLOT" : "CARREGAR SLOT"),
                                      g_jogoConfirmSlot + 1);
                    drawGta6ConfirmScreen(
                        dl, L, mouseMoved, isSave ? 2 : 0, qTitle,
                        isSave ? "Qualquer progresso existente neste slot será substituído pelo estado atual."
                               : "Todo o progresso atual não salvo em Liberty City será perdido.",
                        isSave ? "SALVAR" : "CARREGAR", "CANCELAR", g_jogoConfirmBtn,
                        [&]() {
                            if (isSave)
                                trainer.request_save_slot(g_jogoConfirmSlot);
                            else
                            {
                                trainer.request_load_slot(g_jogoConfirmSlot);
                                closeGlobalMenu();
                            }
                            g_jogoConfirmSlot = -1;
                        },
                        [&]() { g_jogoConfirmSlot = -1; });
                }
                else
                {
                    const int maxSlots = isSave ? 8 : 9;
                    const char *subMsg = isSave
                        ? (snap.in_mission ? "AVISO: Não é possível salvar durante uma missão ativa."
                                           : "Selecione um slot para salvar seu progresso atual em Liberty City.")
                        : "Selecione um slot válido em Liberty City (Slots 1–8 ou AutoSave).";
                    drawGta6SlotScreen(
                        dl, L, mouseMoved, isSave ? 2 : 0, "JOGO /",
                        isSave ? "SALVAR" : "CARREGAR", subMsg, (isSave && snap.in_mission),
                        maxSlots, slots, g_jogoSlotSel, isSave ? "SALVAR" : "CARREGAR",
                        [&](int idx) {
                            const int st = (idx < static_cast<int>(slots.size())) ? slots[idx].state : 1;
                            if (isSave)
                            {
                                if (!snap.attached || !snap.in_gameplay)
                                    trainer.push_notice(ui::loc::tx("Entre no gameplay para poder salvar."), 1);
                                else if (snap.in_mission)
                                    trainer.push_notice(ui::loc::tx("Não é possível salvar durante uma missão!"), 1);
                                else
                                {
                                    g_jogoConfirmSlot = idx;
                                    g_jogoConfirmBtn = 0;
                                }
                            }
                            else if (st == 0)
                            {
                                g_jogoConfirmSlot = idx;
                                g_jogoConfirmBtn = 0;
                            }
                            else
                            {
                                trainer.push_notice(ui::loc::tx("Este slot está vazio ou corrompido."), 1);
                            }
                        },
                        [&]() { g_jogoSubView = 0; });
                }
                return;
            }

            if (g_jogoSubView == 3)
            {
                drawGta6ConfirmScreen(
                    dl, L, mouseMoved, 1,
                    ui::loc::get(ui::loc::Text::ConfirmNewGameTitle),
                    ui::loc::get(ui::loc::Text::ConfirmNewGameBody),
                    ui::loc::get(ui::loc::Text::ConfirmYes),
                    ui::loc::get(ui::loc::Text::ConfirmNo),
                    g_jogoConfirmBtn,
                    [&]() {
                        trainer.request_new_game();
                        g_jogoSubView = 0;
                        closeGlobalMenu();
                    },
                    [&]() { g_jogoSubView = 0; });
                return;
            }

            // Subtle dark vignette over the paused live game frame (frosted dark right side)
            dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(L.sw, L.sh),
                                        u32(kBgTopLeft, 0.35f), u32(kBgBotRight, 0.45f),
                                        u32(kBgTopLeft, 0.65f), u32(kBgTopLeft, 0.55f));

            // Left glassmorphism panel (~29% of screen width, matching GTA 6 Image 8)
            const float panelW = std::clamp(L.sw * 0.29f, L.u(420.0f), L.u(580.0f));
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(0, 0), ImVec2(panelW, L.sh), u32(kPanelGlass, 0.88f * fadeA));
            dl->AddLine(ImVec2(panelW, 0), ImVec2(panelW, L.sh), u32(kHairline, 0.14f * fadeA), 1.0f);

            // Title top-left
            ImGui::PushFont(fontCondensed(), L.uf(40.0f));
            dl->AddText(ImVec2(L.u(56.0f), L.u(54.0f)), u32(kWhite, fadeA), ui::loc::get(ui::loc::Text::TabGame));
            ImGui::PopFont();

            const int uiLang = lcs::settings::get().game_language;
            const char *saveLbl = (uiLang == 0) ? "Save Game" : (uiLang == 4) ? "Guardar Partida" : (uiLang == 1) ? "Sauvegarder" : (uiLang == 2) ? "Spiel speichern" : (uiLang == 3) ? "Salva partita" : (uiLang == 5) ? "Сохранить игру" : (uiLang == 6) ? "セーブ" : "Salvar Jogo";
            const char *mapLbl  = (uiLang == 0) ? "Map" : (uiLang == 1) ? "Carte" : (uiLang == 2) ? "Karte" : (uiLang == 3) ? "Mappa" : (uiLang == 5) ? "Карта" : (uiLang == 6) ? "マップ" : "Mapa";
            const char *briefLbl= (uiLang == 0) ? "Brief" : (uiLang == 1) ? "Briefing" : (uiLang == 2) ? "Bericht" : (uiLang == 3) ? "Diario" : (uiLang == 4) ? "Resumen" : (uiLang == 5) ? "Сводка" : (uiLang == 6) ? "概要" : "Resumo";
            const char *statLbl = (uiLang == 0 || uiLang == 1) ? "Stats" : (uiLang == 2) ? "Statistik" : (uiLang == 3) ? "Statistiche" : (uiLang == 4) ? "Estadísticas" : (uiLang == 5) ? "Прогресс" : (uiLang == 6) ? "統計" : "Progresso";

            struct HubItem { const char *label; std::function<void()> action; };
            std::vector<HubItem> items = {
                {ui::loc::get(ui::loc::Text::ResumeGame), [] {
                    playMenuSfx(lcs::game::MenuSfx::Back);
                    closeGlobalMenu();
                }},
                {saveLbl, [&] {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    trainer.request_refresh_save_slots();
                    g_jogoSubView = 1;
                    g_jogoSlotSel = 0;
                    g_jogoConfirmSlot = -1;
                    g_jogoConfirmBtn = 0;
                    triggerViewTransition(1.0f);
                }},
                {ui::loc::get(ui::loc::Text::LoadGame), [&] {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    trainer.request_refresh_save_slots();
                    g_jogoSubView = 2;
                    g_jogoSlotSel = 0;
                    g_jogoConfirmSlot = -1;
                    g_jogoConfirmBtn = 0;
                    triggerViewTransition(1.0f);
                }},
                {ui::loc::get(ui::loc::Text::StartNewGame), [] {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    g_jogoSubView = 3;
                    g_jogoConfirmSlot = -1;
                    g_jogoConfirmBtn = 0;
                    triggerViewTransition(1.0f);
                }},
                {mapLbl, [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Mapa; triggerViewTransition(1.0f); }},
                {briefLbl, [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Registro; triggerViewTransition(1.0f); }},
                {statLbl, [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Progresso; triggerViewTransition(1.0f); }},
                {ui::loc::get(ui::loc::Text::Options), [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Configuracoes; g_cfgRow = 0; triggerViewTransition(1.0f); }},
                {ui::loc::tx("Mods"), [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Mods; g_modsRow = 0; triggerViewTransition(1.0f); }},
                {ui::loc::tx("Trainer"), [] { playMenuSfx(lcs::game::MenuSfx::Confirm); g_tab = GlobalTab::Trainer; g_trainerRow = 0; triggerViewTransition(1.0f); }},
                {ui::loc::get(ui::loc::Text::QuitGame), [&] {
                    playMenuSfx(lcs::game::MenuSfx::Back);
                    closeGlobalMenu();
                    trainer.request_quit();
                }},
            };
            const int n = static_cast<int>(items.size());
            const int prevHubSel = g_hubSel;

            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                g_hubSel = (g_hubSel - 1 + n) % n;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                g_hubSel = (g_hubSel + 1) % n;
            if (g_hubSel != prevHubSel)
                playMenuSfx(lcs::game::MenuSfx::Move);
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))
            {
                items[std::clamp(g_hubSel, 0, n - 1)].action();
                return;
            }

            const float startY = L.u(156.0f);
            const float footY = L.sh - L.u(78.0f);
            const float rowH = std::min(L.rf(50.0f), (footY - startY - L.u(12.0f)) / static_cast<float>(n));
            const float x0 = L.u(48.0f);
            const float x1 = panelW - L.u(36.0f);

            // Gliding GTA 6 selection bar & Crimson pulse
            glideFloat(g_hubBarY, startY + g_hubSel * rowH, 24.0f);
            const float pulse = 0.88f + 0.12f * std::sin(g_menuGlowTime * 4.2f);
            dl->AddRectFilled(ImVec2(x0, g_hubBarY), ImVec2(x1, g_hubBarY + rowH), u32(kSelectBar, fadeA));
            dl->AddRectFilled(ImVec2(x0, g_hubBarY), ImVec2(x0 + L.u(5.0f), g_hubBarY + rowH), u32(kCrimson, fadeA * pulse));

            for (int i = 0; i < n; ++i)
            {
                const ImVec2 p0(x0, startY + i * rowH);
                const ImVec2 p1(x1, p0.y + rowH);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##hub_") + std::to_string(i)).c_str(), ImVec2(p1.x - p0.x, p1.y - p0.y)))
                {
                    g_hubSel = i;
                    items[i].action();
                    return;
                }
                if (ImGui::IsItemHovered() && mouseMoved && g_hubSel != i)
                {
                    g_hubSel = i;
                    playMenuSfx(lcs::game::MenuSfx::Move);
                }

                const bool sel = (i == g_hubSel);
                if (!sel)
                {
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.14f * fadeA), 1.0f);
                }

                ImGui::PushFont(sel ? fontSemibold() : fontRegular(), L.uf(21.0f));
                const ImVec2 ts = ImGui::CalcTextSize(items[i].label);
                dl->AddText(ImVec2(p0.x + L.u(20.0f), p0.y + (rowH - ts.y) * 0.5f),
                            sel ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), items[i].label);
                ImGui::PopFont();
            }

            // Left footer: Build version
            dl->AddLine(ImVec2(x0, footY), ImVec2(x1, footY), u32(kHairline, 0.16f * fadeA), 1.0f);
            ImGui::PushFont(fontSemibold(), L.uf(19.0f));
            dl->AddText(ImVec2(x0, footY + L.u(18.0f)), u32(kDim), "Build 1.0.0  •  LCS.rec");
            ImGui::PopFont();

            // Right footer prompts
            float rx = L.sw - L.u(56.0f);
            const float cy = L.sh - L.u(46.0f);
            rx = drawFooterPrompt(dl, L, rx, cy, ui::loc::get(ui::loc::Text::HintBack), "ESC");
            drawFooterPrompt(dl, L, rx, cy, ui::loc::get(ui::loc::Text::HintSelect), "ENTER");
        }

        // ---- Screen 2: MAPA (Liberty City Fullscreen Interactive Map — Free Panning + Controller + Clamped Zoom Out)
        void drawTabMapa(ImDrawList *dl, const Layout &L)
        {
            using namespace palette;
            const auto snap = lcs::game::LCSTrainer::instance().read_snapshot();
            const float topY = L.u(62.0f);
            const float availH = std::max(120.0f, L.sh - topY);
            // Measured opaque land pixels in 2000x2000 liberty_city_hd_map.png:
            // world X=[-1627.0..1705.8], Y=[-2187.6..1117.3], vertical land span = 2698.5.
            // Lock maximum zoom-out cleanly to fit the island, while keeping map panning completely free.
            constexpr float kPanMinX = -1750.0f, kPanMaxX = 1800.0f;
            constexpr float kPanMinY = -2250.0f, kPanMaxY = 1180.0f;
            const float minZoom = std::clamp((availH * 0.88f) / (2698.5f * 0.28f * L.unit), 0.90f, 1.35f);
            constexpr float maxZoom = 3.20f;

            if (!g_mapInit)
            {
                g_mapPanX = std::clamp(snap.pos[0], kPanMinX + 150.0f, kPanMaxX - 150.0f);
                g_mapPanY = std::clamp(snap.pos[1], kPanMinY + 150.0f, kPanMaxY - 150.0f);
                g_mapZoom = std::max(minZoom, 1.50f);
                g_mapInit = true;
            }
            g_mapZoom = std::clamp(g_mapZoom, minZoom, maxZoom);

            if (ImGui::IsKeyPressed(ImGuiKey_Tab, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))
            {
                g_mapShowLegend = !g_mapShowLegend;
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            // Deep atmospheric Liberty City ocean background (dark slate-navy with subtle vignette; always opaque)
            dl->AddRectFilledMultiColor(ImVec2(0, topY), ImVec2(L.sw, L.sh),
                                        u32(0x0D151Eu), u32(0x101B26u),
                                        u32(0x140E12u), u32(0x0B121Au));

            // Interactive pan & zoom area
            ImGui::SetCursorScreenPos(ImVec2(0, topY));
            ImGui::InvisibleButton("##lcs_map_canvas", ImVec2(L.sw, L.sh - topY));
            const bool mapHov = ImGui::IsItemHovered();
            const ImGuiIO &io = ImGui::GetIO();
            if (mapHov && io.MouseWheel != 0.0f)
                g_mapZoom = std::clamp(g_mapZoom + io.MouseWheel * 0.16f, minZoom, maxZoom);
            if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
                g_mapZoom = std::clamp(g_mapZoom + 0.16f, minZoom, maxZoom);
            if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
                g_mapZoom = std::clamp(g_mapZoom - 0.16f, minZoom, maxZoom);

            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                g_mapPanX -= io.MouseDelta.x / (0.28f * L.unit * g_mapZoom);
                g_mapPanY += io.MouseDelta.y / (0.28f * L.unit * g_mapZoom);
            }

            // Smooth continuous panning: WASD, Arrow keys, D-Pad, and Controller Left Stick
            const float panRate = (1180.0f / std::max(0.5f, g_mapZoom)) * g_uiDt;
            if (ImGui::IsKeyDown(ImGuiKey_LeftArrow) || ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadLeft))
                g_mapPanX -= panRate;
            if (ImGui::IsKeyDown(ImGuiKey_RightArrow) || ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadRight))
                g_mapPanX += panRate;
            if (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadUp))
                g_mapPanY += panRate;
            if (ImGui::IsKeyDown(ImGuiKey_DownArrow) || ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_GamepadDpadDown))
                g_mapPanY -= panRate;

            const auto &inp = lcs::input::InputSystem::instance();
            const float lsX = inp.get_joy_axis(0, 0);
            const float lsY = inp.get_joy_axis(0, 1);
            if (std::fabs(lsX) > 0.05f) g_mapPanX += lsX * panRate;
            if (std::fabs(lsY) > 0.05f) g_mapPanY -= lsY * panRate;

            // Controller zoom: RT/R2 zooms in, LT/L2 zooms out, Right Stick vertical zooms smoothly
            const float ltZoom = std::clamp(inp.get_joy_axis(0, 4), 0.0f, 1.0f);
            const float rtZoom = std::clamp(inp.get_joy_axis(0, 5), 0.0f, 1.0f);
            const float rsY = inp.get_joy_axis(0, 3);
            const float ctrlZoomDelta = (rtZoom - ltZoom) * 1.45f * g_uiDt
                                      + ((std::fabs(rsY) > 0.12f) ? (-rsY * 1.25f * g_uiDt) : 0.0f);
            if (ctrlZoomDelta != 0.0f)
                g_mapZoom = std::clamp(g_mapZoom + ctrlZoomDelta, minZoom, maxZoom);

            if (ImGui::IsKeyPressed(ImGuiKey_R, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) ||
                ImGui::IsKeyPressed(ImGuiKey_GamepadR3, false))
            {
                g_mapPanX = snap.pos[0];
                g_mapPanY = snap.pos[1];
                g_mapZoom = std::max(minZoom, 1.50f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            // Free panning across the entire Liberty City map; only clamp to world limits
            g_mapPanX = std::clamp(g_mapPanX, kPanMinX, kPanMaxX);
            g_mapPanY = std::clamp(g_mapPanY, kPanMinY, kPanMaxY);
            const float pxPerUnit = 0.28f * L.unit * g_mapZoom;

            const float cx = L.sw * 0.5f;
            const float cy = topY + availH * 0.5f;
            auto worldToScreen = [&](float wx, float wy) -> ImVec2 {
                return ImVec2(cx + (wx - g_mapPanX) * pxPerUnit, cy - (wy - g_mapPanY) * pxPerUnit);
            };

            // Render HD Liberty City Map texture aligned 1:1 to Liberty City world coordinates
            const auto &mapTex = ui::assets::mapTexture();
            if (mapTex.valid())
            {
                const bool isHd2000 = (mapTex.w == 2000 && mapTex.h == 2000);
                const ImVec2 m0 = isHd2000 ? worldToScreen(-1627.0f, 1117.3f) : worldToScreen(-2000.0f, 2000.0f);
                const ImVec2 m1 = isHd2000 ? worldToScreen(1705.8f, -2187.6f) : worldToScreen(2000.0f, -2000.0f);
                dl->PushClipRect(ImVec2(0, topY), ImVec2(L.sw, L.sh), true);
                dl->AddImage(mapTex.id, m0, m1, ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 255));
                dl->PopClipRect();
            }

            const int mapPlat = activePromptPreset();
            const bool hasCtrl = isControllerPromptPreset(mapPlat) &&
                                 (lcs::game::LCSPrompts::instance().active_device() != lcs::game::PromptDevice::KeyboardMouse);
            const ImVec2 hoverRef = (!hasCtrl && mapHov) ? io.MousePos : ImVec2(cx, cy);
            const float inspectWorldX = (!hasCtrl && mapHov) ? (g_mapPanX + (io.MousePos.x - cx) / pxPerUnit) : g_mapPanX;
            const float hoverRadius = (!hasCtrl && mapHov) ? L.u(24.0f) : L.u(36.0f);

            // Real live radar blips from CRadar::ms_RadarTrace with exact pRadarSprites icons
            const auto liveBlips = lcs::game::LCSTrainer::instance().read_radar_blips();
            std::string hoveredName = (inspectWorldX > 620.0f) ? "PORTLAND" : (inspectWorldX < -340.0f) ? "SHORESIDE VALE" : "STAUNTON ISLAND";
            if (snap.curr_area != 0)
                hoveredName += ui::loc::tx(" (INTERIOR)");
            std::string hoveredDesc = ui::loc::tx((snap.curr_area != 0)
                ? "Jogador dentro de um interior (posição mapeada na entrada)."
                : "Mapa em tempo real de Liberty City.");
            float bestHoverDist = hoverRadius;

            auto blipNameForSprite = [](uint16_t sp) -> const char * {
                switch (sp)
                {
                case 10: return "PHIL CASSIDY";
                case 16: return "AMMU-NATION";
                case 19: return ui::loc::tx("CASA SEGURA (SAFEHOUSE)");
                case 20: return ui::loc::tx("CLUBE / NIGHTCLUB");
                case 25: return ui::loc::tx("PROPRIEDADE");
                case 27: return "PAY 'N' SPRAY";
                case 28: return ui::loc::tx("TROCA DE ROUPA");
                case 30: return ui::loc::tx("TELEFONE PÚBLICO");
                case 31: return "8-BALL";
                case 32: return ui::loc::tx("CELULAR / PAGER");
                case 33: return "MA CIPRIANI";
                case 34: return "DONALD LOVE";
                case 35: return "JOSEPH DANIEL O'TOOLE";
                case 36: return "SALVATORE LEONE";
                case 37: return "MARIA";
                case 38: return "LEON MCAFFREY";
                case 39: return "TOSHIKO KASEN";
                case 40: return "VINCENZO CILLI";
                case 41: return ui::loc::tx("OFICINA DE BOMBAS (8-BALL)");
                case 42: return ui::loc::tx("LOJA DE ROUPAS");
                case 43: return ui::loc::tx("PROPRIEDADE");
                case 44: return ui::loc::tx("MISSÃO / OBJETIVO");
                case 45: return ui::loc::tx("BÔNUS / ITEM");
                case 47: return ui::loc::tx("CHECKPOINT");
                case 54: return ui::loc::tx("INDICADOR DE ALVO");
                case 62: return ui::loc::tx("ALVO DO JOGADOR");
                default: return ui::loc::tx("OBJETIVO / MISSÃO");
                }
            };
            auto legendLabelForSprite = [](uint16_t sp) -> const char * {
                switch (sp)
                {
                case 1:  return ui::loc::tx("Jogador");
                case 10: return "Phil Cassidy";
                case 16: return "Ammu-Nation";
                case 19: return ui::loc::tx("Casa Segura");
                case 20: return ui::loc::tx("Clube");
                case 25:
                case 43: return ui::loc::tx("Propriedade");
                case 27: return "Pay 'n' Spray";
                case 28:
                case 42: return ui::loc::tx("Roupas");
                case 30: return ui::loc::tx("Telefone Público");
                case 31: return "8-Ball";
                case 32: return ui::loc::tx("Pager");
                case 33: return "Ma Cipriani";
                case 34: return "Donald Love";
                case 35: return "JD O'Toole";
                case 36: return "Salvatore Leone";
                case 37: return "Maria";
                case 38: return "Leon McAffrey";
                case 39: return "Toshiko Kasen";
                case 40: return "Vincenzo Cilli";
                case 41: return ui::loc::tx("Bombas 8-Ball");
                case 44: return ui::loc::tx("Missão");
                case 45: return ui::loc::tx("Item Bônus");
                case 47: return ui::loc::tx("Checkpoint");
                default: return nullptr;
                }
            };

            struct LegendItem { uint16_t sprite; const char *label; ImU32 fallbackCol; };
            std::vector<LegendItem> legend;
            legend.push_back({1, ui::loc::tx("Jogador"), u32(kCrimson)});

            for (const auto &bl : liveBlips)
            {
                ImU32 col = u32(kCrimson);
                switch (bl.colour)
                {
                case 0: col = IM_COL32(229, 37, 30, 255); break;  // Red
                case 1: col = IM_COL32(46, 204, 113, 255); break; // Green
                case 2: col = IM_COL32(52, 152, 219, 255); break; // Blue
                case 3: col = IM_COL32(255, 255, 255, 255); break;// White
                case 4: col = IM_COL32(241, 196, 15, 255); break; // Yellow
                case 5: col = IM_COL32(155, 89, 182, 255); break; // Purple
                case 6: col = IM_COL32(26, 188, 156, 255); break; // Cyan
                default: col = IM_COL32(241, 196, 15, 255); break;
                }

                const char *ltxt = legendLabelForSprite(bl.sprite);
                if (ltxt)
                {
                    bool dup = false;
                    for (const auto &le : legend)
                    {
                        if (le.sprite == bl.sprite || std::strcmp(le.label, ltxt) == 0)
                        {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup)
                        legend.push_back({bl.sprite, ltxt, col});
                }

                const ImVec2 sp = worldToScreen(bl.x, bl.y);
                if (sp.y < topY || sp.y > L.sh || sp.x < 0.0f || sp.x > L.sw) continue;

                const auto &sprTex = ui::assets::radarSprite(static_cast<int>(bl.sprite));
                const float half = L.u(16.0f);
                if (sprTex.valid())
                {
                    dl->AddImage(sprTex.id, ImVec2(sp.x - half, sp.y - half), ImVec2(sp.x + half, sp.y + half));
                }
                else
                {
                    dl->AddCircleFilled(sp, L.u(7.5f), col);
                    dl->AddCircle(sp, L.u(7.5f), u32(kTopBarBg), 0, L.u(2.0f));
                }

                const float dHover = std::hypot(sp.x - hoverRef.x, sp.y - hoverRef.y);
                if (dHover < bestHoverDist)
                {
                    bestHoverDist = dHover;
                    hoveredName = blipNameForSprite(bl.sprite);
                    hoveredDesc = ui::loc::tx((bl.x > 620.0f) ? "Portland, Liberty City" : (bl.x < -340.0f) ? "Shoreside Vale, Liberty City" : "Staunton Island, Liberty City");
                }
            }

            // Real player position marker using touch_fe/radar_player_centre.png
            const ImVec2 pp = worldToScreen(snap.pos[0], snap.pos[1]);
            const auto &playerSpr = ui::assets::radarSprite(1);
            if (playerSpr.valid())
            {
                const float ph = L.u(15.0f);
                dl->AddImage(playerSpr.id, ImVec2(pp.x - ph, pp.y - ph), ImVec2(pp.x + ph, pp.y + ph));
            }
            else
            {
                dl->AddCircleFilled(pp, L.u(9.0f), u32(kWhite));
                dl->AddCircleFilled(pp, L.u(5.0f), u32(kCrimson));
            }

            // Center crosshair ((▲)): visible ONLY when actively navigating with a controller
            if (hasCtrl)
            {
                dl->AddCircle(ImVec2(cx, cy), L.u(16.0f), u32(kWhite, 0.90f), 24, L.u(2.0f));
                dl->AddTriangleFilled(ImVec2(cx, cy - L.u(7.0f)), ImVec2(cx - L.u(6.0f), cy + L.u(5.0f)),
                                      ImVec2(cx + L.u(6.0f), cy + L.u(5.0f)), u32(kCrimson));
            }

            // Top-left GTA IV-style Optional Legend (toggled with TAB / Y / Triangle)
            if (g_mapShowLegend)
            {
                const float lx = L.u(36.0f);
                const float ly0 = topY + L.u(26.0f);
                const float rowH = L.rf(30.0f);
                const float maxRows = std::min<float>(static_cast<float>(legend.size()), 14.0f);
                const ImVec2 legBg0(lx - L.u(10.0f), ly0 - L.u(8.0f));
                const ImVec2 legBg1(lx + L.rf(256.0f), ly0 + maxRows * rowH + L.u(6.0f));
                dl->AddRectFilled(legBg0, legBg1, u32(kTopBarBg, 0.68f), L.u(4.0f));

                ImGui::PushFont(fontRegular(), L.uf(18.0f));
                const int drawCount = std::min<int>(static_cast<int>(legend.size()), 14);
                for (int i = 0; i < drawCount; ++i)
                {
                    const float ry = ly0 + i * rowH;
                    const float iconR = L.u(10.0f);
                    const ImVec2 ic(lx + L.u(12.0f), ry + rowH * 0.5f);
                    const auto &spr = ui::assets::radarSprite(static_cast<int>(legend[i].sprite));
                    if (spr.valid())
                        dl->AddImage(spr.id, ImVec2(ic.x - iconR, ic.y - iconR), ImVec2(ic.x + iconR, ic.y + iconR));
                    else
                        dl->AddCircleFilled(ic, L.u(6.0f), legend[i].fallbackCol);

                    const ImVec2 ts = ImGui::CalcTextSize(legend[i].label);
                    dl->AddText(ImVec2(lx + L.u(34.0f), ry + (rowH - ts.y) * 0.5f), u32(kWhite), legend[i].label);
                }
                ImGui::PopFont();
            }

            // Top-right player cash & hovered district/blip card
            char cashBuf[48];
            std::snprintf(cashBuf, sizeof(cashBuf), "$%d", snap.money);
            ImGui::PushFont(fontCondensed(), L.uf(28.0f));
            const ImVec2 cs = ImGui::CalcTextSize(cashBuf);
            dl->AddText(ImVec2(L.sw - cs.x - L.u(48.0f), topY + L.u(24.0f)), u32(kWhite), cashBuf);
            ImGui::PopFont();

            const float cardW = L.rf(362.0f);
            const ImVec2 card0(L.sw - L.u(48.0f) - cardW, topY + L.u(68.0f));
            const ImVec2 card1(L.sw - L.u(48.0f), card0.y + L.rf(96.0f));
            dl->AddRectFilled(card0, card1, u32(kTopBarBg, 0.88f), L.u(3.0f));
            dl->AddRectFilled(card0, ImVec2(card1.x, card0.y + L.u(3.0f)), u32(kCrimson));
            ImGui::PushFont(fontCondensed(), L.uf(22.0f));
            dl->AddText(ImVec2(card0.x + L.u(16.0f), card0.y + L.u(14.0f)), u32(kWhite), hoveredName.c_str());
            ImGui::PopFont();
            ImGui::PushFont(fontRegular(), L.uf(16.5f));
            dl->AddText(nullptr, 0.0f, ImVec2(card0.x + L.u(16.0f), card0.y + L.rf(44.0f)),
                        u32(kDim), hoveredDesc.c_str(), nullptr, (card1.x - card0.x) - L.u(32.0f));
            ImGui::PopFont();

            // Bottom-right vertical command list using dynamic input prompts (deviceTokenForAction translates R/SCROLL/MOUSE/TAB/ESC)
            const char *legLabel = g_mapShowLegend ? "LEGENDA DESL." : "LEGENDA LIG.";
            const char *kCmds[][2] = {
                {"RECENTRALIZAR", "R"},
                {"ZOOM", "SCROLL"},
                {"MOVER", "MOUSE"},
                {legLabel, "TAB"},
                {"SAIR", "ESC"},
            };
            float cyCmd = L.sh - L.u(42.0f);
            for (int i = 4; i >= 0; --i)
            {
                drawFooterPrompt(dl, L, L.sw - L.u(48.0f), cyCmd, kCmds[i][0], kCmds[i][1]);
                cyCmd -= L.rf(36.0f);
            }
        }

        void drawRichBriefHelpText(ImDrawList *dl, const Layout &L, float maxW, int plat, const std::string &tag,
                                   const std::string &text, ImU32 tagCol, ImU32 txtCol)
        {
            const float fontSize = L.uf(20.5f);
            ImFont *font = fontRegular();
            const float lineH = fontSize * 1.38f;
            const float iconH = fontSize * 1.12f;
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float startX = cursor.x;
            float x = startX;
            float y = cursor.y;
            float totalH = lineH;

            ImGui::PushFont(fontSemibold(), fontSize);
            const ImVec2 tagSz = ImGui::CalcTextSize(tag.c_str());
            dl->AddText(ImVec2(x, y), tagCol, tag.c_str());
            ImGui::PopFont();
            x += tagSz.x + L.u(10.0f);

            ImGui::PushFont(font, fontSize);
            size_t pos = 0;
            while (pos < text.size())
            {
                if (text[pos] == '\n')
                {
                    x = startX;
                    y += lineH;
                    totalH += lineH;
                    ++pos;
                    continue;
                }
                if (text[pos] == ':' && pos + 1 < text.size() && text[pos + 1] == ':')
                {
                    const size_t close = text.find("::", pos + 2);
                    if (close != std::string::npos && close > pos + 2 && close - (pos + 2) <= 24)
                    {
                        const std::string tok = text.substr(pos + 2, close - (pos + 2));
                        const auto &tex = ui::assets::inputPromptTexture(plat, tok.c_str());
                        float iconW = iconH * (tex.valid() ? (static_cast<float>(tex.w) / std::max(1, tex.h)) : 1.35f);
                        if (x > startX + L.u(2.0f) && (x - startX) + iconW > maxW)
                        {
                            x = startX;
                            y += lineH;
                            totalH += lineH;
                        }
                        const float iy = y + (fontSize - iconH) * 0.5f;
                        if (tex.valid())
                        {
                            dl->AddImage(tex.id, ImVec2(x, iy), ImVec2(x + iconW, iy + iconH));
                        }
                        else
                        {
                            const ImVec2 kw = ImGui::CalcTextSize(tok.c_str());
                            iconW = kw.x + L.u(12.0f);
                            dl->AddRectFilled(ImVec2(x, iy), ImVec2(x + iconW, iy + iconH), palette::u32(palette::kSelectBar, 0.92f), L.u(4.0f));
                            dl->AddText(ImVec2(x + L.u(6.0f), y), palette::u32(palette::kSelectText), tok.c_str());
                        }
                        x += iconW + L.u(3.0f);
                        pos = close + 2;
                        continue;
                    }
                }
                if (text[pos] == ' ')
                {
                    const float sw = ImGui::CalcTextSize(" ").x;
                    if (x > startX) x += sw;
                    ++pos;
                    continue;
                }
                size_t end = pos;
                while (end < text.size() && text[end] != ' ' && text[end] != '\n')
                {
                    if (text[end] == ':' && end + 1 < text.size() && text[end + 1] == ':') break;
                    ++end;
                }
                const std::string word = text.substr(pos, end - pos);
                const float ww = ImGui::CalcTextSize(word.c_str()).x;
                if (x > startX + L.u(2.0f) && (x - startX) + ww > maxW)
                {
                    x = startX;
                    y += lineH;
                    totalH += lineH;
                }
                dl->AddText(ImVec2(x, y), txtCol, word.c_str());
                x += ww;
                pos = end;
            }
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(maxW, totalH));
        }

        // ---- Screen 3: RESUMO (Brief — Grouped Speaker Blocks, Inline Button Icons & Pure Objectives) ----------
        void drawTabRegistro(ImDrawList *dl, const Layout &L, bool mouseMoved)
        {
            using namespace palette;
            auto &trainer = lcs::game::LCSTrainer::instance();
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float slideOff = L.u(g_tabSlide);

            if (!trainer.is_in_gameplay())
            {
                drawBackdrop(dl, L, -1);
                drawFooterPrompt(dl, L, L.sw - L.u(56.0f), L.sh - L.u(46.0f), ui::loc::get(ui::loc::Text::HintBack), "ESC");
                return;
            }

            drawBackdrop(dl, L, 1);

            const float topY = L.u(62.0f);
            const float divX = L.ux(600.0f) + slideOff;
            dl->AddLine(ImVec2(divX, topY + L.u(42.0f)), ImVec2(divX, L.sh - L.u(90.0f)), u32(kHairline, 0.16f * fadeA), 1.0f);

            const char *kFilters[] = {
                ui::loc::get(ui::loc::Text::BriefFilterAll),
                ui::loc::get(ui::loc::Text::BriefFilterDialogue),
                ui::loc::get(ui::loc::Text::BriefFilterHelp),
                ui::loc::get(ui::loc::Text::BriefFilterObjective),
            };
            const int prevFilter = g_briefFilter;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                g_briefFilter = (g_briefFilter + 3) % 4;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                g_briefFilter = (g_briefFilter + 1) % 4;

            const float fx0 = L.ux(72.0f) + slideOff;
            const float fx1 = L.ux(540.0f) + slideOff;
            const float fy0 = topY + L.u(96.0f);
            const float rowH = L.rf(56.0f);

            // Gliding selection bar
            glideFloat(g_briefBarY, fy0 + g_briefFilter * rowH, 28.0f);
            dl->AddRectFilled(ImVec2(fx0, g_briefBarY), ImVec2(fx1, g_briefBarY + rowH), u32(kSelectBar, fadeA));
            dl->AddRectFilled(ImVec2(fx0, g_briefBarY), ImVec2(fx0 + L.u(5.0f), g_briefBarY + rowH), u32(kCrimson, fadeA));

            const float briefBarMid = g_briefBarY + rowH * 0.5f;
            for (int i = 0; i < 4; ++i)
            {
                const ImVec2 p0(fx0, fy0 + i * rowH);
                const ImVec2 p1(fx1, p0.y + rowH);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##bf_") + std::to_string(i)).c_str(), ImVec2(p1.x - p0.x, p1.y - p0.y)))
                    g_briefFilter = i;
                if (ImGui::IsItemHovered() && mouseMoved)
                    g_briefFilter = i;

                const bool sel = (i == g_briefFilter);
                const bool barOnRow = (briefBarMid >= p0.y && briefBarMid < p1.y);
                if (!sel)
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.14f * fadeA), 1.0f);
                ImGui::PushFont(sel ? fontSemibold() : fontRegular(), L.uf(22.0f));
                const ImVec2 ts = ImGui::CalcTextSize(kFilters[i]);
                dl->AddText(ImVec2(p0.x + L.u(18.0f), p0.y + (rowH - ts.y) * 0.5f),
                            barOnRow ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), kFilters[i]);
                ImGui::PopFont();
            }
            if (g_briefFilter != prevFilter)
                playMenuSfx(lcs::game::MenuSfx::Move);

            // Right column: Grouped speaker dialogue blocks, inline-icon help entries, and pure mission objectives
            const float rx0 = L.ux(700.0f) + slideOff;
            const float rw = L.ux(1800.0f) - L.ux(700.0f);
            const auto realBriefs = trainer.read_real_briefs();
            const int plat = activePromptPreset();

            ImGui::SetCursorScreenPos(ImVec2(rx0, fy0));
            ImGui::BeginChild("##BriefScroll", ImVec2(rw, L.sh - fy0 - L.u(100.0f)), false);
            ImDrawList *childDl = ImGui::GetWindowDrawList();
            ImGui::PushFont(fontRegular(), L.uf(20.5f));

            int shownCount = 0;
            std::string activeSpeakerBlock;
            int lastRenderedCat = -1;

            for (const auto &rb : realBriefs)
            {
                if (g_briefFilter > 0 && rb.category != (g_briefFilter - 1))
                    continue;
                ++shownCount;

                if (rb.category == 0)
                {
                    if (lastRenderedCat != 0 || rb.speaker != activeSpeakerBlock)
                    {
                        if (lastRenderedCat != -1)
                            ImGui::Dummy(ImVec2(0.0f, L.rf(14.0f)));
                        activeSpeakerBlock = rb.speaker;
                        const std::string hdr = !rb.speaker.empty()
                            ? (rb.speaker + ":")
                            : std::string(ui::loc::get(ui::loc::Text::BriefTagDialogue));
                        ImGui::PushFont(fontSemibold(), L.uf(21.5f));
                        ImGui::PushStyleColor(ImGuiCol_Text, rgba(kCrimsonHi, fadeA));
                        ImGui::TextUnformatted(hdr.c_str());
                        ImGui::PopStyleColor();
                        ImGui::PopFont();
                        ImGui::Dummy(ImVec2(0.0f, L.rf(2.0f)));
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kWhite, fadeA));
                    ImGui::TextWrapped("%s", rb.text.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, L.rf(3.0f)));
                }
                else if (rb.category == 1)
                {
                    if (lastRenderedCat != -1 && lastRenderedCat == 0)
                        ImGui::Dummy(ImVec2(0.0f, L.rf(10.0f)));
                    activeSpeakerBlock.clear();
                    drawRichBriefHelpText(childDl, L, rw - L.u(28.0f), plat,
                                          ui::loc::get(ui::loc::Text::BriefTagHelp), rb.text,
                                          u32(kMuted, fadeA), u32(kDim, fadeA));
                    ImGui::Dummy(ImVec2(0.0f, L.rf(12.0f)));
                }
                else
                {
                    if (lastRenderedCat != -1 && lastRenderedCat == 0)
                        ImGui::Dummy(ImVec2(0.0f, L.rf(10.0f)));
                    activeSpeakerBlock.clear();
                    const char *tag = ui::loc::get(ui::loc::Text::BriefTagObjective);
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kAmber, fadeA));
                    ImGui::TextUnformatted(tag);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0.0f, L.u(10.0f));
                    ImGui::PushStyleColor(ImGuiCol_Text, rgba(kWhite, fadeA));
                    ImGui::TextWrapped("%s", rb.text.c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0.0f, L.rf(12.0f)));
                }
                lastRenderedCat = rb.category;
            }

            if (shownCount == 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, rgba(kMuted, fadeA));
                ImGui::TextWrapped("%s", realBriefs.empty()
                    ? ui::loc::get(ui::loc::Text::BriefEmptyAll)
                    : ui::loc::get(ui::loc::Text::BriefEmptyFilter));
                ImGui::PopStyleColor();
            }

            ImGui::PopFont();
            ImGui::EndChild();

            drawFooterPrompt(dl, L, L.sw - L.u(56.0f), L.sh - L.u(46.0f), ui::loc::get(ui::loc::Text::HintBack), "ESC");
        }

        // ---- Screen 4: PROGRESSO (Live CStats) ---------------------------------------------------------------------
        void drawTabProgresso(ImDrawList *dl, const Layout &L)
        {
            using namespace palette;
            drawBackdrop(dl, L, 2);
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float slideOff = L.u(g_tabSlide);

            const auto snap = lcs::game::LCSTrainer::instance().read_snapshot();
            const float topY = L.u(100.0f);
            ImGui::PushFont(fontCondensed(), L.uf(36.0f));
            dl->AddText(ImVec2(L.ux(80.0f) + slideOff, topY), u32(kWhite, fadeA), ui::loc::get(ui::loc::Text::StatsTitle));
            ImGui::PopFont();

            const float pct = (snap.total_progress > 0.0f)
                                  ? std::clamp((snap.progress_made / snap.total_progress) * 100.0f, 0.0f, 100.0f)
                                  : 0.0f;
            const float acc = (snap.rounds_fired > 0)
                                  ? std::clamp((static_cast<float>(snap.bullets_hit) / static_cast<float>(snap.rounds_fired)) * 100.0f, 0.0f, 100.0f)
                                  : 0.0f;

            auto fmtF = [](const char *fmt, float v) { char b[48]; std::snprintf(b, sizeof(b), fmt, v); return std::string(b); };
            std::vector<std::pair<std::string, std::string>> stats = {
                {"Progresso total da história", fmtF("%.1f%%", pct)},
                {"Missões concluídas", std::to_string(snap.missions_passed) + " / " + std::to_string(std::max(1, snap.total_missions))},
                {"Dinheiro atual", "$" + std::to_string(snap.money)},
                {"Dias passados em Liberty City", std::to_string(snap.days_passed)},
                {"Saltos únicos encontrados", std::to_string(snap.jumps_found) + " / " + std::to_string(std::max(1, snap.jumps_total))},
                {"Pessoas eliminadas", std::to_string(snap.people_killed)},
                {"Veículos destruídos", std::to_string(snap.cars_exploded)},
                {"Precisão com armas de fogo", fmtF("%.0f%%", acc)},
                {"Estrelas de procurado (atingidas / evadidas)", std::to_string(snap.stars_attained) + " / " + std::to_string(snap.stars_evaded)},
                {"Distância percorrida a pé", fmtF("%.1f km", snap.dist_foot / 1000.0f)},
                {"Distância percorrida de carro", fmtF("%.1f km", snap.dist_car / 1000.0f)},
                {"Hospitalizações / Prisões", std::to_string(snap.times_died) + " / " + std::to_string(snap.times_arrested)},
            };

            const float x0 = L.ux(80.0f) + slideOff;
            const float x1 = L.ux(1040.0f) + slideOff;
            const float y0 = topY + L.rf(68.0f);
            const float rowH = std::min(L.rf(54.0f), (L.sh - y0 - L.u(96.0f)) / static_cast<float>(stats.size()));
            const float rowFont = std::min(L.uf(21.0f), rowH * 0.56f);

            for (size_t i = 0; i < stats.size(); ++i)
            {
                const float y = y0 + i * rowH;
                if (i == 0)
                {
                    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + rowH), u32(kSelectBar, fadeA));
                    dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + L.u(5.0f), y + rowH), u32(kCrimson, fadeA));
                }
                else
                {
                    dl->AddLine(ImVec2(x0, y + rowH), ImVec2(x1, y + rowH), u32(kHairline, 0.14f * fadeA), 1.0f);
                }
                const ImU32 col = (i == 0) ? u32(kSelectText, fadeA) : u32(kWhite, fadeA);
                const char *statName = ui::loc::tx(stats[i].first.c_str());
                ImGui::PushFont(i == 0 ? fontSemibold() : fontRegular(), rowFont);
                const ImVec2 ls = ImGui::CalcTextSize(statName);
                const ImVec2 vs = ImGui::CalcTextSize(stats[i].second.c_str());
                dl->AddText(ImVec2(x0 + L.u(18.0f), y + (rowH - ls.y) * 0.5f), col, statName);
                dl->AddText(ImVec2(x1 - vs.x - L.u(18.0f), y + (rowH - vs.y) * 0.5f), col, stats[i].second.c_str());
                ImGui::PopFont();
            }

            drawFooterPrompt(dl, L, L.sw - L.u(56.0f), L.sh - L.u(46.0f), ui::loc::get(ui::loc::Text::HintBack), "ESC");
        }

        // ---- Screen 5: CONFIGURAÇÕES (Reference Images 1-7) --------------------------------------------------------
        void drawTabConfiguracoes(ImDrawList *dl, const Layout &L, bool mouseMoved)
        {
            using namespace palette;
            drawBackdrop(dl, L, g_cfgSubTab == 0 ? -1 : (g_cfgSubTab % 4));
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float slideOff = L.u(g_tabSlide);

            const float topY = L.u(86.0f);
            ImGui::PushFont(fontCondensed(), L.uf(36.0f));
            dl->AddText(ImVec2(L.ux(80.0f) + slideOff, topY), u32(kWhite, fadeA), ui::loc::get(ui::loc::Text::SettingsTitle));
            ImGui::PopFont();

            const int uiLang = lcs::settings::get().game_language;
            const char *kSubTabs[] = {
                uiLang == 0 ? "KEYMAP" : uiLang == 4 ? "CONTROLES" : uiLang == 1 ? "COMMANDES" : uiLang == 2 ? "STEUERUNG" : uiLang == 3 ? "COMANDI" : uiLang == 5 ? "СХЕМА" : uiLang == 6 ? "キー配置" : "ESQUEMA DE CONTROLE",
                ui::loc::get(ui::loc::Text::CatControls),
                uiLang == 0 ? "GAMEPLAY" : uiLang == 4 ? "JUGABILIDAD" : uiLang == 1 ? "GAMEPLAY" : uiLang == 2 ? "GAMEPLAY" : uiLang == 3 ? "GAMEPLAY" : uiLang == 5 ? "ГЕЙМПЛЕЙ" : uiLang == 6 ? "ゲームプレイ" : "JOGABILIDADE",
                uiLang == 0 ? "DISPLAY" : uiLang == 4 ? "PANTALLA" : uiLang == 1 ? "AFFICHAGE" : uiLang == 2 ? "ANZEIGE" : uiLang == 3 ? "SCHERMO" : uiLang == 5 ? "ЭКРАН" : uiLang == 6 ? "画面" : "TELA",
                ui::loc::get(ui::loc::Text::CatDisplay),
                uiLang == 0 ? "INTERFACE" : uiLang == 4 ? "INTERFAZ" : uiLang == 1 ? "INTERFACE" : uiLang == 2 ? "OBERFLÄCHE" : uiLang == 3 ? "INTERFACCIA" : uiLang == 5 ? "ИНТЕРФЕЙС" : uiLang == 6 ? "インターフェース" : "INTERFACE",
                uiLang == 0 ? "CAMERA" : uiLang == 4 ? "CÁMARA" : uiLang == 1 ? "CAMÉRA" : uiLang == 2 ? "KAMERA" : uiLang == 3 ? "TELECAMERA" : uiLang == 5 ? "КАМЕРА" : uiLang == 6 ? "カメラ" : "CÂMERA",
                ui::loc::get(ui::loc::Text::CatAudio),
            };
            constexpr int kSubCount = 8;

            if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                g_cfgSubTab = (g_cfgSubTab - 1 + kSubCount) % kSubCount;
                g_cfgRow = 0;
                triggerViewTransition(-1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || ImGui::IsKeyPressed(ImGuiKey_C, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Tab, false))
            {
                g_cfgSubTab = (g_cfgSubTab + 1) % kSubCount;
                g_cfgRow = 0;
                triggerViewTransition(1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            const int plat = activePromptPreset();
            const bool hasCtrl = isControllerPromptPreset(plat);
            float pillFontSz = L.uf(19.0f);
            float px = L.ux(80.0f) + slideOff;
            const float pillCenterOffset = L.rf(50.0f);
            const float pillY = topY + pillCenterOffset;

            if (hasCtrl)
            {
                const float prevCy = pillY + std::max(L.u(34.0f), pillFontSz + L.u(14.0f)) * 0.5f;
                const float bw = drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "SUB_PREV"), px, prevCy, false);
                px += bw + L.u(10.0f);
            }
            const float availPillW = std::max(200.0f, L.sw - px - (hasCtrl ? L.u(116.0f) : L.u(80.0f)));
            {
                ImGui::PushFont(fontCondensed(), pillFontSz);
                float totW = 0.0f;
                for (int i = 0; i < kSubCount; ++i)
                    totW += ImGui::CalcTextSize(kSubTabs[i]).x + L.u(26.0f) + L.u(8.0f);
                ImGui::PopFont();
                if (totW > availPillW)
                    pillFontSz *= (availPillW / totW);
            }
            const float pillH = std::max(L.u(34.0f), pillFontSz + L.u(14.0f));
            ImGui::PushFont(fontCondensed(), pillFontSz);
            for (int i = 0; i < kSubCount; ++i)
            {
                const ImVec2 ts = ImGui::CalcTextSize(kSubTabs[i]);
                const float pw = ts.x + L.u(24.0f);
                const ImVec2 p0(px, pillY);
                const ImVec2 p1(px + pw, pillY + pillH);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##cfgsub_") + std::to_string(i)).c_str(), ImVec2(pw, pillH)))
                {
                    if (g_cfgSubTab != i)
                    {
                        const float dir = (i > g_cfgSubTab) ? 1.0f : -1.0f;
                        g_cfgSubTab = i;
                        g_cfgRow = 0;
                        triggerViewTransition(dir);
                        playMenuSfx(lcs::game::MenuSfx::Move);
                    }
                }
                const bool active = (i == g_cfgSubTab);
                const bool hov = ImGui::IsItemHovered();
                if (active)
                {
                    dl->AddRectFilled(p0, p1, u32(kSelectBar, fadeA), L.u(3.0f));
                    dl->AddRectFilled(p0, ImVec2(p0.x + L.u(4.0f), p1.y), u32(kCrimson, fadeA), L.u(2.0f));
                }
                else
                {
                    dl->AddRectFilled(p0, p1, u32(kWhite, (hov ? 0.14f : 0.06f) * fadeA), L.u(3.0f));
                    dl->AddRect(p0, p1, u32(kWhite, (hov ? 0.55f : 0.32f) * fadeA), L.u(3.0f), 0, 1.0f);
                }
                dl->AddText(ImVec2(p0.x + (pw - ts.x) * 0.5f, p0.y + (pillH - ts.y) * 0.5f),
                            active ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), kSubTabs[i]);
                px += pw + L.u(8.0f);
            }
            ImGui::PopFont();
            if (hasCtrl)
                drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "SUB_NEXT"), px + L.u(4.0f), pillY + pillH * 0.5f, false);

            const float listTop = pillY + pillH + L.u(30.0f);
            const float listH = L.sh - listTop - L.u(105.0f);

            // Special Sub-Tab 0: ESQUEMA DE CONTROLE (Reference Image 7)
            if (g_cfgSubTab == 0)
            {
                syncSchemeWithActiveDevice(false);

                const float colX0 = L.ux(80.0f) + slideOff;
                const float colX1 = L.ux(580.0f) + slideOff;
                const float divX = L.ux(605.0f) + slideOff;
                dl->AddLine(ImVec2(divX, listTop), ImVec2(divX, listTop + listH), u32(kHairline, 0.20f * fadeA), L.u(2.0f));

                const int prevPreview = g_schemePreview;
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                    g_schemePreview = (g_schemePreview + 3) % 4;
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                    g_schemePreview = (g_schemePreview + 1) % 4;

                int schemeIdx = g_schemePreset;
                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A))
                    schemeIdx = (schemeIdx + 4) % 5;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D))
                    schemeIdx = (schemeIdx + 1) % 5;

                ImGui::PushFont(fontCondensed(), L.uf(24.0f));
                dl->AddText(ImVec2(colX0, listTop), u32(kWhite, fadeA), ui::loc::tx("ESQUEMA DE CONTROLE"));
                ImGui::PopFont();

                const ImVec2 s0(colX0, listTop + L.rf(36.0f));
                const ImVec2 s1(colX1, s0.y + L.rf(54.0f));
                dl->AddRectFilled(s0, s1, u32(kSelectBar, fadeA));
                dl->AddRectFilled(s0, ImVec2(s0.x + L.u(5.0f), s1.y), u32(kCrimson, fadeA));
                const char *kSchemes[5] = {
                    "Xbox", "PlayStation 5", "PlayStation 2", "Nintendo Switch", ui::loc::tx("Teclado & Mouse")};
                if (drawGta6Cycle(dl, L, "##scheme_cyc", ImVec2(s1.x - L.u(14.0f), s0.y), s1.y - s0.y, schemeIdx, kSchemes, 5, true) ||
                    schemeIdx != g_schemePreset)
                {
                    applyManualSchemePreset(schemeIdx);
                    playMenuSfx(lcs::game::MenuSfx::Move);
                }

                ImGui::PushFont(fontCondensed(), L.uf(24.0f));
                dl->AddText(ImVec2(colX0, s1.y + L.rf(38.0f)), u32(kWhite, fadeA), ui::loc::tx("PRÉVIA"));
                ImGui::PopFont();

                static constexpr const char *kPreviews[] = {
                    "A pé", "Mira", "Corpo a corpo", "Veículos terrestres/aquáticos"};
                const float py0 = s1.y + L.rf(78.0f);
                const float prH = L.rf(54.0f);
                for (int i = 0; i < 4; ++i)
                {
                    const ImVec2 p0(colX0, py0 + i * prH);
                    const ImVec2 p1(colX1, p0.y + prH);
                    ImGui::SetCursorScreenPos(p0);
                    if (ImGui::InvisibleButton((std::string("##prev_") + std::to_string(i)).c_str(), ImVec2(p1.x - p0.x, prH)))
                        g_schemePreview = i;
                    if (ImGui::IsItemHovered() && mouseMoved)
                        g_schemePreview = i;
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.14f * fadeA), 1.0f);
                    ImGui::PushFont(fontRegular(), L.uf(21.0f));
                    const char *prevLbl = ui::loc::tx(kPreviews[i]);
                    const ImVec2 ts = ImGui::CalcTextSize(prevLbl);
                    dl->AddText(ImVec2(p0.x + L.u(12.0f), p0.y + (prH - ts.y) * 0.5f), u32(kWhite, fadeA), prevLbl);
                    ImGui::PopFont();

                    // Radio circle
                    const ImVec2 rc(p1.x - L.u(24.0f), p0.y + prH * 0.5f);
                    dl->AddCircle(rc, L.u(11.0f), u32(kWhite, 0.85f * fadeA), 20, L.u(1.8f));
                    if (i == g_schemePreview)
                        dl->AddCircleFilled(rc, L.u(6.0f), u32(kCrimson, fadeA));
                }
                if (g_schemePreview != prevPreview)
                    playMenuSfx(lcs::game::MenuSfx::Move);

                // Right REAL controller PNG from launcher/assets/controllers/ + live callouts
                const float dcx = L.ux(1260.0f) + slideOff;
                const float dcy = listTop + listH * 0.48f;
                ImGui::PushFont(fontCondensed(), L.uf(28.0f));
                const char *curPreviewLbl = ui::loc::tx(kPreviews[g_schemePreview]);
                const ImVec2 pTitle = ImGui::CalcTextSize(curPreviewLbl);
                dl->AddText(ImVec2(dcx - pTitle.x * 0.5f, listTop + L.u(16.0f)), u32(kWhite, fadeA), curPreviewLbl);
                ImGui::PopFont();
                dl->AddLine(ImVec2(dcx, listTop + L.u(56.0f)), ImVec2(dcx, dcy - L.u(130.0f)), u32(kWhite, 0.45f * fadeA), L.u(1.6f));

                const auto &ctrlTex = ui::assets::controllerTexture(g_schemePreset);
                if (ctrlTex.valid())
                {
                    const float cw = L.u(420.0f);
                    const float ch = cw * (static_cast<float>(ctrlTex.h) / static_cast<float>(std::max(1, ctrlTex.w)));
                    dl->AddImage(ctrlTex.id, ImVec2(dcx - cw * 0.5f, dcy - ch * 0.5f),
                                 ImVec2(dcx + cw * 0.5f, dcy + ch * 0.5f),
                                 ImVec2(0, 0), ImVec2(1, 1), u32(0xFFFFFFu, fadeA));
                }

                // Platform-specific AND context-specific callouts (Xbox / PS5 / PS2 / Nintendo Switch / KB+Mouse)
                struct Callout { const char *lbl; const char *key; };
                struct PlatformKeys {
                    const char *map, *aim, *prevWep, *radio, *sprint, *move;
                    const char *pause, *shoot, *enterVeh, *jump, *lookBack, *look;
                    const char *brake, *accel, *handbrake, *horn, *drivebyL, *camera;
                };
                static constexpr PlatformKeys kPlat[5] = {
                    // 0: Xbox
                    {"VIEW", "LT", "LB", "D-PAD", "A", "LS",
                     "MENU", "RT", "Y", "X", "R3", "RS",
                     "LT", "RT", "RB", "L3", "LB", "VIEW"},
                    // 1: PlayStation 5
                    {"TOUCHPAD", "L2", "L1", "D-PAD", "X", "L",
                     "OPTIONS", "R2", "TRIANGLE", "SQUARE", "R3", "R",
                     "L2", "R2", "R1", "L3", "L1", "TOUCHPAD"},
                    // 2: PlayStation 2
                    {"SELECT", "R1", "L2 / R2", "D-PAD", "X", "L",
                     "START", "CIRCLE", "TRIANGLE", "SQUARE", "R3", "R",
                     "SQUARE", "X", "R1", "L3", "L2 / R2", "SELECT"},
                    // 3: Nintendo Switch
                    {"MINUS", "ZL", "L", "D-PAD", "B", "LS",
                     "PLUS", "ZR", "X", "Y", "R3", "RS",
                     "ZL", "ZR", "R", "L3", "L", "MINUS"},
                    // 4: Teclado & Mouse
                    {"M", "RMB", "SCROLL", "R", "SHIFT", "WASD",
                     "ESC", "LMB", "F", "SPACE", "C", "MOUSE",
                     "S", "W", "SPACE", "H", "Q / E", "V"},
                };
                const PlatformKeys &pk = kPlat[std::clamp(g_schemePreset, 0, 4)];
                const int calloutPlat = schemeIndexToPromptPreset(g_schemePreset);

                Callout leftCalls[6];
                Callout rightCalls[6];
                if (g_schemePreview == 3) // Veículos terrestres/aquáticos
                {
                    leftCalls[0]  = {"CÂMERA", pk.camera};
                    leftCalls[1]  = {"FREAR / RÉ", pk.brake};
                    leftCalls[2]  = {"OLHAR LATERAL", pk.drivebyL};
                    leftCalls[3]  = {"ESTAÇÃO DE RÁDIO", pk.radio};
                    leftCalls[4]  = {"BUZINA / SIRENE", pk.horn};
                    leftCalls[5]  = {"DIREÇÃO", pk.move};

                    rightCalls[0] = {"PAUSAR", pk.pause};
                    rightCalls[1] = {"ACELERAR", pk.accel};
                    rightCalls[2] = {"SAIR DO VEÍCULO", pk.enterVeh};
                    rightCalls[3] = {"FREIO DE MÃO", pk.handbrake};
                    rightCalls[4] = {"OLHAR PARA TRÁS", pk.lookBack};
                    rightCalls[5] = {"ATIRAR DO VEÍCULO", pk.shoot};
                }
                else if (g_schemePreview == 1) // Mira
                {
                    leftCalls[0]  = {"MAPA", pk.map};
                    leftCalls[1]  = {"MANTER MIRA (1:1)", pk.aim};
                    leftCalls[2]  = {"TROCAR ARMA", pk.prevWep};
                    leftCalls[3]  = {"AGACHAR", pk.horn};
                    leftCalls[4]  = {"ESQUIVAR / CORRER", pk.sprint};
                    leftCalls[5]  = {"STRAFE (LATERAL)", pk.move};

                    rightCalls[0] = {"PAUSAR", pk.pause};
                    rightCalls[1] = {"DISPARAR", pk.shoot};
                    rightCalls[2] = {"ENTRAR VEÍCULO", pk.enterVeh};
                    rightCalls[3] = {"PULAR", pk.jump};
                    rightCalls[4] = {"OLHAR PARA TRÁS", pk.lookBack};
                    rightCalls[5] = {"MIRAR (PRECISÃO)", pk.look};
                }
                else if (g_schemePreview == 2) // Corpo a corpo
                {
                    leftCalls[0]  = {"MAPA", pk.map};
                    leftCalls[1]  = {"TRAVAR ALVO", pk.aim};
                    leftCalls[2]  = {"TROCAR ARMA", pk.prevWep};
                    leftCalls[3]  = {"AGACHAR", pk.horn};
                    leftCalls[4]  = {"CORRER", pk.sprint};
                    leftCalls[5]  = {"MOVER-SE", pk.move};

                    rightCalls[0] = {"PAUSAR", pk.pause};
                    rightCalls[1] = {"GOLPE / SOCO", pk.shoot};
                    rightCalls[2] = {"ENTRAR VEÍCULO", pk.enterVeh};
                    rightCalls[3] = {"PULAR / ESQUIVAR", pk.jump};
                    rightCalls[4] = {"OLHAR PARA TRÁS", pk.lookBack};
                    rightCalls[5] = {"CÂMERA", pk.look};
                }
                else // A pé
                {
                    leftCalls[0]  = {"MAPA", pk.map};
                    leftCalls[1]  = {"MIRAR (FREE AIM)", pk.aim};
                    leftCalls[2]  = {"TROCAR ARMA", pk.prevWep};
                    leftCalls[3]  = {"RÁDIO", pk.radio};
                    leftCalls[4]  = {"CORRER", pk.sprint};
                    leftCalls[5]  = {"MOVER-SE", pk.move};

                    rightCalls[0] = {"PAUSAR", pk.pause};
                    rightCalls[1] = {"ATIRAR", pk.shoot};
                    rightCalls[2] = {"ENTRAR VEÍCULO", pk.enterVeh};
                    rightCalls[3] = {"PULAR / ESCALAR", pk.jump};
                    rightCalls[4] = {"OLHAR PARA TRÁS", pk.lookBack};
                    rightCalls[5] = {"OLHAR / CÂMERA", pk.look};
                }

                const float calloutFontSz = std::min(L.uf(19.5f), L.u(23.5f));
                for (int i = 0; i < 6; ++i)
                {
                    const float cyRow = listTop + L.u(105.0f) + i * L.u(68.0f);
                    const char *leftTxt = ui::loc::tx(leftCalls[i].lbl);
                    const char *rightTxt = ui::loc::tx(rightCalls[i].lbl);
                    // Left callout: right-aligned Kenney icon(s) + label to its left
                    ImGui::PushFont(fontCondensed(), calloutFontSz);
                    const ImVec2 lls = ImGui::CalcTextSize(leftTxt);
                    ImGui::PopFont();
                    const float lbw = drawPromptCluster(dl, L, calloutPlat, leftCalls[i].key, dcx - L.u(235.0f), cyRow, true);
                    ImGui::PushFont(fontCondensed(), calloutFontSz);
                    dl->AddText(ImVec2(dcx - L.u(235.0f) - lbw - L.u(12.0f) - lls.x, cyRow - lls.y * 0.5f),
                                u32(kWhite, fadeA), leftTxt);
                    ImGui::PopFont();

                    // Right callout: left-aligned Kenney icon(s) + label to its right
                    const float rx0 = dcx + L.u(235.0f);
                    const float bw = drawPromptCluster(dl, L, calloutPlat, rightCalls[i].key, rx0, cyRow, false);
                    ImGui::PushFont(fontCondensed(), calloutFontSz);
                    const ImVec2 ls = ImGui::CalcTextSize(rightTxt);
                    dl->AddText(ImVec2(rx0 + bw + L.u(12.0f), cyRow - ls.y * 0.5f), u32(kWhite, fadeA), rightTxt);
                    ImGui::PopFont();
                }

                float rx = L.sw - L.u(56.0f);
                rx = drawFooterPrompt(dl, L, rx, L.sh - L.u(46.0f), "VOLTAR", "ESC");
                drawFooterPrompt(dl, L, rx, L.sh - L.u(46.0f), "SUB-ABAS", "SUB_BAR");
                return;
            }

            // Standard 2-column Settings layout (Reference Images 1-6)
            const float leftX = L.ux(80.0f) + slideOff;
            const float leftW = L.u(890.0f);
            const float divX = leftX + leftW + L.u(24.0f);
            const float descX = L.ux(1120.0f) + slideOff;
            const float descW = L.ux(1820.0f) - L.ux(1120.0f);

            // Vertical divider / scrollbar track
            dl->AddLine(ImVec2(divX, listTop), ImVec2(divX, listTop + listH), u32(kHairline, 0.22f * fadeA), L.u(2.0f));

            // Keyboard & Controller D-Pad navigation
            const int prevRow = g_cfgRow;
            bool navMoved = false;
            if (g_cfgLastRows > 0)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                {
                    g_cfgRow = (g_cfgRow - 1 + g_cfgLastRows) % g_cfgLastRows;
                    navMoved = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                {
                    g_cfgRow = (g_cfgRow + 1) % g_cfgLastRows;
                    navMoved = true;
                }
                g_cfgRow = std::clamp(g_cfgRow, 0, g_cfgLastRows - 1);
            }
            const int kbStep =
                (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) ? -1 :
                (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) ? 1 : 0;
            const bool kbAct = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);

            ImGui::SetCursorScreenPos(ImVec2(leftX, listTop));
            ImGui::BeginChild("##Gta6CfgList", ImVec2(leftW, listH), false);
            ImDrawList *childDl = ImGui::GetWindowDrawList();
            childDl->ChannelsSplit(2);
            const float prevCfgBarY = g_cfgBarY;

            auto &cfg = lcs::settings::get();
            bool dirty = false;
            int rowIdx = 0;
            const float rowH = L.rf(54.0f);
            std::string activeTitle = ui::loc::tx("CONFIGURAÇÕES");
            std::string activeDesc = ui::loc::tx("Use as setas ou o mouse para ajustar. Alterações são salvas automaticamente.");

            auto section = [&](const char *name) {
                name = ui::loc::tx(name);
                childDl->ChannelsSetCurrent(1);
                if (rowIdx > 0) ImGui::Dummy(ImVec2(leftW, L.rf(18.0f)));
                const ImVec2 p0 = ImGui::GetCursorScreenPos();
                ImGui::PushFont(fontCondensed(), L.uf(25.0f));
                childDl->AddText(ImVec2(p0.x, p0.y), u32(kWhite, fadeA), name);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(leftW, L.rf(36.0f)));
            };

            auto beginRow = [&](const char *label, const char *descTitle, const char *descBody, ImVec2 &outP0, ImVec2 &outP1, bool &outBarOnRow) -> bool {
                label = ui::loc::tx(label);
                descTitle = ui::loc::tx(descTitle);
                descBody = ui::loc::tx(descBody);
                const int idx = rowIdx++;
                outP0 = ImGui::GetCursorScreenPos();
                outP1 = ImVec2(outP0.x + leftW, outP0.y + rowH);
                const bool hov = ImGui::IsMouseHoveringRect(outP0, outP1);
                if (hov && mouseMoved) g_cfgRow = idx;
                const bool sel = (idx == g_cfgRow);
                if (sel)
                {
                    activeTitle = descTitle;
                    activeDesc = descBody;
                    if (navMoved)
                        ImGui::SetScrollHereY(0.5f);
                }

                // Channel 0: hairlines & smooth gliding selection bar
                childDl->ChannelsSetCurrent(0);
                const float winY = ImGui::GetWindowPos().y;
                const float localY = outP0.y - winY;
                const float effectiveBarY = (prevCfgBarY >= 0.0f) ? prevCfgBarY : (sel ? localY : -9999.0f);
                if (sel)
                {
                    if (prevCfgBarY < 0.0f)
                        g_cfgBarY = localY;
                    else
                        glideFloat(g_cfgBarY, localY, 30.0f);
                    const ImVec2 barP0(outP0.x, winY + effectiveBarY);
                    const ImVec2 barP1(outP1.x, barP0.y + rowH);
                    childDl->AddRectFilled(barP0, barP1, u32(kSelectBar, fadeA));
                    childDl->AddRectFilled(barP0, ImVec2(barP0.x + L.u(5.0f), barP1.y), u32(kCrimson, fadeA));
                }
                else
                {
                    childDl->AddLine(ImVec2(outP0.x, outP1.y), outP1, u32(kHairline, 0.14f * fadeA), 1.0f);
                }

                // Channel 1: label text & interactive widgets (dark text tracks exact drawn white bar position)
                const float barMid = (effectiveBarY > -9000.0f) ? (effectiveBarY + rowH * 0.5f) : -9999.0f;
                outBarOnRow = (barMid >= localY && barMid < localY + rowH);
                childDl->ChannelsSetCurrent(1);
                ImGui::PushFont(outBarOnRow ? fontSemibold() : fontRegular(), L.uf(21.0f));
                const ImVec2 ts = ImGui::CalcTextSize(label);
                childDl->AddText(ImVec2(outP0.x + L.u(18.0f), outP0.y + (rowH - ts.y) * 0.5f),
                                 outBarOnRow ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), label);
                ImGui::PopFont();
                return sel;
            };

            auto toggleRow = [&](const char *id, const char *label, int &val, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                bool b = (val != 0);
                if (sel && (kbAct || kbStep != 0)) { b = !b; val = b ? 1 : 0; dirty = true; }
                if (drawGta6Toggle(childDl, L, id, ImVec2(p1.x - L.u(18.0f), p0.y), rowH, b, barOnRow))
                {
                    g_cfgRow = rowIdx - 1;
                    val = b ? 1 : 0;
                    dirty = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto sliderRow = [&](const char *id, const char *label, int &val, int mn, int mx, int step, const char *suf, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                if (sel && kbStep != 0) { val = std::clamp(val + kbStep * step, mn, mx); dirty = true; }
                if (drawGta6Slider(childDl, L, id, ImVec2(p1.x - L.u(18.0f), p0.y), rowH, val, mn, mx, step, suf, barOnRow))
                {
                    g_cfgRow = rowIdx - 1;
                    dirty = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto cycleRow = [&](const char *id, const char *label, int &val, const char *const *opts, const int *vals, int n, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                int idx = 0;
                for (int i = 0; i < n; ++i) if (vals[i] == val) idx = i;
                if (sel && (kbStep != 0 || kbAct))
                {
                    idx = (idx + (kbStep != 0 ? kbStep : 1) + n) % n;
                    val = vals[idx];
                    dirty = true;
                }
                if (drawGta6Cycle(childDl, L, id, ImVec2(p1.x - L.u(14.0f), p0.y), rowH, idx, opts, n, barOnRow))
                {
                    g_cfgRow = rowIdx - 1;
                    val = vals[idx];
                    dirty = true;
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            switch (g_cfgSubTab)
            {
            case 1: // CONTROLE
                section("SENSIBILIDADE DE MIRA E VISÃO");
                sliderRow("##msens", "Sensibilidade do mouse", cfg.mouse_sensitivity, 10, 400, 5, "%",
                          "SENSIBILIDADE DO MOUSE", "Velocidade da câmera e mira 1:1 ao mover o mouse.");
                toggleRow("##minv", "Inverter eixo Y do mouse", cfg.mouse_invert_y,
                          "INVERTER EIXO Y", "Inverte o movimento vertical da câmera ao olhar para cima e para baixo.");
                section("CONTROLE (XINPUT / DUALSENSE)");
                sliderRow("##vib", "Vibração do controle", cfg.pad_vibration, 0, 200, 10, "%",
                          "VIBRAÇÃO", "Intensidade da vibração háptica em controles Xbox e DualSense.");
                break;

            case 2: // JOGABILIDADE
                section("COMBATE & MIRA");
                toggleRow("##mfa", "Mira livre 1:1 com mouse", cfg.mouse_free_aim,
                          "MIRA LIVRE 1:1", "Ativa a mira direta de precisão com o mouse a pé (lcs_free_aim). Desative para trava clássica.");
                section("INICIALIZAÇÃO");
                toggleRow("##skip", "Pular vídeos de introdução", cfg.skip_intro,
                          "PULAR INTRODUÇÃO", "Inicia o jogo diretamente na tela principal sem exibir os vídeos de abertura.");
                break;

            case 3: // TELA
            {
                section("TELA");
                static const char *kWinO[] = {"Tela cheia sem borda", "Janela"};
                static const int kWinV[] = {0, 1};
                cycleRow("##win", "Modo de exibição", cfg.display_window, kWinO, kWinV, 2,
                         "MODO DE EXIBIÇÃO", "Alterna instantaneamente entre Tela Cheia sem borda (Alt+Enter) e Janela.");
                static const char *kFpsO[] = {"30 FPS", "60 FPS", "120 FPS", "144 FPS", "240 FPS", "Ilimitado"};
                static const int kFpsV[] = {30, 60, 120, 144, 240, 0};
                cycleRow("##fps", "Limite de FPS", cfg.display_fps, kFpsO, kFpsV, 6,
                         "LIMITE DE FPS", "Define a taxa máxima de quadros por segundo do jogo em tempo real.");
                toggleRow("##vsy", "Sincronização vertical (V-Sync)", cfg.display_vsync,
                          "SINCRONIZAÇÃO VERTICAL", "Sincroniza a apresentação de quadros com o monitor para evitar screen tearing.");
                section("CALIBRAGEM DE COR (IPAD RETINA)");
                toggleRow("##cg", "Color Grading iPad Retina", cfg.color_grading,
                          "COLOR GRADING IPAD", "Aplica a calibração de contraste e cores aprimorada dos shaders de iPad.");
                sliderRow("##bri", "Calibragem do brilho", cfg.brightness, 0, 100, 2, "%",
                          "CALIBRAGEM DO BRILHO", "Ajusta o brilho geral da imagem em tempo real.");
                sliderRow("##con", "Contraste", cfg.contrast, 0, 100, 2, "%",
                          "CONTRASTE", "Ajusta o contraste entre sombras noturnas e luzes de Liberty City.");
                sliderRow("##sat", "Saturação", cfg.saturation, 0, 100, 2, "%",
                          "SATURAÇÃO", "Ajusta a intensidade das cores da cena.");
                break;
            }
            case 4: // GRÁFICOS & MUNDO
            {
                section("RENDERIZADOR & QUALIDADE");
                static const char *kRenO[] = {"Direct3D 12", "Vulkan 1.4"};
                static const int kRenV[] = {0, 1};
                cycleRow("##ren", "API Gráfica (Reiniciar)", cfg.display_renderer, kRenO, kRenV, 2,
                         "API GRÁFICA", "Seleciona Direct3D 12 ou Vulkan 1.4 para a próxima inicialização.");
                static const char *kMsaaO[] = {"Desligado", "2x MSAA", "4x MSAA", "8x MSAA"};
                static const int kMsaaV[] = {1, 2, 4, 8};
                cycleRow("##msaa", "Antisserrilhamento (MSAA)", cfg.display_msaa, kMsaaO, kMsaaV, 4,
                         "ANTISSERRILHAMENTO", "Suaviza bordas serrilhadas em veículos, prédios e postes.");
                static const char *kAniO[] = {"Desligado", "2x", "4x", "8x", "16x"};
                static const int kAniV[] = {0, 2, 4, 8, 16};
                cycleRow("##ani", "Filtro anisotrópico", cfg.display_aniso, kAniO, kAniV, 5,
                         "FILTRO ANISOTRÓPICO", "Mantém asfalto e fachadas nítidos em ângulos inclinados.");
                toggleRow("##sdw", "Sombras dinâmicas iPad (_SDWMAP)", cfg.ipad_shadows,
                          "SOMBRAS DINÂMICAS", "Ativa o mapa de sombras em tempo real portado da versão iPad Retina.");

                section("MUNDO & DISTÂNCIAS (PC)");
                sliderRow("##vd", "Distância de visão", cfg.view_distance, 0, 100, 5, "%",
                          "DISTÂNCIA DE VISÃO", "Controla o alcance de renderização de edifícios e veículos em Liberty City.");
                sliderRow("##fd", "Distância da neblina", cfg.fog_distance, 10, 100, 5, "%",
                          "DISTÂNCIA DA NEBLINA", "Controla onde a neblina atmosférica encerra a visibilidade sem cortar os prédios distantes.");
                sliderRow("##fs", "Início da neblina", cfg.fog_start, 0, 300, 5, "%",
                          "INÍCIO DA NEBLINA", "Define a distância em que a neblina atmosférica começa a surgir no horizonte.");
                static const char *kHdRadO[] = {
                    "Original (Desligado)",
                    "Próximo (Anel 1 — 7 células)",
                    "Estendido (Anel 2 — 19 células)",
                    "Horizonte Total (Anel 3 — 37 células)"
                };
                static const int kHdRadV[] = {0, 1, 2, 3};
                cycleRow("##hdrad", "Raio de cidade HD", cfg.world_hd_radius, kHdRadO, kHdRadV, 4,
                         "RAIO DE CIDADE HD", "Mantém múltiplas células hexadecimais de geometria HD carregadas ao redor da câmera via Models/<ilha>.img.");
                sliderRow("##dd", "Distância de detalhe", cfg.detail_distance, 0, 100, 5, "%",
                          "DISTÂNCIA DE DETALHE", "Controla a distância em que edifícios de alta definição substituem os modelos distantes.");
                sliderRow("##pd", "Distância da população", cfg.population_distance, 0, 100, 5, "%",
                          "DISTÂNCIA DA POPULAÇÃO", "Amplia o raio de surgimento e visibilidade de pedestres e veículos ao redor do jogador.");
                sliderRow("##td", "Densidade de tráfego", cfg.traffic_density, 0, 100, 5, "%",
                          "DENSIDADE DE TRÁFEGO", "Ajusta a quantidade de carros e motos circulando pelas ruas de Liberty City.");
                sliderRow("##pedd", "Densidade de pedestres", cfg.ped_density, 0, 100, 5, "%",
                          "DENSIDADE DE PEDESTRES", "Ajusta a quantidade de pedestres caminhando pelas calçadas de Liberty City.");
                static const char *kSmemO[] = {
                    "64 MB (Mobile)",
                    "128 MB (PC Padrão)",
                    "256 MB (Alto)",
                    "384 MB (Ultra)",
                    "512 MB (Máximo)"
                };
                static const int kSmemV[] = {64, 128, 256, 384, 512};
                cycleRow("##smem", "Memória de streaming", cfg.streaming_memory, kSmemO, kSmemV, 5,
                         "MEMÓRIA DE STREAMING", "Reserva memória RAM adicional para manter modelos e texturas do mapa carregados sem engasgos.");

                section("PIPELINE GRÁFICO PLAYSTATION 2 (SLUS_214.23)");
                toggleRow("##ps2_pipe", "Pipeline de Efeitos PS2 (CMBlur + GS)", cfg.ps2_pipeline,
                          "PIPELINE DE EFEITOS PS2", "Ativa o pipeline pós-processado original do PlayStation 2 (SLUS_214.23) combinado com o Recomp (Radiosity, Filtro BlurRGB e Trails).");
                static const char *kPs2TcO[] = {"Recomp (Padrão PC)", "PS2 Original (SLUS_214.23)", "Híbrido (PS2 + Recomp)"};
                static const int kPs2TcV[] = {0, 1, 2};
                cycleRow("##ps2_tc", "Modo Visual & TimeCycle", cfg.ps2_timecycle, kPs2TcO, kPs2TcV, 3,
                         "MODO VISUAL (RECOMP / PS2 / HÍBRIDO)", "Seleciona especificamente o visual Recomp (Padrão PC), o pipeline 100% PS2 Original ou o modo Híbrido.");
                toggleRow("##ps2_rad", "Radiosidade & Bloom PS2 (0x001d17d8)", cfg.ps2_radiosity,
                          "RADIOSIDADE & BLOOM PS2", "Replica o filtro box 4-tap sub-pixel (offset 0.188) e o passe aditivo de radiosidade/glow do PS2.");
                sliderRow("##ps2_radi", "Intensidade da Radiosidade PS2", cfg.ps2_radiosity_intensity, 0, 200, 10, "%",
                          "INTENSIDADE DA RADIOSIDADE", "Controla a força do halo de luz e radiosidade atmosférica do PS2 sobre o céu, faróis e superfícies claras.");
                toggleRow("##ps2_cf", "Filtro de Cor Atmosférico PS2 (BlurRGB)", cfg.ps2_color_filter,
                          "FILTRO DE COR PS2 (BLURRGB)", "Aplica a modulação cromática (255 - FinalColour.rgb) das tabelas BlurRed/Green/Blue do GAME.DTZ.");
                sliderRow("##ps2_cfi", "Intensidade do Filtro de Cor PS2", cfg.ps2_color_filter_intensity, 0, 200, 10, "%",
                          "INTENSIDADE DO FILTRO DE COR", "Ajusta o equilíbrio de contraste quente (amanhecer/entardecer/noite) do filtro de cor do PS2.");
                toggleRow("##ps2_tr", "Trails do PS2 (Persistência Temporal)", cfg.ps2_trails,
                          "TRAILS DO PS2 (0x00272348)", "Replica o acumulador temporal de quadros do PS2 (gMinBlurAmt = 28) com decaimento corrigido para PC/60+ FPS.");
                sliderRow("##ps2_tri", "Intensidade dos Trails PS2", cfg.ps2_trails_intensity, 0, 200, 10, "%",
                          "INTENSIDADE DOS TRAILS", "Controla a persistência temporal dos rastros de luz e movimento (100% = calibração original do PS2).");
                toggleRow("##ps2_spd", "Motion Blur de Alta Velocidade PS2", cfg.ps2_speed_blur,
                          "MOTION BLUR DE VELOCIDADE", "Adiciona desfoque radial periférico progressivo ao dirigir veículos em alta velocidade.");
                break;
            }
            case 5: // INTERFACE
            {
                section(ui::loc::get(ui::loc::Text::CatLanguage));
                static const char *kLangO[] = {
                    "English", "Français", "Deutsch", "Italiano", "Español", "Русский", "日本語", "Português (Brasil)"
                };
                static const int kLangV[] = {0, 1, 2, 3, 4, 5, 6, 7};
                const int prevLang = cfg.game_language;
                cycleRow("##lang", ui::loc::get(ui::loc::Text::OptLanguageLabel), cfg.game_language, kLangO, kLangV, 8,
                         ui::loc::get(ui::loc::Text::OptLanguageLabel), ui::loc::get(ui::loc::Text::OptLanguageDesc));
                if (cfg.game_language != prevLang)
                {
                    lcs::settings::save();
                    lcs::game::LCSTrainer::instance().set_game_language(cfg.game_language);
                }

                section(ui::loc::get(ui::loc::Text::CatSubtitles));
                const char *kSubFontO[] = {
                    ui::loc::get(ui::loc::Text::OptSubtitleFontOriginal),
                    ui::loc::get(ui::loc::Text::OptSubtitleFontOswald),
                };
                static const int kSubFontV[] = {0, 1};
                cycleRow("##sub_fnt", ui::loc::get(ui::loc::Text::OptSubtitleFont), cfg.subtitle_font, kSubFontO, kSubFontV, 2,
                         ui::loc::get(ui::loc::Text::OptSubtitleFont), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                sliderRow("##sub_out", ui::loc::get(ui::loc::Text::OptSubtitleOutline), cfg.subtitle_outline, 0, 100, 10, "%",
                          ui::loc::get(ui::loc::Text::OptSubtitleOutline), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                sliderRow("##sub_sz", ui::loc::get(ui::loc::Text::OptSubtitleSize), cfg.subtitle_size, 80, 150, 5, "%",
                          ui::loc::get(ui::loc::Text::OptSubtitleSize), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                toggleRow("##sub_sh", ui::loc::get(ui::loc::Text::OptSubtitleShadow), cfg.subtitle_shadow,
                          ui::loc::get(ui::loc::Text::OptSubtitleShadow), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                const char *kSubBgO[] = {
                    ui::loc::get(ui::loc::Text::OptSubtitleBgOff),
                    ui::loc::get(ui::loc::Text::OptSubtitleBgLight),
                    ui::loc::get(ui::loc::Text::OptSubtitleBgMed),
                    ui::loc::get(ui::loc::Text::OptSubtitleBgDark),
                };
                static const int kSubBgV[] = {0, 25, 50, 75};
                cycleRow("##sub_bg", ui::loc::get(ui::loc::Text::OptSubtitleBg), cfg.subtitle_background, kSubBgO, kSubBgV, 4,
                         ui::loc::get(ui::loc::Text::OptSubtitleBg), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                const char *kSubColO[] = {
                    ui::loc::get(ui::loc::Text::OptSubtitleColourGame),
                    ui::loc::get(ui::loc::Text::OptSubtitleColourWhite),
                };
                static const int kSubColV[] = {0, 1};
                cycleRow("##sub_col", ui::loc::get(ui::loc::Text::OptSubtitleColour), cfg.subtitle_colour, kSubColO, kSubColV, 2,
                         ui::loc::get(ui::loc::Text::OptSubtitleColour), ui::loc::get(ui::loc::Text::OptSubtitleDesc));
                section("TAMANHO E LEGIBILIDADE");
                static const char *kFntO[] = {"Compacta (90%)", "Padrão (100%)", "Grande (110%)", "Extra Grande (120%)", "Máxima (130%)", "Acessibilidade (140%)"};
                static const int kFntV[] = {90, 100, 110, 120, 130, 140};
                cycleRow("##fnt", "Tamanho das fontes", cfg.ui_font_scale, kFntO, kFntV, 6,
                         "TAMANHO DAS FONTES", "Acessibilidade: ajusta a escala de textos, listas e avisos em todos os menus.");
                static const char *kAspO[] = {"Automático", "16:9", "21:9", "4:3"};
                static const int kAspV[] = {0, 169, 219, 43};
                cycleRow("##asp", "Proporção da tela", cfg.display_aspect, kAspO, kAspV, 4,
                         "PROPORÇÃO DA TELA", "Define a proporção de exibição do viewport.");
                static const char *kHudO[] = {"Esticado", "Centralizado 4:3", "Widescreen"};
                static const int kHudV[] = {0, 1, 2};
                cycleRow("##hud", "Layout do HUD", cfg.display_hud, kHudO, kHudV, 3,
                         "LAYOUT DO HUD", "Widescreen mantém radar, dinheiro e armas perfeitamente proporcionais.");
                static const char *kPrmO[] = {"Automático", "PlayStation", "Xbox", "Teclado"};
                static const int kPrmV[] = {1, 2, 3, 4};
                cycleRow("##prm", "Ícones de botões", cfg.button_prompts, kPrmO, kPrmV, 4,
                         "ÍCONES DE BOTÕES", "Escolhe qual família de ícones de botão aparece na interface.");
                static const char *kCntO[] = {"Desligado", "Simples", "Detalhado"};
                static const int kCntV[] = {0, 1, 2};
                cycleRow("##fpsc", "Contador de FPS", cfg.display_fps_counter, kCntO, kCntV, 3,
                         "CONTADOR DE FPS", "Exibe a taxa de quadros e tempo de frame no canto superior direito.");
                break;
            }
            case 6: // CÂMERA
                section("CÂMERA LIVRE (F6)");
                sliderRow("##fcs", "Velocidade da câmera livre", cfg.free_cam_speed, 25, 400, 25, "%",
                          "VELOCIDADE DA CÂMERA LIVRE", "Multiplicador de velocidade de deslocamento quando a Câmera Livre (F6) está ativa.");
                break;

            case 7: // ÁUDIO
                section("VOLUME");
                sliderRow("##vol", "Volume geral", cfg.audio_volume, 0, 100, 5, "%",
                          "VOLUME GERAL", "Controla o volume mestre da saída OpenAL.");
                sliderRow("##mus", "Música e estações de rádio", cfg.audio_music, 0, 100, 5, "%",
                          "MÚSICA E RÁDIO", "Controla o volume das rádios e cutscenes de data_music.wad.");
                sliderRow("##sfx", "Efeitos sonoros", cfg.audio_sfx, 0, 100, 5, "%",
                          "EFEITOS SONOROS", "Controla o volume de tiros, motores, sirenes e diálogos.");
                break;
            }

            // Required by Dear ImGui 1.92+ after SetCursorScreenPos() to commit content bounds with zero warnings!
            childDl->ChannelsMerge();
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            g_cfgLastRows = rowIdx;
            if (g_cfgRow != prevRow)
                playMenuSfx(lcs::game::MenuSfx::Move);
            if (dirty)
            {
                playMenuSfx(lcs::game::MenuSfx::Move);
                persistAllSettings();
            }
            ImGui::EndChild();

            // Restore defaults hotkey (X)
            if (ImGui::IsKeyPressed(ImGuiKey_X, false))
            {
                cfg = lcs::settings::RuntimeSettings{};
                persistAllSettings();
                playMenuSfx(lcs::game::MenuSfx::Confirm);
                lcs::game::LCSTrainer::instance().push_notice(ui::loc::tx("Configuracoes restauradas para o padrao"), 1);
            }

            // Bottom-left row counter ("1 / 7")
            if (g_cfgLastRows > 0)
            {
                char cntBuf[32];
                std::snprintf(cntBuf, sizeof(cntBuf), "%d / %d", std::clamp(g_cfgRow + 1, 1, g_cfgLastRows), g_cfgLastRows);
                ImGui::PushFont(fontSemibold(), L.uf(20.0f));
                dl->AddText(ImVec2(leftX, L.sh - L.u(74.0f)), u32(kDim, fadeA), cntBuf);
                ImGui::PopFont();
            }

            // Right contextual description column (Reference Images 1-6)
            ImGui::PushFont(fontCondensed(), L.uf(28.0f));
            dl->AddText(ImVec2(descX, listTop), u32(kWhite, fadeA), activeTitle.c_str());
            ImGui::PopFont();
            dl->AddLine(ImVec2(descX, listTop + L.rf(42.0f)), ImVec2(descX + descW, listTop + L.rf(42.0f)),
                        u32(kHairline, 0.25f * fadeA), 1.0f);
            ImGui::PushFont(fontRegular(), L.uf(21.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(descX, listTop + L.rf(62.0f)),
                        u32(kDim, fadeA), activeDesc.c_str(), nullptr, descW);
            ImGui::PopFont();

            // Global bottom-right footer prompts
            float rx = L.sw - L.u(56.0f);
            const float fy = L.sh - L.u(46.0f);
            rx = drawFooterPrompt(dl, L, rx, fy, "VOLTAR", "ESC");
            rx = drawFooterPrompt(dl, L, rx, fy, "RESTAURAR PADRÃO", "X");
            drawFooterPrompt(dl, L, rx, fy, "SUB-ABAS", "SUB_BAR");
        }

        // ---- Screen 6: MODS ----------------------------------------------------------------------------------------
        void drawTabMods(ImDrawList *dl, const Layout &L, bool mouseMoved)
        {
            using namespace palette;
            drawBackdrop(dl, L, 3);
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float slideOff = L.u(g_tabSlide);

            const float topY = L.u(100.0f);
            ImGui::PushFont(fontCondensed(), L.uf(36.0f));
            dl->AddText(ImVec2(L.ux(80.0f) + slideOff, topY), u32(kWhite, fadeA), ui::loc::get(ui::loc::Text::ModsTitle));
            ImGui::PopFont();

            auto &cfg = lcs::settings::get();
            bool dirty = false;
            const float x0 = L.ux(80.0f) + slideOff;
            const float leftW = L.u(860.0f);
            const float y0 = topY + L.rf(64.0f);
            const float rowH = L.rf(56.0f);

            const int prevRow = g_modsRow;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W)) g_modsRow = (g_modsRow + 1) % 2;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S)) g_modsRow = (g_modsRow + 1) % 2;
            const bool kbAct = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow);

            struct ModOpt { const char *id; const char *lbl; int *val; };
            ModOpt opts[2] = {
                {"##m_en", "Ativar Mod Loader (modloader/)", &cfg.mods_enabled},
                {"##m_tx", "Substituição de Texturas HD (.png / .dds)", &cfg.texture_replacements},
            };

            // Handle hover & glide bar behind text
            for (int i = 0; i < 2; ++i)
            {
                const ImVec2 p0(x0, y0 + i * rowH);
                const ImVec2 p1(x0 + leftW, p0.y + rowH);
                if (ImGui::IsMouseHoveringRect(p0, p1) && mouseMoved) g_modsRow = i;
            }
            glideFloat(g_modsBarY, y0 + g_modsRow * rowH, 30.0f);
            dl->AddRectFilled(ImVec2(x0, g_modsBarY), ImVec2(x0 + leftW, g_modsBarY + rowH), u32(kSelectBar, fadeA));
            dl->AddRectFilled(ImVec2(x0, g_modsBarY), ImVec2(x0 + L.u(5.0f), g_modsBarY + rowH), u32(kCrimson, fadeA));

            const float modsBarMid = g_modsBarY + rowH * 0.5f;
            for (int i = 0; i < 2; ++i)
            {
                const ImVec2 p0(x0, y0 + i * rowH);
                const ImVec2 p1(x0 + leftW, p0.y + rowH);
                const bool sel = (i == g_modsRow);
                const bool barOnRow = (modsBarMid >= p0.y && modsBarMid < p1.y);
                if (!sel)
                    dl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.14f * fadeA), 1.0f);
                ImGui::PushFont(barOnRow ? fontSemibold() : fontRegular(), L.uf(21.0f));
                const char *modLbl = ui::loc::tx(opts[i].lbl);
                const ImVec2 ts = ImGui::CalcTextSize(modLbl);
                dl->AddText(ImVec2(p0.x + L.u(18.0f), p0.y + (rowH - ts.y) * 0.5f),
                            barOnRow ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), modLbl);
                ImGui::PopFont();

                bool b = (*opts[i].val != 0);
                if (sel && kbAct) { b = !b; *opts[i].val = b ? 1 : 0; dirty = true; }
                if (drawGta6Toggle(dl, L, opts[i].id, ImVec2(p1.x - L.u(18.0f), p0.y), rowH, b, barOnRow))
                {
                    g_modsRow = i;
                    *opts[i].val = b ? 1 : 0;
                    dirty = true;
                }
            }
            if (g_modsRow != prevRow)
                playMenuSfx(lcs::game::MenuSfx::Move);
            if (dirty)
            {
                playMenuSfx(lcs::game::MenuSfx::Move);
                persistAllSettings();
            }

            // Right column: Live LogicalFS stats & detected files in modloader/
            const float rx0 = L.ux(1060.0f) + slideOff;
            ImGui::PushFont(fontCondensed(), L.uf(26.0f));
            dl->AddText(ImVec2(rx0, y0), u32(kWhite, fadeA), ui::loc::tx("STATUS DO SISTEMA DE ARQUIVOS"));
            ImGui::PopFont();
            dl->AddLine(ImVec2(rx0, y0 + L.rf(38.0f)), ImVec2(L.ux(1820.0f) + slideOff, y0 + L.rf(38.0f)),
                        u32(kHairline, 0.22f * fadeA), 1.0f);

            char fsBuf[320];
            std::snprintf(fsBuf, sizeof(fsBuf),
                          ui::loc::tx("Assets carregados de modloader/: %llu\nAssets carregados de iPad Retina: %llu\n\nColoque arquivos .wad, .txd, .dff, .gxt, .dat, .png ou .dds na pasta modloader/."),
                          static_cast<unsigned long long>(lcs::vfs::LogicalFS::instance().mod_hit_count()),
                          static_cast<unsigned long long>(lcs::vfs::LogicalFS::instance().ipad_hit_count()));
            ImGui::PushFont(fontRegular(), L.uf(20.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(rx0, y0 + L.rf(54.0f)), u32(kDim, fadeA), fsBuf, nullptr, L.u(740.0f));
            ImGui::PopFont();

            drawFooterPrompt(dl, L, L.sw - L.u(56.0f), L.sh - L.u(46.0f), "VOLTAR", "ESC");
        }

        // ---- Screen 7: TRAINER (Unified Ex-F5 in GTA 6 x LCS Style) ------------------------------------------------
        void drawTabTrainer(ImDrawList *dl, const Layout &L, bool mouseMoved)
        {
            using namespace palette;
            drawBackdrop(dl, L, g_trainerCat % 4);
            const float fadeA = std::clamp(g_tabFade, 0.0f, 1.0f);
            const float slideOff = L.u(g_tabSlide);

            const float topY = L.u(86.0f);
            ImGui::PushFont(fontCondensed(), L.uf(36.0f));
            dl->AddText(ImVec2(L.ux(80.0f) + slideOff, topY), u32(kWhite, fadeA), "LCS TRAINER");
            ImGui::PopFont();

            static constexpr const char *kCats[] = {"JOGADOR", "ARMAS", "VEÍCULOS", "TELEPORTE", "CÂMERA", "MUNDO"};
            constexpr int kCatCount = 6;
            if (ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false))
            {
                g_trainerCat = (g_trainerCat - 1 + kCatCount) % kCatCount;
                g_trainerRow = 0;
                triggerViewTransition(-1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || ImGui::IsKeyPressed(ImGuiKey_C, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Tab, false))
            {
                g_trainerCat = (g_trainerCat + 1) % kCatCount;
                g_trainerRow = 0;
                triggerViewTransition(1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }

            const int plat = activePromptPreset();
            const bool hasCtrl = isControllerPromptPreset(plat);
            float pillFontSz = L.uf(19.0f);
            float px = L.ux(80.0f) + slideOff;
            const float pillY = topY + L.rf(50.0f);

            if (hasCtrl)
            {
                const float prevCy = pillY + std::max(L.u(34.0f), pillFontSz + L.u(14.0f)) * 0.5f;
                const float bw = drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "SUB_PREV"), px, prevCy, false);
                px += bw + L.u(10.0f);
            }
            const float availPillW = std::max(200.0f, L.sw - px - (hasCtrl ? L.u(116.0f) : L.u(80.0f)));
            {
                ImGui::PushFont(fontCondensed(), pillFontSz);
                float totW = 0.0f;
                for (int i = 0; i < kCatCount; ++i)
                    totW += ImGui::CalcTextSize(ui::loc::tx(kCats[i])).x + L.u(28.0f) + L.u(8.0f);
                ImGui::PopFont();
                if (totW > availPillW)
                    pillFontSz *= (availPillW / totW);
            }
            const float pillH = std::max(L.u(34.0f), pillFontSz + L.u(14.0f));
            ImGui::PushFont(fontCondensed(), pillFontSz);
            for (int i = 0; i < kCatCount; ++i)
            {
                const char *catLbl = ui::loc::tx(kCats[i]);
                const ImVec2 ts = ImGui::CalcTextSize(catLbl);
                const float pw = ts.x + L.u(28.0f);
                const ImVec2 p0(px, pillY);
                const ImVec2 p1(px + pw, pillY + pillH);
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##trcat_") + std::to_string(i)).c_str(), ImVec2(pw, pillH)))
                {
                    if (g_trainerCat != i)
                    {
                        const float dir = (i > g_trainerCat) ? 1.0f : -1.0f;
                        g_trainerCat = i;
                        g_trainerRow = 0;
                        triggerViewTransition(dir);
                        playMenuSfx(lcs::game::MenuSfx::Move);
                    }
                }
                const bool active = (i == g_trainerCat);
                if (active)
                {
                    dl->AddRectFilled(p0, p1, u32(kSelectBar, fadeA), L.u(3.0f));
                    dl->AddRectFilled(p0, ImVec2(p0.x + L.u(4.0f), p1.y), u32(kCrimson, fadeA), L.u(2.0f));
                }
                else
                {
                    dl->AddRectFilled(p0, p1, u32(kWhite, (ImGui::IsItemHovered() ? 0.14f : 0.06f) * fadeA), L.u(3.0f));
                    dl->AddRect(p0, p1, u32(kWhite, 0.32f * fadeA), L.u(3.0f), 0, 1.0f);
                }
                dl->AddText(ImVec2(p0.x + (pw - ts.x) * 0.5f, p0.y + (pillH - ts.y) * 0.5f),
                            active ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), catLbl);
                px += pw + L.u(8.0f);
            }
            ImGui::PopFont();
            if (hasCtrl)
                drawPromptCluster(dl, L, plat, deviceTokenForAction(plat, "SUB_NEXT"), px + L.u(4.0f), pillY + pillH * 0.5f, false);

            const float listTop = pillY + pillH + L.u(30.0f);
            const float listH = L.sh - listTop - L.u(105.0f);
            const float leftX = L.ux(80.0f) + slideOff;
            const float leftW = L.u(890.0f);
            const float divX = leftX + leftW + L.u(24.0f);
            const float descX = L.ux(1120.0f) + slideOff;
            const float descW = L.ux(1820.0f) - L.ux(1120.0f);

            dl->AddLine(ImVec2(divX, listTop), ImVec2(divX, listTop + listH), u32(kHairline, 0.22f * fadeA), L.u(2.0f));

            const int prevRow = g_trainerRow;
            bool navMoved = false;
            if (g_trainerLastRows > 0)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_W))
                {
                    g_trainerRow = (g_trainerRow - 1 + g_trainerLastRows) % g_trainerLastRows;
                    navMoved = true;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_S))
                {
                    g_trainerRow = (g_trainerRow + 1) % g_trainerLastRows;
                    navMoved = true;
                }
                g_trainerRow = std::clamp(g_trainerRow, 0, g_trainerLastRows - 1);
            }
            const int kbStep =
                (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_A)) ? -1 :
                (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_D)) ? 1 : 0;
            const bool kbAct = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);

            ImGui::SetCursorScreenPos(ImVec2(leftX, listTop));
            ImGui::BeginChild("##Gta6TrainerList", ImVec2(leftW, listH), false);
            ImDrawList *childDl = ImGui::GetWindowDrawList();
            childDl->ChannelsSplit(2);
            const float prevTrainerBarY = g_trainerBarY;

            auto &tr = lcs::game::LCSTrainer::instance();
            int rowIdx = 0;
            const float rowH = L.rf(54.0f);
            std::string activeTitle = "LCS TRAINER";
            std::string activeDesc = ui::loc::tx("Cheats executados diretamente na memória ARM64 de libGame.so.");

            auto beginRow = [&](const char *label, const char *dTitle, const char *dBody, ImVec2 &p0, ImVec2 &p1, bool &outBarOnRow) -> bool {
                label = ui::loc::tx(label);
                dTitle = ui::loc::tx(dTitle);
                dBody = ui::loc::tx(dBody);
                const int idx = rowIdx++;
                p0 = ImGui::GetCursorScreenPos();
                p1 = ImVec2(p0.x + leftW, p0.y + rowH);
                if (ImGui::IsMouseHoveringRect(p0, p1) && mouseMoved) g_trainerRow = idx;
                const bool sel = (idx == g_trainerRow);
                if (sel)
                {
                    activeTitle = dTitle;
                    activeDesc = dBody;
                    if (navMoved)
                        ImGui::SetScrollHereY(0.5f);
                }
                childDl->ChannelsSetCurrent(0);
                const float winY = ImGui::GetWindowPos().y;
                const float localY = p0.y - winY;
                const float effectiveBarY = (prevTrainerBarY >= 0.0f) ? prevTrainerBarY : (sel ? localY : -9999.0f);
                if (sel)
                {
                    if (prevTrainerBarY < 0.0f)
                        g_trainerBarY = localY;
                    else
                        glideFloat(g_trainerBarY, localY, 30.0f);
                    const ImVec2 barP0(p0.x, winY + effectiveBarY);
                    const ImVec2 barP1(p1.x, barP0.y + rowH);
                    childDl->AddRectFilled(barP0, barP1, u32(kSelectBar, fadeA));
                    childDl->AddRectFilled(barP0, ImVec2(barP0.x + L.u(5.0f), barP1.y), u32(kCrimson, fadeA));
                }
                else
                {
                    childDl->AddLine(ImVec2(p0.x, p1.y), p1, u32(kHairline, 0.14f * fadeA), 1.0f);
                }
                const float barMid = (effectiveBarY > -9000.0f) ? (effectiveBarY + rowH * 0.5f) : -9999.0f;
                outBarOnRow = (barMid >= localY && barMid < localY + rowH);
                childDl->ChannelsSetCurrent(1);
                ImGui::PushFont(outBarOnRow ? fontSemibold() : fontRegular(), L.uf(21.0f));
                const ImVec2 ts = ImGui::CalcTextSize(label);
                childDl->AddText(ImVec2(p0.x + L.u(18.0f), p0.y + (rowH - ts.y) * 0.5f),
                                 outBarOnRow ? u32(kSelectText, fadeA) : u32(kWhite, fadeA), label);
                ImGui::PopFont();
                return sel;
            };

            auto trToggle = [&](const char *id, const char *label, bool &flag, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                const char *locLbl = ui::loc::tx(label);
                if (sel && (kbAct || kbStep != 0))
                {
                    flag = !flag;
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    tr.push_notice(std::string(locLbl) + ui::loc::tx(flag ? ": ON" : ": OFF"));
                }
                if (drawGta6Toggle(childDl, L, id, ImVec2(p1.x - L.u(18.0f), p0.y), rowH, flag, barOnRow))
                {
                    g_trainerRow = rowIdx - 1;
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    tr.push_notice(std::string(locLbl) + ui::loc::tx(flag ? ": ON" : ": OFF"));
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto trAction = [&](const char *id, const char *label, const char *badge, std::function<void()> fn, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                badge = ui::loc::tx(badge);
                ImGui::SetCursorScreenPos(p0);
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(leftW, rowH));
                if (clicked || (sel && kbAct))
                {
                    g_trainerRow = rowIdx - 1;
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    fn();
                }
                ImGui::PushFont(fontCondensed(), L.uf(19.0f));
                const ImVec2 bs = ImGui::CalcTextSize(badge);
                childDl->AddText(ImVec2(p1.x - bs.x - L.u(20.0f), p0.y + (rowH - bs.y) * 0.5f),
                                 barOnRow ? u32(kCrimson, fadeA) : u32(kDim, fadeA), badge);
                ImGui::PopFont();
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            auto trCycleAction = [&](const char *id, const char *label, int &idx, const char *const *opts, int count,
                                     std::function<void(int)> onApply, bool applyOnStep, const char *dTitle, const char *dBody) {
                ImVec2 p0, p1;
                bool barOnRow = false;
                const bool sel = beginRow(label, dTitle, dBody, p0, p1, barOnRow);
                if (sel && kbStep != 0)
                {
                    idx = (idx + kbStep + count) % count;
                    playMenuSfx(lcs::game::MenuSfx::Move);
                    if (applyOnStep) onApply(idx);
                }
                if (sel && kbAct)
                {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    onApply(idx);
                }
                if (drawGta6Cycle(childDl, L, id, ImVec2(p1.x - L.u(14.0f), p0.y), rowH, idx, opts, count, barOnRow))
                {
                    g_trainerRow = rowIdx - 1;
                    playMenuSfx(lcs::game::MenuSfx::Move);
                    if (applyOnStep) onApply(idx);
                }
                // Click on left label area also applies the action!
                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string(id) + "_lbl").c_str(), ImVec2(leftW * 0.55f, rowH)))
                {
                    g_trainerRow = rowIdx - 1;
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    onApply(idx);
                }
                ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y));
            };

            switch (g_trainerCat)
            {
            case 0: // JOGADOR
            {
                trToggle("##tg_god", "Modo Deus (Vida e Colete infinitos)", tr.god_mode,
                         "MODO DEUS", "Mantém a vida e o colete de Toni Cipriani travados em 100% continuamente.");
                trAction("##ta_heal", "Curar jogador (100% Vida + Colete)", "APLICAR", [&] { tr.heal_player(); },
                         "CURAR JOGADOR", "Restaura imediatamente toda a vida e o colete.");
                trToggle("##tg_nw", "Nunca Procurado", tr.never_wanted,
                         "NUNCA PROCURADO", "Zera continuamente o nível de procurado da polícia de Liberty City.");
                static const char *kStars[] = {"0 Estrelas", "1 Estrela", "2 Estrelas", "3 Estrelas", "4 Estrelas", "5 Estrelas", "6 Estrelas"};
                trCycleAction("##tc_wan", "Definir nível de procurado", g_wantedLevel, kStars, 7,
                              [&](int s) { tr.set_wanted_level(s); }, true,
                              "NÍVEL DE PROCURADO", "Use Esquerda/Direita ou clique nos chevrons para ajustar de 0 a 6 estrelas.");
                static const char *kMonLbl[] = {"+$1.000", "+$10.000", "+$100.000", "+$1.000.000"};
                static constexpr int kMonVal[] = {1000, 10000, 100000, 1000000};
                trCycleAction("##tc_mon", "Adicionar dinheiro (Enter / Clique)", g_moneyIdx, kMonLbl, 4,
                              [&](int i) { tr.add_money(kMonVal[i]); }, false,
                              "ADICIONAR DINHEIRO", "Escolha o valor com Esquerda/Direita e pressione Enter ou clique no nome para depositar.");
                trToggle("##tg_spr", "Sprint infinito", tr.infinite_sprint,
                         "SPRINT INFINITO", "Toni nunca perde fôlego ao correr pelas ruas de Liberty City.");
                break;
            }
            case 1: // ARMAS
            {
                static const char *kWepLbl[] = {
                    "Pistola 9mm", "Python .357", "Escopeta Pump", "Escopeta SPAS-12", "Stubby",
                    "Tec-9", "Micro Uzi", "Ingram Silenciada", "MP5", "Carabina M4",
                    "Ruger", "Rifle Sniper", "Sniper Laser", "Lança-Foguetes", "Lança-Chamas",
                    "M60", "Minigun"
                };
                static constexpr int kWepId[] = {17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33};
                trCycleAction("##tc_wep", "Dar arma (Enter / Clique)", g_weaponIdx, kWepLbl,
                              static_cast<int>(std::size(kWepLbl)),
                              [&](int i) { tr.give_weapon(kWepId[i], 500); }, false,
                              "DAR ARMA", "Seleciona qualquer arma do jogo, carrega seu modelo via CStreaming e equipa com 500 munições via CPed::GiveWeapon.");
                trAction("##ta_ars", "Arsenal completo (WeaponCheat3)", "APLICAR", [&] { tr.give_full_loadout(); },
                         "ARSENAL COMPLETO", "Concede o pacote completo de armas pesadas do GTA: Liberty City Stories.");
                trToggle("##tg_amo", "Munição infinita", tr.unlimited_ammo,
                         "MUNIÇÃO INFINITA", "Mantém a munição reserva travada em 9999 em todas as armas equipadas.");
                trToggle("##tg_nrl", "Sem recarga", tr.no_reload,
                         "SEM RECARGA", "Mantém o pente sempre cheio durante disparos contínuos.");
                break;
            }
            case 2: // VEÍCULOS
            {
                static const char *kVehLbl[] = {"Infernus", "Cheetah", "Banshee", "Stinger", "Táxi", "Viatura LCPD", "Tanque Rhino", "PCJ-600"};
                static constexpr int kVehId[] = {131, 145, 159, 141, 132, 156, 162, 178};
                trCycleAction("##tc_veh", "Spawnar veículo (Enter / Clique)", g_vehicleIdx, kVehLbl, 8,
                              [&](int i) { tr.spawn_vehicle(kVehId[i]); }, false,
                              "SPAWNAR VEÍCULO", "Gera o veículo selecionado diretamente à frente do jogador.");
                trAction("##ta_rep", "Consertar veículo atual", "APLICAR", [&] { tr.repair_vehicle(); },
                         "CONSERTAR VEÍCULO", "Restaura 100% da lataria e motor do veículo via CAutomobile::Fix.");
                trAction("##ta_flp", "Desvirar veículo", "APLICAR", [&] { tr.flip_vehicle_upright(); },
                         "DESVIRAR VEÍCULO", "Coloca um veículo capotado novamente sobre as 4 rodas.");
                trToggle("##tg_vinv", "Veículo invencível", tr.vehicle_invincible,
                         "VEÍCULO INVENCÍVEL", "Impede que o veículo atual sofra danos ou pegue fogo.");
                trAction("##ta_bst", "Turbo instantâneo", "APLICAR", [&] { tr.boost_vehicle(); },
                         "TURBO", "Aplica um impulso de velocidade para frente no veículo atual.");
                break;
            }
            case 3: // TELEPORTE
                trAction("##ta_sv", "Salvar posição atual", "SALVAR", [&] { tr.save_position(); },
                         "SALVAR POSIÇÃO", "Memoriza as coordenadas atuais de Toni para retornar depois.");
                trAction("##ta_rt", "Voltar para posição salva", tr.has_saved_position() ? "IR" : "VAZIO",
                         [&] { tr.return_to_saved_position(); },
                         "RETORNAR", "Teleporta de volta para a posição salva anteriormente.");
                for (size_t i = 0; i < lcs::game::LCSTrainer::landmarks().size(); ++i)
                {
                    const auto &lm = lcs::game::LCSTrainer::landmarks()[i];
                    const std::string lbl = std::string(lm.island) + "  •  " + lm.name;
                    const std::string id = "##tp_" + std::to_string(i);
                    trAction(id.c_str(), lbl.c_str(), "TELEPORTAR",
                             [&, lm] { tr.teleport_to(lm.x, lm.y, lm.z, lm.name); },
                             lm.name, "Teleporta Toni (ou seu veículo) diretamente para este ponto de Liberty City.");
                }
                break;

            case 4: // CÂMERA
                trToggle("##tg_fc", "Câmera Livre (F6)", tr.free_camera,
                         "CÂMERA LIVRE", "Solta a câmera do personagem para explorar Liberty City livremente.");
                trToggle("##tg_hud", "Ocultar HUD", tr.hide_hud,
                         "OCULTAR HUD", "Esconde radar, dinheiro, vida e armas para capturas limpas.");
                break;

            case 5: // MUNDO
            {
                static const char *kHours[24] = {
                    "00:00","01:00","02:00","03:00","04:00","05:00","06:00","07:00",
                    "08:00","09:00","10:00","11:00","12:00","13:00","14:00","15:00",
                    "16:00","17:00","18:00","19:00","20:00","21:00","22:00","23:00"};
                trCycleAction("##tc_hr", "Hora do dia", g_timeHour, kHours, 24,
                              [&](int h) { tr.set_time_of_day(h); }, true,
                              "HORA DO DIA", "Altera o relógio interno do jogo (CClock) instantaneamente.");
                trToggle("##tg_ft", "Congelar relógio", tr.freeze_time,
                         "CONGELAR RELÓGIO", "Trava o horário atual do dia para manter a iluminação desejada.");
                static const char *kWea[] = {"Ensolarado", "Nublado", "Chuva", "Neblina", "Sol Forte", "Tempestade"};
                trCycleAction("##tc_wea", "Clima", g_weatherIdx, kWea, 6,
                              [&](int w) { tr.set_weather(w); }, true,
                              "CLIMA", "Força o clima imediatamente via CWeather::ForceWeatherNow.");
                static const char *kSpdLbl[] = {"0.25x", "0.50x", "0.75x", "1.00x (Normal)", "1.25x", "1.50x", "2.00x"};
                static constexpr float kSpdVal[] = {0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f};
                int spdIdx = 3;
                for (int i = 0; i < 7; ++i)
                    if (std::fabs(tr.game_speed - kSpdVal[i]) < 0.1f) spdIdx = i;
                trCycleAction("##tc_spd", "Velocidade do jogo", spdIdx, kSpdLbl, 7,
                              [&](int i) { tr.set_game_speed(kSpdVal[i]); }, true,
                              "VELOCIDADE DO JOGO", "Ajusta CTimer::ms_fTimeScale para câmera lenta ou acelerada.");
                break;
            }
            }

            childDl->ChannelsMerge();
            ImGui::Dummy(ImVec2(0.0f, 0.0f));
            g_trainerLastRows = rowIdx;
            if (g_trainerRow != prevRow)
                playMenuSfx(lcs::game::MenuSfx::Move);
            ImGui::EndChild();

            // Row counter bottom-left
            if (g_trainerLastRows > 0)
            {
                char cntBuf[32];
                std::snprintf(cntBuf, sizeof(cntBuf), "%d / %d", std::clamp(g_trainerRow + 1, 1, g_trainerLastRows), g_trainerLastRows);
                ImGui::PushFont(fontSemibold(), L.uf(20.0f));
                dl->AddText(ImVec2(leftX, L.sh - L.u(74.0f)), u32(kDim, fadeA), cntBuf);
                ImGui::PopFont();
            }

            // Right description column
            ImGui::PushFont(fontCondensed(), L.uf(28.0f));
            dl->AddText(ImVec2(descX, listTop), u32(kWhite, fadeA), activeTitle.c_str());
            ImGui::PopFont();
            dl->AddLine(ImVec2(descX, listTop + L.rf(42.0f)), ImVec2(descX + descW, listTop + L.rf(42.0f)),
                        u32(kHairline, 0.25f * fadeA), 1.0f);
            ImGui::PushFont(fontRegular(), L.uf(21.0f));
            dl->AddText(nullptr, 0.0f, ImVec2(descX, listTop + L.rf(62.0f)),
                        u32(kDim, fadeA), activeDesc.c_str(), nullptr, descW);
            ImGui::PopFont();

            float rx = L.sw - L.u(56.0f);
            const float fy = L.sh - L.u(46.0f);
            rx = drawFooterPrompt(dl, L, rx, fy, "VOLTAR", "ESC");
            rx = drawFooterPrompt(dl, L, rx, fy, "APLICAR", "ENTER");
            drawFooterPrompt(dl, L, rx, fy, "CATEGORIAS", "SUB_BAR");
        }
    } // namespace

    void drawMainMenu()
    {
        using namespace palette;
        ImGuiIO &io = ImGui::GetIO();
        if (io.DisplaySize.x < 100.0f || io.DisplaySize.y < 100.0f)
            return;

        if (!g_open)
        {
            g_uiDt = std::clamp(io.DeltaTime, 0.001f, 0.05f);
            g_menuGlowTime += g_uiDt;
            g_tabFade = std::min(1.0f, g_tabFade + smoothStepFactor(12.0f));
            g_tabSlide *= (1.0f - smoothStepFactor(14.0f));
        }

        const Layout L = computeLayout(io);
        const bool mouseMoved = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
        g_mmAnimTime += std::clamp(io.DeltaTime, 0.0f, 0.1f);
        const float fadeIn = std::clamp(g_mmAnimTime / 0.35f, 0.0f, 1.0f);

        auto &trainer = lcs::game::LCSTrainer::instance();
        const auto slots = trainer.read_save_slots();
        const int recentSlot = trainer.get_recent_save_slot();
        bool anyValidSave = (recentSlot >= 0);
        if (!anyValidSave)
        {
            for (const auto &s : slots)
            {
                if (s.state == 0) { anyValidSave = true; break; }
            }
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(L.sw, L.sh), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##LCSMainMenu", nullptr, flags))
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (g_mmSubView == 1)
            {
                if (g_mmConfirmSlot >= 0)
                {
                    char qTitle[96];
                    if (g_mmConfirmSlot == 8)
                        std::snprintf(qTitle, sizeof(qTitle), "%s", ui::loc::tx("CARREGAR AUTOSAVE?"));
                    else
                        std::snprintf(qTitle, sizeof(qTitle), "%s %d?", ui::loc::tx("CARREGAR SLOT"), g_mmConfirmSlot + 1);
                    drawGta6ConfirmScreen(
                        dl, L, mouseMoved, 0, qTitle,
                        "Todo o progresso atual não salvo em Liberty City será perdido.",
                        "CARREGAR", "CANCELAR", g_mmConfirmBtn,
                        [&]() {
                            playMenuSfx(lcs::game::MenuSfx::Confirm);
                            trainer.request_load_slot(g_mmConfirmSlot);
                            g_mmConfirmSlot = -1;
                            g_mmSubView = 0;
                        },
                        [&]() {
                            playMenuSfx(lcs::game::MenuSfx::Back);
                            triggerViewTransition(-1.0f);
                            g_mmConfirmSlot = -1;
                        });
                }
                else
                {
                    drawGta6SlotScreen(
                        dl, L, mouseMoved, 0, "JOGO /", "CARREGAR",
                        "Selecione um slot válido em Liberty City (Slots 1–8 ou AutoSave).",
                        false, 9, slots, g_mmSlotSel, "CARREGAR",
                        [&](int idx) {
                            if (idx < static_cast<int>(slots.size()) && slots[idx].state == 0)
                            {
                                playMenuSfx(lcs::game::MenuSfx::Confirm);
                                triggerViewTransition(1.0f);
                                g_mmConfirmSlot = idx;
                                g_mmConfirmBtn = 0;
                            }
                            else
                            {
                                playMenuSfx(lcs::game::MenuSfx::Error);
                                trainer.push_notice(ui::loc::tx("Este slot está vazio ou corrompido."), 1);
                            }
                        },
                        [&]() {
                            playMenuSfx(lcs::game::MenuSfx::Back);
                            triggerViewTransition(-1.0f);
                            g_mmSubView = 0;
                        });
                }
            }
            else if (g_mmSubView == 2 || g_mmSubView == 3)
            {
                const bool isQuitConfirm = (g_mmSubView == 3);
                drawGta6ConfirmScreen(
                    dl, L, mouseMoved, isQuitConfirm ? 3 : 1,
                    isQuitConfirm ? "SAIR DO JOGO?" : "INICIAR NOVO JOGO?",
                    isQuitConfirm ? "Deseja encerrar GTA: Liberty City Stories e voltar ao Windows?"
                                  : "Seus progressos em slots existentes serão mantidos. Iniciar campanha do começo?",
                    isQuitConfirm ? "SAIR" : "NOVO JOGO", "CANCELAR", g_mmConfirmBtn,
                    [&]() {
                        playMenuSfx(lcs::game::MenuSfx::Confirm);
                        g_mmSubView = 0;
                        if (isQuitConfirm)
                            trainer.request_quit();
                        else
                            trainer.request_new_game();
                    },
                    [&]() {
                        playMenuSfx(lcs::game::MenuSfx::Back);
                        triggerViewTransition(-1.0f);
                        g_mmSubView = 0;
                    });
            }
            else
            {
                const float barH = std::clamp(L.sh * 0.105f, L.u(86.0f), L.u(122.0f));
                const float stageH = L.sh - barH;

                // Pure black full canvas
                dl->AddRectFilled(ImVec2(0, 0), ImVec2(L.sw, L.sh), IM_COL32(0, 0, 0, 255));

                // Subtle cursor parallax (-1..1)
                const float mx = std::clamp((io.MousePos.x - L.sw * 0.5f) / std::max(1.0f, L.sw * 0.5f), -1.0f, 1.0f);
                const float my = std::clamp((io.MousePos.y - L.sh * 0.5f) / std::max(1.0f, L.sh * 0.5f), -1.0f, 1.0f);

                dl->PushClipRect(ImVec2(0, 0), ImVec2(L.sw, stageH), true);

                // 1. LCS Comic Panels Background (cover fill + subtle ambient Ken Burns zoom)
                const auto &bgTex = ui::assets::mainMenuBgTexture();
                if (bgTex.valid())
                {
                    const float zoom = 1.095f + 0.015f * std::sin(g_mmAnimTime * 0.30f);
                    const float scale = std::max(L.sw / static_cast<float>(bgTex.w),
                                                 stageH / static_cast<float>(bgTex.h)) * zoom;
                    const float bw = static_cast<float>(bgTex.w) * scale;
                    const float bh = static_cast<float>(bgTex.h) * scale;
                    const float bx = (L.sw - bw) * 0.5f - mx * L.u(8.0f);
                    const float by = (stageH - bh) * 0.5f - my * L.u(4.0f);
                    const ImU32 bgCol = IM_COL32(255, 255, 255, static_cast<int>(255 * fadeIn));
                    dl->AddImage(bgTex.id, ImVec2(bx, by), ImVec2(bx + bw, by + bh), ImVec2(0, 0), ImVec2(1, 1), bgCol);
                }

                // Atmospheric top/bottom vignette for sharp foreground pop
                dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(L.sw, stageH),
                                            IM_COL32(0, 0, 0, 95), IM_COL32(0, 0, 0, 80),
                                            IM_COL32(0, 0, 0, 145), IM_COL32(0, 0, 0, 130));

                // 2. Toni Cipriani AK-47 Foreground Art (anchored on left, bottom torso cut flush onto black bar)
                const auto &toniTex = ui::assets::mainMenuToniTexture();
                if (toniTex.valid())
                {
                    const float tAspect = static_cast<float>(toniTex.w) / static_cast<float>(std::max(1, toniTex.h));
                    const float th = L.sh;
                    const float tw = th * tAspect;
                    const float tx = L.ox - L.u(12.0f) - mx * L.u(16.0f);
                    const float ty = L.u(10.0f) - my * L.u(6.0f);
                    const ImU32 toniCol = IM_COL32(255, 255, 255, static_cast<int>(255 * fadeIn));
                    dl->AddImage(toniTex.id, ImVec2(tx, ty), ImVec2(tx + tw, ty + th), ImVec2(0, 0), ImVec2(1, 1), toniCol);
                }

                // 3. Stacked Grand Theft Auto: Liberty City Stories Logo on right half
                const auto &logoTex = ui::assets::mainMenuLogoTexture();
                if (logoTex.valid())
                {
                    const float lAspect = static_cast<float>(logoTex.w) / static_cast<float>(std::max(1, logoTex.h));
                    const ImU32 logoCol = IM_COL32(255, 255, 255, static_cast<int>(255 * fadeIn));
                    if (lAspect > 1.5f)
                    {
                        // Full 16:9 transparent plate aligned flush with Toni Cipriani's foreground stage
                        const float lh = stageH;
                        const float lw = lh * lAspect;
                        const float lx = L.ox - mx * L.u(10.0f);
                        const float ly = -my * L.u(4.0f);
                        dl->AddImage(logoTex.id, ImVec2(lx, ly), ImVec2(lx + lw, ly + lh), ImVec2(0, 0), ImVec2(1, 1), logoCol);
                    }
                    else
                    {
                        const float lw = L.contentW * 0.72f;
                        const float lh = lw / lAspect;
                        const float lcx = L.ox + L.contentW * 0.71f - mx * L.u(10.0f);
                        const float lcy = stageH * 0.47f - my * L.u(4.0f);
                        dl->AddImage(logoTex.id,
                                     ImVec2(lcx - lw * 0.5f, lcy - lh * 0.5f),
                                     ImVec2(lcx + lw * 0.5f, lcy + lh * 0.5f),
                                     ImVec2(0, 0), ImVec2(1, 1), logoCol);
                    }
                }

                dl->PopClipRect();

                // Build GTA IV-inspired horizontal action list
                struct MainItem { const char *label; std::function<void()> action; };
                std::vector<MainItem> items;
                if (recentSlot >= 0)
                {
                    items.push_back({ui::loc::get(ui::loc::Text::ResumeGame), [&, recentSlot] {
                        playMenuSfx(lcs::game::MenuSfx::Confirm);
                        trainer.request_load_slot(recentSlot);
                    }});
                }
                items.push_back({ui::loc::get(ui::loc::Text::StartNewGame), [&] {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    if (anyValidSave)
                    {
                        triggerViewTransition(1.0f);
                        g_mmSubView = 2;
                        g_mmConfirmBtn = 0;
                    }
                    else
                    {
                        trainer.request_new_game();
                    }
                }});
                if (anyValidSave)
                {
                    items.push_back({ui::loc::get(ui::loc::Text::LoadGame), [&] {
                        playMenuSfx(lcs::game::MenuSfx::Confirm);
                        triggerViewTransition(1.0f);
                        trainer.request_refresh_save_slots();
                        g_mmSubView = 1;
                        g_mmSlotSel = std::max(0, recentSlot);
                        g_mmConfirmSlot = -1;
                        g_mmConfirmBtn = 0;
                    }});
                }
                items.push_back({ui::loc::get(ui::loc::Text::Options), [] {
                    openGlobalMenu(GlobalTab::Configuracoes);
                }});
                items.push_back({ui::loc::tx("Mods"), [] {
                    openGlobalMenu(GlobalTab::Mods);
                }});
                items.push_back({ui::loc::get(ui::loc::Text::QuitGame), [&] {
                    playMenuSfx(lcs::game::MenuSfx::Confirm);
                    triggerViewTransition(1.0f);
                    g_mmSubView = 3;
                    g_mmConfirmBtn = 0;
                }});

                const int nItems = static_cast<int>(items.size());
                g_mmSel = std::clamp(g_mmSel, 0, std::max(0, nItems - 1));
                const int prevMmSel = g_mmSel;

                // 4. Bottom Solid Black GTA IV Strip
                dl->AddRectFilled(ImVec2(0, stageH), ImVec2(L.sw, L.sh), IM_COL32(0, 0, 0, 255));
                dl->AddLine(ImVec2(0, stageH), ImVec2(L.sw, stageH), IM_COL32(255, 255, 255, 22), 1.0f);

                if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_A, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    g_mmSel = (g_mmSel - 1 + nItems) % nItems;
                if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_D, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_E, false))
                    g_mmSel = (g_mmSel + 1) % nItems;
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_Space, false))
                {
                    items[g_mmSel].action();
                }
                else if (!g_justClosedToMain && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
                {
                    playMenuSfx(lcs::game::MenuSfx::Back);
                    triggerViewTransition(1.0f);
                    g_mmSubView = 3;
                    g_mmConfirmBtn = 0;
                }
                g_justClosedToMain = false;

                const float itemCy = stageH + barH * 0.48f;
                float itemX = L.ux(68.0f);
                const float gap = L.u(34.0f);
                float mmFont = L.uf(27.0f);
                {
                    const float maxItemsW = std::max(300.0f, L.sw - itemX - L.u(360.0f));
                    ImGui::PushFont(fontSemibold(), mmFont);
                    float sumW = 0.0f;
                    for (int i = 0; i < nItems; ++i)
                        sumW += ImGui::CalcTextSize(items[i].label).x + gap;
                    ImGui::PopFont();
                    if (sumW > maxItemsW)
                        mmFont *= (maxItemsW / sumW);
                }

                float targetMmX = itemX;
                float targetMmW = L.u(80.0f);
                ImGui::PushFont(fontSemibold(), mmFont);
                for (int i = 0; i < nItems; ++i)
                {
                    const ImVec2 ts = ImGui::CalcTextSize(items[i].label);
                    const ImVec2 hit0(itemX - L.u(8.0f), stageH);
                    const ImVec2 hit1(itemX + ts.x + L.u(8.0f), L.sh);

                    ImGui::SetCursorScreenPos(hit0);
                    if (ImGui::InvisibleButton((std::string("##mm_") + std::to_string(i)).c_str(),
                                               ImVec2(hit1.x - hit0.x, hit1.y - hit0.y)))
                    {
                        g_mmSel = i;
                        items[i].action();
                    }
                    if (ImGui::IsItemHovered() && mouseMoved)
                        g_mmSel = i;

                    const bool active = (i == g_mmSel);
                    const bool hov = ImGui::IsItemHovered();
                    const ImU32 col = active ? u32(kCrimson) : (hov ? u32(kWhite) : IM_COL32(142, 145, 152, 255));
                    dl->AddText(ImVec2(itemX, itemCy - ts.y * 0.5f), col, items[i].label);
                    if (active)
                    {
                        targetMmX = itemX;
                        targetMmW = ts.x;
                    }
                    itemX += ts.x + gap;
                }
                ImGui::PopFont();

                if (g_mmSel != prevMmSel)
                    playMenuSfx(lcs::game::MenuSfx::Move);

                glideFloat(g_mmBarX, targetMmX, 24.0f);
                glideFloat(g_mmBarW, targetMmW, 24.0f);
                const float pulseA = 0.78f + 0.22f * std::sin(g_menuGlowTime * 4.2f);
                dl->AddRectFilled(ImVec2(g_mmBarX, L.sh - L.u(4.5f)),
                                  ImVec2(g_mmBarX + g_mmBarW, L.sh),
                                  u32(kCrimson, pulseA));

                // Active "Continuar" save subtitle & right-aligned dynamic input prompts inside the bottom bar
                if (recentSlot >= 0 && g_mmSel == 0 && recentSlot < static_cast<int>(slots.size()) && slots[recentSlot].state == 0)
                {
                    const auto &rs = slots[recentSlot];
                    std::string info = rs.name.empty() ? "AutoSave" : rs.name;
                    if (!rs.date.empty()) info += "  •  " + rs.date;
                    ImGui::PushFont(fontRegular(), L.uf(16.5f));
                    const ImVec2 isz = ImGui::CalcTextSize(info.c_str());
                    dl->AddText(ImVec2(L.sw - L.u(56.0f) - isz.x, stageH - isz.y - L.u(14.0f)),
                                IM_COL32(220, 222, 228, 220), info.c_str());
                    ImGui::PopFont();
                }

                float rx = L.sw - L.u(56.0f);
                rx = drawFooterPrompt(dl, L, rx, itemCy, "SAIR", "ESC");
                drawFooterPrompt(dl, L, rx, itemCy, "SELECIONAR", "ENTER");
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(2);
    }

    void openGlobalMenu(GlobalTab tab)
    {
        const bool wasOpen = g_open;
        g_open = true;
        g_justOpened = true;
        const bool inGameplay = lcs::game::LCSTrainer::instance().is_in_gameplay();
        if (!inGameplay && tab != GlobalTab::Configuracoes && tab != GlobalTab::Mods)
            tab = GlobalTab::Configuracoes;
        g_tab = tab;
        g_jogoSubView = 0;
        g_jogoSlotSel = 0;
        g_jogoConfirmSlot = -1;
        g_cfgRow = 0;
        g_trainerRow = 0;
        g_mapInit = false;
        triggerViewTransition(1.0f);
        if (!wasOpen)
            playMenuSfx(lcs::game::MenuSfx::Confirm);
        lcs::game::LCSTrainer::instance().set_user_pause(inGameplay);
    }

    void closeGlobalMenu()
    {
        const bool wasOpen = g_open;
        g_open = false;
        g_justClosedToMain = true;
        g_jogoSubView = 0;
        g_jogoConfirmSlot = -1;
        if (wasOpen)
            playMenuSfx(lcs::game::MenuSfx::Back);
        if (lcs::game::LCSTrainer::instance().is_in_gameplay())
            lcs::game::LCSTrainer::instance().set_user_pause(false);
        else
            g_mmAnimTime = std::max(g_mmAnimTime, 0.35f);
    }

    void toggleGlobalMenu(GlobalTab tab)
    {
        if (g_open && g_tab == tab)
            closeGlobalMenu();
        else
            openGlobalMenu(tab);
    }

    bool globalMenuOpen() { return g_open; }
    bool globalMenuBlocksInput() { return g_open; }
    GlobalTab globalMenuActiveTab() { return g_tab; }

    void drawGlobalMenu()
    {
        if (!g_open)
            return;

        ImGuiIO &io = ImGui::GetIO();
        if (io.DisplaySize.x < 100.0f || io.DisplaySize.y < 100.0f)
            return;

        g_uiDt = std::clamp(io.DeltaTime, 0.001f, 0.05f);
        g_menuGlowTime += g_uiDt;
        g_tabFade = std::min(1.0f, g_tabFade + smoothStepFactor(12.0f));
        g_tabSlide *= (1.0f - smoothStepFactor(14.0f));

        const bool inGameplay = lcs::game::LCSTrainer::instance().is_in_gameplay();
        if (!inGameplay && g_tab != GlobalTab::Configuracoes && g_tab != GlobalTab::Mods)
            g_tab = GlobalTab::Configuracoes;

        // Ignore Escape/Backspace on the opening frame so stale keys never close the menu instantly
        if (!g_justOpened && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)))
        {
            if (!inGameplay)
            {
                closeGlobalMenu();
                return;
            }
            if (g_tab == GlobalTab::Jogo)
            {
                if (g_jogoConfirmSlot >= 0)
                {
                    g_jogoConfirmSlot = -1;
                    playMenuSfx(lcs::game::MenuSfx::Back);
                    triggerViewTransition(-1.0f);
                    return;
                }
                if (g_jogoSubView != 0)
                {
                    g_jogoSubView = 0;
                    playMenuSfx(lcs::game::MenuSfx::Back);
                    triggerViewTransition(-1.0f);
                    return;
                }
                closeGlobalMenu();
                return;
            }
            g_tab = GlobalTab::Jogo;
            playMenuSfx(lcs::game::MenuSfx::Back);
            triggerViewTransition(-1.0f);
            return;
        }
        g_justOpened = false;

        // Global top-level tab switching with Q / E / L1 / R1 (disabled only while inside a Save/Load sub-view)
        if (!inGameplay)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
            {
                const float dir = ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1.0f : -1.0f;
                g_tab = (g_tab == GlobalTab::Configuracoes) ? GlobalTab::Mods : GlobalTab::Configuracoes;
                triggerViewTransition(dir);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
        }
        else if (!(g_tab == GlobalTab::Jogo && g_jogoSubView != 0))
        {
            constexpr int kN = static_cast<int>(GlobalTab::Count);
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            {
                g_tab = static_cast<GlobalTab>((static_cast<int>(g_tab) - 1 + kN) % kN);
                triggerViewTransition(-1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            {
                g_tab = static_cast<GlobalTab>((static_cast<int>(g_tab) + 1) % kN);
                triggerViewTransition(1.0f);
                playMenuSfx(lcs::game::MenuSfx::Move);
            }
        }

        const Layout L = computeLayout(io);
        const bool mouseMoved = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(L.sw, L.sh), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##LCSGlobalMenu", nullptr, flags))
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            switch (g_tab)
            {
            case GlobalTab::Jogo:          drawTabJogo(dl, L, mouseMoved); break;
            case GlobalTab::Mapa:          drawTabMapa(dl, L); break;
            case GlobalTab::Registro:      drawTabRegistro(dl, L, mouseMoved); break;
            case GlobalTab::Progresso:     drawTabProgresso(dl, L); break;
            case GlobalTab::Configuracoes: drawTabConfiguracoes(dl, L, mouseMoved); break;
            case GlobalTab::Mods:          drawTabMods(dl, L, mouseMoved); break;
            case GlobalTab::Trainer:       drawTabTrainer(dl, L, mouseMoved); break;
            default: break;
            }

            // Show the global top bar on all tabs except the clean Image-8 JOGO hub
            if (g_tab != GlobalTab::Jogo)
            {
                drawGlobalTopBar(dl, L, inGameplay);
                // On MAPA, draw the real in-game clock ON TOP of the top bar (top-left, crisp white)
                if (g_tab == GlobalTab::Mapa)
                {
                    const auto snap = lcs::game::LCSTrainer::instance().read_snapshot();
                    char clkBuf[32];
                    std::snprintf(clkBuf, sizeof(clkBuf), "%02d:%02d", snap.clock_hour, snap.clock_minute);
                    ImGui::PushFont(fontSemibold(), L.uf(22.0f));
                    dl->AddText(ImVec2(L.u(32.0f), L.u(18.0f)), palette::u32(palette::kWhite), clkBuf);
                    ImGui::PopFont();
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleColor(1);
        ImGui::PopStyleVar(2);
    }

    void drawNotices()
    {
        using namespace palette;
        const auto &notices = lcs::game::LCSTrainer::instance().notices();
        if (notices.empty())
            return;

        ImGuiIO &io = ImGui::GetIO();
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const float s = std::clamp(io.DisplaySize.y / 900.0f, 0.85f, 2.0f);
        const float fs = std::clamp(static_cast<float>(lcs::settings::get().ui_font_scale) / 100.0f, 0.90f, 1.40f);
        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;
        float y = sh - 92.0f * s;

        ImGui::PushFont(fontSemibold(), 17.0f * s * fs);
        for (int i = static_cast<int>(notices.size()) - 1; i >= 0; --i)
        {
            const std::string &txt = notices[i].text;
            const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
            const float bw = ts.x + 34.0f * s;
            const float bh = std::max(36.0f * s, ts.y + 14.0f * s);
            const ImVec2 p0(sw - bw - 28.0f * s, y - bh);
            const ImVec2 p1(sw - 28.0f * s, y);
            dl->AddRectFilled(p0, p1, u32(kTopBarBg, 0.94f), 4.0f * s);
            dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f * s, p1.y), u32(kCrimson), 2.0f * s);
            dl->AddRect(p0, p1, u32(kCrimson, 0.45f), 4.0f * s, 0, 1.0f);
            dl->AddText(ImVec2(p0.x + 15.0f * s, p0.y + (bh - ts.y) * 0.5f), u32(kWhite), txt.c_str());
            y -= (bh + 8.0f * s);
        }
        ImGui::PopFont();
    }
}
