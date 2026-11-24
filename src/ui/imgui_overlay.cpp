#include "ui/imgui_overlay.h"

#include "game/lcs_prompts.h"
#include "game/lcs_subtitles.h"
#include "game/lcs_trainer.h"
#include "graphics/render_bridge.h"
#include "graphics/window.h"
#include "input/input_system.h"
#include "input/pc_controls.h"
#include "settings/settings.h"
#include "ui/diag_overlay.h"
#include "ui/global_menu.h"
#include "ui/ui_assets.h"
#include "ui/ui_theme.h"

#include "imgui.h"
#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace lcs::ui_overlay {

namespace {
bool g_initialized = false;
bool g_prev_blocking = false;
bool g_assets_loaded = false;
#if defined(LCS_PLATFORM_WINDOWS)
constexpr uint32_t kSrvHeapCapacity = 320;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;
Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> g_imgui_srv_heap;
UINT g_imgui_srv_stride = 0;
std::array<uint64_t, kSrvHeapCapacity / 64> g_imgui_srv_used_mask{};

struct UploadedTexture {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    ui::assets::RealTexture tex;
};

UploadedTexture g_map_tex;
UploadedTexture g_char_tex[4];
UploadedTexture g_ctrl_tex[5];
UploadedTexture g_logo_tex;
UploadedTexture g_mm_bg_tex;
UploadedTexture g_mm_toni_tex;
UploadedTexture g_mm_logo_tex;
UploadedTexture g_radar_sprites[64]; // Indexed directly by 1-based CRadar::pRadarSprites ID
std::unordered_map<std::string, UploadedTexture> g_prompt_textures;

bool alloc_srv_slot(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    uint32_t slot = kSrvHeapCapacity;
    for (uint32_t i = 0; i < kSrvHeapCapacity; ++i) {
        const uint32_t w = i >> 6;
        const uint64_t bit = 1ULL << (i & 63u);
        if ((g_imgui_srv_used_mask[w] & bit) == 0) {
            g_imgui_srv_used_mask[w] |= bit;
            slot = i;
            break;
        }
    }
    if (slot >= kSrvHeapCapacity) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_imgui_srv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_imgui_srv_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * g_imgui_srv_stride;
    gpu.ptr += static_cast<SIZE_T>(slot) * g_imgui_srv_stride;
    *out_cpu = cpu;
    *out_gpu = gpu;
    return true;
}

void imgui_srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    alloc_srv_slot(out_cpu, out_gpu);
}

void imgui_srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    if (!g_imgui_srv_heap || g_imgui_srv_stride == 0) return;
    const SIZE_T base = g_imgui_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    if (cpu.ptr >= base) {
        const uint32_t slot = static_cast<uint32_t>((cpu.ptr - base) / g_imgui_srv_stride);
        if (slot < kSrvHeapCapacity) g_imgui_srv_used_mask[slot >> 6] &= ~(1ULL << (slot & 63u));
    }
}

bool upload_rgba_to_d3d12(const uint8_t* rgba, int w, int h, UploadedTexture& out) {
    if (!g_device || !g_queue || !rgba || w <= 0 || h <= 0) return false;

    D3D12_HEAP_PROPERTIES default_heap{};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc{};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = static_cast<UINT64>(w);
    tex_desc.Height = static_cast<UINT>(h);
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    Microsoft::WRL::ComPtr<ID3D12Resource> tex_res;
    if (FAILED(g_device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex_res)))) {
        return false;
    }

    const UINT row_pitch = (static_cast<UINT>(w) * 4u + 255u) & ~255u;
    const UINT64 upload_size = static_cast<UINT64>(row_pitch) * static_cast<UINT64>(h);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc{};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = upload_size;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buf;
    if (FAILED(g_device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buf)))) {
        return false;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE read_range{0, 0};
    if (FAILED(upload_buf->Map(0, &read_range, reinterpret_cast<void**>(&mapped)))) return false;
    for (int y = 0; y < h; ++y) {
        std::memcpy(mapped + static_cast<size_t>(y) * row_pitch,
                    rgba + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                    static_cast<size_t>(w) * 4u);
    }
    upload_buf->Unmap(0, nullptr);

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl)))) return false;
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;

    D3D12_TEXTURE_COPY_LOCATION dst_loc{};
    dst_loc.pResource = tex_res.Get();
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = upload_buf.Get();
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint.Offset = 0;
    src_loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src_loc.PlacedFootprint.Footprint.Width = static_cast<UINT>(w);
    src_loc.PlacedFootprint.Footprint.Height = static_cast<UINT>(h);
    src_loc.PlacedFootprint.Footprint.Depth = 1;
    src_loc.PlacedFootprint.Footprint.RowPitch = row_pitch;

    cl->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex_res.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &barrier);
    cl->Close();

    ID3D12CommandList* lists[] = {cl.Get()};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (ev) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    if (!alloc_srv_slot(&cpu, &gpu)) return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(tex_res.Get(), &srv, cpu);

    out.resource = tex_res;
    out.tex.id = static_cast<ImTextureID>(gpu.ptr);
    out.tex.w = w;
    out.tex.h = h;
    return true;
}

bool load_png_file(const std::string& path, UploadedTexture& out) {
    if (!std::filesystem::exists(path)) return false;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) return false;
    const bool ok = upload_rgba_to_d3d12(px, w, h, out);
    stbi_image_free(px);
    return ok;
}

