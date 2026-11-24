#include "render_bridge.h"
#include "window.h"
#include "texture_pipeline.h"

#if defined(LCS_ENABLE_D3D12)
#include "r_d3d12/d3d12_backend.h"
#endif

#include "cpu/dynarmic_core.h"
#include "game/lcs_distance.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>

namespace lcs::graphics {

namespace {

constexpr uint32_t kGLDepthBufferBit   = 0x00000100u;
constexpr uint32_t kGLStencilBufferBit = 0x00000400u;
constexpr uint32_t kGLColorBufferBit   = 0x00004000u;

constexpr uint32_t kGLArrayBuffer        = 0x8892u;
constexpr uint32_t kGLElementArrayBuffer = 0x8893u;
constexpr uint32_t kGLTexture0           = 0x84C0u;

constexpr uint32_t kGLRGBA               = 0x1908u;
constexpr uint32_t kGLRGB                = 0x1907u;
constexpr uint32_t kGLLuminanceAlpha     = 0x190Au;
constexpr uint32_t kGLLuminance          = 0x1909u;
constexpr uint32_t kGLAlpha              = 0x1906u;

size_t estimate_pixel_bytes(uint32_t format, uint32_t type) {
    size_t channels = 4;
    switch (format) {
        case kGLAlpha:
        case kGLLuminance:
        case 0x1903: // GL_RED
            channels = 1;
            break;
        case kGLLuminanceAlpha:
        case 0x8227: // GL_RG
            channels = 2;
            break;
        case kGLRGB:
            channels = 3;
            break;
        case kGLRGBA:
        default:
            channels = 4;
            break;
    }
    if (type == 0x8363 || type == 0x8033 || type == 0x8034) {
        // Packed 16-bit formats: UNSIGNED_SHORT_5_6_5 / 4_4_4_4 / 5_5_5_1
        return 2;
    }
    return channels;
}

uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(std::clamp(v, 0, 255));
}

void unpack_rgb565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    uint8_t r5 = static_cast<uint8_t>((c >> 11) & 0x1F);
    uint8_t g6 = static_cast<uint8_t>((c >> 5) & 0x3F);
    uint8_t b5 = static_cast<uint8_t>(c & 0x1F);
    r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

void decode_etc1_rgba8(uint32_t width, uint32_t height, const uint8_t* src, std::vector<uint8_t>& out) {
    static constexpr int kEtc1Mod[8][4] = {
        { 2,   8,  -2,   -8},
        { 5,  17,  -5,  -17},
        { 9,  29,  -9,  -29},
        {13,  42, -13,  -42},
        {18,  60, -18,  -60},
        {24,  80, -24,  -80},
        {33, 106, -33, -106},
        {47, 183, -47, -183},
    };
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    for (uint32_t by = 0; by < bh; by++) {
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * 8u;
            const bool diff = (blk[3] & 0x02u) != 0;
            const bool flip = (blk[3] & 0x01u) != 0;
            int r0, g0, b0, r1, g1, b1;
            if (diff) {
                auto s3 = [](int v) -> int { return (v & 4) ? (v - 8) : v; };
                int br = blk[0] >> 3;
                int bg = blk[1] >> 3;
                int bb = blk[2] >> 3;
                int dr = s3(blk[0] & 7);
                int dg = s3(blk[1] & 7);
                int db = s3(blk[2] & 7);
                r0 = (br << 3) | (br >> 2);
                g0 = (bg << 3) | (bg >> 2);
                b0 = (bb << 3) | (bb >> 2);
                int rr = std::clamp(br + dr, 0, 31);
                int gg = std::clamp(bg + dg, 0, 31);
                int bb2 = std::clamp(bb + db, 0, 31);
                r1 = (rr << 3) | (rr >> 2);
                g1 = (gg << 3) | (gg >> 2);
                b1 = (bb2 << 3) | (bb2 >> 2);
            } else {
                r0 = ((blk[0] >> 4) * 17);
                r1 = ((blk[0] & 0xF) * 17);
                g0 = ((blk[1] >> 4) * 17);
                g1 = ((blk[1] & 0xF) * 17);
                b0 = ((blk[2] >> 4) * 17);
                b1 = ((blk[2] & 0xF) * 17);
            }
            const int t0 = (blk[3] >> 5) & 7;
            const int t1 = (blk[3] >> 2) & 7;
            const uint16_t msb_bits = static_cast<uint16_t>((blk[4] << 8) | blk[5]);
            const uint16_t lsb_bits = static_cast<uint16_t>((blk[6] << 8) | blk[7]);
            for (uint32_t x = 0; x < 4; x++) {
                const uint32_t px = bx * 4 + x;
                if (px >= width) continue;
                for (uint32_t y = 0; y < 4; y++) {
                    const uint32_t py = by * 4 + y;
                    if (py >= height) continue;
                    const uint32_t p = x * 4 + y;
                    const uint32_t idx = (((msb_bits >> p) & 1u) << 1) | ((lsb_bits >> p) & 1u);
                    const bool second = flip ? (y >= 2) : (x >= 2);
                    const int delta = kEtc1Mod[second ? t1 : t0][idx];
                    const int br = second ? r1 : r0;
                    const int bg = second ? g1 : g0;
                    const int bb = second ? b1 : b0;
                    const size_t dst_off = (static_cast<size_t>(py) * width + px) * 4u;
                    out[dst_off + 0] = clamp_u8(br + delta);
                    out[dst_off + 1] = clamp_u8(bg + delta);
                    out[dst_off + 2] = clamp_u8(bb + delta);
                    out[dst_off + 3] = 255;
                }
            }
        }
    }
}

void decode_dxt_color_block(const uint8_t* blk, bool is_dxt1, uint8_t colors[4][4]) {
    uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    unpack_rgb565(c0, colors[0][0], colors[0][1], colors[0][2]);
    colors[0][3] = 255;
    unpack_rgb565(c1, colors[1][0], colors[1][1], colors[1][2]);
    colors[1][3] = 255;
    if (!is_dxt1 || c0 > c1) {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = static_cast<uint8_t>((2 * colors[0][c] + colors[1][c] + 1) / 3);
            colors[3][c] = static_cast<uint8_t>((colors[0][c] + 2 * colors[1][c] + 1) / 3);
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = static_cast<uint8_t>((colors[0][c] + colors[1][c]) / 2);
            colors[3][c] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
}

bool decode_pvrtc_rgba8(uint32_t width, uint32_t height, bool is_2bpp, bool has_alpha,
                        int32_t image_size, const uint8_t* src, std::vector<uint8_t>& out);

bool decode_compressed_to_rgba8(uint32_t internal_format, uint32_t width, uint32_t height,
                                  int32_t image_size, const void* data, std::vector<uint8_t>& out) {
    if (!data || width == 0 || height == 0) return false;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    const uint32_t bw = (width + 3u) / 4u;
    const uint32_t bh = (height + 3u) / 4u;
    if (internal_format >= 0x8C00 && internal_format <= 0x8C03) {
        const bool is_2bpp = (internal_format == 0x8C01 || internal_format == 0x8C03);
        const bool has_alpha = (internal_format == 0x8C02 || internal_format == 0x8C03);
        return decode_pvrtc_rgba8(width, height, is_2bpp, has_alpha, image_size, src, out);
    }

    out.assign(static_cast<size_t>(width) * height * 4u, 255);

    if (internal_format == 0x8D64) { // GL_ETC1_RGB8_OES
        const size_t need = static_cast<size_t>(bw) * bh * 8u;
        if (image_size > 0 && static_cast<size_t>(image_size) < need) return false;
        decode_etc1_rgba8(width, height, src, out);
        return true;
    }

    if (internal_format == 0x83F0 || internal_format == 0x83F1) { // GL_COMPRESSED_RGB/RGBA_S3TC_DXT1_EXT
        const size_t need = static_cast<size_t>(bw) * bh * 8u;
        if (image_size > 0 && static_cast<size_t>(image_size) < need) return false;
        const bool alpha_1bit = (internal_format == 0x83F1);
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* blk = src + (static_cast<size_t>(by) * bw + bx) * 8u;
                uint8_t pal[4][4];
                decode_dxt_color_block(blk, true, pal);
                uint32_t bits = static_cast<uint32_t>(blk[4]) |
                                (static_cast<uint32_t>(blk[5]) << 8) |
                                (static_cast<uint32_t>(blk[6]) << 16) |
                                (static_cast<uint32_t>(blk[7]) << 24);
                for (uint32_t y = 0; y < 4; y++) {
                    uint32_t py = by * 4 + y;
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x;
                        uint32_t ci = (bits >> ((y * 4 + x) * 2)) & 3u;
                        if (px < width && py < height) {
                            size_t dst_off = (static_cast<size_t>(py) * width + px) * 4u;
                            out[dst_off + 0] = pal[ci][0];
                            out[dst_off + 1] = pal[ci][1];
                            out[dst_off + 2] = pal[ci][2];
                            out[dst_off + 3] = alpha_1bit ? pal[ci][3] : 255;
                        }
                    }
                }
            }
        }
        return true;
    }

    if (internal_format == 0x83F2 || internal_format == 0x83F3) { // GL_COMPRESSED_RGBA_S3TC_DXT3/DXT5_EXT
        const size_t need = static_cast<size_t>(bw) * bh * 16u;
        if (image_size > 0 && static_cast<size_t>(image_size) < need) return false;
        const bool is_dxt5 = (internal_format == 0x83F3);
        for (uint32_t by = 0; by < bh; by++) {
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t* ablk = src + (static_cast<size_t>(by) * bw + bx) * 16u;
                const uint8_t* cblk = ablk + 8;
                uint8_t pal[4][4];
                decode_dxt_color_block(cblk, false, pal);
                uint32_t cbits = static_cast<uint32_t>(cblk[4]) |
                                 (static_cast<uint32_t>(cblk[5]) << 8) |
                                 (static_cast<uint32_t>(cblk[6]) << 16) |
                                 (static_cast<uint32_t>(cblk[7]) << 24);
                uint8_t a_pal[8]{};
                uint64_t abits = 0;
                if (is_dxt5) {
                    a_pal[0] = ablk[0];
                    a_pal[1] = ablk[1];
                    if (a_pal[0] > a_pal[1]) {
                        for (int i = 1; i <= 6; i++)
                            a_pal[i + 1] = static_cast<uint8_t>(((7 - i) * a_pal[0] + i * a_pal[1] + 3) / 7);
                    } else {
                        for (int i = 1; i <= 4; i++)
                            a_pal[i + 1] = static_cast<uint8_t>(((5 - i) * a_pal[0] + i * a_pal[1] + 2) / 5);
                        a_pal[6] = 0;
                        a_pal[7] = 255;
                    }
                    for (int b = 0; b < 6; b++) {
                        abits |= static_cast<uint64_t>(ablk[2 + b]) << (b * 8);
                    }
                }
                for (uint32_t y = 0; y < 4; y++) {
                    uint32_t py = by * 4 + y;
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t px = bx * 4 + x;
                        uint32_t p = y * 4 + x;
                        uint32_t ci = (cbits >> (p * 2)) & 3u;
                        uint8_t a = 255;
                        if (is_dxt5) {
                            a = a_pal[(abits >> (p * 3)) & 7u];
                        } else {
                            uint16_t row_a = static_cast<uint16_t>(ablk[y * 2] | (ablk[y * 2 + 1] << 8));
                            a = static_cast<uint8_t>(((row_a >> (x * 4)) & 0xFu) * 17u);
                        }
                        if (px < width && py < height) {
                            size_t dst_off = (static_cast<size_t>(py) * width + px) * 4u;
                            out[dst_off + 0] = pal[ci][0];
                            out[dst_off + 1] = pal[ci][1];
                            out[dst_off + 2] = pal[ci][2];
                            out[dst_off + 3] = a;
                        }
                    }
                }
            }
        }
        return true;
    }

    return false;
}

uint8_t expand_n_to_8(uint32_t v, int bits) {
    if (bits == 5) return static_cast<uint8_t>((v << 3) | (v >> 2));
    if (bits == 4) return static_cast<uint8_t>((v << 4) | v);
    if (bits == 3) return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1));
    return static_cast<uint8_t>(v);
}

uint32_t pvrtc_twiddle(uint32_t x, uint32_t y, uint32_t nx, uint32_t ny) {
    const uint32_t min_dim = std::min(nx, ny);
    const uint32_t max_val = (ny == min_dim) ? x : y;
    uint32_t twiddled = 0;
    uint32_t src_bit = 1;
    uint32_t dst_bit = 1;
    uint32_t shifts = 0;
    while (src_bit < min_dim) {
        if (y & src_bit) twiddled |= dst_bit;
        dst_bit <<= 1;
        if (x & src_bit) twiddled |= dst_bit;
        dst_bit <<= 1;
        src_bit <<= 1;
        shifts++;
    }
    twiddled |= ((max_val >> shifts) << (2u * shifts));
    return twiddled;
}

struct PvrtcColor {
    uint8_t r, g, b, a;
};

void decode_pvrtc_colors(uint32_t color_word, PvrtcColor& out_a, PvrtcColor& out_b, bool& out_mod_mode) {
    out_mod_mode = (color_word & 1u) != 0;
    if (color_word & (1u << 15)) {
        out_a = {
            expand_n_to_8((color_word >> 10) & 0x1Fu, 5),
            expand_n_to_8((color_word >> 5) & 0x1Fu, 5),
            expand_n_to_8((color_word >> 1) & 0x0Fu, 4),
            255
        };
    } else {
        out_a = {
            expand_n_to_8((color_word >> 8) & 0x0Fu, 4),
            expand_n_to_8((color_word >> 4) & 0x0Fu, 4),
            expand_n_to_8((color_word >> 1) & 0x07u, 3),
            expand_n_to_8((color_word >> 12) & 0x07u, 3)
        };
    }

    if (color_word & (1u << 31)) {
        out_b = {
            expand_n_to_8((color_word >> 26) & 0x1Fu, 5),
            expand_n_to_8((color_word >> 21) & 0x1Fu, 5),
            expand_n_to_8((color_word >> 16) & 0x1Fu, 5),
            255
        };
    } else {
        out_b = {
            expand_n_to_8((color_word >> 24) & 0x0Fu, 4),
            expand_n_to_8((color_word >> 20) & 0x0Fu, 4),
            expand_n_to_8((color_word >> 16) & 0x0Fu, 4),
            expand_n_to_8((color_word >> 28) & 0x07u, 3)
        };
    }
}

