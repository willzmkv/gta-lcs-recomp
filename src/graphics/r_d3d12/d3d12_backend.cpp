#if defined(LCS_ENABLE_D3D12)

#include "d3d12_backend.h"

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
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace lcs::graphics::d3d12 {

namespace {

constexpr uint32_t kFrameCount = 3;
constexpr uint32_t kMaxSamplers = 8;
constexpr size_t   kUploadBytesPerFrame = 16ULL * 1024 * 1024;
constexpr uint32_t kMaxCpuTextures = 2048;
constexpr uint32_t kSrvPerFrame = 4096;

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
    bool valid = false;
};

struct D3D12TextureEntry {
    ComPtr<ID3D12Resource> resource;
    uint32_t cpu_slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool in_srv_state = false;
    bool pending_upload = false;
    bool full_upload = true;
    int32_t sub_x = 0;
    int32_t sub_y = 0;
    uint32_t sub_w = 0;
    uint32_t sub_h = 0;
    std::vector<uint8_t> rgba_staging;
};

struct PSOKey {
    uint32_t prog_id = 0;
    uint8_t blend_enabled = 0;
    uint8_t depth_test = 0;
    uint8_t depth_write = 1;
    uint8_t cull_enabled = 0;
    uint8_t color_mask = 0xF;
    uint8_t topology_type = 3; // D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
    uint32_t blend_src_rgb = 1;
    uint32_t blend_dst_rgb = 0;
    uint32_t blend_src_alpha = 1;
    uint32_t blend_dst_alpha = 0;
    uint32_t blend_eq_rgb = 0x8006;
    uint32_t blend_eq_alpha = 0x8006;
    uint32_t depth_func = 0x0201;
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
              (k.cull_enabled << 3) | (k.color_mask << 4) | (k.topology_type << 8));
        mix_u(k.blend_src_rgb);
        mix_u(k.blend_dst_rgb);
        mix_u(k.blend_src_alpha);
        mix_u(k.blend_dst_alpha);
        mix_u(k.blend_eq_rgb);
        mix_u(k.blend_eq_alpha);
        mix_u(k.depth_func);
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

    uint32_t rtv_descriptor_size = 0;
    uint32_t srv_descriptor_size = 0;
    uint32_t frame_index = 0;
    uint32_t width = 1280;
    uint32_t height = 720;

    bool initialized = false;
    bool frame_recording = false;
    bool cleared_this_frame = false;

    uint32_t next_cpu_srv_slot = 1; // slot 0 reserved for 1x1 white fallback
    std::unordered_map<uint32_t, D3D12ProgramEntry> programs;
    std::unordered_map<uint32_t, D3D12TextureEntry> textures;
    std::vector<uint32_t> pending_texture_uploads;
    std::unordered_map<PSOKey, ComPtr<ID3D12PipelineState>, PSOKeyHash> pso_cache;

    // Reusable scratch vectors to avoid per-draw heap allocations
    std::vector<float> scratch_verts;
    std::vector<uint32_t> scratch_indices;
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
    if (aligned + size > kUploadBytesPerFrame) {
        return false;
    }
    g_d3d.upload_cursors[g_d3d.frame_index] = aligned + size;
    out.buffer_offset = base + aligned;
    out.cpu_ptr = g_d3d.upload_ring_mapped + out.buffer_offset;
    out.gpu_addr = g_d3d.upload_ring->GetGPUVirtualAddress() + out.buffer_offset;
    return true;
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
        // GL_ALPHA
        for (size_t i = 0; i < count; i++) {
            rgba[i * 4 + 0] = 0;
            rgba[i * 4 + 1] = 0;
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = src8[i];
        }
    }
    return rgba;
}

