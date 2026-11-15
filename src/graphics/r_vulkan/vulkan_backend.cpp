#if defined(LCS_ENABLE_VULKAN)
namespace lcs::graphics::vulkan {
    bool init_vulkan_instance() { return true; }
    void render_vulkan_frame() {}
    void shutdown_vulkan() {}
}
#endif
