#include "lcs_prompts.h"

#include "cpu/elf_loader.h"
#include "graphics/render_bridge.h"
#include "input/gamepad.h"
#include "input/pc_controls.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lcs::game {

namespace {

// ============================================================================
// Classic Procedural SDF Glyph Art Renderer (ported from VCS Recomp Classic style)
// No external image files / no Kenney PNGs — pure signed-distance shapes + 4x6 stroke font.
// ============================================================================

struct Rgb {
    float r, g, b;
};

constexpr Rgb kDark{0.11f, 0.12f, 0.15f};
constexpr Rgb kRim{0.46f, 0.47f, 0.53f};
constexpr Rgb kWhite{0.94f, 0.94f, 0.96f};
constexpr Rgb kGrey{0.52f, 0.53f, 0.58f};
constexpr Rgb kInner{0.24f, 0.25f, 0.30f};
constexpr Rgb kCapTop{0.93f, 0.93f, 0.95f};
constexpr Rgb kCapSide{0.56f, 0.57f, 0.63f};
constexpr Rgb kCapEdge{0.08f, 0.08f, 0.10f};
constexpr Rgb kCapText{0.10f, 0.10f, 0.12f};
constexpr Rgb kMouseLit{0.96f, 0.47f, 0.24f};

constexpr Rgb kPsCross{0.50f, 0.71f, 1.00f};
constexpr Rgb kPsCircle{1.00f, 0.43f, 0.43f};
constexpr Rgb kPsTriangle{0.32f, 0.84f, 0.65f};
constexpr Rgb kPsSquare{0.96f, 0.56f, 0.84f};

constexpr Rgb kXboxA{0.43f, 0.80f, 0.33f};
constexpr Rgb kXboxB{0.94f, 0.33f, 0.30f};
constexpr Rgb kXboxX{0.31f, 0.57f, 0.94f};
constexpr Rgb kXboxY{0.97f, 0.78f, 0.21f};

struct Canvas {
    int w = 0;
    int h = 0;
    std::vector<float> px; // premultiplied RGBA floats

    template <class Sd>
    void fill(float x0, float y0, float x1, float y1, Rgb c, float alpha, Sd sd) {
        const float s = static_cast<float>(h);
        const int ix0 = std::max(0, static_cast<int>(std::floor(x0 * s)) - 1);
        const int iy0 = std::max(0, static_cast<int>(std::floor(y0 * s)) - 1);
        const int ix1 = std::min(w - 1, static_cast<int>(std::ceil(x1 * s)) + 1);
        const int iy1 = std::min(h - 1, static_cast<int>(std::ceil(y1 * s)) + 1);
        for (int y = iy0; y <= iy1; ++y) {
            for (int x = ix0; x <= ix1; ++x) {
                const float d = sd((x + 0.5f) / s, (y + 0.5f) / s);
                const float cov = std::clamp(0.5f - d * s, 0.0f, 1.0f) * alpha;
                if (cov <= 0.0f) continue;
                float* p = &px[(static_cast<size_t>(y) * w + x) * 4];
                p[0] = c.r * cov + p[0] * (1.0f - cov);
                p[1] = c.g * cov + p[1] * (1.0f - cov);
                p[2] = c.b * cov + p[2] * (1.0f - cov);
                p[3] = cov + p[3] * (1.0f - cov);
            }
        }
    }
};

float sdCircle(float x, float y, float cx, float cy, float r) {
    return std::hypot(x - cx, y - cy) - r;
}

float sdRoundRect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    const float cx = (x0 + x1) * 0.5f;
    const float cy = (y0 + y1) * 0.5f;
    const float hx = (x1 - x0) * 0.5f - r;
    const float hy = (y1 - y0) * 0.5f - r;
    const float qx = std::fabs(x - cx) - hx;
    const float qy = std::fabs(y - cy) - hy;
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

float sdSegment(float x, float y, float ax, float ay, float bx, float by) {
    const float px = x - ax, py = y - ay;
    const float dx = bx - ax, dy = by - ay;
    const float l = dx * dx + dy * dy;
    const float t = l > 0.0f ? std::clamp((px * dx + py * dy) / l, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - dx * t, py - dy * t);
}

float sdConvex(float x, float y, const float* p, int n) {
    float area = 0.0f;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        area += p[2 * i] * p[2 * j + 1] - p[2 * j] * p[2 * i + 1];
    }
    const float sgn = area >= 0.0f ? 1.0f : -1.0f;
    float d = -1e9f;
    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        const float ex = p[2 * j] - p[2 * i];
        const float ey = p[2 * j + 1] - p[2 * i + 1];
        const float len = std::hypot(ex, ey);
        if (len <= 0.0f) continue;
        const float nx = ey / len * sgn;
        const float ny = -ex / len * sgn;
        d = std::max(d, (x - p[2 * i]) * nx + (y - p[2 * i + 1]) * ny);
    }
    return d;
}

struct Stroke { std::vector<float> pts; };
struct GlyphChar { std::vector<Stroke> strokes; float width = 4.0f; };