bool decode_pvrtc_rgba8(uint32_t width, uint32_t height, bool is_2bpp, bool has_alpha,
                        int32_t image_size, const uint8_t* src, std::vector<uint8_t>& out) {
    const uint32_t word_w = is_2bpp ? 8u : 4u;
    const uint32_t word_h = 4u;
    const uint32_t nx = std::max(2u, (width + word_w - 1u) / word_w);
    const uint32_t ny = std::max(2u, (height + word_h - 1u) / word_h);
    const size_t need = static_cast<size_t>(nx) * ny * 8u;
    if (image_size > 0 && static_cast<size_t>(image_size) < need) return false;

    struct WordInfo {
        uint32_t mod_data;
        PvrtcColor a;
        PvrtcColor b;
        bool mod_mode;
    };
    std::vector<WordInfo> words(static_cast<size_t>(nx) * ny);
    for (uint32_t wy = 0; wy < ny; ++wy) {
        for (uint32_t wx = 0; wx < nx; ++wx) {
            const uint32_t tw = pvrtc_twiddle(wx, wy, nx, ny);
            const uint8_t* p = src + static_cast<size_t>(tw) * 8u;
            const uint32_t mod_w = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
            const uint32_t col_w = static_cast<uint32_t>(p[4]) | (static_cast<uint32_t>(p[5]) << 8) |
                                   (static_cast<uint32_t>(p[6]) << 16) | (static_cast<uint32_t>(p[7]) << 24);
            auto& wi = words[static_cast<size_t>(wy) * nx + wx];
            wi.mod_data = mod_w;
            decode_pvrtc_colors(col_w, wi.a, wi.b, wi.mod_mode);
        }
    }

    auto read_2bpp_weight = [&](uint32_t px, uint32_t py) -> int {
        const uint32_t gx = px % (nx * word_w);
        const uint32_t gy = py % (ny * word_h);
        const uint32_t bx = gx / word_w;
        const uint32_t by = gy / word_h;
        const uint32_t lx = gx % word_w;
        const uint32_t ly = gy % word_h;
        const auto& wi = words[static_cast<size_t>(by) * nx + bx];
        if (!wi.mod_mode) {
            return ((wi.mod_data >> (ly * 8u + lx)) & 1u) ? 8 : 0;
        }
        static constexpr int kRep[4] = {0, 3, 5, 8};
        const uint32_t idx = ly * 4u + (lx >> 1);
        return kRep[(wi.mod_data >> (idx * 2u)) & 3u];
    };

    out.resize(static_cast<size_t>(width) * height * 4u);
    uint8_t* dst_base = out.data();
    const uint32_t grid_w = nx * word_w;
    const uint32_t grid_h = ny * word_h;
    const uint32_t area_shift = is_2bpp ? 5u : 4u; // word_w*word_h is always 32 (2bpp) or 16 (4bpp)
    const uint32_t nx_mask = (nx & (nx - 1u)) == 0u ? (nx - 1u) : 0u;
    const uint32_t ny_mask = (ny & (ny - 1u)) == 0u ? (ny - 1u) : 0u;
    const uint32_t gw_mask = (grid_w & (grid_w - 1u)) == 0u ? (grid_w - 1u) : 0u;
    const uint32_t gh_mask = (grid_h & (grid_h - 1u)) == 0u ? (grid_h - 1u) : 0u;
    auto wrap_nx = [nx, nx_mask](uint32_t v) -> uint32_t { return nx_mask ? (v & nx_mask) : (v % nx); };
    auto wrap_ny = [ny, ny_mask](uint32_t v) -> uint32_t { return ny_mask ? (v & ny_mask) : (v % ny); };
    auto wrap_gw = [grid_w, gw_mask](uint32_t v) -> uint32_t { return gw_mask ? (v & gw_mask) : (v % grid_w); };
    auto wrap_gh = [grid_h, gh_mask](uint32_t v) -> uint32_t { return gh_mask ? (v & gh_mask) : (v % grid_h); };

    for (uint32_t py = 0; py < height; ++py) {
        const uint32_t by = py >> 2;
        const uint32_t ly = py & 3u;
        const uint32_t wy0 = (ly < 2u) ? wrap_ny(by + ny - 1u) : by;
        const uint32_t wy1 = (ly < 2u) ? by : wrap_ny(by + 1u);
        const uint32_t fy = (ly < 2u) ? (ly + 2u) : (ly - 2u);
        const uint32_t w_top = 4u - fy;
        const uint32_t w_bot = fy;
        const WordInfo* row0 = words.data() + static_cast<size_t>(wy0) * nx;
        const WordInfo* row1 = words.data() + static_cast<size_t>(wy1) * nx;
        const WordInfo* cur_row = words.data() + static_cast<size_t>(by) * nx;
        uint8_t* dst = dst_base + static_cast<size_t>(py) * width * 4u;

        for (uint32_t px = 0; px < width; ++px, dst += 4) {
            const uint32_t bx = is_2bpp ? (px >> 3) : (px >> 2);
            const uint32_t lx = is_2bpp ? (px & 7u) : (px & 3u);
            const uint32_t wx0 = (lx < (word_w >> 1)) ? wrap_nx(bx + nx - 1u) : bx;
            const uint32_t wx1 = (lx < (word_w >> 1)) ? bx : wrap_nx(bx + 1u);
            const uint32_t fx = (lx < (word_w >> 1)) ? (lx + (word_w >> 1)) : (lx - (word_w >> 1));

            const WordInfo& c00 = row0[wx0];
            const WordInfo& c10 = row0[wx1];
            const WordInfo& c01 = row1[wx0];
            const WordInfo& c11 = row1[wx1];

            const uint32_t w00 = (word_w - fx) * w_top;
            const uint32_t w10 = fx * w_top;
            const uint32_t w01 = (word_w - fx) * w_bot;
            const uint32_t w11 = fx * w_bot;

            const uint32_t ra = (w00 * c00.a.r + w10 * c10.a.r + w01 * c01.a.r + w11 * c11.a.r) >> area_shift;
            const uint32_t ga = (w00 * c00.a.g + w10 * c10.a.g + w01 * c01.a.g + w11 * c11.a.g) >> area_shift;
            const uint32_t ba = (w00 * c00.a.b + w10 * c10.a.b + w01 * c01.a.b + w11 * c11.a.b) >> area_shift;
            const uint32_t rb = (w00 * c00.b.r + w10 * c10.b.r + w01 * c01.b.r + w11 * c11.b.r) >> area_shift;
            const uint32_t gb = (w00 * c00.b.g + w10 * c10.b.g + w01 * c01.b.g + w11 * c11.b.g) >> area_shift;
            const uint32_t bb = (w00 * c00.b.b + w10 * c10.b.b + w01 * c01.b.b + w11 * c11.b.b) >> area_shift;

            const WordInfo& cur = cur_row[bx];
            int mod_w = 0;
            bool punch = false;

            if (!is_2bpp) {
                const uint32_t mv = (cur.mod_data >> ((ly * 4u + lx) * 2u)) & 3u;
                if (!cur.mod_mode) {
                    static constexpr int kW4[4] = {0, 3, 5, 8};
                    mod_w = kW4[mv];
                } else {
                    static constexpr int kW4P[4] = {0, 4, 4, 8};
                    mod_w = kW4P[mv];
                    punch = (mv == 2);
                }
            } else {
                if (!cur.mod_mode) {
                    mod_w = ((cur.mod_data >> (ly * 8u + lx)) & 1u) ? 8 : 0;
                } else if (((lx ^ ly) & 1u) == 0u) {
                    static constexpr int kW2[4] = {0, 3, 5, 8};
                    const uint32_t idx = ly * 4u + (lx >> 1);
                    mod_w = kW2[(cur.mod_data >> (idx * 2u)) & 3u];
                } else {
                    const uint32_t xl = wrap_gw(px + grid_w - 1u);
                    const uint32_t xr = wrap_gw(px + 1u);
                    const uint32_t yu = wrap_gh(py + grid_h - 1u);
                    const uint32_t yd = wrap_gh(py + 1u);
                    mod_w = (read_2bpp_weight(xl, py) + read_2bpp_weight(xr, py) +
                             read_2bpp_weight(px, yu) + read_2bpp_weight(px, yd) + 2) >> 2;
                }
            }

            const uint32_t inv = 8u - static_cast<uint32_t>(mod_w);
            const uint32_t mw = static_cast<uint32_t>(mod_w);
            dst[0] = static_cast<uint8_t>((ra * inv + rb * mw) >> 3);
            dst[1] = static_cast<uint8_t>((ga * inv + gb * mw) >> 3);
            dst[2] = static_cast<uint8_t>((ba * inv + bb * mw) >> 3);
            if (!has_alpha) {
                dst[3] = 255;
            } else if (punch) {
                dst[3] = 0;
            } else {
                const uint32_t aa = (w00 * c00.a.a + w10 * c10.a.a + w01 * c01.a.a + w11 * c11.a.a) >> area_shift;
                const uint32_t ab = (w00 * c00.b.a + w10 * c10.b.a + w01 * c01.b.a + w11 * c11.b.a) >> area_shift;
                dst[3] = static_cast<uint8_t>((aa * inv + ab * mw) >> 3);
            }
        }
    }
    return true;
}

} // namespace

RenderBridge& RenderBridge::instance() {
    static RenderBridge s_instance;
    return s_instance;
}

RenderBridge::RenderBridge() {
    m_vaos.resize(1);
    m_vaos[0].valid = true;
    m_vaos[0].gen = 1;
    m_active_attribs = &m_vaos[0].attribs;
    for (int i = 0; i < 16; i++) {
        m_default_attribs[i][0] = 0.0f;
        m_default_attribs[i][1] = 0.0f;
        m_default_attribs[i][2] = 0.0f;
        m_default_attribs[i][3] = 1.0f;
    }
}
RenderBridge::~RenderBridge() = default;

bool RenderBridge::initialize(RenderBackend backend, void* native_window, uint32_t width, uint32_t height) {
    m_backend = backend;
    m_width = std::max(1u, width);
    m_height = std::max(1u, height);
    m_guest_width = m_width;
    m_guest_height = m_height;
    m_viewport[0] = 0;
    m_viewport[1] = 0;
    m_viewport[2] = static_cast<int32_t>(m_width);
    m_viewport[3] = static_cast<int32_t>(m_height);
    m_scissor[0] = 0;
    m_scissor[1] = 0;
    m_scissor[2] = static_cast<int32_t>(m_width);
    m_scissor[3] = static_cast<int32_t>(m_height);
    m_has_window = (native_window != nullptr);

#if defined(LCS_ENABLE_D3D12)
    if (backend == RenderBackend::Direct3D12 && native_window != nullptr) {
        if (!d3d12::init_d3d12_device(native_window, m_width, m_height)) {
            std::cerr << "[RenderBridge] Direct3D 12 device initialization failed.\n";
            return false;
        }
        TexturePipeline::instance().start(decode_compressed_to_rgba8);
    }
#else
    (void)native_window;
#endif

    m_initialized = true;
    std::cout << "[RenderBridge] Abstract GPU render bridge initialized ("
              << (backend == RenderBackend::Direct3D12 ? "Direct3D 12" : "Vulkan 1.4")
              << ", " << (m_has_window ? "windowed" : "headless") << ", "
              << m_width << "x" << m_height << ").\n";
    return true;
}

void RenderBridge::shutdown() {
#if defined(LCS_ENABLE_D3D12)
    // Workers may be creating GPU resources: stop them before the device goes away.
    TexturePipeline::instance().stop();
    d3d12::shutdown_d3d12();
#endif
    m_initialized = false;
}

void RenderBridge::wait_for_present_slot() {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::wait_d3d12_frame_latency();
    }
#endif
}

void RenderBridge::begin_frame() {
    {
        std::lock_guard<std::mutex> lock(m_ps2_mutex);
        m_ps2_applied_this_frame = false;
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::begin_d3d12_frame();
    }
#endif
}

static std::atomic<uint64_t> g_tmp_pvr_calls{0}, g_tmp_pvr_px{0}, g_tmp_pvr_dec_ns{0}, g_tmp_pvr_d3d_ns{0}, g_tmp_pvr_pump_ns{0}, g_tmp_pvr_tot_ns{0};
static std::atomic<uint64_t> g_tmp_gltex_calls{0}, g_tmp_gltex_d3d_ns{0}, g_tmp_gltex_tot_ns{0};

void RenderBridge::end_frame_and_present() {
    static const bool s_tex_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    if (s_tex_audit) {
        const uint64_t pc = g_tmp_pvr_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t ppx = g_tmp_pvr_px.exchange(0, std::memory_order_relaxed);
        const uint64_t pdc = g_tmp_pvr_dec_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t pd3 = g_tmp_pvr_d3d_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t ppm = g_tmp_pvr_pump_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t ptt = g_tmp_pvr_tot_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t gc = g_tmp_gltex_calls.exchange(0, std::memory_order_relaxed);
        const uint64_t gd3 = g_tmp_gltex_d3d_ns.exchange(0, std::memory_order_relaxed);
        const uint64_t gtt = g_tmp_gltex_tot_ns.exchange(0, std::memory_order_relaxed);
        const double pm = static_cast<double>(ptt) / 1e6;
        const double gm = static_cast<double>(gtt) / 1e6;
        if (pm > 1.5 || gm > 1.5 || (pc > 0 && (m_frame_count % 240) == 0)) {
            std::cout << "  [TMP_TEX_DECODE] f=" << m_frame_count
                      << " pvr(n=" << pc << " px=" << ppx
                      << " dec=" << (static_cast<double>(pdc) / 1e6)
                      << " d3dPrep=" << (static_cast<double>(pd3) / 1e6)
                      << " pump=" << (static_cast<double>(ppm) / 1e6)
                      << " tot=" << pm << ")"
                      << " glTex(n=" << gc
                      << " d3dPrep=" << (static_cast<double>(gd3) / 1e6)
                      << " tot=" << gm << ")\n" << std::flush;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_ps2_mutex);
        m_ps2_applied_this_frame = false;
    }
    m_frame_count++;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        uint32_t sync = m_vsync_enabled ? 1u : 0u;
        d3d12::end_d3d12_frame_and_present(sync);
    }
