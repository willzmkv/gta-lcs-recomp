#include "game/lcs_memory.h"
#include "game/lcs_trainer.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "settings/settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::game {

namespace {

constexpr uint64_t kGotDeleteFarAwayRva         = 0x806E10ULL; // CStreaming::DeleteFarAwayRslObjects(const CVector&)
constexpr uint64_t kGotRemoveLeastUsedRva       = 0x806EB0ULL; // CStreaming::RemoveLeastUsedModel(uint32_t)
constexpr uint64_t kGotDeleteBehindSectorRva    = 0x806ED0ULL; // CStreaming::DeleteRslObjectsBehindCameraInSectorList
constexpr uint64_t kGotStreamingUpdateRva       = 0x802B50ULL; // CStreaming::Update()
constexpr uint64_t kGotProcessLoadingChannelRva = 0x806DF8ULL; // CStreaming::ProcessLoadingChannel(int)
constexpr uint64_t kGotRequestModelStreamRva    = 0x806E28ULL; // CStreaming::RequestModelStream(int)
constexpr uint64_t kGotCdStreamSyncRva          = 0x806DF0ULL; // CdStreamSync(int)
constexpr uint64_t kGotStreamingNumRequestedRva = 0x7FA2F8ULL; // int32_t CStreaming::ms_numModelsRequested
constexpr uint64_t kGotStreamingChannelRva      = 0x7FC5D0ULL; // tStreamingChannel CStreaming::ms_channel[2]
constexpr uint64_t kGotStreamingChannelErrRva   = 0x7FC5C0ULL; // int32_t CStreaming::ms_channelError
constexpr uint64_t kGotDvLoadReqModelsRva       = 0x7FC5F8ULL; // dvDoLoadRequestedModels
constexpr uint64_t kGotDisableStreamingRva      = 0x7F9178ULL; // uint8_t CStreaming::ms_disableStreaming
constexpr uint64_t kGotCutsceneProcRva          = 0x7F9030ULL; // uint8_t CCutsceneMgr::ms_cutsceneProcessing
constexpr uint64_t kGotUserPauseRva             = 0x7F92B8ULL; // uint8_t CTimer::m_UserPause
constexpr uint64_t kGotCodePauseRva             = 0x7F9618ULL; // uint8_t CTimer::m_CodePause

constexpr uint64_t kGotPedPoolRva               = 0x7f9a38ULL;
constexpr uint64_t kGotVehiclePoolRva           = 0x7f9720ULL;
constexpr uint64_t kGotObjectPoolRva            = 0x7f9a40ULL;
constexpr uint64_t kGotPtrNodePoolRva           = 0x7f9ee0ULL;
constexpr uint64_t kGotEntryInfoNodePoolRva     = 0x7f9960ULL;

lcs::cpu::DynarmicCore* s_mem_cpu = nullptr;
uint64_t s_mem_load_bias = 0;
uint64_t s_addr_cd_sync = 0;
uint64_t s_addr_process_channel = 0;
uint64_t s_addr_request_stream = 0;
uint64_t s_orig_streaming_update = 0;

template <typename T>
inline T* read_got_ptr(uint64_t load_bias, uint64_t got_rva) {
    const uint64_t slot = *reinterpret_cast<const uint64_t*>(load_bias + got_rva);
    return reinterpret_cast<T*>(slot);
}

// Pump additional async CD streaming requests per tick before CStreaming::Update() purges
// volatile (flags=0) requests at the end of the update step, preventing 1-model/tick stalls.
void pump_streaming_queue() {
    static thread_local bool s_in_pump = false;
    if (s_in_pump || !s_mem_cpu || !s_mem_load_bias) return;
    if (!s_addr_cd_sync || !s_addr_process_channel || !s_addr_request_stream) return;

    const int mem_mb = std::clamp(lcs::settings::get().streaming_memory, 64, 512);
    if (mem_mb <= 64) return;

    const auto* user_pause = read_got_ptr<uint8_t>(s_mem_load_bias, kGotUserPauseRva);
    const auto* code_pause = read_got_ptr<uint8_t>(s_mem_load_bias, kGotCodePauseRva);
    const auto* disable_st = read_got_ptr<uint8_t>(s_mem_load_bias, kGotDisableStreamingRva);
    const auto* cut_proc   = read_got_ptr<uint8_t>(s_mem_load_bias, kGotCutsceneProcRva);
    const auto* dv_do_load = read_got_ptr<uint8_t>(s_mem_load_bias, kGotDvLoadReqModelsRva);
    if ((user_pause && *user_pause) || (code_pause && *code_pause)) return;
    if ((disable_st && *disable_st) || (cut_proc && *cut_proc)) return;
    if (dv_do_load && dv_do_load[0x3d] == 0) return;

    const auto* num_req  = read_got_ptr<int32_t>(s_mem_load_bias, kGotStreamingNumRequestedRva);
    const auto* chan_err = read_got_ptr<int32_t>(s_mem_load_bias, kGotStreamingChannelErrRva);
    const auto* ch0_base = read_got_ptr<uint8_t>(s_mem_load_bias, kGotStreamingChannelRva);
    if (!num_req || !chan_err || !ch0_base) return;
    if (*num_req <= 0 || *chan_err != -1) return;

    const auto* ch0_state = reinterpret_cast<const int32_t*>(ch0_base + 0x10ULL);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kMaxModelsPerTick = 12;
    constexpr auto kMaxPumpDuration = std::chrono::microseconds(1500);

    auto* cur_cpu = lcs::cpu::DynarmicCore::current();
    auto* cpu = cur_cpu ? cur_cpu : s_mem_cpu;
    if (!cpu) return;

    s_in_pump = true;
    for (int iter = 0; iter < kMaxModelsPerTick; ++iter) {
        if (iter > 0 && (std::chrono::steady_clock::now() - t0) >= kMaxPumpDuration) break;
        if (*num_req <= 0 || *chan_err != -1) break;

        if (*ch0_state == 1) {
            cpu->call_nested(s_addr_cd_sync, {0ULL});
            cpu->call_nested(s_addr_process_channel, {0ULL});
        }
        if (*ch0_state == 0 && *chan_err == -1 && *num_req > 0) {
            cpu->call_nested(s_addr_request_stream, {0ULL});
            if (*ch0_state != 1) break;
        } else {
            break;
        }
    }
    s_in_pump = false;
}

#pragma pack(push, 1)
struct MemPoolHeader {
    uint64_t m_pObjects;
    uint64_t m_byteMap;
    int32_t  m_nSize;
    int32_t  m_nFirstFree;
};
#pragma pack(pop)

inline uint32_t ldr_x16_rel(size_t pc_off, size_t lit_off) {
    const int64_t delta_words = static_cast<int64_t>(lit_off - pc_off) >> 2;
    return 0x58000010u | ((static_cast<uint32_t>(delta_words) & 0x7ffffu) << 5);
}

uint64_t patch_got_mem(lcs::cpu::DynarmicCore& cpu, uint64_t load_bias, uint64_t got_rva,
                       uint64_t new_target, const lcs::cpu::ElfImage& img, const char* sym) {
    const uint64_t slot = load_bias + got_rva;
    uint64_t orig = *reinterpret_cast<const uint64_t*>(slot);
    if (!orig && sym) orig = img.find_symbol(sym);
#if defined(LCS_PLATFORM_WINDOWS)
    DWORD old_prot = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uint64_t), PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(reinterpret_cast<void*>(slot), &new_target, sizeof(uint64_t));
    VirtualProtect(reinterpret_cast<void*>(slot), sizeof(uint64_t), old_prot, &old_prot);
#else
    std::memcpy(reinterpret_cast<void*>(slot), &new_target, sizeof(uint64_t));
#endif
    cpu.invalidate_cache(slot, sizeof(uint64_t));
    return orig;
}

