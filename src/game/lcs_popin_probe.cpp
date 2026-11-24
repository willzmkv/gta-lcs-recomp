#include "game/lcs_popin_probe.h"

#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

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
    scan_pool(cpu, kGotPedPoolRva, kPedStride, m_prev_peds, m_peds, player, ped, veh, true);
    scan_pool(cpu, kGotVehiclePoolRva, kVehicleStride, m_prev_vehicles, m_vehicles, player, ped, veh, false);
    m_primed = true;
}

void PopinProbe::report(const std::string& label, double seconds) {
    auto line = [&](const char* kind, Counts& c) {
        const double per_min = seconds > 0.0 ? 60.0 / seconds : 0.0;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "  [Popin] %s %s: in view <50m %d, 50-100 %d, 100-150 %d, 150+ %d (min %.0f m) | "
                      "out of view <30m %d, 30-60 %d, 60+ %d | in view <100m per minute %.1f\n",
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
