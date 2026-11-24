#pragma once

#include "shader_compiler.h"

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

namespace lcs::graphics {

enum class RenderBackend {
    Direct3D12,
    Vulkan
};

struct Ps2PostProcessParams {
    bool enabled = false;
    bool reset_history = false;
    float blur_rgb[3] = {0.0f, 0.0f, 0.0f};         // (255 - FinalColour.rgb) / 255.0f (CTimeCycle BlurRGB)
    float final_color_rgb[3] = {1.0f, 1.0f, 1.0f};  // FinalColour.rgb / 255.0f
    float trail_tint_rgb[3] = {1.0f, 1.0f, 1.0f};   // (1,1,1) normal PS2 0x00272348 trails, or 2*FinalColour
    float radiosity_intensity = 1.0f;               // 0.0 .. 2.0 (PS2 0x001d17d8 Radiosity / Bloom)
    float color_filter_intensity = 1.0f;            // 0.0 .. 2.0 (PS2 BlurRGB Color Filter)
    float trails_persistence = 0.0f;                // 0.0 .. 0.92 (PS2 0x00272348 Frame-History Trails)
    float speed_blur_factor = 0.0f;                 // 0.0 .. 1.0 (High-speed vehicle motion blur)
    float drunk_blur_factor = 0.0f;                 // 0.0 .. 1.0 (CMBlur::m_fDrunkBlur)
    float anim_phase = 0.0f;                        // Time in seconds for drunk-blur wave
    float soft_focus_weight = 1.0f;                 // 1.0 = PS2 0.188 sub-pixel box4, 0.2 = Hybrid crisp PC
};

struct GLShaderObject {
    uint32_t id = 0;
    uint32_t type = 0; // GL_VERTEX_SHADER (0x8B31) or GL_FRAGMENT_SHADER (0x8B30)
    std::string source;
    std::string preprocessed;
    bool compiled = false;
};

struct GLProgramObject {
    uint32_t id = 0;
    uint32_t vertex_shader = 0;
    uint32_t fragment_shader = 0;
    bool linked = false;
    std::vector<ShaderVariableInfo> active_attribs;
    std::vector<ShaderVariableInfo> active_uniforms;
    std::unordered_map<std::string, int32_t> attrib_locations;
    std::unordered_map<std::string, int32_t> uniform_locations;
    std::vector<UniformMemberLayout> uniform_layout;
    std::vector<uint8_t> uniform_staging;
    std::unordered_map<int32_t, size_t> loc_to_member;
    std::vector<int32_t> loc_to_member_vec;
    int32_t fog_params_loc = -1;
    uint32_t uniform_compact_bytes = 0;
    uint64_t uniform_version = 1;
    uint32_t sampler_units[8]{0, 1, 2, 3, 4, 5, 6, 7};
    int32_t next_attrib_loc = 0;
    int32_t next_uniform_loc = 1;
};

struct GLBufferObject {
    uint32_t id = 0;
    uint32_t target = 0;
    uint32_t usage = 0;
    uint64_t version = 0;
    uint32_t mutations = 0;
    std::vector<uint8_t> data;
};

struct GLTextureObject {
    uint32_t id = 0;
    uint32_t target = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t internal_format = 0;
    uint32_t format = 0;
    uint32_t type = 0;
    std::vector<uint8_t> pixels;
    uint32_t wrap_s = 0x2901;  // GL_REPEAT
    uint32_t wrap_t = 0x2901;  // GL_REPEAT
    uint32_t mag_filter = 0x2600; // GL_LINEAR
    uint32_t min_filter = 0x2600; // GL_LINEAR
};

struct GLFramebufferObject {
    uint32_t id = 0;
    uint32_t color_tex_id = 0;
    uint32_t depth_tex_id = 0;
    uint32_t width = 1024;
    uint32_t height = 1024;
};

struct GLVertexAttribState {
    bool enabled = false;
    int32_t size = 4;
    uint32_t type = 0x1406; // GL_FLOAT
    bool normalized = false;
    int32_t stride = 0;
    uint64_t pointer = 0; // byte offset if vbo_id != 0, else guest pointer
    uint32_t vbo_id = 0;
};

struct GLFastAttrRef {
    uint32_t vbo_id = 0;
    uint64_t vbo_version = 0;
    uint32_t vbo_mutations = 0;
    const uint8_t* vbo_data = nullptr;
    size_t vbo_size = 0;
    uint64_t pointer_offset = 0;
    uint32_t size = 4;
    uint32_t type = 0x1406;
    uint32_t stride = 0;
    uint8_t normalized = 0;
    uint8_t enabled = 0;
    float default_val[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};

struct GLDrawRecord {
    uint64_t frame = 0;
    uint32_t program = 0;
    uint32_t mode = 0;
    int32_t count = 0;
    uint32_t index_type = 0;
    bool indexed = false;
    uint32_t ebo_id = 0;
    uint32_t tex0 = 0;
    uint32_t fbo = 0;
    uint32_t active_mask = 0;
    GLVertexAttribState attribs[8];
};

class RenderBridge {
public:
    static RenderBridge& instance();

