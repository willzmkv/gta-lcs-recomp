#include "render_bridge.h"

#if defined(LCS_ENABLE_D3D12)
#include "r_d3d12/d3d12_backend.h"
#endif

#include <algorithm>
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

} // namespace

RenderBridge& RenderBridge::instance() {
    static RenderBridge s_instance;
    return s_instance;
}

RenderBridge::RenderBridge() {
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
    d3d12::shutdown_d3d12();
#endif
    m_initialized = false;
}

void RenderBridge::begin_frame() {
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::begin_d3d12_frame();
    }
#endif
}

void RenderBridge::end_frame_and_present() {
    m_frame_count++;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        uint32_t sync = static_cast<uint32_t>(std::clamp(m_swap_interval, 0, 1));
        d3d12::end_d3d12_frame_and_present(sync);
    }
#endif
}

void RenderBridge::set_surface_dimensions(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_width = width;
    m_height = height;
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::resize_d3d12_swapchain(width, height);
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
        case 0x3057: *value = static_cast<int32_t>(m_width); break;
        case 0x3056: *value = static_cast<int32_t>(m_height); break;
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
    if (m_bound_fbo == 0 && d3d12::is_d3d12_initialized()) {
        // Flip Y from GL bottom-left to D3D12 top-left.
        float top_y = static_cast<float>(static_cast<int32_t>(m_height) - (y + height));
        d3d12::set_d3d12_viewport(static_cast<float>(x), top_y,
                                  static_cast<float>(std::max(1, width)),
                                  static_cast<float>(std::max(1, height)),
                                  0.0f, 1.0f);
    }
#endif
}

