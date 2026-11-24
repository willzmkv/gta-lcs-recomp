#include "settings/settings.h"

#include "core/config.h"
#include "game/lcs_distance.h"
#include "game/lcs_world_hd.h"
#include "graphics/render_bridge.h"
#include "graphics/window.h"
#include "input/mouse_aim.h"
#include "input/pc_controls.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lcs::settings {

namespace {
RuntimeSettings g_settings;

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

int parse_int(const std::string& s, int fallback) {
    if (s.empty()) return fallback;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return (end && end != s.c_str()) ? static_cast<int>(v) : fallback;
}
} // namespace

RuntimeSettings& get() {
    return g_settings;
}

bool load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        // Initialize from g_config defaults if lcs.ini does not exist yet
        g_settings.display_window = lcs::core::g_config.fullscreen ? 0 : 1;
        g_settings.display_vsync = lcs::core::g_config.vsync ? 1 : 0;
        g_settings.display_fps = lcs::core::g_config.max_fps;
        g_settings.brightness = lcs::core::g_config.brightness;
        g_settings.contrast = lcs::core::g_config.contrast;
        g_settings.saturation = lcs::core::g_config.saturation;
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "display.window") g_settings.display_window = std::clamp(parse_int(val, g_settings.display_window), 0, 1);
        else if (key == "display.aspect") g_settings.display_aspect = parse_int(val, g_settings.display_aspect);
        else if (key == "display.hud") g_settings.display_hud = std::clamp(parse_int(val, g_settings.display_hud), 0, 2);
        else if (key == "display.fps") g_settings.display_fps = std::clamp(parse_int(val, g_settings.display_fps), 0, 500);
        else if (key == "display.vsync") g_settings.display_vsync = std::clamp(parse_int(val, g_settings.display_vsync), 0, 1);
        else if (key == "display.fps_counter") g_settings.display_fps_counter = std::clamp(parse_int(val, g_settings.display_fps_counter), 0, 2);
        else if (key == "game.language") g_settings.game_language = std::clamp(parse_int(val, g_settings.game_language), 0, 7);
        else if (key == "enhance.subtitle_font") g_settings.subtitle_font = std::clamp(parse_int(val, g_settings.subtitle_font), 0, 1);
        else if (key == "enhance.subtitle_outline") g_settings.subtitle_outline = std::clamp(parse_int(val, g_settings.subtitle_outline), 0, 100);
        else if (key == "enhance.subtitle_size") g_settings.subtitle_size = std::clamp(parse_int(val, g_settings.subtitle_size), 80, 150);
        else if (key == "enhance.subtitle_shadow") g_settings.subtitle_shadow = std::clamp(parse_int(val, g_settings.subtitle_shadow), 0, 1);
        else if (key == "enhance.subtitle_background") g_settings.subtitle_background = std::clamp(parse_int(val, g_settings.subtitle_background), 0, 100);
        else if (key == "enhance.subtitle_colour") g_settings.subtitle_colour = std::clamp(parse_int(val, g_settings.subtitle_colour), 0, 1);
        else if (key == "display.renderer") g_settings.display_renderer = std::clamp(parse_int(val, g_settings.display_renderer), 0, 2);
        else if (key == "display.scale") g_settings.display_scale = std::clamp(parse_int(val, g_settings.display_scale), 0, 8);
        else if (key == "display.msaa") g_settings.display_msaa = std::clamp(parse_int(val, g_settings.display_msaa), 1, 8);
        else if (key == "display.aniso") g_settings.display_aniso = std::clamp(parse_int(val, g_settings.display_aniso), 0, 16);
        else if (key == "graphics.ipad_shadows") g_settings.ipad_shadows = std::clamp(parse_int(val, g_settings.ipad_shadows), 0, 1);
        else if (key == "graphics.stochastic_textures")
            g_settings.stochastic_textures = std::clamp(parse_int(val, g_settings.stochastic_textures), 0, 2);
        else if (key == "graphics.color_grading" || key == "graphics.post_process")
            g_settings.color_grading = std::clamp(parse_int(val, g_settings.color_grading), 0, 1);
        else if (key == "graphics.brightness") g_settings.brightness = std::clamp(parse_int(val, g_settings.brightness), 0, 100);
        else if (key == "graphics.contrast") g_settings.contrast = std::clamp(parse_int(val, g_settings.contrast), 0, 100);
        else if (key == "graphics.saturation") g_settings.saturation = std::clamp(parse_int(val, g_settings.saturation), 0, 100);
        else if (key == "world.view_distance") g_settings.view_distance = std::clamp(parse_int(val, g_settings.view_distance), 0, 100);
        else if (key == "fog.distance") g_settings.fog_distance = std::clamp(parse_int(val, g_settings.fog_distance), 10, 100);
        else if (key == "fog.start") g_settings.fog_start = std::clamp(parse_int(val, g_settings.fog_start), 0, 300);
        else if (key == "world.hd_radius") g_settings.world_hd_radius = std::clamp(parse_int(val, g_settings.world_hd_radius), 0, 3);
        else if (key == "world.detail_distance") g_settings.detail_distance = std::clamp(parse_int(val, g_settings.detail_distance), 0, 100);
        else if (key == "world.population_distance") g_settings.population_distance = std::clamp(parse_int(val, g_settings.population_distance), 0, 100);
        else if (key == "world.traffic_density" || key == "world.vehicle_density")
            g_settings.traffic_density = std::clamp(parse_int(val, g_settings.traffic_density), 0, 100);
        else if (key == "world.ped_density" || key == "world.pedestrian_density")
            g_settings.ped_density = std::clamp(parse_int(val, g_settings.ped_density), 0, 100);
        else if (key == "world.streaming_memory") g_settings.streaming_memory = std::clamp(parse_int(val, g_settings.streaming_memory), 64, 512);
        else if (key == "ps2.pipeline" || key == "graphics.ps2_pipeline") g_settings.ps2_pipeline = std::clamp(parse_int(val, g_settings.ps2_pipeline), 0, 1);
        else if (key == "ps2.timecycle" || key == "graphics.ps2_timecycle") g_settings.ps2_timecycle = std::clamp(parse_int(val, g_settings.ps2_timecycle), 0, 2);
        else if (key == "ps2.radiosity" || key == "graphics.ps2_radiosity") g_settings.ps2_radiosity = std::clamp(parse_int(val, g_settings.ps2_radiosity), 0, 1);
        else if (key == "ps2.radiosity_intensity" || key == "graphics.ps2_radiosity_intensity") g_settings.ps2_radiosity_intensity = std::clamp(parse_int(val, g_settings.ps2_radiosity_intensity), 0, 200);
        else if (key == "ps2.color_filter" || key == "graphics.ps2_color_filter") g_settings.ps2_color_filter = std::clamp(parse_int(val, g_settings.ps2_color_filter), 0, 1);
        else if (key == "ps2.color_filter_intensity" || key == "graphics.ps2_color_filter_intensity") g_settings.ps2_color_filter_intensity = std::clamp(parse_int(val, g_settings.ps2_color_filter_intensity), 0, 200);
        else if (key == "ps2.trails" || key == "graphics.ps2_trails") g_settings.ps2_trails = std::clamp(parse_int(val, g_settings.ps2_trails), 0, 1);
        else if (key == "ps2.trails_intensity" || key == "graphics.ps2_trails_intensity") g_settings.ps2_trails_intensity = std::clamp(parse_int(val, g_settings.ps2_trails_intensity), 0, 200);
        else if (key == "ps2.speed_blur" || key == "graphics.ps2_speed_blur") g_settings.ps2_speed_blur = std::clamp(parse_int(val, g_settings.ps2_speed_blur), 0, 1);
        else if (key == "audio.volume") g_settings.audio_volume = std::clamp(parse_int(val, g_settings.audio_volume), 0, 100);
        else if (key == "audio.music_volume" || key == "audio.movie_volume")
            g_settings.audio_music = std::clamp(parse_int(val, g_settings.audio_music), 0, 100);
        else if (key == "audio.sfx_volume") g_settings.audio_sfx = std::clamp(parse_int(val, g_settings.audio_sfx), 0, 100);
        else if (key == "mouse.free_aim") g_settings.mouse_free_aim = std::clamp(parse_int(val, g_settings.mouse_free_aim), 0, 1);
        else if (key == "mouse.sensitivity") g_settings.mouse_sensitivity = std::clamp(parse_int(val, g_settings.mouse_sensitivity), 10, 400);
        else if (key == "mouse.invert_y") g_settings.mouse_invert_y = std::clamp(parse_int(val, g_settings.mouse_invert_y), 0, 1);
        else if (key == "ui.font_scale") g_settings.ui_font_scale = std::clamp(parse_int(val, g_settings.ui_font_scale), 90, 140);
        else if (key == "pad.vibration") g_settings.pad_vibration = std::clamp(parse_int(val, g_settings.pad_vibration), 0, 200);
        else if (key == "enhance.prompts") g_settings.button_prompts = std::clamp(parse_int(val, g_settings.button_prompts), 1, 4);
        else if (key == "display.skip_intro") g_settings.skip_intro = std::clamp(parse_int(val, g_settings.skip_intro), 0, 1);
        else if (key == "enhance.free_cam_speed") g_settings.free_cam_speed = std::clamp(parse_int(val, g_settings.free_cam_speed), 25, 400);
        else if (key == "mods.enabled") g_settings.mods_enabled = std::clamp(parse_int(val, g_settings.mods_enabled), 0, 1);
        else if (key == "graphics.texture_replacements")
            g_settings.texture_replacements = std::clamp(parse_int(val, g_settings.texture_replacements), 0, 1);
    }
    return true;
}