const std::map<char, GlyphChar>& strokeFont() {
    static const std::map<char, GlyphChar> f = [] {
        const std::pair<char, const char*> defs[] = {
            {'A', "0,6 0,2 1.2,0 2.8,0 4,2 4,6|0,3.6 4,3.6"},
            {'B', "0,3 0,0 2.8,0 3.7,0.8 3.7,2.2 2.8,3 0,3 0,6 3,6 4,5 4,4 3,3"},
            {'C', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5"},
            {'D', "0,0 0,6 2.4,6 4,4.4 4,1.6 2.4,0 0,0"},
            {'E', "4,0 0,0 0,6 4,6|0,3 3,3"},
            {'F', "4,0 0,0 0,6|0,3 3,3"},
            {'G', "4,1 3,0 1,0 0,1 0,5 1,6 3,6 4,5 4,3.4 2.2,3.4"},
            {'H', "0,0 0,6|4,0 4,6|0,3 4,3"},
            {'I', "0.6,0 3.4,0|2,0 2,6|0.6,6 3.4,6"},
            {'J', "4,0 4,5 3,6 1,6 0,5"},
            {'K', "0,0 0,6|4,0 0,3.6|1.3,2.7 4,6"},
            {'L', "0,0 0,6 4,6"},
            {'M', "0,6 0,0 2.5,3.6 5,0 5,6"},
            {'N', "0,6 0,0 4,6 4,0"},
            {'O', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
            {'P', "0,6 0,0 3,0 4,1 4,2 3,3 0,3"},
            {'Q', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0|2.5,4.5 4.2,6.2"},
            {'R', "0,6 0,0 3,0 4,1 4,2 3,3 0,3|2,3 4,6"},
            {'S', "4,1 3,0 1,0 0,1 0,2 1,3 3,3 4,4 4,5 3,6 1,6 0,5"},
            {'T', "0,0 4,0|2,0 2,6"},
            {'U', "0,0 0,5 1,6 3,6 4,5 4,0"},
            {'V', "0,0 2,6 4,0"},
            {'W', "0,0 1.2,6 2.5,2.2 3.8,6 5,0"},
            {'X', "0,0 4,6|4,0 0,6"},
            {'Y', "0,0 2,3 4,0|2,3 2,6"},
            {'Z', "0,0 4,0 0,6 4,6"},
            {'0', "1,0 3,0 4,1 4,5 3,6 1,6 0,5 0,1 1,0"},
            {'1', "0.8,1.2 2,0 2,6|0.8,6 3.2,6"},
            {'2', "0,1 1,0 3,0 4,1 4,2 0,6 4,6"},
            {'3', "0,0 4,0 2,2.5 3,2.5 4,3.5 4,5 3,6 1,6 0,5"},
            {'4', "3,6 3,0 0,4 4,4"},
            {'5', "4,0 0,0 0,2.6 3,2.6 4,3.6 4,5 3,6 0,6"},
            {'6', "3.5,0 1.5,0 0,1.5 0,5 1,6 3,6 4,5 4,4 3,3 0,3"},
            {'7', "0,0 4,0 1.5,6"},
            {'8', "1,3 0,2 0,1 1,0 3,0 4,1 4,2 3,3 1,3 0,4 0,5 1,6 3,6 4,5 4,4 3,3"},
            {'9', "4,3 1,3 0,2 0,1 1,0 3,0 4,1 4,4.5 2.5,6 0.5,6"},
            {'+', "2,1.5 2,4.5|0.5,3 3.5,3"},
            {'-', "0.5,3 3.5,3"},
            {'=', "0.5,2 3.5,2|0.5,4 3.5,4"},
            {'/', "0.5,6 3.5,0"},
            {'\\', "0.5,0 3.5,6"},
            {'.', "0.9,5.9 1.1,5.9"},
            {',', "1.2,5.4 0.6,6.8"},
            {';', "1.1,2.2 1.1,2.4|1.2,5.4 0.6,6.8"},
            {'\'', "1,0 1,2"},
            {'`', "0.5,0 1.5,1.5"},
            {'[', "2,0 0.5,0 0.5,6 2,6"},
            {']', "0.5,0 2,0 2,6 0.5,6"},
            {'?', "0,1 1,0 3,0 4,1 4,2 2,3.4 2,4.2|2,5.8 2,6"},
        };
        std::map<char, GlyphChar> m;
        for (const auto& [c, def] : defs) {
            GlyphChar ch;
            float maxX = 0.0f;
            std::stringstream strokes(def);
            std::string stroke;
            while (std::getline(strokes, stroke, '|')) {
                Stroke s;
                std::stringstream points(stroke);
                std::string point;
                while (points >> point) {
                    const size_t comma = point.find(',');
                    const float x = std::strtof(point.c_str(), nullptr);
                    const float y = std::strtof(point.c_str() + comma + 1, nullptr);
                    s.pts.push_back(x);
                    s.pts.push_back(y);
                    maxX = std::max(maxX, x);
                }
                ch.strokes.push_back(std::move(s));
            }
            ch.width = std::max(maxX, 1.0f);
            m[c] = std::move(ch);
        }
        return m;
    }();
    return f;
}

constexpr float kLetterGap = 1.7f;
constexpr float kSpace = 2.6f;

float textWidth(const std::string& text, float capH) {
    const float k = capH / 6.0f;
    float w = 0.0f;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto it = strokeFont().find(static_cast<char>(std::toupper(static_cast<unsigned char>(text[i]))));
        w += (it != strokeFont().end() ? it->second.width : kSpace) + (i + 1 < text.size() ? kLetterGap : 0.0f);
    }
    return w * k;
}

void drawText(Canvas& c, const std::string& text, float x, float cy, float capH, Rgb col) {
    const float k = capH / 6.0f;
    const float top = cy - capH * 0.5f;
    const float sw = capH * 0.16f;
    for (char raw : text) {
        const auto it = strokeFont().find(static_cast<char>(std::toupper(static_cast<unsigned char>(raw))));
        if (it == strokeFont().end()) {
            x += (kSpace + kLetterGap) * k;
            continue;
        }
        const GlyphChar& ch = it->second;
        std::vector<float> segs;
        for (const Stroke& s : ch.strokes) {
            for (size_t i = 0; i + 3 < s.pts.size(); i += 2) {
                segs.push_back(x + s.pts[i] * k);
                segs.push_back(top + s.pts[i + 1] * k);
                segs.push_back(x + s.pts[i + 2] * k);
                segs.push_back(top + s.pts[i + 3] * k);
            }
        }
        const float half = sw * 0.5f;
        c.fill(x - sw, top - sw, x + ch.width * k + sw, top + 7.0f * k + sw, col, 1.0f, [&](float px, float py) {
            float d = 1e9f;
            for (size_t i = 0; i < segs.size(); i += 4)
                d = std::min(d, sdSegment(px, py, segs[i], segs[i + 1], segs[i + 2], segs[i + 3]));
            return d - half;
        });
        x += (ch.width + kLetterGap) * k;
    }
}

void drawTextCentred(Canvas& c, const std::string& text, float cx, float cy, float capH, Rgb col) {
    drawText(c, text, cx - textWidth(text, capH) * 0.5f, cy, capH, col);
}

void drawArrow(Canvas& c, float cx, float cy, int dir, float size, Rgb col) {
    float p[6];
    const float s = size, b = size * 0.62f;
    switch (dir) {
        case 0: p[0] = cx; p[1] = cy - s * 0.6f; p[2] = cx + s; p[3] = cy + b * 0.6f; p[4] = cx - s; p[5] = cy + b * 0.6f; break;
        case 1: p[0] = cx; p[1] = cy + s * 0.6f; p[2] = cx - s; p[3] = cy - b * 0.6f; p[4] = cx + s; p[5] = cy - b * 0.6f; break;
        case 2: p[0] = cx - s * 0.6f; p[1] = cy; p[2] = cx + b * 0.6f; p[3] = cy - s; p[4] = cx + b * 0.6f; p[5] = cy + s; break;
        default: p[0] = cx + s * 0.6f; p[1] = cy; p[2] = cx - b * 0.6f; p[3] = cy + s; p[4] = cx - b * 0.6f; p[5] = cy - s; break;
    }
    c.fill(cx - s - 0.05f, cy - s - 0.05f, cx + s + 0.05f, cy + s + 0.05f, col, 1.0f,
           [&](float x, float y) { return sdConvex(x, y, p, 3) - size * 0.08f; });
}

void drawDisc(Canvas& c, float cx) {
    c.fill(cx - 0.5f, 0.0f, cx + 0.5f, 1.0f, kDark, 1.0f,
           [&](float x, float y) { return sdCircle(x, y, cx, 0.5f, 0.45f); });
    c.fill(cx - 0.5f, 0.0f, cx + 0.5f, 1.0f, kRim, 1.0f,
           [&](float x, float y) { return std::fabs(sdCircle(x, y, cx, 0.5f, 0.425f)) - 0.022f; });
}

void drawFaceButton(Canvas& c, PromptDevice dev, const char* btn) {
    drawDisc(c, 0.5f);
    constexpr float sw = 0.042f;
    if (dev == PromptDevice::Switch) {
        const char* letter = (std::strcmp(btn, "south") == 0) ? "B" :
                             (std::strcmp(btn, "east") == 0) ? "A" :
                             (std::strcmp(btn, "west") == 0) ? "Y" : "X";
        drawTextCentred(c, letter, 0.5f, 0.5f, 0.42f, kWhite);
        return;
    }
    if (dev == PromptDevice::Xbox) {
        const char* letter = (std::strcmp(btn, "south") == 0) ? "A" :
                             (std::strcmp(btn, "east") == 0) ? "B" :
                             (std::strcmp(btn, "west") == 0) ? "X" : "Y";
        const Rgb col = (std::strcmp(btn, "south") == 0) ? kXboxA :
                        (std::strcmp(btn, "east") == 0) ? kXboxB :
                        (std::strcmp(btn, "west") == 0) ? kXboxX : kXboxY;
        drawTextCentred(c, letter, 0.5f, 0.5f, 0.42f, col);
        return;
    }
    // PlayStation
    if (std::strcmp(btn, "south") == 0) {
        c.fill(0.2f, 0.2f, 0.8f, 0.8f, kPsCross, 1.0f, [&](float x, float y) {
            return std::min(sdSegment(x, y, 0.33f, 0.33f, 0.67f, 0.67f),
                            sdSegment(x, y, 0.67f, 0.33f, 0.33f, 0.67f)) - sw;
        });
    } else if (std::strcmp(btn, "east") == 0) {
        c.fill(0.2f, 0.2f, 0.8f, 0.8f, kPsCircle, 1.0f,
               [&](float x, float y) { return std::fabs(sdCircle(x, y, 0.5f, 0.5f, 0.175f)) - sw; });
    } else if (std::strcmp(btn, "north") == 0) {
        const float t[6] = {0.5f, 0.30f, 0.69f, 0.63f, 0.31f, 0.63f};
        c.fill(0.2f, 0.2f, 0.8f, 0.8f, kPsTriangle, 1.0f, [&](float x, float y) {
            return std::min({sdSegment(x, y, t[0], t[1], t[2], t[3]),
                             sdSegment(x, y, t[2], t[3], t[4], t[5]),
                             sdSegment(x, y, t[4], t[5], t[0], t[1])}) - sw;
        });
    } else {
        c.fill(0.2f, 0.2f, 0.8f, 0.8f, kPsSquare, 1.0f,
               [&](float x, float y) { return std::fabs(sdRoundRect(x, y, 0.335f, 0.335f, 0.665f, 0.665f, 0.02f)) - sw; });
    }
}

void drawPillSquare(Canvas& c, const std::string& label, bool trigger) {
    const float y0 = trigger ? 0.14f : 0.22f;
    const float y1 = trigger ? 0.86f : 0.78f;
    const float r = trigger ? 0.26f : 0.20f;
    c.fill(0.0f, 0.0f, 1.0f, 1.0f, kDark, 1.0f,
           [&](float x, float y) { return sdRoundRect(x, y, 0.06f, y0, 0.94f, y1, r); });
    c.fill(0.0f, 0.0f, 1.0f, 1.0f, kRim, 1.0f,
           [&](float x, float y) { return std::fabs(sdRoundRect(x, y, 0.08f, y0 + 0.02f, 0.92f, y1 - 0.02f, r)) - 0.02f; });
    float capH = label.size() <= 2 ? 0.30f : (label.size() <= 4 ? 0.21f : 0.15f);
    const float tw = textWidth(label, capH);
    if (tw > 0.72f) capH *= (0.72f / tw);
    drawTextCentred(c, label, 0.5f, 0.5f, capH, kWhite);
}

void drawDpad(Canvas& c, int lit) {
    auto arms = [](float x, float y) {
        return std::min(sdRoundRect(x, y, 0.35f, 0.07f, 0.65f, 0.93f, 0.07f),
                        sdRoundRect(x, y, 0.07f, 0.35f, 0.93f, 0.65f, 0.07f));
    };
    c.fill(0.0f, 0.0f, 1.0f, 1.0f, kDark, 1.0f, arms);
    c.fill(0.0f, 0.0f, 1.0f, 1.0f, kRim, 1.0f, [&](float x, float y) { return std::fabs(arms(x, y) + 0.02f) - 0.02f; });
    if (lit & 1) drawArrow(c, 0.5f, 0.23f, 0, 0.12f, kWhite);
    if (lit & 2) drawArrow(c, 0.5f, 0.77f, 1, 0.12f, kWhite);
    if (lit & 4) drawArrow(c, 0.23f, 0.5f, 2, 0.12f, kWhite);
    if (lit & 8) drawArrow(c, 0.77f, 0.5f, 3, 0.12f, kWhite);
}

void drawStick(Canvas& c, PromptDevice dev, bool left) {
    drawDisc(c, 0.5f);
    const bool xbox = (dev == PromptDevice::Xbox);
    c.fill(0.15f, 0.15f, 0.85f, 0.85f, kGrey, 1.0f,
           [&](float x, float y) { return std::fabs(sdCircle(x, y, 0.5f, 0.5f, 0.21f)) - 0.02f; });
    drawTextCentred(c, xbox ? (left ? "LS" : "RS") : (left ? "L" : "R"), 0.5f, 0.5f, xbox ? 0.15f : 0.20f, kWhite);
    const float d = 0.33f, s = 0.095f;
    drawArrow(c, 0.5f, 0.5f - d, 0, s, kWhite);
    drawArrow(c, 0.5f, 0.5f + d, 1, s, kWhite);
    drawArrow(c, 0.5f - d, 0.5f, 2, s, kWhite);
    drawArrow(c, 0.5f + d, 0.5f, 3, s, kWhite);
}

void drawKeycapRect(Canvas& c, float x0, float y0, float x1, float y1, const std::string& label, int arrow = -1) {
    const float h = y1 - y0;
    const float r = h * 0.17f;
    const float b1 = h * 0.045f;
    const float b2 = h * 0.08f;
    const float bot = h * 0.20f;
    c.fill(x0, y0, x1, y1, kCapEdge, 1.0f,
           [&](float px, float py) { return sdRoundRect(px, py, x0, y0, x1, y1, r); });
    c.fill(x0, y0, x1, y1, kCapSide, 1.0f,
           [&](float px, float py) { return sdRoundRect(px, py, x0 + b1, y0 + b1, x1 - b1, y1 - b1, r * 0.82f); });
    c.fill(x0, y0, x1, y1, kCapTop, 1.0f,
           [&](float px, float py) { return sdRoundRect(px, py, x0 + b2, y0 + b2 * 0.9f, x1 - b2, y1 - bot, r * 0.70f); });
    const float cx = (x0 + x1) * 0.5f;
    const float cy = y0 + (h - bot + b2 * 0.9f) * 0.5f;
    if (arrow >= 0) {
        drawArrow(c, cx, cy, arrow, h * 0.15f, kCapText);
    } else if (!label.empty()) {
        float capH = (label.size() <= 2) ? (h * 0.39f) : (h * 0.27f);
        const float maxW = (x1 - x0) - b2 * 2.6f;
        const float tw = textWidth(label, capH);
        if (tw > maxW && tw > 0.0f) capH *= (maxW / tw);
        drawTextCentred(c, label, cx, cy, capH, kCapText);
    }
}

void drawMouseIcon(Canvas& c, float cx, float cy, float scale, int lit, int wheel, bool moveArrows) {
    // Mouse body centered at (cx, cy), height = 0.90 * scale, width = 0.50 * scale
    const float bw = 0.50f * scale;
    const float bh = 0.88f * scale;
    const float bx0 = cx - bw * 0.5f;
    const float bx1 = cx + bw * 0.5f;
    const float by0 = cy - bh * 0.5f;
    const float by1 = cy + bh * 0.5f;
    const float midX = cx;
    const float splitY = by0 + bh * 0.42f;
    const float rad = bw * 0.48f;

    auto body = [&](float px, float py) { return sdRoundRect(px, py, bx0, by0, bx1, by1, rad); };
    c.fill(bx0 - 0.05f, by0 - 0.05f, bx1 + 0.05f, by1 + 0.05f, kCapEdge, 1.0f,
           [&](float px, float py) { return body(px, py) - 0.032f * scale; });
    c.fill(bx0 - 0.05f, by0 - 0.05f, bx1 + 0.05f, by1 + 0.05f, kCapTop, 1.0f, body);

    if (lit == 1 || lit == 2) {
        const float hx0 = (lit == 1) ? bx0 : midX;
        const float hx1 = (lit == 1) ? midX : bx1;
        c.fill(bx0, by0, bx1, splitY + 0.02f, kMouseLit, 1.0f, [&](float px, float py) {
            return std::max(body(px, py) + 0.025f * scale, sdRoundRect(px, py, hx0, by0 - 0.02f, hx1, splitY, 0.0f));
        });
    }
    c.fill(bx0, by0, bx1, splitY + 0.03f, kCapEdge, 1.0f, [&](float px, float py) {
        return std::min(sdSegment(px, py, midX, by0, midX, splitY),
                        sdSegment(px, py, bx0, splitY, bx1, splitY)) - 0.016f * scale;
    });
    c.fill(midX - 0.06f, by0, midX + 0.06f, splitY, lit == 3 ? kMouseLit : kCapSide, 1.0f, [&](float px, float py) {
        return sdRoundRect(px, py, midX - 0.045f * scale, by0 + bh * 0.09f, midX + 0.045f * scale, by0 + bh * 0.31f, 0.035f * scale);
    });
    if (wheel >= 0) {
        drawArrow(c, bx1 + 0.14f * scale, wheel == 0 ? (cy - 0.16f * scale) : (cy + 0.16f * scale), wheel == 0 ? 0 : 1, 0.11f * scale, kWhite);
    }
    if (moveArrows) {
        drawArrow(c, bx0 - 0.15f * scale, cy, 2, 0.11f * scale, kWhite);
        drawArrow(c, bx1 + 0.15f * scale, cy, 3, 0.11f * scale, kWhite);
    }
}

void drawWasdCluster(Canvas& c) {
    // Classic inverted-T WASD cluster inside 1x1 square
    const float kw = 0.29f, kh = 0.34f, gap = 0.03f;
    const float topY = 0.14f;
    const float botY = topY + kh + gap;
    const float midX = 0.5f;
    drawKeycapRect(c, midX - kw * 0.5f, topY, midX + kw * 0.5f, topY + kh, "W");
    drawKeycapRect(c, midX - kw * 1.5f - gap, botY, midX - kw * 0.5f - gap, botY + kh, "A");
    drawKeycapRect(c, midX - kw * 0.5f, botY, midX + kw * 0.5f, botY + kh, "S");
    drawKeycapRect(c, midX + kw * 0.5f + gap, botY, midX + kw * 1.5f + gap, botY + kh, "D");
}

struct VkLabel {
    std::string text;
    int arrow = -1;
    int mouse_lit = 0; // 1=LMB, 2=RMB, 3=MMB
};

VkLabel vkToLabel(int vk) {
    if (vk == 1) return {"", -1, 1}; // VK_LBUTTON
    if (vk == 2) return {"", -1, 2}; // VK_RBUTTON
    if (vk == 4) return {"", -1, 3}; // VK_MBUTTON
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        return {std::string(1, static_cast<char>(vk)), -1, 0};
    }
    if (vk >= 0x70 && vk <= 0x7B) {
        return {"F" + std::to_string(vk - 0x70 + 1), -1, 0};
    }
    switch (vk) {
        case 0x08: return {"BKSP", -1, 0};
        case 0x09: return {"TAB", -1, 0};
        case 0x0D: return {"ENTER", -1, 0};
        case 0x10: case 0xA0: case 0xA1: return {"SHIFT", -1, 0};
        case 0x11: case 0xA2: case 0xA3: return {"CTRL", -1, 0};
        case 0x12: case 0xA4: case 0xA5: return {"ALT", -1, 0};
        case 0x14: return {"CAPS", -1, 0};
        case 0x1B: return {"ESC", -1, 0};
        case 0x20: return {"SPACE", -1, 0};
        case 0x21: return {"PGUP", -1, 0};
        case 0x22: return {"PGDN", -1, 0};
        case 0x23: return {"END", -1, 0};
        case 0x24: return {"HOME", -1, 0};
        case 0x25: return {"", 2, 0}; // LEFT
        case 0x26: return {"", 0, 0}; // UP
        case 0x27: return {"", 3, 0}; // RIGHT
        case 0x28: return {"", 1, 0}; // DOWN
        case 0x2D: return {"INS", -1, 0};
        case 0x2E: return {"DEL", -1, 0};
        default:   return {"?", -1, 0};
    }
}

void drawVkPrompt(Canvas& c, int vk) {
    const VkLabel l = vkToLabel(vk);
    if (l.mouse_lit > 0) {
        drawMouseIcon(c, 0.5f, 0.5f, 0.96f, l.mouse_lit, -1, false);
        return;
    }
    const bool wide = l.text.size() >= 3;
    const float y0 = wide ? 0.16f : 0.08f;
    const float y1 = wide ? 0.84f : 0.92f;
    const float x0 = wide ? 0.04f : 0.08f;
    const float x1 = wide ? 0.96f : 0.92f;
    drawKeycapRect(c, x0, y0, x1, y1, l.text, l.arrow);
}

// Bleed RGB into transparent pixels so bilinear filtering never produces dark fringes.
std::vector<uint8_t> finalizeCanvasRGBA8(const Canvas& c) {
    const int w = c.w, h = c.h;
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4, 0);
    std::vector<float> rgb(static_cast<size_t>(w) * h * 3, 0.0f);
    std::vector<uint8_t> has(static_cast<size_t>(w) * h, 0);

    for (int i = 0; i < w * h; ++i) {
        const float a = std::clamp(c.px[i * 4 + 3], 0.0f, 1.0f);
        out[i * 4 + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
        if (a > 0.004f) {
            rgb[i * 3 + 0] = std::clamp(c.px[i * 4 + 0] / a, 0.0f, 1.0f);
            rgb[i * 3 + 1] = std::clamp(c.px[i * 4 + 1] / a, 0.0f, 1.0f);
            rgb[i * 3 + 2] = std::clamp(c.px[i * 4 + 2] / a, 0.0f, 1.0f);
            has[i] = 1;
        }
    }

    // 2 passes of neighbor color bleed
    for (int pass = 0; pass < 2; ++pass) {
        auto next_rgb = rgb;
        auto next_has = has;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int idx = y * w + x;
                if (has[idx]) continue;
                float sr = 0.0f, sg = 0.0f, sb = 0.0f;
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        if (nx < 0 || nx >= w) continue;
                        const int ni = ny * w + nx;
                        if (has[ni]) {
                            sr += rgb[ni * 3 + 0];
                            sg += rgb[ni * 3 + 1];
                            sb += rgb[ni * 3 + 2];
                            ++cnt;
                        }
                    }
                }
                if (cnt > 0) {
                    next_rgb[idx * 3 + 0] = sr / cnt;
                    next_rgb[idx * 3 + 1] = sg / cnt;
                    next_rgb[idx * 3 + 2] = sb / cnt;
                    next_has[idx] = 1;
                }
            }
        }
        rgb.swap(next_rgb);
        has.swap(next_has);
    }

    for (int i = 0; i < w * h; ++i) {
        out[i * 4 + 0] = static_cast<uint8_t>(std::lround(rgb[i * 3 + 0] * 255.0f));
        out[i * 4 + 1] = static_cast<uint8_t>(std::lround(rgb[i * 3 + 1] * 255.0f));
        out[i * 4 + 2] = static_cast<uint8_t>(std::lround(rgb[i * 3 + 2] * 255.0f));
    }
    return out;
}

