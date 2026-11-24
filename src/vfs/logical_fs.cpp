#include "logical_fs.h"
#include "mod_loader.h"
#include "settings/settings.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace lcs::vfs {

namespace {

std::string normalize_rel_path(const char* raw) {
    if (!raw) return {};
    std::string out;
    out.reserve(std::strlen(raw));
    for (const char* p = raw; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        out.push_back(c == '\\' ? '/' : static_cast<char>(std::tolower(c)));
    }
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    if (out.rfind("assets/", 0) == 0) out.erase(0, 7);
    return out;
}

} // namespace

LogicalFS& LogicalFS::instance() {
    static LogicalFS s_instance;
    return s_instance;
}

void LogicalFS::initialize(const std::string& modloader_dir, const std::string& ipad_dir) {
    ModLoader::instance().scan_directory(modloader_dir);
    m_ipad_assets.clear();
    m_mod_hits.store(0, std::memory_order_relaxed);
    m_ipad_hits.store(0, std::memory_order_relaxed);
    m_ipad_model_hits.store(0, std::memory_order_relaxed);

    std::error_code ec;
    std::filesystem::path ipad_root(ipad_dir);
    if (!std::filesystem::exists(ipad_root, ec) || !std::filesystem::is_directory(ipad_root, ec)) {
        return;
    }

    // Index iPad models, high-resolution textures, splash, and map definitions.
    static constexpr const char* kIpadSubtrees[] = {
        "Models",
        "Data/Maps",
        "Textures_PVR",
        "Textures_PVR_LOD",
        "Textures_Shared",
        "Textures_Shared_PVR",
        "SPLASH",
    };

    for (const char* sub : kIpadSubtrees) {
        std::filesystem::path sub_dir = ipad_root / sub;
        if (!std::filesystem::exists(sub_dir, ec) || !std::filesystem::is_directory(sub_dir, ec)) {
            continue;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(sub_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string rel = std::filesystem::relative(entry.path(), ipad_root, ec).generic_string();
            if (ec || rel.empty()) continue;

            std::string key = normalize_rel_path(rel.c_str());
            std::string phys = (ipad_root / rel).generic_string();
            m_ipad_assets.emplace(std::move(key), std::move(phys));
        }
    }

    // Index compatible iPad visual/environment Data files (excluding main.scm / gameplay scripts).
    static constexpr const char* kIpadDataFiles[] = {
        "Data/TIMECYC.DAT",
        "Data/WATER.DAT",
        "Data/waterpro.dat",
        "Data/PARTICLE.CFG",
        "Data/CARCOLS.DAT",
        "Data/SURFACE.DAT",
        "Data/OBJECT.DAT",
        "Data/GTA3.ZON",
        "Data/DEFAULT.IDE",
    };
    for (const char* rel : kIpadDataFiles) {
        std::filesystem::path p = ipad_root / rel;
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
            continue;
        }
        std::string key = normalize_rel_path(rel);
        std::string phys = p.generic_string();
        m_ipad_assets.emplace(key, phys);
        // Also allow bare filename lookup (e.g. "PARTICLE.CFG", "CARCOLS.DAT").
        if (const size_t slash = key.rfind('/'); slash != std::string::npos) {
            m_ipad_assets.emplace(key.substr(slash + 1), phys);
        }
    }

    // Index all iPad GLSL lighting/water/shadow/postprocess shaders at the root of assets/ipad/.
    for (const auto& entry : std::filesystem::directory_iterator(ipad_root, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::string ext = entry.path().extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".glsl") continue;
        std::string rel = entry.path().filename().generic_string();
        std::string key = normalize_rel_path(rel.c_str());
        std::string phys = (ipad_root / rel).generic_string();
        m_ipad_assets.emplace(key, phys);
        m_ipad_assets.emplace("shaders/" + key, phys);
    }

    std::cout << "[LogicalFS] Indexed " << m_ipad_assets.size()
              << " iPad Retina asset(s) (Models + Textures + Data + Shaders) from " << ipad_dir << ".\n";
}

