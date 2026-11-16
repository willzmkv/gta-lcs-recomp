#pragma once

#include "graphics/shader_compiler.h"

#include <cstdint>

namespace lcs::graphics::d3d12 {

struct D3D12RenderState {
    bool blend_enabled = false;
    uint32_t blend_src_rgb = 1;       // GL_ONE
    uint32_t blend_dst_rgb = 0;       // GL_ZERO
    uint32_t blend_src_alpha = 1;     // GL_ONE
    uint32_t blend_dst_alpha = 0;     // GL_ZERO
    uint32_t blend_eq_rgb = 0x8006;   // GL_FUNC_ADD
    uint32_t blend_eq_alpha = 0x8006; // GL_FUNC_ADD

    bool depth_test = false;
    bool depth_write = true;
    uint32_t depth_func = 0x0201;     // GL_LESS

    bool cull_enabled = false;
    uint32_t cull_mode = 0x0405;      // GL_BACK
    uint32_t front_face = 0x0901;     // GL_CCW

    bool scissor_enabled = false;
    uint8_t color_mask = 0xF;
};

struct D3D12VertexAttrDraw {
    int32_t size = 4;
    uint32_t type = 0x1406; // GL_FLOAT
    bool normalized = false;
    int32_t stride = 0;
    const void* base_ptr = nullptr;
    size_t available_bytes = 0;
    float default_val[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

bool init_d3d12_device(void* native_window, uint32_t width, uint32_t height);
void resize_d3d12_swapchain(uint32_t width, uint32_t height);
void begin_d3d12_frame();
void clear_d3d12_targets(bool clear_color, const float color[4],
                         bool clear_depth, float depth,
                         bool clear_stencil, uint8_t stencil);
void set_d3d12_viewport(float x, float y, float width, float height, float min_depth, float max_depth);
void set_d3d12_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height);

bool register_d3d12_program(uint32_t prog_id, const TranslatedProgramHLSL& translated);
void unregister_d3d12_program(uint32_t prog_id);

void upload_d3d12_texture(uint32_t tex_id, uint32_t width, uint32_t height,
                          uint32_t format, uint32_t type, const void* pixels);
void update_d3d12_texture_sub(uint32_t tex_id, int32_t xoffset, int32_t yoffset,
                              uint32_t width, uint32_t height,
                              uint32_t format, uint32_t type, const void* pixels);
void delete_d3d12_texture(uint32_t tex_id);

void draw_d3d12(uint32_t prog_id,
                const D3D12RenderState& state,
                uint32_t gl_mode,
                uint32_t first_vertex,
                uint32_t count,
                bool indexed,
                uint32_t index_type,
                const void* index_ptr,
                size_t index_available_bytes,
                const uint8_t* uniform_data,
                uint32_t uniform_size,
                const uint32_t bound_textures[8],
                uint32_t active_attrib_mask,
                const D3D12VertexAttrDraw attribs[16]);

void end_d3d12_frame_and_present(uint32_t sync_interval = 1);
void shutdown_d3d12();
bool is_d3d12_initialized();

} // namespace lcs::graphics::d3d12