#endif
}

void RenderBridge::compute_guest_transform(float& out_scale, float& out_off_x, float& out_off_y,
                                           float& out_fit_w, float& out_fit_h) const {
    if (m_guest_width > 0 && m_guest_height > 0 && m_width > 0 && m_height > 0) {
        const float sx = static_cast<float>(m_width) / static_cast<float>(m_guest_width);
        const float sy = static_cast<float>(m_height) / static_cast<float>(m_guest_height);
        out_scale = std::min(sx, sy);
        out_fit_w = static_cast<float>(m_guest_width) * out_scale;
        out_fit_h = static_cast<float>(m_guest_height) * out_scale;
        out_off_x = (static_cast<float>(m_width) - out_fit_w) * 0.5f;
        out_off_y = (static_cast<float>(m_height) - out_fit_h) * 0.5f;
    } else {
        out_scale = 1.0f;
        out_off_x = 0.0f;
        out_off_y = 0.0f;
        out_fit_w = static_cast<float>(m_width);
        out_fit_h = static_cast<float>(m_height);
    }
}

void RenderBridge::get_content_rect(float& out_x, float& out_y, float& out_w, float& out_h) const {
    float scale = 1.0f;
    compute_guest_transform(scale, out_x, out_y, out_w, out_h);
}

void RenderBridge::set_guest_dimensions(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_guest_width = width;
    m_guest_height = height;
    gl_viewport(0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
    gl_scissor(0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height));
}

void RenderBridge::set_surface_dimensions(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::resize_d3d12_swapchain(width, height);
        gl_viewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
        gl_scissor(m_scissor[0], m_scissor[1], m_scissor[2], m_scissor[3]);
    }
#endif
}

void RenderBridge::set_video_frame(uint32_t width, uint32_t height, const void* rgba_pixels) {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::set_d3d12_video_frame(width, height, rgba_pixels);
    }
#else
    (void)width; (void)height; (void)rgba_pixels;
#endif
}

void RenderBridge::clear_video_frame() {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::clear_d3d12_video_frame();
    }
#endif
}

uint64_t RenderBridge::egl_get_display(uint64_t native_display) {
    (void)native_display;
    constexpr uint64_t EGL_DUMMY_DISPLAY = 0x7f60400000ULL;
    std::cout << "[EGL] eglGetDisplay(native=" << native_display << ") -> 0x"
              << std::hex << EGL_DUMMY_DISPLAY << std::dec << "\n";
    return EGL_DUMMY_DISPLAY;
}

uint32_t RenderBridge::egl_initialize(uint64_t dpy, int32_t* major, int32_t* minor) {
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 5;
    std::cout << "[EGL] eglInitialize(major=1, minor=5) -> EGL_TRUE\n";
    return 1;
}

uint32_t RenderBridge::egl_terminate(uint64_t dpy) {
    (void)dpy;
    std::cout << "[EGL] eglTerminate() -> EGL_TRUE\n";
    return 1;
}

uint32_t RenderBridge::egl_choose_config(uint64_t dpy, const int32_t* attrib_list,
                                         uint64_t* configs, int32_t config_size,
                                         int32_t* num_config) {
    (void)dpy;
    (void)attrib_list;
    constexpr uint64_t EGL_DUMMY_CONFIG = 0x7f60403000ULL;
    if (configs && config_size > 0) {
        configs[0] = EGL_DUMMY_CONFIG;
    }
    if (num_config) {
        *num_config = 1;
    }
    return 1;
}

uint32_t RenderBridge::egl_get_config_attrib(uint64_t dpy, uint64_t config,
                                             int32_t attribute, int32_t* value) {
    (void)dpy;
    (void)config;
    if (!value) return 0;

    switch (attribute) {
        case 0x3024: *value = 8; break;      // EGL_RED_SIZE
        case 0x3023: *value = 8; break;      // EGL_GREEN_SIZE
        case 0x3022: *value = 8; break;      // EGL_BLUE_SIZE
        case 0x3021: *value = 8; break;      // EGL_ALPHA_SIZE
        case 0x3025: *value = 24; break;     // EGL_DEPTH_SIZE
        case 0x3026: *value = 8; break;      // EGL_STENCIL_SIZE
        case 0x3020: *value = 32; break;     // EGL_BUFFER_SIZE
        case 0x3028: *value = 1; break;      // EGL_CONFIG_ID
        case 0x3033: *value = 0x0004; break; // EGL_SURFACE_TYPE (EGL_WINDOW_BIT)
        case 0x3040: *value = 0x0044; break; // EGL_RENDERABLE_TYPE (ES2 | ES3)
        case 0x3031: *value = 0; break;      // EGL_SAMPLES
        case 0x3032: *value = 0; break;      // EGL_SAMPLE_BUFFERS
        default:     *value = 0; break;
    }
    return 1;
}

uint64_t RenderBridge::egl_create_window_surface(uint64_t dpy, uint64_t config,
                                                 uint64_t native_window,
                                                 const int32_t* attrib_list) {
    (void)dpy;
    (void)config;
    (void)native_window;
    (void)attrib_list;
    constexpr uint64_t EGL_DUMMY_SURFACE = 0x7f60402000ULL;
    std::cout << "[EGL] eglCreateWindowSurface -> 0x" << std::hex
              << EGL_DUMMY_SURFACE << std::dec << "\n";
    return EGL_DUMMY_SURFACE;
}

uint64_t RenderBridge::egl_create_context(uint64_t dpy, uint64_t config,
                                          uint64_t share_context,
                                          const int32_t* attrib_list) {
    (void)dpy;
    (void)config;
    (void)share_context;
    (void)attrib_list;
    constexpr uint64_t EGL_DUMMY_CONTEXT = 0x7f60401000ULL;
    std::cout << "[EGL] eglCreateContext -> 0x" << std::hex
              << EGL_DUMMY_CONTEXT << std::dec << "\n";
    return EGL_DUMMY_CONTEXT;
}

uint32_t RenderBridge::egl_make_current(uint64_t dpy, uint64_t draw,
                                        uint64_t read, uint64_t ctx) {
    (void)dpy;
    const bool changed = (m_current_draw_surface != draw ||
                          m_current_read_surface != read ||
                          m_current_context != ctx);
    m_current_draw_surface = draw;
    m_current_read_surface = read;
    m_current_context = ctx;
    if (changed) {
        std::cout << "[EGL] eglMakeCurrent(draw=0x" << std::hex << draw
                  << ", read=0x" << read << ", ctx=0x" << ctx << std::dec << ") -> EGL_TRUE\n";
    }
    return 1;
}

uint32_t RenderBridge::egl_swap_buffers(uint64_t dpy, uint64_t surface) {
    (void)dpy;
    (void)surface;
    end_frame_and_present();
    return 1;
}

void RenderBridge::set_vsync_enabled(bool enabled) {
    m_vsync_enabled = enabled;
}

void RenderBridge::set_color_grading(float brightness_mul, float contrast_mul, float saturation_mul) {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12) {
        d3d12::set_d3d12_color_grading(brightness_mul, contrast_mul, saturation_mul);
    }
#else
    (void)brightness_mul; (void)contrast_mul; (void)saturation_mul;
#endif
}

void RenderBridge::queue_ps2_post_process(const Ps2PostProcessParams& params) {
    std::lock_guard<std::mutex> lock(m_ps2_mutex);
    const bool prev_reset = m_pending_ps2_params.reset_history;
    m_pending_ps2_params = params;
    if (prev_reset && params.enabled) {
        m_pending_ps2_params.reset_history = true;
    }
    m_has_pending_ps2_params = params.enabled;
}

void RenderBridge::flush_pending_ps2_post_process() {
    if (m_bound_fbo != 0) {
        return;
    }
    Ps2PostProcessParams params{};
    {
        std::lock_guard<std::mutex> lock(m_ps2_mutex);
        if (!m_has_pending_ps2_params || m_ps2_applied_this_frame) {
            return;
        }
        params = m_pending_ps2_params;
        m_pending_ps2_params.reset_history = false;
        m_ps2_applied_this_frame = true;
    }
    apply_ps2_post_process(params);
}

void RenderBridge::gl_flush() {
    flush_pending_ps2_post_process();
}

void RenderBridge::apply_ps2_post_process(const Ps2PostProcessParams& params) {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::apply_d3d12_ps2_post_process(params);
    }
#else
    (void)params;
#endif
}

uint32_t RenderBridge::egl_swap_interval(uint64_t dpy, int32_t interval) {
    (void)dpy;
    m_swap_interval = interval;
    return 1;
}

uint32_t RenderBridge::egl_query_surface(uint64_t dpy, uint64_t surface,
                                         int32_t attribute, int32_t* value) {
    (void)dpy;
    (void)surface;
    if (!value) return 0;

    switch (attribute) {
        case 0x3057: *value = static_cast<int32_t>(m_guest_width > 0 ? m_guest_width : m_width); break;
        case 0x3056: *value = static_cast<int32_t>(m_guest_height > 0 ? m_guest_height : m_height); break;
        default:     *value = 0; break;
    }
    return 1;
}

uint32_t RenderBridge::egl_destroy_surface(uint64_t dpy, uint64_t surface) {
    (void)dpy;
    if (m_current_draw_surface == surface) m_current_draw_surface = 0;
    if (m_current_read_surface == surface) m_current_read_surface = 0;
    return 1;
}

uint32_t RenderBridge::egl_destroy_context(uint64_t dpy, uint64_t ctx) {
    (void)dpy;
    if (m_current_context == ctx) m_current_context = 0;
    return 1;
}

uint32_t RenderBridge::egl_get_error() {
    uint32_t err = m_last_error;
    m_last_error = 0x3000;
    return err;
}

// ---------------------------------------------------------------------------
// GLES State & Resource Bridge
// ---------------------------------------------------------------------------

void RenderBridge::gl_viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
    m_viewport[0] = x;
    m_viewport[1] = y;
    m_viewport[2] = width;
    m_viewport[3] = height;
#if defined(LCS_ENABLE_D3D12)
    if (d3d12::is_d3d12_initialized()) {
        if (m_bound_fbo != 0) {
            uint32_t fbo_h = static_cast<uint32_t>(std::max(1, height));
            auto fit = m_framebuffers.find(m_bound_fbo);
            if (fit != m_framebuffers.end() && fit->second.height > 0) {
                fbo_h = fit->second.height;
            }
            float eff_x = static_cast<float>(x);
            float eff_y = static_cast<float>(y);
            float eff_w = static_cast<float>(std::max(1, width));
            float eff_h = static_cast<float>(std::max(1, height));
            float top_y = static_cast<float>(static_cast<int32_t>(fbo_h)) - (eff_y + eff_h);
            d3d12::set_d3d12_viewport(eff_x, top_y, eff_w, eff_h, 0.0f, 1.0f);
            return;
        }

        float eff_x = static_cast<float>(x);
        float eff_y = static_cast<float>(y);
        float eff_w = static_cast<float>(std::max(1, width));
        float eff_h = static_cast<float>(std::max(1, height));

        // Scale uniformly preserving guest aspect ratio (centered if surface aspect differs).
        if (m_guest_width > 0 && m_guest_height > 0 &&
            (m_width != m_guest_width || m_height != m_guest_height) &&
            static_cast<uint32_t>(width) <= m_guest_width + 1 &&
            static_cast<uint32_t>(height) <= m_guest_height + 1) {
            float scale = 1.0f, off_x = 0.0f, off_y = 0.0f, fit_w = 0.0f, fit_h = 0.0f;
            compute_guest_transform(scale, off_x, off_y, fit_w, fit_h);
            eff_x = off_x + eff_x * scale;
            eff_y = off_y + eff_y * scale;
            eff_w *= scale;
            eff_h *= scale;
        }

        // Flip Y from GL bottom-left to D3D12 top-left.
        float top_y = static_cast<float>(static_cast<int32_t>(m_height)) - (eff_y + eff_h);
        d3d12::set_d3d12_viewport(eff_x, top_y, eff_w, eff_h, 0.0f, 1.0f);
    }
#endif
}

void RenderBridge::gl_scissor(int32_t x, int32_t y, int32_t width, int32_t height) {
    m_scissor[0] = x;
    m_scissor[1] = y;
    m_scissor[2] = width;
    m_scissor[3] = height;
#if defined(LCS_ENABLE_D3D12)
    if (d3d12::is_d3d12_initialized()) {
        if (m_bound_fbo != 0) {
            uint32_t fbo_h = static_cast<uint32_t>(std::max(1, height));
            auto fit = m_framebuffers.find(m_bound_fbo);
            if (fit != m_framebuffers.end() && fit->second.height > 0) {
                fbo_h = fit->second.height;
            }
            int32_t top_y = static_cast<int32_t>(fbo_h) - (y + std::max(0, height));
            d3d12::set_d3d12_scissor(x, top_y,
                                     static_cast<uint32_t>(std::max(0, width)),
                                     static_cast<uint32_t>(std::max(0, height)));
            return;
        }

        float eff_x = static_cast<float>(x);
        float eff_y = static_cast<float>(y);
        float eff_w = static_cast<float>(std::max(0, width));
        float eff_h = static_cast<float>(std::max(0, height));

        if (m_guest_width > 0 && m_guest_height > 0 &&
            (m_width != m_guest_width || m_height != m_guest_height) &&
            static_cast<uint32_t>(width) <= m_guest_width + 1 &&
            static_cast<uint32_t>(height) <= m_guest_height + 1) {
            float scale = 1.0f, off_x = 0.0f, off_y = 0.0f, fit_w = 0.0f, fit_h = 0.0f;
            compute_guest_transform(scale, off_x, off_y, fit_w, fit_h);
            eff_x = off_x + eff_x * scale;
            eff_y = off_y + eff_y * scale;
            eff_w *= scale;
            eff_h *= scale;
        }

        int32_t top_y = static_cast<int32_t>(m_height) - static_cast<int32_t>(std::round(eff_y + eff_h));
        d3d12::set_d3d12_scissor(static_cast<int32_t>(std::round(eff_x)), top_y,
                                 static_cast<uint32_t>(std::max(0.0f, std::round(eff_w))),
                                 static_cast<uint32_t>(std::max(0.0f, std::round(eff_h))));
    }
#endif
}

