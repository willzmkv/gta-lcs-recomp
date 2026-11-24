#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lcs::cpu {
class DynarmicCore;
class ElfImage;
} // namespace lcs::cpu

namespace lcs::game {

// Streams multiple hex cells of static iPad/Mobile world geometry (Models/<level>.img + .lvz)
// around the camera for extended HD draw distances (world.hd_radius = 0..3) while keeping native
// cWorldStream::Update() / CleanupOldSector() state completely isolated from synthetic frames.
class LcsWorldHd {
public:
    static LcsWorldHd& instance();

    void initialize(const lcs::cpu::ElfImage& image_game,
                    lcs::cpu::DynarmicCore& cpu,
                    bool enable_dump = false);

    void set_radius(int level);
    int radius() const { return m_radius; }

    void on_frame(uint64_t frame_index);
    void dump_diagnostics(const char* out_path) const;

    // Used by LcsDistance (SetupBigBuildingVisibility) to hide overlapping blurry LOD* twins
    // whenever an entity's ground footprint falls inside an actively loaded HD cell.
    bool is_point_covered_by_hd(float x, float y) const;
    bool is_circle_covered_by_hd(float x, float y, float radius) const;
    int current_level() const { return m_active_level; }
    int loaded_cell_count() const;
    int rendered_instance_count() const { return m_combined_inst_count; }

private:
    LcsWorldHd() = default;
    ~LcsWorldHd();
    LcsWorldHd(const LcsWorldHd&) = delete;
    LcsWorldHd& operator=(const LcsWorldHd&) = delete;

    struct Impl;
public:
    struct Stats {
        int radius = 0;
        int active_level = 0;
        int center_cx = -1;
        int center_cy = -1;
        int cached_cells = 0;
        int ready_cells = 0;
        int combined_instances = 0;
        int lq_replaced = 0;  // LQ instances left out because a drawn list's detail replaces them
    };
    Stats get_stats() const;

private:
    struct HookOps;
    friend struct HookOps;

    bool init_arena_and_stubs(const lcs::cpu::ElfImage& image_game, lcs::cpu::DynarmicCore& cpu);
    uint64_t on_world_render_pre(uint64_t ws_ptr, int pass);
    void on_world_render_post(uint64_t ws_ptr);
    void on_world_update_pre(uint64_t ws_ptr);
    void on_world_update_post(uint64_t ws_ptr);
    void on_world_load_level_pre(uint64_t ws_ptr, int new_level);
    void on_world_load_level_post(uint64_t ws_ptr);
    void on_world_load_scene_pre(uint64_t ws_ptr);
    void on_world_load_scene_post(uint64_t ws_ptr);
    void on_stream_pre(uint64_t ws_ptr);
    void on_stream_post(uint64_t ws_ptr);
    void on_stream_call_pre(uint64_t ws_ptr, uint64_t hdr_ptr);
    void on_poll_streaming_pre(uint64_t ws_ptr);
    void on_poll_streaming_post(uint64_t ws_ptr);
    void on_cancel_streaming_pre(uint64_t ws_ptr);
    void on_cancel_streaming_post(uint64_t ws_ptr);
    void on_streaming_cb_pre(uint64_t ws_ptr);
    void on_streaming_cb_post(uint64_t ws_ptr);
    void on_swap_in_buf_pre(uint64_t ws_ptr);
    void on_swap_in_buf_post(uint64_t ws_ptr);
    void on_tex_mgr_lock();
    void on_tex_mgr_unlock();
    void sanitize_stream_table(uint64_t ws_ptr, bool scrub_prev, bool scrub_stream);
    uint64_t on_remove_old_models_pre(uint64_t ws_ptr);
    void on_remove_old_models_post(uint64_t ws_ptr);
    uint64_t on_swap_sector_pre(uint64_t ws_ptr);
    void on_swap_sector_post(uint64_t ws_ptr);
    uint64_t on_lgl_destroy_pre(uint64_t mgr_ptr, int destroy_all);

    bool ensure_level_img_open(int level);
    void close_level_img();
    void flush_all_cells(uint64_t ws_ptr, bool destroy_lgl);
    void step_streaming(uint64_t ws_ptr, int max_loads);
    void rebuild_combined_chunk(uint64_t ws_ptr);
    void ensure_pairs_and_models(uint64_t ws_ptr);
    bool area_allows_instance(uint64_t ws_ptr, uint64_t dir_ptr, uint16_t inst_id) const;

    Impl* m_impl = nullptr;
    lcs::cpu::DynarmicCore* m_cpu = nullptr;
    uint64_t m_load_bias = 0;

    int m_radius = 2;
    int m_active_level = 0;
    int m_center_cx = -1;
    int m_center_cy = -1;
    int m_combined_inst_count = 0;
    uint64_t m_frame_index = 0;
    bool m_initialized = false;
    bool m_enable_dump = false;
    bool m_combined_dirty = true;
    bool m_reregister_needed = false;

    // Guest symbol RVAs / pointers resolved on init
    uint64_t m_got_ws_inst = 0;
    uint64_t m_got_world_mgr = 0;
    uint64_t m_got_world_creator = 0;
    uint64_t m_got_camera = 0;
    uint64_t m_got_dyn_shadows_active = 0;
    uint64_t m_got_water_scroll = 0;
    uint64_t m_fn_fixup = 0;
    uint64_t m_fn_get_model = 0;
    uint64_t m_fn_destroy_model = 0;
    uint64_t m_fn_creator_remove = 0;
    uint64_t m_fn_lgl_mutex_lock = 0;
    uint64_t m_fn_lgl_mutex_unlock = 0;
    uint64_t m_fn_is_time_in_range = 0;

    uint64_t m_orig_render = 0;
    uint64_t m_orig_update = 0;
    uint64_t m_orig_load_level = 0;
    uint64_t m_orig_remove_old_models = 0;
};

} // namespace lcs::game
