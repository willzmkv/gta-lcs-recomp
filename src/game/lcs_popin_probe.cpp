#include "game/lcs_popin_probe.h"

#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <vector>

namespace lcs::game {

namespace {
constexpr uint64_t kGotPedPoolRva = 0x7f9a38ULL;      // CPools::ms_pPedPool
constexpr uint64_t kGotVehiclePoolRva = 0x7f9720ULL;  // CPools::ms_pVehiclePool
constexpr uint64_t kPedStride = 0xdc0ULL;
constexpr uint64_t kVehicleStride = 0x7b0ULL;
constexpr uint64_t kEntityPos = 0x40ULL;
constexpr uint64_t kPedVehicle = 0x610ULL;  // CPed::m_pMyVehicle
constexpr uint8_t kFree = 0x80u;            // CPool flag byte: bit 7 free, low bits generation
} // namespace

PopinProbe& PopinProbe::instance() {
    static PopinProbe s_instance;
    return s_instance;
}

void PopinProbe::init(const lcs::cpu::ElfImage& image_game) {
    m_load_bias = image_game.get_load_bias();
    m_fn_on_screen = image_game.find_symbol("_ZN7CEntity13GetIsOnScreenEv");
    m_fn_player_ped = image_game.find_symbol("_Z13FindPlayerPedv");
    m_fn_player_vehicle = image_game.find_symbol("_Z17FindPlayerVehiclev");
}

void PopinProbe::scan_pool(lcs::cpu::DynarmicCore& cpu, uint64_t got_rva, uint64_t stride, uint8_t* prev,
                           Counts& out, const float* player, uint64_t skip_a, uint64_t skip_b, bool is_ped) {
    const uint64_t var = *reinterpret_cast<const uint64_t*>(m_load_bias + got_rva);
    const uint64_t pool = var ? *reinterpret_cast<const uint64_t*>(var) : 0;
    if (!pool) return;
    const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool + 0x00);
    const uint64_t flags = *reinterpret_cast<const uint64_t*>(pool + 0x08);
    const int size = std::min(*reinterpret_cast<const int32_t*>(pool + 0x10), 1024);
    if (!objs || !flags) return;
    ++out.ticks;
    for (int i = 0; i < size; ++i) {
        const uint8_t f = *reinterpret_cast<const uint8_t*>(flags + i);
        const uint8_t before = prev[i];
        prev[i] = f;
        if (f & kFree) continue;
        const uint64_t e = objs + static_cast<uint64_t>(i) * stride;
        if (e == skip_a || e == skip_b) continue;
        if (is_ped && *reinterpret_cast<const uint64_t*>(e + kPedVehicle) != 0) continue;  // drivers/passengers
        const float* p = reinterpret_cast<const float*>(e + kEntityPos);
        const float d = std::hypot(p[0] - player[0], p[1] - player[1]);
        if (d < 100.0f) out.near100 += 1.0;
        if (d < 200.0f) out.near200 += 1.0;
        if (!m_primed || f == before) continue;
        const bool on_screen = m_fn_on_screen && (cpu.call_nested(m_fn_on_screen, {e}) & 1u);
        if (on_screen) {
            out.in_view[d < 50.0f ? 0 : d < 100.0f ? 1 : d < 150.0f ? 2 : 3]++;
            out.min_in_view = std::min(out.min_in_view, d);
        } else {
            out.out_view[d < 30.0f ? 0 : d < 60.0f ? 1 : 2]++;
        }
    }
}

void PopinProbe::tick(lcs::cpu::DynarmicCore& cpu) {
    if (!m_load_bias || !m_fn_player_ped) return;
    const uint64_t ped = cpu.call_nested(m_fn_player_ped, {});
    if (!ped) return;
    const uint64_t veh = m_fn_player_vehicle ? cpu.call_nested(m_fn_player_vehicle, {}) : 0;
    const float* player = reinterpret_cast<const float*>(ped + kEntityPos);
    m_player_entity = veh ? veh : ped;
    scan_pool(cpu, kGotPedPoolRva, kPedStride, m_prev_peds, m_peds, player, ped, veh, true);
    scan_pool(cpu, kGotVehiclePoolRva, kVehicleStride, m_prev_vehicles, m_vehicles, player, ped, veh, false);
    m_primed = true;
}

