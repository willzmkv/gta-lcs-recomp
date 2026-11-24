#if defined(LCS_ENABLE_D3D12)

#include "d3d12_backend.h"
#include "ui/imgui_overlay.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace lcs::graphics::d3d12 {

namespace {

constexpr uint32_t kFrameCount = 3;
constexpr uint32_t kMaxSamplers = 8;
constexpr size_t   kUploadBytesPerFrame = 96ULL * 1024 * 1024;
constexpr uint32_t kMaxCpuTextures = 16384;
constexpr uint32_t kPersistentSrvSlots = 196608; // 24,576 persistent 8-SRV tables across frames
constexpr uint32_t kSrvPerFrame = 65536;         // per-frame dynamic tables (PS2 PP / UI / Video)
constexpr uint32_t kMaxDsvDescriptors = 32;
constexpr size_t   kStaticVramChunkBytes = 32ULL * 1024 * 1024;
constexpr size_t   kStaticVramMaxBytes = 384ULL * 1024 * 1024;

struct FenceEventHandle {
    HANDLE handle = nullptr;
    ~FenceEventHandle() { reset(); }
    void reset(HANDLE new_handle = nullptr) {
        if (handle) CloseHandle(handle);
        handle = new_handle;
    }
};

struct D3D12ProgramEntry {
    ComPtr<ID3DBlob> vs_bytecode;
    ComPtr<ID3DBlob> ps_bytecode;
    std::vector<AttribLayout> attribs;
    std::string vs_preprocessed;
    std::string fs_preprocessed;
    std::unordered_map<std::string, int32_t> bound_attribs;
    mutable std::unordered_map<uint32_t, ComPtr<ID3DBlob>> vs_variants;
    bool valid = false;
};

struct D3D12TextureEntry {
    ComPtr<ID3D12Resource> resource;
    uint32_t cpu_slot = 0;
    uint32_t dsv_slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool is_depth = false;
    bool in_srv_state = false;
    bool in_dsv_state = false;
    bool pending_upload = false;
    bool full_upload = true;
    int32_t sub_x = 0;
    int32_t sub_y = 0;
    uint32_t sub_w = 0;
    uint32_t sub_h = 0;
    uint32_t wrap_s = 0;
    uint32_t wrap_t = 0;
    uint32_t srv_gen = 1;
    std::vector<uint8_t> rgba_staging;
};

struct PSOKey {
    uint32_t prog_id = 0;
    uint8_t blend_enabled = 0;
    uint8_t depth_test = 0;
    uint8_t depth_write = 1;
    uint8_t stencil_enable = 0;
    uint8_t cull_enabled = 0;
    uint8_t color_mask = 0xF;
    uint8_t topology_type = 3; // D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
    uint8_t depth_only_fbo = 0;
    uint8_t stencil_read_mask = 0xFF;
    uint8_t stencil_write_mask = 0xFF;
    uint8_t attr_native_mask = 0;
    std::array<uint8_t, 8> attr_dxgi_formats{};
    uint32_t blend_src_rgb = 1;
    uint32_t blend_dst_rgb = 0;
    uint32_t blend_src_alpha = 1;
    uint32_t blend_dst_alpha = 0;
    uint32_t blend_eq_rgb = 0x8006;
    uint32_t blend_eq_alpha = 0x8006;
    uint32_t depth_func = 0x0201;
    uint32_t stencil_func = 0x0207;
    uint32_t stencil_sfail = 0x1E00;
    uint32_t stencil_zfail = 0x1E00;
    uint32_t stencil_zpass = 0x1E00;
    uint32_t cull_mode = 0x0405;
    uint32_t front_face = 0x0901;

    bool operator==(const PSOKey& o) const = default;
};

struct PSOKeyHash {
    size_t operator()(const PSOKey& k) const noexcept {
        size_t h = std::hash<uint32_t>{}(k.prog_id);
        auto mix_u = [&](uint32_t v) {
            h ^= std::hash<uint32_t>{}(v) + 0x9e3779b9u + (h << 6) + (h >> 2);
        };
        mix_u((k.blend_enabled << 0) | (k.depth_test << 1) | (k.depth_write << 2) |
              (k.stencil_enable << 3) | (k.cull_enabled << 4) | (k.color_mask << 5) |
              (k.topology_type << 9) | (static_cast<uint32_t>(k.depth_only_fbo) << 11) |
              (static_cast<uint32_t>(k.stencil_read_mask) << 12) |
              (static_cast<uint32_t>(k.stencil_write_mask) << 20));
        mix_u(k.attr_native_mask);
        mix_u(static_cast<uint32_t>(k.attr_dxgi_formats[0]) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[1]) << 8) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[2]) << 16) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[3]) << 24));
        mix_u(static_cast<uint32_t>(k.attr_dxgi_formats[4]) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[5]) << 8) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[6]) << 16) |
              (static_cast<uint32_t>(k.attr_dxgi_formats[7]) << 24));
        mix_u(k.blend_src_rgb);
        mix_u(k.blend_dst_rgb);
        mix_u(k.blend_src_alpha);
        mix_u(k.blend_dst_alpha);
        mix_u(k.blend_eq_rgb);
        mix_u(k.blend_eq_alpha);
        mix_u(k.depth_func);
        mix_u(k.stencil_func);
        mix_u(k.stencil_sfail);
        mix_u(k.stencil_zfail);
        mix_u(k.stencil_zpass);
        mix_u(k.cull_mode);
        mix_u(k.front_face);
        return h;
    }
};

struct D3D12State {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> command_queue;
    ComPtr<IDXGISwapChain3> swap_chain;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> dsv_heap;
    ComPtr<ID3D12DescriptorHeap> cpu_srv_heap;
    ComPtr<ID3D12DescriptorHeap> gpu_srv_heap;
    ComPtr<ID3D12DescriptorHeap> sampler_heap;
    UINT sampler_descriptor_size = 0;
    ComPtr<ID3D12RootSignature> root_signature;

    std::array<ComPtr<ID3D12Resource>, kFrameCount> render_targets;
    ComPtr<ID3D12Resource> depth_stencil;
    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> command_allocators;
    ComPtr<ID3D12GraphicsCommandList> command_list;

    ComPtr<ID3D12Resource> upload_ring;
    uint8_t* upload_ring_mapped = nullptr;
    std::array<size_t, kFrameCount> upload_cursors{};
    std::array<uint32_t, kFrameCount> srv_cursors{};

    ComPtr<ID3D12Fence> fence;
    std::array<uint64_t, kFrameCount> fence_values{};
    uint64_t next_fence_value = 1;
    FenceEventHandle fence_event;
    HANDLE frame_latency_waitable = nullptr;