const char* LogicalFS::resolve_bundle_path(const char* guest_path, char* scratch_buf, size_t scratch_cap) const {
    if (!guest_path || !scratch_buf || scratch_cap == 0) return guest_path;
    const std::string key = normalize_rel_path(guest_path);
    if (key.empty()) return guest_path;

    // Route ENGLISH.GXT -> assets/lang/portuguese.gxt when extra PT-BR language (7) is selected.
    if (lcs::settings::get().game_language == 7 &&
        (key == "text/english.gxt" || key == "english.gxt")) {
        static constexpr const char kPtGxt[] = "assets/lang/portuguese.gxt";
        std::error_code pt_ec;
        if (std::filesystem::exists(kPtGxt, pt_ec) && sizeof(kPtGxt) <= scratch_cap) {
            std::memcpy(scratch_buf, kPtGxt, sizeof(kPtGxt));
            return scratch_buf;
        }
    }

    // Build candidate keys: exact path first, then Android->iPad texture & model tree equivalents.
    std::array<std::string, 5> candidates;
    size_t cand_count = 0;
    auto push_candidate = [&](std::string cand) {
        if (cand_count < candidates.size()) {
            candidates[cand_count++] = std::move(cand);
        }
    };
    push_candidate(key);

    auto replace_ext_pvr = [](const std::string& p) -> std::string {
        if (p.size() > 4 && (p.ends_with(".etc") || p.ends_with(".ktx"))) {
            return p.substr(0, p.size() - 4) + ".pvr";
        }
        return {};
    };

    if (key.rfind("textures_etc/", 0) == 0) {
        std::string mapped = "textures_pvr/" + key.substr(13);
        if (std::string pvr = replace_ext_pvr(mapped); !pvr.empty()) push_candidate(std::move(pvr));
        push_candidate(std::move(mapped));
    } else if (key.rfind("textures_etc_lod/", 0) == 0) {
        std::string mapped = "textures_pvr_lod/" + key.substr(17);
        if (std::string pvr = replace_ext_pvr(mapped); !pvr.empty()) push_candidate(std::move(pvr));
        push_candidate(std::move(mapped));
    } else if (key.rfind("textures_shared_etc/", 0) == 0) {
        std::string mapped = "textures_shared_pvr/" + key.substr(20);
        if (std::string pvr = replace_ext_pvr(mapped); !pvr.empty()) push_candidate(std::move(pvr));
        push_candidate(std::move(mapped));
        push_candidate("textures_shared/" + key.substr(20));
    } else if (key.rfind("textures_shared/", 0) == 0) {
        push_candidate("textures_shared_pvr/" + key.substr(16));
    } else if (key.rfind("textures_pvr/", 0) == 0) {
        push_candidate("textures_etc/" + key.substr(13));
    } else if (key.ends_with(".glsl")) {
        if (const size_t slash = key.rfind('/'); slash != std::string::npos) {
            push_candidate(key.substr(slash + 1));
        }
    }

    std::string resolved;
    for (size_t i = 0; i < cand_count; ++i) {
        if (ModLoader::instance().resolve_override(candidates[i], resolved)) {
            if (resolved.size() + 1 <= scratch_cap) {
                std::memcpy(scratch_buf, resolved.c_str(), resolved.size() + 1);
                m_mod_hits.fetch_add(1, std::memory_order_relaxed);
                return scratch_buf;
            }
        }
    }

    if (const char* no_ipad = std::getenv("LCS_DIAG_NO_IPAD"); no_ipad && *no_ipad == '1') {
        return guest_path;
    }

    for (size_t i = 0; i < cand_count; ++i) {
        auto it = m_ipad_assets.find(candidates[i]);
        if (it != m_ipad_assets.end() && it->second.size() + 1 <= scratch_cap) {
            std::memcpy(scratch_buf, it->second.c_str(), it->second.size() + 1);
            m_ipad_hits.fetch_add(1, std::memory_order_relaxed);
            const bool is_tex = candidates[i].rfind("textures_", 0) == 0;
            if (!is_tex) {
                const uint64_t mh = m_ipad_model_hits.fetch_add(1, std::memory_order_relaxed) + 1;
                if (mh <= 120) {
                    std::cout << "[LogicalFS] iPad asset override: " << guest_path
                              << " -> " << it->second << "\n";
                }
            }
            return scratch_buf;
        }
    }

    return guest_path;
}

void init_logical_fs() {
    LogicalFS::instance().initialize("modloader", "assets/ipad");
}

void add_bundle_root(const char* path) {
    (void)path;
}

} // namespace lcs::vfs