void RenderBridge::gl_clear_color(float r, float g, float b, float a) {
    m_clear_color[0] = r;
    m_clear_color[1] = g;
    m_clear_color[2] = b;
    m_clear_color[3] = a;
}

void RenderBridge::gl_clear_depth(float depth) {
    m_clear_depth = depth;
}

void RenderBridge::gl_clear_stencil(int32_t stencil) {
    m_clear_stencil = stencil;
}

void RenderBridge::gl_clear(uint32_t mask) {
#if defined(LCS_ENABLE_D3D12)
    if (d3d12::is_d3d12_initialized()) {
        d3d12::clear_d3d12_targets(
            (mask & kGLColorBufferBit) != 0, m_clear_color,
            (mask & kGLDepthBufferBit) != 0, m_clear_depth,
            (mask & kGLStencilBufferBit) != 0, static_cast<uint8_t>(m_clear_stencil & 0xFF)
        );
    }
#else
    (void)mask;
#endif
}

uint32_t RenderBridge::gl_create_shader(uint32_t type) {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t id = m_next_shader_id++;
    GLShaderObject obj;
    obj.id = id;
    obj.type = type;
    m_shaders[id] = std::move(obj);
    return id;
}

void RenderBridge::gl_delete_shader(uint32_t shader) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shaders.erase(shader);
}

void RenderBridge::gl_shader_source(uint32_t shader, int32_t count,
                                    const char* const* strings, const int32_t* lengths) {
    if (!strings || count <= 0) return;
    std::string combined;
    for (int32_t i = 0; i < count; i++) {
        if (!strings[i]) continue;
        if (lengths && lengths[i] >= 0) {
            combined.append(strings[i], static_cast<size_t>(lengths[i]));
        } else {
            combined.append(strings[i]);
        }
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_shaders.find(shader);
    if (it != m_shaders.end()) {
        it->second.source = std::move(combined);
    }
}

void RenderBridge::gl_compile_shader(uint32_t shader) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_shaders.find(shader);
    if (it != m_shaders.end()) {
        it->second.preprocessed = preprocess_glsl(it->second.source);
        it->second.compiled = true;
    }
}

uint32_t RenderBridge::gl_create_program() {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t id = m_next_program_id++;
    GLProgramObject prog;
    prog.id = id;
    m_programs[id] = std::move(prog);
    return id;
}

void RenderBridge::gl_delete_program(uint32_t program) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_programs.erase(program);
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::unregister_d3d12_program(program);
    }
#endif
}

void RenderBridge::gl_attach_shader(uint32_t program, uint32_t shader) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto pit = m_programs.find(program);
    auto sit = m_shaders.find(shader);
    if (pit == m_programs.end() || sit == m_shaders.end()) return;
    if (sit->second.type == 0x8B31) {
        pit->second.vertex_shader = shader;
    } else if (sit->second.type == 0x8B30) {
        pit->second.fragment_shader = shader;
    }
}

void RenderBridge::gl_bind_attrib_location(uint32_t program, uint32_t index, const char* name) {
    if (!name) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it != m_programs.end()) {
        it->second.attrib_locations[name] = static_cast<int32_t>(index);
    }
}

void RenderBridge::gl_link_program(uint32_t program) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end()) return;
    auto& prog = it->second;
    prog.active_attribs.clear();
    prog.active_uniforms.clear();
    prog.uniform_locations.clear();
    prog.next_uniform_loc = 1;

    auto vs_it = m_shaders.find(prog.vertex_shader);
    auto fs_it = m_shaders.find(prog.fragment_shader);

    std::unordered_map<std::string, bool> seen_uniforms;
    auto add_uniforms = [&](const std::vector<ShaderVariableInfo>& list) {
        for (const auto& u : list) {
            if (!seen_uniforms[u.name]) {
                seen_uniforms[u.name] = true;
                prog.uniform_locations[u.name] = prog.next_uniform_loc++;
                prog.active_uniforms.push_back(u);
            }
        }
    };

    if (vs_it != m_shaders.end()) {
        if (vs_it->second.preprocessed.empty() && !vs_it->second.source.empty()) {
            vs_it->second.preprocessed = preprocess_glsl(vs_it->second.source);
        }
        ShaderReflection vs_refl = reflect_glsl(vs_it->second.preprocessed);
        for (const auto& attr : vs_refl.attributes) {
            if (prog.attrib_locations.find(attr.name) == prog.attrib_locations.end()) {
                while (true) {
                    int32_t candidate = prog.next_attrib_loc++;
                    bool used = false;
                    for (const auto& [k, v] : prog.attrib_locations) {
                        if (v == candidate) { used = true; break; }
                    }
                    if (!used) {
                        prog.attrib_locations[attr.name] = candidate;
                        break;
                    }
                }
            }
            prog.active_attribs.push_back(attr);
        }
        add_uniforms(vs_refl.uniforms);
    }

    if (fs_it != m_shaders.end()) {
        if (fs_it->second.preprocessed.empty() && !fs_it->second.source.empty()) {
            fs_it->second.preprocessed = preprocess_glsl(fs_it->second.source);
        }
        ShaderReflection fs_refl = reflect_glsl(fs_it->second.preprocessed);
        add_uniforms(fs_refl.uniforms);
    }

    prog.uniform_layout.clear();
    prog.uniform_staging.clear();
    prog.loc_to_member.clear();
    for (uint32_t u = 0; u < 8; u++) prog.sampler_units[u] = u;

    if (vs_it != m_shaders.end() && fs_it != m_shaders.end() &&
        !vs_it->second.preprocessed.empty() && !fs_it->second.preprocessed.empty()) {
        TranslatedProgramHLSL translated = translate_program_to_hlsl(
            vs_it->second.preprocessed, fs_it->second.preprocessed, prog.attrib_locations);
        prog.uniform_layout = translated.uniforms;
        prog.uniform_staging.assign(translated.cbuffer_size, 0);
        for (size_t i = 0; i < prog.uniform_layout.size(); i++) {
            const auto& m = prog.uniform_layout[i];
            auto lit = prog.uniform_locations.find(m.name);
            int32_t loc = -1;
            if (lit != prog.uniform_locations.end()) {
                loc = lit->second;
            } else {
                loc = prog.next_uniform_loc++;
                prog.uniform_locations[m.name] = loc;
            }
            prog.loc_to_member[loc] = i;
        }
        int32_t max_loc = 0;
        for (const auto& kv : prog.loc_to_member) {
            if (kv.first > max_loc) max_loc = kv.first;
        }
        prog.loc_to_member_vec.assign(static_cast<size_t>(max_loc) + 1, -1);
        prog.fog_params_loc = -1;
        uint32_t compact_end = 16u;
        for (const auto& m : prog.uniform_layout) {
            if (!m.is_sampler && m.array_size <= 8) {
                compact_end = std::max(compact_end, m.byte_offset + m.byte_size);
            }
        }
        prog.uniform_compact_bytes = std::min<uint32_t>(
            static_cast<uint32_t>(prog.uniform_staging.size()), compact_end);
        for (const auto& kv : prog.loc_to_member) {
            if (kv.first >= 0) {
                prog.loc_to_member_vec[kv.first] = static_cast<int32_t>(kv.second);
                if (prog.uniform_layout[kv.second].name == "vc_scn_vec4_fog_parameters") {
                    prog.fog_params_loc = kv.first;
                }
            }
        }
        prog.uniform_version++;
#if defined(LCS_ENABLE_D3D12)
        if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
            if (!d3d12::register_d3d12_program(program, translated)) {
                std::cerr << "[RenderBridge] D3D12 program translation/compile failed for program "
                          << program << "\n";
            }
        }
#endif
    }

    prog.linked = true;
    if (m_bound_program == program) {
        m_cached_prog_id = program;
        m_cached_prog = &prog;
    }
}

void RenderBridge::gl_use_program(uint32_t program) {
    m_bound_program = program;
    if (program == 0) {
        m_cached_prog_id = 0;
        m_cached_prog = nullptr;
    } else if (m_cached_prog_id != program) {
        auto it = m_programs.find(program);
        if (it != m_programs.end()) {
            m_cached_prog_id = program;
            m_cached_prog = &it->second;
        } else {
            m_cached_prog_id = 0;
            m_cached_prog = nullptr;
        }
    }
}

void RenderBridge::gl_get_programiv(uint32_t program, uint32_t pname, int32_t* params) {
    if (!params) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end()) {
        *params = 0;
        return;
    }
    const auto& prog = it->second;
    switch (pname) {
        case 0x8B82: // GL_LINK_STATUS
            *params = prog.linked ? 1 : 0;
            break;
        case 0x8B86: // GL_ACTIVE_UNIFORMS
            *params = static_cast<int32_t>(prog.active_uniforms.size());
            break;
        case 0x8B87: { // GL_ACTIVE_UNIFORM_MAX_LENGTH
            size_t max_len = 0;
            for (const auto& u : prog.active_uniforms) {
                max_len = std::max(max_len, u.name.size() + 1);
            }
            *params = static_cast<int32_t>(max_len);
            break;
        }
        case 0x8B89: // GL_ACTIVE_ATTRIBUTES
            *params = static_cast<int32_t>(prog.active_attribs.size());
            break;
        case 0x8B8A: { // GL_ACTIVE_ATTRIBUTE_MAX_LENGTH
            size_t max_len = 0;
            for (const auto& a : prog.active_attribs) {
                max_len = std::max(max_len, a.name.size() + 1);
            }
            *params = static_cast<int32_t>(max_len);
            break;
        }
        default:
            *params = 0;
            break;
    }
}

void RenderBridge::gl_get_active_attrib(uint32_t program, uint32_t index, int32_t buf_size,
                                        int32_t* length, int32_t* size, uint32_t* type, char* name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end() || index >= it->second.active_attribs.size()) {
        if (length) *length = 0;
        if (name && buf_size > 0) name[0] = '\0';
        return;
    }
    const auto& var = it->second.active_attribs[index];
    if (size) *size = var.array_size;
    if (type) *type = var.gl_type;
    if (name && buf_size > 0) {
        int32_t copy_len = std::min(static_cast<int32_t>(var.name.size()), buf_size - 1);
        if (copy_len > 0) std::memcpy(name, var.name.data(), static_cast<size_t>(copy_len));
        name[copy_len] = '\0';
        if (length) *length = copy_len;
    } else if (length) {
        *length = 0;
    }
}

void RenderBridge::gl_get_active_uniform(uint32_t program, uint32_t index, int32_t buf_size,
                                         int32_t* length, int32_t* size, uint32_t* type, char* name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end() || index >= it->second.active_uniforms.size()) {
        if (length) *length = 0;
        if (name && buf_size > 0) name[0] = '\0';
        return;
    }
    const auto& var = it->second.active_uniforms[index];
    if (size) *size = var.array_size;
    if (type) *type = var.gl_type;
    if (name && buf_size > 0) {
        std::string full_name = (var.array_size > 1) ? (var.name + "[0]") : var.name;
        int32_t copy_len = std::min(static_cast<int32_t>(full_name.size()), buf_size - 1);
        if (copy_len > 0) std::memcpy(name, full_name.data(), static_cast<size_t>(copy_len));
        name[copy_len] = '\0';
        if (length) *length = copy_len;
    } else if (length) {
        *length = 0;
    }
}

int32_t RenderBridge::gl_get_attrib_location(uint32_t program, const char* name) {
    if (!name) return -1;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end()) return -1;
    auto ait = it->second.attrib_locations.find(name);
    if (ait != it->second.attrib_locations.end()) return ait->second;
    return -1;
}

int32_t RenderBridge::gl_get_uniform_location(uint32_t program, const char* name) {
    if (!name) return -1;
    std::string key(name);
    size_t lbr = key.find('[');
    int32_t elem_offset = 0;
    if (lbr != std::string::npos) {
        elem_offset = static_cast<int32_t>(std::strtol(key.c_str() + lbr + 1, nullptr, 10));
        key = key.substr(0, lbr);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(program);
    if (it == m_programs.end()) return -1;
    auto uit = it->second.uniform_locations.find(key);
    int32_t ret = -1;
    if (uit != it->second.uniform_locations.end()) {
        ret = uit->second + (elem_offset > 0 ? elem_offset * 1000 : 0);
    }
    return ret;
}

void RenderBridge::gl_gen_buffers(int32_t n, uint32_t* buffers) {
    if (!buffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        uint32_t id = m_next_buffer_id++;
        buffers[i] = id;
        GLBufferObject buf;
        buf.id = id;
        auto [ins_it, inserted] = m_buffers.emplace(id, std::move(buf));
        if (id >= m_buffer_ptrs.size()) {
            m_buffer_ptrs.resize(static_cast<size_t>(id) + 64u, nullptr);
        }
        m_buffer_ptrs[id] = &ins_it->second;
    }
}

void RenderBridge::gl_delete_buffers(int32_t n, const uint32_t* buffers) {
    if (!buffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        const uint32_t id = buffers[i];
        if (id < m_buffer_ptrs.size()) {
            m_buffer_ptrs[id] = nullptr;
        }
        m_buffers.erase(id);
#if defined(LCS_ENABLE_D3D12)
        if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
            d3d12::notify_d3d12_buffer_deleted(id);
        }
#endif
    }
    ++m_buffer_delete_epoch;
    ++m_buffer_table_epoch;
}

void RenderBridge::gl_bind_buffer(uint32_t target, uint32_t buffer) {
    if (target == kGLArrayBuffer) {
        m_bound_array_buffer = buffer;
    } else if (target == kGLElementArrayBuffer) {
        if (m_bound_element_buffer != buffer) {
            m_bound_element_buffer = buffer;
            auto& vao = m_vaos[m_bound_vao];
            vao.element_buffer = buffer;
            vao.cached_valid = false;
        }
    }
}

