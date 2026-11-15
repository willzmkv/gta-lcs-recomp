#if defined(LCS_ENABLE_D3D12)
#include <d3d12.h>
#include <dxgi1_6.h>

namespace lcs::graphics::d3d12 {
    bool init_d3d12_device() { return true; }
    void render_d3d12_frame() {}
    void shutdown_d3d12() {}
}
#endif
