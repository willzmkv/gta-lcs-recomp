#include "ui/diag_overlay.h"

#include "game/lcs_trainer.h"
#include "game/lcs_world_hd.h"
#include "graphics/texture_pipeline.h"
#include "ui/ui_theme.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <string>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::ui_diag {

namespace {

constexpr size_t kHistory = 300;
std::array<float, kHistory> g_frame_ms{};
size_t g_frame_head = 0;
size_t g_frame_count = 0;
// CWeaponInfo::ms_aWeaponNames (libGame.so), indexed by eWeaponType
constexpr const char* kWeaponNames[] = {
    "Unarmed", "BrassKnuckle", "ScrewDriver", "GolfClub", "NightStick", "Knife", "BaseballBat", "Hammer",
    "Cleaver", "Machete", "Katana", "Chainsaw", "Grenade", "DetonateGrenade", "TearGas", "Molotov", "Rocket",
    "Colt45", "Python", "Shotgun", "Spas12", "Stubby", "Tec9", "Uzi", "SilencedIngram", "Mp5", "M4", "Ruger",
    "SniperRifle", "LaserScope", "RocketLauncher", "FlameThrower", "M60", "Minigun", "Detonator", "HeliCannon",
    "Camera"};

bool g_open = [] {
    const char* e = std::getenv("LCS_DIAG_F3");  // opens the panel at start (automated captures)
    return e && e[0] == '1';
}();

std::string position_line(const lcs::game::DiagSnapshot& d) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f, %.1f", d.pos[0], d.pos[1], d.pos[2], d.heading_deg);
    return buf;
}

struct FrameStats {
    float avg = 0.0f, p50 = 0.0f, p99 = 0.0f, max = 0.0f;
};

FrameStats frame_stats() {
    FrameStats s;
    const size_t n = std::min(g_frame_count, kHistory);
    if (n == 0) return s;
    std::array<float, kHistory> sorted{};
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        sorted[i] = g_frame_ms[i];
        sum += g_frame_ms[i];
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(n));
    s.avg = sum / static_cast<float>(n);
    s.p50 = sorted[n / 2];
    s.p99 = sorted[std::min(n - 1, (n * 99) / 100)];
    s.max = sorted[n - 1];
    return s;
}

} // namespace

bool is_open() { return g_open; }

void toggle() { g_open = !g_open; }

void record_frame(float delta_seconds) {
    g_frame_ms[g_frame_head] = delta_seconds * 1000.0f;
    g_frame_head = (g_frame_head + 1) % kHistory;
    ++g_frame_count;
}

void copy_position(void* hwnd) {
    auto& tr = lcs::game::LCSTrainer::instance();
    const auto d = tr.read_diag();
    if (!d.valid) return;
    const std::string line = position_line(d);
    std::cout << "[F3] pos+heading: " << line << " area=" << d.area << "\n" << std::flush;
#if defined(LCS_PLATFORM_WINDOWS)
    if (OpenClipboard(static_cast<HWND>(hwnd))) {
        EmptyClipboard();
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, line.size() + 1)) {
            std::memcpy(GlobalLock(mem), line.c_str(), line.size() + 1);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_TEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }
#else
    (void)hwnd;
#endif
    tr.push_notice("Coordenadas copiadas: " + line, 2);
}

