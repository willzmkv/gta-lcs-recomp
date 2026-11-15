import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

# Let's search for calls to PLT or relocations for _ZNSt6__ndk16locale2id6__initEv
e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', elf, 0x18)
e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<IHHHHHH', elf, 0x30)
shstr_offset = struct.unpack_from('<QQ', elf, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]
sections = {}
for i in range(e_shnum):
    sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link = struct.unpack_from('<IIQQQQI', elf, e_shoff + i * e_shentsize)
    name_end = elf.find(b'\0', shstr_offset + sh_name_idx)
    sections[elf[shstr_offset + sh_name_idx:name_end].decode()] = (sh_type, sh_addr, sh_offset, sh_size, sh_link)

dynsym = sections['.dynsym']; dynstr = sections['.dynstr']
sym_data = elf[dynsym[2]:dynsym[2] + dynsym[3]]; str_data = elf[dynstr[2]:dynstr[2] + dynstr[3]]

sym_idx = None
for i in range(len(sym_data) // 24):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', sym_data, i * 24)
    name_end = str_data.find(b'\0', st_name)
    name = str_data[st_name:name_end].decode('latin-1')
    if name == '_ZNSt6__ndk16locale2id6__initEv':
        sym_idx = i
        print(f'Symbol index for _ZNSt6__ndk16locale2id6__initEv: {i}')
        break

if sym_idx is not None:
    # check .rela.plt
    rela = sections['.rela.plt']
    rela_data = elf[rela[2]:rela[2] + rela[3]]
    for j in range(len(rela_data) // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', rela_data, j * 24)
        r_sym = r_info >> 32
        if r_sym == sym_idx:
            print(f'Relocation in .rela.plt: r_offset=0x{r_offset:x}, slot {j}')
            # PLT stub address in AArch64:
            # .plt starts at section .plt addr + 0x20 + j * 0x10
            plt_addr = sections['.plt'][1] + 0x20 + j * 0x10
            print(f'PLT stub at 0x{plt_addr:x}')
            # Find calls to plt_addr
            for k in range(0, len(elf) - 4, 4):
                op, = struct.unpack_from('<I', elf, k)
                if (op >> 26) == 0x25:
                    imm26 = op & 0x3ffffff
                    if imm26 & 0x2000000: imm26 -= 0x4000000
                    dst = k + imm26 * 4
                    if dst == plt_addr:
                        print(f'  Call to PLT from 0x{k:x}')
