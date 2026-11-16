#include "render_bridge.h"
#include <iostream>

namespace lcs::graphics {

RenderBridge& RenderBridge::instance() {
    static RenderBridge s_instance;
    return s_instance;
}

RenderBridge::RenderBridge() = default;
RenderBridge::~RenderBridge() = default;

bool RenderBridge::initialize(RenderBackend backend) {
    m_backend = backend;
    m_initialized = true;
    std::cout << "[RenderBridge] Abstract GPU render bridge initialized with "
              << (backend == RenderBackend::Direct3D12 ? "Direct3D 12" : "Vulkan 1.4")
              << " target pipeline.\n";
    return true;
}

void RenderBridge::shutdown() {
    m_initialized = false;
}

void RenderBridge::set_surface_dimensions(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    std::cout << "[RenderBridge] Surface dimensions updated: " << width << "x" << height << "\n";
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
    return 1; // EGL_TRUE
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
    std::cout << "[EGL] eglChooseConfig -> 1 config returned\n";
    return 1; // EGL_TRUE
}

uint32_t RenderBridge::egl_get_config_attrib(uint64_t dpy, uint64_t config,
                                             int32_t attribute, int32_t* value) {
    (void)dpy;
    (void)config;
    if (!value) return 0; // EGL_FALSE

    switch (attribute) {
        case 0x3024: *value = 8; break;          // EGL_RED_SIZE
        case 0x3023: *value = 8; break;          // EGL_GREEN_SIZE
        case 0x3022: *value = 8; break;          // EGL_BLUE_SIZE
        case 0x3021: *value = 8; break;          // EGL_ALPHA_SIZE
        case 0x3025: *value = 24; break;         // EGL_DEPTH_SIZE
        case 0x3026: *value = 8; break;          // EGL_STENCIL_SIZE
        case 0x3020: *value = 32; break;         // EGL_BUFFER_SIZE
        case 0x3028: *value = 1; break;          // EGL_CONFIG_ID
        case 0x3033: *value = 0x0004; break;     // EGL_SURFACE_TYPE (EGL_WINDOW_BIT)
        case 0x3040: *value = 0x0044; break;     // EGL_RENDERABLE_TYPE (ES2 | ES3)
        case 0x3031: *value = 0; break;          // EGL_SAMPLES
        case 0x3032: *value = 0; break;          // EGL_SAMPLE_BUFFERS
        default:     *value = 0; break;
    }
    return 1; // EGL_TRUE
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
    m_current_draw_surface = draw;
    m_current_read_surface = read;
    m_current_context = ctx;
    std::cout << "[EGL] eglMakeCurrent(draw=0x" << std::hex << draw
              << ", read=0x" << read << ", ctx=0x" << ctx << std::dec << ") -> EGL_TRUE\n";
    return 1; // EGL_TRUE
}

uint32_t RenderBridge::egl_swap_buffers(uint64_t dpy, uint64_t surface) {
    (void)dpy;
    (void)surface;
    m_frame_count++;
    return 1; // EGL_TRUE
}

uint32_t RenderBridge::egl_swap_interval(uint64_t dpy, int32_t interval) {
    (void)dpy;
    (void)interval;
    return 1; // EGL_TRUE
}

uint32_t RenderBridge::egl_query_surface(uint64_t dpy, uint64_t surface,
                                         int32_t attribute, int32_t* value) {
    (void)dpy;
    (void)surface;
    if (!value) return 0;

    switch (attribute) {
        case 0x3057: *value = static_cast<int32_t>(m_width); break;  // EGL_WIDTH
        case 0x3056: *value = static_cast<int32_t>(m_height); break; // EGL_HEIGHT
        default:     *value = 0; break;
    }
    return 1; // EGL_TRUE
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
    m_last_error = 0x3000; // EGL_SUCCESS
    return err;
}

void init_render_bridge() {
    RenderBridge::instance().initialize(RenderBackend::Direct3D12);
}

void dispatch_frame() {
    RenderBridge::instance().egl_swap_buffers(0, 0);
}

} // namespace lcs::graphics