bool save(const std::string& path) {
    const std::vector<std::pair<std::string, std::string>> known = {
        {"display.window", std::to_string(g_settings.display_window)},
        {"display.aspect", std::to_string(g_settings.display_aspect)},
        {"display.hud", std::to_string(g_settings.display_hud)},
        {"display.fps", std::to_string(g_settings.display_fps)},
        {"display.vsync", std::to_string(g_settings.display_vsync)},
        {"display.fps_counter", std::to_string(g_settings.display_fps_counter)},
        {"game.language", std::to_string(g_settings.game_language)},
        {"enhance.subtitle_font", std::to_string(g_settings.subtitle_font)},
        {"enhance.subtitle_outline", std::to_string(g_settings.subtitle_outline)},
        {"enhance.subtitle_size", std::to_string(g_settings.subtitle_size)},
        {"enhance.subtitle_shadow", std::to_string(g_settings.subtitle_shadow)},
        {"enhance.subtitle_background", std::to_string(g_settings.subtitle_background)},
        {"enhance.subtitle_colour", std::to_string(g_settings.subtitle_colour)},
        {"display.renderer", std::to_string(g_settings.display_renderer)},
        {"display.scale", std::to_string(g_settings.display_scale)},
        {"display.msaa", std::to_string(g_settings.display_msaa)},
        {"display.aniso", std::to_string(g_settings.display_aniso)},
        {"graphics.ipad_shadows", std::to_string(g_settings.ipad_shadows)},
        {"graphics.stochastic_textures", std::to_string(g_settings.stochastic_textures)},
        {"graphics.color_grading", std::to_string(g_settings.color_grading)},
        {"graphics.brightness", std::to_string(g_settings.brightness)},
        {"graphics.contrast", std::to_string(g_settings.contrast)},
        {"graphics.saturation", std::to_string(g_settings.saturation)},
        {"world.view_distance", std::to_string(g_settings.view_distance)},
        {"fog.distance", std::to_string(g_settings.fog_distance)},
        {"fog.start", std::to_string(g_settings.fog_start)},
        {"world.hd_radius", std::to_string(g_settings.world_hd_radius)},
        {"world.detail_distance", std::to_string(g_settings.detail_distance)},
        {"world.population_distance", std::to_string(g_settings.population_distance)},
        {"world.traffic_density", std::to_string(g_settings.traffic_density)},
        {"world.ped_density", std::to_string(g_settings.ped_density)},
        {"world.streaming_memory", std::to_string(g_settings.streaming_memory)},
        {"ps2.pipeline", std::to_string(g_settings.ps2_pipeline)},
        {"ps2.timecycle", std::to_string(g_settings.ps2_timecycle)},
        {"ps2.radiosity", std::to_string(g_settings.ps2_radiosity)},
        {"ps2.radiosity_intensity", std::to_string(g_settings.ps2_radiosity_intensity)},
        {"ps2.color_filter", std::to_string(g_settings.ps2_color_filter)},
        {"ps2.color_filter_intensity", std::to_string(g_settings.ps2_color_filter_intensity)},
        {"ps2.trails", std::to_string(g_settings.ps2_trails)},
        {"ps2.trails_intensity", std::to_string(g_settings.ps2_trails_intensity)},
        {"ps2.speed_blur", std::to_string(g_settings.ps2_speed_blur)},
        {"audio.volume", std::to_string(g_settings.audio_volume)},
        {"audio.music_volume", std::to_string(g_settings.audio_music)},
        {"audio.sfx_volume", std::to_string(g_settings.audio_sfx)},
        {"mouse.free_aim", std::to_string(g_settings.mouse_free_aim)},
        {"mouse.sensitivity", std::to_string(g_settings.mouse_sensitivity)},
        {"mouse.invert_y", std::to_string(g_settings.mouse_invert_y)},
        {"ui.font_scale", std::to_string(g_settings.ui_font_scale)},
        {"pad.vibration", std::to_string(g_settings.pad_vibration)},
        {"enhance.prompts", std::to_string(g_settings.button_prompts)},
        {"display.skip_intro", std::to_string(g_settings.skip_intro)},
        {"enhance.free_cam_speed", std::to_string(g_settings.free_cam_speed)},
        {"mods.enabled", std::to_string(g_settings.mods_enabled)},
        {"graphics.texture_replacements", std::to_string(g_settings.texture_replacements)},
    };

    std::map<std::string, std::string> lookup(known.begin(), known.end());
    std::vector<std::string> lines;
    std::set<std::string> written;
    {
        std::ifstream in(path);
        std::string raw;
        while (std::getline(in, raw)) {
            if (!raw.empty() && raw.back() == '\r') raw.pop_back();
            std::string t = trim(raw);
            size_t eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos) {
                std::string k = trim(t.substr(0, eq));
                auto it = lookup.find(k);
                if (it != lookup.end()) {
                    lines.push_back(k + "=" + it->second);
                    written.insert(k);
                    continue;
                }
            }
            lines.push_back(raw);
        }
    }
    for (const auto& [k, v] : known) {
        if (!written.count(k)) lines.push_back(k + "=" + v);
    }

    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out.is_open()) return false;
    for (const auto& l : lines) out << l << "\n";
    return out.good();
}