// Load the user's 2000x2000 HD Liberty City map (fallback to 64 RADAR00..63 tiles if missing)
bool load_real_liberty_city_map(UploadedTexture& out) {
    const std::string hd_path = "launcher/assets/liberty_city_hd_map.png";
    if (std::filesystem::exists(hd_path)) {
        if (load_png_file(hd_path, out)) {
            std::cout << "[GlobalMenu] Loaded HD Liberty City map (" << out.tex.w << "x" << out.tex.h << ")\n";
            return true;
        }
    }
    constexpr int kGrid = 8;
    constexpr int kTileOut = 128;
    constexpr int kAtlas = kGrid * kTileOut;
    std::vector<uint8_t> atlas(static_cast<size_t>(kAtlas) * kAtlas * 4u, 0);
    int loaded = 0;

    for (int idx = 0; idx < 64; ++idx) {
        char path[256];
        std::snprintf(path, sizeof(path), "assets/ipad/Textures_Shared/RADAR%02d/radar%02d.png", idx, idx);
        int w = 0, h = 0, ch = 0;
        stbi_uc* px = stbi_load(path, &w, &h, &ch, 4);
        if (!px) continue;
        const int gx = idx % kGrid;
        const int gy = idx / kGrid;
        for (int dy = 0; dy < kTileOut; ++dy) {
            const int sy = (dy * h) / kTileOut;
            for (int dx = 0; dx < kTileOut; ++dx) {
                const int sx = (dx * w) / kTileOut;
                const size_t src_i = (static_cast<size_t>(sy) * w + sx) * 4u;
                const size_t dst_i = (static_cast<size_t>(gy * kTileOut + dy) * kAtlas + (gx * kTileOut + dx)) * 4u;
                const uint8_t r = px[src_i + 0];
                const uint8_t g = px[src_i + 1];
                const uint8_t b = px[src_i + 2];
                const uint8_t a = px[src_i + 3];
                const bool is_ocean_bg = (a < 20) || (r >= 242 && g >= 242 && b >= 242);
                atlas[dst_i + 0] = r;
                atlas[dst_i + 1] = g;
                atlas[dst_i + 2] = b;
                atlas[dst_i + 3] = is_ocean_bg ? 0 : a;
            }
        }
        stbi_image_free(px);
        loaded++;
    }
    if (loaded == 0) return false;
    return upload_rgba_to_d3d12(atlas.data(), kAtlas, kAtlas, out);
}

