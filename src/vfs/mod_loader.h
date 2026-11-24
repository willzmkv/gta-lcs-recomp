#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace lcs::vfs {

struct ModPackInfo {
    std::string name;
    int priority = 50;
    size_t file_count = 0;
};

// Scans modloader/<mod_name>/ directories and resolves highest-priority asset overrides in O(1).
class ModLoader {
public:
    static ModLoader& instance();

    void scan_directory(const std::string& modloader_root = "modloader");
    bool resolve_override(const std::string& normalized_rel, std::string& out_physical_rel) const;

    const std::vector<ModPackInfo>& mods() const { return m_mods; }
    size_t file_count() const { return m_overrides.size(); }

private:
    ModLoader() = default;

    std::vector<ModPackInfo> m_mods;
    std::unordered_map<std::string, std::string> m_overrides;
};

void scan_modloader_directory();

} // namespace lcs::vfs