    bool initialize(RenderBackend backend = RenderBackend::Direct3D12,
                    void* native_window = nullptr,
                    uint32_t width = 1280,
                    uint32_t height = 720);
    void shutdown();

    // Frame boundary
    void wait_for_present_slot();
    void begin_frame();
    void end_frame_and_present();
    void set_vsync_enabled(bool enabled);
    bool is_vsync_enabled() const { return m_vsync_enabled; }
    void set_color_grading(float brightness_mul, float contrast_mul, float saturation_mul);
    void set_texture_stochastic(uint32_t tex_id, bool enabled);
    void set_stochastic_mode(int mode);
    void queue_ps2_post_process(const Ps2PostProcessParams& params);
    void flush_pending_ps2_post_process();
    void apply_ps2_post_process(const Ps2PostProcessParams& params);
    void gl_flush();
    uint64_t buffer_delete_epoch() const { return m_buffer_delete_epoch; }

    // EGL Emulation API
    uint64_t egl_get_display(uint64_t native_display);
    uint32_t egl_initialize(uint64_t dpy, int32_t* major, int32_t* minor);
    uint32_t egl_terminate(uint64_t dpy);
    uint32_t egl_choose_config(uint64_t dpy, const int32_t* attrib_list, uint64_t* configs, int32_t config_size, int32_t* num_config);
    uint32_t egl_get_config_attrib(uint64_t dpy, uint64_t config, int32_t attribute, int32_t* value);
    uint64_t egl_create_window_surface(uint64_t dpy, uint64_t config, uint64_t native_window, const int32_t* attrib_list);
    uint64_t egl_create_context(uint64_t dpy, uint64_t config, uint64_t share_context, const int32_t* attrib_list);
    uint32_t egl_make_current(uint64_t dpy, uint64_t draw, uint64_t read, uint64_t ctx);
    uint32_t egl_swap_buffers(uint64_t dpy, uint64_t surface);
    uint32_t egl_swap_interval(uint64_t dpy, int32_t interval);
    uint32_t egl_query_surface(uint64_t dpy, uint64_t surface, int32_t attribute, int32_t* value);
    uint32_t egl_destroy_surface(uint64_t dpy, uint64_t surface);
    uint32_t egl_destroy_context(uint64_t dpy, uint64_t ctx);
    uint32_t egl_get_error();

    // GLES State & Draw Bridge
    void gl_viewport(int32_t x, int32_t y, int32_t width, int32_t height);
    void gl_scissor(int32_t x, int32_t y, int32_t width, int32_t height);
    void gl_clear_color(float r, float g, float b, float a);
    void gl_clear_depth(float depth);
    void gl_clear_stencil(int32_t stencil);
    void gl_clear(uint32_t mask);

