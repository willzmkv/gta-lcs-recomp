#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace lcs::vfs {

// Routes bundle file requests across priority layers: modloader/ -> assets/ipad/ -> base WADs.
class LogicalFS {
public:
    static LogicalFS& instance();

    void initialize(const std::string& modloader_dir = "modloader",
                    const std::string& ipad_dir = "assets/ipad");

    // Resolves a logical guest path into an override relative path inside scratch_buf, or returns guest_path unchanged.
    const char* resolve_bundle_path(const char* guest_path, char* scratch_buf, size_t scratch_cap) const;

    size_t ipad_asset_count() const { return m_ipad_assets.size(); }
    uint64_t mod_hit_count() const { return m_mod_hits.load(std::memory_order_relaxed); }
    uint64_t ipad_hit_count() const { return m_ipad_hits.load(std::memory_order_relaxed); }
    uint64_t ipad_model_hit_count() const { return m_ipad_model_hits.load(std::memory_order_relaxed); }

private:
    LogicalFS() = default;

    std::unordered_map<std::string, std::string> m_ipad_assets;
    mutable std::atomic<uint64_t> m_mod_hits{0};
    mutable std::atomic<uint64_t> m_ipad_hits{0};
    mutable std::atomic<uint64_t> m_ipad_model_hits{0};
};

void init_logical_fs();
void add_bundle_root(const char* path);

} // namespace lcs::vfs
