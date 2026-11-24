#pragma once

#include <cstdint>
#include <string>

struct ImDrawList;

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
}

namespace lcs::subtitles {

// Installs CFont hooks in libGame.so to intercept subtitle and help-box draws
// when Subtitle Font is set to Oswald.
void install_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);

// Clears per-frame captured subtitles/help-boxes at frame begin or menu transition.
void begin_frame();

// Returns true when any intercepted Oswald subtitle or help-box is queued for the current frame.
bool has_active_items();
// Timers, counters, mission title and side-mission messages queued for Oswald this frame.
bool has_active_hud_items();

// Returns true when an intercepted Oswald help-box is queued for the current frame.
bool has_active_help_box();

// Renders any queued Oswald subtitles and help boxes via ImGui overlay at native DPI
// with 16:9 safe positioning even on 21:9 / 32:9 ultrawide displays.
void draw_overlay(ImDrawList* dl, float display_w, float display_h);

// Appends a guest UTF-16 code unit decoded via Japanese/Russian .met glyph tables
// when lang is 6 (Japanese) or 5 (Russian), otherwise encodes Unicode as UTF-8.
void append_decoded_u16_char(uint16_t ch, int lang, std::string& out);

} // namespace lcs::subtitles