void PopinProbe::tick_visible() {
    if (!m_load_bias) return;
    const int32_t n = std::min(*reinterpret_cast<const int32_t*>(m_load_bias + 0x85ad6cULL), 4000);  // ms_nNoOfVisibleEntities
    const auto* list = reinterpret_cast<const uint64_t*>(m_load_bias + 0x85ad80ULL);                 // ms_aVisibleEntityPtrs
    const uint64_t s_player = m_player_entity;
    if (!s_player) return;
    const float* pp = reinterpret_cast<const float*>(s_player + kEntityPos);
    const float* m = reinterpret_cast<const float*>(s_player + 0x10ULL);
    const float fx = m[4], fy = m[5];
    const float fl = std::hypot(fx, fy);
    // Positions (0.5 m grid) of last frame's visible entities: a new entity where one was
    // already drawn is a swap (dummy -> real object, LOD -> HD), not a pop-in.
    auto pos_key = [](const float* p) -> uint64_t {
        return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(p[0] * 2.0f))) << 32) |
               static_cast<uint32_t>(static_cast<int32_t>(p[1] * 2.0f));
    };
    std::unordered_set<uint64_t> cur;
    std::unordered_set<uint64_t> cur_pos;
    cur.reserve(static_cast<size_t>(n) * 2);
    cur_pos.reserve(static_cast<size_t>(n) * 2);
    for (int32_t i = 0; i < n; ++i) {
        const uint64_t e = list[i];
        if (!e) continue;
        cur.insert(e);
        cur_pos.insert(pos_key(reinterpret_cast<const float*>(e + kEntityPos)));
        if (m_prev_visible.empty() || m_prev_visible.count(e)) continue;
        if (m_prev_positions.count(pos_key(reinterpret_cast<const float*>(e + kEntityPos)))) {
            ++m_vis_swaps;
            continue;
        }
        const float* p = reinterpret_cast<const float*>(e + kEntityPos);
        const float dx = p[0] - pp[0], dy = p[1] - pp[1];
        const float d = std::hypot(dx, dy);
        if (d < 20.0f || d > 600.0f || fl < 0.1f) continue;
        const float cosang = (dx * fx + dy * fy) / (d * fl);
        if (cosang < 0.94f) continue;  // outside ~20 deg of the heading: entered from the screen edge
        auto& vp = m_vis_pops[*reinterpret_cast<const int16_t*>(e + 0x7cULL)];
        ++vp.count;
        vp.dist_sum += d;
        vp.dmin = std::min(vp.dmin, d);
        vp.dmax = std::max(vp.dmax, d);
    }
    m_prev_visible.swap(cur);
    m_prev_positions.swap(cur_pos);
}

void PopinProbe::report_visible() {
    std::vector<std::pair<int, VisPop>> v(m_vis_pops.begin(), m_vis_pops.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
    int total = 0;
    for (const auto& [id, vp] : v) total += vp.count;
    std::cout << "  [VisPop] entities entering the visible list near screen centre: " << total << '\n';
    for (size_t i = 0; i < v.size() && i < 25; ++i) {
        const auto& vp = v[i].second;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "  [VisPop] model %d: %d times, distance avg %.0f min %.0f max %.0f\n",
                      v[i].first, vp.count, vp.dist_sum / vp.count, vp.dmin, vp.dmax);
        std::cout << buf;
    }
    std::cout << std::flush;
    m_vis_pops.clear();
}

void PopinProbe::report(const std::string& label, double seconds) {
    auto line = [&](const char* kind, Counts& c) {
        const double per_min = seconds > 0.0 ? 60.0 / seconds : 0.0;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "  [Popin] %s %s: in view <50m %d, 50-100 %d, 100-150 %d, 150+ %d (min %.0f m) | "
                      "out of view <30m %d, 30-60 %d, 60+ %d | in view <100m per minute %.1f | "
                      "alive <100m %.1f <200m %.1f\n",
                      label.c_str(), kind, c.in_view[0], c.in_view[1], c.in_view[2], c.in_view[3],
                      c.min_in_view < 1e8f ? c.min_in_view : -1.0f, c.out_view[0], c.out_view[1], c.out_view[2],
                      (c.in_view[0] + c.in_view[1]) * per_min, c.ticks ? c.near100 / c.ticks : 0.0,
                      c.ticks ? c.near200 / c.ticks : 0.0);
        std::cout << buf;
        c = Counts{};
    };
    line("peds", m_peds);
    line("vehicles", m_vehicles);
    std::cout << std::flush;
}

} // namespace lcs::game