constexpr int kPromptTexSize = 128;

const std::vector<uint8_t>& renderCachedSpec(const std::string& spec) {
    static std::unordered_map<std::string, std::vector<uint8_t>> s_cache;
    auto it = s_cache.find(spec);
    if (it != s_cache.end()) return it->second;

    Canvas c;
    c.w = kPromptTexSize;
    c.h = kPromptTexSize;
    c.px.assign(static_cast<size_t>(c.w) * c.h * 4, 0.0f);

    if (spec == "kb:wasd") {
        drawWasdCluster(c);
    } else if (spec == "kb:mouse_move") {
        drawMouseIcon(c, 0.5f, 0.5f, 0.80f, 0, -1, true);
    } else if (spec.rfind("kb:vk:", 0) == 0) {
        drawVkPrompt(c, std::atoi(spec.c_str() + 6));
    } else if (spec.rfind("pad:", 0) == 0) {
        // pad:<ps|xbox|switch>:<btn>
        const size_t p1 = spec.find(':', 4);
        const std::string fam = spec.substr(4, p1 - 4);
        const std::string btn = spec.substr(p1 + 1);
        const PromptDevice dev = (fam == "xbox") ? PromptDevice::Xbox :
                                 (fam == "switch") ? PromptDevice::Switch : PromptDevice::PlayStation;
        if (btn == "south" || btn == "east" || btn == "west" || btn == "north") {
            drawFaceButton(c, dev, btn.c_str());
        } else if (btn == "l1") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "LB" : (dev == PromptDevice::Switch ? "L" : "L1"), false);
        } else if (btn == "r1") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "RB" : (dev == PromptDevice::Switch ? "R" : "R1"), false);
        } else if (btn == "l2") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "LT" : (dev == PromptDevice::Switch ? "ZL" : "L2"), true);
        } else if (btn == "r2") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "RT" : (dev == PromptDevice::Switch ? "ZR" : "R2"), true);
        } else if (btn == "start") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "MENU" : (dev == PromptDevice::Switch ? "+" : "START"), false);
        } else if (btn == "back") {
            drawPillSquare(c, dev == PromptDevice::Xbox ? "VIEW" : (dev == PromptDevice::Switch ? "-" : "SELECT"), false);
        } else if (btn == "lstick") {
            drawStick(c, dev, true);
        } else if (btn == "rstick") {
            drawStick(c, dev, false);
        } else if (btn == "dup") {
            drawDpad(c, 1);
        } else if (btn == "ddown") {
            drawDpad(c, 2);
        } else if (btn == "dleft") {
            drawDpad(c, 4);
        } else if (btn == "dright") {
            drawDpad(c, 8);
        }
    }

    auto [ins, _] = s_cache.emplace(spec, finalizeCanvasRGBA8(c));
    return ins->second;
}