void load_all_real_assets() {
    load_real_liberty_city_map(g_map_tex);

    static constexpr const char* kCharsHd[4] = {
        "launcher/assets/characters/menu_sub_1.png",
        "launcher/assets/characters/menu_sub_2.png",
        "launcher/assets/characters/menu_sub_3.png",
        "launcher/assets/characters/menu_sub_4.png",
    };
    static constexpr const char* kChars[4] = {
        "assets/ipad/Textures_Shared/menu/menu_sub_1.png",
        "assets/ipad/Textures_Shared/menu/menu_sub_2.png",
        "assets/ipad/Textures_Shared/menu/menu_sub_3.png",
        "assets/ipad/Textures_Shared/menu/menu_sub_4.png",
    };
    int chars_ok = 0;
    for (int i = 0; i < 4; ++i) {
        if (load_png_file(kCharsHd[i], g_char_tex[i]) || load_png_file(kChars[i], g_char_tex[i])) chars_ok++;
    }

    static constexpr const char* kCtrls[5] = {
        "launcher/assets/controllers/xbox.png",
        "launcher/assets/controllers/ps5.png",
        "launcher/assets/controllers/ps2.png",
        "launcher/assets/controllers/switch.png",
        "launcher/assets/controllers/kb_mouse.png",
    };
    int ctrls_ok = 0;
    for (int i = 0; i < 5; ++i) {
        if (load_png_file(kCtrls[i], g_ctrl_tex[i])) ctrls_ok++;
    }

    if (!load_png_file("launcher/assets/characters/menu_LCSLogo.png", g_logo_tex)) {
        load_png_file("assets/ipad/Textures_Shared/menu/menu_LCSLogo.png", g_logo_tex);
    }
    load_png_file("launcher/assets/mainmenu/lcs_panels_bg.jpg", g_mm_bg_tex);
    load_png_file("launcher/assets/mainmenu/toni_main.png", g_mm_toni_tex);
    load_png_file("launcher/assets/mainmenu/lcs_logo.png", g_mm_logo_tex);

    // Exact 1-based CRadar::pRadarSprites mapping verified against libGame.so (0x811da0 / LoadTextures 0x357924)
    struct SpriteFile { int id; const char* path; };
    static constexpr SpriteFile kSpriteFiles[] = {
        { 1, "assets/ipad/Textures_Shared/touch_fe/radar_player_centre.png"}, // CentreSprite
        { 3, "assets/ipad/Textures_Shared/touch_fe/radar_north.png"},         // NorthSprite
        {10, "assets/ipad/Textures_Shared/touch_fe/radar_phil.png"},          // PhilSprite
        {16, "assets/ipad/Textures_Shared/touch_fe/radar_gun.png"},           // GunSprite (Ammu-Nation)
        {19, "assets/ipad/Textures_Shared/touch_fe/radar_save.png"},          // SaveHouseSprite
        {20, "assets/ipad/Textures_Shared/hud/radar_strip.png"},              // StripSprite
        {25, "assets/ipad/Textures_Shared/hud/property.png"},                 // PropertySprite
        {27, "assets/ipad/Textures_Shared/touch_fe/radar_spray.png"},         // SpraySprite (Pay 'n' Spray)
        {28, "assets/ipad/Textures_Shared/hud/tshirt.png"},                   // TShirtSprite
        {30, "assets/ipad/Textures_Shared/hud/phone.png"},                    // PhoneSprite
        {31, "assets/ipad/Textures_Shared/touch_fe/radar_8Ball.png"},         // EightBallSprite
        {32, "assets/ipad/Textures_Shared/hud/radar_Burner.png"},             // BurnerSprite
        {33, "assets/ipad/Textures_Shared/touch_fe/radar_Cipriani.png"},      // CiprianiSprite
        {34, "assets/ipad/Textures_Shared/touch_fe/radar_Donald.png"},        // DonaldSprite
        {35, "assets/ipad/Textures_Shared/touch_fe/radar_Joseph.png"},        // JosephSprite (JD)
        {36, "assets/ipad/Textures_Shared/touch_fe/radar_Leone.png"},         // LeoneSprite
        {37, "assets/ipad/Textures_Shared/touch_fe/radar_Maria.png"},         // MariaSprite
        {38, "assets/ipad/Textures_Shared/touch_fe/radar_McRay.png"},         // McraySprite
        {39, "assets/ipad/Textures_Shared/touch_fe/radar_Toshiko.png"},       // ToshikoSprite
        {40, "assets/ipad/Textures_Shared/touch_fe/radar_Vincenzo.png"},      // VincenzoSprite
        {41, "assets/ipad/Textures_Shared/hud/radar_Bomb.png"},               // BombSprite
        {42, "assets/ipad/Textures_Shared/hud/radar_cShop.png"},              // ClothesShopSprite
        {43, "assets/ipad/Textures_Shared/hud/radar_propertyR.png"},          // PropertyRedSprite
        {44, "assets/ipad/Textures_Shared/touch_fe/radar_flag.png"},          // PhilCassidySprite -> radar_flag.png
        {45, "assets/ipad/Textures_Shared/hud/radar_powerup.png"},            // PowerupSprite
        {47, "assets/ipad/Textures_Shared/touch_fe/radar_mp_checkpoint.png"}, // MPCheckPoint
        {54, "assets/ipad/Textures_Shared/touch_fe/radar_arrow.png"},         // MapHereSprite0
        {62, "assets/ipad/Textures_Shared/touch_fe/radar_player_target.png"}, // MapWayPoint
    };
    int icons_ok = 0;
    for (const auto& sf : kSpriteFiles) {
        if (sf.id >= 0 && sf.id < 64 && load_png_file(sf.path, g_radar_sprites[sf.id])) icons_ok++;
    }

    // Load real Kenney Input Prompts PNGs from launcher/assets/prompts/{kb_mouse,xbox,ps,switch}/
    int prompts_ok = 0;
    static constexpr const char* kPromptDirs[] = {
        "launcher/assets/prompts/kb_mouse",
        "launcher/assets/prompts/xbox",
        "launcher/assets/prompts/ps",
        "launcher/assets/prompts/switch",
    };
    std::error_code ec;
    for (const char* dir : kPromptDirs) {
        if (!std::filesystem::exists(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string fname = entry.path().filename().string();
            UploadedTexture ut;
            if (load_png_file(entry.path().generic_string(), ut)) {
                g_prompt_textures.emplace(fname, std::move(ut));
                prompts_ok++;
            }
        }
    }
    static constexpr const char* kTutSprites[][2] = {
        {"RADARBOMB",     "assets/ipad/Textures_Shared/touch_fe/hud_tut_radarBomb.png"},
        {"RADARCLOTHES",  "assets/ipad/Textures_Shared/touch_fe/hud_tut_radarClothes.png"},
        {"RADARFLAG",     "assets/ipad/Textures_Shared/touch_fe/hud_tut_radarFlag.png"},
        {"RADARGUN",      "assets/ipad/Textures_Shared/touch_fe/hud_tut_radarGun.png"},
        {"RADARSPRAY",    "assets/ipad/Textures_Shared/touch_fe/hud_tut_radarSpray.png"},
        {"*HUD_TUT_TICK", "assets/ipad/Textures_Shared/touch_fe/hud_tut_tick.png"},
        {"HUD_TUT_TICK",  "assets/ipad/Textures_Shared/touch_fe/hud_tut_tick.png"},
        {"ZOOMIN",        "assets/ipad/Textures_Shared/touch_fe/hud_tut_zoomin.png"},
        {"ZOOMOUT",       "assets/ipad/Textures_Shared/touch_fe/hud_tut_zoomout.png"},
    };
    for (const auto& ts : kTutSprites) {
        UploadedTexture ut;
        if (load_png_file(ts[1], ut)) {
            g_prompt_textures.emplace(ts[0], std::move(ut));
            prompts_ok++;
        }
    }
    std::cout << "[GlobalMenu] Real assets loaded (chars=" << chars_ok << "/4, controllers=" << ctrls_ok
              << "/5, radar_sprites=" << icons_ok << "/" << (sizeof(kSpriteFiles) / sizeof(kSpriteFiles[0]))
              << ", prompts=" << prompts_ok
              << ", logo=" << (g_logo_tex.tex.valid() ? "ok" : "n/a") << ")\n";
}
#endif

void draw_fps_counter() {
    const int mode = lcs::settings::get().display_fps_counter;
    if (mode <= 0) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float s = std::clamp(io.DisplaySize.y / 900.0f, 0.85f, 2.0f);
    char buf[64];
    if (mode == 1) {
        std::snprintf(buf, sizeof(buf), "%.0f FPS", io.Framerate);
    } else {
        const float ms = (io.Framerate > 0.1f) ? (1000.0f / io.Framerate) : 0.0f;
        std::snprintf(buf, sizeof(buf), "%.0f FPS  (%.2f ms)", io.Framerate, ms);
    }
    ImGui::PushFont(ui::fontSemibold(), 16.0f * s);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const float padX = 10.0f * s;
    const float padY = 5.0f * s;
    const float margin = 12.0f * s;
    const float boxW = ts.x + padX * 2.0f;
    // Place top-right when GlobalMenu is open (clear of centered tabs) and top-center in gameplay (clear of HUD & radar)
    const float posX = ui::globalMenuOpen() ? (io.DisplaySize.x - boxW - 20.0f * s) : (io.DisplaySize.x - boxW) * 0.5f;
    const float posY = ui::globalMenuOpen() ? 14.0f * s : margin;
    const ImVec2 p0(posX, posY);
    const ImVec2 p1(p0.x + boxW, posY + ts.y + padY * 2.0f);
    dl->AddRectFilled(p0, p1, ui::palette::u32(ui::palette::kHeaderBg, 0.85f), 4.0f * s);
    dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f * s, p1.y), ui::palette::u32(ui::palette::kPink), 2.0f * s);
    dl->AddText(ImVec2(p0.x + padX, p0.y + padY), ui::palette::u32(ui::palette::kTealDark), buf);
    ImGui::PopFont();
}
} // namespace