    uint32_t rtv_descriptor_size = 0;
    uint32_t dsv_descriptor_size = 0;
    uint32_t srv_descriptor_size = 0;
    uint32_t frame_index = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t bound_fbo = 0;
    uint32_t fbo_color_tex_id = 0;
    uint32_t fbo_depth_tex_id = 0;
    uint32_t fbo_width = 1024;
    uint32_t fbo_height = 1024;
    uint32_t next_dsv_slot = 1; // slot 0 reserved for main swapchain depth buffer
    float color_grade_brightness = 1.0f;
    float color_grade_contrast = 1.12f;
    float color_grade_saturation = 1.24f;
    D3D12_VIEWPORT current_viewport{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
    D3D12_RECT current_scissor{0, 0, 1280, 720};

    bool initialized = false;
    bool frame_recording = false;
    bool cleared_this_frame = false;
    bool tearing_supported = false;

    uint32_t next_cpu_srv_slot = 1; // slot 0 reserved for 1x1 white fallback
    std::vector<uint32_t> free_cpu_srv_slots;
    std::array<std::vector<ComPtr<ID3D12Resource>>, kFrameCount> retired_resources;
    std::array<std::vector<uint32_t>, kFrameCount> retired_srv_slots;
    std::unordered_map<uint32_t, D3D12ProgramEntry> programs;
    std::unordered_map<uint32_t, D3D12TextureEntry> textures;
    std::vector<uint32_t> pending_texture_uploads;
    std::unordered_map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyHash> pso_cache;
    struct FastPsoEntry {
        PSOKey key{};
        ID3D12PipelineState* pso = nullptr;
        bool valid = false;
    };
    static constexpr size_t kFastPsoCacheSize = 256;
    std::array<FastPsoEntry, kFastPsoCacheSize> fast_pso_cache{};
    struct FastVaoEntry {
        uint32_t prog_id = 0;
        uint32_t vao_id = 0;
        uint64_t vao_gen = 0;
        uint64_t default_attr_seq = 0;
        bool valid = false;
        bool can_native = false;
        uint8_t native_mask = 0;
        std::array<uint8_t, 8> native_fmts{};
        std::array<uint32_t, 8> attr_eff_strides{};
        std::array<uint32_t, 8> attr_nat_strides{};
    };
    static constexpr size_t kFastVaoCacheSize = 512;
    std::array<FastVaoEntry, kFastVaoCacheSize> fast_vao_cache{};
    struct FastStatePsoEntry {
        uint32_t prog_id = 0;
        uint64_t state_seq = 0;
        uint8_t depth_only_fbo = 0;
        uint8_t topo_type = 0;
        uint8_t attr_native_mask = 0;
        std::array<uint8_t, 8> attr_dxgi_formats{};
        ID3D12PipelineState* pso = nullptr;
        bool valid = false;
    };
    static constexpr size_t kFastStatePsoCacheSize = 512;
    std::array<FastStatePsoEntry, kFastStatePsoCacheSize> fast_state_pso_cache{};

    // Reusable scratch vectors to avoid per-draw heap allocations
    std::vector<float> scratch_verts;
    std::vector<uint32_t> scratch_indices;

    // Per-command-list redundant draw state & SRV table cache
    struct SrvTableKey {
        std::array<uint32_t, kMaxSamplers> slots{};
        std::array<uint32_t, kMaxSamplers> gens{};
        bool operator==(const SrvTableKey& o) const { return slots == o.slots && gens == o.gens; }
    };
    struct SrvTableKeyHash {
        size_t operator()(const SrvTableKey& k) const noexcept {
            uint64_t h = 14695981039346656037ULL;
            for (size_t i = 0; i < kMaxSamplers; ++i) {
                h ^= k.slots[i];
                h *= 1099511628211ULL;
                h ^= k.gens[i];
                h *= 1099511628211ULL;
            }
            return static_cast<size_t>(h);
        }
    };
    struct StaticVramChunk {
        ComPtr<ID3D12Resource> resource;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_base = 0;
        size_t used = 0;
        size_t capacity = 0;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    };
    struct RawGpuBufEntry {
        uint64_t version = 0;
        uint32_t chunk_idx = UINT32_MAX;
        D3D12_GPU_VIRTUAL_ADDRESS static_gpu_va = 0;
        size_t static_size = 0;
        uint64_t dyn_frame_seq = 0;
        D3D12_GPU_VIRTUAL_ADDRESS dyn_gpu_va = 0;
        size_t dyn_size = 0;
    };
    struct EboBoundsKey {
        uint32_t ebo_id = 0;
        uint64_t ebo_ver = 0;
        uint64_t byte_off = 0;
        uint32_t count = 0;
        uint32_t idx_type = 0;
        bool operator==(const EboBoundsKey& o) const = default;
    };
    struct EboBoundsHash {
        size_t operator()(const EboBoundsKey& k) const noexcept {
            uint64_t h = 14695981039346656037ULL;
            auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
            mix(k.ebo_id); mix(k.ebo_ver); mix(k.byte_off); mix((uint64_t(k.count) << 32) | k.idx_type);
            return static_cast<size_t>(h);
        }
    };
    struct ConvertedVbKey {
        uint32_t primary_vbo = 0;
        uint64_t primary_ver = 0;
        uint64_t primary_ptr = 0;
        uint32_t vert_count = 0;
        uint32_t active_mask = 0;
        uint64_t layout_sig = 0;
        bool operator==(const ConvertedVbKey& o) const = default;
    };
    struct ConvertedVbHash {
        size_t operator()(const ConvertedVbKey& k) const noexcept {
            uint64_t h = 14695981039346656037ULL;
            auto mix = [&](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
            mix(k.primary_vbo); mix(k.primary_ver); mix(k.primary_ptr);
            mix((uint64_t(k.vert_count) << 32) | k.active_mask); mix(k.layout_sig);
            return static_cast<size_t>(h);
        }
    };
    struct ConvertedVbEntry {
        uint32_t chunk_idx = UINT32_MAX;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0;
        uint32_t stride_bytes = 0;
        uint32_t vert_count = 0;
    };

    ID3D12PipelineState* cached_pso = nullptr;
    PSOKey last_pso_key{};
    bool last_pso_key_valid = false;
    D3D12_PRIMITIVE_TOPOLOGY cached_topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    D3D12_VIEWPORT cached_vp{};
    bool cached_vp_valid = false;
    D3D12_RECT cached_scissor{};
    bool cached_scissor_valid = false;
    uint32_t cached_stencil_ref = 0;
    bool cached_stencil_valid = false;
    uint64_t cached_srv_gpu_ptr = 0;
    uint64_t cached_sampler_gpu_ptr = 0;
    D3D12_GPU_VIRTUAL_ADDRESS cached_ib_va = 0;
    uint32_t cached_ib_size = 0;
    DXGI_FORMAT cached_ib_fmt = DXGI_FORMAT_UNKNOWN;
    D3D12_VERTEX_BUFFER_VIEW cached_vbvs[8]{};
    UINT cached_vb_count = 0;
    uint32_t cached_cb_prog_id = 0;
    uint64_t cached_cb_ver = 0;
    uint32_t cached_cb_size = 0;
    D3D12_GPU_VIRTUAL_ADDRESS cached_cb_addr = 0;
    D3D12_GPU_VIRTUAL_ADDRESS cached_bound_cb_addr = 0;
    bool cached_grade_dirty = true;
    uint64_t frame_seq = 1;
    uint32_t next_srv_gen = 1;
    uint32_t cached_prog_id = 0;
    const D3D12ProgramEntry* cached_prog_entry = nullptr;
    uint32_t persistent_srv_cursor = 0;
    bool need_persistent_srv_gc = false;
    std::unordered_map<SrvTableKey, uint32_t, SrvTableKeyHash> srv_table_cache;
    struct FastSrv2Entry {
        uint64_t key = 0;
        uint32_t sampler_table = 0;
        uint32_t dst_slot = 0;
        bool valid = false;
    };
    static constexpr size_t kSrv2CacheSize = 1024;
    std::array<FastSrv2Entry, kSrv2CacheSize> srv2_cache{};
    std::vector<StaticVramChunk> static_vram_chunks;
    size_t static_vram_total_bytes = 0;
    uint64_t static_chunk_copy_dest_mask = 0;
    bool static_chunk_over64_dirty = false;
    std::vector<RawGpuBufEntry> raw_gpu_bufs;
    std::vector<D3D12TextureEntry*> tex_ptrs;
    std::vector<D3D12ProgramEntry*> prog_ptrs;
    std::unordered_map<EboBoundsKey, uint64_t, EboBoundsHash> ebo_bounds_cache;
    std::unordered_map<ConvertedVbKey, ConvertedVbEntry, ConvertedVbHash> converted_vb_cache;

    // Video playback overlay state
    static constexpr uint32_t kVideoOverlayTexId = 0xFFFFFFFEu;
    bool video_overlay_active = false;
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    bool loading_bar_active = false;
    float loading_bar_progress = 0.0f;
    ComPtr<ID3D12PipelineState> video_pso;

    // PS2 Post-Processing Pipeline state (SLUS_214.23 Radiosity + BlurRGB Color Filter + Frame-History Trails)
    ComPtr<ID3D12Resource> ps2_scene_copy_tex;
    ComPtr<ID3D12Resource> ps2_radiosity_tex;
    ComPtr<ID3D12Resource> ps2_history_tex;
    ComPtr<ID3D12DescriptorHeap> ps2_rtv_heap;
    ComPtr<ID3D12PipelineState> ps2_radiosity_pso;
    ComPtr<ID3D12PipelineState> ps2_composite_pso;
    uint32_t ps2_scene_copy_srv_slot = 0;
    uint32_t ps2_radiosity_srv_slot = 0;
    uint32_t ps2_history_srv_slot = 0;
    uint32_t ps2_tex_width = 0;
    uint32_t ps2_tex_height = 0;
    bool ps2_history_valid = false;
    DWORD main_thread_id = 0;
    std::string screenshot_path;

    // TMP_STUTTER_AUDIT counters (reset each begin_d3d12_frame)
    bool tmp_audit_enabled = false;
    bool tmp_audit_checked = false;
    uint64_t tmp_frame_start_ns = 0;
    uint64_t tmp_wait_ns = 0;
    uint64_t tmp_pso_ns = 0;
    uint32_t tmp_pso_cnt = 0;
    uint64_t tmp_tex_ns = 0;
    uint32_t tmp_tex_cnt = 0;
    uint64_t tmp_buf_del_ns = 0;
    uint32_t tmp_buf_del_cnt = 0;
    uint64_t tmp_buf_up_ns = 0;
    uint32_t tmp_vram_fail = 0;
    uint32_t tmp_split_cnt = 0;
    uint32_t tmp_draws = 0;
    uint64_t tmp_pres_ns = 0;
    uint64_t tmp_draw_ns = 0;
    uint64_t tmp_ps2_ns = 0;
    uint64_t tmp_exec_ns = 0;
    uint64_t tmp_sum_frames = 0;
    double tmp_sum_wall = 0.0, tmp_sum_wait = 0.0, tmp_sum_pres = 0.0;
    double tmp_sum_pso = 0.0, tmp_sum_tex = 0.0, tmp_sum_buf = 0.0, tmp_sum_bdel = 0.0;
};

struct TmpScopedNsTimer {
    uint64_t* dst = nullptr;
    std::chrono::steady_clock::time_point t0{};
    TmpScopedNsTimer(bool en, uint64_t* out)
        : dst(en ? out : nullptr), t0(en ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}
    ~TmpScopedNsTimer() {
        if (dst) {
            *dst += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
    }
};

D3D12State g_d3d;

void log_hresult_failure(const char* stage, HRESULT hr) {
    std::cerr << "[D3D12] " << stage << " failed (HRESULT=0x"
              << std::hex << static_cast<uint32_t>(hr) << std::dec << ").\n";
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_srv_handle(uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_d3d.cpu_srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g_d3d.srv_descriptor_size;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_d3d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g_d3d.dsv_descriptor_size;
    return h;
}

uint32_t active_target_width() {
    return (g_d3d.bound_fbo != 0 && g_d3d.fbo_width > 0) ? g_d3d.fbo_width : g_d3d.width;
}

uint32_t active_target_height() {
    return (g_d3d.bound_fbo != 0 && g_d3d.fbo_height > 0) ? g_d3d.fbo_height : g_d3d.height;
}

void apply_current_render_targets() {
    if (g_d3d.bound_fbo != 0 && g_d3d.fbo_depth_tex_id != 0) {
        auto it = g_d3d.textures.find(g_d3d.fbo_depth_tex_id);
        if (it != g_d3d.textures.end() && it->second.is_depth && it->second.dsv_slot != 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_handle(it->second.dsv_slot);
            g_d3d.command_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
            return;
        }
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_d3d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(g_d3d.frame_index) * g_d3d.rtv_descriptor_size;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_d3d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    g_d3d.command_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
}

D3D12_CPU_DESCRIPTOR_HANDLE gpu_srv_cpu_handle(uint32_t slot) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_d3d.gpu_srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g_d3d.srv_descriptor_size;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv_gpu_handle(uint32_t slot) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = g_d3d.gpu_srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * g_d3d.srv_descriptor_size;
    return h;
}

struct UploadAllocation {
    uint8_t* cpu_ptr = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_addr = 0;
    size_t buffer_offset = 0;
};

bool alloc_upload_slice(size_t size, size_t alignment, UploadAllocation& out) {
    if (!g_d3d.upload_ring_mapped || size > kUploadBytesPerFrame) return false;
    size_t base = static_cast<size_t>(g_d3d.frame_index) * kUploadBytesPerFrame;
    size_t aligned = (g_d3d.upload_cursors[g_d3d.frame_index] + alignment - 1) & ~(alignment - 1);
    if (aligned + size + 4096u > kUploadBytesPerFrame) {
        return false;
    }
    g_d3d.upload_cursors[g_d3d.frame_index] = aligned + size;
    out.buffer_offset = base + aligned;
    out.cpu_ptr = g_d3d.upload_ring_mapped + out.buffer_offset;
    out.gpu_addr = g_d3d.upload_ring->GetGPUVirtualAddress() + out.buffer_offset;
    return true;
}

void reset_draw_state_cache(bool clear_srv_cache) {
    g_d3d.cached_pso = nullptr;
    g_d3d.last_pso_key_valid = false;
    g_d3d.cached_topo = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    g_d3d.cached_vp_valid = false;
    g_d3d.cached_scissor_valid = false;
    g_d3d.cached_stencil_valid = false;
    g_d3d.cached_srv_gpu_ptr = 0;
    g_d3d.cached_sampler_gpu_ptr = 0;
    g_d3d.cached_ib_va = 0;
    g_d3d.cached_ib_size = 0;
    g_d3d.cached_ib_fmt = DXGI_FORMAT_UNKNOWN;
    g_d3d.cached_vb_count = 0;
    g_d3d.cached_cb_prog_id = 0;
    g_d3d.cached_cb_ver = 0;
    g_d3d.cached_cb_size = 0;
    g_d3d.cached_cb_addr = 0;
    g_d3d.cached_bound_cb_addr = 0;
    g_d3d.cached_prog_id = 0;
    g_d3d.cached_prog_entry = nullptr;
    g_d3d.cached_grade_dirty = true;
    if (clear_srv_cache) {
        g_d3d.srv_table_cache.clear();
        g_d3d.srv2_cache.fill({});
        g_d3d.persistent_srv_cursor = 0;
    }
}

void split_command_list_and_wait() {
    if (!g_d3d.initialized || !g_d3d.frame_recording) return;
    ++g_d3d.tmp_split_cnt;
    std::cerr << "  [D3D12] WARN: split_command_list_and_wait! upload="
              << g_d3d.upload_cursors[g_d3d.frame_index]
              << "/" << kUploadBytesPerFrame
              << " srv=" << g_d3d.srv_cursors[g_d3d.frame_index]
              << "/" << kSrvPerFrame << "\n" << std::flush;

    HRESULT hr = g_d3d.command_list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {g_d3d.command_list.Get()};
        g_d3d.command_queue->ExecuteCommandLists(1, lists);
    }

    // Inline fence wait (avoids forward declaration ordering of wait_for_gpu)
    const uint64_t signal_val = g_d3d.next_fence_value++;
    if (SUCCEEDED(g_d3d.command_queue->Signal(g_d3d.fence.Get(), signal_val))) {
        if (g_d3d.fence->GetCompletedValue() < signal_val) {
            g_d3d.fence->SetEventOnCompletion(signal_val, g_d3d.fence_event.handle);
            WaitForSingleObject(g_d3d.fence_event.handle, INFINITE);
        }
    }
    for (uint32_t i = 0; i < kFrameCount; i++) {
        g_d3d.fence_values[i] = signal_val;
    }

    g_d3d.upload_cursors[g_d3d.frame_index] = 0;
    g_d3d.srv_cursors[g_d3d.frame_index] = 0;
    ++g_d3d.frame_seq;

    auto& alloc = g_d3d.command_allocators[g_d3d.frame_index];
    alloc->Reset();
    g_d3d.command_list->Reset(alloc.Get(), nullptr);

    apply_current_render_targets();

    ID3D12DescriptorHeap* heaps[] = {g_d3d.gpu_srv_heap.Get(), g_d3d.sampler_heap.Get()};
    g_d3d.command_list->SetDescriptorHeaps(2, heaps);
    g_d3d.command_list->SetGraphicsRootSignature(g_d3d.root_signature.Get());
    g_d3d.command_list->RSSetViewports(1, &g_d3d.current_viewport);
    g_d3d.command_list->RSSetScissorRects(1, &g_d3d.current_scissor);
    reset_draw_state_cache(false);
    g_d3d.cached_vp = g_d3d.current_viewport;
    g_d3d.cached_vp_valid = true;
    g_d3d.cached_scissor = g_d3d.current_scissor;
    g_d3d.cached_scissor_valid = true;
}

bool alloc_frame_upload(size_t size, size_t alignment, UploadAllocation& out) {
    if (alloc_upload_slice(size, alignment, out)) return true;
    if (g_d3d.frame_recording && size <= kUploadBytesPerFrame) {
        split_command_list_and_wait();
        return alloc_upload_slice(size, alignment, out);
    }
    return false;
}

std::vector<uint8_t> unpack_pixels_to_rgba8(uint32_t width, uint32_t height,
                                            uint32_t format, uint32_t type,
                                            const void* pixels) {
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    std::vector<uint8_t> rgba(count * 4, 255);
    if (!pixels || count == 0) return rgba;

    const uint8_t* src8 = static_cast<const uint8_t*>(pixels);
    const uint16_t* src16 = static_cast<const uint16_t*>(pixels);

    if (type == 0x8033) {
        // GL_UNSIGNED_SHORT_4_4_4_4
        for (size_t i = 0; i < count; i++) {
            uint16_t p = src16[i];
            rgba[i * 4 + 0] = static_cast<uint8_t>(((p >> 12) & 0xF) * 17);
            rgba[i * 4 + 1] = static_cast<uint8_t>(((p >> 8) & 0xF) * 17);
            rgba[i * 4 + 2] = static_cast<uint8_t>(((p >> 4) & 0xF) * 17);
            rgba[i * 4 + 3] = static_cast<uint8_t>((p & 0xF) * 17);
        }
    } else if (type == 0x8034) {
        // GL_UNSIGNED_SHORT_5_5_5_1
        for (size_t i = 0; i < count; i++) {
            uint16_t p = src16[i];
            rgba[i * 4 + 0] = static_cast<uint8_t>((((p >> 11) & 0x1F) * 255 + 15) / 31);
            rgba[i * 4 + 1] = static_cast<uint8_t>((((p >> 6) & 0x1F) * 255 + 15) / 31);
            rgba[i * 4 + 2] = static_cast<uint8_t>((((p >> 1) & 0x1F) * 255 + 15) / 31);
            rgba[i * 4 + 3] = (p & 1) ? 255 : 0;
        }
    } else if (type == 0x8363) {
        // GL_UNSIGNED_SHORT_5_6_5
        for (size_t i = 0; i < count; i++) {
            uint16_t p = src16[i];
            rgba[i * 4 + 0] = static_cast<uint8_t>((((p >> 11) & 0x1F) * 255 + 15) / 31);
            rgba[i * 4 + 1] = static_cast<uint8_t>((((p >> 5) & 0x3F) * 255 + 31) / 63);
            rgba[i * 4 + 2] = static_cast<uint8_t>(((p & 0x1F) * 255 + 15) / 31);
            rgba[i * 4 + 3] = 255;
        }
    } else if (format == 0x1908) {
        // GL_RGBA + GL_UNSIGNED_BYTE
        std::memcpy(rgba.data(), src8, count * 4);
    } else if (format == 0x80E1) {
        // GL_BGRA_EXT (Android/mobile BGRA buffers)
        for (size_t i = 0; i < count; i++) {
            rgba[i * 4 + 0] = src8[i * 4 + 2];
            rgba[i * 4 + 1] = src8[i * 4 + 1];
            rgba[i * 4 + 2] = src8[i * 4 + 0];
            rgba[i * 4 + 3] = src8[i * 4 + 3];
        }
    } else if (format == 0x1907) {
        // GL_RGB
        for (size_t i = 0; i < count; i++) {
            rgba[i * 4 + 0] = src8[i * 3 + 0];
            rgba[i * 4 + 1] = src8[i * 3 + 1];
            rgba[i * 4 + 2] = src8[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
    } else if (format == 0x190A) {
        // GL_LUMINANCE_ALPHA
        for (size_t i = 0; i < count; i++) {
            uint8_t l = src8[i * 2 + 0];
            uint8_t a = src8[i * 2 + 1];
            rgba[i * 4 + 0] = l;
            rgba[i * 4 + 1] = l;
            rgba[i * 4 + 2] = l;
            rgba[i * 4 + 3] = a;
        }
    } else if (format == 0x1909) {
        // GL_LUMINANCE
        for (size_t i = 0; i < count; i++) {
            uint8_t l = src8[i];
            rgba[i * 4 + 0] = l;
            rgba[i * 4 + 1] = l;
            rgba[i * 4 + 2] = l;
            rgba[i * 4 + 3] = 255;
        }
    } else if (format == 0x1906) {
        // GL_ALPHA (expand to white RGBA so alpha masks tint geometry instead of blacking it out)
        for (size_t i = 0; i < count; i++) {
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = src8[i];
        }
    }
    return rgba;
}

bool record_texture_upload(uint32_t tex_id, D3D12TextureEntry& tex) {
    if (!tex.pending_upload || !tex.resource || !g_d3d.frame_recording) return !tex.pending_upload;
    uint32_t w = tex.full_upload ? tex.width : tex.sub_w;
    uint32_t h = tex.full_upload ? tex.height : tex.sub_h;
    if (w == 0 || h == 0 || tex.rgba_staging.empty()) {
        tex.pending_upload = false;
        std::vector<uint8_t>().swap(tex.rgba_staging);
        return true;
    }

    uint32_t src_row_bytes = w * 4;
    uint32_t dst_row_pitch = (src_row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                             ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    size_t total_upload = static_cast<size_t>(dst_row_pitch) * h;

    UploadAllocation alloc;
    if (!alloc_frame_upload(total_upload, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, alloc)) {
        return false;
    }

    for (uint32_t y = 0; y < h; y++) {
        std::memcpy(alloc.cpu_ptr + static_cast<size_t>(y) * dst_row_pitch,
                    tex.rgba_staging.data() + static_cast<size_t>(y) * src_row_bytes,
                    src_row_bytes);
    }

    if (tex.in_srv_state) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex.resource.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_d3d.command_list->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = tex.resource.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = g_d3d.upload_ring.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = alloc.buffer_offset;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = w;
    src_loc.PlacedFootprint.Footprint.Height = h;
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = dst_row_pitch;

    UINT dst_x = tex.full_upload ? 0 : static_cast<UINT>(std::max(0, tex.sub_x));
    UINT dst_y = tex.full_upload ? 0 : static_cast<UINT>(std::max(0, tex.sub_y));
    g_d3d.command_list->CopyTextureRegion(&dst_loc, dst_x, dst_y, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER b_done{};
    b_done.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b_done.Transition.pResource = tex.resource.Get();
    b_done.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b_done.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b_done.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_d3d.command_list->ResourceBarrier(1, &b_done);

    tex.in_srv_state = true;
    tex.pending_upload = false;
    std::vector<uint8_t>().swap(tex.rgba_staging);
    (void)tex_id;
    return true;
}

void flush_pending_texture_uploads() {
    if (g_d3d.pending_texture_uploads.empty() || !g_d3d.frame_recording) return;
    const auto tp0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::vector<uint32_t> remaining;
    for (uint32_t tid : g_d3d.pending_texture_uploads) {
        auto it = g_d3d.textures.find(tid);
        if (it != g_d3d.textures.end()) {
            if (!record_texture_upload(tid, it->second)) {
                remaining.push_back(tid);
            } else {
                ++g_d3d.tmp_tex_cnt;
            }
        }
    }
    g_d3d.pending_texture_uploads.swap(remaining);
    if (g_d3d.tmp_audit_enabled) {
        g_d3d.tmp_tex_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - tp0).count());
    }
}

// Transition chunk state between COPY_DEST and GENERIC_READ only on state change
void transition_static_chunk(D3D12State::StaticVramChunk& ch, D3D12_RESOURCE_STATES target) {
    if (ch.state == target) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = ch.resource.Get();
    b.Transition.StateBefore = ch.state;
    b.Transition.StateAfter = target;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_d3d.command_list->ResourceBarrier(1, &b);
    ch.state = target;
}

bool upload_to_static_vram_pool(const void* data, size_t bytes, size_t alignment,
                                uint32_t& out_chunk_idx, D3D12_GPU_VIRTUAL_ADDRESS& out_gpu_va) {
    if (!data || bytes == 0 || !g_d3d.frame_recording) return false;
    const size_t align = std::max<size_t>(16u, alignment);
    if (bytes > kStaticVramChunkBytes || bytes > kUploadBytesPerFrame) return false;

    uint32_t chosen = UINT32_MAX;
    size_t aligned_off = 0;
    for (uint32_t ci = 0; ci < static_cast<uint32_t>(g_d3d.static_vram_chunks.size()); ++ci) {
        auto& ch = g_d3d.static_vram_chunks[ci];
        const size_t off = (ch.used + align - 1) & ~(align - 1);
        if (off + bytes <= ch.capacity) {
            chosen = ci;
            aligned_off = off;
            break;
        }
    }
    if (chosen == UINT32_MAX) {
        if (g_d3d.static_vram_total_bytes + kStaticVramChunkBytes > kStaticVramMaxBytes) {
            ++g_d3d.tmp_vram_fail;
            return false;
        }
        D3D12State::StaticVramChunk ch;
        ch.capacity = kStaticVramChunkBytes;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = ch.capacity;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_d3d.device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&ch.resource)))) {
            return false;
        }
        ch.gpu_base = ch.resource->GetGPUVirtualAddress();
        ch.state = D3D12_RESOURCE_STATE_COPY_DEST;
        g_d3d.static_vram_total_bytes += ch.capacity;
        chosen = static_cast<uint32_t>(g_d3d.static_vram_chunks.size());
        g_d3d.static_vram_chunks.push_back(std::move(ch));
        aligned_off = 0;
    }

    UploadAllocation stage{};
    if (!alloc_frame_upload(bytes, 16, stage)) return false;
    std::memcpy(stage.cpu_ptr, data, bytes);

    auto& ch = g_d3d.static_vram_chunks[chosen];
    transition_static_chunk(ch, D3D12_RESOURCE_STATE_COPY_DEST);
    g_d3d.command_list->CopyBufferRegion(
        ch.resource.Get(), aligned_off,
        g_d3d.upload_ring.Get(), stage.buffer_offset, bytes);
    ch.used = aligned_off + bytes;
    if (chosen < 64u) g_d3d.static_chunk_copy_dest_mask |= (1ULL << chosen);
    else g_d3d.static_chunk_over64_dirty = true;
    out_chunk_idx = chosen;
    out_gpu_va = ch.gpu_base + aligned_off;
    return true;
}

DXGI_FORMAT map_gl_attrib_to_dxgi(uint32_t size, uint32_t type, uint8_t normalized) {
    switch (type) {
        case 0x1406: // GL_FLOAT
            if (size == 1) return DXGI_FORMAT_R32_FLOAT;
            if (size == 2) return DXGI_FORMAT_R32G32_FLOAT;
            if (size == 3) return DXGI_FORMAT_R32G32B32_FLOAT;
            if (size == 4) return DXGI_FORMAT_R32G32B32A32_FLOAT;
            return DXGI_FORMAT_UNKNOWN;
        case 0x140B: // GL_HALF_FLOAT
        case 0x8D61: // GL_HALF_FLOAT_OES
            if (size == 2) return DXGI_FORMAT_R16G16_FLOAT;
            if (size == 4) return DXGI_FORMAT_R16G16B16A16_FLOAT;
            return DXGI_FORMAT_UNKNOWN;
        case 0x1401: // GL_UNSIGNED_BYTE
            if (normalized) return (size == 4) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_UNKNOWN;
            if (size == 4) return DXGI_FORMAT_R8G8B8A8_UINT;
            if (size == 2) return DXGI_FORMAT_R8G8_UINT;
            if (size == 1) return DXGI_FORMAT_R8_UINT;
            return DXGI_FORMAT_UNKNOWN;
        case 0x1400: // GL_BYTE
            if (normalized) return (size == 4) ? DXGI_FORMAT_R8G8B8A8_SNORM : DXGI_FORMAT_UNKNOWN;
            if (size == 4) return DXGI_FORMAT_R8G8B8A8_SINT;
            if (size == 2) return DXGI_FORMAT_R8G8_SINT;
            if (size == 1) return DXGI_FORMAT_R8_SINT;
            return DXGI_FORMAT_UNKNOWN;
        case 0x1402: // GL_SHORT
            if (normalized) {
                if (size == 2) return DXGI_FORMAT_R16G16_SNORM;
                if (size == 4) return DXGI_FORMAT_R16G16B16A16_SNORM;
                return DXGI_FORMAT_UNKNOWN;
            }
            if (size == 1) return DXGI_FORMAT_R16_SINT;
            if (size == 2) return DXGI_FORMAT_R16G16_SINT;
            if (size == 4) return DXGI_FORMAT_R16G16B16A16_SINT;
            return DXGI_FORMAT_UNKNOWN;
        case 0x1403: // GL_UNSIGNED_SHORT
            if (normalized) {
                if (size == 2) return DXGI_FORMAT_R16G16_UNORM;
                if (size == 4) return DXGI_FORMAT_R16G16B16A16_UNORM;
                return DXGI_FORMAT_UNKNOWN;
            }
            if (size == 1) return DXGI_FORMAT_R16_UINT;
            if (size == 2) return DXGI_FORMAT_R16G16_UINT;
            if (size == 4) return DXGI_FORMAT_R16G16B16A16_UINT;
            return DXGI_FORMAT_UNKNOWN;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t gl_attrib_natural_stride(uint32_t size, uint32_t type) {
    uint32_t bpc = 4;
    switch (type) {
        case 0x1400: case 0x1401: bpc = 1; break;
        case 0x1402: case 0x1403: case 0x140B: case 0x8D61: bpc = 2; break;
        default: bpc = 4; break;
    }
    return std::max(1u, std::min(size, 4u)) * bpc;
}

bool create_render_targets() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = g_d3d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < kFrameCount; i++) {
        HRESULT hr = g_d3d.swap_chain->GetBuffer(i, IID_PPV_ARGS(&g_d3d.render_targets[i]));
        if (FAILED(hr)) {
            log_hresult_failure("IDXGISwapChain3::GetBuffer", hr);
            return false;
        }
        g_d3d.device->CreateRenderTargetView(g_d3d.render_targets[i].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += g_d3d.rtv_descriptor_size;
    }
    return true;
}

bool create_depth_stencil(uint32_t width, uint32_t height) {
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = std::max(1u, width);
    desc.Height = std::max(1u, height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_val{};
    clear_val.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear_val.DepthStencil.Depth = 1.0f;
    clear_val.DepthStencil.Stencil = 0;

    HRESULT hr = g_d3d.device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear_val,
        IID_PPV_ARGS(&g_d3d.depth_stencil)
    );
    if (FAILED(hr)) {
        log_hresult_failure("CreateCommittedResource (DepthStencil)", hr);
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_d3d.device->CreateDepthStencilView(
        g_d3d.depth_stencil.Get(),
        &dsv_desc,
        g_d3d.dsv_heap->GetCPUDescriptorHandleForHeapStart()
    );
    return true;
}

bool create_root_signature() {
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = kMaxSamplers;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE sampler_range{};
    sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampler_range.NumDescriptors = kMaxSamplers; // 8 samplers (s0..s7) per table
    sampler_range.BaseShaderRegister = 0;
    sampler_range.RegisterSpace = 0;
    sampler_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &sampler_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister = 1;
    params[3].Constants.RegisterSpace = 0;
    params[3].Constants.Num32BitValues = 4;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 0;
    desc.pStaticSamplers = nullptr;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> err_blob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (FAILED(hr)) {
        if (err_blob) {
            std::cerr << "[D3D12] RootSig error: "
                      << static_cast<const char*>(err_blob->GetBufferPointer()) << "\n";
        }
        return false;
    }

    hr = g_d3d.device->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        IID_PPV_ARGS(&g_d3d.root_signature));
    if (FAILED(hr)) {
        log_hresult_failure("CreateRootSignature", hr);
        return false;
    }

    // Initialize sampler descriptor heap with 9 tables of 8 samplers (3 AddressU x 3 AddressV: WRAP, CLAMP, MIRROR)
    static constexpr D3D12_TEXTURE_ADDRESS_MODE kAddrModes[3] = {
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
    };
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu = g_d3d.sampler_heap->GetCPUDescriptorHandleForHeapStart();
    uint32_t initialized_samplers = 0;
    for (uint32_t u_mode = 0; u_mode < 3; u_mode++) {
        for (uint32_t v_mode = 0; v_mode < 3; v_mode++) {
            for (uint32_t slot = 0; slot < 8; slot++) {
                D3D12_SAMPLER_DESC samp_desc{};
                samp_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                samp_desc.MaxAnisotropy = 1;
                samp_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                samp_desc.MaxLOD = D3D12_FLOAT32_MAX;
                samp_desc.AddressU = kAddrModes[u_mode];
                samp_desc.AddressV = kAddrModes[v_mode];
                samp_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

                g_d3d.device->CreateSampler(&samp_desc, sampler_cpu);
                sampler_cpu.ptr += g_d3d.sampler_descriptor_size;
                ++initialized_samplers;
            }
        }
    }
    for (; initialized_samplers < 128; ++initialized_samplers) {
        D3D12_SAMPLER_DESC samp_desc{};
        samp_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp_desc.MaxAnisotropy = 1;
        samp_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samp_desc.MaxLOD = D3D12_FLOAT32_MAX;
        samp_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        g_d3d.device->CreateSampler(&samp_desc, sampler_cpu);
        sampler_cpu.ptr += g_d3d.sampler_descriptor_size;
    }

    return true;
}

void wait_for_gpu() {
    if (!g_d3d.command_queue || !g_d3d.fence || !g_d3d.fence_event.handle) return;

    const uint64_t signal_val = g_d3d.next_fence_value++;
    if (SUCCEEDED(g_d3d.command_queue->Signal(g_d3d.fence.Get(), signal_val))) {
        if (g_d3d.fence->GetCompletedValue() < signal_val) {
            g_d3d.fence->SetEventOnCompletion(signal_val, g_d3d.fence_event.handle);
            WaitForSingleObject(g_d3d.fence_event.handle, INFINITE);
        }
    }
    for (uint32_t i = 0; i < kFrameCount; i++) {
        g_d3d.fence_values[i] = signal_val;
    }
}

void wait_for_frame(uint32_t idx) {
    const uint64_t target = g_d3d.fence_values[idx];
    if (g_d3d.fence->GetCompletedValue() < target) {
        g_d3d.fence->SetEventOnCompletion(target, g_d3d.fence_event.handle);
        WaitForSingleObject(g_d3d.fence_event.handle, INFINITE);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE current_rtv() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_d3d.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(g_d3d.frame_index) * g_d3d.rtv_descriptor_size;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE current_dsv() {
    return g_d3d.dsv_heap->GetCPUDescriptorHandleForHeapStart();
}

std::string narrow_adapter_name(const WCHAR* wide_name) {
    if (!wide_name) return "Unknown GPU";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide_name, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "Unknown GPU";
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide_name, -1, out.data(), len, nullptr, nullptr);
    return out;
}

D3D12_BLEND to_d3d12_blend(uint32_t gl_factor, bool is_alpha) {
    switch (gl_factor) {
        case 0:      return D3D12_BLEND_ZERO;
        case 1:      return D3D12_BLEND_ONE;
        case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x0302: return D3D12_BLEND_SRC_ALPHA;
        case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x0304: return D3D12_BLEND_DEST_ALPHA;
        case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
        case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
        default:     return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP to_d3d12_blend_op(uint32_t gl_eq) {
    switch (gl_eq) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8006:
        default:     return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC to_d3d12_cmp(uint32_t gl_func) {
    switch (gl_func) {
        case 0x0200: return D3D12_COMPARISON_FUNC_NEVER;
        case 0x0201: return D3D12_COMPARISON_FUNC_LESS;
        case 0x0202: return D3D12_COMPARISON_FUNC_EQUAL;
        case 0x0203: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 0x0204: return D3D12_COMPARISON_FUNC_GREATER;
        case 0x0205: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 0x0206: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 0x0207: return D3D12_COMPARISON_FUNC_ALWAYS;
        default:     return D3D12_COMPARISON_FUNC_LESS;
    }
}

D3D12_STENCIL_OP to_d3d12_stencil_op(uint32_t gl_op) {
    switch (gl_op) {
        case 0x0000: return D3D12_STENCIL_OP_ZERO;
        case 0x1E00: return D3D12_STENCIL_OP_KEEP;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default:     return D3D12_STENCIL_OP_KEEP;
    }
}

static inline bool is_dxgi_sint_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8_SINT || fmt == DXGI_FORMAT_R8G8_SINT || fmt == DXGI_FORMAT_R8G8B8A8_SINT ||
           fmt == DXGI_FORMAT_R16_SINT || fmt == DXGI_FORMAT_R16G16_SINT || fmt == DXGI_FORMAT_R16G16B16A16_SINT;
}

static inline bool is_dxgi_uint_format(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_R8_UINT || fmt == DXGI_FORMAT_R8G8_UINT || fmt == DXGI_FORMAT_R8G8B8A8_UINT ||
           fmt == DXGI_FORMAT_R16_UINT || fmt == DXGI_FORMAT_R16G16_UINT || fmt == DXGI_FORMAT_R16G16B16A16_UINT;
}

ID3D12PipelineState* get_or_create_pso(const PSOKey& key, const D3D12ProgramEntry& prog) {
    const size_t pso_slot = PSOKeyHash{}(key) & (D3D12State::kFastPsoCacheSize - 1);
    auto& fe = g_d3d.fast_pso_cache[pso_slot];
    if (fe.valid && fe.key == key) {
        return fe.pso;
    }
    auto it = g_d3d.pso_cache.find(key);
    if (it != g_d3d.pso_cache.end()) {
        ID3D12PipelineState* raw = it->second.Get();
        fe = {key, raw, true};
        return raw;
    }
    if (!prog.valid) return nullptr;
    const auto pso_t0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    uint32_t sint_mask = 0;
    uint32_t uint_mask = 0;
    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elems;
    for (size_t i = 0; i < prog.attribs.size(); i++) {
        D3D12_INPUT_ELEMENT_DESC elem{};
        elem.SemanticName = "ATTR";
        elem.SemanticIndex = static_cast<UINT>(prog.attribs[i].location);
        if (i < 8 && (key.attr_native_mask & (1u << i)) != 0 && key.attr_dxgi_formats[i] != 0) {
            const DXGI_FORMAT fmt = static_cast<DXGI_FORMAT>(key.attr_dxgi_formats[i]);
            elem.Format = fmt;
            elem.InputSlot = static_cast<UINT>(i);
            elem.AlignedByteOffset = 0;
            const int32_t loc = prog.attribs[i].location;
            if (loc >= 0 && loc < 16) {
                if (is_dxgi_sint_format(fmt)) sint_mask |= (1u << static_cast<uint32_t>(loc));
                else if (is_dxgi_uint_format(fmt)) uint_mask |= (1u << static_cast<uint32_t>(loc));
            }
        } else {
            elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            elem.InputSlot = 0;
            elem.AlignedByteOffset = static_cast<UINT>(i * 16);
        }
        elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        input_elems.push_back(elem);
    }

    ID3DBlob* vs_blob = prog.vs_bytecode.Get();
    const uint32_t mask_key = (sint_mask << 16) | (uint_mask & 0xFFFFu);
    if (mask_key != 0) {
        auto vit = prog.vs_variants.find(mask_key);
        if (vit != prog.vs_variants.end() && vit->second) {
            vs_blob = vit->second.Get();
        } else if (!prog.vs_preprocessed.empty()) {
            TranslatedProgramHLSL var_hlsl = translate_program_to_hlsl(
                prog.vs_preprocessed, prog.fs_preprocessed, prog.bound_attribs, sint_mask, uint_mask);
            ComPtr<ID3DBlob> new_vs;
            ComPtr<ID3DBlob> err_blob;
            const UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL2;
            HRESULT hr_vs = D3DCompile(
                var_hlsl.vs_hlsl.data(), var_hlsl.vs_hlsl.size(),
                nullptr, nullptr, nullptr, "main", "vs_5_0",
                compile_flags, 0, &new_vs, &err_blob);
            if (FAILED(hr_vs) || !new_vs) {
                std::cerr << "[D3D12] VS variant compile failed for program " << key.prog_id
                          << " sint=0x" << std::hex << sint_mask << " uint=0x" << uint_mask << std::dec << ": "
                          << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
                return nullptr;
            }
            vs_blob = new_vs.Get();
            prog.vs_variants.emplace(mask_key, std::move(new_vs));
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = g_d3d.root_signature.Get();
    pso_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
    pso_desc.PS = {prog.ps_bytecode->GetBufferPointer(), prog.ps_bytecode->GetBufferSize()};
    pso_desc.InputLayout = {input_elems.data(), static_cast<UINT>(input_elems.size())};

    pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pso_desc.BlendState.IndependentBlendEnable = FALSE;
    auto& rt0 = pso_desc.BlendState.RenderTarget[0];
    rt0.BlendEnable = key.blend_enabled ? TRUE : FALSE;
    rt0.SrcBlend = to_d3d12_blend(key.blend_src_rgb, false);
    rt0.DestBlend = to_d3d12_blend(key.blend_dst_rgb, false);
    rt0.BlendOp = to_d3d12_blend_op(key.blend_eq_rgb);
    rt0.SrcBlendAlpha = to_d3d12_blend(key.blend_src_alpha, true);
    rt0.DestBlendAlpha = to_d3d12_blend(key.blend_dst_alpha, true);
    rt0.BlendOpAlpha = to_d3d12_blend_op(key.blend_eq_alpha);
    rt0.RenderTargetWriteMask = (key.color_mask & 0xF);

    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    if (!key.cull_enabled || key.cull_mode == 0x0408) {
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    } else if (key.cull_mode == 0x0404) {
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    } else {
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    }
    pso_desc.RasterizerState.FrontCounterClockwise = (key.front_face == 0x0901) ? TRUE : FALSE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;

    pso_desc.DepthStencilState.DepthEnable = key.depth_test ? TRUE : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = key.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pso_desc.DepthStencilState.DepthFunc = to_d3d12_cmp(key.depth_func);
    pso_desc.DepthStencilState.StencilEnable = key.stencil_enable ? TRUE : FALSE;
    pso_desc.DepthStencilState.StencilReadMask = key.stencil_read_mask;
    pso_desc.DepthStencilState.StencilWriteMask = key.stencil_write_mask;
    pso_desc.DepthStencilState.FrontFace.StencilFailOp = to_d3d12_stencil_op(key.stencil_sfail);
    pso_desc.DepthStencilState.FrontFace.StencilDepthFailOp = to_d3d12_stencil_op(key.stencil_zfail);
    pso_desc.DepthStencilState.FrontFace.StencilPassOp = to_d3d12_stencil_op(key.stencil_zpass);
    pso_desc.DepthStencilState.FrontFace.StencilFunc = to_d3d12_cmp(key.stencil_func);
    pso_desc.DepthStencilState.BackFace = pso_desc.DepthStencilState.FrontFace;

    pso_desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(key.topology_type);
    if (key.depth_only_fbo) {
        pso_desc.NumRenderTargets = 0;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso_desc.RasterizerState.DepthBias = 150;
        pso_desc.RasterizerState.SlopeScaledDepthBias = 1.5f;
        pso_desc.RasterizerState.DepthBiasClamp = 0.01f;
        rt0.RenderTargetWriteMask = 0;
    } else {
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
    pso_desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = g_d3d.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        log_hresult_failure("CreateGraphicsPipelineState", hr);
        return nullptr;
    }
    ID3D12PipelineState* raw = pso.Get();
    g_d3d.pso_cache.emplace(key, std::move(pso));
    g_d3d.fast_pso_cache[PSOKeyHash{}(key) & (D3D12State::kFastPsoCacheSize - 1)] = {key, raw, true};
    if (g_d3d.tmp_audit_enabled) {
        ++g_d3d.tmp_pso_cnt;
        g_d3d.tmp_pso_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pso_t0).count());
    }
    return raw;
}

void decode_vertex_attrib(const D3D12VertexAttrDraw& attr, uint32_t vertex_index, float out[4]) {
    out[0] = attr.default_val[0];
    out[1] = attr.default_val[1];
    out[2] = attr.default_val[2];
    out[3] = attr.default_val[3];

    if (!attr.base_ptr) return;

    int32_t comps = std::clamp(attr.size, 1, 4);
    size_t comp_bytes = 4;
    switch (attr.type) {
        case 0x1400: // GL_BYTE
        case 0x1401: // GL_UNSIGNED_BYTE
            comp_bytes = 1;
            break;
        case 0x1402: // GL_SHORT
        case 0x1403: // GL_UNSIGNED_SHORT
            comp_bytes = 2;
            break;
        case 0x1406: // GL_FLOAT
        default:
            comp_bytes = 4;
            break;
    }

    size_t eff_stride = (attr.stride > 0) ? static_cast<size_t>(attr.stride) : (comp_bytes * static_cast<size_t>(comps));
    size_t byte_off = static_cast<size_t>(vertex_index) * eff_stride;
    if (attr.available_bytes > 0 && byte_off + comp_bytes * static_cast<size_t>(comps) > attr.available_bytes) {
        return;
    }

    const uint8_t* p = static_cast<const uint8_t*>(attr.base_ptr) + byte_off;
    for (int32_t c = 0; c < comps; c++) {
        float val = 0.0f;
        switch (attr.type) {
            case 0x1400: { // GL_BYTE
                int8_t v = static_cast<int8_t>(p[c]);
                val = attr.normalized ? std::max(-1.0f, static_cast<float>(v) / 127.0f) : static_cast<float>(v);
                break;
            }
            case 0x1401: { // GL_UNSIGNED_BYTE
                uint8_t v = p[c];
                val = attr.normalized ? (static_cast<float>(v) / 255.0f) : static_cast<float>(v);
                break;
            }
            case 0x1402: { // GL_SHORT
                int16_t v = 0;
                std::memcpy(&v, p + c * 2, 2);
                val = attr.normalized ? std::max(-1.0f, static_cast<float>(v) / 32767.0f) : static_cast<float>(v);
                break;
            }
            case 0x1403: { // GL_UNSIGNED_SHORT
                uint16_t v = 0;
                std::memcpy(&v, p + c * 2, 2);
                val = attr.normalized ? (static_cast<float>(v) / 65535.0f) : static_cast<float>(v);
                break;
            }
            case 0x1406: // GL_FLOAT
            default: {
                std::memcpy(&val, p + c * 4, 4);
                break;
            }
        }
        out[c] = val;
    }
}

inline void fill_attrib_const(float* dst, uint32_t vert_count, size_t v_stride_floats, const float def[4]) {
    const float d0 = def[0], d1 = def[1], d2 = def[2], d3 = def[3];
    for (uint32_t v = 0; v < vert_count; ++v, dst += v_stride_floats) {
        dst[0] = d0;
        dst[1] = d1;
        dst[2] = d2;
        dst[3] = d3;
    }
}

void unpack_attrib_column(
    const D3D12VertexAttrDraw& attr,
    uint32_t vert_start,
    uint32_t vert_count,
    float* dst_base,
    size_t v_stride_floats) {
    if (vert_count == 0) return;
    if (!attr.base_ptr) {
        fill_attrib_const(dst_base, vert_count, v_stride_floats, attr.default_val);
        return;
    }

    const int32_t comps = std::clamp(attr.size, 1, 4);
    size_t comp_bytes = 4;
    switch (attr.type) {
        case 0x1400: // GL_BYTE
        case 0x1401: // GL_UNSIGNED_BYTE
            comp_bytes = 1;
            break;
        case 0x1402: // GL_SHORT
        case 0x1403: // GL_UNSIGNED_SHORT
            comp_bytes = 2;
            break;
        case 0x1406: // GL_FLOAT
        default:
            comp_bytes = 4;
            break;
    }

    const size_t eff_stride = (attr.stride > 0)
        ? static_cast<size_t>(attr.stride)
        : (comp_bytes * static_cast<size_t>(comps));
    const size_t last_end =
        (static_cast<size_t>(vert_start) + static_cast<size_t>(vert_count) - 1) * eff_stride +
        comp_bytes * static_cast<size_t>(comps);
    if (attr.available_bytes > 0 && last_end > attr.available_bytes) {
        for (uint32_t v = 0; v < vert_count; ++v) {
            decode_vertex_attrib(attr, vert_start + v, dst_base + static_cast<size_t>(v) * v_stride_floats);
        }
        return;
    }

    const uint8_t* p = static_cast<const uint8_t*>(attr.base_ptr) + static_cast<size_t>(vert_start) * eff_stride;
    float* dst = dst_base;
    const float d1 = attr.default_val[1];
    const float d2 = attr.default_val[2];
    const float d3 = attr.default_val[3];

    switch (attr.type) {
        case 0x1406: { // GL_FLOAT
            if (comps == 4) {
                for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                    std::memcpy(dst, p, 16);
                }
            } else if (comps == 3) {
                for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                    std::memcpy(dst, p, 12);
                    dst[3] = d3;
                }
            } else if (comps == 2) {
                for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                    std::memcpy(dst, p, 8);
                    dst[2] = d2;
                    dst[3] = d3;
                }
            } else {
                for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                    std::memcpy(dst, p, 4);
                    dst[1] = d1;
                    dst[2] = d2;
                    dst[3] = d3;
                }
            }
            break;
        }
        case 0x1401: { // GL_UNSIGNED_BYTE
            static const auto kU8Norm = []() {
                std::array<float, 256> lut{};
                for (int i = 0; i < 256; ++i) lut[i] = static_cast<float>(i) / 255.0f;
                return lut;
            }();
            if (attr.normalized) {
                if (comps == 4) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        dst[0] = kU8Norm[p[0]];
                        dst[1] = kU8Norm[p[1]];
                        dst[2] = kU8Norm[p[2]];
                        dst[3] = kU8Norm[p[3]];
                    }
                } else if (comps == 3) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        dst[0] = kU8Norm[p[0]];
                        dst[1] = kU8Norm[p[1]];
                        dst[2] = kU8Norm[p[2]];
                        dst[3] = d3;
                    }
                } else if (comps == 2) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        dst[0] = kU8Norm[p[0]];
                        dst[1] = kU8Norm[p[1]];
                        dst[2] = d2;
                        dst[3] = d3;
                    }
                } else {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        dst[0] = kU8Norm[p[0]];
                        dst[1] = d1;
                        dst[2] = d2;
                        dst[3] = d3;
                    }
                }
            } else {
                for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                    dst[0] = static_cast<float>(p[0]);
                    dst[1] = (comps >= 2) ? static_cast<float>(p[1]) : d1;
                    dst[2] = (comps >= 3) ? static_cast<float>(p[2]) : d2;
                    dst[3] = (comps >= 4) ? static_cast<float>(p[3]) : d3;
                }
            }
            break;
        }
        case 0x1402: { // GL_SHORT
            if (attr.normalized) {
                constexpr float kInv32767 = 1.0f / 32767.0f;
                if (comps == 3) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = std::max(-1.0f, static_cast<float>(sp[0]) * kInv32767);
                        dst[1] = std::max(-1.0f, static_cast<float>(sp[1]) * kInv32767);
                        dst[2] = std::max(-1.0f, static_cast<float>(sp[2]) * kInv32767);
                        dst[3] = d3;
                    }
                } else if (comps == 2) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = std::max(-1.0f, static_cast<float>(sp[0]) * kInv32767);
                        dst[1] = std::max(-1.0f, static_cast<float>(sp[1]) * kInv32767);
                        dst[2] = d2;
                        dst[3] = d3;
                    }
                } else if (comps == 4) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = std::max(-1.0f, static_cast<float>(sp[0]) * kInv32767);
                        dst[1] = std::max(-1.0f, static_cast<float>(sp[1]) * kInv32767);
                        dst[2] = std::max(-1.0f, static_cast<float>(sp[2]) * kInv32767);
                        dst[3] = std::max(-1.0f, static_cast<float>(sp[3]) * kInv32767);
                    }
                } else {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = std::max(-1.0f, static_cast<float>(sp[0]) * kInv32767);
                        dst[1] = d1;
                        dst[2] = d2;
                        dst[3] = d3;
                    }
                }
            } else {
                if (comps == 3) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = static_cast<float>(sp[0]);
                        dst[1] = static_cast<float>(sp[1]);
                        dst[2] = static_cast<float>(sp[2]);
                        dst[3] = d3;
                    }
                } else if (comps == 2) {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = static_cast<float>(sp[0]);
                        dst[1] = static_cast<float>(sp[1]);
                        dst[2] = d2;
                        dst[3] = d3;
                    }
                } else {
                    for (uint32_t v = 0; v < vert_count; ++v, p += eff_stride, dst += v_stride_floats) {
                        const auto* sp = reinterpret_cast<const int16_t*>(p);
                        dst[0] = static_cast<float>(sp[0]);
                        dst[1] = (comps >= 2) ? static_cast<float>(sp[1]) : d1;
                        dst[2] = (comps >= 3) ? static_cast<float>(sp[2]) : d2;
                        dst[3] = (comps >= 4) ? static_cast<float>(sp[3]) : d3;
                    }
                }
            }
            break;
        }
        default: {
            for (uint32_t v = 0; v < vert_count; ++v) {
                decode_vertex_attrib(attr, vert_start + v, dst_base + static_cast<size_t>(v) * v_stride_floats);
            }
            break;
        }
    }
}

