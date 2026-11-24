#include "game/lcs_trainer.h"

#include "cpu/callbacks.h"
#include "cpu/dynarmic_core.h"
#include "cpu/elf_loader.h"
#include "game/lcs_dialogue_speakers.h"
#include "game/lcs_subtitles.h"
#include "settings/settings.h"
#include "ui/ui_loc.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>

namespace lcs::game {

namespace {
// Disassembly-verified ARM64 offsets in libGame.so (arm64-v8a)
constexpr uint64_t kGotPlayerIdxRva       = 0x7f9118ULL;
constexpr uint64_t kGotPlayersArrRva      = 0x7f9120ULL;
constexpr uint64_t kPlayerInfoStride      = 0x1a0ULL;
constexpr uint64_t kPlayerInfoMoneyOffset = 0xe0ULL;  // int32_t m_nMoney (from MoneyCheat)
constexpr uint64_t kPlayerInfoMaxHpOffset = 0x18fULL; // uint8_t m_nMaxHealth
constexpr uint64_t kPlayerInfoMaxArOffset = 0x190ULL; // uint8_t m_nMaxArmour

constexpr uint64_t kEntityMatrixOffset    = 0x10ULL;  // CMatrix (right/up/at/pos)
constexpr uint64_t kEntityPosOffset       = 0x40ULL;  // CVector position (CPlaceable)
constexpr uint64_t kPedHealthOffset       = 0x5a0ULL; // float m_fHealth (from HealthCheat)
constexpr uint64_t kPedArmourOffset       = 0x5a4ULL; // float m_fArmour (from ArmourCheat)
constexpr uint64_t kPedVehiclePtrOffset   = 0x610ULL; // CVehicle* m_pMyVehicle (from FindPlayerVehicle)
constexpr uint64_t kPedInVehicleFlag      = 0x618ULL; // uint8_t bInVehicle (from FindPlayerVehicle)
constexpr uint64_t kPedWeaponsOffset      = 0x6a8ULL; // CWeapon[10], stride 0x1c (from CPed::GiveWeapon)
constexpr uint64_t kPedWeaponStride       = 0x1cULL;
constexpr uint64_t kPedStaminaOffset      = 0x7ecULL; // float m_fSprintEnergy

constexpr uint64_t kVehMoveSpeedOffset    = 0x90ULL;  // CVector m_vecMoveSpeed (verified at 0x4cb778)
constexpr uint64_t kVehHealthOffset       = 0x2e0ULL; // float m_fHealth (1000.0 = max, from HealthCheat)

constexpr uint64_t kVehPoolStride         = 0x7b0ULL; // CRadar::DrawEntityBlip
constexpr uint64_t kPedPoolStride         = 0xdc0ULL;
constexpr uint64_t kObjPoolStride         = 0x270ULL;

double now_seconds() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

bool is_readable_ptr(uint64_t addr, size_t min_bytes = 2) {
    if (addr < 0x10000ULL) return false;
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xffu;
    if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD) != 0) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + min_bytes <= static_cast<uint64_t>(end);
#else
    (void)min_bytes;
    return true;
#endif
}

uint32_t decode_rockstar_cp(uint16_t c) {
    static constexpr uint32_t kMap[96] = {
        0x00C0,0x00C1,0x00C2,0x00C3,0x00C4,0x00C5,0x00C6,0x00C7,0x00C8,0x00C9,0x00CA,0x00CB,0x00CC,0x00CD,0x00CE,0x00CF,
        0x00D0,0x00D1,0x00D2,0x00D3,0x00D4,0x00D5,0x00D6,0x00D7,0x00D8,0x00D9,0x00DA,0x00DB,0x00DC,0x00DD,0x00DE,0x00DF,
        0x00E0,0x00E1,0x00E2,0x00E3,0x00E4,0x00E5,0x00E6,0x00E7,0x00E8,0x00E9,0x00EA,0x00EB,0x00EC,0x00ED,0x00EE,0x00EF,
        0x00F0,0x00F1,0x00F2,0x00F3,0x00F4,0x00F5,0x00F6,0x00F7,0x00F8,0x00F9,0x00FA,0x00FB,0x00FC,0x00FD,0x00FE,0x00FF,
        0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,
        0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x0020,0x00A1,0x00BF,0x0020,0x2122,0x0020,0x0020,0x0020,0x0020,0x0020,
    };
    if (c >= 0x80u && c < 0xE0u) return kMap[c - 0x80u];
    return c;
}