    uint32_t gl_create_shader(uint32_t type);
    void gl_delete_shader(uint32_t shader);
    void gl_shader_source(uint32_t shader, int32_t count, const char* const* strings, const int32_t* lengths);
    void gl_compile_shader(uint32_t shader);
    uint32_t gl_create_program();
    void gl_delete_program(uint32_t program);
    void gl_attach_shader(uint32_t program, uint32_t shader);
    void gl_bind_attrib_location(uint32_t program, uint32_t index, const char* name);
    void gl_link_program(uint32_t program);
    void gl_use_program(uint32_t program);
    void gl_get_programiv(uint32_t program, uint32_t pname, int32_t* params);
    void gl_get_active_attrib(uint32_t program, uint32_t index, int32_t buf_size,
                              int32_t* length, int32_t* size, uint32_t* type, char* name);
    void gl_get_active_uniform(uint32_t program, uint32_t index, int32_t buf_size,
                               int32_t* length, int32_t* size, uint32_t* type, char* name);
    int32_t gl_get_attrib_location(uint32_t program, const char* name);
    int32_t gl_get_uniform_location(uint32_t program, const char* name);

    void gl_gen_buffers(int32_t n, uint32_t* buffers);
    void gl_delete_buffers(int32_t n, const uint32_t* buffers);
    void gl_bind_buffer(uint32_t target, uint32_t buffer);
    void gl_buffer_data(uint32_t target, ptrdiff_t size, const void* data, uint32_t usage);
    void gl_buffer_sub_data(uint32_t target, ptrdiff_t offset, ptrdiff_t size, const void* data);

    void gl_gen_textures(int32_t n, uint32_t* textures);
    void gl_delete_textures(int32_t n, const uint32_t* textures);
    void gl_active_texture(uint32_t texture);
    void gl_bind_texture(uint32_t target, uint32_t texture);
    void gl_tex_image_2d(uint32_t target, int32_t level, int32_t internal_format,
                         int32_t width, int32_t height, int32_t border,
                         uint32_t format, uint32_t type, const void* pixels);
    void gl_tex_sub_image_2d(uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset,
                             int32_t width, int32_t height, uint32_t format, uint32_t type, const void* pixels);
    void gl_compressed_tex_image_2d(uint32_t target, int32_t level, uint32_t internal_format,
                                    int32_t width, int32_t height, int32_t border,
                                    int32_t image_size, const void* data);
    void gl_tex_parameteri(uint32_t target, uint32_t pname, int32_t param);
    void gl_tex_parameterf(uint32_t target, uint32_t pname, float param);

    // Direct host texture inspection & RGBA8 replacement (used for dynamic Classic PC/gamepad prompts).
    bool has_texture(uint32_t tex_id) const;
    void replace_texture_rgba8(uint32_t tex_id, uint32_t width, uint32_t height, const void* rgba_pixels);

    void gl_gen_framebuffers(int32_t n, uint32_t* framebuffers);
    void gl_delete_framebuffers(int32_t n, const uint32_t* framebuffers);
    void gl_bind_framebuffer(uint32_t target, uint32_t fbo);
    void gl_framebuffer_texture_2d(uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level);
    void gl_enable(uint32_t cap);
    void gl_disable(uint32_t cap);
    void gl_blend_func(uint32_t sfactor, uint32_t dfactor);
    void gl_blend_func_separate(uint32_t src_rgb, uint32_t dst_rgb, uint32_t src_alpha, uint32_t dst_alpha);
    void gl_blend_equation(uint32_t mode);
    void gl_blend_equation_separate(uint32_t mode_rgb, uint32_t mode_alpha);
    void gl_depth_func(uint32_t func);
    void gl_depth_mask(uint8_t flag);
    void gl_stencil_func(uint32_t func, int32_t ref, uint32_t mask);
    void gl_stencil_op(uint32_t sfail, uint32_t zfail, uint32_t zpass);
    void gl_stencil_mask(uint32_t mask);
    void gl_cull_face(uint32_t mode);
    void gl_front_face(uint32_t mode);
    void gl_color_mask(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void gl_vertex_attrib4f(uint32_t index, float x, float y, float z, float w);
    void gl_uniform_iv(int32_t location, int32_t count, int32_t components, const int32_t* v);
    void gl_uniform_fv(int32_t location, int32_t count, int32_t components, const float* v);
    void gl_uniform_matrix_fv(int32_t location, int32_t count, int32_t dim, uint8_t transpose, const float* value);
    void gl_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                  uint8_t normalized, int32_t stride, uint64_t pointer);
    void gl_enable_vertex_attrib_array(uint32_t index);
    void gl_disable_vertex_attrib_array(uint32_t index);
    void gl_gen_vertex_arrays(int32_t n, uint32_t* arrays);
    void gl_delete_vertex_arrays(int32_t n, const uint32_t* arrays);
    void gl_bind_vertex_array(uint32_t array);
    void gl_draw_arrays(uint32_t mode, int32_t first, int32_t count);
    void gl_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void* indices);