void create_fallback_white_texture() {
    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    D3D12TextureEntry& fb = g_d3d.textures[0];
    fb.cpu_slot = 0;
    fb.width = 1;
    fb.height = 1;
    HRESULT hr = g_d3d.device->CreateCommittedResource(
        &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&fb.resource));
    if (SUCCEEDED(hr)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        g_d3d.device->CreateShaderResourceView(fb.resource.Get(), &srv_desc, cpu_srv_handle(0));
        fb.rgba_staging = {255, 255, 255, 255};
        fb.pending_upload = true;
        fb.full_upload = true;
        g_d3d.pending_texture_uploads.push_back(0);
    }
}

} // namespace

bool init_d3d12_device(void* native_window, uint32_t width, uint32_t height) {
    if (g_d3d.initialized) return true;
    HWND hwnd = static_cast<HWND>(native_window);
    if (!hwnd) {
        std::cerr << "[D3D12] Cannot initialize without a valid HWND.\n";
        return false;
    }

    g_d3d.width = std::max(1u, width);
    g_d3d.height = std::max(1u, height);

    UINT dxgi_flags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debug_ctrl;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl)))) {
        debug_ctrl->EnableDebugLayer();
        dxgi_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    HRESULT hr = CreateDXGIFactory2(dxgi_flags, IID_PPV_ARGS(&g_d3d.factory));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDXGIFactory2", hr);
        return false;
    }

    ComPtr<IDXGIAdapter1> chosen_adapter;
    std::string adapter_name;
    for (UINT idx = 0;
         g_d3d.factory->EnumAdapterByGpuPreference(
             idx, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&chosen_adapter)) != DXGI_ERROR_NOT_FOUND;
         ++idx) {
        DXGI_ADAPTER_DESC1 desc{};
        chosen_adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            chosen_adapter.Reset();
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(chosen_adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
            adapter_name = narrow_adapter_name(desc.Description);
            break;
        }
        chosen_adapter.Reset();
    }

    if (!chosen_adapter) {
        ComPtr<IDXGIAdapter> warp_adapter;
        hr = g_d3d.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter));
        if (SUCCEEDED(hr)) {
            warp_adapter.As(&chosen_adapter);
            adapter_name = "Microsoft Basic Render Driver (WARP)";
        }
    }

    hr = D3D12CreateDevice(chosen_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_d3d.device));
    if (FAILED(hr)) {
        log_hresult_failure("D3D12CreateDevice", hr);
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_d3d.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&g_d3d.command_queue));
    if (FAILED(hr)) {
        log_hresult_failure("CreateCommandQueue", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.BufferCount = kFrameCount;
    sc_desc.Width = g_d3d.width;
    sc_desc.Height = g_d3d.height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.SampleDesc.Count = 1;

    ComPtr<IDXGIFactory5> factory5;
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(g_d3d.factory.As(&factory5)) &&
        SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing)))) {
        g_d3d.tearing_supported = (allow_tearing == TRUE);
    }
    if (g_d3d.tearing_supported) {
        sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> sc1;
    hr = g_d3d.factory->CreateSwapChainForHwnd(
        g_d3d.command_queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &sc1);
    if (FAILED(hr) && (sc_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        sc_desc.Flags &= ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr = g_d3d.factory->CreateSwapChainForHwnd(
            g_d3d.command_queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &sc1);
    }
    if (FAILED(hr)) {
        log_hresult_failure("CreateSwapChainForHwnd", hr);
        return false;
    }

    g_d3d.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    hr = sc1.As(&g_d3d.swap_chain);
    if (FAILED(hr)) {
        log_hresult_failure("SwapChain1 -> SwapChain3", hr);
        return false;
    }
    if (sc_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        // Match triple-buffered swapchain (kFrameCount = 3): allows 2 frames queued/pipelined
        // so V-Sync overlaps CPU draw recording with scanout instead of stalling 7-9ms before drawFn.
        if (SUCCEEDED(g_d3d.swap_chain->SetMaximumFrameLatency(2))) {
            g_d3d.frame_latency_waitable = g_d3d.swap_chain->GetFrameLatencyWaitableObject();
        }
    }
    g_d3d.frame_index = g_d3d.swap_chain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = kFrameCount;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = g_d3d.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&g_d3d.rtv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (RTV)", hr);
        return false;
    }
    g_d3d.rtv_descriptor_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
    dsv_heap_desc.NumDescriptors = kMaxDsvDescriptors;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr = g_d3d.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&g_d3d.dsv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (DSV)", hr);
        return false;
    }
    g_d3d.dsv_descriptor_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    D3D12_DESCRIPTOR_HEAP_DESC cpu_srv_desc{};
    cpu_srv_desc.NumDescriptors = kMaxCpuTextures;
    cpu_srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cpu_srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = g_d3d.device->CreateDescriptorHeap(&cpu_srv_desc, IID_PPV_ARGS(&g_d3d.cpu_srv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (CPU SRV)", hr);
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC gpu_srv_desc{};
    gpu_srv_desc.NumDescriptors = kPersistentSrvSlots + kFrameCount * kSrvPerFrame;
    gpu_srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpu_srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_d3d.device->CreateDescriptorHeap(&gpu_srv_desc, IID_PPV_ARGS(&g_d3d.gpu_srv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (GPU SRV)", hr);
        return false;
    }
    g_d3d.srv_descriptor_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Sampler descriptor heap (9 tables * 8 samplers = 72 descriptors for wrap/clamp/mirror combos)
    D3D12_DESCRIPTOR_HEAP_DESC sampler_desc{};
    sampler_desc.NumDescriptors = 128; // 9 tables * 8 samplers + headroom
    sampler_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_d3d.device->CreateDescriptorHeap(&sampler_desc, IID_PPV_ARGS(&g_d3d.sampler_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (Sampler)", hr);
        return false;
    }
    g_d3d.sampler_descriptor_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

    if (!create_render_targets() || !create_depth_stencil(g_d3d.width, g_d3d.height) || !create_root_signature()) {
        shutdown_d3d12();
        return false;
    }

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = kFrameCount * kUploadBytesPerFrame;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = g_d3d.device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_d3d.upload_ring));
    if (FAILED(hr)) {
        log_hresult_failure("CreateCommittedResource (UploadRing)", hr);
        shutdown_d3d12();
        return false;
    }
    hr = g_d3d.upload_ring->Map(0, nullptr, reinterpret_cast<void**>(&g_d3d.upload_ring_mapped));
    if (FAILED(hr)) {
        log_hresult_failure("UploadRing::Map", hr);
        shutdown_d3d12();
        return false;
    }

    for (uint32_t i = 0; i < kFrameCount; i++) {
        hr = g_d3d.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d3d.command_allocators[i]));
        if (FAILED(hr)) {
            log_hresult_failure("CreateCommandAllocator", hr);
            shutdown_d3d12();
            return false;
        }
    }

    hr = g_d3d.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        g_d3d.command_allocators[g_d3d.frame_index].Get(),
        nullptr,
        IID_PPV_ARGS(&g_d3d.command_list)
    );
    if (FAILED(hr)) {
        log_hresult_failure("CreateCommandList", hr);
        shutdown_d3d12();
        return false;
    }
    g_d3d.command_list->Close();

    hr = g_d3d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d.fence));
    if (FAILED(hr)) {
        log_hresult_failure("CreateFence", hr);
        shutdown_d3d12();
        return false;
    }
    g_d3d.fence_event.reset(CreateEventA(nullptr, FALSE, FALSE, nullptr));
    if (!g_d3d.fence_event.handle) {
        std::cerr << "[D3D12] Failed to create fence event.\n";
        shutdown_d3d12();
        return false;
    }

    create_fallback_white_texture();
    g_d3d.current_viewport = {0.0f, 0.0f, static_cast<float>(g_d3d.width), static_cast<float>(g_d3d.height), 0.0f, 1.0f};
    g_d3d.current_scissor = {0, 0, static_cast<LONG>(g_d3d.width), static_cast<LONG>(g_d3d.height)};
    g_d3d.next_fence_value = 1;
    g_d3d.fence_values.fill(0);
    g_d3d.main_thread_id = GetCurrentThreadId();
    g_d3d.initialized = true;
    lcs::ui_overlay::init_d3d12(native_window, g_d3d.device.Get(), g_d3d.command_queue.Get(), kFrameCount);

    std::cout << "[D3D12] Device initialized on adapter: " << adapter_name
              << " (" << g_d3d.width << "x" << g_d3d.height << ", " << kFrameCount << " buffers)\n";
    return true;
}