void append_utf8(std::string& out, uint32_t cp) {
    cp = decode_rockstar_cp(static_cast<uint16_t>(cp));
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

std::string utf16_to_utf8_plain(const uint16_t* u16, size_t max_chars = 256) {
    if (!u16 || !is_readable_ptr(reinterpret_cast<uint64_t>(u16), 2)) return {};
    const int lang = lcs::settings::get().game_language;
    std::string out;
    for (size_t i = 0; i < max_chars; ++i) {
        const uint16_t c = u16[i];
        if (c == 0) break;
        if (c == '~') {
            size_t j = i + 1;
            while (j < max_chars && j - i < 16 && u16[j] != 0 && u16[j] != '~') ++j;
            if (j < max_chars && u16[j] == '~') {
                if (j == i + 2 && (u16[i + 1] == 'n' || u16[i + 1] == 'N')) out.push_back('\n');
                i = j;
                continue;
            }
        }
        lcs::subtitles::append_decoded_u16_char(c, lang, out);
    }
    return out;
}

std::string format_gta_utf16(const uint16_t* u16, const int32_t* nums, int max_nums,
                             const uint16_t* sub_u16, bool keep_newlines = false) {
    if (!u16 || !is_readable_ptr(reinterpret_cast<uint64_t>(u16), 2)) return {};
    const int lang = lcs::settings::get().game_language;
    std::string out;
    int num_idx = 0;
    for (size_t i = 0; i < 512; ++i) {
        const uint16_t c = u16[i];
        if (c == 0) break;
        if (c == '~') {
            size_t j = i + 1;
            while (j < 512 && j - i < 20 && u16[j] != 0 && u16[j] != '~') ++j;
            if (j < 512 && u16[j] == '~') {
                const size_t len = j - (i + 1);
                if (len == 1) {
                    const uint16_t tag = u16[i + 1];
                    if (tag == '1') {
                        if (nums && num_idx < max_nums && nums[num_idx] != -1) {
                            out += std::to_string(nums[num_idx]);
                        }
                        ++num_idx;
                    } else if (tag == 'a' || tag == 'A' || tag == 's') {
                        if (sub_u16) out += utf16_to_utf8_plain(sub_u16, 128);
                    } else if (tag == 'S') {
                        if (sub_u16) out += utf16_to_utf8_plain(sub_u16, 128);
                        else out += "::SQUARE::";
                    } else if (tag == 'X') {
                        out += "::CROSS::";
                    } else if (tag == 'O') {
                        out += "::CIRCLE::";
                    } else if (tag == 'T') {
                        out += "::TRIANGLE::";
                    } else if (tag == 'n' || tag == 'N') {
                        out.push_back(keep_newlines ? '\n' : ' ');
                    } else if (tag == 'k' || tag == 'K') {
                        if (j + 1 < 512 && u16[j + 1] == '~') {
                            size_t k = j + 2;
                            std::string keyname;
                            while (k < 512 && k - j < 32 && u16[k] != 0 && u16[k] != '~') {
                                if (u16[k] < 128) keyname.push_back(static_cast<char>(u16[k]));
                                ++k;
                            }
                            if (k < 512 && u16[k] == '~') {
                                j = k;
                                if (!keyname.empty()) {
                                    out += "::" + keyname + "::";
                                }
                            }
                        }
                    }
                }
                i = j;
                continue;
            }
        }
        lcs::subtitles::append_decoded_u16_char(c, lang, out);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return out;
}

struct TextKeySlot {
    uint64_t ptr = 0;
    char key[12]{};
    uint64_t seq = 0;
};

std::mutex g_brief_cap_mutex;
std::array<TextKeySlot, 64> g_text_key_ring{};
size_t g_text_key_head = 0;
uint64_t g_text_key_seq = 0;
std::vector<RealBriefEntry> g_captured_briefs;
uint64_t g_brief_load_bias = 0;
uint64_t g_cutscene_proc_addr = 0;
uint64_t g_cutscene_run_ptr_addr = 0;

void clear_brief_capture() {
    std::lock_guard<std::mutex> lk(g_brief_cap_mutex);
    g_captured_briefs.clear();
}

std::string lookup_recent_key_locked(uint64_t ptr) {
    uint64_t best_seq = 0;
    std::string best;
    for (const auto& s : g_text_key_ring) {
        if (s.ptr == ptr && s.seq > best_seq && s.key[0] != '\0') {
            best_seq = s.seq;
            best = s.key;
        }
    }
    return best;
}

void push_brief_entry_locked(RealBriefEntry entry) {
    if (entry.text.empty()) return;
    if (!g_captured_briefs.empty()) {
        const auto& last = g_captured_briefs.back();
        if (last.category == entry.category && last.text == entry.text) return;
    }
    // Avoid recent duplicates for repeating objective reminders within the last 8 entries
    const size_t check_start = g_captured_briefs.size() > 8 ? g_captured_briefs.size() - 8 : 0;
    for (size_t i = check_start; i < g_captured_briefs.size(); ++i) {
        if (g_captured_briefs[i].category == entry.category && g_captured_briefs[i].text == entry.text) {
            return;
        }
    }
    g_captured_briefs.push_back(std::move(entry));
    if (g_captured_briefs.size() > 240) {
        g_captured_briefs.erase(g_captured_briefs.begin(), g_captured_briefs.begin() + 40);
    }
}

void on_text_get_post(uint64_t ret_u16_va, const char* key) {
    if (!ret_u16_va || !key || !*key) return;
    std::lock_guard<std::mutex> lk(g_brief_cap_mutex);
    auto& slot = g_text_key_ring[g_text_key_head % g_text_key_ring.size()];
    slot.ptr = ret_u16_va;
    std::strncpy(slot.key, key, sizeof(slot.key) - 1);
    slot.key[sizeof(slot.key) - 1] = '\0';
    slot.seq = ++g_text_key_seq;
    ++g_text_key_head;
}

void on_brief_msg_call(int kind, const uint64_t* regs) {
    if (!regs || !regs[0]) return;
    const uint64_t text_va = regs[0];
    if (!is_readable_ptr(text_va, 2)) return;
    const uint64_t ra_va = regs[8];
    const uint64_t ra_rva = (g_brief_load_bias && ra_va >= g_brief_load_bias) ? (ra_va - g_brief_load_bias) : 0;

    bool is_cutscene = false;
    if (g_cutscene_proc_addr && is_readable_ptr(g_cutscene_proc_addr, 1)) {
        is_cutscene = (*reinterpret_cast<const uint8_t*>(g_cutscene_proc_addr) != 0);
    }
    if (!is_cutscene && g_cutscene_run_ptr_addr && is_readable_ptr(g_cutscene_run_ptr_addr, 8)) {
        const uint64_t rp = *reinterpret_cast<const uint64_t*>(g_cutscene_run_ptr_addr);
        if (rp && is_readable_ptr(rp, 1)) {
            is_cutscene = (*reinterpret_cast<const uint8_t*>(rp) != 0);
        }
    }

    std::string key;
    {
        std::lock_guard<std::mutex> lk(g_brief_cap_mutex);
        key = lookup_recent_key_locked(text_va);
    }
    const int lang = lcs::settings::get().game_language;
    // Objective/HUD color tags (~b~, ~g~, ~r~, ~y~, ~p~) identify navigation targets/objectives,
    // never spoken dialogue, even when emitted from mission GXT key prefixes.
    bool has_obj_color_tag = false;
    {
        const auto* u16 = reinterpret_cast<const uint16_t*>(text_va);
        for (int i = 0; i + 2 < 256 && u16[i] != 0; ++i) {
            if (u16[i] == '~' && u16[i + 2] == '~') {
                const uint16_t ch = (u16[i + 1] >= 'A' && u16[i + 1] <= 'Z')
                    ? static_cast<uint16_t>(u16[i + 1] + 32)
                    : u16[i + 1];
                if (ch == 'b' || ch == 'g' || ch == 'r' || ch == 'y' || ch == 'p') {
                    has_obj_color_tag = true;
                    break;
                }
            }
        }
    }
    // Key-only speaker check identifies genuine dialogue GXT keys without false-matching
    // character names inside mission objectives (e.g. "Drive to Salvatore's house").
    const bool is_dlg_key = !has_obj_color_tag && !key.empty() && !resolve_dialogue_speaker(key, "", "", lang).empty();

    int cat = -1;
    int32_t nums[6] = {-1, -1, -1, -1, -1, -1};
    const uint16_t* sub_u16 = nullptr;

    switch (kind) {
        case 0: // CHud::SetHelpMessage(u16*, bool, bool)
            cat = 1;
            break;
        case 1: { // CMessages::AddMessageJumpQ(u16*, u32, u16, u8 bAddToPreviousBrief)
            const bool is_dlg = !has_obj_color_tag &&
                ((ra_rva == 0x407c94ULL) || (ra_rva == 0x2eef54ULL) || is_cutscene || is_dlg_key);
            cat = is_dlg ? 0 : 2;
            break;
        }
        case 2: // CMessages::AddMessage
        case 3: // CMessages::AddMessageSoon
            cat = (!has_obj_color_tag && (is_cutscene || is_dlg_key)) ? 0 : 2;
            break;
        case 4: // CMessages::AddMessageWithNumber
        case 5: // CMessages::AddMessageJumpQWithNumber
        case 7: // CMessages::AddMessageSoonWithNumber
            for (int i = 0; i < 5; ++i) nums[i] = static_cast<int32_t>(regs[3 + i]);
            cat = (!has_obj_color_tag && (is_cutscene || is_dlg_key)) ? 0 : 2;
            break;
        case 6: // CMessages::AddMessageJumpQWithString
            if (regs[3] && is_readable_ptr(regs[3], 2)) {
                sub_u16 = reinterpret_cast<const uint16_t*>(regs[3]);
            }
            cat = (!has_obj_color_tag && (is_cutscene || is_dlg_key)) ? 0 : 2;
            break;
        case 8: // CMessages::AddToPreviousBriefArray (only direct 0x0655 no-subtitles path!)
            if (ra_rva != 0x408088ULL) return;
            cat = 0;
            break;
        default:
            return;
    }

    std::string text = format_gta_utf16(
        reinterpret_cast<const uint16_t*>(text_va), nums, 6, sub_u16, (cat == 0));
    if (text.empty()) return;

    std::lock_guard<std::mutex> lk(g_brief_cap_mutex);
    if (cat == 0) {
        const auto segs = split_dialogue_segments(key, "", text, lang);
        for (const auto& seg : segs) {
            if (!seg.second.empty()) {
                push_brief_entry_locked({seg.second, 0, seg.first, key});
            }
        }
    } else {
        std::replace(text.begin(), text.end(), '\n', ' ');
        push_brief_entry_locked({std::move(text), cat, {}, key});
    }
}

bool resolve_pool_pos(uint64_t pool_ptr_addr, uint64_t stride, int32_t handle, uint64_t ped_in_veh_offset,
                      float& out_x, float& out_y, float& out_z) {
    if (!pool_ptr_addr || !is_readable_ptr(pool_ptr_addr, 8)) return false;
    const uint64_t pool = *reinterpret_cast<const uint64_t*>(pool_ptr_addr);
    if (!pool || !is_readable_ptr(pool, 0x18)) return false;
    if (handle < 0x100) return false;
    const uint32_t idx = static_cast<uint32_t>(handle) >> 8;
    const uint8_t ref = static_cast<uint8_t>(handle & 0xff);
    const int32_t pool_size = *reinterpret_cast<const int32_t*>(pool + 0x10);
    if (pool_size <= 0 || idx >= static_cast<uint32_t>(pool_size)) return false;
    const uint64_t objects = *reinterpret_cast<const uint64_t*>(pool + 0x00);
    const uint64_t byte_map = *reinterpret_cast<const uint64_t*>(pool + 0x08);
    if (!objects || !byte_map || !is_readable_ptr(byte_map + idx, 1)) return false;
    if (*reinterpret_cast<const uint8_t*>(byte_map + idx) != ref) return false;
    uint64_t ent = objects + static_cast<uint64_t>(idx) * stride;
    if (!is_readable_ptr(ent, 0x50)) return false;
    if (ped_in_veh_offset != 0 && is_readable_ptr(ent + ped_in_veh_offset, 1)) {
        if (*reinterpret_cast<const uint8_t*>(ent + ped_in_veh_offset) != 0) {
            const uint64_t veh = *reinterpret_cast<const uint64_t*>(ent + kPedVehiclePtrOffset);
            if (veh && is_readable_ptr(veh + kEntityPosOffset, 12)) {
                ent = veh;
            }
        }
    }
    const float* p = reinterpret_cast<const float*>(ent + kEntityPosOffset);
    if (!std::isfinite(p[0]) || !std::isfinite(p[1])) return false;
    out_x = p[0];
    out_y = p[1];
    out_z = p[2];
    return true;
}
} // namespace

LCSTrainer& LCSTrainer::instance() {
    static LCSTrainer s_inst;
    return s_inst;
}

void LCSTrainer::attach(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
    m_cpu = &cpu;
    m_load_bias = image_game.get_load_bias();
    m_the_camera = image_game.find_symbol("TheCamera");
    m_game_state_addr = image_game.find_symbol("gGameState");
    m_femm_addr = image_game.find_symbol("FrontEndMenuManager");
    m_user_pause_addr = image_game.find_symbol("_ZN6CTimer11m_UserPauseE");
    m_player_idx_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotPlayerIdxRva);
    m_players_arr_addr = *reinterpret_cast<const uint64_t*>(m_load_bias + kGotPlayersArrRva);

    // Exact mangled symbol names verified against libGame.so dynsym
    m_clock_hours_addr    = image_game.find_symbol("_ZN6CClock18ms_nGameClockHoursE");
    m_clock_mins_addr     = image_game.find_symbol("_ZN6CClock20ms_nGameClockMinutesE");
    m_weather_old_addr    = image_game.find_symbol("_ZN8CWeather14OldWeatherTypeE");
    m_weather_new_addr    = image_game.find_symbol("_ZN8CWeather14NewWeatherTypeE");
    m_weather_forced_addr = image_game.find_symbol("_ZN8CWeather17ForcedWeatherTypeE");
    m_time_scale_addr     = image_game.find_symbol("_ZN6CTimer13ms_fTimeScaleE");
    m_hud_draw_addr       = image_game.find_symbol("_ZN4CHud19m_Wants_To_Draw_HudE");
    m_radar_trace_addr    = image_game.find_symbol("_ZN6CRadar13ms_RadarTraceE");
    m_curr_area_addr      = image_game.find_symbol("_ZN5CGame8currAreaE");
    m_in_stadium_addr       = image_game.find_symbol("_ZN11CTheScripts21bPlayerIsInTheStadiumE");
    m_on_mission_flag_addr  = image_game.find_symbol("_ZN11CTheScripts14OnAMissionFlagE");
    m_script_space_ptr_addr = image_game.find_symbol("_ZN11CTheScripts11ScriptSpaceE");
    m_pool_veh_ptr_addr     = image_game.find_symbol("_ZN6CPools15ms_pVehiclePoolE");
    m_pool_ped_ptr_addr   = image_game.find_symbol("_ZN6CPools11ms_pPedPoolE");
    m_pool_obj_ptr_addr   = image_game.find_symbol("_ZN6CPools14ms_pObjectPoolE");

    m_prefs_lang_addr         = image_game.find_symbol("_ZN12CMenuManager15m_PrefsLanguageE");
    m_prefs_lang_user_addr    = image_game.find_symbol("_ZN12CMenuManager24m_PrefsLanguageSetByUserE");
    m_prev_briefs_addr        = image_game.find_symbol("_ZN9CMessages14PreviousBriefsE");
    m_brief_msgs_addr         = image_game.find_symbol("_ZN9CMessages13BriefMessagesE");
    m_help_msgs_ptr_addr      = m_load_bias + 0x7fca78ULL;
    m_hud_msg_addr            = image_game.find_symbol("_ZN4CHud9m_MessageE");
    m_last_mission_addr       = image_game.find_symbol("_ZN6CStats21LastMissionPassedNameE");
    m_ctext_instance_ptr_addr = image_game.find_symbol("_ZN5CText10msInstanceE");

    m_pc_save_helper_addr = image_game.find_symbol("PcSaveHelper");
    m_slots_addr          = image_game.find_symbol("Slots");
    m_slot_filename_addr  = image_game.find_symbol("SlotFileName");
    m_slot_savedate_addr  = image_game.find_symbol("SlotSaveDate");
    m_want_to_load_addr   = image_game.find_symbol("b_FoundRecentSavedGameWantToLoad");
    m_is_quicksave_addr   = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7fd8a8ULL);
    m_allow_replay_addr   = *reinterpret_cast<const uint64_t*>(m_load_bias + 0x7fd658ULL);

    // CStats live addresses
    m_stat_progress_made   = image_game.find_symbol("_ZN6CStats12ProgressMadeE");
    m_stat_total_progress  = image_game.find_symbol("_ZN6CStats19TotalProgressInGameE");
    m_stat_missions_passed = image_game.find_symbol("_ZN6CStats14MissionsPassedE");
    m_stat_total_missions  = image_game.find_symbol("_ZN6CStats19TotalNumberMissionsE");
    m_stat_people_killed   = image_game.find_symbol("_ZN6CStats20PeopleKilledByPlayerE");
    m_stat_cars_exploded   = image_game.find_symbol("_ZN6CStats12CarsExplodedE");
    m_stat_stars_attained  = image_game.find_symbol("_ZN6CStats19WantedStarsAttainedE");
    m_stat_stars_evaded    = image_game.find_symbol("_ZN6CStats17WantedStarsEvadedE");
    m_stat_dist_foot       = image_game.find_symbol("_ZN6CStats23DistanceTravelledOnFootE");
    m_stat_dist_car        = image_game.find_symbol("_ZN6CStats22DistanceTravelledByCarE");
    m_stat_days_passed     = image_game.find_symbol("_ZN6CStats10DaysPassedE");
    m_stat_times_died      = image_game.find_symbol("_ZN6CStats9TimesDiedE");
    m_stat_times_arrested  = image_game.find_symbol("_ZN6CStats13TimesArrestedE");
    m_stat_rounds_fired    = image_game.find_symbol("_ZN6CStats19RoundsFiredByPlayerE");
    m_stat_bullets_hit     = image_game.find_symbol("_ZN6CStats14BulletsThatHitE");
    m_stat_jumps_found     = image_game.find_symbol("_ZN6CStats24NumberOfUniqueJumpsFoundE");
    m_stat_jumps_total     = image_game.find_symbol("_ZN6CStats24TotalNumberOfUniqueJumpsE");
    m_stat_cheated_count   = image_game.find_symbol("_ZN6CStats12CheatedCountE");

    m_fn_give_weapon        = image_game.find_symbol("_ZN4CPed10GiveWeaponE11eWeaponTypejb");
    m_fn_set_current_weapon = image_game.find_symbol("_ZN4CPed16SetCurrentWeaponE11eWeaponType");
    m_fn_get_weapon_info    = image_game.find_symbol("_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponType");
    m_fn_request_model      = image_game.find_symbol("_ZN10CStreaming12RequestModelEii");
    m_fn_load_models        = image_game.find_symbol("_ZN10CStreaming22LoadAllRequestedModelsEb");
    m_fn_set_model_deletable= image_game.find_symbol("_ZN10CStreaming19SetModelIsDeletableEi");
    m_fn_weapon_cheat3      = image_game.find_symbol("_Z12WeaponCheat3v");
    m_fn_set_wanted    = image_game.find_symbol("_ZN10CPlayerPed14SetWantedLevelEi");
    m_fn_vehicle_cheat = image_game.find_symbol("_Z12VehicleCheati");
    m_fn_tank_cheat    = image_game.find_symbol("_Z9TankCheatv");
    m_fn_get_model_info = image_game.find_symbol("_ZN10CModelInfo12GetModelInfoEPKcPi");
    install_unlimited_ammo_patch(image_game, cpu);
    m_fn_fix_car       = image_game.find_symbol("_ZN11CAutomobile3FixEv");
    m_fn_force_weather = image_game.find_symbol("_ZN8CWeather15ForceWeatherNowEs");
    m_fn_text_get           = image_game.find_symbol("_ZN5CText3GetEPKc");
    m_fn_init_changed_lang  = image_game.find_symbol("_ZN12CMenuManager33InitialiseChangedLanguageSettingsEv");
    m_fn_populate_slot_info = image_game.find_symbol("_ZN8C_PcSave16PopulateSlotInfoEv");
    m_fn_save_slot          = image_game.find_symbol("_ZN8C_PcSave8SaveSlotEi");
    m_fn_save_settings      = image_game.find_symbol("_ZN12CMenuManager12SaveSettingsEv");
    m_fn_load_from_gate     = image_game.find_symbol("_ZN12CMenuManager16LoadGameFromGateEv");
    m_fn_start_new_game     = image_game.find_symbol("_ZN12CMenuManager12StartNewGameEv");
    m_fn_check_slot_valid   = image_game.find_symbol("_Z18CheckSlotDataValidi");
    m_fn_play_fe_sound      = image_game.find_symbol("_ZN8cDMAudio17PlayFrontEndSoundEtj");

