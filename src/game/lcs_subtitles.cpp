#include "game/lcs_subtitles.h"
#include "ui/diag_overlay.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "game/lcs_prompts.h"
#include "settings/settings.h"
#include "ui/ui_assets.h"
#include "ui/ui_theme.h"
#include "vfs/logical_fs.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::subtitles {
namespace {

constexpr uint64_t kGotFontInitPerFrameRva = 0x802b60ULL; // CFont::InitPerFrame()
constexpr uint64_t kGotFontPrintStringRva  = 0x8030b8ULL; // CFont::PrintString(float, float, uint16_t*, int*)
constexpr uint64_t kGotFontDetailsRva      = 0x7fbbd8ULL; // CFont::Details*
constexpr uint64_t kGotDrawBordersRva      = 0x801d68ULL; // CCamera::DrawBordersForWideScreen()
constexpr uint64_t kGotPrefsWideScreenRva  = 0x7f9698ULL; // CMenuManager::m_PrefsUseWideScreen*

constexpr uint64_t kRaHudCutsceneSub  = 0x4818a4ULL; // CHud::Draw letterbox subtitle
constexpr uint64_t kRaHudGameplaySub  = 0x4817ecULL; // CHud::Draw gameplay subtitle
constexpr uint64_t kRaFadeCutsceneSub = 0x484aacULL; // CHud::DrawAfterFade letterbox subtitle
constexpr uint64_t kRaFadeGameplaySub = 0x484bc4ULL; // CHud::DrawAfterFade gameplay subtitle
constexpr uint64_t kRaHelpBox         = 0x481dc4ULL; // CHud::Draw tutorial help box

struct QueuedText {
    bool is_help_box = false;
    bool is_cutscene = false;
    float x = 320.0f;
    float y = 397.0f;
    float alpha = 1.0f;
    float bg_alpha = 0.0f;
    std::string raw_tagged;
};

struct ColorSeg {
    enum class Kind { Text, Token, NewLine };
    Kind kind = Kind::Text;
    std::string text;
    ImU32 color = IM_COL32(220, 220, 220, 255);
};

std::mutex g_sub_mutex;
std::vector<QueuedText> g_sub_queue;
bool g_letterbox_active = false;

// Other CHud texts redrawn in Oswald where the game places them, with its alignment, scale,
// colour and drop shadow (CFont::Details). Positions are the game's 640x448 virtual canvas.
struct HudText {
    float x = 0.0f, y = 0.0f;
    float scale_y = 1.0f;
    float wrap_w = 0.0f;  // centred texts wrap at this virtual width (0: one line)
    int align = 0;        // 0 left, 1 centre, 2 right
    int style = 1;        // CFont style: 0 the title font, 1 the HUD / subtitle font
    ImU32 color = 0;
    uint8_t alpha = 255;
    float shadow = 0.0f;  // drop shadow offset (virtual px)
    ImU32 shadow_color = 0;
    std::string raw_tagged;
};
std::vector<HudText> g_hud_queue;

// CFont::PrintString return addresses of those texts, found with the rig
// (LCS_DIAG_HUDTIMER: COnscreenTimer::AddClock / AddCounter and CMessages::AddBigMessage).
enum class HudKind { None, Line, Wrapped, Pricedown };
HudKind classify_hud_ra(uint64_t ra_rva) {
    switch (ra_rva) {
    case 0x480908ULL:  // COnscreenTimer clock value ("0:58"), right-justified left of the HUD
    case 0x480a08ULL:  // clock label
    case 0x481080ULL:  // counter value ("7/10")
    case 0x480b44ULL:  // counter label ("Nível")
    case 0x4806d8ULL:  // bar counter label
    case 0x485414ULL:  // CHud::DrawAfterFade big message style 1: the mission title, bottom right
        return HudKind::Line;
    case 0x484e7cULL:  // big message styles 3, 4 and 5: side-mission messages, centred
    case 0x484f54ULL:
    case 0x4851acULL:
        return HudKind::Wrapped;
    case 0x482614ULL:  // big message style 0 ("Missão cumprida"), Pricedown, centred
    case 0x48273cULL:  // big message style 2 ("Wasted" / "Busted"), Pricedown, centred
        return HudKind::Pricedown;
    default:
        return HudKind::None;
    }
}
float g_letterbox_top_frac = 0.8973f;
uint64_t g_load_bias = 0;
uint64_t g_using_japanese_addr = 0;
uint64_t g_using_russian_addr  = 0;
std::vector<uint32_t> g_jp_met_table;
std::vector<uint32_t> g_ru_met_table;

std::vector<uint32_t> load_font_met_table(const char* rel_path) {
    std::vector<uint32_t> table;
    char resolved[1024]{};
    const char* phys = lcs::vfs::LogicalFS::instance().resolve_bundle_path(
        rel_path, resolved, sizeof(resolved));
    std::string path = (phys && *phys) ? std::string(phys)
                                       : (std::string("assets/ipad/") + rel_path);
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        const std::string fallback = std::string("assets/ipad/") + rel_path;
        if (path != fallback) ifs.open(fallback);
    }
    if (!ifs.is_open()) {
        std::cerr << "  [Subtitles] Failed to load font MET table: " << rel_path << "\n";
        return table;
    }
    std::string line;
    for (int i = 0; i < 3 && std::getline(ifs, line); ++i) {}
    while (std::getline(ifs, line)) {
        const char* p = line.c_str();
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) continue;
        char* endp = nullptr;
        const unsigned long cp = std::strtoul(p, &endp, 10);
        if (endp != p) {
            table.push_back(static_cast<uint32_t>(cp));
        }
    }
    std::cout << "  [Subtitles] Loaded MET table " << rel_path
              << " (" << table.size() << " glyphs).\n";
    return table;
}

bool is_japanese_active() {
    return g_using_japanese_addr &&
           (*reinterpret_cast<const uint8_t*>(g_using_japanese_addr) != 0);
}

bool is_russian_active() {
    return g_using_russian_addr &&
           (*reinterpret_cast<const uint8_t*>(g_using_russian_addr) != 0);
}