    struct BatchProgramView {
        uint8_t*  staging_data = nullptr;
        uint32_t  staging_size = 0;
        uint32_t  compact_bytes = 0;
        uint64_t* uniform_version = nullptr;
        int32_t   fog_params_loc = -1;
    };
    BatchProgramView get_batch_program_view(uint32_t program_id);
    bool get_uniform_layout_offset(uint32_t program_id, int32_t location, uint32_t& out_offset, uint32_t& out_size);
    void draw_elements_batched_compact(uint32_t mode, int32_t count, uint32_t type,
                                       const void* indices, uint32_t uniform_copy_bytes);
    void gl_get_integerv(uint32_t pname, int32_t* params);
    void gl_get_floatv(uint32_t pname, float* params);
    void gl_get_booleanv(uint32_t pname, uint8_t* params);

    // Surface dimensions & diagnostics
    void set_surface_dimensions(uint32_t width, uint32_t height);
    void set_loading_bar(bool active, float progress);
    void present_loading_bar(float progress);
    void set_video_frame(uint32_t width, uint32_t height, const void* rgba_pixels);
    void clear_video_frame();
    uint32_t get_surface_width() const { return m_width; }
    uint32_t get_surface_height() const { return m_height; }

    void set_guest_dimensions(uint32_t width, uint32_t height);
    uint32_t get_guest_width() const { return m_guest_width; }
    uint32_t get_guest_height() const { return m_guest_height; }
    void get_content_rect(float& out_x, float& out_y, float& out_w, float& out_h) const;

    uint64_t get_frame_count() const { return m_frame_count; }
    uint64_t get_draw_call_count() const { return m_draw_calls; }
    size_t get_shader_count() const { return m_shaders.size(); }
    size_t get_program_count() const { return m_programs.size(); }
    size_t get_texture_count() const { return m_textures.size(); }
    size_t get_buffer_count() const { return m_buffers.size(); }
    void dump_diagnostics(const std::string& filepath) const;

private:
    RenderBridge();
    ~RenderBridge();

    mutable std::mutex m_mutex;
    mutable std::mutex m_ps2_mutex;
    Ps2PostProcessParams m_pending_ps2_params{};
    bool m_has_pending_ps2_params = false;
    bool m_ps2_applied_this_frame = false;

    RenderBackend m_backend = RenderBackend::Direct3D12;
    bool m_initialized = false;
    bool m_has_window = false;
    uint32_t m_width = 1280;
    uint32_t m_height = 720;
    uint32_t m_guest_width = 1280;
    uint32_t m_guest_height = 720;
    uint64_t m_frame_count = 0;
    uint64_t m_draw_calls = 0;
    uint32_t m_last_error = 0x3000; // EGL_SUCCESS
    int32_t m_swap_interval = 1;
    bool m_vsync_enabled = false;

    uint64_t m_current_context = 0;
    uint64_t m_current_draw_surface = 0;
    uint64_t m_current_read_surface = 0;

    float m_clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float m_clear_depth = 1.0f;
    int32_t m_clear_stencil = 0;
    int32_t m_viewport[4] = {0, 0, 1280, 720};
    int32_t m_scissor[4] = {0, 0, 1280, 720};

    uint32_t m_next_shader_id = 1;
    uint32_t m_next_program_id = 1;
    uint32_t m_next_buffer_id = 1;
    uint32_t m_next_texture_id = 1;
    uint32_t m_next_fbo_id = 1;

    void dispatch_d3d12_draw(uint32_t mode, int32_t first_vertex, int32_t count,
                             bool indexed, uint32_t index_type, const void* indices,
                             uint32_t uniform_copy_bytes = 0);
    void compute_guest_transform(float& out_scale, float& out_off_x, float& out_off_y,
                                 float& out_fit_w, float& out_fit_h) const;