#if defined(_WIN32)
    if (!m_scratch_key_va) {
        m_scratch_key_va = reinterpret_cast<uint64_t>(
            VirtualAlloc(nullptr, 256, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
#endif

    m_attached = (m_player_idx_addr != 0 && m_players_arr_addr != 0);

    m_log_history.clear();
    m_was_in_mission = false;
    m_pre_mission_snapshot.clear();
    m_mission_resumo.clear();
    m_lang_checked = false;
    m_has_exterior_pos = false;
    m_mainmenu_slots_queried = false;
    m_data_refresh_accum = 1.0f;

    std::cout << "[LCSTrainer] Attached to libGame.so ("
              << "clock=" << (m_clock_hours_addr ? "ok" : "n/a")
              << ", weather=" << (m_weather_new_addr ? "ok" : "n/a")
              << ", timescale=" << (m_time_scale_addr ? "ok" : "n/a")
              << ", wanted=" << (m_fn_set_wanted ? "ok" : "n/a")
              << ", weapons=" << (m_fn_give_weapon ? "ok" : "n/a")
              << ", vehicles=" << (m_fn_vehicle_cheat ? "ok" : "n/a")
              << ", pause=" << (m_user_pause_addr ? "ok" : "n/a")
              << ")\n";
}

bool LCSTrainer::is_in_gameplay() const {
    if (!m_attached || !m_game_state_addr) return false;
    const uint32_t gstate = *reinterpret_cast<const uint32_t*>(m_game_state_addr);
    if (gstate != 9) return false;
    if (m_femm_addr && *reinterpret_cast<const uint8_t*>(m_femm_addr + 0x14) != 0) return false;
    return get_player_ped() != 0;
}

bool LCSTrainer::is_in_main_menu() const {
    if (!m_attached || m_video_playing || !m_game_state_addr || !m_femm_addr) return false;
    const uint32_t gstate = *reinterpret_cast<const uint32_t*>(m_game_state_addr);
    if (gstate != 7) return false;
    const uint8_t menu_active  = *reinterpret_cast<const uint8_t*>(m_femm_addr + 0x14);
    const uint8_t want_restart = *reinterpret_cast<const uint8_t*>(m_femm_addr + 0x15);
    const uint8_t want_load    = *reinterpret_cast<const uint8_t*>(m_femm_addr + 0x19);
    const uint8_t wait_java    = *reinterpret_cast<const uint8_t*>(m_femm_addr + 0x2b);
    const int32_t page         = *reinterpret_cast<const int32_t*>(m_femm_addr + 0x110);
    return menu_active != 0 && want_restart == 0 && want_load == 0 && wait_java == 0 &&
           (page == 51 || page == 69);
}

int LCSTrainer::get_recent_save_slot() const {
    const auto slots = read_save_slots();
    if (m_femm_addr && m_want_to_load_addr && *reinterpret_cast<const uint8_t*>(m_want_to_load_addr) != 0) {
        const int32_t cur = *reinterpret_cast<const int32_t*>(m_femm_addr + 0x118);
        if (cur >= 0 && cur < static_cast<int32_t>(slots.size()) && slots[cur].state == 0) {
            return cur;
        }
    }
    if (slots.size() > 8 && slots[8].state == 0) return 8;
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].state == 0) return static_cast<int>(i);
    }
    return -1;
}

void LCSTrainer::set_user_pause(bool paused) {
    m_menu_paused = paused;
    if (m_user_pause_addr) {
        *reinterpret_cast<uint8_t*>(m_user_pause_addr) = paused ? 1 : 0;
    }
}

void LCSTrainer::request_quit() {
    m_quit_requested = true;
}

bool LCSTrainer::consume_quit_request() {
    const bool q = m_quit_requested;
    m_quit_requested = false;
    return q;
}

GameSnapshot LCSTrainer::read_snapshot() const {
    GameSnapshot snap;
    snap.attached = m_attached;
    if (!m_attached) return snap;

    snap.in_gameplay = is_in_gameplay();
    snap.in_mission = false;
    if (m_on_mission_flag_addr && m_script_space_ptr_addr) {
        const uint32_t flag = *reinterpret_cast<const uint32_t*>(m_on_mission_flag_addr);
        const uint64_t space = *reinterpret_cast<const uint64_t*>(m_script_space_ptr_addr);
        if (flag != 0 && space && is_readable_ptr(space + flag, 4)) {
            snap.in_mission = (*reinterpret_cast<const int32_t*>(space + flag) == 1);
        }
    }
    snap.curr_area = m_curr_area_addr ? *reinterpret_cast<const int32_t*>(m_curr_area_addr) : 0;
    const bool in_stadium = m_in_stadium_addr && (*reinterpret_cast<const uint8_t*>(m_in_stadium_addr) != 0);

    const uint64_t pinfo = get_player_info();
    const uint64_t ped = get_player_ped();
    if (pinfo) {
        snap.money = *reinterpret_cast<const int32_t*>(pinfo + kPlayerInfoMoneyOffset);
    }
    if (ped) {
        snap.health = *reinterpret_cast<const float*>(ped + kPedHealthOffset);
        snap.armour = *reinterpret_cast<const float*>(ped + kPedArmourOffset);
        const float* pos = reinterpret_cast<const float*>(ped + kEntityPosOffset);
        snap.pos[0] = pos[0];
        snap.pos[1] = pos[1];
        snap.pos[2] = pos[2];
        const bool on_city_map = !in_stadium &&
                                 std::fabs(pos[0]) <= 2100.0f &&
                                 pos[1] >= -2300.0f && pos[1] <= 1500.0f &&
                                 pos[2] < 200.0f;
        if (snap.curr_area == 0 && on_city_map) {
            m_last_exterior_pos[0] = pos[0];
            m_last_exterior_pos[1] = pos[1];
            m_last_exterior_pos[2] = pos[2];
            m_has_exterior_pos = true;
        } else if (snap.curr_area != 0 && !on_city_map && m_has_exterior_pos) {
            snap.pos[0] = m_last_exterior_pos[0];
            snap.pos[1] = m_last_exterior_pos[1];
            snap.pos[2] = m_last_exterior_pos[2];
        }
    }
    if (m_clock_hours_addr) snap.clock_hour = *reinterpret_cast<const uint8_t*>(m_clock_hours_addr);
    if (m_clock_mins_addr) snap.clock_minute = *reinterpret_cast<const uint8_t*>(m_clock_mins_addr);

    if (m_stat_progress_made)   snap.progress_made   = *reinterpret_cast<const float*>(m_stat_progress_made);
    if (m_stat_total_progress)  snap.total_progress  = *reinterpret_cast<const float*>(m_stat_total_progress);
    if (m_stat_missions_passed) snap.missions_passed = *reinterpret_cast<const int32_t*>(m_stat_missions_passed);
    if (m_stat_total_missions)  snap.total_missions  = *reinterpret_cast<const int32_t*>(m_stat_total_missions);
    if (m_stat_people_killed)   snap.people_killed   = *reinterpret_cast<const int32_t*>(m_stat_people_killed);
    if (m_stat_cars_exploded)   snap.cars_exploded   = *reinterpret_cast<const int32_t*>(m_stat_cars_exploded);
    if (m_stat_stars_attained)  snap.stars_attained  = *reinterpret_cast<const int32_t*>(m_stat_stars_attained);
    if (m_stat_stars_evaded)    snap.stars_evaded    = *reinterpret_cast<const int32_t*>(m_stat_stars_evaded);
    if (m_stat_dist_foot)       snap.dist_foot       = *reinterpret_cast<const float*>(m_stat_dist_foot);
    if (m_stat_dist_car)        snap.dist_car        = *reinterpret_cast<const float*>(m_stat_dist_car);
    if (m_stat_days_passed)     snap.days_passed     = *reinterpret_cast<const int32_t*>(m_stat_days_passed);
    if (m_stat_times_died)      snap.times_died      = *reinterpret_cast<const int32_t*>(m_stat_times_died);
    if (m_stat_times_arrested)  snap.times_arrested  = *reinterpret_cast<const int32_t*>(m_stat_times_arrested);
    if (m_stat_rounds_fired)    snap.rounds_fired    = *reinterpret_cast<const int32_t*>(m_stat_rounds_fired);
    if (m_stat_bullets_hit)     snap.bullets_hit     = *reinterpret_cast<const int32_t*>(m_stat_bullets_hit);
    if (m_stat_jumps_found)     snap.jumps_found     = *reinterpret_cast<const int32_t*>(m_stat_jumps_found);
    if (m_stat_jumps_total)     snap.jumps_total     = *reinterpret_cast<const int32_t*>(m_stat_jumps_total);
    if (m_stat_cheated_count)   snap.cheated_count   = *reinterpret_cast<const int32_t*>(m_stat_cheated_count);
    return snap;
}

