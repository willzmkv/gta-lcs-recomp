#include "core/config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace lcs::core {

Config g_config;

static std::string trim_copy(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static bool parse_bool_val(std::string v, bool fallback) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "on" || v == "true" || v == "1" || v == "yes") return true;
    if (v == "off" || v == "false" || v == "0" || v == "no") return false;
    return fallback;
}

void load_config(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim_copy(line.substr(0, eq));
        std::string val = trim_copy(line.substr(eq + 1));
        if (key.empty() || val.empty()) continue;

        if (key == "width") {
            int w = std::atoi(val.c_str());
            if (w >= 320 && w <= 7680) g_config.width = w;
        } else if (key == "height") {
            int h = std::atoi(val.c_str());
            if (h >= 240 && h <= 4320) g_config.height = h;
        } else if (key == "fullscreen") {
            g_config.fullscreen = parse_bool_val(val, g_config.fullscreen);
        } else if (key == "vsync") {
            g_config.vsync = parse_bool_val(val, g_config.vsync);
        } else if (key == "max_fps") {
            int fps = std::atoi(val.c_str());
            if (fps >= 0 && fps <= 1000) g_config.max_fps = fps;
        } else if (key == "brightness") {
            int b = std::atoi(val.c_str());
            if (b >= 0 && b <= 100) g_config.brightness = b;
        } else if (key == "contrast") {
            int c = std::atoi(val.c_str());
            if (c >= 0 && c <= 100) g_config.contrast = c;
        } else if (key == "saturation") {
            int s = std::atoi(val.c_str());
            if (s >= 0 && s <= 100) g_config.saturation = s;
        }
    }
}

bool save_config_setting(const std::string& path, const std::string& key, const std::string& value) {
    if (key.empty()) return false;
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        if (in.is_open()) {
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(std::move(line));
            }
        }
    }

    bool updated = false;
    for (std::string& line : lines) {
        std::string code = line;
        std::string trailing_comment;
        auto hash = line.find('#');
        if (hash != std::string::npos) {
            code = line.substr(0, hash);
            trailing_comment = line.substr(hash);
        }
        auto eq = code.find('=');
        if (eq == std::string::npos) continue;

        if (trim_copy(code.substr(0, eq)) == key) {
            line = key + "=" + value;
            if (!trailing_comment.empty()) {
                line += " " + trailing_comment;
            }
            updated = true;
            break;
        }
    }

    if (!updated) {
        lines.push_back(key + "=" + value);
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const std::string& line : lines) {
        out << line << "\n";
    }
    return out.good();
}

} // namespace lcs::core
