// GTA: Liberty City Stories (PC) In-Game Trainer & Live Game State Bridge
// Connects directly to libGame.so ARM64 memory via ElfImage symbols & verified offsets.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace lcs::cpu {
class ElfImage;
class DynarmicCore;
}

namespace lcs::game {

enum class MenuSfx : uint16_t {
    Move    = 0xC3, // Authentic LCS cursor navigation / highlight shift
    Confirm = 0xC4, // Authentic LCS menu confirm / select / toggle
    Back    = 0xC5, // Authentic LCS menu back / cancel
    Error   = 0xC2, // Authentic LCS unavailable / invalid buzz
};

struct TrainerNotice {
    std::string text;
    double created_at = 0.0;
    int category = 2; // 0=Dialogue, 1=Help, 2=Objective/System
};

struct TeleportLandmark {
    const char* island;
    const char* name;
    float x;
    float y;
    float z;
};

struct LiveRadarBlip {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t colour = 0;
    uint8_t blip_type = 0;
    uint8_t display = 0;
    uint16_t sprite = 0;
};

struct RealBriefEntry {
    std::string text;
    int category = 0; // 0=Dialogue/Brief, 1=Help, 2=Objective/HUD
    std::string speaker;
    std::string key;
};

struct SaveSlotInfo {
    int slot = 0;      // 0..7 user slots, 8 auto/resume slot
    int state = 1;     // 0=valid, 1=empty, 2=corrupted, 3=unavailable
    std::string name;
    std::string date;
};

struct GameSnapshot {
    bool attached = false;
    bool in_gameplay = false;
    bool in_mission = false;
    int curr_area = 0;
    int32_t money = 0;
    float health = 100.0f;
    float armour = 0.0f;
    float pos[3] = {1145.0f, -260.0f, 16.5f};
    int clock_hour = 12;
    int clock_minute = 0;

    // Live CStats from libGame.so
    float progress_made = 0.0f;
    float total_progress = 100.0f;
    int32_t missions_passed = 0;
    int32_t total_missions = 70;
    int32_t people_killed = 0;
    int32_t cars_exploded = 0;
    int32_t stars_attained = 0;
    int32_t stars_evaded = 0;
    float dist_foot = 0.0f;
    float dist_car = 0.0f;
    int32_t days_passed = 0;
    int32_t times_died = 0;
    int32_t times_arrested = 0;
    int32_t rounds_fired = 0;
    int32_t bullets_hit = 0;
    int32_t jumps_found = 0;
    int32_t jumps_total = 26;
    int32_t cheated_count = 0;
};

class LCSTrainer {
public:
    static LCSTrainer& instance();