std::vector<LiveRadarBlip> LCSTrainer::read_radar_blips() const {
    std::vector<LiveRadarBlip> out;
    if (!m_attached || !m_radar_trace_addr) return out;

    // CRadar::ms_RadarTrace: 75 entries x 0x3c (60) bytes (verified via CRadar::DrawBlips & DrawEntityBlip)
    // +0x00 colour, +0x04 blipType (1=Car,2=Ped,3=Obj,4=Coord,5=Contact), +0x08 entityHandle,
    // +0x0c vecPos, +0x18 vecPosResolved, +0x2b bInUse, +0x36 display, +0x38 sprite, +0x3a hidden
    constexpr size_t kMaxBlips = 75;
    constexpr size_t kStride = 0x3c;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(m_radar_trace_addr);
    for (size_t i = 0; i < kMaxBlips; ++i) {
        const uint8_t* e = base + i * kStride;
        const uint8_t in_use = e[0x2b];
        const uint8_t hidden = e[0x3a];
        const uint32_t btype = *reinterpret_cast<const uint32_t*>(e + 0x04);
        const uint16_t disp = *reinterpret_cast<const uint16_t*>(e + 0x36);
        if (!in_use || hidden || btype == 0 || disp < 2) continue;

        float bx = 0.0f, by = 0.0f, bz = 0.0f;
        bool has_pos = false;
        if (btype == 1 || btype == 2 || btype == 3) {
            const int32_t handle = *reinterpret_cast<const int32_t*>(e + 0x08);
            if (btype == 1) {
                has_pos = resolve_pool_pos(m_pool_veh_ptr_addr, kVehPoolStride, handle, 0, bx, by, bz);
            } else if (btype == 2) {
                has_pos = resolve_pool_pos(m_pool_ped_ptr_addr, kPedPoolStride, handle, kPedInVehicleFlag, bx, by, bz);
            } else {
                has_pos = resolve_pool_pos(m_pool_obj_ptr_addr, kObjPoolStride, handle, 0, bx, by, bz);
            }
        }
        if (!has_pos) {
            bx = *reinterpret_cast<const float*>(e + 0x18);
            by = *reinterpret_cast<const float*>(e + 0x1c);
            bz = *reinterpret_cast<const float*>(e + 0x20);
            if (bx == 0.0f && by == 0.0f) {
                bx = *reinterpret_cast<const float*>(e + 0x0c);
                by = *reinterpret_cast<const float*>(e + 0x10);
                bz = *reinterpret_cast<const float*>(e + 0x14);
            }
        }
        if (!std::isfinite(bx) || !std::isfinite(by) || (bx == 0.0f && by == 0.0f) ||
            std::fabs(bx) > 2500.0f || std::fabs(by) > 2500.0f) continue;

        LiveRadarBlip b;
        b.x = bx;
        b.y = by;
        b.z = bz;
        b.colour = *reinterpret_cast<const uint32_t*>(e + 0x00);
        b.blip_type = static_cast<uint8_t>(btype);
        b.display = static_cast<uint8_t>(disp);
        b.sprite = *reinterpret_cast<const uint16_t*>(e + 0x38);
        out.push_back(b);
    }
    return out;
}

void LCSTrainer::enqueue_cpu_action(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_action_mutex);
    m_pending_actions.push_back(std::move(fn));
}

uint64_t LCSTrainer::call_guest(uint64_t fn, const std::vector<uint64_t>& args) const {
    if (!fn) return 0;
    if (auto* cur = lcs::cpu::DynarmicCore::current()) {
        return cur->call_nested(fn, args);
    }
    return m_cpu ? m_cpu->call(fn, args) : 0;
}

void LCSTrainer::drain_cpu_actions() {
    const auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    m_last_tick_drain_ms.store(now_ms, std::memory_order_release);

    std::vector<std::function<void()>> actions;
    {
        std::lock_guard<std::mutex> lock(m_action_mutex);
        if (m_pending_actions.empty()) return;
        actions.swap(m_pending_actions);
    }
    for (auto& fn : actions) {
        if (fn) fn();
    }
}

uint64_t LCSTrainer::get_player_info() const {
    if (!m_attached) return 0;
    const uint8_t idx = *reinterpret_cast<const uint8_t*>(m_player_idx_addr);
    return m_players_arr_addr + static_cast<uint64_t>(idx) * kPlayerInfoStride;
}

uint64_t LCSTrainer::get_player_ped() const {
    const uint64_t pinfo = get_player_info();
    if (!pinfo) return 0;
    return *reinterpret_cast<const uint64_t*>(pinfo);
}

uint64_t LCSTrainer::get_player_vehicle() const {
    const uint64_t ped = get_player_ped();
    if (!ped) return 0;
    if (*reinterpret_cast<const uint8_t*>(ped + kPedInVehicleFlag) == 0) return 0;
    return *reinterpret_cast<const uint64_t*>(ped + kPedVehiclePtrOffset);
}

void LCSTrainer::tick(float dt_sec) {
    if (!m_attached) return;

    // Keep game simulation paused while the Global Menu is open
    if (m_menu_paused && m_user_pause_addr) {
        *reinterpret_cast<uint8_t*>(m_user_pause_addr) = 1;
    }

    // Query save slots as soon as GameState enters frontend (7) so Frame 1 of Main Menu
    // already shows "Continuar" & "Carregar Jogo" immediately upon HideLoadingScreen.
    const uint32_t gstate_now = m_game_state_addr ? *reinterpret_cast<const uint32_t*>(m_game_state_addr) : 0;
    if (is_in_main_menu() || gstate_now == 7) {
        if (!m_mainmenu_slots_queried) {
            m_mainmenu_slots_queried = true;
            refresh_save_slots_sync();
        }
    } else if (gstate_now == 9) {
        m_mainmenu_slots_queried = false;
    }

    // 0. Sync configured game language and guard against out-of-range m_PrefsLanguage in gta_lcs.set
    if (!m_lang_checked && m_prefs_lang_addr && m_femm_addr && m_game_state_addr) {
        const uint32_t gs = *reinterpret_cast<const uint32_t*>(m_game_state_addr);
        if (gs >= 7) {
            m_lang_checked = true;
            const int32_t cfg_lang = std::clamp(lcs::settings::get().game_language, 0, 7);
            const int32_t want_lang = (cfg_lang == 7) ? 0 : cfg_lang;
            const int32_t cur_lang = *reinterpret_cast<const int32_t*>(m_prefs_lang_addr);
            if (cur_lang != want_lang || cur_lang < 0 || cur_lang > 6) {
                *reinterpret_cast<int32_t*>(m_prefs_lang_addr) = want_lang;
                if (m_prefs_lang_user_addr) {
                    *reinterpret_cast<uint8_t*>(m_prefs_lang_user_addr) = 1;
                }
                if (m_cpu && m_fn_init_changed_lang) {
                    *reinterpret_cast<uint8_t*>(m_femm_addr + 0x2d) = 1;
                    call_guest(m_fn_init_changed_lang, {m_femm_addr});
                }
            }
        }
    }

    // 1. Drain queued native ARM64 calls on MainThread ONLY while gGameTickThread is idle in Frontend Main Menu.
    // Once gameplay ticks CStreaming::Update(), drain_cpu_actions() runs strictly on gGameTickThread.
    const auto now_ms = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t last_gt_ms = m_last_tick_drain_ms.load(std::memory_order_acquire);
    const bool gt_idle = (last_gt_ms == 0ULL) || (now_ms > last_gt_ms + 250ULL);
    if (is_in_main_menu() && gstate_now != 9 && gt_idle) {
        std::vector<std::function<void()>> actions;
        {
            std::lock_guard<std::mutex> lock(m_action_mutex);
            actions.swap(m_pending_actions);
        }
        for (auto& fn : actions) {
            if (fn) fn();
        }
    }

    // 2. Apply continuous cheats
    const uint64_t pinfo = get_player_info();
    const uint64_t ped = get_player_ped();
    if (ped) {
        if (god_mode) {
            const float maxHp = pinfo ? std::max(100.0f, static_cast<float>(*reinterpret_cast<const uint8_t*>(pinfo + kPlayerInfoMaxHpOffset))) : 100.0f;
            const float maxAr = pinfo ? std::max(100.0f, static_cast<float>(*reinterpret_cast<const uint8_t*>(pinfo + kPlayerInfoMaxArOffset))) : 100.0f;
            *reinterpret_cast<float*>(ped + kPedHealthOffset) = maxHp;
            *reinterpret_cast<float*>(ped + kPedArmourOffset) = maxAr;
        }
        if (infinite_sprint) {
            *reinterpret_cast<float*>(ped + kPedStaminaOffset) = 100.0f;
        }
        if (never_wanted && m_cpu && m_fn_set_wanted) {
            m_never_wanted_accum += dt_sec;
            if (m_never_wanted_accum >= 0.25f) {
                m_never_wanted_accum = 0.0f;
                enqueue_cpu_action([this]() {
                    const uint64_t p = get_player_ped();
                    if (p) call_guest(m_fn_set_wanted, {p, 0});
                });
            }
        }
        // Unlimited ammo: CWeapon::Fire skips both ammo stores for this ped (neither the clip
        // nor the total ever drops, so no reload either). Other peds are unaffected.
        if (m_ammo_player_literal) {
            *m_ammo_player_literal = unlimited_ammo ? ped : 0;
        }
        const bool ammo_fallback = unlimited_ammo && !m_ammo_player_literal;
        if (ammo_fallback || no_reload) {
            for (int slot = 0; slot < 10; ++slot) {
                const uint64_t wptr = ped + kPedWeaponsOffset + static_cast<uint64_t>(slot) * kPedWeaponStride;
                const uint32_t wtype = *reinterpret_cast<const uint32_t*>(wptr + 0x00);
                if (wtype >= 12 && wtype <= 35) {
                    if (no_reload) *reinterpret_cast<uint32_t*>(wptr + 0x08) = 99;
                    if (ammo_fallback) *reinterpret_cast<uint32_t*>(wptr + 0x0c) = 9999;
                }
            }
        }
    }

    const uint64_t veh = get_player_vehicle();
    if (veh && vehicle_invincible) {
        *reinterpret_cast<float*>(veh + kVehHealthOffset) = 1000.0f;
    }

    if (freeze_time && m_clock_hours_addr) {
        *reinterpret_cast<uint8_t*>(m_clock_hours_addr) = static_cast<uint8_t>(std::clamp(frozen_hour, 0, 23));
        if (m_clock_mins_addr) *reinterpret_cast<uint8_t*>(m_clock_mins_addr) = 0;
    }

    static bool s_prev_hide_hud = false;
    if (m_hud_draw_addr && (hide_hud || s_prev_hide_hud)) {
        *reinterpret_cast<uint8_t*>(m_hud_draw_addr) = hide_hud ? 0 : 1;
        s_prev_hide_hud = hide_hud;
    }

    bool cur_in_mission = false;
    if (m_on_mission_flag_addr && m_script_space_ptr_addr) {
        const uint32_t flag = *reinterpret_cast<const uint32_t*>(m_on_mission_flag_addr);
        const uint64_t space = *reinterpret_cast<const uint64_t*>(m_script_space_ptr_addr);
        if (flag != 0 && space && is_readable_ptr(space + flag, 4)) {
            cur_in_mission = (*reinterpret_cast<const int32_t*>(space + flag) == 1);
        }
    }
    m_data_refresh_accum += dt_sec;
    const float refresh_interval = cur_in_mission ? 0.08f : 0.25f;
    if (cur_in_mission != m_was_in_mission || m_data_refresh_accum >= refresh_interval) {
        m_data_refresh_accum = 0.0f;
        refresh_cached_game_data();
    }

    prune_notices(now_seconds());
}