uint64_t handle_draw_borders(const uint64_t* args) {
    if (!args || !args[0] || !g_load_bias) return 0;
    const auto* ws_ptr = *reinterpret_cast<const uint8_t* const*>(g_load_bias + kGotPrefsWideScreenRva);
    if (ws_ptr && *ws_ptr != 0) return 0;
    const float red = *reinterpret_cast<const float*>(args[0] + 0x154ULL);
    // Exact formula from CCamera::DrawBordersForWideScreen (0x327ff4..0x32801c):
    // top_y_448 = (-red / 100.0f) * 224.0f + 448.0f - 46.0f
    const float top_y_448 = (-red / 100.0f) * 224.0f + 402.0f;
    const float top_frac = std::clamp(top_y_448 / 448.0f, 0.50f, 0.92f);
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    g_letterbox_active = true;
    g_letterbox_top_frac = top_frac;
    return 0;
}

void patch_bytes(lcs::cpu::DynarmicCore& cpu, uint64_t guest_va, const void* src, size_t sz) {
#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    auto* dst = reinterpret_cast<void*>(guest_va);
    VirtualProtect(dst, sz, PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(dst, src, sz);
    VirtualProtect(dst, sz, old_prot, &old_prot);
#else
    std::memcpy(reinterpret_cast<void*>(guest_va), src, sz);
#endif
    cpu.invalidate_cache(guest_va, sz);
}

uint64_t patch_got(lcs::cpu::DynarmicCore& cpu, uint64_t load_bias, uint64_t got_rva, uint64_t target,
                   const lcs::cpu::ElfImage& image, const char* sym) {
    const uint64_t slot_va = load_bias + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot_va);
    if (!orig && sym) orig = image.find_symbol(sym);
    patch_bytes(cpu, slot_va, &target, sizeof(uint64_t));
    return orig;
}

void append_utf8_cp(std::string& out, uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

uint32_t decode_rockstar_cp(uint16_t code) {
    // Same Rockstar LCS font1.png Latin-1 extended table used in lcs_trainer.cpp
    static constexpr uint32_t kMap[96] = {
        0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
        0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
        0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
        0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
        0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,
        0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x00A1,0x00BF,0x0020,0x2122,0x0020,0x0020,0x0020,0x0020,0x0020,
    };
    if (code >= 0x80u && code < 0xE0u) {
        return kMap[code - 0x80u];
    }
    if (code == 0x2019u || code == 0x2018u) return '\'';
    if (code == 0x201Cu || code == 0x201Du) return '"';
    if (code == 0x2026u) return '.';
    return code;
}

bool decode_u16_preserve_tags(const uint16_t* u16, std::string& out_utf8, bool& out_has_cjk) {
    out_utf8.clear();
    out_has_cjk = false;
    if (!u16) return false;

    const std::vector<uint32_t>* met_table = nullptr;
    if (is_japanese_active() && !g_jp_met_table.empty()) {
        met_table = &g_jp_met_table;
        out_has_cjk = true;
    } else if (is_russian_active() && !g_ru_met_table.empty()) {
        met_table = &g_ru_met_table;
    }

    for (int i = 0; i < 1024 && u16[i] != 0; ++i) {
        const uint16_t ch = u16[i];
        uint32_t cp = 0;
        if (met_table && ch >= 0x20u && static_cast<size_t>(ch - 0x20u) < met_table->size()) {
            cp = (*met_table)[ch - 0x20u];
        } else if (ch >= 0x3000u) {
            out_has_cjk = true;
            cp = ch;
        } else {
            cp = decode_rockstar_cp(ch);
        }
        if (cp >= 0x3000u) {
            out_has_cjk = true;
        }
        append_utf8_cp(out_utf8, cp);
    }
    return !out_utf8.empty();
}

ImU32 gta_tag_color(char c, ImU32 base_col, bool force_white, uint8_t alpha) {
    if (force_white) {
        return IM_COL32(245, 245, 247, alpha);
    }
    switch (std::tolower(static_cast<unsigned char>(c))) {
        case 'r': return IM_COL32(228, 56, 56, alpha);
        case 'g': return IM_COL32(72, 200, 96, alpha);
        case 'b': return IM_COL32(92, 165, 240, alpha);
        case 'y': return IM_COL32(245, 198, 66, alpha);
        case 'p': return IM_COL32(198, 128, 245, alpha);
        case 'o': return IM_COL32(245, 146, 58, alpha);
        case 'h': return IM_COL32(255, 255, 255, alpha);
        case 'l': return IM_COL32(30, 30, 30, alpha);
        case 'w':
        case 's': return IM_COL32(236, 236, 240, alpha);
        default:  return (base_col & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24);
    }
}

std::vector<ColorSeg> parse_tagged_segments(const std::string& raw, ImU32 base_col, bool force_white, uint8_t alpha) {
    std::vector<ColorSeg> out;
    ImU32 cur_col = force_white ? IM_COL32(245, 245, 247, alpha)
                                : ((base_col & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << 24));
    std::string buf;
    auto flush_text = [&]() {
        if (!buf.empty()) {
            out.push_back({ColorSeg::Kind::Text, std::move(buf), cur_col});
            buf.clear();
        }
    };
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '~') {
            const size_t close = raw.find('~', i + 1);
            if (close != std::string::npos && close - i <= 4) {
                const std::string tag = raw.substr(i + 1, close - i - 1);
                if (tag == "n" || tag == "N") {
                    flush_text();
                    out.push_back({ColorSeg::Kind::NewLine, {}, cur_col});
                } else if (tag == "X") {
                    flush_text();
                    out.push_back({ColorSeg::Kind::Token, "CROSS", cur_col});
                } else if (tag == "O") {
                    flush_text();
                    out.push_back({ColorSeg::Kind::Token, "CIRCLE", cur_col});
                } else if (tag == "S") {
                    flush_text();
                    out.push_back({ColorSeg::Kind::Token, "SQUARE", cur_col});
                } else if (tag == "T") {
                    flush_text();
                    out.push_back({ColorSeg::Kind::Token, "TRIANGLE", cur_col});
                } else if (tag.size() == 1) {
                    flush_text();
                    cur_col = gta_tag_color(tag[0], base_col, force_white, alpha);
                }
                i = close;
                continue;
            }
        }
        if (raw[i] == ':' && i + 1 < raw.size() && raw[i + 1] == ':') {
            const size_t close = raw.find("::", i + 2);
            if (close != std::string::npos && close > i + 2 && close - (i + 2) <= 24) {
                flush_text();
                out.push_back({ColorSeg::Kind::Token, raw.substr(i + 2, close - (i + 2)), cur_col});
                i = close + 1;
                continue;
            }
        }
        if (raw[i] == '\n') {
            flush_text();
            out.push_back({ColorSeg::Kind::NewLine, {}, cur_col});
            continue;
        }
        buf.push_back(raw[i]);
    }
    flush_text();
    return out;
}