bool init_d3d12(void* hwnd, ID3D12Device* device, ID3D12CommandQueue* command_queue, uint32_t num_frames_in_flight) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_initialized || !hwnd || !device || !command_queue) return g_initialized;

    g_device = device;
    g_queue = command_queue;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kSrvHeapCapacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_imgui_srv_heap)))) {
        return false;
    }
    g_imgui_srv_stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_imgui_srv_used_mask.fill(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::initTheme();

    ImGui_ImplWin32_Init(reinterpret_cast<HWND>(hwnd));

    ImGui_ImplDX12_InitInfo init_info{};
    init_info.Device = device;
    init_info.CommandQueue = command_queue;
    init_info.NumFramesInFlight = static_cast<int>(num_frames_in_flight);
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    init_info.SrvDescriptorHeap = g_imgui_srv_heap.Get();
    init_info.SrvDescriptorAllocFn = imgui_srv_alloc;
    init_info.SrvDescriptorFreeFn = imgui_srv_free;

    if (!ImGui_ImplDX12_Init(&init_info)) {
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imgui_srv_heap.Reset();
        return false;
    }

    // Defer load_all_real_assets() until first openGlobalMenu() so boot & main-menu initialization never touches the GPU queue.
    g_assets_loaded = false;
    g_initialized = true;
    g_prev_blocking = false;
    return true;
#else
    (void)hwnd; (void)device; (void)command_queue; (void)num_frames_in_flight;
    return false;
#endif
}

void shutdown() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_initialized) return;
    g_map_tex.resource.Reset();
    for (auto& c : g_char_tex) c.resource.Reset();
    for (auto& c : g_ctrl_tex) c.resource.Reset();
    g_logo_tex.resource.Reset();
    g_mm_bg_tex.resource.Reset();
    g_mm_toni_tex.resource.Reset();
    g_mm_logo_tex.resource.Reset();
    for (auto& r : g_radar_sprites) r.resource.Reset();
    g_prompt_textures.clear();
    g_assets_loaded = false;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_imgui_srv_heap.Reset();
    g_device = nullptr;
    g_queue = nullptr;
    g_initialized = false;
    g_prev_blocking = false;
#endif
}

