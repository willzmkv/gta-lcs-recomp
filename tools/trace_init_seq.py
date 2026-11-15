import struct

with open('gtalcs/lcs_runtime', 'rb') as f:
    elf = f.read()

# Read symtab to get function names
e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', elf, 0x18)
e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<IHHHHHH', elf, 0x30)
shstr_offset = struct.unpack_from('<QQ', elf, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
sections = {}
for i in range(e_shnum):
    sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from('<IIQQQQI', elf, e_shoff + i * e_shentsize)
    name_end = elf.find(b'\0', shstr_offset + sh_name_idx)
    sections[elf[shstr_offset + sh_name_idx:name_end].decode()] = (sh_type, sh_addr, sh_offset, sh_size, sh_link)
symtab = sections['.symtab']; strtab = sections['.strtab']
sym_data = elf[symtab[2]:symtab[2] + symtab[3]]; str_data = elf[strtab[2]:strtab[2] + strtab[3]]
sym_names = {}
for i in range(len(sym_data) // 24):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', sym_data, i * 24)
    name_end = str_data.find(b'\0', st_name)
    sym_names[st_value] = str_data[st_name:name_end].decode('latin-1')

def va_to_offset(va):
    for i in range(e_shnum):
        sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<IIQQQQ', elf, e_shoff + i * e_shentsize)
        if sh_addr <= va < sh_addr + sh_size: return sh_offset + (va - sh_addr)
    return None

def get_str(va):
    off = va_to_offset(va)
    if off is None: return ""
    end = elf.find(b'\0', off)
    if end == -1 or end - off > 120: return ""
    try:
        s = elf[off:end].decode('ascii')
        if all(32 <= ord(c) < 127 for c in s): return s
    except Exception: pass
    return ""

# Inspect instructions from 0xc200 to 0xc500
print('=== Disassembly of main init sequence (0xc200 to 0xc500) ===')
for i in range(0xc200, 0xc500, 4):
    op, = struct.unpack_from('<I', elf, i)
    extra = ""
    # BL
    if (op >> 26) == 0x25:
        imm26 = op & 0x3ffffff
        if imm26 & 0x2000000: imm26 -= 0x4000000
        dst = i + imm26 * 4
        sym = sym_names.get(dst, f'0x{dst:x}')
        extra = f'BL {sym}'
    # BLR
    elif (op & 0xfffffc1f) == 0xd63f0000:
        rn = (op >> 5) & 0x1f
        extra = f'BLR X{rn}'
    # ADRP
    elif (op & 0x9f000000) == 0x90000000:
        immlo = (op >> 29) & 3
        immhi = (op >> 5) & 0x7ffff
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000: imm -= 0x200000000
        dst = (i & ~0xfff) + imm
        rt = op & 0x1f
        extra = f'ADRP X{rt}, 0x{dst:x}'
    # LDR/STR immediate
    elif (op & 0xbfc00000) == 0xb9400000:
        imm = ((op >> 10) & 0xfff) * 4
        rn = (op >> 5) & 0x1f
        rt = op & 0x1f
        extra = f'LDR W{rt}, [X{rn}, #0x{imm:x}]'
    elif (op & 0xffc00000) == 0xf9400000:
        imm = ((op >> 10) & 0xfff) * 8
        rn = (op >> 5) & 0x1f
        rt = op & 0x1f
        extra = f'LDR X{rt}, [X{rn}, #0x{imm:x}]'
    elif (op & 0xff800000) == 0x91000000:
        imm = (op >> 10) & 0xfff
        rn = (op >> 5) & 0x1f
        rt = op & 0x1f
        extra = f'ADD X{rt}, X{rn}, #0x{imm:x}'
    elif (op & 0xff800000) == 0x52800000:
        imm = (op >> 5) & 0xffff
        rt = op & 0x1f
        extra = f'MOV W{rt}, #0x{imm:x}'

    if extra:
        print(f'0x{i:05x}: {op:08x}  {extra}')
