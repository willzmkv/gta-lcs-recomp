#pragma once

#include <cstdint>
#include <cstddef>

namespace lcs::cpu {

// Standard ELF64 types
using Elf64_Addr   = uint64_t;
using Elf64_Off    = uint64_t;
using Elf64_Half   = uint16_t;
using Elf64_Word   = uint32_t;
using Elf64_Sword  = int32_t;
using Elf64_Xword  = uint64_t;
using Elf64_Sxword = int64_t;

// ELF magic bytes
constexpr uint8_t ELFMAG0 = 0x7f;
constexpr uint8_t ELFMAG1 = 'E';
constexpr uint8_t ELFMAG2 = 'L';
constexpr uint8_t ELFMAG3 = 'F';

// ELF header
struct Elf64_Ehdr {
    uint8_t    e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    Elf64_Off  e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
};

// Machine architecture
constexpr Elf64_Half EM_AARCH64 = 183;

// Segment types (p_type)
constexpr Elf64_Word PT_NULL    = 0;
constexpr Elf64_Word PT_LOAD    = 1;
constexpr Elf64_Word PT_DYNAMIC = 2;
constexpr Elf64_Word PT_INTERP  = 3;
constexpr Elf64_Word PT_NOTE    = 4;
constexpr Elf64_Word PT_SHLIB   = 5;
constexpr Elf64_Word PT_PHDR    = 6;
constexpr Elf64_Word PT_TLS     = 7;
constexpr Elf64_Word PT_GNU_EH_FRAME = 0x6474e550;
constexpr Elf64_Word PT_GNU_STACK    = 0x6474e551;
constexpr Elf64_Word PT_GNU_RELRO    = 0x6474e552;

// Segment flags (p_flags)
constexpr Elf64_Word PF_X = 0x1;
constexpr Elf64_Word PF_W = 0x2;
constexpr Elf64_Word PF_R = 0x4;

// Program header
struct Elf64_Phdr {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
};

// Dynamic array tags (d_tag)
constexpr Elf64_Sxword DT_NULL         = 0;
constexpr Elf64_Sxword DT_NEEDED       = 1;
constexpr Elf64_Sxword DT_PLTRELSZ     = 2;
constexpr Elf64_Sxword DT_PLTGOT       = 3;
constexpr Elf64_Sxword DT_HASH         = 4;
constexpr Elf64_Sxword DT_STRTAB       = 5;
constexpr Elf64_Sxword DT_SYMTAB       = 6;
constexpr Elf64_Sxword DT_RELA         = 7;
constexpr Elf64_Sxword DT_RELASZ       = 8;
constexpr Elf64_Sxword DT_RELAENT      = 9;
constexpr Elf64_Sxword DT_STRSZ        = 10;
constexpr Elf64_Sxword DT_SYMENT       = 11;
constexpr Elf64_Sxword DT_INIT         = 12;
constexpr Elf64_Sxword DT_FINI         = 13;
constexpr Elf64_Sxword DT_SONAME       = 14;
constexpr Elf64_Sxword DT_RPATH        = 15;
constexpr Elf64_Sxword DT_SYMBOLIC     = 16;
constexpr Elf64_Sxword DT_REL          = 17;
constexpr Elf64_Sxword DT_RELSZ        = 18;
constexpr Elf64_Sxword DT_RELENT       = 19;
constexpr Elf64_Sxword DT_PLTREL       = 20;
constexpr Elf64_Sxword DT_DEBUG        = 21;
constexpr Elf64_Sxword DT_TEXTREL      = 22;
constexpr Elf64_Sxword DT_JMPREL       = 23;
constexpr Elf64_Sxword DT_BIND_NOW     = 24;
constexpr Elf64_Sxword DT_INIT_ARRAY   = 25;
constexpr Elf64_Sxword DT_FINI_ARRAY   = 26;
constexpr Elf64_Sxword DT_INIT_ARRAYSZ = 27;
constexpr Elf64_Sxword DT_FINI_ARRAYSZ = 28;
constexpr Elf64_Sxword DT_GNU_HASH     = 0x6ffffef5;

struct Elf64_Dyn {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr  d_ptr;
    } d_un;
};

// Symbol table entry
struct Elf64_Sym {
    Elf64_Word    st_name;
    uint8_t       st_info;
    uint8_t       st_other;
    Elf64_Half    st_shndx;
    Elf64_Addr    st_value;
    Elf64_Xword   st_size;
};

#define ELF64_ST_BIND(val) (((unsigned char) (val)) >> 4)
#define ELF64_ST_TYPE(val) ((val) & 0xf)

constexpr uint8_t STB_LOCAL  = 0;
constexpr uint8_t STB_GLOBAL = 1;
constexpr uint8_t STB_WEAK   = 2;

constexpr uint8_t STT_NOTYPE = 0;
constexpr uint8_t STT_OBJECT = 1;
constexpr uint8_t STT_FUNC   = 2;

// Relocations with addend
struct Elf64_Rela {
    Elf64_Addr   r_offset;
    Elf64_Xword  r_info;
    Elf64_Sxword r_addend;
};

#define ELF64_R_SYM(i)  ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffffL)

// AArch64 Relocation Types
constexpr uint32_t R_AARCH64_NONE      = 0;
constexpr uint32_t R_AARCH64_ABS64     = 257;
constexpr uint32_t R_AARCH64_COPY      = 1024;
constexpr uint32_t R_AARCH64_GLOB_DAT  = 1025;
constexpr uint32_t R_AARCH64_JUMP_SLOT = 1026;
constexpr uint32_t R_AARCH64_RELATIVE  = 1027;

} // namespace lcs::cpu
