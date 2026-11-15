#include "elf_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace lcs::cpu {

static constexpr uint64_t PAGE_SIZE_4K = 4096;
static inline uint64_t page_align_down(uint64_t val) { return val & ~(PAGE_SIZE_4K - 1); }
static inline uint64_t page_align_up(uint64_t val) { return (val + PAGE_SIZE_4K - 1) & ~(PAGE_SIZE_4K - 1); }

struct GnuHashHeader {
    uint32_t nbuckets;
    uint32_t symndx;
    uint32_t maskwords;
    uint32_t shift2;
};

static size_t determine_gnu_hash_sym_count(const void* gnu_hash_ptr) {
    if (!gnu_hash_ptr) return 0;
    auto* hdr = reinterpret_cast<const GnuHashHeader*>(gnu_hash_ptr);
    const uint64_t* bloom = reinterpret_cast<const uint64_t*>(hdr + 1);
    const uint32_t* buckets = reinterpret_cast<const uint32_t*>(bloom + hdr->maskwords);
    const uint32_t* chains = buckets + hdr->nbuckets;

    uint32_t max_sym = 0;
    for (uint32_t i = 0; i < hdr->nbuckets; ++i) {
        if (buckets[i] > max_sym) {
            max_sym = buckets[i];
        }
    }
    if (max_sym < hdr->symndx) {
        return hdr->symndx;
    }
    uint32_t curr = max_sym;
    while (true) {
        uint32_t chain_idx = curr - hdr->symndx;
        uint32_t hval = chains[chain_idx];
        curr++;
        if ((hval & 1) != 0) {
            break;
        }
    }
    return curr;
}

ElfImage::ElfImage() = default;

ElfImage::~ElfImage() {
    cleanup();
}

ElfImage::ElfImage(ElfImage&& other) noexcept {
    *this = std::move(other);
}

ElfImage& ElfImage::operator=(ElfImage&& other) noexcept {
    if (this != &other) {
        cleanup();
        m_name = std::move(other.m_name);
        m_load_bias = other.m_load_bias;
        m_min_vaddr = other.m_min_vaddr;
        m_max_vaddr = other.m_max_vaddr;
        m_image_size = other.m_image_size;
        m_mapping = other.m_mapping;
        m_relocated = other.m_relocated;
        m_symtab = other.m_symtab;
        m_strtab = other.m_strtab;
        m_strsz = other.m_strsz;
        m_sym_count = other.m_sym_count;
        m_rela = other.m_rela;
        m_rela_count = other.m_rela_count;
        m_jmprel = other.m_jmprel;
        m_jmprel_count = other.m_jmprel_count;
        m_init_array = other.m_init_array;
        m_init_array_count = other.m_init_array_count;

        other.m_mapping = nullptr;
        other.m_image_size = 0;
        other.m_relocated = false;
    }
    return *this;
}

void ElfImage::cleanup() {
    if (m_mapping) {
#if defined(LCS_PLATFORM_WINDOWS)
        VirtualFree(m_mapping, 0, MEM_RELEASE);
#else
        munmap(m_mapping, m_image_size);
#endif
        m_mapping = nullptr;
    }
    m_image_size = 0;
    m_relocated = false;
}

bool ElfImage::load(const std::string& filepath, uint64_t load_bias) {
    cleanup();
    m_name = filepath;
    m_load_bias = load_bias;

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[ELF] Failed to open file: " << filepath << "\n";
        return false;
    }

    Elf64_Ehdr ehdr;
    file.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr));
    if (!file || file.gcount() != sizeof(ehdr)) {
        std::cerr << "[ELF] Corrupted ELF header: " << filepath << "\n";
        return false;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3) {
        std::cerr << "[ELF] Invalid magic bytes: " << filepath << "\n";
        return false;
    }

    if (ehdr.e_machine != EM_AARCH64) {
        std::cerr << "[ELF] Machine is not AArch64 (" << ehdr.e_machine << ")\n";
        return false;
    }

    // Read program headers
    std::vector<Elf64_Phdr> phdrs(ehdr.e_phnum);
    file.seekg(ehdr.e_phoff);
    file.read(reinterpret_cast<char*>(phdrs.data()), ehdr.e_phnum * sizeof(Elf64_Phdr));
    if (!file) {
        std::cerr << "[ELF] Failed to read program headers\n";
        return false;
    }

    // Determine virtual address boundaries
    bool has_load = false;
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t max_vaddr = 0;

    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD) {
            has_load = true;
            min_vaddr = std::min(min_vaddr, page_align_down(ph.p_vaddr));
            max_vaddr = std::max(max_vaddr, page_align_up(ph.p_vaddr + ph.p_memsz));
        }
    }

    if (!has_load) {
        std::cerr << "[ELF] No PT_LOAD segments found in " << filepath << "\n";
        return false;
    }

    m_min_vaddr = min_vaddr;
    m_max_vaddr = max_vaddr;
    m_image_size = max_vaddr - min_vaddr;

    // Allocate virtual memory at target load bias
    uint64_t target_base = m_load_bias + m_min_vaddr;