std::vector<ColorSeg> tokenize_words(const std::vector<ColorSeg>& segs) {
    auto utf8_char_len = [](const std::string& txt, size_t idx) -> size_t {
        const unsigned char b = static_cast<unsigned char>(txt[idx]);
        if (b < 0x80u) return 1;
        if ((b & 0xE0u) == 0xC0u) return (idx + 1 < txt.size()) ? 2 : 1;
        if ((b & 0xF0u) == 0xE0u) return (idx + 2 < txt.size()) ? 3 : 1;
        if ((b & 0xF8u) == 0xF0u) return (idx + 3 < txt.size()) ? 4 : 1;
        return 1;
    };
    auto is_break_after_cjk = [&](const std::string& txt, size_t idx, size_t clen) -> bool {
        // Split 3-byte UTF-8 CJK characters (U+3000..U+FFFF: lead byte >= 0xE3)
        // so Japanese sentences without spaces wrap cleanly inside help boxes & subtitles.
        return clen >= 3 && static_cast<unsigned char>(txt[idx]) >= 0xE3u;
    };
    auto is_cjk_trailing_punct = [&](const std::string& txt, size_t idx, size_t clen) -> bool {
        if (clen == 1) {
            const char c = txt[idx];
            return c == '.' || c == ',' || c == '!' || c == '?' || c == ')' || c == ']';
        }
        if (clen == 3 && static_cast<unsigned char>(txt[idx]) == 0xE3u &&
            static_cast<unsigned char>(txt[idx + 1]) == 0x80u) {
            // U+3001 (、), U+3002 (。), U+300D (」), U+300F (』)
            const unsigned char b2 = static_cast<unsigned char>(txt[idx + 2]);
            return b2 == 0x81u || b2 == 0x82u || b2 == 0x8Du || b2 == 0x8Fu;
        }
        if (clen == 3 && static_cast<unsigned char>(txt[idx]) == 0xEFu &&
            static_cast<unsigned char>(txt[idx + 1]) == 0xBCu) {
            // U+FF01 (！), U+FF09 (）), U+FF0C (，), U+FF0E (．), U+FF1F (？)
            const unsigned char b2 = static_cast<unsigned char>(txt[idx + 2]);
            return b2 == 0x81u || b2 == 0x89u || b2 == 0x8Cu || b2 == 0x8Eu || b2 == 0x9Fu;
        }
        return false;
    };

    std::vector<ColorSeg> words;
    for (const auto& s : segs) {
        if (s.kind != ColorSeg::Kind::Text) {
            words.push_back(s);
            continue;
        }
        size_t pos = 0;
        while (pos < s.text.size()) {
            if (s.text[pos] == ' ') {
                size_t end = pos;
                while (end < s.text.size() && s.text[end] == ' ') ++end;
                words.push_back({ColorSeg::Kind::Text, " ", s.color});
                pos = end;
                continue;
            }
            const size_t clen = utf8_char_len(s.text, pos);
            if (is_break_after_cjk(s.text, pos, clen)) {
                size_t end = pos + clen;
                while (end < s.text.size()) {
                    const size_t nlen = utf8_char_len(s.text, end);
                    if (!is_cjk_trailing_punct(s.text, end, nlen)) break;
                    end += nlen;
                }
                words.push_back({ColorSeg::Kind::Text, s.text.substr(pos, end - pos), s.color});
                pos = end;
            } else {
                size_t end = pos;
                while (end < s.text.size() && s.text[end] != ' ') {
                    const size_t nlen = utf8_char_len(s.text, end);
                    if (is_break_after_cjk(s.text, end, nlen)) break;
                    end += nlen;
                }
                if (end == pos) end = pos + clen;
                words.push_back({ColorSeg::Kind::Text, s.text.substr(pos, end - pos), s.color});
                pos = end;
            }
        }
    }
    return words;
}

int active_prompt_platform_for_hud() {
    const int manual = lcs::settings::get().button_prompts;
    if (manual == 2) return 1; // PlayStation
    if (manual == 3) return 0; // Xbox
    if (manual == 4) return 3; // Keyboard & Mouse
    using Dev = lcs::game::PromptDevice;
    const auto dev = lcs::game::LCSPrompts::instance().active_device();
    if (dev == Dev::KeyboardMouse) return 3;
    if (dev == Dev::Xbox) return 0;
    return 1;
}

const char* token_fallback_label(const std::string& tok) {
    const bool kbm = (lcs::game::LCSPrompts::instance().active_device() == lcs::game::PromptDevice::KeyboardMouse);
    if (tok == "ACCEL" || tok == "DRIVE") return kbm ? "W" : "R2";
    if (tok == "BRAKE") return kbm ? "S" : "L2";
    if (tok == "HANDBRAKE") return kbm ? "SPACE" : "R1";
    if (tok == "ATTACK" || tok == "PEDFIRE") return kbm ? "LMB" : "O";
    if (tok == "LOCKON") return kbm ? "RMB" : "R1";
    if (tok == "GETINCAR" || tok == "GETONBIKE" || tok == "EXITVEH") return kbm ? "F" : "TRI";
    if (tok == "JUMP") return kbm ? "SPACE" : "[]";
    if (tok == "SPRINT") return kbm ? "SHIFT" : "X";
    if (tok == "CROUCH" || tok == "BTNL3" || tok == "L3" || tok == "BTNR3" || tok == "R3") return kbm ? "C" : "R3";
    if (tok == "CAMERA") return kbm ? "V" : "SELECT";
    if (tok == "HORN") return kbm ? "H" : "L3";
    if (tok == "LOOKBEHIND") return kbm ? "Q/E" : "L2+R2";
    if (tok == "PREVWEAPON") return kbm ? "WHEEL-" : "L2";
    if (tok == "NEXTWEAPON") return kbm ? "WHEEL+" : "R2";
    if (tok == "SIREN") return kbm ? "CAPS" : "L3";
    if (tok == "DPAD" || tok == "BTNDPAD") return kbm ? "WASD" : "D-PAD";
    if (tok == "LANALOG" || tok == "LSTICK" || tok == "BTNLEFTSTICK") return kbm ? "WASD" : "LS";
    if (tok == "RANALOG" || tok == "RSTICK" || tok == "BTNRIGHTSTICK") return kbm ? "MOUSE" : "RS";
    if (tok == "CROSS" || tok == "BTNA") return kbm ? "SPACE" : "X";
    if (tok == "CIRCLE" || tok == "BTNB") return kbm ? "LMB" : "O";
    if (tok == "SQUARE" || tok == "BTNX") return kbm ? "SPACE" : "[]";
    if (tok == "TRIANGLE" || tok == "BTNY") return kbm ? "F" : "TRI";
    return tok.c_str();
}