float pool_usage_ratio(uint64_t load_bias, uint64_t got_rva) {
    const uint64_t pp = *reinterpret_cast<const uint64_t*>(load_bias + got_rva);
    if (!pp) return 0.0f;
    const auto* pool = *reinterpret_cast<const MemPoolHeader* const*>(pp);
    if (!pool || !pool->m_byteMap || pool->m_nSize <= 0) return 0.0f;
    const auto* flags = reinterpret_cast<const uint8_t*>(pool->m_byteMap);
    int used = 0;
    for (int i = 0; i < pool->m_nSize; ++i) {
        if ((flags[i] & 0x80u) == 0) ++used;
    }
    return static_cast<float>(used) / static_cast<float>(pool->m_nSize);
}

} // namespace

struct LcsMemory::HookOps {
    static uint64_t allow_far(uint64_t vec_ptr) {
        return LcsMemory::instance().should_allow_far_delete(vec_ptr);
    }
    static uint64_t allow_behind(uint64_t /*a0*/) {
        return LcsMemory::instance().should_allow_behind_cam_delete();
    }
    static uint64_t allow_least(uint64_t /*a0*/) {
        return LcsMemory::instance().should_allow_remove_least_used();
    }
};

LcsMemory::~LcsMemory() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (m_stub_page) {
        VirtualFree(m_stub_page, 0, MEM_RELEASE);
        m_stub_page = nullptr;
    }
#endif
}

LcsMemory& LcsMemory::instance() {
    static LcsMemory s_instance;
    return s_instance;
}

