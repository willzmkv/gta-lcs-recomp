#include "mod_loader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace lcs::vfs {

namespace {

std::string normalize_vfs_key(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if (c == '\\') {
            out.push_back('/');
        } else {
            out.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    if (out.rfind("assets/", 0) == 0) out.erase(0, 7);
    return out;
}

int read_mod_priority(const std::filesystem::path& mod_dir) {
    std::filesystem::path prio_file = mod_dir / "priority.txt";
    std::ifstream in(prio_file);
    if (!in.is_open()) return 50;
    int val = 50;
    if (in >> val) return val;
    return 50;
}

} // namespace

ModLoader& ModLoader::instance() {
    static ModLoader s_instance;
    return s_instance;
}

void ModLoader::scan_directory(const std::string& modloader_root) {
    m_mods.clear();
    m_overrides.clear();

    std::error_code ec;
    std::filesystem::path root(modloader_root);
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) {
        return;
    }

    struct DiscoveredMod {
        ModPackInfo info;
        std::filesystem::path dir;
    };
    std::vector<DiscoveredMod> discovered;

    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.') continue; // Allow disabling mods with a leading dot
        DiscoveredMod dm;
        dm.info.name = name;
        dm.info.priority = read_mod_priority(entry.path());
        dm.dir = entry.path();
        discovered.push_back(std::move(dm));
    }

    // Lower priority scanned first so higher priority (and later alphabetical names) win on insert_or_assign.
    std::sort(discovered.begin(), discovered.end(), [](const DiscoveredMod& a, const DiscoveredMod& b) {
        if (a.info.priority != b.info.priority) return a.info.priority < b.info.priority;
        return a.info.name < b.info.name;
    });

    for (auto& dm : discovered) {
        size_t count = 0;
        for (const auto& file_it : std::filesystem::recursive_directory_iterator(dm.dir, ec)) {
            if (!file_it.is_regular_file(ec)) continue;
            if (file_it.path().filename() == "priority.txt" && file_it.path().parent_path() == dm.dir) continue;

            std::string rel = std::filesystem::relative(file_it.path(), dm.dir, ec).generic_string();
            if (ec || rel.empty()) continue;

            std::string key = normalize_vfs_key(rel);
            std::string phys = (root / dm.info.name / rel).generic_string();
            m_overrides.insert_or_assign(std::move(key), std::move(phys));
            count++;
        }
        dm.info.file_count = count;
        m_mods.push_back(dm.info);
    }

    if (!m_mods.empty()) {
        std::cout << "[ModLoader] Indexed " << m_mods.size() << " mod folder(s), "
                  << m_overrides.size() << " override file(s).\n";
        for (size_t i = m_mods.size(); i > 0; --i) {
            const auto& m = m_mods[i - 1];
            std::cout << "  - [Priority " << m.priority << "] " << m.name
                      << " (" << m.file_count << " file(s))\n";
        }
    }
}

bool ModLoader::resolve_override(const std::string& normalized_rel, std::string& out_physical_rel) const {
    auto it = m_overrides.find(normalized_rel);
    if (it == m_overrides.end()) return false;
    out_physical_rel = it->second;
    return true;
}

void scan_modloader_directory() {
    ModLoader::instance().scan_directory("modloader");
}

} // namespace lcs::vfs
