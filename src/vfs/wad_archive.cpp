#include "wad_archive.h"

#include <cstring>
#include <fstream>
#include <iostream>

namespace lcs::vfs {

namespace {

constexpr uint32_t kWadMagic = 0x4C574144u; // "DAWL"
constexpr uint32_t kWadHeaderSize = 0x800u;
constexpr uint32_t kDefaultXorKey = 0x66AF66AFu;

void xor_decrypt_inplace(uint8_t* data, size_t size, uint32_t key_le) {
    const uint8_t k[4] = {
        static_cast<uint8_t>(key_le & 0xffu),
        static_cast<uint8_t>((key_le >> 8) & 0xffu),
        static_cast<uint8_t>((key_le >> 16) & 0xffu),
        static_cast<uint8_t>((key_le >> 24) & 0xffu),
    };
    for (size_t i = 0; i < size; ++i) {
        data[i] ^= k[i & 3u];
    }
}

template <typename T>
T read_le(const uint8_t* p) {
    T val{};
    std::memcpy(&val, p, sizeof(T));
    return val;
}

} // namespace

bool WadArchive::open(const std::string& path) {
    close();

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        return false;
    }

    const auto file_end = in.tellg();
    if (file_end < static_cast<std::streamoff>(kWadHeaderSize + 8)) {
        return false;
    }
    const uint64_t file_size = static_cast<uint64_t>(file_end);
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> hdr_buf(kWadHeaderSize);
    if (!in.read(reinterpret_cast<char*>(hdr_buf.data()), static_cast<std::streamsize>(hdr_buf.size()))) {
        return false;
    }

    // Plaintext headers begin with "DAWL"; XOR-obfuscated War Drum WADs mask the padding with kDefaultXorKey.
    uint32_t xor_key = 0;
    const uint32_t raw_magic = read_le<uint32_t>(hdr_buf.data());
    if (raw_magic != kWadMagic) {
        xor_key = read_le<uint32_t>(hdr_buf.data() + 12);
        if ((raw_magic ^ xor_key) != kWadMagic) {
            xor_key = kDefaultXorKey;
            if ((raw_magic ^ xor_key) != kWadMagic) {
                return false;
            }
        }
        xor_decrypt_inplace(hdr_buf.data(), hdr_buf.size(), xor_key);
    }

    m_header.magic = read_le<uint32_t>(hdr_buf.data() + 0);
    m_header.version = read_le<uint32_t>(hdr_buf.data() + 4);
    m_header.encrypted = read_le<uint32_t>(hdr_buf.data() + 8);

    uint8_t count_buf[4]{};
    if (!in.read(reinterpret_cast<char*>(count_buf), sizeof(count_buf))) {
        return false;
    }
    if (xor_key != 0) {
        xor_decrypt_inplace(count_buf, sizeof(count_buf), xor_key);
    }
    const uint32_t num_dirs = read_le<uint32_t>(count_buf);
    if (num_dirs > 262144u) {
        return false;
    }

    const uint64_t dirs_bytes = static_cast<uint64_t>(num_dirs) * 8ULL;
    if (kWadHeaderSize + 4ULL + dirs_bytes + 4ULL > file_size) {
        return false;
    }

    std::vector<uint8_t> dir_raw(dirs_bytes);
    if (dirs_bytes > 0 && !in.read(reinterpret_cast<char*>(dir_raw.data()), static_cast<std::streamsize>(dirs_bytes))) {
        return false;
    }
    if (xor_key != 0 && dirs_bytes > 0) {
        xor_decrypt_inplace(dir_raw.data(), dir_raw.size(), xor_key);
    }

    m_dirs.resize(num_dirs);
    for (uint32_t i = 0; i < num_dirs; ++i) {
        m_dirs[i].parent_index = read_le<int32_t>(dir_raw.data() + static_cast<size_t>(i) * 8u);
        m_dirs[i].name_offset = read_le<uint32_t>(dir_raw.data() + static_cast<size_t>(i) * 8u + 4u);
    }

    if (!in.read(reinterpret_cast<char*>(count_buf), sizeof(count_buf))) {
        close();
        return false;
    }
    if (xor_key != 0) {
        xor_decrypt_inplace(count_buf, sizeof(count_buf), xor_key);
    }
    const uint32_t num_entries = read_le<uint32_t>(count_buf);
    if (num_entries > 1048576u) {
        close();
        return false;
    }

    const uint64_t fat_bytes = static_cast<uint64_t>(num_entries) * 48ULL;
    if (kWadHeaderSize + 8ULL + dirs_bytes + fat_bytes > file_size) {
        close();
        return false;
    }

    std::vector<uint8_t> fat_raw(fat_bytes);
    if (fat_bytes > 0 && !in.read(reinterpret_cast<char*>(fat_raw.data()), static_cast<std::streamsize>(fat_bytes))) {
        close();
        return false;
    }
    if (xor_key != 0 && fat_bytes > 0) {
        xor_decrypt_inplace(fat_raw.data(), fat_raw.size(), xor_key);
    }

    m_entries.resize(num_entries);
    for (uint32_t i = 0; i < num_entries; ++i) {
        const uint8_t* p = fat_raw.data() + static_cast<size_t>(i) * 48u;
        m_entries[i].path_id = read_le<uint32_t>(p + 0);
        m_entries[i].name_id = read_le<uint32_t>(p + 4);
        m_entries[i].dir_index = read_le<uint32_t>(p + 8);
        m_entries[i].crc32 = read_le<uint32_t>(p + 12);
        m_entries[i].offset = read_le<uint64_t>(p + 16);
        m_entries[i].size = read_le<uint64_t>(p + 24);
        m_entries[i].packed_size = read_le<uint64_t>(p + 32);
        m_entries[i].flags = read_le<uint64_t>(p + 40);
    }

    m_path = path;
    m_open = true;
    return true;
}

void WadArchive::close() {
    m_path.clear();
    m_open = false;
    m_header = {};
    m_dirs.clear();
    m_entries.clear();
}

bool mount_wad_archive(const char* path) {
    if (!path) return false;
    WadArchive wad;
    if (!wad.open(path)) {
        return false;
    }
    std::cout << "[WadArchive] Verified " << path
              << " (v" << wad.header().version
              << ", dirs=" << wad.dir_count()
              << ", entries=" << wad.entry_count() << ").\n";
    return true;
}

} // namespace lcs::vfs
