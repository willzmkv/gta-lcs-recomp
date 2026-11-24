// Shared runtime settings for GTA: Liberty City Stories (PC), synced with lcs.ini
// (read and written by both LCSLauncher.exe and the in-game F2 Settings menu).
#pragma once

#include <string>

namespace lcs::settings {

struct RuntimeSettings {
    // Display
    int display_window = 1;       // 0 = Borderless Fullscreen, 1 = Windowed
    int display_aspect = 169;     // 0 = Auto, 169 = 16:9, 219 = 21:9, 43 = 4:3
    int display_hud = 2;          // 0 = Stretched, 1 = Centered 4:3, 2 = Widescreen
    int display_fps = 0;          // 0 = Monitor V-Sync / Unlimited, 30, 60, 120, 144, 240
    int display_vsync = 1;        // 0 = Off, 1 = On
    int display_fps_counter = 0;  // 0 = Off, 1 = Simple, 2 = Detailed
    int game_language = 0;        // 0=EN, 1=FR, 2=DE, 3=IT, 4=ES, 5=RU, 6=JA, 7=PT-BR
    int subtitle_font = 1;        // 0=Original, 1=Oswald
    int subtitle_outline = 50;    // 0..100%
    int subtitle_size = 100;      // 80..150%
    int subtitle_shadow = 1;      // 0=Off, 1=On
    int subtitle_background = 0;  // 0, 25, 50, 75 (%)
    int subtitle_colour = 0;      // 0=Game's, 1=White

    // Graphics
    int display_renderer = 0;     // 0 = Direct3D 12, 1 = Vulkan 1.4
    int display_scale = 0;        // 0 = Match Window, 1..4
    int display_msaa = 4;         // 1, 2, 4, 8
    int display_aniso = 16;       // 0, 2, 4, 8, 16
    int ipad_shadows = 1;         // 0 = Off, 1 = Dynamic iPad Shadows (_SDWMAP)
    int color_grading = 1;        // 0 = Off, 1 = iPad Retina Color Grading
    int brightness = 62;          // 0..100
    int contrast = 50;            // 0..100
    int saturation = 50;          // 0..100
    int view_distance = 50;       // 0..100
    int fog_distance = 75;        // 10..100 (VCS atmospheric fog end %)
    int fog_start = 120;          // 0..300 (VCS atmospheric fog start %)
    int world_hd_radius = 2;      // 0 = Off (Original), 1..3 = Multi-cell HD streaming
    int detail_distance = 50;     // 0..100 (LOD switch distance)
    int population_distance = 50; // 0..100 (Ped & traffic spawn/cull radius)
    int traffic_density = 50;     // 0..100
    int ped_density = 50;         // 0..100
    int streaming_memory = 128;   // 64..512 MiB (retained Rsl/world cache)

    // PS2 Graphical Effects Pipeline (SLUS_214.23 + GAME.DTZ combined with Recomp)
    int ps2_pipeline = 1;               // 0 = Off, 1 = On (Master toggle for PS2 post-processing)
    int ps2_timecycle = 1;              // 0 = Recomp/iPad, 1 = PS2 Original (GAME.DTZ), 2 = Hibrido PS2 + Recomp
    int ps2_radiosity = 1;              // 0 = Off, 1 = On (PS2 0x001d17d8 Radiosity / Bloom light bleed)
    int ps2_radiosity_intensity = 100;  // 0..200 (%)
    int ps2_color_filter = 1;           // 0 = Off, 1 = On (PS2 CMBlur BlurRGB atmospheric color grading)
    int ps2_color_filter_intensity = 100; // 0..200 (%)
    int ps2_trails = 1;                 // 0 = Off, 1 = On (PS2 0x00272348 Frame-History Trails)
    int ps2_trails_intensity = 100;     // 0..200 (%)
    int ps2_speed_blur = 1;             // 0 = Off, 1 = On (PS2 High-Speed Vehicle Motion Blur)

    // Audio
    int audio_volume = 100;       // 0..100
    int audio_music = 100;        // 0..100
    int audio_sfx = 100;          // 0..100

    // Controls
    int mouse_free_aim = 1;       // 0 = Off, 1 = On (1:1 Free Aim)
    int mouse_sensitivity = 100;  // 10..400%
    int mouse_invert_y = 0;       // 0 = Normal, 1 = Inverted
    int ui_font_scale = 100;      // 90..140 (%) Menu font accessibility scale
    int pad_vibration = 100;      // 0..200%
    int button_prompts = 1;       // 1 = Auto, 2 = PlayStation, 3 = Xbox, 4 = Keyboard

    // Enhancements
    int skip_intro = 0;           // 0 = Off, 1 = On
    int free_cam_speed = 100;     // 25..400%

    // Mods
    int mods_enabled = 1;         // 0 = Off, 1 = On
    int texture_replacements = 1; // 0 = Off, 1 = On
};

RuntimeSettings& get();
bool load(const std::string& path = "lcs.ini");
bool save(const std::string& path = "lcs.ini");
void apply_live();

} // namespace lcs::settings