void resize_d3d12_swapchain(uint32_t width, uint32_t height) {
    if (!g_d3d.initialized || width == 0 || height == 0) return;
    if (width == g_d3d.width && height == g_d3d.height) return;

    if (g_d3d.frame_recording) {
        g_d3d.command_list->Close();
        g_d3d.frame_recording = false;
    }

    wait_for_gpu();

    for (auto& rt : g_d3d.render_targets) rt.Reset();
    g_d3d.depth_stencil.Reset();

    UINT sc_flags = g_d3d.tearing_supported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (g_d3d.frame_latency_waitable != nullptr) {
        sc_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }
    HRESULT hr = g_d3d.swap_chain->ResizeBuffers(
        kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, sc_flags);
    if (FAILED(hr)) {
        log_hresult_failure("IDXGISwapChain3::ResizeBuffers", hr);
        return;
    }

    g_d3d.width = width;
    g_d3d.height = height;
    g_d3d.current_viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    g_d3d.current_scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    g_d3d.frame_index = g_d3d.swap_chain->GetCurrentBackBufferIndex();

    create_render_targets();
    create_depth_stencil(width, height);
    std::cout << "[D3D12] Swapchain resized to " << width << "x" << height << "\n";
}

void begin_d3d12_frame() {
    if (!g_d3d.initialized || g_d3d.frame_recording) return;
    if (!g_d3d.tmp_audit_checked) {
        g_d3d.tmp_audit_checked = true;
        g_d3d.tmp_audit_enabled = (std::getenv("LCS_DIAG_AUDIT") != nullptr);
    }
    const auto b_t0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    g_d3d.tmp_wait_ns = 0;
    g_d3d.tmp_pso_ns = 0; g_d3d.tmp_pso_cnt = 0;
    g_d3d.tmp_tex_ns = 0; g_d3d.tmp_tex_cnt = 0;
    g_d3d.tmp_buf_del_ns = 0; g_d3d.tmp_buf_del_cnt = 0;
    g_d3d.tmp_buf_up_ns = 0; g_d3d.tmp_vram_fail = 0;
    g_d3d.tmp_split_cnt = 0; g_d3d.tmp_draws = 0;
    g_d3d.tmp_pres_ns = 0;
    g_d3d.tmp_draw_ns = 0; g_d3d.tmp_ps2_ns = 0; g_d3d.tmp_exec_ns = 0;

    wait_for_frame(g_d3d.frame_index);
    if (g_d3d.tmp_audit_enabled) {
        const auto now = std::chrono::steady_clock::now();
        g_d3d.tmp_wait_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - b_t0).count());
        g_d3d.tmp_frame_start_ns = static_cast<uint64_t>(b_t0.time_since_epoch().count());
    }
    if (g_d3d.need_persistent_srv_gc) {
        for (uint32_t fi = 0; fi < kFrameCount; ++fi) {
            if (fi != g_d3d.frame_index) wait_for_frame(fi);
        }
        g_d3d.srv_table_cache.clear();
        g_d3d.srv2_cache.fill({});
        g_d3d.persistent_srv_cursor = 0;
        g_d3d.need_persistent_srv_gc = false;
    }
    // Reclaim resources and CPU SRV slots retired on this frame slot after GPU fence wait.
    g_d3d.retired_resources[g_d3d.frame_index].clear();
    for (uint32_t slot : g_d3d.retired_srv_slots[g_d3d.frame_index]) {
        g_d3d.free_cpu_srv_slots.push_back(slot);
    }
    g_d3d.retired_srv_slots[g_d3d.frame_index].clear();

    g_d3d.upload_cursors[g_d3d.frame_index] = 0;
    g_d3d.srv_cursors[g_d3d.frame_index] = 0;
    ++g_d3d.frame_seq;

    auto& alloc = g_d3d.command_allocators[g_d3d.frame_index];
    alloc->Reset();
    g_d3d.command_list->Reset(alloc.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_d3d.command_list->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = current_rtv();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = current_dsv();
    g_d3d.command_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    ID3D12DescriptorHeap* heaps[] = {g_d3d.gpu_srv_heap.Get(), g_d3d.sampler_heap.Get()};
    g_d3d.command_list->SetDescriptorHeaps(2, heaps);
    g_d3d.command_list->SetGraphicsRootSignature(g_d3d.root_signature.Get());

    g_d3d.command_list->RSSetViewports(1, &g_d3d.current_viewport);
    g_d3d.command_list->RSSetScissorRects(1, &g_d3d.current_scissor);

    g_d3d.frame_recording = true;
    g_d3d.cleared_this_frame = false;
    reset_draw_state_cache(false);
    g_d3d.cached_vp = g_d3d.current_viewport;
    g_d3d.cached_vp_valid = true;
    g_d3d.cached_scissor = g_d3d.current_scissor;
    g_d3d.cached_scissor_valid = true;

    flush_pending_texture_uploads();
}

void clear_d3d12_targets(bool clear_color, const float color[4],
                         bool clear_depth, float depth,
                         bool clear_stencil, uint8_t stencil) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();

    if (g_d3d.bound_fbo != 0) {
        if (clear_depth && g_d3d.fbo_depth_tex_id != 0) {
            auto it = g_d3d.textures.find(g_d3d.fbo_depth_tex_id);
            if (it != g_d3d.textures.end() && it->second.is_depth && it->second.dsv_slot != 0) {
                g_d3d.command_list->ClearDepthStencilView(
                    dsv_handle(it->second.dsv_slot), D3D12_CLEAR_FLAG_DEPTH, depth, 0, 0, nullptr);
            }
        }
        return;
    }

    if (clear_color) {
        g_d3d.command_list->ClearRenderTargetView(current_rtv(), color, 0, nullptr);
        g_d3d.cleared_this_frame = true;
    }

    D3D12_CLEAR_FLAGS ds_flags = static_cast<D3D12_CLEAR_FLAGS>(0);
    if (clear_depth) ds_flags |= D3D12_CLEAR_FLAG_DEPTH;
    if (clear_stencil) ds_flags |= D3D12_CLEAR_FLAG_STENCIL;
    if (ds_flags != 0) {
        g_d3d.command_list->ClearDepthStencilView(current_dsv(), ds_flags, depth, stencil, 0, nullptr);
    }
}

void set_d3d12_viewport(float x, float y, float width, float height, float min_depth, float max_depth) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();

    D3D12_VIEWPORT vp{x, y, width, height, min_depth, max_depth};
    g_d3d.current_viewport = vp;
    if (!g_d3d.cached_vp_valid ||
        g_d3d.cached_vp.TopLeftX != vp.TopLeftX ||
        g_d3d.cached_vp.TopLeftY != vp.TopLeftY ||
        g_d3d.cached_vp.Width    != vp.Width    ||
        g_d3d.cached_vp.Height   != vp.Height   ||
        g_d3d.cached_vp.MinDepth != vp.MinDepth ||
        g_d3d.cached_vp.MaxDepth != vp.MaxDepth) {
        g_d3d.command_list->RSSetViewports(1, &vp);
        g_d3d.cached_vp = vp;
        g_d3d.cached_vp_valid = true;
    }
}

void set_d3d12_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();

    // RenderBridge::gl_scissor already converts GL bottom-left Y to D3D12 top-left Y.
    const LONG max_w = static_cast<LONG>(active_target_width());
    const LONG max_h = static_cast<LONG>(active_target_height());
    LONG top = static_cast<LONG>(y);
    LONG bottom = static_cast<LONG>(y) + static_cast<LONG>(height);
    D3D12_RECT rect{
        std::max<LONG>(0, x),
        std::max<LONG>(0, top),
        std::min<LONG>(max_w, static_cast<LONG>(x) + static_cast<LONG>(width)),
        std::min<LONG>(max_h, bottom)
    };
    if (rect.right < rect.left) rect.right = rect.left;
    if (rect.bottom < rect.top) rect.bottom = rect.top;
    g_d3d.current_scissor = rect;
    if (!g_d3d.cached_scissor_valid ||
        g_d3d.cached_scissor.left   != rect.left   ||
        g_d3d.cached_scissor.top    != rect.top    ||
        g_d3d.cached_scissor.right  != rect.right  ||
        g_d3d.cached_scissor.bottom != rect.bottom) {
        g_d3d.command_list->RSSetScissorRects(1, &rect);
        g_d3d.cached_scissor = rect;
        g_d3d.cached_scissor_valid = true;
    }
}

void bind_d3d12_framebuffer(uint32_t fbo_id, uint32_t color_tex_id, uint32_t depth_tex_id,
                            uint32_t width, uint32_t height) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();
    flush_pending_texture_uploads();

    if (g_d3d.fbo_depth_tex_id != 0 && (fbo_id == 0 || depth_tex_id != g_d3d.fbo_depth_tex_id)) {
        auto prev_it = g_d3d.textures.find(g_d3d.fbo_depth_tex_id);
        if (prev_it != g_d3d.textures.end() && prev_it->second.resource && prev_it->second.in_dsv_state) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = prev_it->second.resource.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_d3d.command_list->ResourceBarrier(1, &b);
            prev_it->second.in_dsv_state = false;
            prev_it->second.in_srv_state = true;
        }
    }

    g_d3d.bound_fbo = fbo_id;
    g_d3d.fbo_color_tex_id = color_tex_id;
    g_d3d.fbo_depth_tex_id = depth_tex_id;
    g_d3d.fbo_width = (width > 0) ? width : g_d3d.width;
    g_d3d.fbo_height = (height > 0) ? height : g_d3d.height;

    if (fbo_id != 0 && depth_tex_id != 0) {
        auto it = g_d3d.textures.find(depth_tex_id);
        if (it != g_d3d.textures.end() && it->second.resource && it->second.is_depth) {
            if (it->second.in_srv_state) {
                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource = it->second.resource.Get();
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                g_d3d.command_list->ResourceBarrier(1, &b);
                it->second.in_srv_state = false;
                it->second.in_dsv_state = true;
            }
            if (it->second.width > 0 && it->second.height > 0) {
                g_d3d.fbo_width = it->second.width;
                g_d3d.fbo_height = it->second.height;
            }
        }
    }

    apply_current_render_targets();
    g_d3d.cached_scissor_valid = false;
}

void set_d3d12_color_grading(float brightness_mul, float contrast_mul, float saturation_mul) {
    const float nb = std::clamp(brightness_mul, 0.2f, 2.5f);
    const float nc = std::clamp(contrast_mul, 0.2f, 2.5f);
    const float ns = std::clamp(saturation_mul, 0.0f, 3.0f);
    if (nb != g_d3d.color_grade_brightness || nc != g_d3d.color_grade_contrast || ns != g_d3d.color_grade_saturation) {
        g_d3d.color_grade_brightness = nb;
        g_d3d.color_grade_contrast = nc;
        g_d3d.color_grade_saturation = ns;
        g_d3d.cached_grade_dirty = true;
    }
}

namespace {

bool ensure_ps2_post_process_resources() {
    if (!g_d3d.initialized || g_d3d.width == 0 || g_d3d.height == 0) return false;

    if (!g_d3d.ps2_radiosity_pso || !g_d3d.ps2_composite_pso) {
        static constexpr const char* kPs2FullscreenVS = R"(
            struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
            VSOut main(uint id : SV_VertexID) {
                float2 pos[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
                float2 uv[4]  = { float2(0.0, 1.0),   float2(0.0, 0.0),  float2(1.0, 1.0),  float2(1.0, 0.0) };
                VSOut o;
                o.pos = float4(pos[id], 0.0f, 1.0f);
                o.uv  = uv[id];
                return o;
            }
        )";

        static constexpr const char* kPs2RadiosityPS = R"(
            cbuffer Ps2RadCB : register(b0) {
                float4 texel_and_sub_off;
            };
            Texture2D scene_tex : register(t0);
            SamplerState lin_clamp : register(s0);
            struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

            float4 main(VSOut i) : SV_TARGET {
                float2 uv  = i.uv;
                float2 t   = texel_and_sub_off.xy;
                float2 sub = texel_and_sub_off.zw;

                float3 box4 = 0.25f * (
                    scene_tex.Sample(lin_clamp, uv).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2(sub.x, 0.0f)).rgb +
                    scene_tex.Sample(lin_clamp, uv + sub).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2(0.0f, sub.y)).rgb
                );

                float2 r1 = t * 1.75f;
                float2 r2 = t * 3.75f;
                float3 ring1 = 0.25f * (
                    scene_tex.Sample(lin_clamp, uv + float2( r1.x,  r1.y)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2(-r1.x,  r1.y)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2( r1.x, -r1.y)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2(-r1.x, -r1.y)).rgb
                );
                float3 ring2 = 0.25f * (
                    scene_tex.Sample(lin_clamp, uv + float2( r2.x,  0.0f)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2(-r2.x,  0.0f)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2( 0.0f,  r2.y)).rgb +
                    scene_tex.Sample(lin_clamp, uv + float2( 0.0f, -r2.y)).rgb
                );

                float3 soft_scene = box4 * 0.45f + ring1 * 0.35f + ring2 * 0.20f;
                float luma = dot(soft_scene, float3(0.299f, 0.587f, 0.114f));
                float peak_c = max(soft_scene.r, max(soft_scene.g, soft_scene.b));
                float hi_mask = smoothstep(0.38f, 0.90f, max(luma, peak_c * 0.78f));
                float3 rad_out = soft_scene * (0.28f + 0.72f * hi_mask) * hi_mask;
                return float4(rad_out, hi_mask);
            }
        )";

        static constexpr const char* kPs2CompositePS = R"(
            cbuffer Ps2CompCB : register(b0) {
                float4 c_blur_rgb;
                float4 c_final_rgb;
                float4 c_trail_tint;
                float4 c_motion_params;
                float4 c_uv_scales;
            };
            Texture2D scene_tex   : register(t0);
            Texture2D rad_tex     : register(t1);
            Texture2D history_tex : register(t2);
            SamplerState lin_clamp : register(s0);
            struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

            float4 main(VSOut i) : SV_TARGET {
                float2 uv  = i.uv;
                float2 sub = c_uv_scales.zw;

                float3 c00 = scene_tex.Sample(lin_clamp, uv).rgb;
                float3 c10 = scene_tex.Sample(lin_clamp, uv + float2(sub.x, 0.0f)).rgb;
                float3 c11 = scene_tex.Sample(lin_clamp, uv + sub).rgb;
                float3 c01 = scene_tex.Sample(lin_clamp, uv + float2(0.0f, sub.y)).rgb;
                float3 box4 = 0.25f * (c00 + c10 + c11 + c01);
                float3 cur_col = lerp(c00, box4, saturate(c_motion_params.w));

                float speed_f = c_motion_params.x;
                if (speed_f > 0.001f) {
                    float2 to_center = float2(0.5f, 0.52f) - uv;
                    float edge_weight = smoothstep(0.14f, 0.72f, length(to_center));
                    float2 step_uv = to_center * (0.018f * speed_f * edge_weight);
                    float3 r_acc = cur_col * 0.28f;
                    r_acc += scene_tex.Sample(lin_clamp, uv + step_uv * 0.25f).rgb * 0.24f;
                    r_acc += scene_tex.Sample(lin_clamp, uv + step_uv * 0.50f).rgb * 0.20f;
                    r_acc += scene_tex.Sample(lin_clamp, uv + step_uv * 0.75f).rgb * 0.16f;
                    r_acc += scene_tex.Sample(lin_clamp, uv + step_uv * 1.00f).rgb * 0.12f;
                    cur_col = r_acc;
                }

                // c_blur_rgb is CTimeCycle BlurRGB (= (255 - FinalColour) / 255 from PS2 GS subtractive blending),
                // producing LCS's classic cool blueish-cyan daylight filter and warm amber sunset filter.
                float3 blur_rgb   = c_blur_rgb.xyz;
                float rad_int     = c_blur_rgb.w;
                float col_int     = c_final_rgb.w;
                float blur_peak   = max(blur_rgb.r, max(blur_rgb.g, blur_rgb.b));
                float3 blur_dir   = (blur_peak > 0.001f) ? (blur_rgb / blur_peak) : float3(1.0f, 1.0f, 1.0f);
                float cool_day    = saturate((blur_dir.b - blur_dir.r) * 1.6f) * saturate((blur_dir.g - blur_dir.r) * 1.4f);
                float3 atmos_dir  = saturate(blur_dir + cool_day * float3(-0.04f, -0.02f, 0.16f));
                float tint_w      = saturate(blur_peak * 1.45f) * saturate(col_int);
                float3 ps2_atmos_grade = lerp(float3(1.0f, 1.0f, 1.0f), atmos_dir, 0.40f * tint_w);

                if (rad_int > 0.001f) {
                    float4 rad_sample = rad_tex.Sample(lin_clamp, uv);
                    float3 rad_tint = lerp(float3(0.92f, 0.98f, 1.04f), atmos_dir, 0.58f * tint_w);
                    float3 add_glow = rad_sample.rgb * rad_tint * (0.42f * rad_int);
                    cur_col = saturate(cur_col + add_glow * (1.0f - 0.50f * cur_col));
                }

                if (col_int > 0.001f) {
                    float scene_luma = dot(box4, float3(0.299f, 0.587f, 0.114f));
                    float mid_hi_w = smoothstep(0.05f, 0.65f, scene_luma);
                    float3 graded = cur_col * lerp(float3(1.0f, 1.0f, 1.0f), ps2_atmos_grade, mid_hi_w);
                    float3 haze_col = lerp(blur_rgb, atmos_dir * blur_peak, 0.65f * cool_day);
                    graded += haze_col * (0.10f * tint_w * mid_hi_w) * (1.0f - graded);
                    float3 contrast_col = smoothstep(0.0f, 1.0f, saturate(graded));
                    cur_col = saturate(lerp(graded, contrast_col, 0.08f * saturate(col_int)));
                }

                float trail_p = c_trail_tint.w;
                if (trail_p > 0.001f) {
                    float2 hist_uv = uv;
                    float drunk_f = c_motion_params.y;
                    if (drunk_f > 0.001f) {
                        float ph = c_motion_params.z;
                        hist_uv += float2(sin(uv.y * 10.0f + ph * 3.2f), cos(uv.x * 10.0f + ph * 2.7f)) * (0.0045f * drunk_f);
                    }
                    float3 hist_col = history_tex.Sample(lin_clamp, hist_uv).rgb * c_trail_tint.xyz;
                    cur_col = lerp(cur_col, hist_col, saturate(trail_p));
                }

                return float4(cur_col, 1.0f);
            }
        )";

        ComPtr<ID3DBlob> vs_blob, rad_ps_blob, comp_ps_blob, err_blob;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        if (FAILED(D3DCompile(kPs2FullscreenVS, std::strlen(kPs2FullscreenVS), nullptr, nullptr, nullptr,
                              "main", "vs_5_0", flags, 0, &vs_blob, &err_blob))) {
            std::cerr << "[D3D12] PS2 VS compile error: "
                      << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
            return false;
        }
        err_blob.Reset();
        if (FAILED(D3DCompile(kPs2RadiosityPS, std::strlen(kPs2RadiosityPS), nullptr, nullptr, nullptr,
                              "main", "ps_5_0", flags, 0, &rad_ps_blob, &err_blob))) {
            std::cerr << "[D3D12] PS2 Radiosity PS compile error: "
                      << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
            return false;
        }
        err_blob.Reset();
        if (FAILED(D3DCompile(kPs2CompositePS, std::strlen(kPs2CompositePS), nullptr, nullptr, nullptr,
                              "main", "ps_5_0", flags, 0, &comp_ps_blob, &err_blob))) {
            std::cerr << "[D3D12] PS2 Composite PS compile error: "
                      << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
            return false;
        }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = g_d3d.root_signature.Get();
        desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
        desc.PS = {rad_ps_blob->GetBufferPointer(), rad_ps_blob->GetBufferSize()};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = FALSE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;

        if (FAILED(g_d3d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_d3d.ps2_radiosity_pso)))) {
            return false;
        }
        desc.PS = {comp_ps_blob->GetBufferPointer(), comp_ps_blob->GetBufferSize()};
        if (FAILED(g_d3d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_d3d.ps2_composite_pso)))) {
            return false;
        }
    }

    if (!g_d3d.ps2_rtv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
        rtv_desc.NumDescriptors = 1;
        rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(g_d3d.device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&g_d3d.ps2_rtv_heap)))) {
            return false;
        }
    }
    auto alloc_srv_slot = [](uint32_t& slot) -> bool {
        if (slot != 0) return true;
        if (!g_d3d.free_cpu_srv_slots.empty()) {
            slot = g_d3d.free_cpu_srv_slots.back();
            g_d3d.free_cpu_srv_slots.pop_back();
            return true;
        }
        if (g_d3d.next_cpu_srv_slot >= kMaxCpuTextures) return false;
        slot = g_d3d.next_cpu_srv_slot++;
        return true;
    };
    if (!alloc_srv_slot(g_d3d.ps2_scene_copy_srv_slot) ||
        !alloc_srv_slot(g_d3d.ps2_radiosity_srv_slot) ||
        !alloc_srv_slot(g_d3d.ps2_history_srv_slot)) {
        return false;
    }

    if (g_d3d.ps2_scene_copy_tex && g_d3d.ps2_radiosity_tex && g_d3d.ps2_history_tex &&
        g_d3d.ps2_tex_width == g_d3d.width && g_d3d.ps2_tex_height == g_d3d.height) {
        return true;
    }

    if (g_d3d.ps2_scene_copy_tex) {
        g_d3d.retired_resources[g_d3d.frame_index].push_back(std::move(g_d3d.ps2_scene_copy_tex));
    }
    if (g_d3d.ps2_radiosity_tex) {
        g_d3d.retired_resources[g_d3d.frame_index].push_back(std::move(g_d3d.ps2_radiosity_tex));
    }
    if (g_d3d.ps2_history_tex) {
        g_d3d.retired_resources[g_d3d.frame_index].push_back(std::move(g_d3d.ps2_history_tex));
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto create_tex2d = [&](uint32_t w, uint32_t h, bool allow_rt, ComPtr<ID3D12Resource>& out_res) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = std::max(1u, w);
        desc.Height = std::max(1u, h);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = allow_rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        return SUCCEEDED(g_d3d.device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&out_res)));
    };

    const uint32_t rad_w = std::max(1u, g_d3d.width / 4u);
    const uint32_t rad_h = std::max(1u, g_d3d.height / 4u);
    if (!create_tex2d(g_d3d.width, g_d3d.height, false, g_d3d.ps2_scene_copy_tex) ||
        !create_tex2d(rad_w, rad_h, true, g_d3d.ps2_radiosity_tex) ||
        !create_tex2d(g_d3d.width, g_d3d.height, false, g_d3d.ps2_history_tex)) {
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;

    g_d3d.device->CreateShaderResourceView(
        g_d3d.ps2_scene_copy_tex.Get(), &srv_desc, cpu_srv_handle(g_d3d.ps2_scene_copy_srv_slot));
    g_d3d.device->CreateShaderResourceView(
        g_d3d.ps2_radiosity_tex.Get(), &srv_desc, cpu_srv_handle(g_d3d.ps2_radiosity_srv_slot));
    g_d3d.device->CreateShaderResourceView(
        g_d3d.ps2_history_tex.Get(), &srv_desc, cpu_srv_handle(g_d3d.ps2_history_srv_slot));
    g_d3d.device->CreateRenderTargetView(
        g_d3d.ps2_radiosity_tex.Get(), nullptr, g_d3d.ps2_rtv_heap->GetCPUDescriptorHandleForHeapStart());

    g_d3d.ps2_tex_width = g_d3d.width;
    g_d3d.ps2_tex_height = g_d3d.height;
    g_d3d.ps2_history_valid = false;
    return true;
}

} // namespace