void draw() {
    if (!g_open) return;
    const auto& tr = lcs::game::LCSTrainer::instance();
    const auto d = tr.read_diag();
    const auto whd = lcs::game::LcsWorldHd::instance().get_stats();
    const auto tex = lcs::graphics::TexturePipeline::instance().stats();
    const FrameStats fs = frame_stats();

    ImGuiIO& io = ImGui::GetIO();
    const float s = std::clamp(io.DisplaySize.y / 900.0f, 0.85f, 2.0f);
    ImGui::SetNextWindowPos(ImVec2(14.0f * s, 14.0f * s), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ui::palette::u32(ui::palette::kHeaderBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ui::palette::u32(ui::palette::kCrimson));
    ImGui::PushStyleColor(ImGuiCol_Text, ui::palette::u32(ui::palette::kWhite));
    ImGui::PushFont(ui::fontRegular(), 14.0f * s);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("##lcs_diag_f3", nullptr, kFlags)) {
        const ImVec4 muted = ImGui::ColorConvertU32ToFloat4(ui::palette::u32(ui::palette::kMuted));
        auto header = [&](const char* t) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::palette::u32(ui::palette::kCrimson));
            ImGui::TextUnformatted(t);
            ImGui::PopStyleColor();
        };

        header("DESEMPENHO");
        ImGui::Text("%.0f FPS   média %.2f ms   p50 %.2f   p99 %.2f   máx %.1f", io.Framerate, fs.avg, fs.p50, fs.p99,
                    fs.max);
        {
            // Oldest to newest, so the graph scrolls left
            std::array<float, kHistory> line{};
            const size_t n = std::min(g_frame_count, kHistory);
            for (size_t i = 0; i < n; ++i) {
                line[i] = g_frame_ms[(g_frame_head + kHistory - n + i) % kHistory];
            }
            ImGui::PlotLines("##ft", line.data(), static_cast<int>(n), 0, "frame ms (0..40)", 0.0f, 40.0f,
                             ImVec2(420.0f * s, 60.0f * s));
        }
        ImGui::Text("Texturas: %llu jobs  %llu do cache  %llu esperas no draw  %.0f Mpx",
                    static_cast<unsigned long long>(tex.jobs), static_cast<unsigned long long>(tex.cache_hits),
                    static_cast<unsigned long long>(tex.waits + tex.inline_runs), tex.pixels / 1.0e6);
        ImGui::Text("HD World: raio %d  nível %d  célula (%d, %d)  %d prontas / %d em cache", whd.radius,
                    whd.active_level, whd.center_cx, whd.center_cy, whd.ready_cells, whd.cached_cells);
        ImGui::Text("          %d instâncias  %d LQ substituídos", whd.combined_instances, whd.lq_replaced);

        header("JOGADOR");
        if (!d.valid) {
            ImGui::TextColored(muted, "fora do jogo");
        } else {
            ImGui::Text("Posição  %.2f, %.2f, %.2f   direção %.1f°", d.pos[0], d.pos[1], d.pos[2], d.heading_deg);
            ImGui::Text("Área %d   relógio %02d:%02d   vida %.0f   colete %.0f   $%d", d.area, d.clock_hour,
                        d.clock_minute, d.health, d.armour, d.money);
            std::string w;
            for (int i = 0; i < d.weapon_count; ++i) {
                char b[48];
                const int t = d.weapons[i].type;
                const char* name = (t >= 0 && t < static_cast<int>(std::size(kWeaponNames))) ? kWeaponNames[t] : "?";
                std::snprintf(b, sizeof(b), "%s%s %d/%d", i ? "  " : "", name, d.weapons[i].clip, d.weapons[i].total);
                w += b;
            }
            ImGui::Text("Armas    %s", w.empty() ? "-" : w.c_str());
            if (d.in_vehicle) {
                ImGui::Text("Veículo  modelo %d   %.0f km/h   lataria %.0f", d.vehicle_model, d.vehicle_speed_kmh,
                            d.vehicle_health);
            } else {
                ImGui::TextColored(muted, "Veículo  a pé");
            }
        }

        header("MUNDO");
        ImGui::Text("Pedestres %d / %d   Veículos %d / %d", d.peds_used, d.peds_cap, d.vehicles_used, d.vehicles_cap);
        ImGui::Spacing();
        ImGui::TextColored(muted, "F3 fechar   Ctrl+F3 copiar coordenadas");
    }
    ImGui::End();
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
}

} // namespace lcs::ui_diag