void RenderBridge::gl_scissor(int32_t x, int32_t y, int32_t width, int32_t height) {
    m_scissor[0] = x;
    m_scissor[1] = y;
    m_scissor[2] = width;
    m_scissor[3] = height;
#if defined(LCS_ENABLE_D3D12)
    if (m_bound_fbo == 0 && d3d12::is_d3d12_initialized()) {
        int32_t top_y = static_cast<int32_t>(m_height) - (y + height);
        d3d12::set_d3d12_scissor(x, top_y,
                                 static_cast<uint32_t>(std::max(0, width)),
                                 static_cast<uint32_t>(std::max(0, height)));
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
    if (m_bound_fbo == 0 && d3d12::is_d3d12_initialized()) {
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
}

void RenderBridge::gl_use_program(uint32_t program) {
    m_bound_program = program;
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
        int32_t copy_len = std::min(static_cast<int32_t>(var.name.size()), buf_size - 1);
        if (copy_len > 0) std::memcpy(name, var.name.data(), static_cast<size_t>(copy_len));
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
    if (uit != it->second.uniform_locations.end()) {
        return uit->second + (elem_offset > 0 ? elem_offset * 1000 : 0);
    }
    return -1;
}

void RenderBridge::gl_gen_buffers(int32_t n, uint32_t* buffers) {
    if (!buffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        uint32_t id = m_next_buffer_id++;
        buffers[i] = id;
        GLBufferObject buf;
        buf.id = id;
        m_buffers[id] = std::move(buf);
    }
}

void RenderBridge::gl_delete_buffers(int32_t n, const uint32_t* buffers) {
    if (!buffers || n <= 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int32_t i = 0; i < n; i++) {
        m_buffers.erase(buffers[i]);
    }
}

void RenderBridge::gl_bind_buffer(uint32_t target, uint32_t buffer) {
    if (target == kGLArrayBuffer) {
        m_bound_array_buffer = buffer;
    } else if (target == kGLElementArrayBuffer) {
        m_bound_element_buffer = buffer;
    }
}

void RenderBridge::gl_buffer_data(uint32_t target, ptrdiff_t size, const void* data, uint32_t usage) {
    if (size < 0) return;
    uint32_t id = (target == kGLElementArrayBuffer) ? m_bound_element_buffer : m_bound_array_buffer;
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& buf = m_buffers[id];
    buf.id = id;
    buf.target = target;
    buf.usage = usage;
    buf.data.resize(static_cast<size_t>(size));
    if (data && size > 0) {
        std::memcpy(buf.data.data(), data, static_cast<size_t>(size));
    }
}

void RenderBridge::gl_buffer_sub_data(uint32_t target, ptrdiff_t offset, ptrdiff_t size, const void* data) {
    if (offset < 0 || size <= 0 || !data) return;
    uint32_t id = (target == kGLElementArrayBuffer) ? m_bound_element_buffer : m_bound_array_buffer;
    if (id == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_buffers.find(id);
    if (it == m_buffers.end()) return;
    const size_t end = static_cast<size_t>(offset) + static_cast<size_t>(size);
    if (end <= it->second.data.size()) {
        std::memcpy(it->second.data.data() + offset, data, static_cast<size_t>(size));
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
    (void)target;
    m_bound_textures[m_active_tex_unit] = texture;
}

void RenderBridge::gl_tex_image_2d(uint32_t target, int32_t level, int32_t internal_format,
                                   int32_t width, int32_t height, int32_t border,
                                   uint32_t format, uint32_t type, const void* pixels) {
    (void)border;
    if (level != 0 || width <= 0 || height <= 0) return;
    uint32_t id = m_bound_textures[m_active_tex_unit];
    if (id == 0) return;

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
        if (pixels) {
            size_t bpp = estimate_pixel_bytes(format, type);
            size_t total = static_cast<size_t>(width) * static_cast<size_t>(height) * bpp;
            tex.pixels.resize(total);
            std::memcpy(tex.pixels.data(), pixels, total);
        }
    }
#if defined(LCS_ENABLE_D3D12)
    if (m_backend == RenderBackend::Direct3D12 && d3d12::is_d3d12_initialized()) {
        d3d12::upload_d3d12_texture(id, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                    format, type, pixels);
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

    {
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

void RenderBridge::gl_bind_framebuffer(uint32_t target, uint32_t fbo) {
    (void)target;
    m_bound_fbo = fbo;
}

void RenderBridge::gl_enable(uint32_t cap) {
    switch (cap) {
        case 0x0BE2: m_blend_enabled = true; break;   // GL_BLEND
        case 0x0B71: m_depth_test = true; break;      // GL_DEPTH_TEST
        case 0x0B44: m_cull_enabled = true; break;    // GL_CULL_FACE
        case 0x0C11: m_scissor_enabled = true; break; // GL_SCISSOR_TEST
        default: break;
    }
}

void RenderBridge::gl_disable(uint32_t cap) {
    switch (cap) {
        case 0x0BE2: m_blend_enabled = false; break;
        case 0x0B71: m_depth_test = false; break;
        case 0x0B44: m_cull_enabled = false; break;
        case 0x0C11: m_scissor_enabled = false; break;
        default: break;
    }
}

void RenderBridge::gl_blend_func(uint32_t sfactor, uint32_t dfactor) {
    m_blend_src_rgb = sfactor;
    m_blend_dst_rgb = dfactor;
    m_blend_src_alpha = sfactor;
    m_blend_dst_alpha = dfactor;
}

void RenderBridge::gl_blend_func_separate(uint32_t src_rgb, uint32_t dst_rgb,
                                          uint32_t src_alpha, uint32_t dst_alpha) {
    m_blend_src_rgb = src_rgb;
    m_blend_dst_rgb = dst_rgb;
    m_blend_src_alpha = src_alpha;
    m_blend_dst_alpha = dst_alpha;
}

void RenderBridge::gl_blend_equation(uint32_t mode) {
    m_blend_eq_rgb = mode;
    m_blend_eq_alpha = mode;
}

void RenderBridge::gl_blend_equation_separate(uint32_t mode_rgb, uint32_t mode_alpha) {
    m_blend_eq_rgb = mode_rgb;
    m_blend_eq_alpha = mode_alpha;
}

void RenderBridge::gl_depth_func(uint32_t func) {
    m_depth_func = func;
}

void RenderBridge::gl_depth_mask(uint8_t flag) {
    m_depth_write = (flag != 0);
}

void RenderBridge::gl_cull_face(uint32_t mode) {
    m_cull_mode = mode;
}

void RenderBridge::gl_front_face(uint32_t mode) {
    m_front_face = mode;
}

void RenderBridge::gl_color_mask(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    m_color_mask = static_cast<uint8_t>((r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0));
}

void RenderBridge::gl_vertex_attrib4f(uint32_t index, float x, float y, float z, float w) {
    if (index >= 16) return;
    m_default_attribs[index][0] = x;
    m_default_attribs[index][1] = y;
    m_default_attribs[index][2] = z;
    m_default_attribs[index][3] = w;
}

void RenderBridge::gl_uniform_iv(int32_t location, int32_t count, int32_t components, const int32_t* v) {
    if (location < 0 || count <= 0 || !v || m_bound_program == 0) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(m_bound_program);
    if (it == m_programs.end()) return;
    auto& prog = it->second;
    auto mit = prog.loc_to_member.find(base_loc);
    if (mit == prog.loc_to_member.end()) return;
    const auto& member = prog.uniform_layout[mit->second];

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
}

void RenderBridge::gl_uniform_fv(int32_t location, int32_t count, int32_t components, const float* v) {
    if (location < 0 || count <= 0 || !v || m_bound_program == 0) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(m_bound_program);
    if (it == m_programs.end()) return;
    auto& prog = it->second;
    auto mit = prog.loc_to_member.find(base_loc);
    if (mit == prog.loc_to_member.end()) return;
    const auto& member = prog.uniform_layout[mit->second];
    if (member.is_sampler) return;

    int32_t clamped_comp = std::clamp(components, 1, 4);
    for (int32_t i = 0; i < count; i++) {
        size_t off = member.byte_offset + static_cast<size_t>(elem_offset + i) * 16u;
        size_t bytes = static_cast<size_t>(clamped_comp) * sizeof(float);
        if (off + bytes <= member.byte_offset + member.byte_size &&
            off + bytes <= prog.uniform_staging.size()) {
            std::memcpy(prog.uniform_staging.data() + off,
                        v + static_cast<size_t>(i) * static_cast<size_t>(components), bytes);
        }
    }
}

void RenderBridge::gl_uniform_matrix_fv(int32_t location, int32_t count, int32_t dim,
                                        uint8_t transpose, const float* value) {
    if (location < 0 || count <= 0 || !value || m_bound_program == 0) return;
    if (dim != 2 && dim != 3 && dim != 4) return;
    int32_t base_loc = location % 1000;
    int32_t elem_offset = location / 1000;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_programs.find(m_bound_program);
    if (it == m_programs.end()) return;
    auto& prog = it->second;
    auto mit = prog.loc_to_member.find(base_loc);
    if (mit == prog.loc_to_member.end()) return;
    const auto& member = prog.uniform_layout[mit->second];
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
}

void RenderBridge::gl_vertex_attrib_pointer(uint32_t index, int32_t size, uint32_t type,
                                            uint8_t normalized, int32_t stride, uint64_t pointer) {
    if (index >= 16) return;
    auto& a = m_attribs[index];
    a.size = size;
    a.type = type;
    a.normalized = (normalized != 0);
    a.stride = stride;
    a.pointer = pointer;
    a.vbo_id = m_bound_array_buffer;
}

void RenderBridge::gl_enable_vertex_attrib_array(uint32_t index) {
    if (index < 16) m_attribs[index].enabled = true;
}

void RenderBridge::gl_disable_vertex_attrib_array(uint32_t index) {
    if (index < 16) m_attribs[index].enabled = false;
}

void RenderBridge::dispatch_d3d12_draw(uint32_t mode, int32_t first_vertex, int32_t count,
                                       bool indexed, uint32_t index_type, const void* indices) {
#if defined(LCS_ENABLE_D3D12)
    if (count <= 0 || m_bound_fbo != 0 || m_bound_program == 0 ||
        m_backend != RenderBackend::Direct3D12 || !d3d12::is_d3d12_initialized()) {
        return;
    }

    auto pit = m_programs.find(m_bound_program);
    if (pit == m_programs.end()) return;
    const auto& prog = pit->second;

    d3d12::D3D12RenderState state{};
    state.blend_enabled = m_blend_enabled;
    state.blend_src_rgb = m_blend_src_rgb;
    state.blend_dst_rgb = m_blend_dst_rgb;
    state.blend_src_alpha = m_blend_src_alpha;
    state.blend_dst_alpha = m_blend_dst_alpha;
    state.blend_eq_rgb = m_blend_eq_rgb;
    state.blend_eq_alpha = m_blend_eq_alpha;
    state.depth_test = m_depth_test;
    state.depth_write = m_depth_write;
    state.depth_func = m_depth_func;
    state.cull_enabled = m_cull_enabled;
    state.cull_mode = m_cull_mode;
    state.front_face = m_front_face;
    state.scissor_enabled = m_scissor_enabled;
    state.color_mask = m_color_mask;

    d3d12::D3D12VertexAttrDraw attrs[16]{};
    uint32_t active_mask = 0;
    for (uint32_t i = 0; i < 16; i++) {
        attrs[i].size = m_attribs[i].size;
        attrs[i].type = m_attribs[i].type;
        attrs[i].normalized = m_attribs[i].normalized;
        attrs[i].stride = m_attribs[i].stride;
        for (int c = 0; c < 4; c++) attrs[i].default_val[c] = m_default_attribs[i][c];
        if (!m_attribs[i].enabled) continue;

        active_mask |= (1u << i);
        if (m_attribs[i].vbo_id != 0) {
            auto bit = m_buffers.find(m_attribs[i].vbo_id);
            if (bit != m_buffers.end() && m_attribs[i].pointer < bit->second.data.size()) {
                attrs[i].base_ptr = bit->second.data.data() + m_attribs[i].pointer;
                attrs[i].available_bytes = bit->second.data.size() - static_cast<size_t>(m_attribs[i].pointer);
            }
        } else if (m_attribs[i].pointer != 0) {
            attrs[i].base_ptr = reinterpret_cast<const void*>(m_attribs[i].pointer);
            attrs[i].available_bytes = 16u * 1024u * 1024u;
        }
    }

    const void* idx_ptr = nullptr;
    size_t idx_bytes = 0;
    if (indexed) {
        if (m_bound_element_buffer != 0) {
            auto ebo_it = m_buffers.find(m_bound_element_buffer);
            uint64_t offset = reinterpret_cast<uint64_t>(indices);
            if (ebo_it == m_buffers.end() || offset >= ebo_it->second.data.size()) return;
            idx_ptr = ebo_it->second.data.data() + offset;
            idx_bytes = ebo_it->second.data.size() - static_cast<size_t>(offset);
        } else {
            idx_ptr = indices;
            size_t elem_sz = (index_type == 0x1405) ? 4 : ((index_type == 0x1401) ? 1 : 2);
            idx_bytes = static_cast<size_t>(count) * elem_sz;
        }
    }

    uint32_t bound_tex[8]{};
    for (uint32_t s = 0; s < 8; s++) {
        uint32_t unit = prog.sampler_units[s];
        bound_tex[s] = (unit < 16) ? m_bound_textures[unit] : 0;
    }

    d3d12::draw_d3d12(
        m_bound_program, state, mode,
        static_cast<uint32_t>(std::max(0, first_vertex)),
        static_cast<uint32_t>(count),
        indexed, index_type, idx_ptr, idx_bytes,
        prog.uniform_staging.empty() ? nullptr : prog.uniform_staging.data(),
        static_cast<uint32_t>(prog.uniform_staging.size()),
        bound_tex, active_mask, attrs);
#else
    (void)mode; (void)first_vertex; (void)count; (void)indexed; (void)index_type; (void)indices;
#endif
}

void RenderBridge::gl_draw_arrays(uint32_t mode, int32_t first, int32_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_draw_calls++;
    if (m_draw_records.size() < 200) {
        GLDrawRecord rec;
        rec.frame = m_frame_count;
        rec.program = m_bound_program;
        rec.mode = mode;
        rec.count = count;
        rec.indexed = false;
        rec.tex0 = m_bound_textures[0];
        rec.fbo = m_bound_fbo;
        for (uint32_t i = 0; i < 8; i++) {
            rec.attribs[i] = m_attribs[i];
            if (m_attribs[i].enabled) rec.active_mask |= (1u << i);
        }
        m_draw_records.push_back(rec);
    }
    dispatch_d3d12_draw(mode, first, count, false, 0, nullptr);
}

void RenderBridge::gl_draw_elements(uint32_t mode, int32_t count, uint32_t type, const void* indices) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_draw_calls++;
    if (m_draw_records.size() < 200) {
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
        for (uint32_t i = 0; i < 8; i++) {
            rec.attribs[i] = m_attribs[i];
            if (m_attribs[i].enabled) rec.active_mask |= (1u << i);
        }
        m_draw_records.push_back(rec);
    }
    dispatch_d3d12_draw(mode, 0, count, true, type, indices);
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
        if (pid <= 4 || pid >= 37) {
            auto vs_it = m_shaders.find(prog.vertex_shader);
            if (vs_it != m_shaders.end()) {
                ofs << "[VS " << prog.vertex_shader << " (preprocessed)]:\n" << vs_it->second.preprocessed << "\n";
            }
            auto fs_it = m_shaders.find(prog.fragment_shader);
            if (fs_it != m_shaders.end()) {
                ofs << "[FS " << prog.fragment_shader << " (preprocessed)]:\n" << fs_it->second.preprocessed << "\n";
            }
        }
    }

    ofs << "\n=== Textures Summary (first 30) ===\n";
    std::map<uint32_t, GLTextureObject> sorted_tex(m_textures.begin(), m_textures.end());
    int tcount = 0;
    for (const auto& [tid, tex] : sorted_tex) {
        if (tcount++ >= 30) break;
        ofs << "Tex " << tid << ": " << tex.width << "x" << tex.height
            << " fmt=0x" << std::hex << tex.format << " type=0x" << tex.type
            << " bytes=" << std::dec << tex.pixels.size() << "\n";
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

void init_render_bridge() {
    RenderBridge::instance().initialize(RenderBackend::Direct3D12);
}

void dispatch_frame() {
    RenderBridge::instance().egl_swap_buffers(0, 0);
}

} // namespace lcs::graphics
