import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', elf, 0x18)
e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<IHHHHHH', elf, 0x30)
shstr_offset = struct.unpack_from('<QQ', elf, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
sections = {}
for i in range(e_shnum):
    sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from('<IIQQQQI', elf, e_shoff + i * e_shentsize)
    name_end = elf.find(b'\0', shstr_offset + sh_name_idx)
    sections[elf[shstr_offset + sh_name_idx:name_end].decode()] = (sh_type, sh_addr, sh_offset, sh_size, sh_link)

rela_plt = sections['.rela.plt']
rela_data = elf[rela_plt[2]:rela_plt[2] + rela_plt[3]]
plt_start = sections['.plt'][1]

sym_idx = 368 # _ZNKSt6__ndk16locale9use_facetERNS0_2idE
for j in range(len(rela_data) // 24):
    r_offset, r_info, r_addend = struct.unpack_from('<QQq', rela_data, j * 24)
    if (r_info >> 32) == sym_idx:
        plt_addr = plt_start + 0x20 + j * 0x10
        print(f'use_facet PLT at 0x{plt_addr:x}')
        for k in range(0, len(elf) - 4, 4):
            op, = struct.unpack_from('<I', elf, k)
            if (op >> 26) == 0x25:
                imm26 = op & 0x3ffffff
                if imm26 & 0x2000000: imm26 -= 0x4000000
                if k + imm26 * 4 == plt_addr:
                    print(f'  Call from 0x{k:x}')
