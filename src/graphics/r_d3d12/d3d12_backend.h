#pragma once

#include "graphics/render_bridge.h"
#include "graphics/shader_compiler.h"
#include "graphics/texture_pipeline.h"

#include <cstdint>
#include <memory>
#include <vector>

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

    bool stencil_enable = false;
    uint32_t stencil_func = 0x0207;   // GL_ALWAYS
    uint8_t stencil_ref = 0;
    uint8_t stencil_read_mask = 0xFF;
    uint8_t stencil_write_mask = 0xFF;
    uint32_t stencil_sfail = 0x1E00;  // GL_KEEP
    uint32_t stencil_zfail = 0x1E00;  // GL_KEEP
    uint32_t stencil_zpass = 0x1E00;  // GL_KEEP

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
void bind_d3d12_framebuffer(uint32_t fbo_id, uint32_t color_tex_id, uint32_t depth_tex_id,
                            uint32_t width, uint32_t height);
void set_d3d12_color_grading(float brightness_mul, float contrast_mul, float saturation_mul);
void apply_d3d12_ps2_post_process(const Ps2PostProcessParams& params);

bool register_d3d12_program(uint32_t prog_id, const TranslatedProgramHLSL& translated);
void unregister_d3d12_program(uint32_t prog_id);

void upload_d3d12_texture(uint32_t tex_id, uint32_t width, uint32_t height,
                          uint32_t format, uint32_t type, const void* pixels);
void upload_d3d12_texture_rgba8(uint32_t tex_id, uint32_t width, uint32_t height,
                                std::vector<uint8_t>&& rgba_pixels);
void update_d3d12_texture_sub(uint32_t tex_id, int32_t xoffset, int32_t yoffset,
                              uint32_t width, uint32_t height,
                              uint32_t format, uint32_t type, const void* pixels);
void set_d3d12_texture_param(uint32_t tex_id, uint32_t pname, int32_t param);
void delete_d3d12_texture(uint32_t tex_id);
// Uploads the result of an asynchronous TextureJob (texture_pipeline.h) once it finishes.
void upload_d3d12_texture_async(uint32_t tex_id, std::shared_ptr<lcs::graphics::TextureJob> job);

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

void wait_d3d12_frame_latency();
void end_d3d12_frame_and_present(uint32_t sync_interval = 1);
void set_d3d12_loading_bar(bool active, float progress);
void present_d3d12_loading_bar(float progress);
void set_d3d12_video_frame(uint32_t width, uint32_t height, const void* rgba_pixels);
void clear_d3d12_video_frame();
void shutdown_d3d12();
bool is_d3d12_initialized();
void set_d3d12_screenshot_request(const char* filepath);

using D3D12FastAttrRef = GLFastAttrRef;

struct D3D12FastVaoLayout {
    uint32_t vao_id = 0;
    uint64_t vao_gen = 0;
    uint64_t default_attr_seq = 0;
    uint64_t state_seq = 0;
};

struct D3D12FastEboRef {
    uint32_t ebo_id = 0;
    uint64_t ebo_version = 0;
    uint32_t ebo_mutations = 0;
    const uint8_t* ebo_data = nullptr;
    size_t ebo_size = 0;
    uint64_t index_byte_offset = 0;
};

void draw_d3d12_fast(
    uint32_t prog_id,
    const D3D12RenderState& state,
    uint32_t gl_mode,
    uint32_t first_vertex,
    uint32_t count,
    bool indexed,
    uint32_t index_type,
    const D3D12FastEboRef& ebo,
    const uint8_t* uniform_data,
    uint32_t uniform_bytes,
    uint64_t uniform_version,
    const uint32_t bound_tex_ids[8],
    uint32_t active_attr_mask,
    const D3D12FastAttrRef attrs[16],
    const D3D12FastVaoLayout* vao_hint = nullptr,
    uint32_t eff_uniform_bytes = 0);

void notify_d3d12_buffer_deleted(uint32_t buf_id);

} // namespace lcs::graphics::d3d12