uint64_t handle_print_string(const uint64_t* args) {
    const auto& cfg = lcs::settings::get();
    if (cfg.subtitle_font != 1) {
        return 0; // Original bitmap font
    }

    const uint64_t str_va = args[0];
    const uint64_t ra_va  = args[1];
    if (!str_va || !g_load_bias || ra_va < g_load_bias) {
        return 0;
    }
    const uint64_t ra_rva = ra_va - g_load_bias;

    const bool is_cut  = (ra_rva == kRaHudCutsceneSub) || (ra_rva == kRaFadeCutsceneSub);
    const bool is_sub  = is_cut || (ra_rva == kRaHudGameplaySub) || (ra_rva == kRaFadeGameplaySub);
    const bool is_help = (ra_rva == kRaHelpBox);

    static const bool s_diag_verify = (std::getenv("LCS_DIAG_VERIFY") != nullptr);
    if (s_diag_verify && (is_sub || is_help || (ra_rva >= 0x47db64ULL && ra_rva < 0x485500ULL))) {
        std::string preview_utf8;
        bool preview_cjk = false;
        const uint16_t* u16_dbg = reinterpret_cast<const uint16_t*>(str_va);
        if (u16_dbg && u16_dbg[0]) decode_u16_preserve_tags(u16_dbg, preview_utf8, preview_cjk);
        static std::string s_last_vkey;
        const std::string vkey = std::to_string(ra_rva) + "|" + preview_utf8;
        if (!preview_utf8.empty() && vkey != s_last_vkey && (is_help || preview_utf8.find("::") != std::string::npos || preview_utf8.find("~") != std::string::npos || ra_rva == 0x481480ULL || is_cut)) {
            s_last_vkey = vkey;
            std::cout << "  [VerifySub] ra=0x" << std::hex << ra_rva << std::dec
                      << " is_help=" << is_help << " is_cut=" << is_cut
                      << " cjk=" << preview_cjk
                      << " dev=" << static_cast<int>(lcs::game::LCSPrompts::instance().active_device())
                      << " txt=\"" << preview_utf8 << "\"\n" << std::flush;
        }
    }

    // LCS_DIAG_HUDTEXT=1: every distinct (return address, text) drawn through PrintString,
    // with position and the CFont details block head (finding which HUD element is which).
    // Also on while the F3 panel is open, so a player can record the HUD texts of a mission.
    static const bool s_diag_hud = (std::getenv("LCS_DIAG_HUDTEXT") != nullptr);
    if (s_diag_hud || lcs::ui_diag::is_open()) {
        std::string t;
        bool cjk = false;
        const uint16_t* u = reinterpret_cast<const uint16_t*>(str_va);
        if (u[0]) decode_u16_preserve_tags(u, t, cjk);
        static std::unordered_set<std::string> s_seen;
        const std::string key = std::to_string(ra_rva) + "|" + t.substr(0, 24);
        if (!t.empty() && s_seen.size() < 400 && s_seen.insert(key).second) {
            float fx = 0.0f, fy = 0.0f;
            const uint32_t bx = static_cast<uint32_t>(args[2]), by = static_cast<uint32_t>(args[3]);
            std::memcpy(&fx, &bx, 4);
            std::memcpy(&fy, &by, 4);
            const auto* d = *reinterpret_cast<const uint8_t* const*>(g_load_bias + kGotFontDetailsRva);
            std::cout << "  [HudText] ra=0x" << std::hex << ra_rva << std::dec << " x=" << fx << " y=" << fy;
            if (d) {
                std::cout << " det=";
                for (int i = 0; i < 0x48; i += 4) {
                    uint32_t w;
                    std::memcpy(&w, d + i, 4);
                    std::cout << std::hex << w << std::dec << (i + 4 < 0x48 ? "," : "");
                }
            }
            std::cout << " txt=\"" << t.substr(0, 60) << "\"\n" << std::flush;
            // Also to a file next to lcs.exe (players start it without a console)
            static FILE* s_log = std::fopen("lcs_hudtext.log", "a");
            if (s_log) {
                std::fprintf(s_log, "ra=0x%llx x=%.1f y=%.1f txt=\"%s\"\n",
                             static_cast<unsigned long long>(ra_rva), fx, fy, t.substr(0, 60).c_str());
                std::fflush(s_log);
            }
        }
    }

    if (const HudKind hk = classify_hud_ra(ra_rva); hk != HudKind::None) {
        if (hk == HudKind::Pricedown && !ui::fontPricedown()) return 0;  // no Pricedown Bl.otf: game font
        const uint16_t* hu = reinterpret_cast<const uint16_t*>(str_va);
        const auto* det = *reinterpret_cast<const uint8_t* const*>(g_load_bias + kGotFontDetailsRva);
        if (!hu[0] || !det) return 0;
        HudText ht;
        bool cjk = false;
        if (!decode_u16_preserve_tags(hu, ht.raw_tagged, cjk) || cjk) return 0;  // CJK keeps the game's font
        const uint32_t bx = static_cast<uint32_t>(args[2]), by = static_cast<uint32_t>(args[3]);
        std::memcpy(&ht.x, &bx, 4);
        std::memcpy(&ht.y, &by, 4);
        float scale_y = 1.0f, alpha_fade = 255.0f, centre_size = 0.0f, shadow = 0.0f;
        std::memcpy(&scale_y, det + 0x08, 4);
        std::memcpy(&alpha_fade, det + 0x24, 4);
        std::memcpy(&centre_size, det + 0x30, 4);
        int16_t style = 1, shadow_pos = 0;
        std::memcpy(&style, det + 0x38, 2);
        std::memcpy(&shadow_pos, det + 0x40, 2);
        shadow = static_cast<float>(shadow_pos);
        ht.scale_y = std::clamp(scale_y, 0.1f, 4.0f);
        ht.align = det[0x1a] ? 2 : (det[0x19] ? 1 : 0);
        ht.wrap_w = (hk != HudKind::Line && ht.align == 1) ? centre_size : 0.0f;
        ht.style = style;
        ht.color = IM_COL32(det[0], det[1], det[2], 255);
        const float a_fade = (alpha_fade >= 0.0f && alpha_fade <= 255.0f) ? alpha_fade / 255.0f : 1.0f;
        ht.alpha = static_cast<uint8_t>(std::clamp(det[3] / 255.0f * a_fade, 0.0f, 1.0f) * 255.0f + 0.5f);
        ht.shadow = shadow;
        ht.shadow_color = IM_COL32(det[0x44], det[0x45], det[0x46], det[0x47]);
        std::lock_guard<std::mutex> lk(g_sub_mutex);
        if (g_hud_queue.size() < 24) g_hud_queue.push_back(std::move(ht));
        return 1;  // Suppress the native CFont bitmap draw
    }

    if (!is_sub && !is_help) return 0;

    const uint16_t* u16 = reinterpret_cast<const uint16_t*>(str_va);
    if (!u16[0]) return 0;

    float px = 320.0f, py = 397.0f;
    uint32_t xb = static_cast<uint32_t>(args[2]);
    uint32_t yb = static_cast<uint32_t>(args[3]);
    std::memcpy(&px, &xb, sizeof(float));
    std::memcpy(&py, &yb, sizeof(float));

    std::string raw_utf8;
    bool has_cjk = false;
    if (!decode_u16_preserve_tags(u16, raw_utf8, has_cjk)) {
        return 0;
    }

    float alpha = 1.0f;
    float bg_alpha = 0.0f;
    const auto* details_ptr = *reinterpret_cast<const uint8_t* const*>(g_load_bias + kGotFontDetailsRva);
    if (details_ptr) {
        const float alpha_fade = *reinterpret_cast<const float*>(details_ptr + 0x24);
        const uint8_t color_a  = details_ptr[0x03];
        const uint8_t bg_a     = details_ptr[0x2b];
        const uint8_t drop_a   = details_ptr[0x47];
        const float a_col  = color_a / 255.0f;
        const float a_fade = (alpha_fade >= 0.0f && alpha_fade <= 255.0f) ? (alpha_fade / 255.0f) : 1.0f;
        alpha = std::clamp(a_col * a_fade, 0.0f, 1.0f);
        if (is_sub && drop_a > 0 && drop_a < 255) {
            alpha = std::min(alpha, drop_a / 255.0f);
        }
        bg_alpha = bg_a / 255.0f;
    }

    QueuedText qt;
    qt.is_help_box = is_help;
    qt.is_cutscene = is_cut;
    qt.x = px;
    qt.y = py;
    qt.alpha = alpha;
    qt.bg_alpha = bg_alpha;
    qt.raw_tagged = std::move(raw_utf8);

    std::lock_guard<std::mutex> lk(g_sub_mutex);
    if (g_sub_queue.size() < 8) {
        g_sub_queue.push_back(std::move(qt));
    }
    return 1; // Suppress native CFont bitmap draw
}