void LcsMemory::install(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    if (m_installed) return;
    m_load_bias = image_game.get_load_bias();

#if defined(LCS_PLATFORM_WINDOWS)
    m_stub_page = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    m_stub_page = static_cast<uint8_t*>(std::aligned_alloc(4096, 0x1000));
#endif
    if (!m_stub_page) {
        std::cerr << "  [Memory] WARN: Failed to allocate streaming guard page.\n";
        return;
    }
    std::memset(m_stub_page, 0, 0x1000);

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t cb_far = hle.register_function("__lcs_mem_allow_far",
        [](const uint64_t* a) -> uint64_t { return HookOps::allow_far(a[0]); });
    const uint64_t cb_behind = hle.register_function("__lcs_mem_allow_behind",
        [](const uint64_t* a) -> uint64_t { return HookOps::allow_behind(a[0]); });
    const uint64_t cb_least = hle.register_function("__lcs_mem_allow_least",
        [](const uint64_t* a) -> uint64_t { return HookOps::allow_least(a[0]); });

    const uint64_t base_va = reinterpret_cast<uint64_t>(m_stub_page);
    size_t cursor = 0;

    // Gated tail-call wrapper: save x0..x1, call guard(x0). If 0 -> return 0 immediately; else br orig.
    auto emit_gated = [&](uint64_t got_rva, const char* sym, uint64_t guard_va, uint64_t& out_orig) {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        out_orig = patch_got_mem(cpu, m_load_bias, got_rva, stub_va, image_game, sym);

        const uint32_t insns[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90107e0u, // 0x0c: stp x0, x1, [sp, #0x10]
            ldr_x16_rel(0x10, 0x40),
            0xd63f0200u, // 0x14: blr x16 (guard_va)
            0x35000080u, // 0x18: cbnz w0, +0x10 (0x28 proceed)
            0x2a1f03e0u, // 0x1c: mov w0, wzr
            0xa9407bfdu, // 0x20: ldp x29, x30, [sp]
            0x910083ffu, // 0x24: add sp, sp, #0x20  (note overlap: return after 0x24 -> place ret cleanly!)
            0xd65f03c0u, // 0x28: ret -- ADJUST branch target!
            0xa94107e0u,
            0xa9407bfdu,
            0x910083ffu,
            ldr_x16_rel(0x38, 0x48),
            0xd61f0200u
        };
        (void)insns;
        const uint32_t insns_exact[16] = {
            0xd10083ffu, // 0x00: sub sp, sp, #0x20
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            0xa90107e0u, // 0x0c: stp x0, x1, [sp, #0x10]
            ldr_x16_rel(0x10, 0x40),
            0xd63f0200u, // 0x14: blr x16 (guard_va)
            0x350000a0u, // 0x18: cbnz w0, +0x14 (0x2c proceed)
            0x2a1f03e0u, // 0x1c: mov w0, wzr
            0xa9407bfdu, // 0x20: ldp x29, x30, [sp]
            0x910083ffu, // 0x24: add sp, sp, #0x20
            0xd65f03c0u, // 0x28: ret
            0xa94107e0u, // 0x2c: ldp x0, x1, [sp, #0x10]
            0xa9407bfdu, // 0x30: ldp x29, x30, [sp]
            0x910083ffu, // 0x34: add sp, sp, #0x20
            ldr_x16_rel(0x38, 0x48),
            0xd61f0200u, // 0x3c: br x16 (orig_fn)
        };
        std::memcpy(code, insns_exact, sizeof(insns_exact));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x40ULL);
        lits[0] = guard_va;
        lits[1] = out_orig;
        cursor += 0x50ULL;
    };

    emit_gated(kGotDeleteFarAwayRva, "_ZN10CStreaming23DeleteFarAwayRslObjectsERK7CVector",
               cb_far, m_orig_delete_far);
    emit_gated(kGotDeleteBehindSectorRva,
               "_ZN10CStreaming40DeleteRslObjectsBehindCameraInSectorListER8CPtrListi",
               cb_behind, m_orig_delete_behind);
    emit_gated(kGotRemoveLeastUsedRva, "_ZN10CStreaming20RemoveLeastUsedModelEj",
               cb_least, m_orig_remove_least);

    s_mem_cpu = &cpu;
    s_mem_load_bias = m_load_bias;
    s_addr_cd_sync = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotCdStreamSyncRva);
    s_addr_process_channel = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotProcessLoadingChannelRva);
    s_addr_request_stream = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotRequestModelStreamRva);

    const uint64_t cb_stream_pre = hle.register_function("__lcs_mem_streaming_pre",
        [](const uint64_t*) -> uint64_t {
            pump_streaming_queue();
            return 0;
        });
    const uint64_t cb_stream_post = hle.register_function("__lcs_mem_streaming_post",
        [](const uint64_t*) -> uint64_t {
            LCSTrainer::instance().drain_cpu_actions();
            return 0;
        });
    {
        const uint64_t stub_va = base_va + cursor;
        auto* code = reinterpret_cast<uint32_t*>(m_stub_page + cursor);
        s_orig_streaming_update = patch_got_mem(
            cpu, m_load_bias, kGotStreamingUpdateRva, stub_va, image_game,
            "_ZN10CStreaming6UpdateEv");
        const uint32_t insns_pre_post[14] = {
            0xd10043ffu, // 0x00: sub sp, sp, #0x10
            0xa9007bfdu, // 0x04: stp x29, x30, [sp]
            0x910003fdu, // 0x08: mov x29, sp
            ldr_x16_rel(0x0c, 0x38),
            0xd63f0200u, // 0x10: blr x16 (cb_stream_pre)
            ldr_x16_rel(0x14, 0x40),
            0xd63f0200u, // 0x18: blr x16 (orig CStreaming::Update)
            ldr_x16_rel(0x1c, 0x48),
            0xd63f0200u, // 0x20: blr x16 (cb_stream_post -> drain LCSTrainer actions on gGameTickThread)
            0xa9407bfdu, // 0x24: ldp x29, x30, [sp]
            0x910043ffu, // 0x28: add sp, sp, #0x10
            0xd65f03c0u, // 0x2c: ret
            0xd503201fu, 0xd503201fu
        };
        std::memcpy(code, insns_pre_post, sizeof(insns_pre_post));
        auto* lits = reinterpret_cast<uint64_t*>(m_stub_page + cursor + 0x38ULL);
        lits[0] = cb_stream_pre;
        lits[1] = s_orig_streaming_update;
        lits[2] = cb_stream_post;
        cursor += 0x58ULL;
    }

    cpu.invalidate_cache(base_va, 0x1000);
    m_installed = true;
    std::cout << "  [Memory] Installed adaptive CStreaming retention manager (budget="
              << lcs::settings::get().streaming_memory << " MB).\n";
}