void LCSTrainer::heal_player() {
    const uint64_t pinfo = get_player_info();
    const uint64_t ped = get_player_ped();
    if (!ped) {
        push_notice(ui::loc::tx("Jogador ainda não carregado no mundo"), 1);
        return;
    }
    const float maxHp = pinfo ? std::max(100.0f, static_cast<float>(*reinterpret_cast<const uint8_t*>(pinfo + kPlayerInfoMaxHpOffset))) : 100.0f;
    const float maxAr = pinfo ? std::max(100.0f, static_cast<float>(*reinterpret_cast<const uint8_t*>(pinfo + kPlayerInfoMaxArOffset))) : 100.0f;
    *reinterpret_cast<float*>(ped + kPedHealthOffset) = maxHp;
    *reinterpret_cast<float*>(ped + kPedArmourOffset) = maxAr;
    push_notice(ui::loc::tx("Vida e Colete restaurados (100%)"), 2);
}

void LCSTrainer::set_wanted_level(int stars) {
    stars = std::clamp(stars, 0, 6);
    if (!m_attached || !m_cpu || !m_fn_set_wanted) {
        push_notice(ui::loc::tx("Nível de procurado indisponível"), 1);
        return;
    }
    if (stars > 0) never_wanted = false;
    enqueue_cpu_action([this, stars]() {
        const uint64_t ped = get_player_ped();
        if (!ped) return;
        call_guest(m_fn_set_wanted, {ped, static_cast<uint64_t>(stars)});
    });
    push_notice(std::string(ui::loc::tx("Nível de procurado")) + ": " + std::to_string(stars) + " " + ui::loc::tx("estrela(s)"), 2);
}

void LCSTrainer::set_game_language(int lang_id) {
    lang_id = std::clamp(lang_id, 0, 7);
    const int32_t guest_lang = (lang_id == 7) ? 0 : lang_id;
    lcs::settings::get().game_language = lang_id;
    m_lang_checked = true;
    if (!m_prefs_lang_addr || !m_femm_addr || !m_cpu || !m_fn_init_changed_lang) {
        return;
    }
    static constexpr const char* kLangNames[] = {
        "English", "Français", "Deutsch", "Italiano", "Español", "Русский", "日本語", "Português (Brasil)"
    };
    enqueue_cpu_action([this, guest_lang]() {
        *reinterpret_cast<int32_t*>(m_prefs_lang_addr) = guest_lang;
        if (m_prefs_lang_user_addr) {
            *reinterpret_cast<uint8_t*>(m_prefs_lang_user_addr) = 1;
        }
        *reinterpret_cast<uint8_t*>(m_femm_addr + 0x2d) = 1;
        call_guest(m_fn_init_changed_lang, {m_femm_addr});
        if (m_fn_save_settings) {
            call_guest(m_fn_save_settings, {m_femm_addr});
        }
    });
    push_notice(std::string(ui::loc::get(ui::loc::Text::CatLanguage, lang_id)) + ": " + kLangNames[lang_id], 2);
}

void LCSTrainer::install_brief_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
#if defined(_WIN32)
    void* page = VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!page) return;
    auto* code = static_cast<uint8_t*>(page);
    const uint64_t base_va = reinterpret_cast<uint64_t>(page);
    const uint64_t load_bias = image_game.get_load_bias();
    g_brief_load_bias = load_bias;
    g_cutscene_proc_addr = load_bias + 0x847d36ULL;
    g_cutscene_run_ptr_addr = load_bias + 0x7f9010ULL;

    auto& hle = lcs::cpu::HLEManager::instance();
    const uint64_t hle_text_get = hle.register_function("__pc_text_get_obs", [](const uint64_t* args) -> uint64_t {
        if (args[0] && args[1]) {
            on_text_get_post(args[0], reinterpret_cast<const char*>(args[1]));
        }
        return 0;
    });
    const uint64_t hle_msg_obs = hle.register_function("__pc_brief_msg_obs", [](const uint64_t* args) -> uint64_t {
        on_brief_msg_call(static_cast<int>(args[0]), reinterpret_cast<const uint64_t*>(args[1]));
        return 0;
    });

    auto patch_got = [&](uint64_t got_rva, uint64_t target, const char* sym) -> uint64_t {
        const uint64_t slot_va = load_bias + got_rva;
        uint64_t orig = *reinterpret_cast<const uint64_t*>(slot_va);
        if (!orig && sym) orig = image_game.find_symbol(sym);
        DWORD old_prot = 0;
        VirtualProtect(reinterpret_cast<void*>(slot_va), sizeof(uint64_t), PAGE_EXECUTE_READWRITE, &old_prot);
        std::memcpy(reinterpret_cast<void*>(slot_va), &target, sizeof(uint64_t));
        VirtualProtect(reinterpret_cast<void*>(slot_va), sizeof(uint64_t), old_prot, &old_prot);
        cpu.invalidate_cache(slot_va, sizeof(uint64_t));
        return orig;
    };

    // Stub 0 (+0x000): CText::Get post-call observer (GOT 0x800bd0)
    const uint64_t orig_text_get = patch_got(0x800bd0ULL, base_va + 0x000, "_ZN5CText3GetEPKc");
    const uint32_t text_get_insns[12] = {
        0xa9be7bfdu, 0xf9000be1u, 0x58000190u, 0xd63f0200u,
        0xf9000fe0u, 0xf9400be1u, 0x580000d0u, 0xd63f0200u,
        0xf9400fe0u, 0xa8c27bfdu, 0xd65f03c0u, 0xd503201fu,
    };
    std::memcpy(code + 0x000, text_get_insns, sizeof(text_get_insns));
    std::memcpy(code + 0x030, &hle_text_get, sizeof(uint64_t));
    std::memcpy(code + 0x038, &orig_text_get, sizeof(uint64_t));

    // Stubs 1..9 (+0x100 + idx*0x80): CHud::SetHelpMessage + CMessages::Add* observers
    struct MsgHookDef { uint64_t got; int kind; const char* sym; };
    static constexpr MsgHookDef kHooks[] = {
        {0x800b90ULL, 0, "_ZN4CHud14SetHelpMessageEPtbb"},
        {0x800bd8ULL, 1, "_ZN9CMessages15AddMessageJumpQEPtjthh"},
        {0x805098ULL, 2, "_ZN9CMessages10AddMessageEPtjt"},
        {0x805440ULL, 3, "_ZN9CMessages14AddMessageSoonEPtjt"},
        {0x804e00ULL, 4, "_ZN9CMessages20AddMessageWithNumberEPtjtiiiiii"},
        {0x805548ULL, 5, "_ZN9CMessages25AddMessageJumpQWithNumberEPtjtiiiiiih"},
        {0x805708ULL, 6, "_ZN9CMessages25AddMessageJumpQWithStringEPtjtS0_"},
        {0x805520ULL, 7, "_ZN9CMessages24AddMessageSoonWithNumberEPtjtiiiiii"},
        {0x804e78ULL, 8, "_ZN9CMessages23AddToPreviousBriefArrayEPtiiiiiiS0_"},
    };
    for (size_t i = 0; i < std::size(kHooks); ++i) {
        const size_t off = 0x100u + i * 0x80u;
        const uint64_t orig = patch_got(kHooks[i].got, base_va + off, kHooks[i].sym);
        uint32_t insns[20] = {
            0xd10143ffu, 0xa90007e0u, 0xa9010fe2u, 0xa90217e4u,
            0xa9031fe6u, 0xf90023feu,
            0x52800000u | (static_cast<uint32_t>(kHooks[i].kind & 0xffffu) << 5),
            0x910003e1u, 0x58000190u, 0xd63f0200u, 0xf94023feu,
            0xa9431fe6u, 0xa94217e4u, 0xa9410fe2u, 0xa94007e0u,
            0x910143ffu, 0x580000d0u, 0xd61f0200u, 0xd503201fu, 0xd503201fu,
        };
        std::memcpy(code + off, insns, sizeof(insns));
        std::memcpy(code + off + 0x50, &hle_msg_obs, sizeof(uint64_t));
        std::memcpy(code + off + 0x58, &orig, sizeof(uint64_t));
    }
    cpu.invalidate_cache(base_va, 0x1000);
    std::cout << "  [Resumo] Hooked CText::Get + CMessages/HelpMessage observers for accurate Objectives/Help/Speaker logs.\n";