    void attach(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    void install_brief_hooks(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    void tick(float dt_sec);
    void drain_cpu_actions();
    void enqueue_cpu_action(std::function<void()> fn);

    // Live game state & pause control
    GameSnapshot read_snapshot() const;
    std::vector<LiveRadarBlip> read_radar_blips() const;
    bool is_in_gameplay() const;
    bool is_in_main_menu() const;
    void set_video_playing(bool playing) { m_video_playing = playing; }
    int get_recent_save_slot() const;
    void set_user_pause(bool paused);
    void request_quit();
    bool consume_quit_request();

    // State toggles
    bool god_mode = false;
    bool never_wanted = false;
    bool infinite_sprint = false;
    bool unlimited_ammo = false;
    bool no_reload = false;
    bool vehicle_invincible = false;
    bool free_camera = false;
    bool hide_hud = false;
    bool freeze_time = false;
    int frozen_hour = 12;
    float game_speed = 1.0f;

    // Player actions
    void heal_player();
    void set_wanted_level(int stars);
    void add_money(int amount);

    // Weapon actions
    void give_weapon(int weapon_id, int ammo);
    void give_full_loadout();

    // Vehicle actions
    void spawn_vehicle(int model_id);
    void repair_vehicle();
    void flip_vehicle_upright();
    void boost_vehicle();

    // Teleport actions
    bool save_position();
    bool return_to_saved_position();
    bool teleport_to(float x, float y, float z, const char* label = nullptr);
    bool teleport_beside_nearest_vehicle(int preferred_model = -1);
    bool has_saved_position() const { return m_has_saved_pos; }

    // World actions
    void set_time_of_day(int hour);
    int get_time_of_day() const;
    void set_weather(int weather_id);
    void set_game_speed(float speed);
    void set_game_language(int lang_id);

    // Notices, Real Brief Log & Native Save/Load Actions
    void push_notice(const std::string& text, int category = 2);
    const std::vector<TrainerNotice>& notices() const { return m_notices; }
    const std::vector<TrainerNotice>& log_history() const { return m_log_history; }
    std::string read_last_mission_passed() const;
    std::string read_current_objective() const;
    std::vector<RealBriefEntry> read_real_briefs() const;
    std::vector<SaveSlotInfo> read_save_slots() const;
    void request_refresh_save_slots();
    void request_save_slot(int slot);
    void request_load_slot(int slot);
    void request_new_game();
    void play_menu_sfx(MenuSfx sfx);
    void prune_notices(double now_sec);

    // Static tables
    static const std::vector<TeleportLandmark>& landmarks();

private:
    LCSTrainer() = default;

    uint64_t call_guest(uint64_t fn, const std::vector<uint64_t>& args = {}) const;
    void refresh_cached_game_data();
    void refresh_save_slots_sync();
    uint64_t get_player_ped() const;
    uint64_t get_player_info() const;
    uint64_t get_player_vehicle() const;

    bool m_attached = false;
    bool m_quit_requested = false;
    bool m_menu_paused = false;
    bool m_video_playing = false;
    bool m_mainmenu_slots_queried = false;
    uint64_t m_load_bias = 0;
    uint64_t m_player_idx_addr = 0;
    uint64_t m_players_arr_addr = 0;
    uint64_t m_the_camera = 0;
    uint64_t m_game_state_addr = 0;
    uint64_t m_femm_addr = 0;
    uint64_t m_user_pause_addr = 0;
    uint64_t m_clock_hours_addr = 0;
    uint64_t m_clock_mins_addr = 0;
    uint64_t m_weather_old_addr = 0;
    uint64_t m_weather_new_addr = 0;
    uint64_t m_weather_forced_addr = 0;
    uint64_t m_time_scale_addr = 0;
    uint64_t m_hud_draw_addr = 0;
    uint64_t m_radar_trace_addr = 0;
    uint64_t m_curr_area_addr = 0;
    uint64_t m_in_stadium_addr = 0;
    uint64_t m_on_mission_flag_addr = 0;
    uint64_t m_script_space_ptr_addr = 0;
    uint64_t m_pool_veh_ptr_addr = 0;
    uint64_t m_pool_ped_ptr_addr = 0;
    uint64_t m_pool_obj_ptr_addr = 0;

    // Language & Messages / Text addresses
    uint64_t m_prefs_lang_addr = 0;
    uint64_t m_prefs_lang_user_addr = 0;
    uint64_t m_prev_briefs_addr = 0;
    uint64_t m_brief_msgs_addr = 0;
    uint64_t m_help_msgs_ptr_addr = 0;
    uint64_t m_hud_msg_addr = 0;
    uint64_t m_last_mission_addr = 0;
    uint64_t m_ctext_instance_ptr_addr = 0;
    uint64_t m_scratch_key_va = 0;

    // Save/Load addresses
    uint64_t m_pc_save_helper_addr = 0;
    uint64_t m_slots_addr = 0;
    uint64_t m_slot_filename_addr = 0;
    uint64_t m_slot_savedate_addr = 0;
    uint64_t m_want_to_load_addr = 0;
    uint64_t m_is_quicksave_addr = 0;
    uint64_t m_allow_replay_addr = 0;

    // CStats addresses
    uint64_t m_stat_progress_made = 0;
    uint64_t m_stat_total_progress = 0;
    uint64_t m_stat_missions_passed = 0;
    uint64_t m_stat_total_missions = 0;
    uint64_t m_stat_people_killed = 0;
    uint64_t m_stat_cars_exploded = 0;
    uint64_t m_stat_stars_attained = 0;
    uint64_t m_stat_stars_evaded = 0;
    uint64_t m_stat_dist_foot = 0;
    uint64_t m_stat_dist_car = 0;
    uint64_t m_stat_days_passed = 0;
    uint64_t m_stat_times_died = 0;
    uint64_t m_stat_times_arrested = 0;
    uint64_t m_stat_rounds_fired = 0;
    uint64_t m_stat_bullets_hit = 0;
    uint64_t m_stat_jumps_found = 0;
    uint64_t m_stat_jumps_total = 0;
    uint64_t m_stat_cheated_count = 0;

    // Verified ARM64 native function addresses (called safely on main thread inside tick())
    uint64_t m_fn_give_weapon = 0;
    uint64_t m_fn_set_current_weapon = 0;
    uint64_t m_fn_get_weapon_info = 0;
    uint64_t m_fn_request_model = 0;
    uint64_t m_fn_load_models = 0;
    uint64_t m_fn_set_model_deletable = 0;
    uint64_t m_fn_weapon_cheat3 = 0;
    uint64_t m_fn_set_wanted = 0;
    uint64_t m_fn_vehicle_cheat = 0;
    uint64_t m_fn_tank_cheat = 0;
    uint64_t m_fn_fix_car = 0;
    uint64_t m_fn_force_weather = 0;
    uint64_t m_fn_text_get = 0;
    uint64_t m_fn_init_changed_lang = 0;
    uint64_t m_fn_populate_slot_info = 0;
    uint64_t m_fn_save_slot = 0;
    uint64_t m_fn_save_settings = 0;
    uint64_t m_fn_load_from_gate = 0;
    uint64_t m_fn_start_new_game = 0;
    uint64_t m_fn_check_slot_valid = 0;
    uint64_t m_fn_play_fe_sound = 0;
    lcs::cpu::DynarmicCore* m_cpu = nullptr;

    float m_never_wanted_accum = 0.0f;
    float m_data_refresh_accum = 1.0f;
    bool m_lang_checked = false;
    mutable bool m_has_exterior_pos = false;
    mutable float m_last_exterior_pos[3] = {1145.0f, -260.0f, 16.5f};
    bool m_has_saved_pos = false;
    float m_saved_pos[3] = {0.0f, 0.0f, 0.0f};
    std::vector<TrainerNotice> m_notices;
    std::vector<TrainerNotice> m_log_history;

    mutable std::mutex m_data_mutex;
    std::string m_cached_last_mission;
    std::string m_cached_objective;
    bool m_was_in_mission = false;
    std::vector<std::string> m_pre_mission_snapshot;
    std::vector<RealBriefEntry> m_mission_resumo;
    std::vector<RealBriefEntry> m_cached_real_briefs;
    std::vector<SaveSlotInfo> m_cached_save_slots;

    std::mutex m_action_mutex;
    std::vector<std::function<void()>> m_pending_actions;
    std::atomic<uint64_t> m_last_tick_drain_ms{0};
    std::string m_resolved_mission_key;
    std::string m_resolved_mission_name;
};

} // namespace lcs::game