void RenderBridge::gl_buffer_data(uint32_t target, ptrdiff_t size, const void* data, uint32_t usage) {
    if (size < 0) return;
    uint32_t id = (target == kGLElementArrayBuffer) ? m_bound_element_buffer : m_bound_array_buffer;
    if (id == 0) return;
    GLBufferObject* bp = lookup_buffer_fast(id);
    if (!bp) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& buf = m_buffers[id];
        if (id >= m_buffer_ptrs.size()) {
            m_buffer_ptrs.resize(static_cast<size_t>(id) + 64u, nullptr);
        }
        m_buffer_ptrs[id] = &buf;
        bp = &buf;
        ++m_buffer_table_epoch;
    }
    bp->id = id;
    bp->target = target;
    bp->usage = usage;
    bp->data.resize(static_cast<size_t>(size));
    if (data && size > 0) {
        lcs::cpu::DynarmicCore::read_guest_memory(
            reinterpret_cast<uint64_t>(data), bp->data.data(), static_cast<size_t>(size));
    }
    bp->version++;
    if (bp->mutations < 1000u) bp->mutations++;
}

void RenderBridge::gl_buffer_sub_data(uint32_t target, ptrdiff_t offset, ptrdiff_t size, const void* data) {
    if (offset < 0 || size <= 0 || !data) return;
    uint32_t id = (target == kGLElementArrayBuffer) ? m_bound_element_buffer : m_bound_array_buffer;
    if (id == 0) return;
    GLBufferObject* bp = lookup_buffer_fast(id);
    if (!bp) return;
    const size_t end = static_cast<size_t>(offset) + static_cast<size_t>(size);
    if (end <= bp->data.size()) {
        lcs::cpu::DynarmicCore::read_guest_memory(
            reinterpret_cast<uint64_t>(data), bp->data.data() + offset, static_cast<size_t>(size));
        bp->version++;
        if (bp->mutations < 1000u) bp->mutations++;
    }
}

void RenderBridge::gl_gen_textures(int32_t n, uint32_t* textures) {
    if (!textures || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        uint32_t id = m_next_texture_id++;
        textures[i] = id;
        GLTextureObject tex;
        tex.id = id;
        m_textures[id] = std::move(tex);
    }
}

void RenderBridge::gl_delete_textures(int32_t n, const uint32_t* textures) {
    if (!textures || n <= 0) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (int32_t i = 0; i < n; i++) {
            m_textures.erase(textures[i]);
        }
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        for (int32_t i = 0; i < n; i++) {
            d3d12::delete_d3d12_texture(textures[i]);
        }
    }
#endif
}

void RenderBridge::gl_active_texture(uint32_t texture) {
    if (texture >= kGLTexture0 && texture < kGLTexture0 + 16) {
        m_active_tex_unit = texture - kGLTexture0;
    }
}

void RenderBridge::gl_bind_texture(uint32_t target, uint32_t texture) {
    if (target == 0x0DE1) { // GL_TEXTURE_2D
        m_bound_texture_2d = texture;
    }
    m_bound_textures[m_active_tex_unit] = texture;
}

void RenderBridge::gl_tex_image_2d(uint32_t target, int32_t level, int32_t internal_format,
                                   int32_t width, int32_t height, int32_t border,
                                   uint32_t format, uint32_t type, const void* pixels) {
    (void)border;
    if (level != 0 || width <= 0 || height <= 0) {
        return;
    }
    uint32_t id = m_bound_textures[m_active_tex_unit];
    if (id == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& tex = m_textures[id];
        tex.id = id;
        tex.target = target;
        tex.width = static_cast<uint32_t>(width);
        tex.height = static_cast<uint32_t>(height);
        tex.internal_format = static_cast<uint32_t>(internal_format);
        tex.format = format;
        tex.type = type;
        if (pixels && m_backend != RenderBackend::Direct3D12) {
            size_t bpp = estimate_pixel_bytes(format, type);
            size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * bpp;
            tex.pixels.resize(total);
            std::memcpy(tex.pixels.data(), pixels, total);
        } else if (m_backend == RenderBackend::Direct3D12 && !tex.pixels.empty()) {
            std::vector<uint8_t>().swap(tex.pixels);
        }
    }
    const bool tex_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    const auto t_start = tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    uint64_t d3d_ns = 0;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        uint32_t eff_fmt = (format != 0) ? format : static_cast<uint32_t>(internal_format);
        const auto td0 = tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        d3d12::upload_d3d12_texture(id, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                    eff_fmt, type, pixels);
        if (tex_audit) {
            d3d_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - td0).count());
        }
    }
#endif
    static auto s_last_pump = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - s_last_pump >= std::chrono::milliseconds(40)) {
        s_last_pump = now;
        pump_loading_events();
    }
    if (tex_audit) {
        g_tmp_gltex_calls.fetch_add(1, std::memory_order_relaxed);
        g_tmp_gltex_d3d_ns.fetch_add(d3d_ns, std::memory_order_relaxed);
        g_tmp_gltex_tot_ns.fetch_add(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_start).count()),
            std::memory_order_relaxed);
    }
}

bool RenderBridge::has_texture(uint32_t tex_id) const {
    if (tex_id == 0) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_textures.find(tex_id) != m_textures.end();
}

void RenderBridge::replace_texture_rgba8(uint32_t tex_id, uint32_t width, uint32_t height, const void* rgba_pixels) {
    if (tex_id == 0 || width == 0 || height == 0 || !rgba_pixels) return;
    constexpr uint32_t kGlRgba = 0x1908u;
    constexpr uint32_t kGlUnsignedByte = 0x1401u;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_textures.find(tex_id);
        if (it == m_textures.end()) return;
        auto& tex = it->second;
        tex.width = width;
        tex.height = height;
        tex.internal_format = kGlRgba;
        tex.format = kGlRgba;
        tex.type = kGlUnsignedByte;
        if (m_backend != RenderBackend::Direct3D12) {
            const size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
            tex.pixels.resize(total);
            std::memcpy(tex.pixels.data(), rgba_pixels, total);
        }
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::upload_d3d12_texture(tex_id, width, height, kGlRgba, kGlUnsignedByte, rgba_pixels);
    }
#endif
}

void RenderBridge::gl_tex_sub_image_2d(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset,
                                       int32_t width, int32_t height, uint32_t format, uint32_t type,
                                       const void* pixels) {
    (void)target;
    if (level != 0 || width <= 0 || height <= 0 || !pixels) return;
    uint32_t id = m_bound_textures[m_active_tex_unit];
    if (id == 0) return;

    if (m_backend != RenderBackend::Direct3D12) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_textures.find(id);
        if (it != m_textures.end()) {
            auto& tex = it->second;
            size_t bpp = estimate_pixel_bytes(format, type);
            size_t row_bytes = static_cast<size_t>(width) * bpp;
            size_t tex_row_bytes = static_cast<size_t>(tex.width) * bpp;
            if (xoffset >= 0 && yoffset >= 0 &&
                static_cast<uint32_t>(xoffset + width) <= tex.width &&
                static_cast<uint32_t>(yoffset + height) <= tex.height) {
                if (tex.pixels.size() < static_cast<size_t>(tex.height) * tex_row_bytes) {
                    tex.pixels.resize(static_cast<size_t>(tex.height) * tex_row_bytes, 0);
                }
                const uint8_t* src = static_cast<const uint8_t*>(pixels);
                for (int32_t row = 0; row < height; row++) {
                    size_t dst_off = static_cast<size_t>(yoffset + row) * tex_row_bytes +
                                     static_cast<size_t>(xoffset) * bpp;
                    std::memcpy(tex.pixels.data() + dst_off,
                                src + static_cast<size_t>(row) * row_bytes, row_bytes);
                }
            }
        }
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::update_d3d12_texture_sub(id, xoffset, yoffset,
                                        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                        format, type, pixels);
    }
#endif
}

void RenderBridge::gl_compressed_tex_image_2d(uint32_t target, int32_t level, uint32_t internal_format,
                                              int32_t width, int32_t height, int32_t border,
                                              int32_t image_size, const void* data) {
    if (level != 0 || width <= 0 || height <= 0 || !data) return;

#if defined(LCS_ENABLE_D3D12)
    // Asynchronous path: copy the payload, decode on the texture pipeline's workers.
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized() &&
        TexturePipeline::instance().enabled()) {
        const uint32_t id = m_bound_textures[m_active_tex_unit];
        if (id == 0) return;
        auto job = TexturePipeline::instance().submit(internal_format, static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height), image_size, data);
        if (job) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto& tex = m_textures[id];
                tex.id = id;
                tex.target = target;
                tex.width = static_cast<uint32_t>(width);
                tex.height = static_cast<uint32_t>(height);
                tex.internal_format = kGLRGBA;
                tex.format = kGLRGBA;
                tex.type = 0x1401u;
                if (!tex.pixels.empty()) std::vector<uint8_t>().swap(tex.pixels);
            }
            d3d12::upload_d3d12_texture_async(id, std::move(job));
            static auto s_last_async_pump = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - s_last_async_pump >= std::chrono::milliseconds(40)) {
                s_last_async_pump = now;
                pump_loading_events();
            }
            return;
        }
    }
#endif

    static const bool s_tex_audit = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    const auto t0 = s_tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<uint8_t> rgba;
    if (!decode_compressed_to_rgba8(internal_format, static_cast<uint32_t>(width),
                                      static_cast<uint32_t>(height), image_size, data, rgba)) {
        static uint32_t s_last_unsupported = 0;
        if (s_last_unsupported != internal_format) {
            s_last_unsupported = internal_format;
            std::cerr << "[RenderBridge] Unsupported compressed texture fmt=0x"
                      << std::hex << internal_format << std::dec
                      << " (" << width << "x" << height << ", size=" << image_size << ")\n";
        }
        return;
    }
    const auto t1 = s_tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    static bool s_logged_first = false;
    if (!s_logged_first) {
        s_logged_first = true;
        std::cout << "[RenderBridge] Decoded compressed texture fmt=0x" << std::hex << internal_format
                  << std::dec << " (" << width << "x" << height << ") to RGBA8\n";
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        const uint32_t id = m_bound_textures[m_active_tex_unit];
        if (id == 0) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto& tex = m_textures[id];
            tex.id = id;
            tex.target = target;
            tex.width = static_cast<uint32_t>(width);
            tex.height = static_cast<uint32_t>(height);
            tex.internal_format = kGLRGBA;
            tex.format = kGLRGBA;
            tex.type = 0x1401u;
            if (!tex.pixels.empty()) std::vector<uint8_t>().swap(tex.pixels);
        }
        const auto td0 = s_tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        d3d12::upload_d3d12_texture_rgba8(id, static_cast<uint32_t>(width), static_cast<uint32_t>(height), std::move(rgba));
        const auto td1 = s_tex_audit ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        uint64_t pump_ns = 0;
        static auto s_last_pump = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last_pump >= std::chrono::milliseconds(40)) {
            s_last_pump = now;
            pump_loading_events();
            if (s_tex_audit) {
                pump_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - now).count());
            }
        }
        if (s_tex_audit) {
            const auto t_end = std::chrono::steady_clock::now();
            g_tmp_pvr_calls.fetch_add(1, std::memory_order_relaxed);
            g_tmp_pvr_px.fetch_add(static_cast<uint64_t>(width) * static_cast<uint64_t>(height), std::memory_order_relaxed);
            g_tmp_pvr_dec_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()), std::memory_order_relaxed);
            g_tmp_pvr_d3d_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(td1 - td0).count()), std::memory_order_relaxed);
            g_tmp_pvr_pump_ns.fetch_add(pump_ns, std::memory_order_relaxed);
            g_tmp_pvr_tot_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t0).count()), std::memory_order_relaxed);
        }
        return;
    }
#endif
    gl_tex_image_2d(target, level, kGLRGBA, width, height, border, kGLRGBA, 0x1401u, rgba.data());
}

void RenderBridge::gl_tex_parameteri(uint32_t target, uint32_t pname, int32_t param) {
    if (target != 0x0DE1) return; // GL_TEXTURE_2D
    uint32_t id = m_bound_textures[m_active_tex_unit];
    if (id == 0) return;
    auto& tex = m_textures[id];
    if (pname == 0x2802) { // GL_TEXTURE_WRAP_S
        tex.wrap_s = static_cast<uint32_t>(param);
    } else if (pname == 0x2803) { // GL_TEXTURE_WRAP_T
        tex.wrap_t = static_cast<uint32_t>(param);
    } else if (pname == 0x2800) { // GL_TEXTURE_MAG_FILTER
        tex.mag_filter = static_cast<uint32_t>(param);
    } else if (pname == 0x2801) { // GL_TEXTURE_MIN_FILTER
        tex.min_filter = static_cast<uint32_t>(param);
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::set_d3d12_texture_param(id, pname, param);
    }
#endif
}

void RenderBridge::gl_tex_parameterf(uint32_t target, uint32_t pname, float param) {
    gl_tex_parameteri(target, pname, static_cast<int32_t>(param));
}

void RenderBridge::gl_gen_framebuffers(int32_t n, uint32_t* framebuffers) {
    if (!framebuffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        uint32_t id = m_next_fbo_id++;
        framebuffers[i] = id;
        GLFramebufferObject fbo;
        fbo.id = id;
        m_framebuffers[id] = fbo;
    }
}

void RenderBridge::gl_delete_framebuffers(int32_t n, const uint32_t* framebuffers) {
    if (!framebuffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        if (m_bound_fbo == framebuffers[i]) {
            m_bound_fbo = 0;
        }
        m_framebuffers.erase(framebuffers[i]);
    }
}