#else
    (void)image_game; (void)cpu;
#endif
}

void LCSTrainer::add_money(int amount) {
    const uint64_t pinfo = get_player_info();
    if (!pinfo) {
        push_notice(ui::loc::tx("Jogador ainda não carregado no mundo"), 1);
        return;
    }
    int32_t* money = reinterpret_cast<int32_t*>(pinfo + kPlayerInfoMoneyOffset);
    *money += amount;
    push_notice(std::string(ui::loc::tx("Adicionado $")) + std::to_string(amount), 2);
}

void LCSTrainer::give_weapon(int weapon_id, int ammo) {
    if (!m_attached || !m_cpu || !m_fn_give_weapon) {
        push_notice(ui::loc::tx("Função de armas indisponível"), 1);
        return;
    }
    enqueue_cpu_action([this, weapon_id, ammo]() {
        const uint64_t ped = get_player_ped();
        if (!ped) return;
        int32_t model1 = -1;
        int32_t model2 = -1;
        if (m_fn_get_weapon_info && m_fn_request_model && m_fn_load_models) {
            const uint64_t info = call_guest(m_fn_get_weapon_info, {static_cast<uint64_t>(weapon_id)});
            if (info) {
                model1 = *reinterpret_cast<const int32_t*>(info + 0x58ULL);
                model2 = *reinterpret_cast<const int32_t*>(info + 0x5cULL);
            }
            if (weapon_id == 33 && model2 <= 0) {
                model2 = 294; // minigun2 barrel required by CPed::GiveWeapon (0x419e9c..0x419edc)
            }
            if (model1 > 0) call_guest(m_fn_request_model, {static_cast<uint64_t>(model1), 1});
            if (model2 > 0) call_guest(m_fn_request_model, {static_cast<uint64_t>(model2), 1});
            if (model1 > 0 || model2 > 0) {
                call_guest(m_fn_load_models, {0});
            }
        }
        call_guest(m_fn_give_weapon, {ped, static_cast<uint64_t>(weapon_id), static_cast<uint64_t>(ammo), 1});
        if (m_fn_set_current_weapon) {
            call_guest(m_fn_set_current_weapon, {ped, static_cast<uint64_t>(weapon_id)});
        }
        if (m_fn_set_model_deletable) {
            if (model1 > 0) call_guest(m_fn_set_model_deletable, {static_cast<uint64_t>(model1)});
            if (model2 > 0) call_guest(m_fn_set_model_deletable, {static_cast<uint64_t>(model2)});
        }
    });
    push_notice(std::string(ui::loc::tx("Arma equipada")) + " (ID " + std::to_string(weapon_id) + ")", 2);
}

void LCSTrainer::give_full_loadout() {
    if (!m_attached || !m_cpu) {
        push_notice(ui::loc::tx("Jogador ainda não carregado no mundo"), 1);
        return;
    }
    if (m_fn_weapon_cheat3) {
        enqueue_cpu_action([this]() {
            if (!get_player_ped()) return;
            call_guest(m_fn_weapon_cheat3, {});
        });
        push_notice(ui::loc::tx("Arsenal completo concedido!"), 2);
    } else if (m_fn_give_weapon) {
        for (int wid : {17, 19, 25, 26, 28, 30}) {
            give_weapon(wid, (wid == 30) ? 50 : 500);
        }
        push_notice(ui::loc::tx("Arsenal completo concedido!"), 2);
    }
}

void LCSTrainer::spawn_vehicle(const char* model_name, int fallback_id) {
    if (!m_attached || !m_cpu || !model_name) {
        push_notice(ui::loc::tx("Spawn de veículo indisponível"), 1);
        return;
    }
    if (std::strcmp(model_name, "rhino") == 0 && m_fn_tank_cheat) {
        enqueue_cpu_action([this]() {
            if (!get_player_ped()) return;
            call_guest(m_fn_tank_cheat, {});
        });
        push_notice(ui::loc::tx("Tanque Rhino spawnado!"), 2);
        return;
    }
    if (!m_fn_vehicle_cheat) return;
    // The menus pass string literals, so the pointer stays valid until the action runs.
    enqueue_cpu_action([this, model_name, fallback_id]() {
        if (!get_player_ped()) return;
        // Guest and host share one address space: the host stack slot is a valid guest pointer.
        int32_t model_id = -1;
        uint64_t info = 0;
        if (m_fn_get_model_info) {
            info = call_guest(m_fn_get_model_info, {reinterpret_cast<uint64_t>(model_name),
                                                    reinterpret_cast<uint64_t>(&model_id)});
        }
        if (!info || model_id <= 0) {
            std::cout << "[LCSTrainer] Model '" << model_name << "' not found, using id " << fallback_id << "\n";
            model_id = fallback_id;
        }
        call_guest(m_fn_vehicle_cheat, {static_cast<uint64_t>(model_id)});
    });
    push_notice(std::string(ui::loc::tx("Veículo spawnado")), 2);
}

void LCSTrainer::install_unlimited_ammo_patch(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu) {
#if defined(_WIN32)
    if (m_ammo_player_literal) return;
    const uint64_t fire = image_game.find_symbol("_ZN7CWeapon4FireEP7CEntityP7CVector");
    if (!fire) return;
    // In CWeapon::Fire (x19 = this CWeapon, x20 = shooter entity):
    //   +0x5d8: str w8, [x19, #8]    ; m_nAmmoInClip -= 1
    //   +0x628: str w8, [x19, #0xc]  ; m_nAmmoTotal -= 1
    constexpr uint64_t kClipStoreOff = 0x5d8, kTotalStoreOff = 0x628;
    constexpr uint32_t kStrClip = 0xB9000A68u, kStrTotal = 0xB9000E68u;
    auto* clip_site = reinterpret_cast<uint32_t*>(fire + kClipStoreOff);
    auto* total_site = reinterpret_cast<uint32_t*>(fire + kTotalStoreOff);
    if (*clip_site != kStrClip || *total_site != kStrTotal) {
        std::cerr << "[LCSTrainer] CWeapon::Fire layout differs; unlimited ammo uses the per-frame fallback.\n";
        return;
    }

    // The stubs must sit within B's +-128 MB of libGame's code: try pages after the image.
    uint8_t* page = nullptr;
    for (uint64_t probe = (fire + 0x1000000ULL) & ~0xFFFFULL; probe < fire + 0x7000000ULL && !page; probe += 0x10000ULL) {
        page = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(probe), 0x1000,
                                                  MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    }
    if (!page) {
        std::cerr << "[LCSTrainer] No stub page near libGame; unlimited ammo uses the per-frame fallback.\n";
        return;
    }

    auto b_to = [](uint64_t from, uint64_t to) -> uint32_t {
        return 0x14000000u | (static_cast<uint32_t>((static_cast<int64_t>(to) - static_cast<int64_t>(from)) >> 2) & 0x03FFFFFFu);
    };
    auto ldr_lit = [](uint64_t from, uint64_t lit, uint32_t rt) -> uint32_t {
        return 0x58000000u | ((static_cast<uint32_t>((static_cast<int64_t>(lit) - static_cast<int64_t>(from)) >> 2) & 0x7FFFFu) << 5) | rt;
    };
    const uint64_t base = reinterpret_cast<uint64_t>(page);
    const uint64_t lit = base;  // uint64 player ped (0 = off)
    auto emit_stub = [&](uint64_t at, uint32_t store, uint64_t resume) {
        uint32_t* c = reinterpret_cast<uint32_t*>(at);
        c[0] = ldr_lit(at, lit, 9);      // ldr x9, player
        c[1] = 0xEB09029Fu;              // cmp x20, x9
        c[2] = 0x54000040u;              // b.eq +8 (skip the store)
        c[3] = store;                    // original str
        c[4] = b_to(at + 16, resume);    // b back
    };
    *reinterpret_cast<uint64_t*>(lit) = 0;
    const uint64_t clip_stub = base + 0x10, total_stub = base + 0x30;
    emit_stub(clip_stub, kStrClip, fire + kClipStoreOff + 4);
    emit_stub(total_stub, kStrTotal, fire + kTotalStoreOff + 4);
    cpu.invalidate_cache(base, 0x40);

    const uint32_t clip_jump = b_to(fire + kClipStoreOff, clip_stub);
    const uint32_t total_jump = b_to(fire + kTotalStoreOff, total_stub);
    DWORD old_prot = 0;
    VirtualProtect(clip_site, 0x60, PAGE_EXECUTE_READWRITE, &old_prot);
    *clip_site = clip_jump;
    *total_site = total_jump;
    VirtualProtect(clip_site, 0x60, old_prot, &old_prot);
    cpu.invalidate_cache(fire + kClipStoreOff, 4);
    cpu.invalidate_cache(fire + kTotalStoreOff, 4);

    m_ammo_player_literal = reinterpret_cast<volatile uint64_t*>(lit);
    std::cout << "[LCSTrainer] Unlimited ammo hooks installed in CWeapon::Fire.\n";
#else
    (void)image_game;
    (void)cpu;
#endif
}

void LCSTrainer::repair_vehicle() {
    const uint64_t veh = get_player_vehicle();
    if (!veh) {
        push_notice(ui::loc::tx("Entre em um veículo primeiro"), 1);
        return;
    }
    *reinterpret_cast<float*>(veh + kVehHealthOffset) = 1000.0f;
    if (m_cpu && m_fn_fix_car) {
        enqueue_cpu_action([this]() {
            const uint64_t v = get_player_vehicle();
            if (!v) return;
            *reinterpret_cast<float*>(v + kVehHealthOffset) = 1000.0f;
            call_guest(m_fn_fix_car, {v});
        });
    }
    push_notice(ui::loc::tx("Veículo consertado!"), 2);
}

