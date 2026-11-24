#pragma once

namespace ui::loc {

enum class Text {
    // Top bar tabs & controls
    TabGame = 0,
    TabMap,
    TabBrief,
    TabStats,
    TabSettings,
    TabMods,
    TabTrainer,
    HintBack,
    HintSelect,
    HintEnter,
    HintClose,

    // Pause / Main menu shared actions
    ResumeGame,
    StartNewGame,
    LoadGame,
    Options,
    QuitGame,
    ConfirmNewGameTitle,
    ConfirmNewGameBody,
    ConfirmLoadGameTitle,
    ConfirmLoadGameBody,
    ConfirmQuitTitle,
    ConfirmQuitBody,
    ConfirmYes,
    ConfirmNo,
    EmptySlot,
    SaveSlotFmt,
    StoryMode,

    // Map tab
    MapNoInfo,
    MapUnknownArea,
    MapZoomHint,
    MapWaypointSet,
    MapWaypointCleared,

    // Brief (Resumo) tab
    BriefFilterAll,
    BriefFilterDialogue,
    BriefFilterHelp,
    BriefFilterObjective,
    BriefTagDialogue,
    BriefTagHelp,
    BriefTagObjective,
    BriefEmptyAll,
    BriefEmptyFilter,

    // Progress tab
    StatsTitle,
    StatsProgress,
    StatsMissionPassed,
    StatsMoney,
    StatsCriminalRating,

    // Mods tab
    ModsTitle,
    ModsEmpty,
    ModsActiveCount,
    ModsFolderLabel,

    // Settings categories & common items
    SettingsTitle,
    CatDisplay,
    CatAudio,
    CatControls,
    CatSubtitles,
    CatLanguage,
    OptOn,
    OptOff,
    OptLanguageLabel,
    OptLanguageDesc,
    OptSubtitleFont,
    OptSubtitleFontOriginal,
    OptSubtitleFontOswald,
    OptSubtitleOutline,
    OptSubtitleSize,
    OptSubtitleShadow,
    OptSubtitleBg,
    OptSubtitleBgOff,
    OptSubtitleBgLight,
    OptSubtitleBgMed,
    OptSubtitleBgDark,
    OptSubtitleColour,
    OptSubtitleColourGame,
    OptSubtitleColourWhite,
    OptSubtitleDesc,

    // Trainer & Misc
    TrainerTitle,
    SavedNotice,
    LoadedNotice,
    Count
};

// Returns localized UTF-8 string for the active (or specified) language:
// 0=English, 1=French, 2=German, 3=Italian, 4=Spanish, 5=Russian, 6=Japanese, 7=Português (Brasil)
const char* get(Text id, int lang = -1);

// Translates a Portuguese base UI label/option/header into the active (or specified) language.
// Returns the input string verbatim when language == 7 (PT-BR) or if unmapped.
const char* tx(const char* pt_str, int lang = -1);

} // namespace ui::loc
