#pragma once

namespace lcs::cpu {
class ElfImage;
class DynarmicCore;
} // namespace lcs::cpu

namespace lcs::game {

// Stochastic (hex-tiling) texturing for repeating world textures, after SkyGfx Extended:
// hooks the game's PVR/PNG texture upload (lglTexture: GL id at +4, file path at +0x40) and tags
// GL textures whose name is on the whitelist; the D3D12 pixel shader then samples them three
// times at random offsets pinned to a triangle grid (src/graphics/shader_compiler.cpp).
// LCS_DIAG_TEXNAMES=1 logs every uploaded texture path once ([TexName]).
void install_stochastic_texture_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);

} // namespace lcs::game