std::string resolvePromptSpec(const char* symbol, PromptDevice dev, bool in_menu, bool in_vehicle) {
    if (dev != PromptDevice::KeyboardMouse) {
        const char* fam = (dev == PromptDevice::Xbox) ? "xbox" :
                          (dev == PromptDevice::Switch) ? "switch" : "ps";
        if (std::strstr(symbol, "AButton"))    return std::string("pad:") + fam + ":south";
        if (std::strstr(symbol, "BButton"))    return std::string("pad:") + fam + ":east";
        if (std::strstr(symbol, "XButton"))    return std::string("pad:") + fam + ":west";
        if (std::strstr(symbol, "YButton"))    return std::string("pad:") + fam + ":north";
        if (std::strstr(symbol, "_L1"))        return std::string("pad:") + fam + ":l1";
        if (std::strstr(symbol, "_R1"))        return std::string("pad:") + fam + ":r1";
        if (std::strstr(symbol, "_L2"))        return std::string("pad:") + fam + ":l2";
        if (std::strstr(symbol, "_R2"))        return std::string("pad:") + fam + ":r2";
        if (std::strstr(symbol, "LeftStick"))  return std::string("pad:") + fam + ":lstick";
        if (std::strstr(symbol, "RightStick")) return std::string("pad:") + fam + ":rstick";
        if (std::strstr(symbol, "DUp"))        return std::string("pad:") + fam + ":dup";
        if (std::strstr(symbol, "DDown"))      return std::string("pad:") + fam + ":ddown";
        if (std::strstr(symbol, "DLeft"))      return std::string("pad:") + fam + ":dleft";
        if (std::strstr(symbol, "DRight"))     return std::string("pad:") + fam + ":dright";
        if (std::strstr(symbol, "Start"))      return std::string("pad:") + fam + ":start";
        return std::string("pad:") + fam + ":back";
    }

    const auto& b = lcs::input::PCControls::instance().keybinds();
    auto vk = [](int code) { return "kb:vk:" + std::to_string(code); };

    if (std::strstr(symbol, "AButton")) {
        if (in_menu) return vk(b.menu_enter);
        return vk(in_vehicle ? b.move_forward : b.sprint);
    }
    if (std::strstr(symbol, "BButton")) {
        if (in_menu) return vk(b.menu_back);
        return vk(b.fire);
    }
    if (std::strstr(symbol, "XButton")) {
        return vk(in_vehicle ? b.move_backward : b.jump);
    }
    if (std::strstr(symbol, "YButton")) {
        return vk(b.enter_vehicle);
    }
    if (std::strstr(symbol, "_L1")) {
        return vk(in_vehicle ? b.sub_mission : b.horn);
    }
    if (std::strstr(symbol, "_R1")) {
        return vk(in_vehicle ? b.jump : b.aim);
    }
    if (std::strstr(symbol, "_L2")) {
        return vk(in_vehicle ? b.look_left : b.look_behind);
    }
    if (std::strstr(symbol, "_R2")) {
        return vk(in_vehicle ? b.look_right : b.look_behind);
    }
    if (std::strstr(symbol, "LeftStick")) {
        return "kb:wasd";
    }
    if (std::strstr(symbol, "RightStick")) {
        return "kb:mouse_move";
    }
    if (std::strstr(symbol, "DUp")) {
        return vk(b.menu_up);
    }
    if (std::strstr(symbol, "DDown")) {
        return vk(in_vehicle ? b.horn : b.menu_down);
    }
    if (std::strstr(symbol, "DLeft")) {
        return vk(b.prev_weapon);
    }
    if (std::strstr(symbol, "DRight")) {
        return vk(in_vehicle ? b.radio_next : b.next_weapon);
    }
    if (std::strstr(symbol, "Start")) {
        return vk(b.pause);
    }
    return vk(b.camera_mode); // Back / Select
}

