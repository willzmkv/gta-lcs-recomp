#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lcs::vfs {

struct WadHeader {
    uint32_t magic = 0;      // "DAWL" (0x4C574144)
    uint32_t version = 0;
    uint32_t encrypted = 0;
};

struct WadDirEntry {
    int32_t parent_index = -1;
    uint32_t name_offset = 0;
};

struct WadFatEntry {
    uint32_t path_id = 0;
    uint32_t name_id = 0;
    uint32_t dir_index = 0;
    uint32_t crc32 = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t packed_size = 0;
    uint64_t flags = 0;
};

// Minimal RAII parser and inspector for War Drum Android .wad ("DAWL") asset archives.
class WadArchive {
public:
    WadArchive() = default;

    bool open(const std::string& path);
    void close();

    bool is_open() const { return m_open; }
    const std::string& path() const { return m_path; }
    const WadHeader& header() const { return m_header; }
    uint32_t dir_count() const { return static_cast<uint32_t>(m_dirs.size()); }
    uint32_t entry_count() const { return static_cast<uint32_t>(m_entries.size()); }
    const std::vector<WadDirEntry>& dirs() const { return m_dirs; }
    const std::vector<WadFatEntry>& entries() const { return m_entries; }

private:
    std::string m_path;
    bool m_open = false;
    WadHeader m_header{};
    std::vector<WadDirEntry> m_dirs;
    std::vector<WadFatEntry> m_entries;
};

bool mount_wad_archive(const char* path);

} // namespace lcs::vfs