void LCSTrainer::flip_vehicle_upright() {
    const uint64_t veh = get_player_vehicle();
    if (!veh) {
        push_notice(ui::loc::tx("Entre em um veículo primeiro"), 1);
        return;
    }
    float* pos = reinterpret_cast<float*>(veh + kEntityPosOffset);
    pos[2] += 1.5f;
    float* mat = reinterpret_cast<float*>(veh + kEntityMatrixOffset);
    mat[2] = 0.0f;
    mat[6] = 0.0f;
    mat[8] = 0.0f;
    mat[9] = 0.0f;
    mat[10] = 1.0f;
    push_notice(ui::loc::tx("Veículo desvirado!"), 2);
}

void LCSTrainer::boost_vehicle() {
    const uint64_t veh = get_player_vehicle();
    if (!veh) {
        push_notice(ui::loc::tx("Entre em um veículo primeiro"), 1);
        return;
    }
    const float* mat = reinterpret_cast<const float*>(veh + kEntityMatrixOffset);
    float* vel = reinterpret_cast<float*>(veh + kVehMoveSpeedOffset);
    vel[0] += mat[4] * 0.45f;
    vel[1] += mat[5] * 0.45f;
    vel[2] += mat[6] * 0.45f;
    push_notice(ui::loc::tx("Turbo ativado!"), 2);
}

bool LCSTrainer::save_position() {
    const uint64_t ped = get_player_ped();
    if (!ped) {
        push_notice(ui::loc::tx("Jogador ainda não carregado no mundo"), 1);
        return false;
    }
    const float* pos = reinterpret_cast<const float*>(ped + kEntityPosOffset);
    m_saved_pos[0] = pos[0];
    m_saved_pos[1] = pos[1];
    m_saved_pos[2] = pos[2];
    m_has_saved_pos = true;
    push_notice(ui::loc::tx("Posição atual salva"), 2);
    return true;
}

bool LCSTrainer::return_to_saved_position() {
    if (!m_has_saved_pos) {
        push_notice(ui::loc::tx("Nenhuma posição salva ainda"), 1);
        return false;
    }
    return teleport_to(m_saved_pos[0], m_saved_pos[1], m_saved_pos[2], ui::loc::tx("Posição Salva"));
}

bool LCSTrainer::teleport_to(float x, float y, float z, const char* label) {
    const uint64_t veh = get_player_vehicle();
    const uint64_t target = veh ? veh : get_player_ped();
    if (!target) {
        push_notice(ui::loc::tx("Jogador ainda não carregado no mundo"), 1);
        return false;
    }
    float* pos = reinterpret_cast<float*>(target + kEntityPosOffset);
    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    if (veh) {
        float* vel = reinterpret_cast<float*>(veh + kVehMoveSpeedOffset);
        vel[0] = 0.0f;
        vel[1] = 0.0f;
        vel[2] = 0.0f;
        float* mat = reinterpret_cast<float*>(veh + kEntityMatrixOffset);
        if (mat[10] < 0.7f) {
            mat[2] = 0.0f;
            mat[6] = 0.0f;
            mat[8] = 0.0f;
            mat[9] = 0.0f;
            mat[10] = 1.0f;
        }
    }
    push_notice(std::string(ui::loc::tx("Teleportado para")) + " " + (label ? label : ui::loc::tx("destino")), 2);
    return true;
}

void LCSTrainer::set_time_of_day(int hour) {
    hour = std::clamp(hour, 0, 23);
    frozen_hour = hour;
    if (m_clock_hours_addr) {
        *reinterpret_cast<uint8_t*>(m_clock_hours_addr) = static_cast<uint8_t>(hour);
        if (m_clock_mins_addr) *reinterpret_cast<uint8_t*>(m_clock_mins_addr) = 0;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %02d:00", ui::loc::tx("Relógio definido para"), hour);
        push_notice(buf, 2);
    }
}

int LCSTrainer::get_time_of_day() const {
    if (!m_clock_hours_addr) return frozen_hour;
    return static_cast<int>(*reinterpret_cast<const uint8_t*>(m_clock_hours_addr));
}

void LCSTrainer::set_weather(int weather_id) {
    if (m_cpu && m_fn_force_weather) {
        enqueue_cpu_action([this, weather_id]() {
            call_guest(m_fn_force_weather, {static_cast<uint64_t>(static_cast<int16_t>(weather_id))});
        });
        push_notice(ui::loc::tx("Clima atualizado"), 2);
    } else if (m_weather_old_addr && m_weather_new_addr) {
        *reinterpret_cast<int16_t*>(m_weather_old_addr) = static_cast<int16_t>(weather_id);
        *reinterpret_cast<int16_t*>(m_weather_new_addr) = static_cast<int16_t>(weather_id);
        if (m_weather_forced_addr)
            *reinterpret_cast<int16_t*>(m_weather_forced_addr) = static_cast<int16_t>(weather_id);
        push_notice(ui::loc::tx("Clima atualizado"), 2);
    }
}

void LCSTrainer::set_game_speed(float speed) {
    game_speed = std::clamp(speed, 0.25f, 2.0f);
    if (m_time_scale_addr) {
        *reinterpret_cast<float*>(m_time_scale_addr) = game_speed;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s %.2fx", ui::loc::tx("Velocidade do jogo:"), game_speed);
        push_notice(buf, 2);
    }
}

bool LCSTrainer::teleport_beside_nearest_vehicle(int preferred_model) {
    if (!m_attached || !m_pool_veh_ptr_addr) return false;
    const uint64_t pool_va = *reinterpret_cast<const uint64_t*>(m_pool_veh_ptr_addr);
    if (!pool_va || !is_readable_ptr(pool_va, 24)) return false;
    const uint64_t objs = *reinterpret_cast<const uint64_t*>(pool_va + 0x00ULL);
    const uint64_t bmap = *reinterpret_cast<const uint64_t*>(pool_va + 0x08ULL);
    const int32_t size  = *reinterpret_cast<const int32_t*>(pool_va + 0x10ULL);
    if (!objs || !bmap || size <= 0 || size > 4096) return false;
    const auto snap = read_snapshot();
    float rx = 1.0f, ry = 0.0f;
    float best_d2 = 180.0f * 180.0f;
    float bx = 0.0f, by = 0.0f, bz = 0.0f;
    bool found = false;
    for (int32_t i = 0; i < size; ++i) {
        if (*reinterpret_cast<const int8_t*>(bmap + static_cast<uint64_t>(i)) < 0) continue;
        const uint64_t veh = objs + static_cast<uint64_t>(i) * kVehPoolStride;
        const int16_t model_id = *reinterpret_cast<const int16_t*>(veh + 0x7cULL);
        if (preferred_model >= 0 && model_id != preferred_model) continue;
        const float hp = *reinterpret_cast<const float*>(veh + kVehHealthOffset);
        if (!std::isfinite(hp) || hp <= 0.0f) continue;
        const float* pos = reinterpret_cast<const float*>(veh + kEntityPosOffset);
        if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2])) continue;
        const float dx = pos[0] - snap.pos[0];
        const float dy = pos[1] - snap.pos[1];
        const float dz = pos[2] - snap.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < best_d2) {
            best_d2 = d2;
            bx = pos[0]; by = pos[1]; bz = pos[2];
            const float rvx = *reinterpret_cast<const float*>(veh + 0x10ULL);
            const float rvy = *reinterpret_cast<const float*>(veh + 0x14ULL);
            const float rlen = std::sqrt(rvx * rvx + rvy * rvy);
            if (std::isfinite(rlen) && rlen > 0.1f) {
                rx = rvx / rlen;
                ry = rvy / rlen;
            }
            found = true;
        }
    }
    if (!found) return false;
    return teleport_to(bx - rx * 1.45f, by - ry * 1.45f, bz + 0.25f, nullptr);
}

void LCSTrainer::push_notice(const std::string& text, int category) {
    const double t = now_seconds();
    m_notices.push_back({text, t, category});
    if (m_notices.size() > 5) m_notices.erase(m_notices.begin());
    m_log_history.insert(m_log_history.begin(), {text, t, category});
    if (m_log_history.size() > 60) m_log_history.pop_back();
}

void LCSTrainer::prune_notices(double now_sec) {
    m_notices.erase(
        std::remove_if(m_notices.begin(), m_notices.end(),
                       [now_sec](const TrainerNotice& n) { return (now_sec - n.created_at) > 3.5; }),
        m_notices.end());
}