void apply_live() {
    // Sync to core g_config
    lcs::core::g_config.fullscreen = (g_settings.display_window == 0);
    lcs::core::g_config.vsync = (g_settings.display_vsync != 0);
    lcs::core::g_config.max_fps = g_settings.display_fps;
    lcs::core::g_config.brightness = g_settings.brightness;
    lcs::core::g_config.contrast = g_settings.contrast;
    lcs::core::g_config.saturation = g_settings.saturation;

    // Apply window fullscreen & VSync
    if (lcs::graphics::is_window_open()) {
        if (lcs::graphics::is_fullscreen() != lcs::core::g_config.fullscreen) {
            lcs::graphics::set_fullscreen(lcs::core::g_config.fullscreen);
        }
    }
    lcs::graphics::RenderBridge::instance().set_vsync_enabled(lcs::core::g_config.vsync);

    int stochastic_mode = g_settings.stochastic_textures;
    if (const char* e = std::getenv("LCS_STOCHASTIC"); e && *e) stochastic_mode = std::atoi(e); // A/B tests
    lcs::graphics::RenderBridge::instance().set_stochastic_mode(stochastic_mode);

    // Apply iPad Color Grading
    if (g_settings.color_grading) {
        const float bri_norm = static_cast<float>(g_settings.brightness) / 100.0f;
        const float con_norm = static_cast<float>(g_settings.contrast) / 100.0f;
        const float sat_norm = static_cast<float>(g_settings.saturation) / 100.0f;
        const float bri_mul = 0.70f + bri_norm * 0.60f;
        const float con_mul = 0.80f + con_norm * 0.64f;
        const float sat_mul = 0.60f + sat_norm * 1.28f;
        lcs::graphics::RenderBridge::instance().set_color_grading(bri_mul, con_mul, sat_mul);
    } else {
        lcs::graphics::RenderBridge::instance().set_color_grading(1.0f, 1.0f, 1.0f);
    }

    // Apply Controls (Free Aim, Mouse Sensitivity, Invert Y)
    lcs::input::PCControls::instance().options_mut().free_aim = (g_settings.mouse_free_aim != 0);
    auto& mouseCfg = lcs::input::MouseAim::instance().config();
    mouseCfg.sensitivity = static_cast<float>(g_settings.mouse_sensitivity) / 100.0f;
    mouseCfg.invert_y = (g_settings.mouse_invert_y != 0);

    // Apply World HD Streaming & Distance / Density Multipliers
    lcs::game::LcsWorldHd::instance().set_radius(g_settings.world_hd_radius);
    lcs::game::LcsDistance::instance().sync_settings_now();
}

} // namespace lcs::settings