struct RenderedRun {
    ColorSeg::Kind kind = ColorSeg::Kind::Text;
    std::string text;
    ImU32 color = 0xFFFFFFFFu;
    float width = 0.0f;
    int plat = 1;
};

void layout_wrapped_lines(ImFont* font, float font_size, float max_w,
                        const std::vector<ColorSeg>& tokens,
                        std::vector<std::vector<RenderedRun>>& out_lines) {
    out_lines.clear();
    out_lines.emplace_back();
    float cur_w = 0.0f;
    const int plat = active_prompt_platform_for_hud();
    const float icon_w = font_size * 1.15f;

    for (const auto& t : tokens) {
        if (t.kind == ColorSeg::Kind::NewLine) {
            out_lines.emplace_back();
            cur_w = 0.0f;
            continue;
        }
        if (t.kind == ColorSeg::Kind::Token) {
            const auto& tex = ui::assets::inputPromptTexture(plat, t.text.c_str());
            float w = tex.valid() ? icon_w : (font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, token_fallback_label(t.text)).x + font_size * 0.45f);
            if (cur_w > 0.5f && cur_w + w > max_w) {
                out_lines.emplace_back();
                cur_w = 0.0f;
            }
            out_lines.back().push_back({ColorSeg::Kind::Token, t.text, t.color, w, plat});
            cur_w += w;
            continue;
        }
        const float w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, t.text.c_str()).x;
        if (t.text == " " && cur_w < 0.5f) continue; // skip leading space on wrapped line
        if (t.text != " " && cur_w > 0.5f && cur_w + w > max_w) {
            // Trim trailing space on previous line
            if (!out_lines.back().empty() && out_lines.back().back().text == " ") {
                out_lines.back().pop_back();
            }
            out_lines.emplace_back();
            cur_w = 0.0f;
        }
        out_lines.back().push_back({ColorSeg::Kind::Text, t.text, t.color, w, plat});
        cur_w += w;
    }
    while (!out_lines.empty() && out_lines.back().empty()) {
        out_lines.pop_back();
    }
}