#if defined(LCS_PLATFORM_WINDOWS)
    void* mapped = VirtualAlloc(reinterpret_cast<void*>(target_base), m_image_size,
                                MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void* mapped = mmap(reinterpret_cast<void*>(target_base), m_image_size,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped == MAP_FAILED) mapped = nullptr;
#endif

    if (!mapped) {
        std::cerr << "[ELF] Failed to allocate " << m_image_size << " bytes at 0x"
                  << std::hex << target_base << std::dec << "\n";
        return false;
    }

    m_mapping = mapped;
    std::memset(m_mapping, 0, m_image_size);

    // Map segments
    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_LOAD) {
            uint64_t seg_dest = m_load_bias + ph.p_vaddr;
            file.seekg(ph.p_offset);
            if (ph.p_filesz > 0) {
                file.read(reinterpret_cast<char*>(seg_dest), ph.p_filesz);
            }
        }
    }

    return parse_dynamic_section(phdrs);
}

bool ElfImage::parse_dynamic_section(const std::vector<Elf64_Phdr>& phdrs) {
    uint64_t dyn_addr = 0;
    size_t dyn_sz = 0;

    for (const auto& ph : phdrs) {
        if (ph.p_type == PT_DYNAMIC) {
            dyn_addr = m_load_bias + ph.p_vaddr;
            dyn_sz = ph.p_memsz;
            break;
        }
    }

    if (dyn_addr == 0) {
        std::cerr << "[ELF] Missing PT_DYNAMIC segment\n";
        return false;
    }

    const Elf64_Dyn* dyn = reinterpret_cast<const Elf64_Dyn*>(dyn_addr);
    size_t num_dyn = dyn_sz / sizeof(Elf64_Dyn);

    uint64_t rela_addr = 0;
    size_t rela_sz = 0;
    uint64_t jmprel_addr = 0;
    size_t plt_rel_sz = 0;
    const void* gnu_hash_ptr = nullptr;
    const void* sysv_hash_ptr = nullptr;

    for (size_t i = 0; i < num_dyn; i++) {
        switch (dyn[i].d_tag) {
            case DT_STRTAB:
                m_strtab = reinterpret_cast<const char*>(m_load_bias + dyn[i].d_un.d_ptr);
                break;
            case DT_STRSZ:
                m_strsz = dyn[i].d_un.d_val;
                break;
            case DT_SYMTAB:
                m_symtab = reinterpret_cast<const Elf64_Sym*>(m_load_bias + dyn[i].d_un.d_ptr);
                break;
            case DT_RELA:
                rela_addr = m_load_bias + dyn[i].d_un.d_ptr;
                break;
            case DT_RELASZ:
                rela_sz = dyn[i].d_un.d_val;
                break;
            case DT_JMPREL:
                jmprel_addr = m_load_bias + dyn[i].d_un.d_ptr;
                break;
            case DT_PLTRELSZ:
                plt_rel_sz = dyn[i].d_un.d_val;
                break;
            case DT_INIT_ARRAY:
                m_init_array = reinterpret_cast<const uint64_t*>(m_load_bias + dyn[i].d_un.d_ptr);
                break;
            case DT_INIT_ARRAYSZ:
                m_init_array_count = dyn[i].d_un.d_val / sizeof(uint64_t);
                break;
            case DT_GNU_HASH:
                gnu_hash_ptr = reinterpret_cast<const void*>(m_load_bias + dyn[i].d_un.d_ptr);
                break;
            case DT_HASH:
                sysv_hash_ptr = reinterpret_cast<const void*>(m_load_bias + dyn[i].d_un.d_ptr);
                break;
            case DT_NULL:
                break;
        }
        if (dyn[i].d_tag == DT_NULL) break;
    }

    if (gnu_hash_ptr) {
        m_sym_count = determine_gnu_hash_sym_count(gnu_hash_ptr);
    } else if (sysv_hash_ptr) {
        const uint32_t* h = reinterpret_cast<const uint32_t*>(sysv_hash_ptr);
        m_sym_count = h[1]; // nchain
    }

    if (rela_addr) {
        m_rela = reinterpret_cast<const Elf64_Rela*>(rela_addr);
        m_rela_count = rela_sz / sizeof(Elf64_Rela);
    }
    if (jmprel_addr) {
        m_jmprel = reinterpret_cast<const Elf64_Rela*>(jmprel_addr);
        m_jmprel_count = plt_rel_sz / sizeof(Elf64_Rela);
    }

    return true;
}