void apply_d3d12_ps2_post_process(const Ps2PostProcessParams& params) {
    if (!params.enabled || !g_d3d.initialized || !g_d3d.frame_recording) return;
    if (g_d3d.main_thread_id != 0 && GetCurrentThreadId() != g_d3d.main_thread_id) return;
    if (g_d3d.video_overlay_active || g_d3d.loading_bar_active || g_d3d.bound_fbo != 0) return;
    TmpScopedNsTimer ps2_timer(g_d3d.tmp_audit_enabled, &g_d3d.tmp_ps2_ns);

    flush_pending_texture_uploads();

    if (!ensure_ps2_post_process_resources()) return;

    if (g_d3d.upload_cursors[g_d3d.frame_index] + 512 > kUploadBytesPerFrame ||
        g_d3d.srv_cursors[g_d3d.frame_index] + 2 * kMaxSamplers > kSrvPerFrame) {
        split_command_list_and_wait();
    }

    ID3D12DescriptorHeap* heaps[] = {g_d3d.gpu_srv_heap.Get(), g_d3d.sampler_heap.Get()};
    g_d3d.command_list->SetDescriptorHeaps(2, heaps);
    g_d3d.command_list->SetGraphicsRootSignature(g_d3d.root_signature.Get());

    ID3D12Resource* backbuffer = g_d3d.render_targets[g_d3d.frame_index].Get();
    if (!backbuffer) return;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_d3d.command_list->ResourceBarrier(1, &b);
    };

    transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(g_d3d.ps2_scene_copy_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_d3d.command_list->CopyResource(g_d3d.ps2_scene_copy_tex.Get(), backbuffer);
    transition(g_d3d.ps2_scene_copy_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (!g_d3d.ps2_history_valid || params.reset_history) {
        transition(g_d3d.ps2_history_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        g_d3d.command_list->CopyResource(g_d3d.ps2_history_tex.Get(), backbuffer);
        transition(g_d3d.ps2_history_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        g_d3d.ps2_history_valid = true;
    }

    transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    const float inv_w = 1.0f / static_cast<float>(std::max(1u, g_d3d.width));
    const float inv_h = 1.0f / static_cast<float>(std::max(1u, g_d3d.height));
    const float ps2_sub_u = 0.188f / 640.0f;
    const float ps2_sub_v = 0.188f / 448.0f;

    D3D12_GPU_DESCRIPTOR_HANDLE clamp_samp_gpu = g_d3d.sampler_heap->GetGPUDescriptorHandleForHeapStart();
    clamp_samp_gpu.ptr += static_cast<SIZE_T>(4) * 8 * g_d3d.sampler_descriptor_size;

    if (params.radiosity_intensity > 0.001f) {
        UploadAllocation rad_cb;
        if (alloc_upload_slice(256, 256, rad_cb)) {
            const float cb_data[4] = {inv_w, inv_h, ps2_sub_u, ps2_sub_v};
            std::memcpy(rad_cb.cpu_ptr, cb_data, sizeof(cb_data));

            uint32_t& srv_cursor = g_d3d.srv_cursors[g_d3d.frame_index];
            const uint32_t rad_srv_dst = kPersistentSrvSlots + g_d3d.frame_index * kSrvPerFrame + srv_cursor;
            srv_cursor += kMaxSamplers;

            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> src_handles;
            std::array<UINT, kMaxSamplers> src_sizes;
            for (uint32_t i = 0; i < kMaxSamplers; ++i) {
                src_handles[i] = cpu_srv_handle(i == 0 ? g_d3d.ps2_scene_copy_srv_slot : 0);
                src_sizes[i] = 1;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE dst_start = gpu_srv_cpu_handle(rad_srv_dst);
            UINT dst_size = kMaxSamplers;
            g_d3d.device->CopyDescriptors(
                1, &dst_start, &dst_size,
                kMaxSamplers, src_handles.data(), src_sizes.data(),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

            transition(g_d3d.ps2_radiosity_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

            const uint32_t rad_w = std::max(1u, g_d3d.width / 4u);
            const uint32_t rad_h = std::max(1u, g_d3d.height / 4u);
            const D3D12_VIEWPORT rad_vp{0.0f, 0.0f, static_cast<float>(rad_w), static_cast<float>(rad_h), 0.0f, 1.0f};
            const D3D12_RECT rad_sc{0, 0, static_cast<LONG>(rad_w), static_cast<LONG>(rad_h)};
            const D3D12_CPU_DESCRIPTOR_HANDLE rad_rtv = g_d3d.ps2_rtv_heap->GetCPUDescriptorHandleForHeapStart();

            g_d3d.command_list->OMSetRenderTargets(1, &rad_rtv, FALSE, nullptr);
            g_d3d.command_list->RSSetViewports(1, &rad_vp);
            g_d3d.command_list->RSSetScissorRects(1, &rad_sc);
            g_d3d.command_list->SetPipelineState(g_d3d.ps2_radiosity_pso.Get());
            g_d3d.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            g_d3d.command_list->SetGraphicsRootConstantBufferView(0, rad_cb.gpu_addr);
            g_d3d.command_list->SetGraphicsRootDescriptorTable(1, gpu_srv_gpu_handle(rad_srv_dst));
            g_d3d.command_list->SetGraphicsRootDescriptorTable(2, clamp_samp_gpu);
            g_d3d.command_list->DrawInstanced(4, 1, 0, 0);

            transition(g_d3d.ps2_radiosity_tex.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    }

    UploadAllocation comp_cb;
    if (alloc_upload_slice(256, 256, comp_cb)) {
        struct Ps2CompConstants {
            float blur_rgb[4];
            float final_rgb[4];
            float trail_tint[4];
            float motion_params[4];
            float uv_scales[4];
        } cb{};
        cb.blur_rgb[0] = params.blur_rgb[0];
        cb.blur_rgb[1] = params.blur_rgb[1];
        cb.blur_rgb[2] = params.blur_rgb[2];
        cb.blur_rgb[3] = params.radiosity_intensity;

        cb.final_rgb[0] = params.final_color_rgb[0];
        cb.final_rgb[1] = params.final_color_rgb[1];
        cb.final_rgb[2] = params.final_color_rgb[2];
        cb.final_rgb[3] = params.color_filter_intensity;

        cb.trail_tint[0] = params.trail_tint_rgb[0];
        cb.trail_tint[1] = params.trail_tint_rgb[1];
        cb.trail_tint[2] = params.trail_tint_rgb[2];
        cb.trail_tint[3] = params.trails_persistence;

        cb.motion_params[0] = params.speed_blur_factor;
        cb.motion_params[1] = params.drunk_blur_factor;
        cb.motion_params[2] = params.anim_phase;
        cb.motion_params[3] = (params.radiosity_intensity > 0.001f) ? params.soft_focus_weight : 0.0f;

        cb.uv_scales[0] = inv_w;
        cb.uv_scales[1] = inv_h;
        cb.uv_scales[2] = ps2_sub_u;
        cb.uv_scales[3] = ps2_sub_v;

        std::memcpy(comp_cb.cpu_ptr, &cb, sizeof(cb));

        uint32_t& srv_cursor = g_d3d.srv_cursors[g_d3d.frame_index];
        const uint32_t comp_srv_dst = kPersistentSrvSlots + g_d3d.frame_index * kSrvPerFrame + srv_cursor;
        srv_cursor += kMaxSamplers;

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> src_handles;
        std::array<UINT, kMaxSamplers> src_sizes;
        src_handles[0] = cpu_srv_handle(g_d3d.ps2_scene_copy_srv_slot);
        src_handles[1] = cpu_srv_handle(g_d3d.ps2_radiosity_srv_slot);
        src_handles[2] = cpu_srv_handle(g_d3d.ps2_history_srv_slot);
        for (uint32_t i = 3; i < kMaxSamplers; ++i) {
            src_handles[i] = cpu_srv_handle(0);
        }
        src_sizes.fill(1);

        D3D12_CPU_DESCRIPTOR_HANDLE dst_start = gpu_srv_cpu_handle(comp_srv_dst);
        UINT dst_size = kMaxSamplers;
        g_d3d.device->CopyDescriptors(
            1, &dst_start, &dst_size,
            kMaxSamplers, src_handles.data(), src_sizes.data(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        const D3D12_VIEWPORT full_vp{0.0f, 0.0f, static_cast<float>(g_d3d.width), static_cast<float>(g_d3d.height), 0.0f, 1.0f};
        const D3D12_RECT full_sc{0, 0, static_cast<LONG>(g_d3d.width), static_cast<LONG>(g_d3d.height)};
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = current_rtv();

        g_d3d.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_d3d.command_list->RSSetViewports(1, &full_vp);
        g_d3d.command_list->RSSetScissorRects(1, &full_sc);
        g_d3d.command_list->SetPipelineState(g_d3d.ps2_composite_pso.Get());
        g_d3d.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_d3d.command_list->SetGraphicsRootConstantBufferView(0, comp_cb.gpu_addr);
        g_d3d.command_list->SetGraphicsRootDescriptorTable(1, gpu_srv_gpu_handle(comp_srv_dst));
        g_d3d.command_list->SetGraphicsRootDescriptorTable(2, clamp_samp_gpu);
        g_d3d.command_list->DrawInstanced(4, 1, 0, 0);
    }

    transition(backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(g_d3d.ps2_history_tex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    g_d3d.command_list->CopyResource(g_d3d.ps2_history_tex.Get(), backbuffer);
    transition(g_d3d.ps2_history_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    apply_current_render_targets();
    g_d3d.command_list->RSSetViewports(1, &g_d3d.current_viewport);
    g_d3d.command_list->RSSetScissorRects(1, &g_d3d.current_scissor);
    reset_draw_state_cache(false);
    g_d3d.cached_vp = g_d3d.current_viewport;
    g_d3d.cached_vp_valid = true;
    g_d3d.cached_scissor = g_d3d.current_scissor;
    g_d3d.cached_scissor_valid = true;
}

static inline void invalidate_program_caches(uint32_t prog_id) {
    if (g_d3d.last_pso_key_valid && g_d3d.last_pso_key.prog_id == prog_id) {
        g_d3d.last_pso_key_valid = false;
    }
    for (auto& e : g_d3d.fast_pso_cache) {
        if (e.valid && e.key.prog_id == prog_id) e.valid = false;
    }
    for (auto& e : g_d3d.fast_vao_cache) {
        if (e.valid && e.prog_id == prog_id) e.valid = false;
    }
    for (auto& e : g_d3d.fast_state_pso_cache) {
        if (e.valid && e.prog_id == prog_id) e.valid = false;
    }
}

bool register_d3d12_program(uint32_t prog_id, const TranslatedProgramHLSL& translated) {
    if (!g_d3d.initialized) return false;

    D3D12ProgramEntry entry;
    entry.attribs = translated.attribs;
    entry.vs_preprocessed = translated.vs_preprocessed;
    entry.fs_preprocessed = translated.fs_preprocessed;
    entry.bound_attribs = translated.bound_attribs;

    UINT compile_flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL2;
    ComPtr<ID3DBlob> err_blob;

    HRESULT hr = D3DCompile(
        translated.vs_hlsl.data(), translated.vs_hlsl.size(),
        nullptr, nullptr, nullptr, "main", "vs_5_0",
        compile_flags, 0, &entry.vs_bytecode, &err_blob);
    if (FAILED(hr)) {
        std::cerr << "[D3D12] VS compile failed for program " << prog_id << ": "
                  << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
        return false;
    }

    err_blob.Reset();
    hr = D3DCompile(
        translated.ps_hlsl.data(), translated.ps_hlsl.size(),
        nullptr, nullptr, nullptr, "main", "ps_5_0",
        compile_flags, 0, &entry.ps_bytecode, &err_blob);
    if (FAILED(hr)) {
        std::cerr << "[D3D12] PS compile failed for program " << prog_id << ": "
                  << (err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "unknown") << "\n";
        return false;
    }

    entry.valid = true;
    auto& slot = g_d3d.programs[prog_id];
    slot = std::move(entry);
    if (prog_id >= g_d3d.prog_ptrs.size()) {
        g_d3d.prog_ptrs.resize(static_cast<size_t>(prog_id) + 32u, nullptr);
    }
    g_d3d.prog_ptrs[prog_id] = &slot;
    if (g_d3d.cached_prog_id == prog_id) {
        g_d3d.cached_prog_id = 0;
        g_d3d.cached_prog_entry = nullptr;
    }
    if (g_d3d.cached_cb_prog_id == prog_id) {
        g_d3d.cached_cb_prog_id = 0;
        g_d3d.cached_cb_addr = 0;
    }
    invalidate_program_caches(prog_id);
    return true;
}

void unregister_d3d12_program(uint32_t prog_id) {
    if (g_d3d.cached_prog_id == prog_id) {
        g_d3d.cached_prog_id = 0;
        g_d3d.cached_prog_entry = nullptr;
    }
    if (g_d3d.cached_cb_prog_id == prog_id) {
        g_d3d.cached_cb_prog_id = 0;
        g_d3d.cached_cb_addr = 0;
    }
    invalidate_program_caches(prog_id);
    if (prog_id < g_d3d.prog_ptrs.size()) {
        g_d3d.prog_ptrs[prog_id] = nullptr;
    }
    g_d3d.programs.erase(prog_id);
}

namespace {

// Allocate/resize GPU Texture2D resource & CPU SRV descriptor if dimensions/depth changed.
bool prepare_d3d12_texture_entry(uint32_t tex_id, uint32_t width, uint32_t height,
                                 bool is_depth_fmt, D3D12TextureEntry*& out_tex) {
    auto& tex = g_d3d.textures[tex_id];
    if (tex.cpu_slot == 0) {
        if (!g_d3d.free_cpu_srv_slots.empty()) {
            tex.cpu_slot = g_d3d.free_cpu_srv_slots.back();
            g_d3d.free_cpu_srv_slots.pop_back();
        } else {
            if (g_d3d.next_cpu_srv_slot >= kMaxCpuTextures) {
                static bool logged_full = false;
                if (!logged_full) {
                    logged_full = true;
                    std::cerr << "[D3D12] CPU SRV heap full (" << kMaxCpuTextures << " slots).\n";
                }
                return false;
            }
            tex.cpu_slot = g_d3d.next_cpu_srv_slot++;
        }
    }

    const bool need_realloc = (!tex.resource || tex.width != width || tex.height != height || tex.is_depth != is_depth_fmt);
    if (need_realloc) {
        if (tex.resource) {
            g_d3d.retired_resources[g_d3d.frame_index].push_back(std::move(tex.resource));
        }
        tex.width = width;
        tex.height = height;
        tex.is_depth = is_depth_fmt;
        tex.in_srv_state = false;
        tex.in_dsv_state = false;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = is_depth_fmt ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        if (is_depth_fmt) {
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        }

        D3D12_CLEAR_VALUE clear_val{};
        clear_val.Format = DXGI_FORMAT_D32_FLOAT;
        clear_val.DepthStencil.Depth = 1.0f;
        clear_val.DepthStencil.Stencil = 0;

        HRESULT hr = g_d3d.device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            is_depth_fmt ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST,
            is_depth_fmt ? &clear_val : nullptr,
            IID_PPV_ARGS(&tex.resource));
        if (FAILED(hr)) {
            log_hresult_failure("CreateCommittedResource (Texture2D)", hr);
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = is_depth_fmt ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        g_d3d.device->CreateShaderResourceView(tex.resource.Get(), &srv_desc, cpu_srv_handle(tex.cpu_slot));
        tex.srv_gen = ++g_d3d.next_srv_gen;
        if (tex.srv_gen == 0) tex.srv_gen = ++g_d3d.next_srv_gen;

        if (is_depth_fmt) {
            if (tex.dsv_slot == 0 && g_d3d.next_dsv_slot < kMaxDsvDescriptors) {
                tex.dsv_slot = g_d3d.next_dsv_slot++;
            }
            if (tex.dsv_slot != 0) {
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
                dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
                dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                g_d3d.device->CreateDepthStencilView(tex.resource.Get(), &dsv_desc, dsv_handle(tex.dsv_slot));
            }
            tex.in_srv_state = true;
            tex.pending_upload = false;
            out_tex = &tex;
            return false;
        }
    } else if (is_depth_fmt) {
        out_tex = &tex;
        return false;
    }

    out_tex = &tex;
    return true;
}

} // namespace

void upload_d3d12_texture(uint32_t tex_id, uint32_t width, uint32_t height,
                          uint32_t format, uint32_t type, const void* pixels) {
    if (!g_d3d.initialized || tex_id == 0 || width == 0 || height == 0) return;
    TmpScopedNsTimer tx_timer(g_d3d.tmp_audit_enabled, &g_d3d.tmp_tex_ns);

    const bool is_depth_fmt = (format == 0x1902 || format == 0x81A5 || format == 0x81A6 ||
                               format == 0x81A7 || format == 0x88F0);
    D3D12TextureEntry* tex = nullptr;
    if (!prepare_d3d12_texture_entry(tex_id, width, height, is_depth_fmt, tex) || !tex) return;

    tex->rgba_staging = unpack_pixels_to_rgba8(width, height, format, type, pixels);
    tex->full_upload = true;
    if (!tex->pending_upload) {
        tex->pending_upload = true;
        g_d3d.pending_texture_uploads.push_back(tex_id);
    }
}

void upload_d3d12_texture_rgba8(uint32_t tex_id, uint32_t width, uint32_t height,
                                std::vector<uint8_t>&& rgba_pixels) {
    if (!g_d3d.initialized || tex_id == 0 || width == 0 || height == 0) return;
    TmpScopedNsTimer tx_timer(g_d3d.tmp_audit_enabled, &g_d3d.tmp_tex_ns);

    D3D12TextureEntry* tex = nullptr;
    if (!prepare_d3d12_texture_entry(tex_id, width, height, false, tex) || !tex) return;

    tex->rgba_staging = std::move(rgba_pixels);
    tex->full_upload = true;
    if (!tex->pending_upload) {
        tex->pending_upload = true;
        g_d3d.pending_texture_uploads.push_back(tex_id);
    }
}

void update_d3d12_texture_sub(uint32_t tex_id, int32_t xoffset, int32_t yoffset,
                              uint32_t width, uint32_t height,
                              uint32_t format, uint32_t type, const void* pixels) {
    if (!g_d3d.initialized || tex_id == 0 || width == 0 || height == 0 || !pixels) return;
    auto it = g_d3d.textures.find(tex_id);
    if (it == g_d3d.textures.end() || !it->second.resource) return;

    auto& tex = it->second;
    // If a sub-upload arrives while a previous upload is pending, flush first to preserve order.
    if (tex.pending_upload && g_d3d.frame_recording) {
        flush_pending_texture_uploads();
    }
    tex.rgba_staging = unpack_pixels_to_rgba8(width, height, format, type, pixels);
    tex.full_upload = false;
    tex.sub_x = xoffset;
    tex.sub_y = yoffset;
    tex.sub_w = width;
    tex.sub_h = height;
    if (!tex.pending_upload) {
        tex.pending_upload = true;
        g_d3d.pending_texture_uploads.push_back(tex_id);
    }
}

void delete_d3d12_texture(uint32_t tex_id) {
    if (tex_id == 0) return;
    auto it = g_d3d.textures.find(tex_id);
    if (it == g_d3d.textures.end()) return;
    if (it->second.resource) {
        g_d3d.retired_resources[g_d3d.frame_index].push_back(std::move(it->second.resource));
    }
    if (it->second.cpu_slot != 0) {
        g_d3d.retired_srv_slots[g_d3d.frame_index].push_back(it->second.cpu_slot);
    }
    if (tex_id < g_d3d.tex_ptrs.size()) {
        g_d3d.tex_ptrs[tex_id] = nullptr;
    }
    g_d3d.textures.erase(it);
}

void set_d3d12_texture_param(uint32_t tex_id, uint32_t pname, int32_t param) {
    if (tex_id == 0) return;
    // Create entry if glTexParameteri is called before glTexImage2D so wrap modes persist across upload.
    auto& tex = g_d3d.textures[tex_id];
    if (pname == 0x2802) { // GL_TEXTURE_WRAP_S
        tex.wrap_s = static_cast<uint32_t>(param);
    } else if (pname == 0x2803) { // GL_TEXTURE_WRAP_T
        tex.wrap_t = static_cast<uint32_t>(param);
    } else if (pname == 0x2800) { // GL_TEXTURE_MAG_FILTER
        // Mag filter doesn't affect sampler selection
    } else if (pname == 0x2801) { // GL_TEXTURE_MIN_FILTER
        // Min filter doesn't affect sampler selection
    }
}

struct ResolvedTopo {
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
    D3D_PRIMITIVE_TOPOLOGY d3d;
    bool expand_fan;
};

ResolvedTopo resolve_topology(uint32_t gl_mode) {
    switch (gl_mode) {
        case 0:  return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT,    D3D_PRIMITIVE_TOPOLOGY_POINTLIST,    false};
        case 1:  return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     D3D_PRIMITIVE_TOPOLOGY_LINELIST,     false};
        case 3:  return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE,     D3D_PRIMITIVE_TOPOLOGY_LINESTRIP,    false};
        case 5:  return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,false};
        case 6:  return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, true};
        case 4:
        default: return {D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false};
    }
}

inline D3D12TextureEntry* lookup_tex_fast(uint32_t tex_id) {
    if (tex_id < g_d3d.tex_ptrs.size()) {
        if (D3D12TextureEntry* p = g_d3d.tex_ptrs[tex_id]) return p;
    }
    auto it = g_d3d.textures.find(tex_id);
    if (it == g_d3d.textures.end()) return nullptr;
    if (tex_id >= g_d3d.tex_ptrs.size()) {
        g_d3d.tex_ptrs.resize(static_cast<size_t>(tex_id) + 128u, nullptr);
    }
    g_d3d.tex_ptrs[tex_id] = &it->second;
    return &it->second;
}

const D3D12ProgramEntry* find_program_fast(uint32_t prog_id) {
    if (prog_id == g_d3d.cached_prog_id && g_d3d.cached_prog_entry && g_d3d.cached_prog_entry->valid) {
        return g_d3d.cached_prog_entry;
    }
    D3D12ProgramEntry* p = (prog_id < g_d3d.prog_ptrs.size()) ? g_d3d.prog_ptrs[prog_id] : nullptr;
    if (!p) {
        auto pit = g_d3d.programs.find(prog_id);
        if (pit == g_d3d.programs.end()) return nullptr;
        p = &pit->second;
        if (prog_id >= g_d3d.prog_ptrs.size()) {
            g_d3d.prog_ptrs.resize(static_cast<size_t>(prog_id) + 32u, nullptr);
        }
        g_d3d.prog_ptrs[prog_id] = p;
    }
    if (!p->valid) return nullptr;
    g_d3d.cached_prog_id = prog_id;
    g_d3d.cached_prog_entry = p;
    return p;
}

bool check_fbo_depth_ready() {
    if (g_d3d.bound_fbo == 0) return true;
    if (g_d3d.fbo_depth_tex_id == 0) return false;
    D3D12TextureEntry* dit = lookup_tex_fast(g_d3d.fbo_depth_tex_id);
    return (dit && dit->is_depth && dit->dsv_slot != 0);
}

PSOKey build_pso_key(uint32_t prog_id, const D3D12RenderState& state,
                     D3D12_PRIMITIVE_TOPOLOGY_TYPE topo_type) {
    PSOKey pso_key{};
    pso_key.prog_id = prog_id;
    pso_key.depth_only_fbo = (g_d3d.bound_fbo != 0) ? 1 : 0;
    pso_key.blend_enabled = state.blend_enabled ? 1 : 0;
    pso_key.depth_test = state.depth_test ? 1 : 0;
    pso_key.depth_write = state.depth_write ? 1 : 0;
    pso_key.stencil_enable = state.stencil_enable ? 1 : 0;
    pso_key.cull_enabled = state.cull_enabled ? 1 : 0;
    pso_key.color_mask = state.color_mask;
    pso_key.topology_type = static_cast<uint8_t>(topo_type);
    pso_key.blend_src_rgb = state.blend_src_rgb;
    pso_key.blend_dst_rgb = state.blend_dst_rgb;
    pso_key.blend_src_alpha = state.blend_src_alpha;
    pso_key.blend_dst_alpha = state.blend_dst_alpha;
    pso_key.blend_eq_rgb = state.blend_eq_rgb;
    pso_key.blend_eq_alpha = state.blend_eq_alpha;
    pso_key.depth_func = state.depth_func;
    if (state.stencil_enable) {
        pso_key.stencil_read_mask = state.stencil_read_mask;
        pso_key.stencil_write_mask = state.stencil_write_mask;
        pso_key.stencil_func = state.stencil_func;
        pso_key.stencil_sfail = state.stencil_sfail;
        pso_key.stencil_zfail = state.stencil_zfail;
        pso_key.stencil_zpass = state.stencil_zpass;
    }
    pso_key.cull_mode = state.cull_mode;
    pso_key.front_face = state.front_face;
    return pso_key;
}

bool resolve_srv_table(const uint32_t bound_textures[8],
                       uint32_t& out_dst_slot,
                       uint32_t& out_sampler_table) {
    auto addr_mode_idx = [](uint32_t wrap) -> uint32_t {
        if (wrap == 0x812F || wrap == 0x2900) return 1u; // GL_CLAMP_TO_EDGE / GL_CLAMP
        if (wrap == 0x8370) return 2u;                   // GL_MIRRORED_REPEAT
        return 0u;                                       // GL_REPEAT
    };
    const bool only_two = ((bound_textures[2] | bound_textures[3] | bound_textures[4] |
                            bound_textures[5] | bound_textures[6] | bound_textures[7]) == 0);
    if (only_two) {
        D3D12TextureEntry* t0 = (bound_textures[0] != 0) ? lookup_tex_fast(bound_textures[0]) : nullptr;
        D3D12TextureEntry* t1 = (bound_textures[1] != 0) ? lookup_tex_fast(bound_textures[1]) : nullptr;
        uint32_t c0 = 0, g0 = 0, c1 = 0, g1 = 0;
        uint32_t sampler_table = 0;
        if (t0) {
            sampler_table = addr_mode_idx(t0->wrap_s) * 3u + addr_mode_idx(t0->wrap_t);
            if (t0->resource && t0->in_srv_state) { c0 = t0->cpu_slot; g0 = t0->srv_gen; }
        } else if (t1) {
            sampler_table = addr_mode_idx(t1->wrap_s) * 3u + addr_mode_idx(t1->wrap_t);
        }
        if (t1 && t1->resource && t1->in_srv_state) {
            c1 = t1->cpu_slot; g1 = t1->srv_gen;
        }
        const uint64_t fkey = (uint64_t(c0 & 0xFFFFu)) |
                              (uint64_t(g0 & 0xFFFFu) << 16) |
                              (uint64_t(c1 & 0xFFFFu) << 32) |
                              (uint64_t(g1 & 0xFFFFu) << 48);
        const size_t hidx = (fkey * 11400714819323198485ULL) >> 54;
        const auto& ce = g_d3d.srv2_cache[hidx];
        if (ce.valid && ce.key == fkey && g0 < 0x10000u && g1 < 0x10000u) {
            out_dst_slot = ce.dst_slot;
            out_sampler_table = sampler_table;
            return true;
        }
        D3D12State::SrvTableKey srv_key{};
        srv_key.slots[0] = c0; srv_key.gens[0] = g0;
        srv_key.slots[1] = c1; srv_key.gens[1] = g1;
        out_sampler_table = sampler_table;
        if (auto cit = g_d3d.srv_table_cache.find(srv_key); cit != g_d3d.srv_table_cache.end()) {
            out_dst_slot = cit->second;
            if (g0 < 0x10000u && g1 < 0x10000u) {
                g_d3d.srv2_cache[hidx] = {fkey, sampler_table, cit->second, true};
            }
            return true;
        }
    }
    D3D12State::SrvTableKey srv_key{};
    uint32_t sampler_table = 0;
    bool have_sampler = false;
    for (uint32_t i = 0; i < kMaxSamplers; i++) {
        uint32_t tid = bound_textures[i];
        uint32_t cslot = 0;
        uint32_t sgen = 0;
        if (tid != 0) {
            if (D3D12TextureEntry* tit = lookup_tex_fast(tid)) {
                if (!have_sampler) {
                    sampler_table = addr_mode_idx(tit->wrap_s) * 3u + addr_mode_idx(tit->wrap_t);
                    have_sampler = true;
                }
                if (tit->resource && tit->in_srv_state) {
                    cslot = tit->cpu_slot;
                    sgen = tit->srv_gen;
                }
            }
        }
        srv_key.slots[i] = cslot;
        srv_key.gens[i] = sgen;
    }
    out_sampler_table = sampler_table;

    if (auto cit = g_d3d.srv_table_cache.find(srv_key); cit != g_d3d.srv_table_cache.end()) {
        out_dst_slot = cit->second;
        return true;
    }

    uint32_t dst_slot = 0;
    bool cache_persistent = false;
    if (g_d3d.persistent_srv_cursor + kMaxSamplers <= kPersistentSrvSlots) {
        dst_slot = g_d3d.persistent_srv_cursor;
        g_d3d.persistent_srv_cursor += kMaxSamplers;
        cache_persistent = true;
    } else {
        g_d3d.need_persistent_srv_gc = true;
        uint32_t& srv_cursor = g_d3d.srv_cursors[g_d3d.frame_index];
        if (srv_cursor + kMaxSamplers > kSrvPerFrame) {
            split_command_list_and_wait();
        }
        if (g_d3d.srv_cursors[g_d3d.frame_index] + kMaxSamplers > kSrvPerFrame) return false;
        dst_slot = kPersistentSrvSlots + g_d3d.frame_index * kSrvPerFrame + g_d3d.srv_cursors[g_d3d.frame_index];
        g_d3d.srv_cursors[g_d3d.frame_index] += kMaxSamplers;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> src_handles;
    std::array<UINT, kMaxSamplers> src_sizes;
    for (uint32_t i = 0; i < kMaxSamplers; i++) {
        src_handles[i] = cpu_srv_handle(srv_key.slots[i]);
        src_sizes[i] = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dst_range_start = gpu_srv_cpu_handle(dst_slot);
    UINT dst_range_size = kMaxSamplers;
    g_d3d.device->CopyDescriptors(
        1, &dst_range_start, &dst_range_size,
        kMaxSamplers, src_handles.data(), src_sizes.data(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (cache_persistent) {
        g_d3d.srv_table_cache.emplace(srv_key, dst_slot);
    }
    out_dst_slot = dst_slot;
    return true;
}

void bind_draw_pipeline_state(ID3D12PipelineState* pso,
                              const D3D12RenderState& state,
                              D3D_PRIMITIVE_TOPOLOGY d3d_topo,
                              D3D12_GPU_VIRTUAL_ADDRESS cb_addr,
                              uint32_t srv_slot,
                              uint32_t sampler_table) {
    const D3D12_VIEWPORT& eff_vp = g_d3d.current_viewport;
    if (!g_d3d.cached_vp_valid ||
        g_d3d.cached_vp.TopLeftX != eff_vp.TopLeftX ||
        g_d3d.cached_vp.TopLeftY != eff_vp.TopLeftY ||
        g_d3d.cached_vp.Width    != eff_vp.Width    ||
        g_d3d.cached_vp.Height   != eff_vp.Height   ||
        g_d3d.cached_vp.MinDepth != eff_vp.MinDepth ||
        g_d3d.cached_vp.MaxDepth != eff_vp.MaxDepth) {
        g_d3d.command_list->RSSetViewports(1, &eff_vp);
        g_d3d.cached_vp = eff_vp;
        g_d3d.cached_vp_valid = true;
    }
    const D3D12_RECT eff_scissor = state.scissor_enabled
        ? g_d3d.current_scissor
        : D3D12_RECT{0, 0, static_cast<LONG>(active_target_width()), static_cast<LONG>(active_target_height())};
    if (!g_d3d.cached_scissor_valid ||
        g_d3d.cached_scissor.left   != eff_scissor.left   ||
        g_d3d.cached_scissor.top    != eff_scissor.top    ||
        g_d3d.cached_scissor.right  != eff_scissor.right  ||
        g_d3d.cached_scissor.bottom != eff_scissor.bottom) {
        g_d3d.command_list->RSSetScissorRects(1, &eff_scissor);
        g_d3d.cached_scissor = eff_scissor;
        g_d3d.cached_scissor_valid = true;
    }
    if (g_d3d.cached_pso != pso) {
        g_d3d.command_list->SetPipelineState(pso);
        g_d3d.cached_pso = pso;
    }
    if (state.stencil_enable && (!g_d3d.cached_stencil_valid || g_d3d.cached_stencil_ref != state.stencil_ref)) {
        g_d3d.command_list->OMSetStencilRef(state.stencil_ref);
        g_d3d.cached_stencil_ref = state.stencil_ref;
        g_d3d.cached_stencil_valid = true;
    }
    if (g_d3d.cached_topo != d3d_topo) {
        g_d3d.command_list->IASetPrimitiveTopology(d3d_topo);
        g_d3d.cached_topo = d3d_topo;
    }
    if (g_d3d.cached_bound_cb_addr != cb_addr) {
        g_d3d.command_list->SetGraphicsRootConstantBufferView(0, cb_addr);
        g_d3d.cached_bound_cb_addr = cb_addr;
    }
    const D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = gpu_srv_gpu_handle(srv_slot);
    if (g_d3d.cached_srv_gpu_ptr != srv_gpu.ptr) {
        g_d3d.command_list->SetGraphicsRootDescriptorTable(1, srv_gpu);
        g_d3d.cached_srv_gpu_ptr = srv_gpu.ptr;
    }
    if (g_d3d.cached_grade_dirty) {
        const float grade_consts[4] = {
            g_d3d.color_grade_brightness,
            g_d3d.color_grade_contrast,
            g_d3d.color_grade_saturation,
            1.0f
        };
        g_d3d.command_list->SetGraphicsRoot32BitConstants(3, 4, grade_consts, 0);
        g_d3d.cached_grade_dirty = false;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu = g_d3d.sampler_heap->GetGPUDescriptorHandleForHeapStart();
    sampler_gpu.ptr += static_cast<SIZE_T>(sampler_table) * 8 * g_d3d.sampler_descriptor_size;
    if (g_d3d.cached_sampler_gpu_ptr != sampler_gpu.ptr) {
        g_d3d.command_list->SetGraphicsRootDescriptorTable(2, sampler_gpu);
        g_d3d.cached_sampler_gpu_ptr = sampler_gpu.ptr;
    }
}

bool ensure_raw_gpu_buffer(uint32_t buf_id,
                           uint64_t ver,
                           uint32_t mutations,
                           const uint8_t* data,
                           size_t size,
                           D3D12_GPU_VIRTUAL_ADDRESS& out_va,
                           uint32_t& out_static_chunk) {
    if (buf_id == 0 || !data || size == 0) return false;
    if (buf_id >= g_d3d.raw_gpu_bufs.size()) {
        g_d3d.raw_gpu_bufs.resize(static_cast<size_t>(buf_id) + 128u);
    }
    auto& entry = g_d3d.raw_gpu_bufs[buf_id];
    if (entry.version == ver && ver != 0) {
        if (entry.static_gpu_va != 0 && entry.static_size >= size) {
            out_va = entry.static_gpu_va;
            out_static_chunk = entry.chunk_idx;
            return true;
        }
        if (entry.dyn_gpu_va != 0 && entry.dyn_frame_seq == g_d3d.frame_seq && entry.dyn_size >= size) {
            out_va = entry.dyn_gpu_va;
            out_static_chunk = UINT32_MAX;
            return true;
        }
    }

    const bool want_static = (mutations <= 4 && entry.dyn_gpu_va == 0 &&
                              (entry.static_gpu_va == 0 || mutations <= 2) &&
                              size <= kStaticVramChunkBytes);
    if (want_static) {
        uint32_t chunk_idx = UINT32_MAX;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_va = 0;
        if (upload_to_static_vram_pool(data, size, 16, chunk_idx, gpu_va)) {
            entry.version = ver;
            entry.chunk_idx = chunk_idx;
            entry.static_gpu_va = gpu_va;
            entry.static_size = size;
            entry.dyn_gpu_va = 0;
            entry.dyn_frame_seq = 0;
            entry.dyn_size = 0;
            out_va = gpu_va;
            out_static_chunk = chunk_idx;
            return true;
        }
    }

    UploadAllocation ua{};
    if (!alloc_frame_upload(size, 16, ua)) return false;
    std::memcpy(ua.cpu_ptr, data, size);
    entry.version = ver;
    entry.chunk_idx = UINT32_MAX;
    entry.static_gpu_va = 0;
    entry.static_size = 0;
    entry.dyn_frame_seq = g_d3d.frame_seq;
    entry.dyn_gpu_va = ua.gpu_addr;
    entry.dyn_size = size;
    out_va = ua.gpu_addr;
    out_static_chunk = UINT32_MAX;
    return true;
}

inline void mix_fnv64(uint64_t& h, uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
}

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
                const D3D12VertexAttrDraw attribs[16]) {
    if (g_d3d.tmp_audit_enabled) ++g_d3d.tmp_draws;
    TmpScopedNsTimer draw_timer(g_d3d.tmp_audit_enabled, &g_d3d.tmp_draw_ns);
    if (!g_d3d.initialized || count == 0) return;
    const D3D12ProgramEntry* prog_ptr = find_program_fast(prog_id);
    if (!prog_ptr) return;
    const D3D12ProgramEntry& prog = *prog_ptr;

    if (!g_d3d.frame_recording) begin_d3d12_frame();
    flush_pending_texture_uploads();
    if (!check_fbo_depth_ready()) return;

    const ResolvedTopo topo = resolve_topology(gl_mode);

    PSOKey pso_key = build_pso_key(prog_id, state, topo.type);
    ID3D12PipelineState* pso = (g_d3d.last_pso_key_valid && pso_key == g_d3d.last_pso_key && g_d3d.cached_pso)
        ? g_d3d.cached_pso
        : get_or_create_pso(pso_key, prog);
    if (!pso) return;

    uint32_t min_vertex_index = indexed ? UINT32_MAX : first_vertex;
    uint32_t max_vertex_index = 0;
    const bool use_index_buffer = indexed || topo.expand_fan;
    g_d3d.scratch_indices.clear();

    if (indexed) {
        if (!index_ptr) return;
        const uint8_t* raw = static_cast<const uint8_t*>(index_ptr);
        const size_t idx_bytes = (index_type == 0x1405) ? 4 : (index_type == 0x1401 ? 1 : 2);
        if (index_available_bytes > 0 && static_cast<size_t>(count) * idx_bytes > index_available_bytes) {
            count = static_cast<uint32_t>(index_available_bytes / idx_bytes);
            if (count == 0) return;
        }
        auto read_idx = [&](uint32_t i) -> uint32_t {
            if (index_type == 0x1405) { uint32_t v = 0; std::memcpy(&v, raw + i * 4, 4); return v; }
            if (index_type == 0x1401) return raw[i];
            uint16_t v = 0; std::memcpy(&v, raw + i * 2, 2); return v;
        };

        if (topo.expand_fan && count >= 3) {
            uint32_t i0 = read_idx(0);
            min_vertex_index = i0;
            max_vertex_index = i0;
            for (uint32_t i = 1; i + 1 < count; i++) {
                uint32_t i1 = read_idx(i);
                uint32_t i2 = read_idx(i + 1);
                min_vertex_index = std::min({min_vertex_index, i1, i2});
                max_vertex_index = std::max({max_vertex_index, i1, i2});
                g_d3d.scratch_indices.push_back(i0);
                g_d3d.scratch_indices.push_back(i1);
                g_d3d.scratch_indices.push_back(i2);
            }
        } else {
            g_d3d.scratch_indices.resize(count);
            uint32_t* dst_idx = g_d3d.scratch_indices.data();
            if (index_type == 0x1403) {
                const auto* src16 = reinterpret_cast<const uint16_t*>(raw);
                for (uint32_t i = 0; i < count; i++) {
                    const uint32_t idx = src16[i];
                    dst_idx[i] = idx;
                    if (idx < min_vertex_index) min_vertex_index = idx;
                    if (idx > max_vertex_index) max_vertex_index = idx;
                }
            } else {
                for (uint32_t i = 0; i < count; i++) {
                    const uint32_t idx = read_idx(i);
                    dst_idx[i] = idx;
                    if (idx < min_vertex_index) min_vertex_index = idx;
                    if (idx > max_vertex_index) max_vertex_index = idx;
                }
            }
        }
        if (min_vertex_index == UINT32_MAX) {
            min_vertex_index = 0;
        } else if (min_vertex_index > 0) {
            uint32_t* dst_idx = g_d3d.scratch_indices.data();
            const size_t n_idx = g_d3d.scratch_indices.size();
            for (size_t i = 0; i < n_idx; i++) dst_idx[i] -= min_vertex_index;
        }
    } else if (topo.expand_fan && count >= 3) {
        max_vertex_index = first_vertex + count - 1;
        for (uint32_t i = 1; i + 1 < count; i++) {
            g_d3d.scratch_indices.push_back(0);
            g_d3d.scratch_indices.push_back(i);
            g_d3d.scratch_indices.push_back(i + 1);
        }
    } else {
        max_vertex_index = first_vertex + count - 1;
    }

    if (use_index_buffer && g_d3d.scratch_indices.empty()) return;

    const uint32_t vert_start = min_vertex_index;
    const uint32_t vert_count = (max_vertex_index >= vert_start) ? (max_vertex_index - vert_start + 1) : 1;
    const uint32_t num_attrs = static_cast<uint32_t>(prog.attribs.size());
    const uint32_t stride_bytes = std::max(16u, num_attrs * 16u);
    const size_t vb_bytes = static_cast<size_t>(vert_count) * stride_bytes;
    const uint32_t cb_aligned = (std::max(16u, uniform_size) + 255u) & ~255u;
    const size_t ib_bytes = use_index_buffer ? (g_d3d.scratch_indices.size() * sizeof(uint32_t)) : 0;
    const size_t worst_case_bytes = vb_bytes + cb_aligned + ib_bytes + 512;

    if ((worst_case_bytes <= kUploadBytesPerFrame &&
         g_d3d.upload_cursors[g_d3d.frame_index] + worst_case_bytes > kUploadBytesPerFrame) ||
        (g_d3d.srv_cursors[g_d3d.frame_index] + kMaxSamplers > kSrvPerFrame)) {
        split_command_list_and_wait();
    }

    UploadAllocation vb_alloc;
    if (!alloc_upload_slice(vb_bytes, 16, vb_alloc)) return;
    if (num_attrs > 0) {
        const size_t total_floats = static_cast<size_t>(vert_count) * num_attrs * 4;
        g_d3d.scratch_verts.resize(total_floats);
        float* scratch_base = g_d3d.scratch_verts.data();
        const size_t v_stride_floats = static_cast<size_t>(num_attrs) * 4;
        static constexpr float kFallbackDef[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        for (uint32_t a = 0; a < num_attrs; a++) {
            int32_t loc = prog.attribs[a].location;
            float* col_dst = scratch_base + static_cast<size_t>(a) * 4;
            if (loc >= 0 && loc < 16 && (active_attrib_mask & (1u << loc))) {
                unpack_attrib_column(attribs[loc], vert_start, vert_count, col_dst, v_stride_floats);
            } else if (loc >= 0 && loc < 16) {
                fill_attrib_const(col_dst, vert_count, v_stride_floats, attribs[loc].default_val);
            } else {
                fill_attrib_const(col_dst, vert_count, v_stride_floats, kFallbackDef);
            }
        }
        std::memcpy(vb_alloc.cpu_ptr, scratch_base, vb_bytes);
    } else {
        std::memset(vb_alloc.cpu_ptr, 0, vb_bytes);
    }

    UploadAllocation cb_alloc;
    if (!alloc_upload_slice(cb_aligned, 256, cb_alloc)) return;
    if (uniform_data && uniform_size > 0) {
        std::memcpy(cb_alloc.cpu_ptr, uniform_data, uniform_size);
    } else {
        std::memset(cb_alloc.cpu_ptr, 0, cb_aligned);
    }
    g_d3d.cached_cb_prog_id = 0;
    g_d3d.cached_cb_addr = 0;

    UploadAllocation ib_alloc{};
    if (use_index_buffer) {
        if (!alloc_upload_slice(ib_bytes, 4, ib_alloc)) return;
        std::memcpy(ib_alloc.cpu_ptr, g_d3d.scratch_indices.data(), ib_bytes);
    }

    uint32_t dst_slot = 0;
    uint32_t sampler_table = 0;
    if (!resolve_srv_table(bound_textures, dst_slot, sampler_table)) return;

    bind_draw_pipeline_state(pso, state, topo.d3d, cb_alloc.gpu_addr, dst_slot, sampler_table);
    g_d3d.last_pso_key = pso_key;
    g_d3d.last_pso_key_valid = true;

    D3D12_VERTEX_BUFFER_VIEW vbv{vb_alloc.gpu_addr, vert_count * stride_bytes, stride_bytes};
    g_d3d.command_list->IASetVertexBuffers(0, 1, &vbv);
    g_d3d.cached_vb_count = 0;

    if (use_index_buffer) {
        if (g_d3d.cached_ib_va != ib_alloc.gpu_addr ||
            g_d3d.cached_ib_size != static_cast<UINT>(ib_bytes) ||
            g_d3d.cached_ib_fmt != DXGI_FORMAT_R32_UINT) {
            D3D12_INDEX_BUFFER_VIEW ibv{ib_alloc.gpu_addr, static_cast<UINT>(ib_bytes), DXGI_FORMAT_R32_UINT};
            g_d3d.command_list->IASetIndexBuffer(&ibv);
            g_d3d.cached_ib_va = ib_alloc.gpu_addr;
            g_d3d.cached_ib_size = static_cast<UINT>(ib_bytes);
            g_d3d.cached_ib_fmt = DXGI_FORMAT_R32_UINT;
        }
        g_d3d.command_list->DrawIndexedInstanced(
            static_cast<UINT>(g_d3d.scratch_indices.size()), 1, 0, 0, 0);
    } else {
        g_d3d.command_list->DrawInstanced(vert_count, 1, 0, 0);
    }
}

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
    const D3D12FastVaoLayout* vao_hint,
    uint32_t eff_uniform_bytes) {
    if (g_d3d.tmp_audit_enabled) ++g_d3d.tmp_draws;
    TmpScopedNsTimer draw_timer(g_d3d.tmp_audit_enabled, &g_d3d.tmp_draw_ns);
    if (!g_d3d.initialized || count == 0) return;
    const D3D12ProgramEntry* prog_ptr = find_program_fast(prog_id);
    if (!prog_ptr) return;
    const D3D12ProgramEntry& prog = *prog_ptr;

    const uint32_t num_attrs = static_cast<uint32_t>(prog.attribs.size());
    if (num_attrs == 0 || (indexed && index_type != 0x1403 && index_type != 0x1405)) {
        D3D12VertexAttrDraw legacy_attrs[16]{};
        for (uint32_t i = 0; i < 16; ++i) {
            legacy_attrs[i].size = static_cast<int32_t>(attrs[i].size);
            legacy_attrs[i].type = attrs[i].type;
            legacy_attrs[i].normalized = (attrs[i].normalized != 0);
            legacy_attrs[i].stride = static_cast<int32_t>(attrs[i].stride);
            for (int c = 0; c < 4; ++c) legacy_attrs[i].default_val[c] = attrs[i].default_val[c];
            if (attrs[i].enabled && attrs[i].vbo_data && attrs[i].pointer_offset < attrs[i].vbo_size) {
                legacy_attrs[i].base_ptr = attrs[i].vbo_data + attrs[i].pointer_offset;
                legacy_attrs[i].available_bytes = attrs[i].vbo_size - static_cast<size_t>(attrs[i].pointer_offset);
            }
        }
        const void* ebo_ptr = (indexed && ebo.ebo_data && ebo.index_byte_offset < ebo.ebo_size)
            ? (ebo.ebo_data + ebo.index_byte_offset) : nullptr;
        const size_t ebo_avail = ebo_ptr ? (ebo.ebo_size - static_cast<size_t>(ebo.index_byte_offset)) : 0;
        draw_d3d12(prog_id, state, gl_mode, first_vertex, count, indexed, index_type,
                   ebo_ptr, ebo_avail, uniform_data, uniform_bytes, bound_tex_ids,
                   active_attr_mask, legacy_attrs);
        return;
    }

    if (!g_d3d.frame_recording) begin_d3d12_frame();
    flush_pending_texture_uploads();
    if (!check_fbo_depth_ready()) return;

    const ResolvedTopo topo = resolve_topology(gl_mode);
    const size_t idx_bytes = (index_type == 0x1405) ? 4 : 2;
    uint32_t draw_count = count;
    if (indexed) {
        if (!ebo.ebo_data || ebo.index_byte_offset >= ebo.ebo_size ||
            (ebo.index_byte_offset % idx_bytes) != 0) {
            return;
        }
        const size_t avail_bytes = ebo.ebo_size - static_cast<size_t>(ebo.index_byte_offset);
        const uint32_t avail_idx = static_cast<uint32_t>(avail_bytes / idx_bytes);
        draw_count = std::min<uint32_t>(draw_count, avail_idx);
        if (draw_count == 0) return;
    }

    // 1. Determine whether all shader attributes can bind directly via native DXGI input slots
    bool can_native = false;
    uint8_t native_mask = 0;
    std::array<uint8_t, 8> native_fmts{};
    std::array<uint32_t, 8> attr_eff_strides{};
    std::array<uint32_t, 8> attr_nat_strides{};

    D3D12State::FastVaoEntry* vao_slot_ptr = nullptr;
    bool vao_hit = false;
    if (vao_hint && vao_hint->vao_gen != 0) {
        const size_t vslot = ((uint64_t(prog_id) * 0x9E3779B97F4A7C15ULL) ^
                              (uint64_t(vao_hint->vao_id) * 0xBF58476D1CE4E5B9ULL) ^
                              vao_hint->vao_gen) & (D3D12State::kFastVaoCacheSize - 1);
        vao_slot_ptr = &g_d3d.fast_vao_cache[vslot];
        if (vao_slot_ptr->valid &&
            vao_slot_ptr->prog_id == prog_id &&
            vao_slot_ptr->vao_id == vao_hint->vao_id &&
            vao_slot_ptr->vao_gen == vao_hint->vao_gen &&
            vao_slot_ptr->default_attr_seq == vao_hint->default_attr_seq) {
            vao_hit = true;
            can_native = vao_slot_ptr->can_native;
            native_mask = vao_slot_ptr->native_mask;
            native_fmts = vao_slot_ptr->native_fmts;
            attr_eff_strides = vao_slot_ptr->attr_eff_strides;
            attr_nat_strides = vao_slot_ptr->attr_nat_strides;
        }
    }

    if (!vao_hit) {
        can_native = (num_attrs <= 8);
        for (uint32_t a = 0; a < num_attrs && can_native; ++a) {
            const int32_t loc = prog.attribs[a].location;
            if (loc < 0 || loc >= 16 || (active_attr_mask & (1u << loc)) == 0) {
                can_native = false;
                break;
            }
            const D3D12FastAttrRef& fa = attrs[loc];
            if (fa.vbo_id == 0 || !fa.vbo_data || fa.pointer_offset >= fa.vbo_size) {
                can_native = false;
                break;
            }
            const DXGI_FORMAT fmt = map_gl_attrib_to_dxgi(fa.size, fa.type, fa.normalized);
            if (fmt == DXGI_FORMAT_UNKNOWN) {
                can_native = false;
                break;
            }
            if (fa.size < 4) {
                const bool def_ok = (fa.default_val[3] == 1.0f) &&
                                    (fa.size >= 3 || fa.default_val[2] == 0.0f) &&
                                    (fa.size >= 2 || fa.default_val[1] == 0.0f);
                if (!def_ok) {
                    can_native = false;
                    break;
                }
            }
            const uint32_t nat_stride = gl_attrib_natural_stride(fa.size, fa.type);
            const uint32_t eff_stride = (fa.stride > 0) ? fa.stride : nat_stride;
            uint32_t req_align = 1u;
            if (fa.type == 0x1406) req_align = 4u;
            else if (fa.type == 0x1402 || fa.type == 0x1403 || fa.type == 0x140B || fa.type == 0x8D61) req_align = 2u;
            if (eff_stride == 0 || eff_stride > 2048u ||
                (eff_stride % req_align) != 0 || (fa.pointer_offset % req_align) != 0) {
                can_native = false;
                break;
            }
            native_mask |= static_cast<uint8_t>(1u << a);
            native_fmts[a] = static_cast<uint8_t>(fmt);
            attr_eff_strides[a] = eff_stride;
            attr_nat_strides[a] = nat_stride;
        }
        if (vao_slot_ptr) {
            vao_slot_ptr->prog_id = prog_id;
            vao_slot_ptr->vao_id = vao_hint->vao_id;
            vao_slot_ptr->vao_gen = vao_hint->vao_gen;
            vao_slot_ptr->default_attr_seq = vao_hint->default_attr_seq;
            vao_slot_ptr->can_native = can_native;
            vao_slot_ptr->native_mask = native_mask;
            vao_slot_ptr->native_fmts = native_fmts;
            vao_slot_ptr->attr_eff_strides = attr_eff_strides;
            vao_slot_ptr->attr_nat_strides = attr_nat_strides;
            vao_slot_ptr->valid = true;
        }
    }

    if (can_native && !indexed) {
        for (uint32_t a = 0; a < num_attrs; ++a) {
            const int32_t loc = prog.attribs[a].location;
            const D3D12FastAttrRef& fa = attrs[loc];
            const size_t avail_v = fa.vbo_size - static_cast<size_t>(fa.pointer_offset);
            const size_t need_v = static_cast<size_t>(first_vertex + draw_count - 1) * attr_eff_strides[a] + attr_nat_strides[a];
            if (need_v > avail_v) {
                can_native = false;
                break;
            }
        }
    }

    const uint8_t depth_only = (g_d3d.bound_fbo != 0) ? 1u : 0u;
    const uint8_t eff_mask = can_native ? native_mask : 0u;
    ID3D12PipelineState* pso = nullptr;
    PSOKey pso_key{};
    bool used_fast_state_pso = false;
    D3D12State::FastStatePsoEntry* sp_slot = nullptr;

    if (vao_hint && vao_hint->state_seq != 0) {
        uint64_t sh = (uint64_t(prog_id) * 0x9E3779B97F4A7C15ULL) ^
                      (vao_hint->state_seq * 0xBF58476D1CE4E5B9ULL) ^
                      (uint64_t(depth_only) << 8) ^
                      (uint64_t(topo.type) << 4) ^
                      uint64_t(eff_mask);
        if (can_native) {
            uint64_t fmts_u64 = 0;
            std::memcpy(&fmts_u64, native_fmts.data(), 8);
            sh ^= fmts_u64 * 0x94D049BB133111EBULL;
        }
        sp_slot = &g_d3d.fast_state_pso_cache[sh & (D3D12State::kFastStatePsoCacheSize - 1)];
        if (sp_slot->valid &&
            sp_slot->prog_id == prog_id &&
            sp_slot->state_seq == vao_hint->state_seq &&
            sp_slot->depth_only_fbo == depth_only &&
            sp_slot->topo_type == static_cast<uint8_t>(topo.type) &&
            sp_slot->attr_native_mask == eff_mask &&
            (!can_native || sp_slot->attr_dxgi_formats == native_fmts)) {
            pso = sp_slot->pso;
            used_fast_state_pso = true;
        }
    }

    if (!pso) {
        pso_key = build_pso_key(prog_id, state, topo.type);
        if (can_native) {
            pso_key.attr_native_mask = native_mask;
            pso_key.attr_dxgi_formats = native_fmts;
        }
        pso = (g_d3d.last_pso_key_valid && pso_key == g_d3d.last_pso_key && g_d3d.cached_pso)
            ? g_d3d.cached_pso
            : get_or_create_pso(pso_key, prog);
        if (!pso) return;
        if (sp_slot && vao_hint && vao_hint->state_seq != 0) {
            sp_slot->prog_id = prog_id;
            sp_slot->state_seq = vao_hint->state_seq;
            sp_slot->depth_only_fbo = depth_only;
            sp_slot->topo_type = static_cast<uint8_t>(topo.type);
            sp_slot->attr_native_mask = eff_mask;
            if (can_native) sp_slot->attr_dxgi_formats = native_fmts;
            else sp_slot->attr_dxgi_formats.fill(0);
            sp_slot->pso = pso;
            sp_slot->valid = true;
        }
    }

    // 2. If non-native, compute/lookup [min_v..max_v] bounds (cached for static EBOs)
    uint32_t vert_start = first_vertex;
    uint32_t vert_count = draw_count;
    if (!can_native && indexed) {
        const uint8_t* raw = ebo.ebo_data + ebo.index_byte_offset;
        uint32_t min_v = UINT32_MAX;
        uint32_t max_v = 0;
        bool bounds_found = false;
        D3D12State::EboBoundsKey bkey{};
        const bool cache_bounds = (ebo.ebo_mutations <= 4 && ebo.ebo_version != 0);
        if (cache_bounds) {
            bkey = {ebo.ebo_id, ebo.ebo_version, ebo.index_byte_offset, draw_count, index_type};
            if (auto bit = g_d3d.ebo_bounds_cache.find(bkey); bit != g_d3d.ebo_bounds_cache.end()) {
                min_v = static_cast<uint32_t>(bit->second >> 32);
                max_v = static_cast<uint32_t>(bit->second & 0xFFFFFFFFu);
                bounds_found = true;
            }
        }
        if (!bounds_found) {
            if (index_type == 0x1405) {
                const auto* src32 = reinterpret_cast<const uint32_t*>(raw);
                for (uint32_t i = 0; i < draw_count; ++i) {
                    const uint32_t idx = src32[i];
                    if (idx < min_v) min_v = idx;
                    if (idx > max_v) max_v = idx;
                }
            } else {
                const auto* src16 = reinterpret_cast<const uint16_t*>(raw);
                for (uint32_t i = 0; i < draw_count; ++i) {
                    const uint32_t idx = src16[i];
                    if (idx < min_v) min_v = idx;
                    if (idx > max_v) max_v = idx;
                }
            }
            if (min_v == UINT32_MAX) min_v = 0;
            if (cache_bounds) {
                g_d3d.ebo_bounds_cache.emplace(bkey, (uint64_t(min_v) << 32) | max_v);
            }
        }
        if (min_v > static_cast<uint32_t>(INT32_MAX)) return;
        vert_start = min_v;
        vert_count = (max_v >= min_v) ? (max_v - min_v + 1) : 1;
    }

    // 3. Stage VB(s), EBO, CB, and SRV table cleanly (retrying once if a ring split occurs)
    D3D12_VERTEX_BUFFER_VIEW vbvs[8]{};
    D3D12_GPU_VIRTUAL_ADDRESS ebo_gpu_va = 0;
    D3D12_GPU_VIRTUAL_ADDRESS cb_addr = 0;
    uint32_t srv_slot = 0;
    uint32_t sampler_table = 0;

    const uint32_t copy_bytes = (eff_uniform_bytes > 0u && eff_uniform_bytes < uniform_bytes)
        ? eff_uniform_bytes : uniform_bytes;
    const uint32_t cb_aligned = (std::max(16u, copy_bytes) + 255u) & ~255u;
    const uint32_t conv_stride_bytes = std::max(16u, num_attrs * 16u);

    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t seq0 = g_d3d.frame_seq;

        if (can_native) {
            bool ok = true;
            uint32_t last_vbo_id = 0;
            D3D12_GPU_VIRTUAL_ADDRESS last_vbo_va = 0;
            for (uint32_t a = 0; a < num_attrs; ++a) {
                const int32_t loc = prog.attribs[a].location;
                const D3D12FastAttrRef& fa = attrs[loc];
                D3D12_GPU_VIRTUAL_ADDRESS vbo_va = last_vbo_va;
                if (fa.vbo_id != last_vbo_id) {
                    uint32_t chunk_idx = UINT32_MAX;
                    if (!ensure_raw_gpu_buffer(fa.vbo_id, fa.vbo_version, fa.vbo_mutations,
                                               fa.vbo_data, fa.vbo_size, vbo_va, chunk_idx)) {
                        ok = false;
                        break;
                    }
                    last_vbo_id = fa.vbo_id;
                    last_vbo_va = vbo_va;
                }
                vbvs[a].BufferLocation = vbo_va + fa.pointer_offset;
                vbvs[a].SizeInBytes = static_cast<UINT>(fa.vbo_size - static_cast<size_t>(fa.pointer_offset));
                vbvs[a].StrideInBytes = attr_eff_strides[a];
            }
            if (!ok) return;
        } else {
            bool all_vbos_static = (!indexed || ebo.ebo_mutations <= 4);
            D3D12State::ConvertedVbKey ck{};
            uint64_t sig = 14695981039346656037ULL;
            mix_fnv64(sig, prog_id);
            mix_fnv64(sig, vert_start);
            bool have_primary = false;
            for (uint32_t a = 0; a < num_attrs; ++a) {
                const int32_t loc = prog.attribs[a].location;
                mix_fnv64(sig, static_cast<uint64_t>(loc + 1));
                if (loc >= 0 && loc < 16 && (active_attr_mask & (1u << loc))) {
                    const D3D12FastAttrRef& fa = attrs[loc];
                    if (fa.vbo_id == 0 || fa.vbo_mutations > 4) all_vbos_static = false;
                    if (!have_primary && fa.vbo_id != 0) {
                        ck.primary_vbo = fa.vbo_id;
                        ck.primary_ver = fa.vbo_version;
                        ck.primary_ptr = fa.pointer_offset;
                        have_primary = true;
                    }
                    mix_fnv64(sig, fa.vbo_id);
                    mix_fnv64(sig, fa.vbo_version);
                    mix_fnv64(sig, fa.pointer_offset);
                    mix_fnv64(sig, (uint64_t(fa.size) << 32) | fa.type);
                    mix_fnv64(sig, (uint64_t(fa.stride) << 8) | fa.normalized);
                } else if (loc >= 0 && loc < 16) {
                    const D3D12FastAttrRef& fa = attrs[loc];
                    uint64_t d01 = 0, d23 = 0;
                    std::memcpy(&d01, &fa.default_val[0], 8);
                    std::memcpy(&d23, &fa.default_val[2], 8);
                    mix_fnv64(sig, d01);
                    mix_fnv64(sig, d23);
                }
            }
            if (!have_primary) all_vbos_static = false;
            ck.vert_count = vert_count;
            ck.active_mask = active_attr_mask;
            ck.layout_sig = sig;

            bool got_conv = false;
            if (all_vbos_static) {
                if (auto cit = g_d3d.converted_vb_cache.find(ck); cit != g_d3d.converted_vb_cache.end()) {
                    vbvs[0].BufferLocation = cit->second.gpu_va;
                    vbvs[0].SizeInBytes = vert_count * cit->second.stride_bytes;
                    vbvs[0].StrideInBytes = cit->second.stride_bytes;
                    got_conv = true;
                }
            }
            if (!got_conv) {
                const size_t vb_bytes = static_cast<size_t>(vert_count) * conv_stride_bytes;
                const size_t total_floats = static_cast<size_t>(vert_count) * num_attrs * 4;
                g_d3d.scratch_verts.resize(total_floats);
                float* scratch_base = g_d3d.scratch_verts.data();
                const size_t v_stride_floats = static_cast<size_t>(num_attrs) * 4;
                static constexpr float kFallbackDef[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (uint32_t a = 0; a < num_attrs; ++a) {
                    const int32_t loc = prog.attribs[a].location;
                    float* col_dst = scratch_base + static_cast<size_t>(a) * 4;
                    if (loc >= 0 && loc < 16 && (active_attr_mask & (1u << loc))) {
                        const D3D12FastAttrRef& fa = attrs[loc];
                        D3D12VertexAttrDraw da{};
                        da.size = static_cast<int32_t>(fa.size);
                        da.type = fa.type;
                        da.normalized = (fa.normalized != 0);
                        da.stride = static_cast<int32_t>(fa.stride);
                        for (int c = 0; c < 4; ++c) da.default_val[c] = fa.default_val[c];
                        if (fa.vbo_data && fa.pointer_offset < fa.vbo_size) {
                            da.base_ptr = fa.vbo_data + fa.pointer_offset;
                            da.available_bytes = fa.vbo_size - static_cast<size_t>(fa.pointer_offset);
                        }
                        unpack_attrib_column(da, vert_start, vert_count, col_dst, v_stride_floats);
                    } else if (loc >= 0 && loc < 16) {
                        fill_attrib_const(col_dst, vert_count, v_stride_floats, attrs[loc].default_val);
                    } else {
                        fill_attrib_const(col_dst, vert_count, v_stride_floats, kFallbackDef);
                    }
                }
                uint32_t chunk_idx = UINT32_MAX;
                D3D12_GPU_VIRTUAL_ADDRESS conv_va = 0;
                if (all_vbos_static && upload_to_static_vram_pool(scratch_base, vb_bytes, 16, chunk_idx, conv_va)) {
                    g_d3d.converted_vb_cache.emplace(ck, D3D12State::ConvertedVbEntry{chunk_idx, conv_va, conv_stride_bytes, vert_count});
                } else {
                    UploadAllocation vb_alloc{};
                    if (!alloc_frame_upload(vb_bytes, 16, vb_alloc)) return;
                    std::memcpy(vb_alloc.cpu_ptr, scratch_base, vb_bytes);
                    conv_va = vb_alloc.gpu_addr;
                }
                vbvs[0].BufferLocation = conv_va;
                vbvs[0].SizeInBytes = static_cast<UINT>(vb_bytes);
                vbvs[0].StrideInBytes = conv_stride_bytes;
            }
        }

        if (indexed) {
            uint32_t ebo_chunk = UINT32_MAX;
            if (!ensure_raw_gpu_buffer(ebo.ebo_id, ebo.ebo_version, ebo.ebo_mutations,
                                       ebo.ebo_data, ebo.ebo_size, ebo_gpu_va, ebo_chunk)) {
                return;
            }
        }

        if (g_d3d.cached_cb_prog_id == prog_id &&
            g_d3d.cached_cb_ver == uniform_version &&
            g_d3d.cached_cb_size >= cb_aligned &&
            g_d3d.cached_cb_addr != 0) {
            cb_addr = g_d3d.cached_cb_addr;
        } else {
            UploadAllocation cb_alloc{};
            if (!alloc_frame_upload(cb_aligned, 256, cb_alloc)) return;
            if (uniform_data && copy_bytes > 0) {
                std::memcpy(cb_alloc.cpu_ptr, uniform_data, copy_bytes);
            } else {
                std::memset(cb_alloc.cpu_ptr, 0, cb_aligned);
            }
            cb_addr = cb_alloc.gpu_addr;
            g_d3d.cached_cb_prog_id = prog_id;
            g_d3d.cached_cb_ver = uniform_version;
            g_d3d.cached_cb_size = cb_aligned;
            g_d3d.cached_cb_addr = cb_addr;
        }

        if (!resolve_srv_table(bound_tex_ids, srv_slot, sampler_table)) return;
        if (g_d3d.frame_seq == seq0) break;
    }

    // 4. Transition any dirty static VRAM chunks to GENERIC_READ, bind state, and draw
    if (g_d3d.static_chunk_copy_dest_mask != 0 || g_d3d.static_chunk_over64_dirty) {
        for (uint32_t ci = 0; ci < static_cast<uint32_t>(g_d3d.static_vram_chunks.size()); ++ci) {
            transition_static_chunk(g_d3d.static_vram_chunks[ci],
                                    D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        g_d3d.static_chunk_copy_dest_mask = 0;
        g_d3d.static_chunk_over64_dirty = false;
    }

    bind_draw_pipeline_state(pso, state, topo.d3d, cb_addr, srv_slot, sampler_table);
    if (used_fast_state_pso) {
        g_d3d.last_pso_key_valid = false;
    } else {
        g_d3d.last_pso_key = pso_key;
        g_d3d.last_pso_key_valid = true;
    }

    const UINT vb_cnt = can_native ? num_attrs : 1u;
    bool vb_same = (g_d3d.cached_vb_count == vb_cnt);
    if (vb_same) {
        for (UINT vi = 0; vi < vb_cnt; ++vi) {
            if (g_d3d.cached_vbvs[vi].BufferLocation != vbvs[vi].BufferLocation ||
                g_d3d.cached_vbvs[vi].SizeInBytes    != vbvs[vi].SizeInBytes ||
                g_d3d.cached_vbvs[vi].StrideInBytes  != vbvs[vi].StrideInBytes) {
                vb_same = false;
                break;
            }
        }
    }
    if (!vb_same) {
        g_d3d.command_list->IASetVertexBuffers(0, vb_cnt, vbvs);
        g_d3d.cached_vb_count = vb_cnt;
        for (UINT vi = 0; vi < vb_cnt && vi < 8u; ++vi) {
            g_d3d.cached_vbvs[vi] = vbvs[vi];
        }
    }

    if (indexed) {
        const DXGI_FORMAT ib_fmt = (index_type == 0x1405) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        const UINT ib_size = static_cast<UINT>(ebo.ebo_size);
        if (g_d3d.cached_ib_va != ebo_gpu_va ||
            g_d3d.cached_ib_size != ib_size ||
            g_d3d.cached_ib_fmt != ib_fmt) {
            D3D12_INDEX_BUFFER_VIEW ibv{ebo_gpu_va, ib_size, ib_fmt};
            g_d3d.command_list->IASetIndexBuffer(&ibv);
            g_d3d.cached_ib_va = ebo_gpu_va;
            g_d3d.cached_ib_size = ib_size;
            g_d3d.cached_ib_fmt = ib_fmt;
        }
        const UINT start_idx = static_cast<UINT>(ebo.index_byte_offset / idx_bytes);
        const INT base_vtx = can_native ? 0 : -static_cast<INT>(vert_start);
        g_d3d.command_list->DrawIndexedInstanced(draw_count, 1, start_idx, base_vtx, 0);
    } else {
        g_d3d.command_list->DrawInstanced(draw_count, 1, can_native ? first_vertex : 0, 0);
    }
}

void notify_d3d12_buffer_deleted(uint32_t buf_id) {
    if (!g_d3d.initialized || buf_id == 0) return;
    const auto bd_t0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (buf_id < g_d3d.raw_gpu_bufs.size()) {
        g_d3d.raw_gpu_bufs[buf_id] = D3D12State::RawGpuBufEntry{};
    }
    std::erase_if(g_d3d.ebo_bounds_cache, [buf_id](const auto& kv) {
        return kv.first.ebo_id == buf_id;
    });
    std::erase_if(g_d3d.converted_vb_cache, [buf_id](const auto& kv) {
        return kv.first.primary_vbo == buf_id;
    });
    if (g_d3d.tmp_audit_enabled) {
        ++g_d3d.tmp_buf_del_cnt;
        g_d3d.tmp_buf_del_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - bd_t0).count());
    }
}

void end_d3d12_frame_and_present(uint32_t sync_interval) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) {
        begin_d3d12_frame();
    }

    if (g_d3d.bound_fbo != 0) {
        bind_d3d12_framebuffer(0, 0, 0, g_d3d.width, g_d3d.height);
    }

    flush_pending_texture_uploads();

    if (g_d3d.video_overlay_active && g_d3d.video_width > 0 && g_d3d.video_height > 0) {
        auto ensure_video_pso = [&]() -> bool {
            if (g_d3d.video_pso) return true;
            static constexpr const char* kVideoVS = R"(
                cbuffer VideoCB : register(b0) { float4 scale_xy; };
                struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
                VSOut main(uint id : SV_VertexID) {
                    float2 pos[4] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1) };
                    float2 uv[4]  = { float2(0, 1),   float2(0, 0),  float2(1, 1),  float2(1, 0) };
                    VSOut o;
                    o.pos = float4(pos[id] * scale_xy.xy, 0.0f, 1.0f);
                    o.uv  = uv[id];
                    return o;
                }
            )";
            static constexpr const char* kVideoPS = R"(
                Texture2D video_tex : register(t0);
                SamplerState video_samp : register(s0);
                struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
                float4 main(VSOut i) : SV_TARGET {
                    return video_tex.Sample(video_samp, i.uv);
                }
            )";
            ComPtr<ID3DBlob> vs_blob, ps_blob, err_blob;
            if (FAILED(D3DCompile(kVideoVS, std::strlen(kVideoVS), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob, &err_blob))) {
                return false;
            }
            if (FAILED(D3DCompile(kVideoPS, std::strlen(kVideoPS), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob, &err_blob))) {
                return false;
            }
            D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = g_d3d.root_signature.Get();
            desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
            desc.PS = {ps_blob->GetBufferPointer(), ps_blob->GetBufferSize()};
            desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.SampleMask = UINT_MAX;
            desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            desc.RasterizerState.DepthClipEnable = FALSE;
            desc.DepthStencilState.DepthEnable = FALSE;
            desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            desc.NumRenderTargets = 1;
            desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            desc.SampleDesc.Count = 1;
            return SUCCEEDED(g_d3d.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&g_d3d.video_pso)));
        };

        auto vtit = g_d3d.textures.find(D3D12State::kVideoOverlayTexId);
        if (vtit != g_d3d.textures.end() && vtit->second.resource && vtit->second.in_srv_state && ensure_video_pso()) {
            if (g_d3d.upload_cursors[g_d3d.frame_index] + 256 > kUploadBytesPerFrame ||
                g_d3d.srv_cursors[g_d3d.frame_index] + kMaxSamplers > kSrvPerFrame) {
                split_command_list_and_wait();
            }

            UploadAllocation cb_alloc;
            if (alloc_upload_slice(256, 256, cb_alloc)) {
                float win_aspect = static_cast<float>(g_d3d.width) / static_cast<float>(std::max(1u, g_d3d.height));
                float vid_aspect = static_cast<float>(g_d3d.video_width) / static_cast<float>(std::max(1u, g_d3d.video_height));
                float sx = (vid_aspect < win_aspect) ? (vid_aspect / win_aspect) : 1.0f;
                float sy = (vid_aspect > win_aspect) ? (win_aspect / vid_aspect) : 1.0f;
                float cb_vals[4] = {sx, sy, 0.0f, 0.0f};
                std::memcpy(cb_alloc.cpu_ptr, cb_vals, sizeof(cb_vals));

                uint32_t& srv_cursor = g_d3d.srv_cursors[g_d3d.frame_index];
                uint32_t dst_slot = kPersistentSrvSlots + g_d3d.frame_index * kSrvPerFrame + srv_cursor;
                srv_cursor += kMaxSamplers;

                std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> src_handles;
                std::array<UINT, kMaxSamplers> src_sizes;
                for (uint32_t i = 0; i < kMaxSamplers; i++) {
                    src_handles[i] = cpu_srv_handle(i == 0 ? vtit->second.cpu_slot : 0);
                    src_sizes[i] = 1;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE dst_start = gpu_srv_cpu_handle(dst_slot);
                UINT dst_size = kMaxSamplers;
                g_d3d.device->CopyDescriptors(
                    1, &dst_start, &dst_size,
                    kMaxSamplers, src_handles.data(), src_sizes.data(),
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

                constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                g_d3d.command_list->ClearRenderTargetView(current_rtv(), kBlack, 0, nullptr);
                g_d3d.cleared_this_frame = true;

                D3D12_VIEWPORT full_vp{0.0f, 0.0f, static_cast<float>(g_d3d.width), static_cast<float>(g_d3d.height), 0.0f, 1.0f};
                D3D12_RECT full_sc{0, 0, static_cast<LONG>(g_d3d.width), static_cast<LONG>(g_d3d.height)};
                g_d3d.command_list->RSSetViewports(1, &full_vp);
                g_d3d.command_list->RSSetScissorRects(1, &full_sc);
                g_d3d.command_list->SetPipelineState(g_d3d.video_pso.Get());
                g_d3d.command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                g_d3d.command_list->IASetVertexBuffers(0, 0, nullptr);
                g_d3d.cached_vb_count = 0;
                g_d3d.command_list->SetGraphicsRootConstantBufferView(0, cb_alloc.gpu_addr);
                g_d3d.command_list->SetGraphicsRootDescriptorTable(1, gpu_srv_gpu_handle(dst_slot));

                // Clamp/Clamp sampler table (index 1)
                D3D12_GPU_DESCRIPTOR_HANDLE samp_gpu = g_d3d.sampler_heap->GetGPUDescriptorHandleForHeapStart();
                samp_gpu.ptr += static_cast<SIZE_T>(1) * 8 * g_d3d.sampler_descriptor_size;
                g_d3d.command_list->SetGraphicsRootDescriptorTable(2, samp_gpu);
                g_d3d.command_list->DrawInstanced(4, 1, 0, 0);
            }
        }
    }

    if (!g_d3d.cleared_this_frame) {
        constexpr float kDefaultBg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        g_d3d.command_list->ClearRenderTargetView(current_rtv(), kDefaultBg, 0, nullptr);
    }

    // Render Dear ImGui in-game menus (F2 Settings, F5 Trainer, Notices, FPS Counter) only when active
    if (!g_d3d.loading_bar_active && lcs::ui_overlay::has_visible_overlay()) {
        const D3D12_VIEWPORT ui_vp{0.0f, 0.0f, static_cast<float>(g_d3d.width), static_cast<float>(g_d3d.height), 0.0f, 1.0f};
        const D3D12_RECT ui_sc{0, 0, static_cast<LONG>(g_d3d.width), static_cast<LONG>(g_d3d.height)};
        g_d3d.command_list->RSSetViewports(1, &ui_vp);
        g_d3d.command_list->RSSetScissorRects(1, &ui_sc);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = current_rtv();
        g_d3d.command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        lcs::ui_overlay::render_d3d12(g_d3d.command_list.Get());
    }

    std::string pending_shot;
    ComPtr<ID3D12Resource> shot_rb;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT shot_fp{};
    UINT64 shot_total_bytes = 0;
    uint32_t shot_w = 0;
    uint32_t shot_h = 0;
    bool transitioned_to_present = false;

    if (!g_d3d.screenshot_path.empty() && g_d3d.render_targets[g_d3d.frame_index]) {
        std::swap(pending_shot, g_d3d.screenshot_path);
        const D3D12_RESOURCE_DESC rt_desc = g_d3d.render_targets[g_d3d.frame_index]->GetDesc();
        UINT num_rows = 0;
        UINT64 row_bytes = 0;
        g_d3d.device->GetCopyableFootprints(&rt_desc, 0, 1, 0, &shot_fp, &num_rows, &row_bytes, &shot_total_bytes);

        D3D12_HEAP_PROPERTIES rb_heap{};
        rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb_desc{};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = shot_total_bytes;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(g_d3d.device->CreateCommittedResource(
                &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&shot_rb)))) {
            shot_w = static_cast<uint32_t>(rt_desc.Width);
            shot_h = rt_desc.Height;

            D3D12_RESOURCE_BARRIER b_copy{};
            b_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b_copy.Transition.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
            b_copy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_d3d.command_list->ResourceBarrier(1, &b_copy);

            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src_loc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = shot_rb.Get();
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst_loc.PlacedFootprint = shot_fp;
            g_d3d.command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

            D3D12_RESOURCE_BARRIER b_pres{};
            b_pres.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b_pres.Transition.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
            b_pres.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b_pres.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            b_pres.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_d3d.command_list->ResourceBarrier(1, &b_pres);
            transitioned_to_present = true;
        }
    }

    if (!transitioned_to_present) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_d3d.command_list->ResourceBarrier(1, &barrier);
    }

    const auto ex_t0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    HRESULT hr = g_d3d.command_list->Close();
    g_d3d.frame_recording = false;

    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {g_d3d.command_list.Get()};
        g_d3d.command_queue->ExecuteCommandLists(1, lists);
    }
    if (g_d3d.tmp_audit_enabled) {
        g_d3d.tmp_exec_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ex_t0).count());
    }

    UINT present_flags = (sync_interval == 0 && g_d3d.tearing_supported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const auto p_t0 = g_d3d.tmp_audit_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    g_d3d.swap_chain->Present(sync_interval, present_flags);
    if (g_d3d.tmp_audit_enabled) {
        const auto p_t1 = std::chrono::steady_clock::now();
        g_d3d.tmp_pres_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(p_t1 - p_t0).count());
        const double wall_ms = (g_d3d.tmp_frame_start_ns > 0)
            ? (static_cast<double>(p_t1.time_since_epoch().count() - g_d3d.tmp_frame_start_ns) / 1e6) : 0.0;
        const double wait_ms = static_cast<double>(g_d3d.tmp_wait_ns) / 1e6;
        const double pres_ms = static_cast<double>(g_d3d.tmp_pres_ns) / 1e6;
        const double pso_ms  = static_cast<double>(g_d3d.tmp_pso_ns) / 1e6;
        const double tex_ms  = static_cast<double>(g_d3d.tmp_tex_ns) / 1e6;
        const double bdel_ms = static_cast<double>(g_d3d.tmp_buf_del_ns) / 1e6;
        const double draw_ms = static_cast<double>(g_d3d.tmp_draw_ns) / 1e6;
        const double ps2_ms  = static_cast<double>(g_d3d.tmp_ps2_ns) / 1e6;
        const double exec_ms = static_cast<double>(g_d3d.tmp_exec_ns) / 1e6;
        const double guest_ms = wall_ms - wait_ms - pres_ms - draw_ms - ps2_ms - exec_ms;
        if (g_d3d.tmp_draws > 500) {
            ++g_d3d.tmp_sum_frames;
            g_d3d.tmp_sum_wall += wall_ms; g_d3d.tmp_sum_wait += wait_ms; g_d3d.tmp_sum_pres += pres_ms;
            g_d3d.tmp_sum_pso  += pso_ms;  g_d3d.tmp_sum_tex  += tex_ms;  g_d3d.tmp_sum_bdel += bdel_ms;
            if (wall_ms > 10.5 || g_d3d.tmp_pso_cnt > 0 || g_d3d.tmp_split_cnt > 0 || (g_d3d.tmp_sum_frames % 250 == 0)) {
                std::cout << "  [TMP_D3D] f=" << g_d3d.frame_seq << " draws=" << g_d3d.tmp_draws
                          << " wall=" << wall_ms << " guest=" << guest_ms << " draw=" << draw_ms
                          << " ps2=" << ps2_ms << " exec=" << exec_ms << " wait=" << wait_ms << " pres=" << pres_ms
                          << " pso=" << pso_ms << "(n=" << g_d3d.tmp_pso_cnt << ")"
                          << " tex=" << tex_ms << "(n=" << g_d3d.tmp_tex_cnt << ")"
                          << " bdel=" << bdel_ms << "(n=" << g_d3d.tmp_buf_del_cnt << ")"
                          << " vramMB=" << (g_d3d.static_vram_total_bytes / (1024 * 1024))
                          << " vramFail=" << g_d3d.tmp_vram_fail
                          << " upMB=" << (g_d3d.upload_cursors[g_d3d.frame_index] / (1024 * 1024))
                          << " splits=" << g_d3d.tmp_split_cnt
                          << " avgWall=" << (g_d3d.tmp_sum_wall / g_d3d.tmp_sum_frames)
                          << "\n" << std::flush;
            }
        }
    }

    const uint64_t signal_val = g_d3d.next_fence_value++;
    g_d3d.command_queue->Signal(g_d3d.fence.Get(), signal_val);
    g_d3d.fence_values[g_d3d.frame_index] = signal_val;
    g_d3d.frame_index = g_d3d.swap_chain->GetCurrentBackBufferIndex();

    if (shot_rb && !pending_shot.empty()) {
        if (g_d3d.fence->GetCompletedValue() < signal_val && g_d3d.fence_event.handle) {
            g_d3d.fence->SetEventOnCompletion(signal_val, g_d3d.fence_event.handle);
            WaitForSingleObject(g_d3d.fence_event.handle, 2000);
        }
        void* mapped = nullptr;
        D3D12_RANGE rb_range{0, static_cast<SIZE_T>(shot_total_bytes)};
        if (SUCCEEDED(shot_rb->Map(0, &rb_range, &mapped)) && mapped) {
            const uint8_t* src_bytes = static_cast<const uint8_t*>(mapped) + shot_fp.Offset;
            const size_t pitch = shot_fp.Footprint.RowPitch;
            std::vector<uint8_t> bgra(static_cast<size_t>(shot_w) * shot_h * 4);
            for (uint32_t y = 0; y < shot_h; ++y) {
                const uint8_t* row = src_bytes + static_cast<size_t>(y) * pitch;
                uint8_t* dst_row = bgra.data() + static_cast<size_t>(y) * shot_w * 4;
                for (uint32_t x = 0; x < shot_w; ++x) {
                    dst_row[x * 4 + 0] = row[x * 4 + 2]; // B
                    dst_row[x * 4 + 1] = row[x * 4 + 1]; // G
                    dst_row[x * 4 + 2] = row[x * 4 + 0]; // R
                    dst_row[x * 4 + 3] = 255;
                }
            }
            const D3D12_RANGE zero_w{0, 0};
            shot_rb->Unmap(0, &zero_w);

            BITMAPINFOHEADER bi{};
            bi.biSize = sizeof(BITMAPINFOHEADER);
            bi.biWidth = static_cast<LONG>(shot_w);
            bi.biHeight = -static_cast<LONG>(shot_h); // top-down
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;
            BITMAPFILEHEADER bmf{};
            bmf.bfType = 0x4D42;
            bmf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            bmf.bfSize = bmf.bfOffBits + static_cast<uint32_t>(bgra.size());
            std::ofstream ofs(pending_shot, std::ios::binary);
            if (ofs) {
                ofs.write(reinterpret_cast<const char*>(&bmf), sizeof(bmf));
                ofs.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
                ofs.write(reinterpret_cast<const char*>(bgra.data()), bgra.size());
            }
        }
    }
}

