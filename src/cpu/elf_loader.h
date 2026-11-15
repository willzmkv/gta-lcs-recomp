#pragma once

#include "elf_types.h"
#include <string>
#include <vector>
#include <functional>

namespace lcs::cpu {

struct RelocationStats {
    size_t relative = 0;
    size_t absolute = 0;
    size_t global_data = 0;
    size_t jump_slots = 0;
    size_t unresolved = 0;
};

// Callback to resolve external symbols
// Returns the guest virtual address of the symbol, or 0 if not found
using SymbolResolver = std::function<uint64_t(const char* name, uint8_t binding)>;

class ElfImage {
public:
    ElfImage();
    ~ElfImage();

    // Disallow copy, allow move
    ElfImage(const ElfImage&) = delete;
    ElfImage& operator=(const ElfImage&) = delete;
    ElfImage(ElfImage&& other) noexcept;
    ElfImage& operator=(ElfImage&& other) noexcept;

    // Load ELF segments into virtual memory at specified load_bias
    bool load(const std::string& filepath, uint64_t load_bias);

    // Apply relocations
    bool relocate(const SymbolResolver& resolver, RelocationStats* out_stats = nullptr);

    // Look up an exported symbol by name
    uint64_t find_symbol(const char* name) const;

    // Get list of constructor function pointers (.init_array)
    std::vector<uint64_t> get_init_array() const;

    // Properties
    uint64_t get_load_bias() const { return m_load_bias; }
    uint64_t get_min_vaddr() const { return m_min_vaddr; }
    uint64_t get_max_vaddr() const { return m_max_vaddr; }
    size_t get_image_size() const { return m_image_size; }
    size_t get_sym_count() const { return m_sym_count; }
    void* get_mapping() const { return m_mapping; }
    bool is_loaded() const { return m_mapping != nullptr; }
    bool is_relocated() const { return m_relocated; }
    const std::string& get_name() const { return m_name; }

private:
    void cleanup();
    bool parse_dynamic_section(const std::vector<Elf64_Phdr>& phdrs);

    std::string m_name;
    uint64_t m_load_bias = 0;
    uint64_t m_min_vaddr = 0;
    uint64_t m_max_vaddr = 0;
    size_t m_image_size = 0;
    void* m_mapping = nullptr;
    bool m_relocated = false;

    // Dynamic section pointers (within guest virtual mapping)
    const Elf64_Sym* m_symtab = nullptr;
    const char* m_strtab = nullptr;
    size_t m_strsz = 0;
    size_t m_sym_count = 0;

    const Elf64_Rela* m_rela = nullptr;
    size_t m_rela_count = 0;

    const Elf64_Rela* m_jmprel = nullptr;
    size_t m_jmprel_count = 0;

    const uint64_t* m_init_array = nullptr;
    size_t m_init_array_count = 0;
};

} // namespace lcs::cpu