void RenderBridge::gl_bind_framebuffer(uint32_t target, uint32_t fbo) {
    (void)target;
    m_bound_fbo = fbo;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        if (fbo == 0) {
            d3d12::bind_d3d12_framebuffer(0, 0, 0, m_width, m_height);
            gl_viewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
            gl_scissor(m_scissor[0], m_scissor[1], m_scissor[2], m_scissor[3]);
        } else {
            auto& fbo_obj = m_framebuffers[fbo];
            fbo_obj.id = fbo;
            d3d12::bind_d3d12_framebuffer(
                fbo, fbo_obj.color_tex_id, fbo_obj.depth_tex_id, fbo_obj.width, fbo_obj.height);
        }
    }
#endif
}

void RenderBridge::gl_framebuffer_texture_2d(uint32_t target, uint32_t attachment,
                                             uint32_t textarget, uint32_t texture, int32_t level) {
    (void)target;
    (void)textarget;
    (void)level;
    if (m_bound_fbo == 0) return;
    uint32_t tex_w = 1024;
    uint32_t tex_h = 1024;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto tit = m_textures.find(texture);
        if (tit != m_textures.end() && tit->second.width > 0 && tit->second.height > 0) {
            tex_w = tit->second.width;
            tex_h = tit->second.height;
        }
        auto& fbo_obj = m_framebuffers[m_bound_fbo];
        fbo_obj.id = m_bound_fbo;
        if (texture != 0) {
            fbo_obj.width = tex_w;
            fbo_obj.height = tex_h;
        }
        if (attachment == 0x8D00 || attachment == 0x821A) { // GL_DEPTH_ATTACHMENT / GL_DEPTH_STENCIL_ATTACHMENT
            fbo_obj.depth_tex_id = texture;
        } else if (attachment == 0x8CE0) { // GL_COLOR_ATTACHMENT0
            fbo_obj.color_tex_id = texture;
        }
#if defined(LCS_ENABLE_D3D12)
        if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
            d3d12::bind_d3d12_framebuffer(
                m_bound_fbo, fbo_obj.color_tex_id, fbo_obj.depth_tex_id, fbo_obj.width, fbo_obj.height);
        }
#endif
    }
}

void RenderBridge::gl_enable(uint32_t cap) {
    switch (cap) {
        case 0x0BE2: if (!m_blend_enabled) { m_blend_enabled = true; ++m_render_state_seq; } break;   // GL_BLEND
        case 0x0B71: if (!m_depth_test) { m_depth_test = true; ++m_render_state_seq; } break;         // GL_DEPTH_TEST
        case 0x0B90: if (!m_stencil_test) { m_stencil_test = true; ++m_render_state_seq; } break;     // GL_STENCIL_TEST
        case 0x0B44: if (!m_cull_enabled) { m_cull_enabled = true; ++m_render_state_seq; } break;     // GL_CULL_FACE
        case 0x0C11: if (!m_scissor_enabled) { m_scissor_enabled = true; ++m_render_state_seq; } break; // GL_SCISSOR_TEST
        default: break;
    }
}

void RenderBridge::gl_disable(uint32_t cap) {
    switch (cap) {
        case 0x0BE2: if (m_blend_enabled) { m_blend_enabled = false; ++m_render_state_seq; } break;
        case 0x0B71: if (m_depth_test) { m_depth_test = false; ++m_render_state_seq; } break;
        case 0x0B90: if (m_stencil_test) { m_stencil_test = false; ++m_render_state_seq; } break;
        case 0x0B44: if (m_cull_enabled) { m_cull_enabled = false; ++m_render_state_seq; } break;
        case 0x0C11: if (m_scissor_enabled) { m_scissor_enabled = false; ++m_render_state_seq; } break;
        default: break;
    }
}