float LcsMemory::measure_pool_pressure() const {
    float p = 0.0f;
    p = std::max(p, pool_usage_ratio(m_load_bias, kGotPedPoolRva));
    p = std::max(p, pool_usage_ratio(m_load_bias, kGotVehiclePoolRva));
    p = std::max(p, pool_usage_ratio(m_load_bias, kGotObjectPoolRva));
    p = std::max(p, pool_usage_ratio(m_load_bias, kGotPtrNodePoolRva));
    p = std::max(p, pool_usage_ratio(m_load_bias, kGotEntryInfoNodePoolRva));
    return p;
}

uint64_t LcsMemory::should_allow_far_delete(uint64_t vec_ptr) {
    const int mem_mb = std::clamp(lcs::settings::get().streaming_memory, 64, 512);
    if (mem_mb <= 64) return 1;

    ++m_tick_counter;
    m_last_pressure = measure_pool_pressure();
    const float thresh = (mem_mb >= 256) ? 0.85f : (mem_mb >= 128 ? 0.72f : 0.60f);
    if (m_last_pressure >= thresh) return 1;

    // Allow immediate cleanup only on an actual single-tick teleport (> 120m jump in 1 frame),
    // never from accumulated normal driving distance.
    if (vec_ptr) {
        const float px = *reinterpret_cast<const float*>(vec_ptr);
        const float py = *reinterpret_cast<const float*>(vec_ptr + 4ULL);
        bool jumped = false;
        if (m_has_last_pos) {
            const float dx = px - m_last_jump_x;
            const float dy = py - m_last_jump_y;
            jumped = (dx * dx + dy * dy > 120.0f * 120.0f);
        }
        m_last_jump_x = px;
        m_last_jump_y = py;
        m_has_last_pos = true;
        if (jumped) return 1;
    }

    // Preserve loaded entity RslObjects at >= 128 MB until pool pressure exceeds threshold
    // so props/lamps/trees/facades stay instanced across the entire extended draw range.
    if (mem_mb >= 128) return 0;
    return (m_tick_counter % 4ULL == 0) ? 1ULL : 0ULL;
}

uint64_t LcsMemory::should_allow_behind_cam_delete() {
    const int mem_mb = std::clamp(lcs::settings::get().streaming_memory, 64, 512);
    if (mem_mb <= 64) return 1;
    m_last_pressure = measure_pool_pressure();
    const float thresh = (mem_mb >= 256) ? 0.85f : 0.75f;
    return (m_last_pressure >= thresh) ? 1ULL : 0ULL;
}

uint64_t LcsMemory::should_allow_remove_least_used() {
    const int mem_mb = std::clamp(lcs::settings::get().streaming_memory, 64, 512);
    if (mem_mb <= 64) return 1;
    m_last_pressure = measure_pool_pressure();
    const float thresh = (mem_mb >= 512) ? 0.90f : (mem_mb >= 256 ? 0.85f : 0.78f);
    return (m_last_pressure >= thresh) ? 1ULL : 0ULL;
}

} // namespace lcs::game