bool handle_win32_message(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_initialized) return false;

    const auto& tr = lcs::game::LCSTrainer::instance();
    const bool allow_global_menu = tr.is_in_gameplay() || tr.is_in_main_menu();

    if (msg == WM_KEYDOWN && (lparam & (1 << 30)) == 0) {
        // ESC during active gameplay opens the unified GTA 6-style Global Menu (JOGO hub)
        if (wparam == VK_ESCAPE && !ui::globalMenuOpen() && tr.is_in_gameplay()) {
            ui::openGlobalMenu(ui::GlobalTab::Jogo);
            ImGui::GetIO().ClearInputKeys();
            lcs::input::PCControls::instance().reset_all();
            lcs::input::InputSystem::instance().release_all_guest_inputs();
            g_prev_blocking = true;
            lcs::graphics::set_menu_active(true);
            return true;
        }
        // F2 shortcut opens directly to CONFIGURACOES (only once in Main Menu or Gameplay)
        if (wparam == VK_F2 && allow_global_menu) {
            ui::toggleGlobalMenu(ui::GlobalTab::Configuracoes);
            ImGui::GetIO().ClearInputKeys();
            const bool blocking = is_blocking_game_input();
            if (blocking) {
                lcs::input::PCControls::instance().reset_all();
                lcs::input::InputSystem::instance().release_all_guest_inputs();
            }
            g_prev_blocking = blocking;
            lcs::graphics::set_menu_active(blocking);
            return true;
        }
        // F5 shortcut opens directly to TRAINER (during Gameplay; opens Mods if on Main Menu)
        if (wparam == VK_F5 && allow_global_menu) {
            ui::toggleGlobalMenu(tr.is_in_gameplay() ? ui::GlobalTab::Trainer : ui::GlobalTab::Mods);
            ImGui::GetIO().ClearInputKeys();
            const bool blocking = is_blocking_game_input();
            if (blocking) {
                lcs::input::PCControls::instance().reset_all();
                lcs::input::InputSystem::instance().release_all_guest_inputs();
            }
            g_prev_blocking = blocking;
            lcs::graphics::set_menu_active(blocking);
            return true;
        }
        // F3: diagnostics panel; Ctrl+F3 copies the player's coordinates
        if (wparam == VK_F3 && tr.is_in_gameplay()) {
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                lcs::ui_diag::copy_position(hwnd);
            } else {
                lcs::ui_diag::toggle();
            }
            return true;
        }
        if (wparam == VK_F6 && tr.is_in_gameplay()) {
            auto& mtr = lcs::game::LCSTrainer::instance();
            mtr.free_camera = !mtr.free_camera;
            mtr.push_notice(mtr.free_camera ? "Camera Livre: ON" : "Camera Livre: OFF");
            return true;
        }
    }

    if (is_blocking_game_input()) {
        ImGui_ImplWin32_WndProcHandler(reinterpret_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                       static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
        return true;
    }

    // Keep ImGui key-up / focus state clean while closed without blocking game input
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_KILLFOCUS) {
        ImGui_ImplWin32_WndProcHandler(reinterpret_cast<HWND>(hwnd), static_cast<UINT>(msg),
                                       static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));
    }
    return false;
#else
    (void)hwnd; (void)msg; (void)wparam; (void)lparam;
    return false;
#endif
}

bool is_blocking_game_input() {
    return ui::globalMenuBlocksInput() || lcs::game::LCSTrainer::instance().is_in_main_menu();
}

bool has_visible_overlay() {
    // CRITICAL: Never invoke ImGui_ImplDX12_RenderDrawData during boot splash/Legal loading,
    // which would submit a synchronous command list while the game is loading textures/fonts!
    if (is_blocking_game_input()) return true;
    if (lcs::subtitles::has_active_items() || lcs::subtitles::has_active_hud_items()) return true;
    const auto& tr = lcs::game::LCSTrainer::instance();
    if (!tr.is_in_gameplay()) return false;
    return !tr.notices().empty() || lcs::settings::get().display_fps_counter > 0 || lcs::ui_diag::is_open();
}

void feed_gamepad_navigation_to_imgui(ImGuiIO& io) {
    struct NavBinding { ImGuiKey key; bool held; };
    const auto& inp = lcs::input::InputSystem::instance();
    // Use get_pad_axis (physical controller ONLY) so keyboard WASD (m_kb_axes) never triggers gamepad prompts!
    const float lx = inp.get_pad_axis(0, 0);
    const float ly = inp.get_pad_axis(0, 1);
    const float lt = inp.get_pad_axis(0, 4);
    const float rt = inp.get_pad_axis(0, 5);

    const bool active = is_blocking_game_input();
    const bool in_map_tab = ui::globalMenuOpen() && (ui::globalMenuActiveTab() == ui::GlobalTab::Mapa);
    const NavBinding bindings[] = {
        {ImGuiKey_Enter,      active && inp.is_joy_button_held(0, 0)},
        {ImGuiKey_Escape,     active && inp.is_joy_button_held(0, 1)},
        {ImGuiKey_X,          active && inp.is_joy_button_held(0, 2)},
        {ImGuiKey_Tab,        active && inp.is_joy_button_held(0, 3)},
        {ImGuiKey_Q,          active && inp.is_joy_button_held(0, 6)},
        {ImGuiKey_E,          active && inp.is_joy_button_held(0, 7)},
        {ImGuiKey_PageUp,     active && !in_map_tab && (lt > 0.45f)},
        {ImGuiKey_PageDown,   active && !in_map_tab && (rt > 0.45f)},
        {ImGuiKey_R,          active && in_map_tab && (inp.is_joy_button_held(0, 13) || inp.is_joy_button_held(0, 12))},
        {ImGuiKey_UpArrow,    active && (inp.is_joy_button_held(0, 8)  || (!in_map_tab && ly < -0.55f))},
        {ImGuiKey_DownArrow,  active && (inp.is_joy_button_held(0, 9)  || (!in_map_tab && ly >  0.55f))},
        {ImGuiKey_LeftArrow,  active && (inp.is_joy_button_held(0, 10) || (!in_map_tab && lx < -0.55f))},
        {ImGuiKey_RightArrow, active && (inp.is_joy_button_held(0, 11) || (!in_map_tab && lx >  0.55f))},
    };
    static bool s_prev[sizeof(bindings) / sizeof(bindings[0])] = {};
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
        if (bindings[i].held != s_prev[i]) {
            s_prev[i] = bindings[i].held;
            io.AddKeyEvent(bindings[i].key, bindings[i].held);
        }
    }
}