void RenderBridge::gl_blend_func(uint32_t sfactor, uint32_t dfactor) {
    if (m_blend_src_rgb != sfactor || m_blend_dst_rgb != dfactor ||
        m_blend_src_alpha != sfactor || m_blend_dst_alpha != dfactor) {
        m_blend_src_rgb = sfactor;
        m_blend_dst_rgb = dfactor;
        m_blend_src_alpha = sfactor;
        m_blend_dst_alpha = dfactor;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_blend_func_separate(uint32_t src_rgb, uint32_t dst_rgb,
                                          uint32_t src_alpha, uint32_t dst_alpha) {
    if (m_blend_src_rgb != src_rgb || m_blend_dst_rgb != dst_rgb ||
        m_blend_src_alpha != src_alpha || m_blend_dst_alpha != dst_alpha) {
        m_blend_src_rgb = src_rgb;
        m_blend_dst_rgb = dst_rgb;
        m_blend_src_alpha = src_alpha;
        m_blend_dst_alpha = dst_alpha;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_blend_equation(uint32_t mode) {
    if (m_blend_eq_rgb != mode || m_blend_eq_alpha != mode) {
        m_blend_eq_rgb = mode;
        m_blend_eq_alpha = mode;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_blend_equation_separate(uint32_t mode_rgb, uint32_t mode_alpha) {
    if (m_blend_eq_rgb != mode_rgb || m_blend_eq_alpha != mode_alpha) {
        m_blend_eq_rgb = mode_rgb;
        m_blend_eq_alpha = mode_alpha;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_depth_func(uint32_t func) {
    if (m_depth_func != func) {
        m_depth_func = func;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_depth_mask(uint8_t flag) {
    const bool w = (flag != 0);
    if (m_depth_write != w) {
        m_depth_write = w;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_stencil_func(uint32_t func, int32_t ref, uint32_t mask) {
    const uint8_t r = static_cast<uint8_t>(std::clamp(ref, 0, 255));
    const uint8_t rm = static_cast<uint8_t>(mask & 0xFFu);
    if (m_stencil_func != func || m_stencil_ref != r || m_stencil_read_mask != rm) {
        m_stencil_func = func;
        m_stencil_ref = r;
        m_stencil_read_mask = rm;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_stencil_op(uint32_t sfail, uint32_t zfail, uint32_t zpass) {
    if (m_stencil_sfail != sfail || m_stencil_zfail != zfail || m_stencil_zpass != zpass) {
        m_stencil_sfail = sfail;
        m_stencil_zfail = zfail;
        m_stencil_zpass = zpass;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_stencil_mask(uint32_t mask) {
    const uint8_t wm = static_cast<uint8_t>(mask & 0xFFu);
    if (m_stencil_write_mask != wm) {
        m_stencil_write_mask = wm;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_cull_face(uint32_t mode) {
    if (m_cull_mode != mode) {
        m_cull_mode = mode;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_front_face(uint32_t mode) {
    if (m_front_face != mode) {
        m_front_face = mode;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_color_mask(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const uint8_t cm = static_cast<uint8_t>((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
    if (m_color_mask != cm) {
        m_color_mask = cm;
        ++m_render_state_seq;
    }
}

void RenderBridge::gl_vertex_attrib4f(uint32_t index, float x, float y, float z, float w) {
    if (index >= 16) return;
    if (m_default_attribs[index][0] != x || m_default_attribs[index][1] != y ||
        m_default_attribs[index][2] != z || m_default_attribs[index][3] != w) {
        m_default_attribs[index][0] = x;
        m_default_attribs[index][1] = y;
        m_default_attribs[index][2] = z;
        m_default_attribs[index][3] = w;
        ++m_default_attr_seq;
    }
}

void RenderBridge::gl_uniform_iv(int32_t location, int32_t count, int32_t components, const int32_t* v) {
    if (location < 0 || count <= 0 || !v || m_bound_program == 0) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    if (m_cached_prog_id != m_bound_program || !m_cached_prog) {
        auto it = m_programs.find(m_bound_program);
        if (it == m_programs.end()) return;
        m_cached_prog_id = m_bound_program;
        m_cached_prog = &it->second;
    }
    auto& prog = *m_cached_prog;
    if (base_loc < 0 || base_loc >= static_cast<int32_t>(prog.loc_to_member_vec.size())) return;
    int32_t midx = prog.loc_to_member_vec[base_loc];
    if (midx < 0) return;
    const auto& member = prog.uniform_layout[static_cast<size_t>(midx)];

    if (member.is_sampler) {
        if (components == 1) {
            for (int32_t i = 0; i < count; i++) {
                int32_t slot = member.sampler_slot + elem_offset + i;
                if (slot >= 0 && slot < 8) {
                    prog.sampler_units[slot] = static_cast<uint32_t>(std::max(0, v[i]));
                }
            }
        }
        return;
    }

    int32_t clamped_comp = std::clamp(components, 1, 4);
    for (int32_t i = 0; i < count; i++) {
        size_t off = member.byte_offset + static_cast<size_t>(elem_offset + i) * 16u;
        size_t bytes = static_cast<size_t>(clamped_comp) * sizeof(int32_t);
        if (off + bytes <= member.byte_offset + member.byte_size &&
            off + bytes <= prog.uniform_staging.size()) {
            std::memcpy(prog.uniform_staging.data() + off,
                        v + static_cast<size_t>(i) * static_cast<size_t>(components), bytes);
        }
    }
    prog.uniform_version++;
}

void RenderBridge::gl_uniform_fv(int32_t location, int32_t count, int32_t components, const float* v) {
    if (location < 0 || count <= 0 || !v || m_bound_program == 0) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    if (m_cached_prog_id != m_bound_program || !m_cached_prog) {
        auto it = m_programs.find(m_bound_program);
        if (it == m_programs.end()) return;
        m_cached_prog_id = m_bound_program;
        m_cached_prog = &it->second;
    }
    auto& prog = *m_cached_prog;
    if (base_loc < 0 || base_loc >= static_cast<int32_t>(prog.loc_to_member_vec.size())) return;
    int32_t midx = prog.loc_to_member_vec[base_loc];
    if (midx < 0) return;
    const auto& member = prog.uniform_layout[static_cast<size_t>(midx)];
    if (member.is_sampler) return;

    int32_t clamped_comp = std::clamp(components, 1, 4);
    if (base_loc == prog.fog_params_loc && clamped_comp >= 2 && count == 1) {
        // Apply VCS-style atmospheric fog (fog.distance & fog.start) using unscaled TimeCycle farClip,
        // while preserving indoor FogStart >= 0 clamping for negative ExtraColour FogStart slots.
        float fixed_v[4] = {
            v[0],
            v[1],
            (clamped_comp > 2) ? v[2] : 0.0f,
            (clamped_comp > 3) ? v[3] : 0.0f
        };
        lcs::game::LcsDistance::instance().adjust_fog_parameters(fixed_v);
        size_t off = member.byte_offset + static_cast<size_t>(elem_offset) * 16u;
        size_t bytes = static_cast<size_t>(clamped_comp) * sizeof(float);
        if (off + bytes <= member.byte_offset + member.byte_size &&
            off + bytes <= prog.uniform_staging.size()) {
            std::memcpy(prog.uniform_staging.data() + off, fixed_v, bytes);
            prog.uniform_version++;
        }
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        size_t off = member.byte_offset + static_cast<size_t>(elem_offset + i) * 16u;
        size_t bytes = static_cast<size_t>(clamped_comp) * sizeof(float);
        if (off + bytes <= member.byte_offset + member.byte_size &&
            off + bytes <= prog.uniform_staging.size()) {
            std::memcpy(prog.uniform_staging.data() + off,
                        v + static_cast<size_t>(i) * static_cast<size_t>(components), bytes);
        }
    }
    prog.uniform_version++;
}

void RenderBridge::gl_uniform_matrix_fv(int32_t location, int32_t count, int32_t dim,
                                        uint8_t transpose, const float* value) {
    if (location < 0 || count <= 0 || !value || m_bound_program == 0) return;
    if (dim != 2 && dim != 3 && dim != 4) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    if (m_cached_prog_id != m_bound_program || !m_cached_prog) {
        auto it = m_programs.find(m_bound_program);
        if (it == m_programs.end()) return;
        m_cached_prog_id = m_bound_program;
        m_cached_prog = &it->second;
    }
    auto& prog = *m_cached_prog;
    if (base_loc < 0 || base_loc >= static_cast<int32_t>(prog.loc_to_member_vec.size())) return;
    int32_t midx = prog.loc_to_member_vec[base_loc];
    if (midx < 0) return;
    const auto& member = prog.uniform_layout[static_cast<size_t>(midx)];
    if (member.is_sampler) return;

    size_t mat_stride_bytes = static_cast<size_t>(dim) * 16u;
    for (int32_t i = 0; i < count; i++) {
        size_t mat_off = member.byte_offset + static_cast<size_t>(elem_offset + i) * mat_stride_bytes;
        if (mat_off + mat_stride_bytes > member.byte_offset + member.byte_size ||
            mat_off + mat_stride_bytes > prog.uniform_staging.size()) {
            break;
        }
        const float* src = value + static_cast<size_t>(i) * static_cast<size_t>(dim * dim);
        for (int32_t r = 0; r < dim; r++) {
            float* dst_row = reinterpret_cast<float*>(
                prog.uniform_staging.data() + mat_off + static_cast<size_t>(r) * 16u);
            if (transpose == 0) {
                std::memcpy(dst_row, src + static_cast<size_t>(r * dim),
                            static_cast<size_t>(dim) * sizeof(float));
            } else {
                for (int32_t c = 0; c < dim; c++) {
                    dst_row[c] = src[c * dim + r];
                }
            }
        }
    }
    prog.uniform_version++;
}

void RenderBridge::gl_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                            uint8_t normalized, int32_t stride, uint64_t pointer) {
    if (index >= 16) return;
    auto& vao = m_vaos[m_bound_vao];
    auto& a = vao.attribs[index];
    const bool norm = (normalized != 0);
    if (a.size == size && a.type == type && a.normalized == norm &&
        a.stride == stride && a.pointer == pointer && a.vbo_id == m_bound_array_buffer) {
        return;
    }
    a.size = size;
    a.type = type;
    a.normalized = norm;
    a.stride = stride;
    a.pointer = pointer;
    a.vbo_id = m_bound_array_buffer;
    vao.cached_valid = false;
    vao.gen = m_next_vao_gen++;
}

void RenderBridge::gl_enable_vertex_attrib_array(uint32_t index) {
    if (index < 16) {
        auto& vao = m_vaos[m_bound_vao];
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        if (vao.attribs[index].enabled && (vao.enabled_mask & bit)) return;
        vao.attribs[index].enabled = true;
        vao.enabled_mask |= bit;
        vao.cached_valid = false;
        vao.gen = m_next_vao_gen++;
    }
}

void RenderBridge::gl_disable_vertex_attrib_array(uint32_t index) {
    if (index < 16) {
        auto& vao = m_vaos[m_bound_vao];
        const uint16_t bit = static_cast<uint16_t>(1u << index);
        if (!vao.attribs[index].enabled && !(vao.enabled_mask & bit)) return;
        vao.attribs[index].enabled = false;
        vao.enabled_mask &= static_cast<uint16_t>(~bit);
        vao.cached_valid = false;
        vao.gen = m_next_vao_gen++;
    }
}

void RenderBridge::gl_gen_vertex_arrays(int32_t n, uint32_t* arrays) {
    if (!arrays || n <= 0) return;
    if (m_vaos.empty()) {
        m_vaos.resize(1);
        m_vaos[0].valid = true;
    }
    for (int32_t i = 0; i < n; ++i) {
        uint32_t id = 0;
        if (!m_free_vaos.empty()) {
            id = m_free_vaos.back();
            m_free_vaos.pop_back();
            m_vaos[id] = GLVaoObject{};
        } else {
            id = static_cast<uint32_t>(m_vaos.size());
            m_vaos.emplace_back();
        }
        m_vaos[id].valid = true;
        m_vaos[id].gen = m_next_vao_gen++;
        arrays[i] = id;
    }
}

void RenderBridge::gl_delete_vertex_arrays(int32_t n, const uint32_t* arrays) {
    if (!arrays || n <= 0) return;
    for (int32_t i = 0; i < n; ++i) {
        const uint32_t id = arrays[i];
        if (id > 0 && id < m_vaos.size() && m_vaos[id].valid) {
            if (m_bound_vao == id) {
                gl_bind_vertex_array(0);
            }
            m_vaos[id] = GLVaoObject{};
            m_free_vaos.push_back(id);
        }
    }
}

void RenderBridge::gl_bind_vertex_array(uint32_t array) {
    const uint32_t target = (array < m_vaos.size() && m_vaos[array].valid) ? array : 0;
    if (m_bound_vao == target) return;
    m_bound_vao = target;
    m_active_attribs = &m_vaos[target].attribs;
    m_bound_element_buffer = m_vaos[target].element_buffer;
}

void RenderBridge::dispatch_d3d12_draw(uint32_t mode, int32_t first_vertex, int32_t count,
                                        bool indexed, uint32_t index_type, const void* indices,
                                        uint32_t uniform_copy_bytes) {
#if defined(LCS_ENABLE_D3D12)
    if (count <= 0 || m_bound_program == 0 ||
        m_backend != RenderBackend::Direct3D12 || !d3d12::is_d3d12_initialized()) {
        return;
    }

    if (m_cached_prog_id != m_bound_program || !m_cached_prog) {
        auto pit = m_programs.find(m_bound_program);
        if (pit == m_programs.end()) return;
        m_cached_prog_id = m_bound_program;
        m_cached_prog = &pit->second;
    }
    const auto& prog = *m_cached_prog;

    static const RenderBridge* s_state_owner = nullptr;
    static uint64_t s_cached_state_seq = 0;
    static d3d12::D3D12RenderState s_cached_state{};
    if (s_state_owner != this || s_cached_state_seq != m_render_state_seq) {
        s_cached_state.blend_enabled = m_blend_enabled;
        s_cached_state.blend_src_rgb = m_blend_src_rgb;
        s_cached_state.blend_dst_rgb = m_blend_dst_rgb;
        s_cached_state.blend_src_alpha = m_blend_src_alpha;
        s_cached_state.blend_dst_alpha = m_blend_dst_alpha;
        s_cached_state.blend_eq_rgb = m_blend_eq_rgb;
        s_cached_state.blend_eq_alpha = m_blend_eq_alpha;
        s_cached_state.depth_test = m_depth_test;
        s_cached_state.depth_write = m_depth_write;
        s_cached_state.depth_func = m_depth_func;
        s_cached_state.stencil_enable = m_stencil_test;
        s_cached_state.stencil_func = m_stencil_func;
        s_cached_state.stencil_ref = m_stencil_ref;
        s_cached_state.stencil_read_mask = m_stencil_read_mask;
        s_cached_state.stencil_write_mask = m_stencil_write_mask;
        s_cached_state.stencil_sfail = m_stencil_sfail;
        s_cached_state.stencil_zfail = m_stencil_zfail;
        s_cached_state.stencil_zpass = m_stencil_zpass;
        s_cached_state.cull_enabled = m_cull_enabled;
        s_cached_state.cull_mode = m_cull_mode;
        s_cached_state.front_face = m_front_face;
        s_cached_state.scissor_enabled = m_scissor_enabled;
        s_cached_state.color_mask = m_color_mask;
        s_state_owner = this;
        s_cached_state_seq = m_render_state_seq;
    }
    const d3d12::D3D12RenderState& state = s_cached_state;

    GLVaoObject& vao = m_vaos[m_bound_vao];
    const auto& attribs = vao.attribs;
    const uint32_t active_mask = static_cast<uint32_t>(vao.enabled_mask);
    auto* fast_attrs = vao.cached_fast_attrs.data();

    if (vao.cached_valid &&
        vao.cached_buf_table_epoch == m_buffer_table_epoch &&
        vao.cached_default_attr_seq == m_default_attr_seq) {
        for (uint8_t k = 0; k < vao.cached_active_count; ++k) {
            const uint8_t idx = vao.cached_active_locs[k];
            const GLBufferObject* bp = vao.cached_attr_bps[idx];
            if (bp && fast_attrs[idx].vbo_version != bp->version) {
                if (attribs[idx].pointer < bp->data.size()) {
                    fast_attrs[idx].vbo_version = bp->version;
                    fast_attrs[idx].vbo_mutations = bp->mutations;
                    fast_attrs[idx].vbo_data = bp->data.data();
                    fast_attrs[idx].vbo_size = bp->data.size();
                } else {
                    vao.cached_valid = false;
                    vao.cached_all_attribs_vbo = false;
                    break;
                }
            }
        }
    } else {
        vao.cached_valid = false;
    }

    if (!vao.cached_valid) {
        bool all_vbo_ok = true;
        uint32_t last_vbo_id = 0;
        GLBufferObject* last_bp = nullptr;
        vao.cached_active_count = 0;
        for (uint32_t i = 0; i < 16; i++) {
            fast_attrs[i].enabled = 0;
            fast_attrs[i].vbo_id = 0;
            fast_attrs[i].vbo_data = nullptr;
            vao.cached_attr_bps[i] = nullptr;
            if (i < 8 && (active_mask & (1u << i)) == 0) {
                fast_attrs[i].size = 4;
                fast_attrs[i].type = 0x1406;
                fast_attrs[i].stride = 0;
                fast_attrs[i].normalized = 0;
                std::memcpy(fast_attrs[i].default_val, m_default_attribs[i], sizeof(fast_attrs[i].default_val));
            }
        }
        for (uint32_t m_active = active_mask, i = 0; m_active; m_active >>= 1, ++i) {
            if ((m_active & 1u) == 0) continue;
            vao.cached_active_locs[vao.cached_active_count++] = static_cast<uint8_t>(i);
            auto& fa = fast_attrs[i];
            const auto& src_a = attribs[i];
            fa.size = static_cast<uint32_t>(src_a.size);
            fa.type = src_a.type;
            fa.normalized = src_a.normalized ? 1u : 0u;
            fa.stride = static_cast<uint32_t>(std::max(0, src_a.stride));
            fa.enabled = 1u;
            std::memcpy(fa.default_val, m_default_attribs[i], sizeof(fa.default_val));
            if (src_a.vbo_id != 0) {
                GLBufferObject* bp = (src_a.vbo_id == last_vbo_id) ? last_bp : lookup_buffer_fast(src_a.vbo_id);
                last_vbo_id = src_a.vbo_id;
                last_bp = bp;
                vao.cached_attr_bps[i] = bp;
                if (bp && src_a.pointer < bp->data.size()) {
                    fa.vbo_id = src_a.vbo_id;
                    fa.vbo_version = bp->version;
                    fa.vbo_mutations = bp->mutations;
                    fa.vbo_data = bp->data.data();
                    fa.vbo_size = bp->data.size();
                    fa.pointer_offset = src_a.pointer;
                } else {
                    fa.vbo_id = 0;
                    all_vbo_ok = false;
                }
            } else {
                fa.vbo_id = 0;
                all_vbo_ok = false;
            }
        }
        vao.cached_ebo_bp = (vao.element_buffer != 0) ? lookup_buffer_fast(vao.element_buffer) : nullptr;
        vao.cached_all_attribs_vbo = all_vbo_ok;
        vao.cached_buf_table_epoch = m_buffer_table_epoch;
        vao.cached_default_attr_seq = m_default_attr_seq;
        vao.cached_valid = all_vbo_ok && (vao.element_buffer == 0 || vao.cached_ebo_bp != nullptr);
    }

    const bool all_attribs_vbo = (mode != 6) && vao.cached_all_attribs_vbo;
    GLBufferObject* ebo_bp = (indexed && m_bound_element_buffer != 0) ? vao.cached_ebo_bp : nullptr;
    const uint64_t ebo_offset = reinterpret_cast<uint64_t>(indices);
    if (indexed && m_bound_element_buffer != 0 && (!ebo_bp || ebo_offset >= ebo_bp->data.size())) {
        return;
    }
    const bool fast_ebo_ok = !indexed || (ebo_bp != nullptr);

    uint32_t bound_tex[8]{};
    for (uint32_t s = 0; s < 8; s++) {
        uint32_t unit = prog.sampler_units[s];
        bound_tex[s] = (unit < 16) ? m_bound_textures[unit] : 0;
    }

    const uint8_t* u_ptr = prog.uniform_staging.empty() ? nullptr : prog.uniform_staging.data();
    const uint32_t full_u_size = static_cast<uint32_t>(prog.uniform_staging.size());
    const uint32_t u_size = (uniform_copy_bytes > 0u && uniform_copy_bytes < full_u_size)
        ? uniform_copy_bytes : full_u_size;

    if (all_attribs_vbo && fast_ebo_ok) {
        d3d12::D3D12FastEboRef ebo_ref{};
        if (indexed) {
            ebo_ref.ebo_id = m_bound_element_buffer;
            ebo_ref.ebo_version = ebo_bp->version;
            ebo_ref.ebo_mutations = ebo_bp->mutations;
            ebo_ref.ebo_data = ebo_bp->data.data();
            ebo_ref.ebo_size = ebo_bp->data.size();
            ebo_ref.index_byte_offset = ebo_offset;
        }
        const d3d12::D3D12FastVaoLayout vao_hint{
            m_bound_vao,
            vao.gen,
            m_default_attr_seq,
            m_render_state_seq
        };
        d3d12::draw_d3d12_fast(
            m_bound_program, state, mode,
            static_cast<uint32_t>(std::max(0, first_vertex)),
            static_cast<uint32_t>(count),
            indexed, index_type, ebo_ref,
            u_ptr, full_u_size, prog.uniform_version,
            bound_tex, active_mask, fast_attrs, &vao_hint, u_size);
    } else {
        d3d12::D3D12VertexAttrDraw attrs[16]{};
        for (uint32_t i = 0; i < 16; i++) {
            attrs[i].size = attribs[i].size;
            attrs[i].type = attribs[i].type;
            attrs[i].normalized = attribs[i].normalized;
            attrs[i].stride = attribs[i].stride;
            for (int c = 0; c < 4; c++) attrs[i].default_val[c] = m_default_attribs[i][c];
            if (!attribs[i].enabled) continue;
            if (fast_attrs[i].vbo_data != nullptr) {
                attrs[i].base_ptr = fast_attrs[i].vbo_data + fast_attrs[i].pointer_offset;
                attrs[i].available_bytes = fast_attrs[i].vbo_size - static_cast<size_t>(fast_attrs[i].pointer_offset);
            } else if (attribs[i].vbo_id == 0 && attribs[i].pointer != 0) {
                attrs[i].base_ptr = reinterpret_cast<const void*>(attribs[i].pointer);
                attrs[i].available_bytes = 16u * 1024u * 1024u;
            }
        }

        const void* idx_ptr = nullptr;
        size_t idx_bytes = 0;
        uint32_t max_vertex_index = 0;

        if (indexed) {
            size_t elem_sz = (index_type == 0x1405) ? 4 : ((index_type == 0x1401) ? 1 : 2);
            const uint8_t* raw = nullptr;
            uint32_t index_count = static_cast<uint32_t>(count);
            if (ebo_bp) {
                idx_ptr = ebo_bp->data.data() + ebo_offset;
                idx_bytes = ebo_bp->data.size() - static_cast<size_t>(ebo_offset);
                uint32_t avail_count = static_cast<uint32_t>(idx_bytes / elem_sz);
                index_count = std::min<uint32_t>(index_count, avail_count);
                raw = static_cast<const uint8_t*>(idx_ptr);
            } else {
                idx_bytes = static_cast<size_t>(count) * elem_sz;
                m_idx_staging.resize(idx_bytes);
                if (!lcs::cpu::DynarmicCore::read_guest_memory(
                        reinterpret_cast<uint64_t>(indices), m_idx_staging.data(), idx_bytes)) {
                    return;
                }
                idx_ptr = m_idx_staging.data();
                raw = m_idx_staging.data();
            }
            auto read_idx = [&](uint32_t i) -> uint32_t {
                if (index_type == 0x1405) {
                    uint32_t v = 0; std::memcpy(&v, raw + i * 4, 4); return v;
                }
                if (index_type == 0x1401) return raw[i];
                uint16_t v = 0; std::memcpy(&v, raw + i * 2, 2); return v;
            };
            for (uint32_t i = 0; i < index_count; i++) {
                uint32_t idx = read_idx(i);
                if (idx > max_vertex_index) max_vertex_index = idx;
            }
        } else {
            max_vertex_index = static_cast<uint32_t>(first_vertex) + static_cast<uint32_t>(count) - 1;
        }

        uint32_t vert_count = indexed ? (max_vertex_index + 1) : static_cast<uint32_t>(count);
        for (uint32_t i = 0; i < 16; i++) {
            if (!attribs[i].enabled || attribs[i].vbo_id != 0 || attribs[i].pointer == 0) continue;
            uint32_t comps = std::min<uint32_t>(attribs[i].size, 4);
            size_t comp_bytes = 4;
            switch (attribs[i].type) {
                case 0x1400: case 0x1401: comp_bytes = 1; break;
                case 0x1402: case 0x1403: comp_bytes = 2; break;
            }
            size_t eff_stride = (attribs[i].stride > 0) ? static_cast<size_t>(attribs[i].stride) : (comp_bytes * comps);
            size_t total_bytes = static_cast<size_t>(vert_count) * eff_stride;
            m_vert_staging[i].resize(total_bytes);
            uint64_t src_ptr = indexed ? attribs[i].pointer : (attribs[i].pointer + static_cast<uint64_t>(first_vertex) * eff_stride);
            if (!lcs::cpu::DynarmicCore::read_guest_memory(src_ptr, m_vert_staging[i].data(), total_bytes)) {
                return;
            }
            attrs[i].base_ptr = m_vert_staging[i].data();
            attrs[i].available_bytes = total_bytes;
        }

        d3d12::draw_d3d12(
            m_bound_program, state, mode,
            static_cast<uint32_t>(std::max(0, first_vertex)),
            static_cast<uint32_t>(count),
            indexed, index_type, idx_ptr, idx_bytes,
            u_ptr, u_size,
            bound_tex, active_mask, attrs);
    }
#else
    (void)mode; (void)first_vertex; (void)count; (void)indexed; (void)index_type; (void)indices;
#endif
}

void RenderBridge::gl_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
    m_draw_calls++;
    if (m_draw_records.size() < 200) {
        std::lock_guard<std::mutex> lock(m_mutex);
        GLDrawRecord rec;
        rec.frame = m_frame_count;
        rec.program = m_bound_program;
        rec.mode = mode;
        rec.count = count;
        rec.indexed = false;
        rec.tex0 = m_bound_textures[0];
        rec.fbo = m_bound_fbo;
        auto& attribs = get_program_attribs(m_bound_program);
        for (uint32_t i = 0; i < 8; i++) {
            rec.attribs[i] = attribs[i];
            if (attribs[i].enabled) rec.active_mask |= (1u << i);
        }
        m_draw_records.push_back(rec);
    }
    dispatch_d3d12_draw(mode, first, count, false, 0, nullptr);
}

void RenderBridge::gl_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void* indices) {
    m_draw_calls++;
    if (m_draw_records.size() < 200) {
        std::lock_guard<std::mutex> lock(m_mutex);
        GLDrawRecord rec;
        rec.frame = m_frame_count;
        rec.program = m_bound_program;
        rec.mode = mode;
        rec.count = count;
        rec.index_type = type;
        rec.indexed = true;
        rec.ebo_id = m_bound_element_buffer;
        rec.tex0 = m_bound_textures[0];
        rec.fbo = m_bound_fbo;
        auto& attribs = get_program_attribs(m_bound_program);
        for (uint32_t i = 0; i < 8; i++) {
            rec.attribs[i] = attribs[i];
            if (attribs[i].enabled) rec.active_mask |= (1u << i);
        }
        m_draw_records.push_back(rec);
    }
    dispatch_d3d12_draw(mode, 0, count, true, type, indices);
}

RenderBridge::BatchProgramView RenderBridge::get_batch_program_view(uint32_t program_id) {
    BatchProgramView view{};
    if (program_id == 0) return view;
    if (m_cached_prog_id != program_id || !m_cached_prog) {
        auto it = m_programs.find(program_id);
        if (it == m_programs.end()) return view;
        m_cached_prog_id = program_id;
        m_cached_prog = &it->second;
    }
    auto& prog = *m_cached_prog;
    view.staging_data    = prog.uniform_staging.empty() ? nullptr : prog.uniform_staging.data();
    view.staging_size    = static_cast<uint32_t>(prog.uniform_staging.size());
    view.compact_bytes   = prog.uniform_compact_bytes ? prog.uniform_compact_bytes : view.staging_size;
    view.uniform_version = &prog.uniform_version;
    view.fog_params_loc  = prog.fog_params_loc;
    return view;
}

bool RenderBridge::get_uniform_layout_offset(uint32_t program_id, int32_t location,
                                             uint32_t& out_offset, uint32_t& out_size) {
    if (program_id == 0 || location < 0) return false;
    if (m_cached_prog_id != program_id || !m_cached_prog) {
        auto it = m_programs.find(program_id);
        if (it == m_programs.end()) return false;
        m_cached_prog_id = program_id;
        m_cached_prog = &it->second;
    }
    const auto& prog = *m_cached_prog;
    const int32_t base_loc = location % 1000;
    const int32_t elem_off = location / 1000;
    if (base_loc < 0 || base_loc >= static_cast<int32_t>(prog.loc_to_member_vec.size())) return false;
    const int32_t midx = prog.loc_to_member_vec[base_loc];
    if (midx < 0) return false;
    const auto& m = prog.uniform_layout[static_cast<size_t>(midx)];
    if (m.is_sampler) return false;
    const uint64_t byte_off = static_cast<uint64_t>(m.byte_offset) + static_cast<uint64_t>(elem_off) * 16ULL;
    if (byte_off >= m.byte_offset + m.byte_size || byte_off >= prog.uniform_staging.size()) return false;
    out_offset = static_cast<uint32_t>(byte_off);
    out_size   = static_cast<uint32_t>((m.byte_offset + m.byte_size) - byte_off);
    return true;
}

void RenderBridge::draw_elements_batched_compact(uint32_t mode, int32_t count, uint32_t type,
                                                 const void* indices, uint32_t uniform_copy_bytes) {
    m_draw_calls++;
    dispatch_d3d12_draw(mode, 0, count, true, type, indices, uniform_copy_bytes);
}

void RenderBridge::gl_get_integerv(uint32_t pname, int32_t* params) {
    if (!params) return;
    switch (pname) {
        case 0x0BA2: // GL_VIEWPORT
            params[0] = m_viewport[0];
            params[1] = m_viewport[1];
            params[2] = m_viewport[2];
            params[3] = m_viewport[3];
            break;
        case 0x0C10: // GL_SCISSOR_BOX
            params[0] = m_scissor[0];
            params[1] = m_scissor[1];
            params[2] = m_scissor[2];
            params[3] = m_scissor[3];
            break;
        case 0x0D33: *params = 4096; break;  // GL_MAX_TEXTURE_SIZE
        case 0x851C: *params = 4096; break;  // GL_MAX_CUBE_MAP_TEXTURE_SIZE
        case 0x84E8: *params = 4096; break;  // GL_MAX_RENDERBUFFER_SIZE
        case 0x8869: *params = 16; break;    // GL_MAX_VERTEX_ATTRIBS
        case 0x8DFB: *params = 256; break;   // GL_MAX_VERTEX_UNIFORM_VECTORS
        case 0x8DFD: *params = 256; break;   // GL_MAX_FRAGMENT_UNIFORM_VECTORS
        case 0x8DFC: *params = 16; break;    // GL_MAX_VARYING_VECTORS
        case 0x8872: *params = 16; break;    // GL_MAX_TEXTURE_IMAGE_UNITS
        case 0x8B4C: *params = 16; break;    // GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS
        case 0x8B4D: *params = 32; break;    // GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS
        case 0x0D52: *params = 8; break;     // GL_RED_BITS
        case 0x0D53: *params = 8; break;     // GL_GREEN_BITS
        case 0x0D54: *params = 8; break;     // GL_BLUE_BITS
        case 0x0D55: *params = 8; break;     // GL_ALPHA_BITS
        case 0x0D56: *params = 24; break;    // GL_DEPTH_BITS
        case 0x0D57: *params = 8; break;     // GL_STENCIL_BITS
        case 0x8CA6: *params = static_cast<int32_t>(m_bound_fbo); break; // GL_FRAMEBUFFER_BINDING
        case 0x8009: *params = 0x8006; break; // GL_BLEND_EQUATION (GL_FUNC_ADD)
        case 0x883D: *params = 0x8006; break; // GL_BLEND_EQUATION_ALPHA
        default:     *params = 0; break;
    }
}

void RenderBridge::gl_get_floatv(uint32_t pname, float* params) {
    if (!params) return;
    switch (pname) {
        case 0x846E: // GL_ALIASED_LINE_WIDTH_RANGE
            params[0] = 1.0f;
            params[1] = 8.0f;
            break;
        case 0x846D: // GL_ALIASED_POINT_SIZE_RANGE
            params[0] = 1.0f;
            params[1] = 64.0f;
            break;
        case 0x84FF: // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
            *params = 16.0f;
            break;
        default:
            *params = 0.0f;
            break;
    }
}

void RenderBridge::gl_get_booleanv(uint32_t pname, uint8_t* params) {
    (void)pname;
    if (params) *params = 0;
}

void RenderBridge::dump_diagnostics(const std::string& filepath) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::ofstream ofs(filepath);
    if (!ofs) return;

    ofs << "=== GTA: LCS RenderBridge Diagnostics ===\n";
    ofs << "Frames: " << m_frame_count << " | Draws: " << m_draw_calls
        << " | Shaders: " << m_shaders.size() << " | Programs: " << m_programs.size()
        << " | Textures: " << m_textures.size() << " | Buffers: " << m_buffers.size() << "\n";
    ofs << "Last ClearColor: (" << m_clear_color[0] << ", " << m_clear_color[1]
        << ", " << m_clear_color[2] << ", " << m_clear_color[3] << ")\n\n";

    std::map<uint32_t, GLProgramObject> sorted_progs(m_programs.begin(), m_programs.end());
    for (const auto& [pid, prog] : sorted_progs) {
        ofs << "--- Program " << pid << " (VS=" << prog.vertex_shader
            << ", FS=" << prog.fragment_shader << ") ---\n";
        ofs << "Attribs:";
        for (const auto& [aname, aloc] : prog.attrib_locations) {
            ofs << " " << aname << "=" << aloc;
        }
        ofs << "\nUniforms:";
        for (const auto& [uname, uloc] : prog.uniform_locations) {
            ofs << " " << uname << "=" << uloc;
        }
        ofs << "\n";
        auto vs_it = m_shaders.find(prog.vertex_shader);
        if (vs_it != m_shaders.end()) {
            ofs << "[VS " << prog.vertex_shader << " (preprocessed)]:\n" << vs_it->second.preprocessed << "\n";
        }
        auto fs_it = m_shaders.find(prog.fragment_shader);
        if (fs_it != m_shaders.end()) {
            ofs << "[FS " << prog.fragment_shader << " (preprocessed)]:\n" << fs_it->second.preprocessed << "\n";
        }
        if (vs_it != m_shaders.end() && fs_it != m_shaders.end()) {
            auto hlsl = translate_program_to_hlsl(vs_it->second.preprocessed, fs_it->second.preprocessed, prog.attrib_locations);
            ofs << "[Program " << pid << " VS HLSL]:\n" << hlsl.vs_hlsl << "\n";
            ofs << "[Program " << pid << " PS HLSL]:\n" << hlsl.ps_hlsl << "\n";
        }
    }

    ofs << "\n=== Textures Summary (first 30 & >= 488) ===\n";
    std::map<uint32_t, GLTextureObject> sorted_tex(m_textures.begin(), m_textures.end());
    int tcount = 0;
    for (const auto& [tid, tex] : sorted_tex) {
        if (tcount < 30 || tid >= 488) {
            ofs << "Tex " << tid << ": " << tex.width << "x" << tex.height
                << " int=0x" << std::hex << tex.internal_format
                << " fmt=0x" << tex.format << " type=0x" << tex.type
                << " bytes=" << std::dec << tex.pixels.size() << "\n";
        }
        tcount++;
    }

    ofs << "\n=== Buffers Summary (first 30) ===\n";
    std::map<uint32_t, GLBufferObject> sorted_buf(m_buffers.begin(), m_buffers.end());
    int bcount = 0;
    for (const auto& [bid, buf] : sorted_buf) {
        if (bcount++ >= 30) break;
        ofs << "Buf " << bid << ": target=0x" << std::hex << buf.target
            << " bytes=" << std::dec << buf.data.size() << "\n";
    }

    ofs << "\n=== Draw Records (count=" << m_draw_records.size() << ") ===\n";
    for (size_t i = 0; i < m_draw_records.size(); i++) {
        const auto& r = m_draw_records[i];
        ofs << "Draw #" << i << " frame=" << r.frame
            << " prog=" << r.program
            << (r.indexed ? " Elements" : " Arrays")
            << " mode=" << r.mode
            << " count=" << r.count
            << " idxType=0x" << std::hex << r.index_type << std::dec
            << " ebo=" << r.ebo_id
            << " tex0=" << r.tex0
            << " fbo=" << r.fbo
            << " mask=0x" << std::hex << r.active_mask << std::dec << "\n";
        for (uint32_t a = 0; a < 8; a++) {
            if (r.active_mask & (1u << a)) {
                const auto& attr = r.attribs[a];
                ofs << "    attr[" << a << "] size=" << attr.size
                    << " type=0x" << std::hex << attr.type << std::dec
                    << " norm=" << (attr.normalized ? 1 : 0)
                    << " stride=" << attr.stride
                    << " vbo=" << attr.vbo_id
                    << " ptr=0x" << std::hex << attr.pointer << std::dec << "\n";
            }
        }
    }
}

void RenderBridge::set_loading_bar(bool active, float progress) {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::set_d3d12_loading_bar(active, progress);
    }
#else
    (void)active;
    (void)progress;
#endif
}

void RenderBridge::present_loading_bar(float progress) {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::present_d3d12_loading_bar(progress);
    }
#else
    (void)progress;
#endif
}

void init_render_bridge() {
    RenderBridge::instance().initialize(RenderBackend::Direct3D12);
}

void dispatch_frame() {
    RenderBridge::instance().egl_swap_buffers(0, 0);
}

} // namespace lcs::graphics