void draw_text_with_outline(ImDrawList* dl, ImFont* font, float font_size, ImVec2 pos,
                            const char* text, ImU32 col, uint8_t alpha,
                            float outline_pct, bool drop_shadow) {
    if (drop_shadow) {
        const float sh = std::max(1.2f, font_size * 0.055f);
        dl->AddText(font, font_size, ImVec2(pos.x + sh, pos.y + sh), IM_COL32(0, 0, 0, static_cast<int>(alpha * 0.85f)), text);
    }
    if (outline_pct > 1.0f) {
        const float r = std::max(0.8f, font_size * 0.048f * (outline_pct / 100.0f));
        const ImU32 ocol = IM_COL32(0, 0, 0, alpha);
        static constexpr float kDirs[8][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1},
            {-0.707f, -0.707f}, {0.707f, -0.707f}, {-0.707f, 0.707f}, {0.707f, 0.707f}
        };
        for (const auto& d : kDirs) {
            dl->AddText(font, font_size, ImVec2(pos.x + d[0] * r, pos.y + d[1] * r), ocol, text);
        }
    }
    dl->AddText(font, font_size, pos, col, text);
}

void draw_inline_token(ImDrawList* dl, ImFont* font, float font_size, ImVec2 pos, float w,
                       const std::string& tok, int plat, uint8_t alpha) {
    const auto& tex = ui::assets::inputPromptTexture(plat, tok.c_str());
    const float h = font_size * 1.04f;
    const float y0 = pos.y + (font_size - h) * 0.5f;
    if (tex.valid()) {
        const float pad = (w - h) * 0.5f;
        dl->AddImage(tex.id, ImVec2(pos.x + pad, y0), ImVec2(pos.x + pad + h, y0 + h),
                     ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, alpha));
        return;
    }
    const char* lbl = token_fallback_label(tok);
    const float tw = font->CalcTextSizeA(font_size * 0.82f, FLT_MAX, 0.0f, lbl).x;
    dl->AddRectFilled(ImVec2(pos.x + 1.0f, y0 + 1.0f), ImVec2(pos.x + w - 1.0f, y0 + h - 1.0f),
                      IM_COL32(235, 235, 240, alpha), font_size * 0.16f);
    dl->AddText(font, font_size * 0.82f, ImVec2(pos.x + (w - tw) * 0.5f, pos.y + font_size * 0.09f),
                IM_COL32(14, 14, 18, alpha), lbl);
}

} // namespace

void install_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
#if defined(LCS_PLATFORM_WINDOWS)
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page) {
        std::cerr << "  [Subtitles] Failed to allocate RWX hook page.\n";
        return;
    }
    auto* code = static_cast<uint8_t*>(page);
    const uint64_t base_va = reinterpret_cast<uint64_t>(page);
    const uint64_t load_bias = image_game.get_load_bias();
    g_load_bias = load_bias;

    g_using_japanese_addr = image_game.find_symbol("_ZN5CFont21UsingJapaneseLanguageE");
    if (!g_using_japanese_addr) g_using_japanese_addr = load_bias + 0x8ec094ULL;
    g_using_russian_addr = image_game.find_symbol("_ZN5CFont20UsingRussianLanguageE");
    if (!g_using_russian_addr) g_using_russian_addr = load_bias + 0x8ec095ULL;
    g_jp_met_table = load_font_met_table("Textures_Shared/fonts/JapaneseFont.met");
    g_ru_met_table = load_font_met_table("Textures_Shared/fonts/RussianFont.met");

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t hle_print = hle.register_function("__pc_font_print_string", handle_print_string);

    // Note: Do NOT clear g_sub_queue inside CFont::InitPerFrame(), because libGame.so invokes
    // InitPerFrame() multiple times per frame (including after CHud::Draw() at 0x4b057c / 0x5336e0)
    // before SwapBuffers renders the ImGui overlay. main.cpp clears the queue once per host frame.
    const uint64_t orig_print = patch_got(cpu, load_bias, kGotFontPrintStringRva, base_va + 0x100,
                                          image_game, "_ZN5CFont11PrintStringEffPtPi");

    // Stub 1 (+0x100): CFont::PrintString(float x, float y, uint16_t* str, int* p)
    //   0x00: stp x29, x30, [sp, #-48]!   0xa9bd7bfdu
    //   0x04: stp x0,  x1,  [sp, #16]     0xa90107e0u
    //   0x08: stp d0,  d1,  [sp, #32]     0x6d0207e0u
    //   0x0c: mov x1,  x30                0xaa1e03e1u  (args[1] = caller RA; args[0] already str!)
    //   0x10: fmov w2, s0                 0x1e260002u  (args[2] = float x bits)
    //   0x14: fmov w3, s1                 0x1e260023u  (args[3] = float y bits)
    //   0x18: ldr x16, +0x28 -> 0x40      0x58000150u  (lit_hle_print)
    //   0x1c: blr x16                     0xd63f0200u
    //   0x20: cbnz x0, +0x18 -> 0x38      0xb50000c0u  (if intercepted, skip native PrintString)
    //   0x24: ldp x0,  x1,  [sp, #16]     0xa94107e0u
    //   0x28: ldp d0,  d1,  [sp, #32]     0x6d4207e0u
    //   0x2c: ldp x29, x30, [sp], #48     0xa8c37bfdu
    //   0x30: ldr x16, +0x18 -> 0x48      0x580000d0u  (lit_orig_print)
    //   0x34: br  x16                     0xd61f0200u
    //   0x38: ldp x29, x30, [sp], #48     0xa8c37bfdu
    //   0x3c: ret                         0xd65f03c0u
    //   0x40: hle_print
    //   0x48: orig_print
    const uint32_t stub_print[16] = {
        0xa9bd7bfdu, 0xa90107e0u, 0x6d0207e0u, 0xaa1e03e1u,
        0x1e260002u, 0x1e260023u, 0x58000150u, 0xd63f0200u,
        0xb50000c0u, 0xa94107e0u, 0x6d4207e0u, 0xa8c37bfdu,
        0x580000d0u, 0xd61f0200u, 0xa8c37bfdu, 0xd65f03c0u,
    };
    std::memcpy(code + 0x100, stub_print, sizeof(stub_print));
    std::memcpy(code + 0x140, &hle_print, sizeof(uint64_t));
    std::memcpy(code + 0x148, &orig_print, sizeof(uint64_t));

    // Stub 2 (+0x200): CCamera::DrawBordersForWideScreen(CCamera* this [x0])
    const uint64_t hle_borders = hle.register_function("__pc_draw_borders_ws", handle_draw_borders);
    const uint64_t orig_borders = patch_got(cpu, load_bias, kGotDrawBordersRva, base_va + 0x200,
                                            image_game, "_ZN7CCamera24DrawBordersForWideScreenEv");
    const uint32_t stub_borders[10] = {
        0xa9be7bfdu, // 0x00: stp x29, x30, [sp, #-32]!
        0xf9000be0u, // 0x04: str x0, [sp, #16]
        0x58000110u, // 0x08: ldr x16, +0x20 -> 0x28 (hle_borders)
        0xd63f0200u, // 0x0c: blr x16
        0xf9400be0u, // 0x10: ldr x0, [sp, #16]
        0xa8c27bfdu, // 0x14: ldp x29, x30, [sp], #32
        0x580000d0u, // 0x18: ldr x16, +0x18 -> 0x30 (orig_borders)
        0xd61f0200u, // 0x1c: br  x16
        0xd503201fu, // 0x20: nop
        0xd503201fu, // 0x24: nop
    };
    std::memcpy(code + 0x200, stub_borders, sizeof(stub_borders));
    std::memcpy(code + 0x228, &hle_borders, sizeof(uint64_t));
    std::memcpy(code + 0x230, &orig_borders, sizeof(uint64_t));

    cpu.invalidate_cache(base_va, 0x1000);
    std::cout << "  [Subtitles] CFont Oswald hooks armed.\n";