uint32_t extractSpriteGLTexId(uint64_t sprite_ptr) {
    if (sprite_ptr < 0x10000ULL) return 0;
    const uint64_t rsl_tex = *reinterpret_cast<const uint64_t*>(sprite_ptr);
    if (rsl_tex < 0x10000ULL) return 0;
    const uint64_t c_tex = *reinterpret_cast<const uint64_t*>(rsl_tex + 0x68ULL);
    if (c_tex < 0x10000ULL) return 0;
    // Managed lglTexture stores GLuint texture ID at +0x04; unmanaged C_Texture stores it at +0xf8.
    const uint64_t lgl_tex = *reinterpret_cast<const uint64_t*>(c_tex + 0xf0ULL);
    if (lgl_tex >= 0x10000ULL) {
        const uint32_t id = *reinterpret_cast<const uint32_t*>(lgl_tex + 0x04ULL);
        if (id != 0) return id;
    }
    return *reinterpret_cast<const uint32_t*>(c_tex + 0xf8ULL);
}

} // namespace

LCSPrompts& LCSPrompts::instance() {
    static LCSPrompts s_inst;
    return s_inst;
}

void LCSPrompts::init(const lcs::cpu::ElfImage& image_game) {
    if (m_initialized) return;
    static constexpr const char* kSymbols[] = {
        "tut_controller_AButton",
        "tut_controller_AButtonLarge",
        "tut_controller_BButton",
        "tut_controller_BButtonLarge",
        "tut_controller_XButton",
        "tut_controller_XButtonLarge",
        "tut_controller_YButton",
        "tut_controller_YButtonLarge",
        "tut_controller_L1",
        "tut_controller_L2",
        "tut_controller_R1",
        "tut_controller_R2",
        "tut_controller_LeftStick",
        "tut_controller_RightStick",
        "tut_controller_DUp",
        "tut_controller_DDown",
        "tut_controller_DLeft",
        "tut_controller_DRight",
        "tut_controller_Start",
        "tut_controller_Back",
    };

    m_slot_count = 0;
    for (const char* sym : kSymbols) {
        const uint64_t addr = image_game.find_symbol(sym);
        if (addr && m_slot_count < 20) {
            m_slots[m_slot_count++] = Slot{sym, addr, 0, 0};
        }
    }
    m_initialized = (m_slot_count > 0);
    std::cout << "  - Initialized Classic SDF input prompts (" << m_slot_count
              << " tut_controller sprites tracked; KB/Mouse + PS/Xbox/Switch auto-switch).\n";
}

