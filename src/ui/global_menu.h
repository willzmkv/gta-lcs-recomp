// Unified GTA 6-style Global In-Game Menu for GTA: Liberty City Stories (PC)
// Unifies: JOGO (Hub), MAPA, REGISTRO, PROGRESSO, CONFIGURACOES, MODS, and TRAINER.
#pragma once

namespace ui
{
    enum class GlobalTab
    {
        Jogo = 0,
        Mapa,
        Registro,
        Progresso,
        Configuracoes,
        Mods,
        Trainer,
        Count
    };

    void openGlobalMenu(GlobalTab tab = GlobalTab::Jogo);
    void closeGlobalMenu();
    void toggleGlobalMenu(GlobalTab tab = GlobalTab::Jogo);
    bool globalMenuOpen();
    bool globalMenuBlocksInput();
    GlobalTab globalMenuActiveTab();

    void drawMainMenu();
    void drawGlobalMenu();
    void drawNotices();
}
