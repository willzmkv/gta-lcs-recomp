#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace lcs::graphics {

enum class RenderBackend {
    Direct3D12,
    Vulkan
};

class RenderBridge {
public:
    static RenderBridge& instance();

    bool initialize(RenderBackend backend = RenderBackend::Direct3D12);
    void shutdown();

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

    // Surface dimensions
    void set_surface_dimensions(uint32_t width, uint32_t height);
    uint32_t get_surface_width() const { return m_width; }
    uint32_t get_surface_height() const { return m_height; }

    uint64_t get_frame_count() const { return m_frame_count; }

private:
    RenderBridge();
    ~RenderBridge();

    RenderBackend m_backend = RenderBackend::Direct3D12;
    bool m_initialized = false;
    uint32_t m_width = 1920;
    uint32_t m_height = 1080;
    uint64_t m_frame_count = 0;
    uint32_t m_last_error = 0x3000; // EGL_SUCCESS

    uint64_t m_current_context = 0;
    uint64_t m_current_draw_surface = 0;
    uint64_t m_current_read_surface = 0;
};

void init_render_bridge();
void dispatch_frame();

} // namespace lcs::graphics