void record_texture_upload(uint32_t tex_id, D3D12TextureEntry& tex) {
    if (!tex.pending_upload || !tex.resource || !g_d3d.frame_recording) return;
    uint32_t w = tex.full_upload ? tex.width : tex.sub_w;
    uint32_t h = tex.full_upload ? tex.height : tex.sub_h;
    if (w == 0 || h == 0 || tex.rgba_staging.empty()) {
        tex.pending_upload = false;
        return;
    }

    uint32_t src_row_bytes = w * 4;
    uint32_t dst_row_pitch = (src_row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                             ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    size_t total_upload = static_cast<size_t>(dst_row_pitch) * h;

    UploadAllocation alloc;
    if (!alloc_upload_slice(total_upload, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, alloc)) {
        return;
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
    (void)tex_id;
}

void flush_pending_texture_uploads() {
    if (g_d3d.pending_texture_uploads.empty() || !g_d3d.frame_recording) return;
    for (uint32_t tid : g_d3d.pending_texture_uploads) {
        auto it = g_d3d.textures.find(tid);
        if (it != g_d3d.textures.end()) {
            record_texture_upload(tid, it->second);
        }
    }
    g_d3d.pending_texture_uploads.clear();
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

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &srv_range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    std::array<D3D12_STATIC_SAMPLER_DESC, kMaxSamplers> samplers{};
    for (uint32_t i = 0; i < kMaxSamplers; i++) {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    D3D12_ROOT_SIGNATURE_DESC desc{};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = static_cast<UINT>(samplers.size());
    desc.pStaticSamplers = samplers.data();
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
        g_d3d.fence_values[i] = g_d3d.next_fence_value;
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

ID3D12PipelineState* get_or_create_pso(const PSOKey& key, const D3D12ProgramEntry& prog) {
    auto it = g_d3d.pso_cache.find(key);
    if (it != g_d3d.pso_cache.end()) {
        return it->second.Get();
    }
    if (!prog.valid) return nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_elems;
    for (size_t i = 0; i < prog.attribs.size(); i++) {
        D3D12_INPUT_ELEMENT_DESC elem{};
        elem.SemanticName = "ATTR";
        elem.SemanticIndex = static_cast<UINT>(prog.attribs[i].location);
        elem.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        elem.InputSlot = 0;
        elem.AlignedByteOffset = static_cast<UINT>(i * 16);
        elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        input_elems.push_back(elem);
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = g_d3d.root_signature.Get();
    pso_desc.VS = {prog.vs_bytecode->GetBufferPointer(), prog.vs_bytecode->GetBufferSize()};
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
    pso_desc.DepthStencilState.StencilEnable = FALSE;

    pso_desc.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(key.topology_type);
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso_desc.SampleDesc.Count = 1;

    ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = g_d3d.device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        log_hresult_failure("CreateGraphicsPipelineState", hr);
        return nullptr;
    }
    ID3D12PipelineState* raw = pso.Get();
    g_d3d.pso_cache.emplace(key, std::move(pso));
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

    ComPtr<IDXGISwapChain1> sc1;
    hr = g_d3d.factory->CreateSwapChainForHwnd(
        g_d3d.command_queue.Get(), hwnd, &sc_desc, nullptr, nullptr, &sc1);
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
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr = g_d3d.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&g_d3d.dsv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (DSV)", hr);
        return false;
    }

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
    gpu_srv_desc.NumDescriptors = kFrameCount * kSrvPerFrame;
    gpu_srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    gpu_srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_d3d.device->CreateDescriptorHeap(&gpu_srv_desc, IID_PPV_ARGS(&g_d3d.gpu_srv_heap));
    if (FAILED(hr)) {
        log_hresult_failure("CreateDescriptorHeap (GPU SRV)", hr);
        return false;
    }
    g_d3d.srv_descriptor_size = g_d3d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

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
    g_d3d.next_fence_value = 1;
    g_d3d.fence_values.fill(0);
    g_d3d.initialized = true;

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

    HRESULT hr = g_d3d.swap_chain->ResizeBuffers(
        kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        log_hresult_failure("IDXGISwapChain3::ResizeBuffers", hr);
        return;
    }

    g_d3d.width = width;
    g_d3d.height = height;
    g_d3d.frame_index = g_d3d.swap_chain->GetCurrentBackBufferIndex();

    create_render_targets();
    create_depth_stencil(width, height);
    std::cout << "[D3D12] Swapchain resized to " << width << "x" << height << "\n";
}

void begin_d3d12_frame() {
    if (!g_d3d.initialized || g_d3d.frame_recording) return;

    wait_for_frame(g_d3d.frame_index);

    g_d3d.upload_cursors[g_d3d.frame_index] = 0;
    g_d3d.srv_cursors[g_d3d.frame_index] = 0;

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

    ID3D12DescriptorHeap* heaps[] = {g_d3d.gpu_srv_heap.Get()};
    g_d3d.command_list->SetDescriptorHeaps(1, heaps);
    g_d3d.command_list->SetGraphicsRootSignature(g_d3d.root_signature.Get());

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(g_d3d.width), static_cast<float>(g_d3d.height), 0.0f, 1.0f};
    D3D12_RECT scissor{0, 0, static_cast<LONG>(g_d3d.width), static_cast<LONG>(g_d3d.height)};
    g_d3d.command_list->RSSetViewports(1, &vp);
    g_d3d.command_list->RSSetScissorRects(1, &scissor);

    g_d3d.frame_recording = true;
    g_d3d.cleared_this_frame = false;

    flush_pending_texture_uploads();
}

void clear_d3d12_targets(bool clear_color, const float color[4],
                         bool clear_depth, float depth,
                         bool clear_stencil, uint8_t stencil) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();

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
    g_d3d.command_list->RSSetViewports(1, &vp);
}

void set_d3d12_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) begin_d3d12_frame();

    LONG top = static_cast<LONG>(g_d3d.height) - (static_cast<LONG>(y) + static_cast<LONG>(height));
    LONG bottom = static_cast<LONG>(g_d3d.height) - static_cast<LONG>(y);
    D3D12_RECT rect{
        std::max<LONG>(0, x),
        std::max<LONG>(0, top),
        std::min<LONG>(static_cast<LONG>(g_d3d.width), static_cast<LONG>(x) + static_cast<LONG>(width)),
        std::min<LONG>(static_cast<LONG>(g_d3d.height), bottom)
    };
    if (rect.right < rect.left) rect.right = rect.left;
    if (rect.bottom < rect.top) rect.bottom = rect.top;
    g_d3d.command_list->RSSetScissorRects(1, &rect);
}