void LCSTrainer::refresh_cached_game_data() {
    if (!m_attached) return;

    bool in_mission = false;
    if (m_on_mission_flag_addr && m_script_space_ptr_addr) {
        const uint32_t flag = *reinterpret_cast<const uint32_t*>(m_on_mission_flag_addr);
        const uint64_t space = *reinterpret_cast<const uint64_t*>(m_script_space_ptr_addr);
        if (flag != 0 && space && is_readable_ptr(space + flag, 4)) {
            in_mission = (*reinterpret_cast<const int32_t*>(space + flag) == 1);
        }
    }

    std::string last_mission_resolved;
    if (m_last_mission_addr && is_readable_ptr(m_last_mission_addr, 8)) {
        const char* raw_key = reinterpret_cast<const char*>(m_last_mission_addr);
        const size_t klen = strnlen(raw_key, 8);
        if (klen > 0) {
            std::string key(raw_key, klen);
            {
                std::lock_guard<std::mutex> lock(m_data_mutex);
                if (m_resolved_mission_key == key && !m_resolved_mission_name.empty()) {
                    last_mission_resolved = m_resolved_mission_name;
                }
            }
            if (last_mission_resolved.empty()) {
                if (is_in_main_menu() && m_cpu && m_fn_text_get && m_ctext_instance_ptr_addr && m_scratch_key_va) {
                    const uint64_t t_inst = *reinterpret_cast<const uint64_t*>(m_ctext_instance_ptr_addr);
                    if (t_inst) {
                        char* dst = reinterpret_cast<char*>(m_scratch_key_va);
                        std::memcpy(dst, key.data(), klen);
                        dst[klen] = '\0';
                        const uint64_t u16_va = m_cpu->call(m_fn_text_get, {t_inst, m_scratch_key_va});
                        if (u16_va && is_readable_ptr(u16_va, 2)) {
                            last_mission_resolved = utf16_to_utf8_plain(reinterpret_cast<const uint16_t*>(u16_va), 128);
                        }
                    }
                } else if (m_fn_text_get && m_ctext_instance_ptr_addr && m_scratch_key_va) {
                    enqueue_cpu_action([this, key]() {
                        const uint64_t t_inst = *reinterpret_cast<const uint64_t*>(m_ctext_instance_ptr_addr);
                        if (!t_inst) return;
                        char* dst = reinterpret_cast<char*>(m_scratch_key_va);
                        const size_t n = std::min<size_t>(key.size(), 63);
                        std::memcpy(dst, key.data(), n);
                        dst[n] = '\0';
                        const uint64_t u16_va = call_guest(m_fn_text_get, {t_inst, m_scratch_key_va});
                        if (u16_va && is_readable_ptr(u16_va, 2)) {
                            std::string name = utf16_to_utf8_plain(reinterpret_cast<const uint16_t*>(u16_va), 128);
                            std::lock_guard<std::mutex> lock(m_data_mutex);
                            m_resolved_mission_key = key;
                            m_resolved_mission_name = std::move(name);
                        }
                    });
                }
            }
            if (last_mission_resolved.empty()) {
                last_mission_resolved = key;
            }
        }
    }

    std::string objective_txt;
    if (m_hud_msg_addr && is_readable_ptr(m_hud_msg_addr, 2)) {
        objective_txt = utf16_to_utf8_plain(reinterpret_cast<const uint16_t*>(m_hud_msg_addr), 256);
    }

    // Build Resumo directly from our hooked live message log (Dialogue / Help / Objectives cleanly separated!)
    std::vector<RealBriefEntry> captured;
    {
        std::lock_guard<std::mutex> lk(g_brief_cap_mutex);
        captured = g_captured_briefs;
    }

    // Fallback seed for pre-existing HelpMessages not yet seen by the live hook (preserving ::TOKEN:: markers)
    if (m_help_msgs_ptr_addr && is_readable_ptr(m_help_msgs_ptr_addr, 8)) {
        const uint64_t hm_base = *reinterpret_cast<const uint64_t*>(m_help_msgs_ptr_addr);
        if (hm_base && is_readable_ptr(hm_base, 32 * 0x40)) {
            const uint8_t* hm = reinterpret_cast<const uint8_t*>(hm_base);
            for (size_t i = 0; i < 32; ++i) {
                const uint8_t* e = hm + i * 0x40;
                const uint64_t tp = *reinterpret_cast<const uint64_t*>(e + 0x00);
                if (!tp || !is_readable_ptr(tp, 2)) continue;
                const int32_t* nums = reinterpret_cast<const int32_t*>(e + 0x14);
                const uint64_t sp = *reinterpret_cast<const uint64_t*>(e + 0x30);
                std::string txt = format_gta_utf16(reinterpret_cast<const uint16_t*>(tp), nums, 6,
                                                   reinterpret_cast<const uint16_t*>(sp), false);
                if (txt.empty()) continue;
                bool exists = false;
                for (const auto& c : captured) {
                    if (c.category == 1 && c.text == txt) { exists = true; break; }
                }
                if (!exists) {
                    captured.push_back({std::move(txt), 1, {}, {}});
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_cached_last_mission = std::move(last_mission_resolved);
    m_cached_objective = std::move(objective_txt);
    m_cached_real_briefs = std::move(captured);
}

void LCSTrainer::refresh_save_slots_sync() {
    if (!m_attached || !m_cpu || !m_pc_save_helper_addr || !m_fn_populate_slot_info) return;
    call_guest(m_fn_populate_slot_info, {m_pc_save_helper_addr});
    if (!m_slots_addr || !m_slot_filename_addr || !m_slot_savedate_addr) return;

    std::vector<SaveSlotInfo> list;
    list.reserve(9);
    for (int s = 0; s < 9; ++s) {
        SaveSlotInfo info;
        info.slot = s;
        info.state = *reinterpret_cast<const int32_t*>(m_slots_addr + static_cast<size_t>(s) * 4u);
        const uint64_t fn_addr = m_slot_filename_addr + static_cast<size_t>(s) * 0x8cu;
        const uint64_t dt_addr = m_slot_savedate_addr + static_cast<size_t>(s) * 0x50u;
        info.name = utf16_to_utf8_plain(reinterpret_cast<const uint16_t*>(fn_addr), 64);
        info.date = utf16_to_utf8_plain(reinterpret_cast<const uint16_t*>(dt_addr), 36);
        list.push_back(std::move(info));
    }
    std::lock_guard<std::mutex> lock(m_data_mutex);
    m_cached_save_slots = std::move(list);
}

std::string LCSTrainer::read_last_mission_passed() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_cached_last_mission;
}

std::string LCSTrainer::read_current_objective() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_cached_objective;
}

std::vector<RealBriefEntry> LCSTrainer::read_real_briefs() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_cached_real_briefs;
}

std::vector<SaveSlotInfo> LCSTrainer::read_save_slots() const {
    std::lock_guard<std::mutex> lock(m_data_mutex);
    return m_cached_save_slots;
}

void LCSTrainer::request_refresh_save_slots() {
    enqueue_cpu_action([this]() { refresh_save_slots_sync(); });
}

void LCSTrainer::request_save_slot(int slot) {
    if (slot < 0 || slot > 8) return;
    enqueue_cpu_action([this, slot]() {
        if (!m_cpu || !m_pc_save_helper_addr || !m_fn_save_slot) return;
        const uint32_t gs = m_game_state_addr ? *reinterpret_cast<const uint32_t*>(m_game_state_addr) : 0;
        const int32_t replay = m_allow_replay_addr ? *reinterpret_cast<const int32_t*>(m_allow_replay_addr) : 0;
        bool on_mission = false;
        if (m_on_mission_flag_addr && m_script_space_ptr_addr) {
            const uint32_t flag = *reinterpret_cast<const uint32_t*>(m_on_mission_flag_addr);
            const uint64_t space = *reinterpret_cast<const uint64_t*>(m_script_space_ptr_addr);
            if (flag != 0 && space && is_readable_ptr(space + flag, 4)) {
                on_mission = (*reinterpret_cast<const int32_t*>(space + flag) == 1);
            }
        }
        if (gs != 9 || (replay > 0 && replay < 7)) {
            push_notice(ui::loc::tx("Não foi possível salvar neste estado do jogo"), 1);
            return;
        }
        if (on_mission) {
            push_notice(ui::loc::tx("Não é possível salvar durante uma missão ativa"), 1);
            return;
        }
        if (m_is_quicksave_addr) *reinterpret_cast<int32_t*>(m_is_quicksave_addr) = 0;
        const uint64_t ok = call_guest(m_fn_save_slot, {m_pc_save_helper_addr, static_cast<uint64_t>(slot)});
        if (ok) {
            refresh_save_slots_sync();
            if (m_fn_save_settings && m_femm_addr) {
                call_guest(m_fn_save_settings, {m_femm_addr});
            }
            push_notice(std::string(ui::loc::tx("Jogo salvo no Slot")) + " " + std::to_string(slot + 1) + "!", 2);
        } else {
            push_notice(std::string(ui::loc::tx("Falha ao gravar o Slot")) + " " + std::to_string(slot + 1), 1);
        }
    });
}

void LCSTrainer::request_load_slot(int slot) {
    if (slot < 0 || slot > 8) return;
    enqueue_cpu_action([this, slot]() {
        if (!m_cpu || !m_femm_addr || !m_fn_check_slot_valid || !m_fn_load_from_gate) return;
        const uint64_t valid = call_guest(m_fn_check_slot_valid, {static_cast<uint64_t>(slot)});
        if (!valid) {
            push_notice(std::string(ui::loc::tx("Slot vazio ou corrompido")) + " (" + std::to_string(slot + 1) + ")", 1);
            return;
        }
        clear_brief_capture();
        set_user_pause(false);
        *reinterpret_cast<int32_t*>(m_femm_addr + 0x118) = slot;
        *reinterpret_cast<uint8_t*>(m_femm_addr + 0x2b) = 0;
        // LoadGameFromGate natively sets femm+0x15=1, femm+0x19=1, femm+0x14=0 and b_FoundRecentSavedGameWantToLoad=1
        call_guest(m_fn_load_from_gate, {m_femm_addr});
        push_notice(std::string(ui::loc::get(ui::loc::Text::LoadedNotice)) + " (" + std::to_string(slot + 1) + ")", 2);
    });
}

void LCSTrainer::request_new_game() {
    enqueue_cpu_action([this]() {
        if (!m_cpu || !m_femm_addr || !m_fn_start_new_game) return;
        const uint32_t gs = m_game_state_addr ? *reinterpret_cast<const uint32_t*>(m_game_state_addr) : 0;
        clear_brief_capture();
        set_user_pause(false);
        if (m_want_to_load_addr) *reinterpret_cast<uint8_t*>(m_want_to_load_addr) = 0;
        *reinterpret_cast<uint8_t*>(m_femm_addr + 0x2b) = 0;
        call_guest(m_fn_start_new_game, {m_femm_addr});
        *reinterpret_cast<uint8_t*>(m_femm_addr + 0x19) = 0;
        if (gs != 9) {
            // Matching RockstarJNIlib_StartGame on initial boot from Main Menu (gGameState=7 -> 8 -> 9)
            *reinterpret_cast<uint8_t*>(m_femm_addr + 0x15) = 0;
        }
        push_notice(ui::loc::tx("Iniciando Novo Jogo em Liberty City..."), 2);
    });
}

void LCSTrainer::play_menu_sfx(MenuSfx sfx) {
    if (!m_attached || !m_cpu || !m_fn_play_fe_sound || m_video_playing) return;
    if (lcs::settings::get().audio_sfx <= 0) return;
    const uint16_t id = static_cast<uint16_t>(sfx);
    enqueue_cpu_action([this, id]() {
        if (!m_cpu || !m_fn_play_fe_sound || m_video_playing) return;
        call_guest(m_fn_play_fe_sound, {0ULL, static_cast<uint64_t>(id), 0ULL});
    });
}

const std::vector<TeleportLandmark>& LCSTrainer::landmarks() {
    static const std::vector<TeleportLandmark> kLandmarks = {
        {"Portland", "Saint Mark's Safehouse", 1145.0f, -260.0f, 16.5f},
        {"Portland", "Salvatore's Gentlemen's Club", 1455.0f, -187.0f, 56.0f},
        {"Portland", "Harwood Ferry Terminal", 1044.0f, 108.0f, 5.0f},
        {"Staunton Island", "Newport Safehouse", 103.0f, -475.0f, 16.5f},
        {"Staunton Island", "Belleville Park", 45.0f, -725.0f, 23.0f},
        {"Staunton Island", "Love Media Building", 92.0f, -1548.0f, 28.5f},
        {"Shoreside Vale", "Cedar Grove Safehouse", -663.0f, 245.0f, 65.5f},
        {"Shoreside Vale", "Francis Intl. Airport", -1255.0f, -610.0f, 11.5f},
        {"Shoreside Vale", "Cochrane Dam", -1050.0f, 440.0f, 42.0f},
    };
    return kLandmarks;
}

} // namespace lcs::game
