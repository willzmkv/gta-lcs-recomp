// In-game F2 Settings Menu for GTA: Liberty City Stories (PC)
#pragma once

#include <string>

namespace ui
{
    void openSettingsMenu(const std::string &page = "Display");
    void closeSettingsMenu();
    void toggleSettingsMenu();
    void drawSettingsMenu();
    bool settingsMenuOpen();
    bool settingsMenuBlocksInput();
}