bool register_d3d12_program(uint32_t prog_id, const TranslatedProgramHLSL& translated) {
    if (!g_d3d.initialized) return false;

    D3D12ProgramEntry entry;
    entry.attribs = translated.attribs;

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
    g_d3d.programs[prog_id] = std::move(entry);
    return true;
}

void unregister_d3d12_program(uint32_t prog_id) {
    g_d3d.programs.erase(prog_id);
}

void upload_d3d12_texture(uint32_t tex_id, uint32_t width, uint32_t height,
                          uint32_t format, uint32_t type, const void* pixels) {
    if (!g_d3d.initialized || tex_id == 0 || width == 0 || height == 0) return;

    auto& tex = g_d3d.textures[tex_id];
    if (tex.cpu_slot == 0) {
        if (g_d3d.next_cpu_srv_slot >= kMaxCpuTextures) return;
        tex.cpu_slot = g_d3d.next_cpu_srv_slot++;
    }

    bool need_realloc = (!tex.resource || tex.width != width || tex.height != height);
    if (need_realloc) {
        tex.resource.Reset();
        tex.width = width;
        tex.height = height;
        tex.in_srv_state = false;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;

        HRESULT hr = g_d3d.device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex.resource));
        if (FAILED(hr)) {
            log_hresult_failure("CreateCommittedResource (Texture2D)", hr);
            return;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Texture2D.MipLevels = 1;
        g_d3d.device->CreateShaderResourceView(tex.resource.Get(), &srv_desc, cpu_srv_handle(tex.cpu_slot));
    }

    tex.rgba_staging = unpack_pixels_to_rgba8(width, height, format, type, pixels);
    tex.full_upload = true;
    if (!tex.pending_upload) {
        tex.pending_upload = true;
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
    tex.pending_upload = true;
    g_d3d.pending_texture_uploads.push_back(tex_id);
}

