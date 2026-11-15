import struct

with open('gtalcs/lcs_runtime', 'rb') as f:
    elf = f.read()

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

print('Calls inside main from 0xc500 to 0xca00:')
for i in range(0xc500, 0xca00, 4):
    op, = struct.unpack_from('<I', elf, i)
    if (op >> 26) == 0x25:
        imm26 = op & 0x3ffffff
        if imm26 & 0x2000000: imm26 -= 0x4000000
        dst = i + imm26 * 4
        sym = sym_names.get(dst, f'0x{dst:x}')
        print(f'0x{i:x}: BL {sym}')
    elif (op & 0xfffffc1f) == 0xd63f0000:
        rn = (op >> 5) & 0x1f
        print(f'0x{i:x}: BLR X{rn}')