void render_d3d12(ID3D12GraphicsCommandList* cmd_list) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_initialized || !cmd_list || !g_imgui_srv_heap || !has_visible_overlay()) return;

    const auto& tr = lcs::game::LCSTrainer::instance();
    const bool in_main_menu = tr.is_in_main_menu();

    if (!g_assets_loaded && (ui::globalMenuOpen() || in_main_menu || tr.is_in_gameplay())) {
        load_all_real_assets();
        g_assets_loaded = true;
    }

    ImGuiIO& io = ImGui::GetIO();
    // Always rely on the native Windows hardware cursor in menus to prevent double-cursor artifacts
    io.MouseDrawCursor = false;
    feed_gamepad_navigation_to_imgui(io);
    const bool hide_pad_mouse = is_blocking_game_input() &&
        lcs::game::LCSPrompts::instance().active_device() != lcs::game::PromptDevice::KeyboardMouse;
    if (hide_pad_mouse) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
    lcs::graphics::sync_menu_cursor_visibility();

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    // Keep io.DisplaySize matched to Win32 client rect so io.MousePos and InputEventsQueue
    // remain 1:1 with the OS cursor, and apply swapchain supersampling via DisplayFramebufferScale.
    {
        const auto& rb = lcs::graphics::RenderBridge::instance();
        const float sw = static_cast<float>(rb.get_surface_width());
        const float sh = static_cast<float>(rb.get_surface_height());
        if (sw > 64.0f && sh > 64.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f) {
            io.DisplayFramebufferScale = ImVec2(sw / io.DisplaySize.x, sh / io.DisplaySize.y);
        } else {
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        }
    }
    ImGui::NewFrame();
    if (hide_pad_mouse) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ui::globalMenuOpen() && !in_main_menu) {
        lcs::subtitles::draw_overlay(ImGui::GetBackgroundDrawList(), io.DisplaySize.x, io.DisplaySize.y);
    }
    if (ui::globalMenuOpen()) {
        ui::drawGlobalMenu();
    }
    if (!ui::globalMenuOpen() && in_main_menu) {
        ui::drawMainMenu();
    }
    ui::drawNotices();
    if (!in_main_menu || ui::globalMenuOpen()) {
        draw_fps_counter();
    }
    lcs::ui_diag::record_frame(io.DeltaTime);
    if (!ui::globalMenuOpen() && !in_main_menu) {
        lcs::ui_diag::draw();
    }

    // Sync window cursor / input lock if the menu closed itself via ESC / Continuar
    const bool now_blocking = is_blocking_game_input();
    if (now_blocking != g_prev_blocking) {
        g_prev_blocking = now_blocking;
        lcs::graphics::set_menu_active(now_blocking);
        if (!now_blocking) {
            io.ClearInputKeys();
        }
    }

    ImGui::Render();
    ID3D12DescriptorHeap* heaps[] = {g_imgui_srv_heap.Get()};
    cmd_list->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd_list);
#else
    (void)cmd_list;
#endif
}

} // namespace lcs::ui_overlay

namespace ui::assets {

const RealTexture& mapTexture() {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_map_tex.tex;
#else
    static RealTexture empty{}; return empty;
#endif
}

const RealTexture& characterTexture(int idx) {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_char_tex[std::clamp(idx, 0, 3)].tex;
#else
    (void)idx; static RealTexture empty{}; return empty;
#endif
}

const RealTexture& controllerTexture(int idx) {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_ctrl_tex[std::clamp(idx, 0, 4)].tex;
#else
    (void)idx; static RealTexture empty{}; return empty;
#endif
}

const RealTexture& logoTexture() {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_logo_tex.tex;
#else
    static RealTexture empty{}; return empty;
#endif
}

const RealTexture& mainMenuBgTexture() {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_mm_bg_tex.tex;
#else
    static RealTexture empty{}; return empty;
#endif
}

const RealTexture& mainMenuToniTexture() {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_mm_toni_tex.tex;
#else
    static RealTexture empty{}; return empty;
#endif
}

const RealTexture& mainMenuLogoTexture() {
#if defined(LCS_PLATFORM_WINDOWS)
    return lcs::ui_overlay::g_mm_logo_tex.tex;
#else
    static RealTexture empty{}; return empty;
#endif
}

const RealTexture& radarSprite(int spriteId) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (spriteId >= 0 && spriteId < 64) return lcs::ui_overlay::g_radar_sprites[spriteId].tex;
    static RealTexture empty{}; return empty;
#else
    (void)spriteId; static RealTexture empty{}; return empty;
#endif
}

