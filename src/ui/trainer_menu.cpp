#include "ui/trainer_menu.h"

#include "game/lcs_trainer.h"
#include "ui/ui_theme.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ui
{
    namespace
    {
        enum class TrainerPage
        {
            Main = 0,
            Player,
            Weapons,
            Vehicles,
            Teleport,
            Camera,
            World,
        };

        struct TrainerRow
        {
            std::string label;
            std::string value;
            std::string help;
            int stateColor = 0; // 0=normal, 1=ON green, 2=OFF red
            std::function<void()> onActivate;
            std::function<void(int)> onStep;
        };

        bool g_open = false;
        TrainerPage g_page = TrainerPage::Main;
        int g_selected = 0;
        int g_wantedLevel = 0;
        int g_moneyIdx = 1;
        int g_weaponIdx = 0;
        int g_vehicleIdx = 0;
        int g_weatherIdx = 0;
        int g_timeHour = 12;

        const char *pageTitle(TrainerPage p)
        {
            switch (p)
            {
            case TrainerPage::Main: return "MAIN MENU";
            case TrainerPage::Player: return "MAIN MENU > PLAYER";
            case TrainerPage::Weapons: return "MAIN MENU > WEAPONS";
            case TrainerPage::Vehicles: return "MAIN MENU > VEHICLES";
            case TrainerPage::Teleport: return "MAIN MENU > TELEPORT";
            case TrainerPage::Camera: return "MAIN MENU > CAMERA";
            case TrainerPage::World: return "MAIN MENU > WORLD";
            }
            return "MAIN MENU";
        }

        std::vector<TrainerRow> buildRows()
        {
            auto &tr = lcs::game::LCSTrainer::instance();
            std::vector<TrainerRow> rows;

            auto addToggle = [&](const char *lbl, bool &flag, const char *help) {
                rows.push_back({
                    lbl,
                    flag ? "ON" : "OFF",
                    help,
                    flag ? 1 : 2,
                    [&flag, lbl, &tr]() {
                        flag = !flag;
                        tr.push_notice(std::string(lbl) + (flag ? ": ON" : ": OFF"));
                    },
                    [&flag, lbl, &tr](int) {
                        flag = !flag;
                        tr.push_notice(std::string(lbl) + (flag ? ": ON" : ": OFF"));
                    },
                });
            };

            switch (g_page)
            {
            case TrainerPage::Main:
                rows.push_back({"Player", ">", "God Mode, Heal, Wanted Level, Money, Infinite Sprint", 0, [] { g_page = TrainerPage::Player; g_selected = 0; }, nullptr});
                rows.push_back({"Weapons", ">", "Give Weapon, Full Loadout, Unlimited Ammo, No Reload", 0, [] { g_page = TrainerPage::Weapons; g_selected = 0; }, nullptr});
                rows.push_back({"Vehicles", ">", "Spawn Vehicle, Repair, Flip Upright, Invincible, Boost", 0, [] { g_page = TrainerPage::Vehicles; g_selected = 0; }, nullptr});
                rows.push_back({"Teleport", ">", "Save/Return Position & Liberty City Landmarks", 0, [] { g_page = TrainerPage::Teleport; g_selected = 0; }, nullptr});
                rows.push_back({"Camera", ">", "Free Camera (F6) & Hide HUD", 0, [] { g_page = TrainerPage::Camera; g_selected = 0; }, nullptr});
                rows.push_back({"World", ">", "Time of Day, Freeze Time, Weather, Game Speed", 0, [] { g_page = TrainerPage::World; g_selected = 0; }, nullptr});
                break;

            case TrainerPage::Player:
            {
                addToggle("God Mode", tr.god_mode, "Keeps Toni's Health and Armor locked at 100%.");
                rows.push_back({"Heal (Health + Armor 100%)", "APPLY", "Instantly restores full Health and Armor.", 0,
                                [&tr] { tr.heal_player(); }, nullptr});
                addToggle("Never Wanted", tr.never_wanted, "Prevents police wanted stars from rising.");
                rows.push_back({"Set Wanted Level", "< " + std::to_string(g_wantedLevel) + " Stars >",
                                "Left/Right to choose 0..6 stars, Enter to apply.", 0,
                                [&tr] { tr.set_wanted_level(g_wantedLevel); },
                                [&tr](int d) {
                                    g_wantedLevel = std::clamp(g_wantedLevel + d, 0, 6);
                                    tr.set_wanted_level(g_wantedLevel);
                                }});
                static constexpr int kMoneyAmts[] = {1000, 10000, 100000, 1000000};
                static const char *kMoneyLbls[] = {"+$1,000", "+$10,000", "+$100,000", "+$1,000,000"};
                rows.push_back({"Add Money", std::string("< ") + kMoneyLbls[g_moneyIdx] + " >",
                                "Press Enter to add cash to Toni's account.", 0,
                                [&tr] { tr.add_money(kMoneyAmts[g_moneyIdx]); },
                                [](int d) { g_moneyIdx = (g_moneyIdx + d + 4) % 4; }});
                addToggle("Infinite Sprint", tr.infinite_sprint, "Never run out of stamina while sprinting.");
                break;
            }

            case TrainerPage::Weapons:
            {
                struct WeaponOpt { int id; const char *name; };
                static constexpr WeaponOpt kWeapons[] = {
                    {17, "9mm Pistol"}, {18, ".357 Python"}, {19, "Pump Shotgun"}, {20, "SPAS-12 Shotgun"},
                    {21, "Stubby Shotgun"}, {22, "Tec-9"}, {23, "Micro Uzi"}, {24, "Silenced Ingram"},
                    {25, "MP5"}, {26, "M4 Carbine"}, {27, "Ruger"}, {28, "Sniper Rifle"},
                    {29, "Laser Sniper"}, {30, "Rocket Launcher"}, {31, "Flamethrower"}, {32, "M60"},
                    {33, "Minigun"},
                };
                constexpr int kNumWep = static_cast<int>(sizeof(kWeapons) / sizeof(kWeapons[0]));
                rows.push_back({"Give Weapon", std::string("< ") + kWeapons[g_weaponIdx].name + " >",
                                "Left/Right to pick weapon, Enter to equip.", 0,
                                [&tr] { tr.give_weapon(kWeapons[g_weaponIdx].id, 500); },
                                [](int d) { g_weaponIdx = (g_weaponIdx + d + kNumWep) % kNumWep; }});
                rows.push_back({"Give Full Loadout", "APPLY", "Grants Pistol, Shotgun, MP5, M4, Sniper & RPG.", 0,
                                [&tr] { tr.give_full_loadout(); }, nullptr});
                addToggle("Unlimited Ammo", tr.unlimited_ammo, "Shots use no ammo: the clip never empties, no reload.");
                addToggle("No Reload", tr.no_reload, "Keeps active magazine full while firing.");
                break;
            }

            case TrainerPage::Vehicles:
            {
                struct VehOpt { int id; const char *name; const char *model; };
                // Ids from DEFAULT.IDE; the model is resolved by name in the game (VehOpt::model).
                static constexpr VehOpt kVehs[] = {
                    {142, "Infernus", "infernus"}, {146, "Cheetah", "cheetah"}, {160, "Banshee", "banshee"},
                    {133, "Stinger", "stinger"}, {152, "Kuruma", "kuruma"}, {167, "Stallion", "stallion"},
                    {136, "Sentinel", "sentinel"}, {151, "Taxi", "taxi"}, {157, "Police Car", "police"},
                    {162, "Rhino Tank", "rhino"}, {205, "PCJ-600", "pcj600"}, {207, "Freeway", "freeway"},
                    {210, "Sanchez", "sanchez"}, {214, "Maverick", "maverick"},
                };
                constexpr int kNumVeh = static_cast<int>(sizeof(kVehs) / sizeof(kVehs[0]));
                rows.push_back({"Spawn Vehicle", std::string("< ") + kVehs[g_vehicleIdx].name + " >",
                                "Left/Right to pick vehicle, Enter to spawn.", 0,
                                [&tr] { tr.spawn_vehicle(kVehs[g_vehicleIdx].model, kVehs[g_vehicleIdx].id); },
                                [](int d) { g_vehicleIdx = (g_vehicleIdx + d + kNumVeh) % kNumVeh; }});
                rows.push_back({"Repair Vehicle", "APPLY", "Restores current vehicle health to 100%.", 0,
                                [&tr] { tr.repair_vehicle(); }, nullptr});
                rows.push_back({"Flip Upright", "APPLY", "Rights an overturned vehicle.", 0,
                                [&tr] { tr.flip_vehicle_upright(); }, nullptr});
                addToggle("Vehicle Invincible", tr.vehicle_invincible, "Current vehicle takes no damage.");
                rows.push_back({"Boost Vehicle", "APPLY", "Gives a forward velocity boost.", 0,
                                [&tr] { tr.boost_vehicle(); }, nullptr});
                break;
            }

            case TrainerPage::Teleport:
            {
                rows.push_back({"Save Current Position", "SAVE", "Stores Toni's current coordinates.", 0,
                                [&tr] { tr.save_position(); }, nullptr});
                rows.push_back({"Return to Saved Position", tr.has_saved_position() ? "GO" : "NONE",
                                "Teleports back to your saved coordinates.", 0,
                                [&tr] { tr.return_to_saved_position(); }, nullptr});
                for (const auto &lm : lcs::game::LCSTrainer::landmarks())
                {
                    rows.push_back({std::string(lm.island) + ": " + lm.name, "TP",
                                    "Teleport directly to " + std::string(lm.name), 0,
                                    [&tr, lm] { tr.teleport_to(lm.x, lm.y, lm.z, lm.name); }, nullptr});
                }
                break;
            }

            case TrainerPage::Camera:
                addToggle("Free Camera (F6)", tr.free_camera, "Detaches camera for free-roaming screenshots.");
                addToggle("Hide HUD", tr.hide_hud, "Hides radar, health, cash and weapon HUD.");
                break;

            case TrainerPage::World:
            {
                char timeBuf[32];
                std::snprintf(timeBuf, sizeof(timeBuf), "< %02d:00 >", g_timeHour);
                rows.push_back({"Time of Day", timeBuf, "Left/Right to adjust clock hour (CClock).", 0,
                                [&tr] { tr.set_time_of_day(g_timeHour); },
                                [&tr](int d) {
                                    g_timeHour = (g_timeHour + d + 24) % 24;
                                    tr.set_time_of_day(g_timeHour);
                                }});
                addToggle("Freeze Time", tr.freeze_time, "Locks the in-game clock to the selected hour.");

                static const char *kWeathers[] = {"Sunny", "Cloudy", "Rainy", "Foggy", "Extrasunny", "Hurricane"};
                constexpr int kNumW = 6;
                rows.push_back({"Weather", std::string("< ") + kWeathers[g_weatherIdx] + " >",
                                "Left/Right to switch weather (CWeather).", 0,
                                [&tr] { tr.set_weather(g_weatherIdx); },
                                [&tr](int d) {
                                    g_weatherIdx = (g_weatherIdx + d + kNumW) % kNumW;
                                    tr.set_weather(g_weatherIdx);
                                }});

                char spdBuf[32];
                std::snprintf(spdBuf, sizeof(spdBuf), "< %.2fx >", tr.game_speed);
                rows.push_back({"Game Speed", spdBuf, "Adjusts CTimer::ms_fTimeScale (0.25x .. 2.00x).", 0,
                                [&tr] { tr.set_game_speed(1.0f); },
                                [&tr](int d) { tr.set_game_speed(tr.game_speed + d * 0.25f); }});
                break;
            }
            }
            return rows;
        }
    } // namespace

    void toggleTrainerMenu() { g_open = !g_open; }
    void closeTrainerMenu() { g_open = false; }
    bool trainerMenuOpen() { return g_open; }
    bool trainerMenuBlocksInput() { return g_open; }

    void drawTrainerMenu()
    {
        if (!g_open)
            return;

        using namespace palette;
        std::vector<TrainerRow> rows = buildRows();
        const int count = static_cast<int>(rows.size());
        if (count > 0)
            g_selected = std::clamp(g_selected, 0, count - 1);

        // Keyboard / Numpad navigation
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad8))
            g_selected = (g_selected - 1 + count) % std::max(1, count);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad2))
            g_selected = (g_selected + 1) % std::max(1, count);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad4))
            if (count > 0 && rows[g_selected].onStep) rows[g_selected].onStep(-1);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_Keypad6))
            if (count > 0 && rows[g_selected].onStep) rows[g_selected].onStep(1);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Keypad5))
            if (count > 0 && rows[g_selected].onActivate) rows[g_selected].onActivate();
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            if (g_page == TrainerPage::Main)
                g_open = false;
            else
            {
                g_page = TrainerPage::Main;
                g_selected = 0;
            }
            return;
        }

        const float panelW = 360.0f;
        const float hdrH = 62.0f;
        const float subH = 28.0f;
        const float rowH = 34.0f;
        const float helpH = 44.0f;
        const int visibleRows = std::min(count, 11);
        const float totalH = hdrH + subH + visibleRows * rowH + helpH;

        ImGui::SetNextWindowPos(ImVec2(28.0f, 28.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(panelW, totalH), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(kSkyTop, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_Border, rgba(kPink, 0.85f));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoScrollbar;
        if (ImGui::Begin("##LCSTrainerMenu", nullptr, flags))
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 wp = ImGui::GetWindowPos();

            // Crimson Gradient Header: "LCS TRAINER"
            dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + panelW, wp.y + hdrH),
                                        u32(kPink), u32(kPinkSoft),
                                        u32(kPinkBorder), u32(kPinkSoft));
            ImGui::PushFont(fontSemibold());
            dl->AddText(ImVec2(wp.x + 18.0f, wp.y + 14.0f), u32(kOnPink), "LCS TRAINER");
            ImGui::PopFont();
            ImGui::PushFont(fontRegular());
            dl->AddText(ImVec2(wp.x + 18.0f, wp.y + 36.0f), u32(kOnPink, 0.85f), "GTA: Liberty City Stories PC");
            ImGui::PopFont();

            // Path Sub-Header + Counter (e.g., MAIN MENU > PLAYER    1 / 6)
            const float subY = wp.y + hdrH;
            dl->AddRectFilled(ImVec2(wp.x, subY), ImVec2(wp.x + panelW, subY + subH), u32(kSectionBg));
            dl->AddLine(ImVec2(wp.x, subY + subH), ImVec2(wp.x + panelW, subY + subH), u32(kPink, 0.45f), 1.0f);
            ImGui::PushFont(fontSemibold());
            dl->AddText(ImVec2(wp.x + 14.0f, subY + 6.0f), u32(kOnPink), pageTitle(g_page));
            char cntBuf[32];
            std::snprintf(cntBuf, sizeof(cntBuf), "%d / %d", count > 0 ? (g_selected + 1) : 0, count);
            const ImVec2 cs = ImGui::CalcTextSize(cntBuf);
            dl->AddText(ImVec2(wp.x + panelW - cs.x - 14.0f, subY + 6.0f), u32(kTeal), cntBuf);
            ImGui::PopFont();

            // Rows (scrolling window if > 11 rows)
            const int startRow = std::max(0, g_selected - visibleRows + 1);
            const float listY = subY + subH;
            for (int vi = 0; vi < visibleRows; ++vi)
            {
                const int idx = startRow + vi;
                if (idx >= count) break;
                const TrainerRow &r = rows[idx];
                const ImVec2 p0(wp.x, listY + vi * rowH);
                const ImVec2 p1(wp.x + panelW, p0.y + rowH);

                ImGui::SetCursorScreenPos(p0);
                if (ImGui::InvisibleButton((std::string("##trow_") + std::to_string(idx)).c_str(), ImVec2(panelW, rowH)))
                {
                    g_selected = idx;
                    if (r.onActivate) r.onActivate();
                }
                if (ImGui::IsItemHovered())
                    g_selected = idx;

                const bool sel = (idx == g_selected);
                const ImU32 bg = sel ? u32(kRowHover) : u32((idx % 2 == 0) ? kRowEven : kRowOdd);
                dl->AddRectFilled(p0, p1, bg);
                if (sel)
                    dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f, p1.y), u32(kPink));

                ImGui::PushFont(sel ? fontSemibold() : fontRegular());
                dl->AddText(ImVec2(p0.x + 14.0f, p0.y + 8.0f), u32(sel ? kTealDark : kTeal), r.label.c_str());

                const ImU32 valCol = (r.stateColor == 1) ? u32(kStateOn)
                                   : (r.stateColor == 2) ? u32(kRed)
                                                         : u32(sel ? kOnPink : kTealMuted);
                const ImVec2 vs = ImGui::CalcTextSize(r.value.c_str());
                dl->AddText(ImVec2(p1.x - vs.x - 14.0f, p0.y + 8.0f), valCol, r.value.c_str());
                ImGui::PopFont();
            }

            // Bottom Help Card
            const float helpY = listY + visibleRows * rowH;
            dl->AddRectFilled(ImVec2(wp.x, helpY), ImVec2(wp.x + panelW, helpY + helpH), u32(kHeaderBg));
            dl->AddLine(ImVec2(wp.x, helpY), ImVec2(wp.x + panelW, helpY), u32(kTealLine, 0.35f), 1.0f);
            ImGui::PushFont(fontRegular());
            const char *helpStr = (count > 0) ? rows[g_selected].help.c_str() : "F5 to close.";
            dl->AddText(ImVec2(wp.x + 12.0f, helpY + 13.0f), u32(kTealMuted), helpStr);
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
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
        const float sw = io.DisplaySize.x;
        const float sh = io.DisplaySize.y;
        float y = sh - 28.0f;

        ImGui::PushFont(fontSemibold());
        for (int i = static_cast<int>(notices.size()) - 1; i >= 0; --i)
        {
            const std::string &txt = notices[i].text;
            const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
            const float bw = ts.x + 32.0f;
            const float bh = 32.0f;
            const ImVec2 p0(sw - bw - 24.0f, y - bh);
            const ImVec2 p1(sw - 24.0f, y);
            dl->AddRectFilled(p0, p1, u32(kHeaderBg, 0.94f), 4.0f);
            dl->AddRectFilled(p0, ImVec2(p0.x + 4.0f, p1.y), u32(kPink), 2.0f);
            dl->AddRect(p0, p1, u32(kPink, 0.45f), 4.0f, 0, 1.0f);
            dl->AddText(ImVec2(p0.x + 14.0f, p0.y + 8.0f), u32(kTealDark), txt.c_str());
            y -= (bh + 8.0f);
        }
        ImGui::PopFont();
    }
}