bool ElfImage::relocate(const SymbolResolver& resolver, RelocationStats* out_stats) {
    if (!m_mapping) return false;

    RelocationStats stats;

    auto apply_table = [&](const Elf64_Rela* table, size_t count) {
        if (!table) return;
        for (size_t i = 0; i < count; i++) {
            const auto& rel = table[i];
            uint32_t type = static_cast<uint32_t>(ELF64_R_TYPE(rel.r_info));
            uint32_t sym_idx = static_cast<uint32_t>(ELF64_R_SYM(rel.r_info));

            uint64_t* target = reinterpret_cast<uint64_t*>(m_load_bias + rel.r_offset);

            uint64_t sym_val = 0;
            if (sym_idx != 0 && sym_idx < m_sym_count && m_symtab) {
                const Elf64_Sym& sym = m_symtab[sym_idx];
                const char* sym_name = (m_strtab && sym.st_name < m_strsz) ? (m_strtab + sym.st_name) : "";

                if (sym.st_shndx == 0) {
                    // External import
                    uint8_t bind = ELF64_ST_BIND(sym.st_info);
                    sym_val = resolver ? resolver(sym_name, bind) : 0;
                    if (sym_val == 0 && bind != STB_WEAK) {
                        stats.unresolved++;
                    }
                } else {
                    // Internal symbol
                    sym_val = m_load_bias + sym.st_value;
                }
            }

            switch (type) {
                case R_AARCH64_RELATIVE:
                    *target = m_load_bias + rel.r_addend;
                    stats.relative++;
                    break;
                case R_AARCH64_ABS64:
                    *target = sym_val + rel.r_addend;
                    stats.absolute++;
                    break;
                case R_AARCH64_GLOB_DAT:
                    *target = sym_val + rel.r_addend;
                    stats.global_data++;
                    break;
                case R_AARCH64_JUMP_SLOT:
                    *target = sym_val + rel.r_addend;
                    stats.jump_slots++;
                    break;
                default:
                    break;
            }
        }
    };

    apply_table(m_rela, m_rela_count);
    apply_table(m_jmprel, m_jmprel_count);

    if (out_stats) {
        *out_stats = stats;
    }

    m_relocated = (stats.unresolved == 0);
    return true;
}

uint64_t ElfImage::find_symbol(const char* name) const {
    if (!m_symtab || !m_strtab || m_sym_count == 0) return 0;

    for (size_t i = 0; i < m_sym_count; i++) {
        const auto& sym = m_symtab[i];
        if (sym.st_name >= m_strsz) continue;
        const char* sym_name = m_strtab + sym.st_name;
        if (std::strcmp(sym_name, name) == 0) {
            if (sym.st_shndx != 0) {
                return m_load_bias + sym.st_value;
            }
            break;
        }
    }
    return 0;
}

std::vector<uint64_t> ElfImage::get_init_array() const {
    std::vector<uint64_t> constructors;
    if (m_init_array && m_init_array_count > 0) {
        constructors.reserve(m_init_array_count);
        for (size_t i = 0; i < m_init_array_count; i++) {
            constructors.push_back(m_init_array[i]);
        }
    }
    return constructors;
}

} // namespace lcs::cpu