    uint32_t m_bound_program = 0;
    uint32_t m_bound_array_buffer = 0;
    uint32_t m_bound_element_buffer = 0;
    uint32_t m_bound_vao = 0;
    struct GLVaoObject {
        bool valid = false;
        uint16_t enabled_mask = 0;
        uint32_t element_buffer = 0;
        uint64_t gen = 1;
        bool cached_valid = false;
        bool cached_all_attribs_vbo = false;
        uint64_t cached_buf_table_epoch = 0;
        uint64_t cached_default_attr_seq = 0;
        uint8_t cached_active_count = 0;
        std::array<uint8_t, 16> cached_active_locs{};
        std::array<GLBufferObject*, 16> cached_attr_bps{};
        GLBufferObject* cached_ebo_bp = nullptr;
        std::array<GLFastAttrRef, 16> cached_fast_attrs{};
        std::array<GLVertexAttribState, 16> attribs{};
    };
    uint64_t m_next_vao_gen = 2;
    std::vector<GLVaoObject> m_vaos;
    std::vector<uint32_t> m_free_vaos;
    std::array<GLVertexAttribState, 16>* m_active_attribs = nullptr;
    uint32_t m_bound_fbo = 0;
    uint32_t m_active_tex_unit = 0;
    uint32_t m_bound_textures[16]{};
    uint32_t m_bound_texture_2d = 0;

    uint64_t m_render_state_seq = 1;
    uint64_t m_default_attr_seq = 1;
    bool m_blend_enabled = false;
    uint32_t m_blend_src_rgb = 1;
    uint32_t m_blend_dst_rgb = 0;
    uint32_t m_blend_src_alpha = 1;
    uint32_t m_blend_dst_alpha = 0;
    uint32_t m_blend_eq_rgb = 0x8006;
    uint32_t m_blend_eq_alpha = 0x8006;
    bool m_depth_test = false;
    bool m_depth_write = true;
    uint32_t m_depth_func = 0x0201;
    bool m_stencil_test = false;
    uint32_t m_stencil_func = 0x0207;   // GL_ALWAYS
    uint8_t m_stencil_ref = 0;
    uint8_t m_stencil_read_mask = 0xFF;
    uint8_t m_stencil_write_mask = 0xFF;
    uint32_t m_stencil_sfail = 0x1E00;  // GL_KEEP
    uint32_t m_stencil_zfail = 0x1E00;  // GL_KEEP
    uint32_t m_stencil_zpass = 0x1E00;  // GL_KEEP
    bool m_cull_enabled = false;
    uint32_t m_cull_mode = 0x0405;
    uint32_t m_front_face = 0x0901;
    bool m_scissor_enabled = false;
    uint8_t m_color_mask = 0x0F;

    std::vector<uint8_t> m_idx_staging;
    std::array<std::vector<uint8_t>, 16> m_vert_staging;

    std::unordered_map<uint32_t, GLShaderObject> m_shaders;
    std::unordered_map<uint32_t, GLProgramObject> m_programs;
    std::unordered_map<uint32_t, GLBufferObject> m_buffers;
    std::unordered_map<uint32_t, GLTextureObject> m_textures;
    std::unordered_map<uint32_t, GLFramebufferObject> m_framebuffers;
    float m_default_attribs[16][4]{};
    std::vector<GLDrawRecord> m_draw_records;

    // Lock-free O(1) render-thread caches (STL maps never invalidate element pointers on insert)
    uint32_t m_cached_prog_id = 0;
    GLProgramObject* m_cached_prog = nullptr;
    std::vector<GLBufferObject*> m_buffer_ptrs;
    uint64_t m_buffer_delete_epoch = 0;
    uint64_t m_buffer_table_epoch = 1;

    inline GLBufferObject* lookup_buffer_fast(uint32_t id) const {
        return (id < m_buffer_ptrs.size()) ? m_buffer_ptrs[id] : nullptr;
    }

    std::array<GLVertexAttribState, 16>& get_program_attribs(uint32_t program) {
        (void)program;
        return m_vaos[m_bound_vao].attribs;
    }
};

void init_render_bridge();
void dispatch_frame();

} // namespace lcs::graphics