void wait_d3d12_frame_latency() {
    if (!g_d3d.initialized || !g_d3d.frame_latency_waitable) return;
    // Wait up to 100ms before input sampling so DXGI never queues stale frames ahead of the CPU.
    WaitForSingleObjectEx(g_d3d.frame_latency_waitable, 100, TRUE);
}

void set_d3d12_video_frame(uint32_t width, uint32_t height, const void* rgba_pixels) {
    if (!g_d3d.initialized) return;
    if (width == 0 || height == 0 || !rgba_pixels) {
        clear_d3d12_video_frame();
        return;
    }
    g_d3d.video_width = width;
    g_d3d.video_height = height;
    g_d3d.video_overlay_active = true;
    upload_d3d12_texture(D3D12State::kVideoOverlayTexId, width, height, 0x1908 /*GL_RGBA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, rgba_pixels);
}

void clear_d3d12_video_frame() {
    g_d3d.video_overlay_active = false;
    g_d3d.video_width = 0;
    g_d3d.video_height = 0;
    delete_d3d12_texture(D3D12State::kVideoOverlayTexId);
}

void set_d3d12_loading_bar(bool active, float progress) {
    g_d3d.loading_bar_active = active;
    g_d3d.loading_bar_progress = std::clamp(progress, 0.0f, 1.0f);
}

void present_d3d12_loading_bar(float progress) {
    set_d3d12_loading_bar(true, progress);
    if (!g_d3d.initialized || g_d3d.frame_recording || g_d3d.width < 64 || g_d3d.height < 64) {
        return;
    }
    begin_d3d12_frame();
    constexpr float kBgColor[4] = {0.03f, 0.03f, 0.04f, 1.0f};
    g_d3d.command_list->ClearRenderTargetView(current_rtv(), kBgColor, 0, nullptr);
    g_d3d.cleared_this_frame = true;
    end_d3d12_frame_and_present(0);
}

void shutdown_d3d12() {
    if (!g_d3d.initialized && !g_d3d.device) return;

    wait_for_gpu();
    lcs::ui_overlay::shutdown();
    g_d3d.video_overlay_active = false;
    g_d3d.video_pso.Reset();
    g_d3d.ps2_scene_copy_tex.Reset();
    g_d3d.ps2_radiosity_tex.Reset();
    g_d3d.ps2_history_tex.Reset();
    g_d3d.ps2_rtv_heap.Reset();
    g_d3d.ps2_radiosity_pso.Reset();
    g_d3d.ps2_composite_pso.Reset();
    g_d3d.ps2_scene_copy_srv_slot = 0;
    g_d3d.ps2_radiosity_srv_slot = 0;
    g_d3d.ps2_history_srv_slot = 0;
    g_d3d.ps2_tex_width = 0;
    g_d3d.ps2_tex_height = 0;
    g_d3d.ps2_history_valid = false;
    g_d3d.pso_cache.clear();
    g_d3d.fast_pso_cache.fill({});
    g_d3d.fast_vao_cache.fill({});
    g_d3d.fast_state_pso_cache.fill({});
    g_d3d.srv_table_cache.clear();
    g_d3d.srv2_cache.fill({});
    g_d3d.persistent_srv_cursor = 0;
    g_d3d.need_persistent_srv_gc = false;
    g_d3d.cached_prog_id = 0;
    g_d3d.cached_prog_entry = nullptr;
    g_d3d.cached_bound_cb_addr = 0;
    g_d3d.raw_gpu_bufs.clear();
    g_d3d.tex_ptrs.clear();
    g_d3d.prog_ptrs.clear();
    g_d3d.ebo_bounds_cache.clear();
    g_d3d.converted_vb_cache.clear();
    g_d3d.static_vram_chunks.clear();
    g_d3d.static_vram_total_bytes = 0;
    g_d3d.static_chunk_copy_dest_mask = 0;
    g_d3d.static_chunk_over64_dirty = false;
    g_d3d.programs.clear();
    g_d3d.textures.clear();
    g_d3d.pending_texture_uploads.clear();
    g_d3d.free_cpu_srv_slots.clear();
    for (auto& v : g_d3d.retired_resources) v.clear();
    for (auto& v : g_d3d.retired_srv_slots) v.clear();

    if (g_d3d.upload_ring && g_d3d.upload_ring_mapped) {
        g_d3d.upload_ring->Unmap(0, nullptr);
        g_d3d.upload_ring_mapped = nullptr;
    }
    g_d3d.upload_ring.Reset();

    g_d3d.fence_event.reset();
    g_d3d.fence.Reset();
    g_d3d.command_list.Reset();
    for (auto& alloc : g_d3d.command_allocators) alloc.Reset();
    g_d3d.depth_stencil.Reset();
    for (auto& rt : g_d3d.render_targets) rt.Reset();
    g_d3d.root_signature.Reset();
    g_d3d.gpu_srv_heap.Reset();
    g_d3d.cpu_srv_heap.Reset();
    g_d3d.dsv_heap.Reset();
    g_d3d.rtv_heap.Reset();
    if (g_d3d.frame_latency_waitable) {
        CloseHandle(g_d3d.frame_latency_waitable);
        g_d3d.frame_latency_waitable = nullptr;
    }
    g_d3d.swap_chain.Reset();
    g_d3d.command_queue.Reset();
    g_d3d.device.Reset();
    g_d3d.factory.Reset();

    g_d3d.initialized = false;
    g_d3d.frame_recording = false;
}

bool is_d3d12_initialized() {
    return g_d3d.initialized;
}

void set_d3d12_screenshot_request(const char* filepath) {
    if (!filepath || !*filepath) return;
    g_d3d.screenshot_path = filepath;
}

} // namespace lcs::graphics::d3d12

#endif