#else
    (void)image_game; (void)cpu;
#endif
}

void begin_frame() {
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    g_sub_queue.clear();
    g_hud_queue.clear();
    g_letterbox_active = false;
}

bool has_active_items() {
    if (lcs::settings::get().subtitle_font != 1) return false;
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    return !g_sub_queue.empty();
}

bool has_active_hud_items() {
    if (lcs::settings::get().subtitle_font != 1) return false;
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    return !g_hud_queue.empty();
}

bool has_active_help_box() {
    if (lcs::settings::get().subtitle_font != 1) return false;
    std::lock_guard<std::mutex> lk(g_sub_mutex);
    for (const auto& item : g_sub_queue) {
        if (item.is_help_box && item.alpha > 0.05f) return true;
    }
    return false;
}

void draw_overlay(ImDrawList* dl, float display_w, float display_h) {
    if (!dl || display_w <= 0.0f || display_h <= 0.0f) return;
    const auto& cfg = lcs::settings::get();
    if (cfg.subtitle_font != 1) return;

    std::vector<QueuedText> items;
    std::vector<HudText> hud_items;
    bool letterbox_active = false;
    float letterbox_top_frac = 0.8973f;
    {
        std::lock_guard<std::mutex> lk(g_sub_mutex);
        if (g_sub_queue.empty() && g_hud_queue.empty()) return;
        items = g_sub_queue;
        hud_items = g_hud_queue;
        letterbox_active = g_letterbox_active;
        letterbox_top_frac = g_letterbox_top_frac;
    }

    ImFont* font = ui::fontOswald();
    if (!font) font = ui::fontSemibold();
    if (!font) return;

    const float aspect = display_w / display_h;
    const float safe_w = std::min(display_w, display_h * (16.0f / 9.0f));
    const float safe_x = (display_w - safe_w) * 0.5f;
    const float base_h = display_h / 720.0f;

    const float size_mul = std::clamp(cfg.subtitle_size, 80, 150) / 100.0f;
    const float outline_pct = static_cast<float>(std::clamp(cfg.subtitle_outline, 0, 100));
    const bool drop_shadow = (cfg.subtitle_shadow != 0);
    const int bg_pct = std::clamp(cfg.subtitle_background, 0, 100);
    const bool force_white = (cfg.subtitle_colour != 0);

    // CHud texts (timers, counters, mission title, side-mission messages) where the game puts them
    const float vx = display_w / 640.0f, vy = display_h / 448.0f;
    for (const auto& h : hud_items) {
        if (h.alpha == 0) continue;
        // Oswald (Pricedown for style 2) at the bitmap font's glyph height; style 0, the title
        // font, is larger
        ImFont* hf = (h.style == 2 && ui::fontPricedown()) ? ui::fontPricedown() : font;
        const float px_per_scale = h.style == 2 ? 11.5f : (h.style == 0 ? 22.0f : 17.5f);
        const float font_sz = std::max(10.0f, h.scale_y * px_per_scale * vy);
        const float max_w = h.wrap_w > 0.0f ? h.wrap_w * vx : FLT_MAX;
        const ImU32 base = (h.color & 0x00FFFFFFu) | (static_cast<ImU32>(h.alpha) << IM_COL32_A_SHIFT);
        const auto words = tokenize_words(parse_tagged_segments(h.raw_tagged, base, false, h.alpha));
        std::vector<std::vector<RenderedRun>> lines;
        layout_wrapped_lines(hf, font_sz, max_w, words, lines);
        const float sh = h.shadow * vy;
        const uint8_t sh_a = static_cast<uint8_t>(((h.shadow_color >> IM_COL32_A_SHIFT) & 0xFFu) * (h.alpha / 255.0f));
        const ImU32 sh_col = (h.shadow_color & 0x00FFFFFFu) | (static_cast<ImU32>(sh_a) << IM_COL32_A_SHIFT);
        float cy = h.y * vy + font_sz * 0.12f;
        for (const auto& ln : lines) {
            float lw = 0.0f;
            for (const auto& r : ln) lw += r.width;
            float cx = h.x * vx - (h.align == 2 ? lw : h.align == 1 ? lw * 0.5f : 0.0f);
            for (const auto& r : ln) {
                if (r.kind == ColorSeg::Kind::Token) {
                    draw_inline_token(dl, hf, font_sz, ImVec2(cx, cy), r.width, r.text, r.plat, h.alpha);
                } else {
                    if (sh > 0.0f && sh_a > 0) dl->AddText(hf, font_sz, ImVec2(cx + sh, cy + sh), sh_col, r.text.c_str());
                    // (the Pricedown bitmap glyphs carry a thick black outline)
                    draw_text_with_outline(dl, hf, font_sz, ImVec2(cx, cy), r.text.c_str(), r.color, h.alpha,
                                           h.style == 2 ? 160.0f : outline_pct * 0.5f, false);
                }
                cx += r.width;
            }
            cy += font_sz * 1.12f;
        }
    }

    for (const auto& item : items) {
        if (item.alpha <= 0.01f) continue;
        const uint8_t alpha = static_cast<uint8_t>(std::clamp(item.alpha, 0.0f, 1.0f) * 255.0f + 0.5f);

        if (item.is_help_box) {
            const float font_sz = std::max(14.0f, 22.0f * base_h * std::min(size_mul, 1.15f));
            const float pad_x = 16.0f * base_h;
            const float pad_y = 12.0f * base_h;
            const float max_w = std::min(safe_w * 0.36f, 430.0f * base_h);

            const auto segs = parse_tagged_segments(item.raw_tagged, IM_COL32(236, 236, 240, alpha), false, alpha);
            const auto words = tokenize_words(segs);
            std::vector<std::vector<RenderedRun>> lines;
            layout_wrapped_lines(font, font_sz, max_w - pad_x * 2.0f, words, lines);
            if (lines.empty()) continue;

            float actual_w = 0.0f;
            for (const auto& ln : lines) {
                float lw = 0.0f;
                for (const auto& r : ln) lw += r.width;
                actual_w = std::max(actual_w, lw);
            }
            const float line_h = font_sz * 1.24f;
            const float box_w = actual_w + pad_x * 2.0f;
            const float box_h = static_cast<float>(lines.size()) * line_h + pad_y * 2.0f;

            // Keep HelpBox inside the safe top-left HUD margin (or wide left edge with generous safe margin)
            const float bx = safe_x * 0.25f + 28.0f * base_h;
            const float by = 24.0f * base_h;
            const float box_alpha_f = (item.bg_alpha > 0.05f ? item.bg_alpha : 0.78f) * (alpha / 255.0f);
            const uint8_t box_a = static_cast<uint8_t>(std::clamp(box_alpha_f, 0.0f, 0.92f) * 255.0f + 0.5f);

            dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h),
                              IM_COL32(8, 8, 12, box_a), 0.0f);
            dl->AddRect(ImVec2(bx, by), ImVec2(bx + box_w, by + box_h),
                        IM_COL32(255, 255, 255, static_cast<int>(box_a * 0.18f)), 0.0f, 0, 1.0f);

            float cy = by + pad_y;
            for (const auto& ln : lines) {
                float cx = bx + pad_x;
                for (const auto& r : ln) {
                    if (r.kind == ColorSeg::Kind::Token) {
                        draw_inline_token(dl, font, font_sz, ImVec2(cx, cy), r.width, r.text, r.plat, alpha);
                    } else {
                        draw_text_with_outline(dl, font, font_sz, ImVec2(cx, cy), r.text.c_str(),
                                               r.color, alpha, outline_pct * 0.65f, drop_shadow);
                    }
                    cx += r.width;
                }
                cy += line_h;
            }
            continue;
        }

        // Subtitle (bottom-centered in 16:9 safe frame)
        const float font_sz = std::max(16.0f, 30.0f * base_h * size_mul);
        const float max_w = std::min(safe_w * 0.80f, display_w - 48.0f * base_h);
        const auto segs = parse_tagged_segments(item.raw_tagged, IM_COL32(236, 236, 240, alpha), force_white, alpha);
        const auto words = tokenize_words(segs);
        std::vector<std::vector<RenderedRun>> lines;
        layout_wrapped_lines(font, font_sz, max_w, words, lines);
        if (lines.empty()) continue;

        const float line_h = font_sz * 1.18f;
        const float total_h = static_cast<float>(lines.size()) * line_h;
        // Anchor above Rockstar's widescreen letterbox bars whenever black bars are active in cutscenes.
        float bottom_anchor = item.is_cutscene ? (display_h * 0.875f) : (display_h * 0.910f);
        if (letterbox_active) {
            bottom_anchor = std::min(bottom_anchor, display_h * letterbox_top_frac - 14.0f * base_h);
        }
        float cy = bottom_anchor - total_h;
        const float center_x = (aspect > 1.85f) ? (safe_x + safe_w * 0.5f) : (display_w * 0.5f);

        for (const auto& ln : lines) {
            float lw = 0.0f;
            for (const auto& r : ln) lw += r.width;
            const float lx = center_x - lw * 0.5f;

            if (bg_pct > 0) {
                const uint8_t bga = static_cast<uint8_t>((bg_pct / 100.0f) * alpha * 0.88f);
                const float pad_x = 12.0f * base_h;
                const float pad_y = 2.0f * base_h;
                dl->AddRectFilled(ImVec2(lx - pad_x, cy - pad_y),
                                  ImVec2(lx + lw + pad_x, cy + line_h + pad_y),
                                  IM_COL32(6, 6, 10, bga), 0.0f);
            }

            float cx = lx;
            for (const auto& r : ln) {
                if (r.kind == ColorSeg::Kind::Token) {
                    draw_inline_token(dl, font, font_sz, ImVec2(cx, cy), r.width, r.text, r.plat, alpha);
                } else {
                    draw_text_with_outline(dl, font, font_sz, ImVec2(cx, cy), r.text.c_str(),
                                           r.color, alpha, outline_pct, drop_shadow);
                }
                cx += r.width;
            }
            cy += line_h;
        }
    }
}

void append_decoded_u16_char(uint16_t ch, int lang, std::string& out) {
    uint32_t cp = ch;
    if (lang == 6 && ch >= 0x20u && (ch - 0x20u) < g_jp_met_table.size()) {
        const uint32_t mapped = g_jp_met_table[ch - 0x20u];
        if (mapped != 0u) cp = mapped;
    } else if (lang == 5 && ch >= 0x20u && (ch - 0x20u) < g_ru_met_table.size()) {
        const uint32_t mapped = g_ru_met_table[ch - 0x20u];
        if (mapped != 0u) cp = mapped;
    } else if (ch >= 0x80u && ch < 0xE0u) {
        static constexpr uint32_t kRockstarMap[96] = {
            0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
            0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
            0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
            0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
            0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,
            0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x00A1,0x00BF,0x0020,0x2122,0x0020,0x0020,0x0020,0x0020,0x0020,
        };
        cp = kRockstarMap[ch - 0x80u];
    }
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

} // namespace lcs::subtitles
