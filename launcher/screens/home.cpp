// Home ("JOGO" / "GAME" tab): Rockstar Games Launcher layout for GTA: Vice City Stories (VCS.rec).
// Features the Vice City Stories .REC logo over the background art, the animated PLAY GAME / JOGAR button, the 4
// localized Rockstar glass action buttons on the right, the Build Confirmation bar, and the Vice City Commodore 64
// live CMD terminal (Terminal.png style) in the bottom area.
#include "shell.h"

#include "model/os.h"
#include "model/sound.h"
#include "model/system.h"
#include "platform/platform.h"

#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;

namespace screens
{
    using namespace render;
    using core::mix;
    using core::rgb;
    using core::withAlpha;
    using ui::ButtonStyle;

    void drawHome(Rect r)
    {
        App &app = *shell.app;
        const double since = shell.mode == Mode::Home ? shell.modeSince : ui::state.time;
        const bool c64Active = shell.c64TerminalOpen ||
                               (app.task && (app.task->kind == "build" || app.task->kind == "auto_update" ||
                                             app.task->kind == "shader_cache"));

        // 1. Left-center: Grand Theft Auto: Vice City Stories (.REC) Logo + PLAY GAME / JOGAR Button
        const float leftX = r.x + std::max(px(64), r.w * 0.082f);
        const float logoW = px(252);
        const float logoH = px(246);
        const float logoY = r.y + (c64Active ? px(6) : std::max(px(12), r.h * 0.025f));
        const Rect logoRect{leftX, logoY, logoW, logoH};

        pushReveal(since, 0);
        if (imageInfo(ImageId::GameLogo).valid)
        {
            image(logoRect, ImageId::GameLogo);
        }
        else
        {
            const float size = fontFound(kFontBrand) ? px(56) : px(48);
            drawText(kFontBrand, size, leftX, logoY + px(120), theme::kText, "Liberty City Stories");
        }
        popReveal();

        // Primary CTA: JOGAR / PLAY GAME
        const Rect playRect{leftX + px(6), logoRect.bottom() + px(14), px(240), px(54)};
        pushReveal(since, 1);
        if (app.gameRunning)
        {
            const double played = ui::state.wall - app.gameStart;
            const std::string playText = std::string(tr("RODANDO • ", "RUNNING • ")) + duration(played);
            ui::playGameButton(playText, playRect, theme::icon::kPlayFilled, false);
            ui::wakeAt(std::floor(ui::state.wall) + 1.0);
        }
        else
        {
            const std::string playLabel = std::string(tr("JOGAR", "PLAY GAME")) + "##home-play";
            if (shell.options.demo == "hoverplay")
            {
                ui::Widget &pw = ui::widget(ui::id(playLabel));
                pw.hover = 1.0f;
                pw.fresh = false;
                ui::state.time = 0.42;
            }
            const bool clickedPlay = ui::playGameButton(playLabel, playRect, theme::icon::kPlayFilled, !app.busy());
            const bool enterPressed = platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible &&
                                      ui::inputAllowed() && !shell.buildConfirmOpen;
            if ((clickedPlay || enterPressed) && !app.busy())
            {
                std::error_code ec;
                const bool hasExe = fs::is_regular_file(app.root / launcher::os::kGameExe, ec) ||
                                    fs::is_regular_file(app.build / "bin" / "Release" / launcher::os::kGameExe, ec);
                if (!app.gameFilesReady())
                {
                    toast(tr("Selecione a base Android (.apk/.obb) e o pacote iPad (GTALcs.ipa) primeiro.",
                             "Select both Android (.apk/.obb) and iPad (GTALcs.ipa) game files first."),
                          theme::kAmber);
                    openSetup(kStepDisc);
                }
                else if (hasExe)
                {
                    play(app);
                }
                else
                {
                    chooseInstallDirForBuild();
                }
            }
        }
        popReveal();

        // 2. Right-Side Vertical Stack of 6 LCS Noir Action Buttons (Localized PT-BR / EN):
        const float btnW = px(218);
        const float btnH = c64Active ? px(48) : px(52);
        const float btnGap = c64Active ? px(7) : px(9);
        const float btnX = r.right() - btnW - px(24);
        const float stackH = btnH * 6 + btnGap * 5;
        const float btnY0 = c64Active ? (r.y + px(12)) : std::max(r.y + px(22), r.y + (r.h - stackH) * 0.34f);

        // Button 1: SELECT GAME / SELECIONAR JOGO (opens Dual-Source Frankenstein panel)
        pushReveal(since, 2);
        {
            const Rect b1{btnX, btnY0, btnW, btnH};
            if (ui::rockstarButton("#rockstar-select-game", b1, theme::icon::kFolder,
                                   tr("SELECIONAR", "SELECT"), tr("JOGO", "GAME"), !app.busy()))
            {
                if (platform::input.shift)
                    chooseBuildDir(app.build);
                else
                    openSetup(kStepDisc);
            }
            const std::string tip1 =
                std::string("Android: ") + (app.gameFiles.android.ok ? "OK" : tr("FALTANDO", "MISSING")) +
                "  |  iPad (.ipa): " + (app.gameFiles.ipad.ok ? "OK" : tr("FALTANDO", "MISSING"));
            ui::tooltip(ui::id("#rockstar-select-game"), b1, tip1);
        }
        popReveal();

        // Button 2: BUILD GAME / COMPILAR JOGO
        pushReveal(since, 3);
        {
            const Rect b2{btnX, btnY0 + (btnH + btnGap) * 1, btnW, btnH};
            if (ui::rockstarButton("#rockstar-build-game", b2, theme::icon::kCrossedTools,
                                   tr("COMPILAR", "BUILD"), tr("JOGO", "GAME"), !app.busy()))
            {
                if (!app.gameFilesReady())
                {
                    toast(tr("Verifique os arquivos Android e iPad antes de compilar.",
                             "Verify both Android and iPad files before building."),
                          theme::kAmber);
                    openSetup(kStepDisc);
                }
                else
                {
                    chooseInstallDirForBuild();
                }
            }
            ui::tooltip(ui::id("#rockstar-build-game"), b2,
                        tr("Compilar o executável nativo de GTA: Liberty City Stories (lcs.exe)",
                           "Build the native GTA: Liberty City Stories executable (lcs.exe)"));
        }
        popReveal();

        // Button 3: UPDATE BUILD / ATUALIZAR BUILD
        pushReveal(since, 4);
        {
            const Rect b3{btnX, btnY0 + (btnH + btnGap) * 2, btnW, btnH};
            if (ui::rockstarButton("#rockstar-update-build", b3, theme::icon::kSync,
                                   tr("ATUALIZAR", "UPDATE"), tr("BUILD", "BUILD"), !app.busy()))
            {
                shell.buildConfirmOpen = false;
                shell.c64TerminalOpen = true;
                app.startAutoUpdateBuild();
            }
            ui::tooltip(ui::id("#rockstar-update-build"), b3,
                        tr("Atualizar e recompilar a build nativa de GTA: Liberty City Stories",
                           "Update and rebuild the native GTA: Liberty City Stories executable"));
        }
        popReveal();

        // Button 4: SHADER CACHE / CACHE DE SHADERS
        pushReveal(since, 5);
        {
            const Rect b4{btnX, btnY0 + (btnH + btnGap) * 3, btnW, btnH};
            if (ui::rockstarButton("#rockstar-shader-cache", b4, theme::icon::kChip,
                                   tr("CACHE DE", "SHADER"), tr("SHADERS", "CACHE"), !app.busy()))
            {
                shell.buildConfirmOpen = false;
                shell.c64TerminalOpen = true;
                app.startShaderCacheBuild();
            }
            ui::tooltip(ui::id("#rockstar-shader-cache"), b4,
                        tr("Pré-compilar os shaders GLSL do iPad (WorldVs/Ps, ModelXYZ, Sprite2D) para D3D12/Vulkan",
                           "Pre-compile iPad GLSL shaders (WorldVs/Ps, ModelXYZ, Sprite2D) for D3D12/Vulkan"));
        }
        popReveal();

        // Button 5: CONFIGURATION / CONFIGURAÇÕES
        pushReveal(since, 6);
        {
            const Rect b5{btnX, btnY0 + (btnH + btnGap) * 4, btnW, btnH};
            if (ui::rockstarButton("#rockstar-config", b5, theme::icon::kSettings,
                                   tr("CONFIGURAÇÕES", "CONFIGURATION"), "", true))
            {
                shell.tab = Tab::Settings;
                shell.tabSince = ui::state.time;
            }
            ui::tooltip(ui::id("#rockstar-config"), b5,
                        tr("Abrir menu de opções gráficas, mundo, população e áudio",
                           "Open graphics, world, population and audio options menu"));
        }
        popReveal();

        // Button 6: ADD TO STEAM / ADICIONAR À STEAM
        pushReveal(since, 7);
        {
            const Rect b6{btnX, btnY0 + (btnH + btnGap) * 5, btnW, btnH};
            if (ui::rockstarButton("#rockstar-steam", b6, theme::icon::kSteam,
                                   tr("ADICIONAR À", "ADD TO"), "STEAM", !app.busy()))
            {
                std::string msg;
                const bool ok = app.addToSteam(msg);
                toast(msg, ok ? theme::kTeal : theme::kRed);
            }
            ui::tooltip(ui::id("#rockstar-steam"), b6,
                        tr("Adicionar o launcher à Steam com Grid, Hero, Logo e Ícone pré-configurados",
                           "Add the launcher to Steam with pre-configured Grid, Hero, Logo and Icon artwork"));
        }
        popReveal();

        // 3. Bottom Area: Build Confirmation Bar OR Vice City C64 Terminal OR Default Footer
        if (c64Active)
        {
            const float termH = px(198);
            const Rect termR{r.x + px(24), r.bottom() - termH - px(12), r.w - px(48), termH};

            std::deque<std::string> emptyLog;
            const std::deque<std::string> &logLines = (app.task && !app.task->log.empty()) ? app.task->log : emptyLog;

            float prog = 0.0f;
            std::string statusStr = tr("EXECUTANDO PIPELINE...", "RUNNING PIPELINE...");
            bool finished = false;
            if (app.task)
            {
                finished = app.task->finished;
                if (app.task->total > 0)
                {
                    prog = float(app.task->done) / float(app.task->total);
                    const int pct = std::clamp(int(std::round(prog * 100.0f)), 0, 100);
                    statusStr = "[" + std::to_string(pct) + "%]";
                }
                else if (finished)
                {
                    prog = 1.0f;
                    statusStr = app.task->ok() ? tr("[CONCLUÍDO]", "[COMPLETE]") : tr("[ERRO]", "[FAILED]");
                }
            }

            bool closeClicked = false;
            ui::c64TerminalView(ui::id("#home-c64-terminal"), termR, logLines, statusStr, prog, finished, &closeClicked);
            if (closeClicked)
            {
                if (app.busy())
                    app.cancelTask();
                else
                    app.dismissTask();
                shell.c64TerminalOpen = false;
            }
        }
        else if (shell.buildConfirmOpen)
        {
            const float barH = px(74);
            const Rect barR{r.x + px(24), r.bottom() - barH - px(16), r.w - px(48), barH};
            shadow(barR.offset(0, px(5)), rgb(0x000000, 195), px(16), px(2));
            rectEx(barR, rgb(0x141418, 248), rgb(0x0D0D10, 248), px(3), rgb(0xD91E18, 210), px(1.5f));
            rect({barR.x, barR.y, px(6), barR.h}, rgb(0xD91E18));

            const fs::path targetDir = shell.stagedBuildDir.empty() ? app.build : shell.stagedBuildDir;
            ui::label(kFontSemibold, px(11.5f), {barR.x + px(20), barR.y + px(12), barR.w * 0.55f, px(18)},
                      rgb(0xFF3B30), tr("PASTA DE INSTALAÇÃO FINAL DO JOGO", "FINAL GAME INSTALLATION FOLDER"));
            ui::label(kFontMono, px(12.5f), {barR.x + px(20), barR.y + px(34), barR.w * 0.54f, px(24)},
                      rgb(0xFFFFFF), launcher::pathText(targetDir));

            const float cancelW = px(96);
            const float confirmW = px(176);
            const float changeW = px(126);
            const float btnY = barR.cy() - px(19);
            const Rect cancelR{barR.right() - px(16) - cancelW, btnY, cancelW, px(38)};
            const Rect confirmR{cancelR.x - px(10) - confirmW, btnY, confirmW, px(38)};
            const Rect changeR{confirmR.x - px(10) - changeW, btnY, changeW, px(38)};

            if (ui::button(tr("Alterar pasta", "Change folder"), changeR, ButtonStyle::Secondary))
            {
                chooseInstallDirForBuild();
            }
            if (ui::button(tr("CONFIRMAR BUILD", "CONFIRM BUILD"), confirmR, ButtonStyle::Primary, theme::icon::kPlayFilled))
            {
                shell.buildConfirmOpen = false;
                shell.c64TerminalOpen = true;
                sound::play(sound::Sfx::Play);
                app.startBuildTo(targetDir);
            }
            if (ui::button(tr("Cancelar", "Cancel"), cancelR, ButtonStyle::Ghost))
            {
                shell.buildConfirmOpen = false;
                sound::play(sound::Sfx::Back);
            }
        }
        else
        {
            pushReveal(since, 6);
            const float infoY = r.bottom() - px(52);
            const float iconCx = r.x + px(36);
            const float iconCy = infoY + px(20);
            const Rect infoHit{r.x + px(22), infoY, px(420), px(40)};
            const ui::Interaction infoIn = ui::interact(ui::id("#home-info-footer"), infoHit, false);

            drawIcon(kFontIcons, px(20), iconCx, iconCy, mix(rgb(0xFFFFFF), theme::kPinkHi, infoIn.w->hover),
                     theme::icon::kInfo);

            const float textX = r.x + px(54);
            ui::label(kFontSemibold, px(13.5f), {textX, infoY + px(2), px(460), px(20)}, rgb(0xFFFFFF),
                      "Grand Theft Auto: Liberty City Stories (PC)");

            std::string subLine = tr("Port Nativo PC (Android ARM64 + iPad HD)  •  Build 1.0.0",
                                     "Native PC Port (Android ARM64 + iPad HD)  •  Build 1.0.0");
            if (app.gameFilesReady())
                subLine += tr("  •  Android + iPad prontos", "  •  Android + iPad ready");
            else
                subLine += tr("  •  Arquivos pendentes", "  •  Files pending");
            ui::label(kFontBody, px(12.0f), {textX, infoY + px(21), px(540), px(18)}, rgb(0xB5B5BC), subLine);

            if (infoIn.clicked)
            {
                shell.tab = Tab::About;
                shell.tabSince = ui::state.time;
            }
            popReveal();
        }
    }
}