void delete_d3d12_texture(uint32_t tex_id) {
    if (tex_id == 0) return;
    g_d3d.textures.erase(tex_id);
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
    if (!g_d3d.initialized || count == 0) return;
    auto pit = g_d3d.programs.find(prog_id);
    if (pit == g_d3d.programs.end() || !pit->second.valid) return;

    if (!g_d3d.frame_recording) begin_d3d12_frame();
    flush_pending_texture_uploads();

    const D3D12ProgramEntry& prog = pit->second;

    // 1. Determine topology
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    D3D_PRIMITIVE_TOPOLOGY d3d_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool expand_fan = false;
    switch (gl_mode) {
        case 0: // GL_POINTS
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            break;
        case 1: // GL_LINES
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case 3: // GL_LINE_STRIP
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case 5: // GL_TRIANGLE_STRIP
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case 6: // GL_TRIANGLE_FAN
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            expand_fan = true;
            break;
        case 4: // GL_TRIANGLES
        default:
            topo_type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            d3d_topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
    }

    // 2. Get or create PSO
    PSOKey pso_key{};
    pso_key.prog_id = prog_id;
    pso_key.blend_enabled = state.blend_enabled ? 1 : 0;
    pso_key.depth_test = state.depth_test ? 1 : 0;
    pso_key.depth_write = state.depth_write ? 1 : 0;
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
    pso_key.cull_mode = state.cull_mode;
    pso_key.front_face = state.front_face;

    ID3D12PipelineState* pso = get_or_create_pso(pso_key, prog);
    if (!pso) return;

    // 3. Build indices (determining max_vertex_index needed for vertex stream unpacking)
    uint32_t max_vertex_index = 0;
    bool use_index_buffer = indexed || expand_fan;
    g_d3d.scratch_indices.clear();

    if (indexed) {
        if (!index_ptr) return;
        const uint8_t* raw = static_cast<const uint8_t*>(index_ptr);
        size_t idx_bytes = (index_type == 0x1405) ? 4 : (index_type == 0x1401 ? 1 : 2);
        if (index_available_bytes > 0 && static_cast<size_t>(count) * idx_bytes > index_available_bytes) {
            count = static_cast<uint32_t>(index_available_bytes / idx_bytes);
            if (count == 0) return;
        }
        auto read_idx = [&](uint32_t i) -> uint32_t {
            if (index_type == 0x1405) { // GL_UNSIGNED_INT
                uint32_t v = 0;
                std::memcpy(&v, raw + i * 4, 4);
                return v;
            }
            if (index_type == 0x1401) { // GL_UNSIGNED_BYTE
                return raw[i];
            }
            uint16_t v = 0;
            std::memcpy(&v, raw + i * 2, 2);
            return v;
        };

        if (expand_fan && count >= 3) {
            uint32_t i0 = read_idx(0);
            max_vertex_index = i0;
            for (uint32_t i = 1; i + 1 < count; i++) {
                uint32_t i1 = read_idx(i);
                uint32_t i2 = read_idx(i + 1);
                max_vertex_index = std::max({max_vertex_index, i1, i2});
                g_d3d.scratch_indices.push_back(i0);
                g_d3d.scratch_indices.push_back(i1);
                g_d3d.scratch_indices.push_back(i2);
            }
        } else {
            g_d3d.scratch_indices.resize(count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t idx = read_idx(i);
                g_d3d.scratch_indices[i] = idx;
                if (idx > max_vertex_index) max_vertex_index = idx;
            }
        }
    } else if (expand_fan && count >= 3) {
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

    // 4. Unpack vertex stream covering [0..max_vertex_index] (or [first_vertex..max_vertex_index] if non-indexed)
    uint32_t vert_start = indexed ? 0 : first_vertex;
    uint32_t vert_count = (max_vertex_index >= vert_start) ? (max_vertex_index - vert_start + 1) : 1;
    uint32_t num_attrs = static_cast<uint32_t>(prog.attribs.size());
    uint32_t stride_bytes = std::max(16u, num_attrs * 16u);

    UploadAllocation vb_alloc;
    if (!alloc_upload_slice(static_cast<size_t>(vert_count) * stride_bytes, 16, vb_alloc)) {
        return;
    }
    float* v_out = reinterpret_cast<float*>(vb_alloc.cpu_ptr);
    if (num_attrs > 0) {
        for (uint32_t v = 0; v < vert_count; v++) {
            uint32_t src_v = vert_start + v;
            float* dst = v_out + static_cast<size_t>(v) * num_attrs * 4;
            for (uint32_t a = 0; a < num_attrs; a++) {
                int32_t loc = prog.attribs[a].location;
                if (loc >= 0 && loc < 16 && (active_attrib_mask & (1u << loc))) {
                    decode_vertex_attrib(attribs[loc], src_v, dst + a * 4);
                } else if (loc >= 0 && loc < 16) {
                    std::memcpy(dst + a * 4, attribs[loc].default_val, 16);
                } else {
                    dst[a * 4 + 0] = 0.0f;
                    dst[a * 4 + 1] = 0.0f;
                    dst[a * 4 + 2] = 0.0f;
                    dst[a * 4 + 3] = 1.0f;
                }
            }
        }
    } else {
        std::memset(v_out, 0, static_cast<size_t>(vert_count) * stride_bytes);
    }

    // 5. Upload constant buffer slice
    UploadAllocation cb_alloc;
    uint32_t cb_aligned = (std::max(16u, uniform_size) + 255u) & ~255u;
    if (!alloc_upload_slice(cb_aligned, 256, cb_alloc)) return;
    if (uniform_data && uniform_size > 0) {
        std::memcpy(cb_alloc.cpu_ptr, uniform_data, uniform_size);
    } else {
        std::memset(cb_alloc.cpu_ptr, 0, cb_aligned);
    }

    // 6. Stage SRV descriptor table [t0..t7]
    uint32_t& srv_cursor = g_d3d.srv_cursors[g_d3d.frame_index];
    if (srv_cursor + kMaxSamplers > kSrvPerFrame) return;
    uint32_t dst_slot = g_d3d.frame_index * kSrvPerFrame + srv_cursor;
    srv_cursor += kMaxSamplers;

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxSamplers> src_handles;
    std::array<UINT, kMaxSamplers> src_sizes;
    for (uint32_t i = 0; i < kMaxSamplers; i++) {
        uint32_t tid = bound_textures[i];
        auto tit = g_d3d.textures.find(tid);
        uint32_t cslot = (tit != g_d3d.textures.end() && tit->second.resource) ? tit->second.cpu_slot : 0;
        src_handles[i] = cpu_srv_handle(cslot);
        src_sizes[i] = 1;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dst_range_start = gpu_srv_cpu_handle(dst_slot);
    UINT dst_range_size = kMaxSamplers;
    g_d3d.device->CopyDescriptors(
        1, &dst_range_start, &dst_range_size,
        kMaxSamplers, src_handles.data(), src_sizes.data(),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // 7. Record draw command
    g_d3d.command_list->SetPipelineState(pso);
    g_d3d.command_list->IASetPrimitiveTopology(d3d_topo);
    g_d3d.command_list->SetGraphicsRootConstantBufferView(0, cb_alloc.gpu_addr);
    g_d3d.command_list->SetGraphicsRootDescriptorTable(1, gpu_srv_gpu_handle(dst_slot));

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = vb_alloc.gpu_addr;
    vbv.SizeInBytes = vert_count * stride_bytes;
    vbv.StrideInBytes = stride_bytes;
    g_d3d.command_list->IASetVertexBuffers(0, 1, &vbv);

    if (use_index_buffer) {
        size_t ib_bytes = g_d3d.scratch_indices.size() * sizeof(uint32_t);
        UploadAllocation ib_alloc;
        if (!alloc_upload_slice(ib_bytes, 4, ib_alloc)) return;
        std::memcpy(ib_alloc.cpu_ptr, g_d3d.scratch_indices.data(), ib_bytes);

        D3D12_INDEX_BUFFER_VIEW ibv{};
        ibv.BufferLocation = ib_alloc.gpu_addr;
        ibv.SizeInBytes = static_cast<UINT>(ib_bytes);
        ibv.Format = DXGI_FORMAT_R32_UINT;
        g_d3d.command_list->IASetIndexBuffer(&ibv);
        g_d3d.command_list->DrawIndexedInstanced(
            static_cast<UINT>(g_d3d.scratch_indices.size()), 1, 0, 0, 0);
    } else {
        g_d3d.command_list->DrawInstanced(vert_count, 1, 0, 0);
    }
}

void end_d3d12_frame_and_present(uint32_t sync_interval) {
    if (!g_d3d.initialized) return;
    if (!g_d3d.frame_recording) {
        begin_d3d12_frame();
    }

    flush_pending_texture_uploads();

    if (!g_d3d.cleared_this_frame) {
        constexpr float kDefaultBg[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        g_d3d.command_list->ClearRenderTargetView(current_rtv(), kDefaultBg, 0, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_d3d.render_targets[g_d3d.frame_index].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_d3d.command_list->ResourceBarrier(1, &barrier);

    HRESULT hr = g_d3d.command_list->Close();
    g_d3d.frame_recording = false;

    if (SUCCEEDED(hr)) {
        ID3D12CommandList* lists[] = {g_d3d.command_list.Get()};
        g_d3d.command_queue->ExecuteCommandLists(1, lists);
    }

    g_d3d.swap_chain->Present(sync_interval, 0);

    const uint64_t signal_val = g_d3d.next_fence_value++;
    g_d3d.command_queue->Signal(g_d3d.fence.Get(), signal_val);
    g_d3d.fence_values[g_d3d.frame_index] = signal_val;
    g_d3d.frame_index = g_d3d.swap_chain->GetCurrentBackBufferIndex();
}

void shutdown_d3d12() {
    if (!g_d3d.initialized && !g_d3d.device) return;

    wait_for_gpu();
    g_d3d.pso_cache.clear();
    g_d3d.programs.clear();
    g_d3d.textures.clear();
    g_d3d.pending_texture_uploads.clear();

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

} // namespace lcs::graphics::d3d12

#endif