void LCSPrompts::notify_keyboard_mouse() {
    m_device = PromptDevice::KeyboardMouse;
}

void LCSPrompts::notify_gamepad() {
    const auto fam = lcs::input::GamepadDriver::instance().get_family();
    switch (fam) {
        case lcs::input::ControllerFamily::PlayStation:
            m_device = PromptDevice::PlayStation;
            break;
        case lcs::input::ControllerFamily::Nintendo:
            m_device = PromptDevice::Switch;
            break;
        case lcs::input::ControllerFamily::Xbox:
        case lcs::input::ControllerFamily::Unknown:
        default:
            m_device = PromptDevice::Xbox;
            break;
    }
}

void LCSPrompts::update(bool in_menu, bool in_vehicle) {
    if (!m_initialized) return;
    auto& rb = lcs::graphics::RenderBridge::instance();
    const uint64_t state_hash =
        (static_cast<uint64_t>(m_device) << 2) |
        (in_menu ? 2ULL : 0ULL) |
        (in_vehicle ? 1ULL : 0ULL);

    for (int i = 0; i < m_slot_count; ++i) {
        auto& slot = m_slots[i];
        const uint64_t sprite_ptr = *reinterpret_cast<const uint64_t*>(slot.global_addr);
        if (!sprite_ptr) continue;

        const uint32_t tex_id = extractSpriteGLTexId(sprite_ptr);
        if (tex_id == 0 || !rb.has_texture(tex_id)) continue;

        if (slot.last_tex_id == tex_id && slot.last_state_hash == state_hash) {
            continue;
        }

        const std::string spec = resolvePromptSpec(slot.symbol, m_device, in_menu, in_vehicle);
        const auto& rgba = renderCachedSpec(spec);
        rb.replace_texture_rgba8(tex_id, kPromptTexSize, kPromptTexSize, rgba.data());
        slot.last_tex_id = tex_id;
        slot.last_state_hash = state_hash;
    }
}

} // namespace lcs::game