const RealTexture& inputPromptTexture(int platformPreset, const char* token) {
    static const RealTexture empty{};
#if defined(LCS_PLATFORM_WINDOWS)
    if (!token || !*token) return empty;
    std::string k;
    for (const char* p = token; *p; ++p) {
        if (*p != ' ') k.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(*p))));
    }

    // Platform-independent radar & HUD tutorial icons embedded directly from touch_fe/
    if (k.rfind("RADAR", 0) == 0 || k == "*HUD_TUT_TICK" || k == "HUD_TUT_TICK" || k == "ZOOMIN" || k == "ZOOMOUT") {
        const auto it = lcs::ui_overlay::g_prompt_textures.find(k);
        if (it != lcs::ui_overlay::g_prompt_textures.end()) return it->second.tex;
    }

    // Map GTA LCS GXT ::TOKEN:: and ~X/O/S/T~ action names to canonical controller/keyboard button keys
    auto normalize_gxt_token = [](std::string& key, int plat) {
        const bool kbm = (plat == 3);
        const bool ps  = (plat == 1 || plat == 2);
        const bool xb  = (plat == 0);
        if (key == "ACCEL" || key == "DRIVE" || key == "VEHICLE_ACCELERATE") key = kbm ? "W" : (ps ? "R2" : (xb ? "RT" : "ZR"));
        else if (key == "BRAKE" || key == "VEHICLE_BRAKE") key = kbm ? "S" : (ps ? "L2" : (xb ? "LT" : "ZL"));
        else if (key == "HANDBRAKE" || key == "VEHICLE_HANDBRAKE") key = kbm ? "SPACE" : (ps ? "R1" : (xb ? "RB" : "R"));
        else if (key == "BTNA" || key == "BTNALARGE" || key == "CROSS") key = kbm ? "SPACE" : (ps ? "X" : (xb ? "A" : "B"));
        else if (key == "BTNB" || key == "BTNBLARGE") key = kbm ? "LMB" : (ps ? "CIRCLE" : (xb ? "B" : "A"));
        else if (key == "BTNX" || key == "BTNXLARGE") key = kbm ? "S" : (ps ? "SQUARE" : (xb ? "X" : "Y"));
        else if (key == "BTNY" || key == "BTNYLARGE") key = kbm ? "F" : (ps ? "TRIANGLE" : (xb ? "Y" : "X"));
        else if (key == "CAR" || key == "BOATIN" || key == "EXIT" || key == "DOOR" || key == "GETINCAR" || key == "GETONBIKE" || key == "EXITVEH")
            key = kbm ? "F" : (ps ? "TRIANGLE" : (xb ? "Y" : "X"));
        else if (key == "FIRE" || key == "ATTACK" || key == "PUNCH" || key == "THROW" || key == "HOSE" || key == "PEDFIRE" || key == "PED_FIREWEAPON")
            key = kbm ? "LMB" : (ps ? "CIRCLE" : (xb ? "B" : "A"));
        else if (key == "SPRINT" || key == "PED_SPRINT") key = kbm ? "SHIFT" : (ps ? "X" : (xb ? "A" : "B"));
        else if (key == "JUMP" || key == "PED_JUMPING") key = kbm ? "SPACE" : (ps ? "SQUARE" : (xb ? "X" : "Y"));
        else if (key == "CROUCH" || key == "BTNL3" || key == "L3") key = kbm ? "C" : "L3";
        else if (key == "HORN" || key == "SIREN" || key == "VEHICLE_HORN") key = kbm ? "H" : "L3";
        else if (key == "SUBMISSIONS" || key == "BTNR3" || key == "R3") key = kbm ? "C" : "R3";
        else if (key == "CAMERA") key = kbm ? "V" : (ps ? "SELECT" : (xb ? "VIEW" : "MINUS"));
        else if (key == "PHONE" || key == "BUY" || key == "SWAP" || key == "PILL") key = kbm ? "F" : (ps ? "L1" : (xb ? "LB" : "L"));
        else if (key == "BTNL1" || key == "LOCKON" || key == "LOCKONCENTRE" || key == "LOCKONLEFT" || key == "LOCKONRIGHT" || key == "SHOOTCAM")
            key = kbm ? "RMB" : (ps ? "L1" : (xb ? "LB" : "L"));
        else if (key == "BTNR1") key = kbm ? "SPACE" : (ps ? "R1" : (xb ? "RB" : "R"));
        else if (key == "BTNL2" || key == "LEFT" || key == "PREVWEAPON") key = kbm ? (key == "PREVWEAPON" ? "SCROLL" : "Q") : (ps ? "L2" : (xb ? "LT" : "ZL"));
        else if (key == "BTNR2" || key == "RIGHT" || key == "NEXTWEAPON") key = kbm ? (key == "NEXTWEAPON" ? "SCROLL" : "E") : (ps ? "R2" : (xb ? "RT" : "ZR"));
        else if (key == "BTNLEFTSTICK" || key == "LANALOG" || key == "LSTICK" || key == "TANKLEFT" || key == "TANKRIGHT" ||
                 key == "GO_FORWARD" || key == "GO_BACK" || key == "GO_LEFT" || key == "GO_RIGHT")
            key = kbm ? "W" : (ps ? "L" : "LS");
        else if (key == "BTNRIGHTSTICK" || key == "RANALOG" || key == "RSTICK") key = kbm ? "MOUSE" : (ps ? "R" : "RS");
        else if (key == "BTNDDOWN" || key == "DPADDOWN") key = kbm ? "S" : "D-PAD";
        else if (key == "BTNDLEFT" || key == "DPADLEFT") key = kbm ? "A" : "D-PAD";
        else if (key == "BTNDRIGHT" || key == "DPADRIGHT") key = kbm ? "D" : "D-PAD";
        else if (key == "BTNDUP" || key == "DPADUP" || key == "DPAD" || key == "BTNDPAD") key = kbm ? "W" : "D-PAD";
        else if (key == "CIRCLE") key = kbm ? "LMB" : (ps ? "CIRCLE" : (xb ? "B" : "A"));
        else if (key == "SQUARE") key = kbm ? "SPACE" : (ps ? "SQUARE" : (xb ? "X" : "Y"));
        else if (key == "TRIANGLE") key = kbm ? "F" : (ps ? "TRIANGLE" : (xb ? "Y" : "X"));
        else if (key == "LOOKBEHIND" || key == "EXPLOSION") key = kbm ? "LMB" : (ps ? "CIRCLE" : (xb ? "B" : "A"));
    };
    normalize_gxt_token(k, platformPreset);

    const char* fname = nullptr;
    if (platformPreset == 0) { // Xbox (Monochrome Outline)
        if (k == "VIEW") fname = "xbox_button_view_outline.png";
        else if (k == "MENU") fname = "xbox_button_menu_outline.png";
        else if (k == "LT") fname = "xbox_lt_outline.png";
        else if (k == "LB") fname = "xbox_lb_outline.png";
        else if (k == "RT") fname = "xbox_rt_outline.png";
        else if (k == "RB") fname = "xbox_rb_outline.png";
        else if (k == "D-PAD") fname = "xbox_dpad_none.png";
        else if (k == "LS") fname = "xbox_ls_outline.png";
        else if (k == "RS") fname = "xbox_rs_outline.png";
        else if (k == "L3") fname = "xbox_stick_l_press.png";
        else if (k == "R3") fname = "xbox_stick_r_press.png";
        else if (k == "A") fname = "xbox_button_a_outline.png";
        else if (k == "B") fname = "xbox_button_b_outline.png";
        else if (k == "X") fname = "xbox_button_x_outline.png";
        else if (k == "Y") fname = "xbox_button_y_outline.png";
    } else if (platformPreset == 1 || platformPreset == 2) { // PS5 / PS2 (Monochrome Outline)
        if (k == "TOUCHPAD") fname = "playstation5_touchpad_outline.png";
        else if (k == "OPTIONS") fname = "playstation5_button_options_outline.png";
        else if (k == "SELECT") fname = "playstation3_button_select_outline.png";
        else if (k == "START") fname = "playstation3_button_start_outline.png";
        else if (k == "L1") fname = "playstation_trigger_l1_outline.png";
        else if (k == "L2") fname = "playstation_trigger_l2_outline.png";
        else if (k == "R1") fname = "playstation_trigger_r1_outline.png";
        else if (k == "R2") fname = "playstation_trigger_r2_outline.png";
        else if (k == "D-PAD") fname = "playstation_dpad_none.png";
        else if (k == "L") fname = "playstation_stick_l.png";
        else if (k == "R") fname = "playstation_stick_r.png";
        else if (k == "L3") fname = "playstation_button_l3_outline.png";
        else if (k == "R3") fname = "playstation_button_r3_outline.png";
        else if (k == "X") fname = "playstation_button_cross_outline.png";
        else if (k == "CIRCLE") fname = "playstation_button_circle_outline.png";
        else if (k == "SQUARE") fname = "playstation_button_square_outline.png";
        else if (k == "TRIANGLE") fname = "playstation_button_triangle_outline.png";
    } else if (platformPreset == 4) { // Nintendo Switch
        if (k == "MINUS") fname = "switch_button_minus.png";
        else if (k == "PLUS") fname = "switch_button_plus.png";
        else if (k == "L") fname = "switch_button_l.png";
        else if (k == "ZL") fname = "switch_button_zl.png";
        else if (k == "R") fname = "switch_button_r.png";
        else if (k == "ZR") fname = "switch_button_zr.png";
        else if (k == "D-PAD") fname = "switch_dpad.png";
        else if (k == "LS") fname = "switch_stick_l.png";
        else if (k == "RS") fname = "switch_stick_r.png";
        else if (k == "L3") fname = "switch_stick_l_press.png";
        else if (k == "R3") fname = "switch_stick_r_press.png";
        else if (k == "A") fname = "switch_button_a.png";
        else if (k == "B") fname = "switch_button_b.png";
        else if (k == "X") fname = "switch_button_x.png";
        else if (k == "Y") fname = "switch_button_y.png";
    }

    if (!fname) { // Keyboard & Mouse (and fallback footer tokens)
        if (k == "M") fname = "keyboard_m.png";
        else if (k == "R") fname = "keyboard_r.png";
        else if (k == "SHIFT") fname = "keyboard_shift.png";
        else if (k == "W") fname = "keyboard_w.png";
        else if (k == "A") fname = "keyboard_a.png";
        else if (k == "S") fname = "keyboard_s.png";
        else if (k == "D") fname = "keyboard_d.png";
        else if (k == "F") fname = "keyboard_f.png";
        else if (k == "C") fname = "keyboard_c.png";
        else if (k == "H") fname = "keyboard_h.png";
        else if (k == "Q") fname = "keyboard_q.png";
        else if (k == "E") fname = "keyboard_e.png";
        else if (k == "V") fname = "keyboard_v.png";
        else if (k == "X") fname = "keyboard_x.png";
        else if (k == "SPACE") fname = "keyboard_space.png";
        else if (k == "ESC") fname = "keyboard_escape.png";
        else if (k == "ENTER") fname = "keyboard_enter.png";
        else if (k == "TAB") fname = "keyboard_tab.png";
        else if (k == "BACKSPACE") fname = "keyboard_backspace.png";
        else if (k == "LMB") fname = "mouse_left.png";
        else if (k == "RMB") fname = "mouse_right.png";
        else if (k == "SCROLL") fname = "mouse_scroll.png";
        else if (k == "MOUSE") fname = "mouse_move.png";
    }
    if (!fname) return empty;
    const auto it = lcs::ui_overlay::g_prompt_textures.find(fname);
    return (it != lcs::ui_overlay::g_prompt_textures.end()) ? it->second.tex : empty;
#else
    (void)platformPreset; (void)token;
    return empty;
#endif
}

} // namespace ui::assets
