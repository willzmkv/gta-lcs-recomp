#pragma once

#include <string>

namespace lcs::core {

struct Config {
    int width = 1280;
    int height = 720;
    bool fullscreen = false;
    bool vsync = false;
    int max_fps = 0; // 0 = unlimited; common targets: 60, 75, 120, 240
    int brightness = 62; // 0..100 slider % (62% = 336 iPad Retina default)
    int contrast = 50;   // 0..100 slider % (50% = 1.12x iPad Retina default)
    int saturation = 50; // 0..100 slider % (50% = 1.24x iPad Retina default)
};

extern Config g_config;

void load_config(const std::string& path);
bool save_config_setting(const std::string& path, const std::string& key, const std::string& value);

} // namespace lcs::core
