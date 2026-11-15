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

dynsym = sections['.dynsym']; dynstr = sections['.dynstr']
sym_data = elf[dynsym[2]:dynsym[2] + dynsym[3]]; str_data = elf[dynstr[2]:dynstr[2] + dynstr[3]]

rela_plt = sections['.rela.plt']
rela_data = elf[rela_plt[2]:rela_plt[2] + rela_plt[3]]
plt_start = sections['.plt'][1]

plt_targets = [0x7d8780, 0x7c8be0, 0x7c8b30, 0x7c8ca0, 0x7e1380, 0x7c8b50, 0x7dd9f0]

for target in plt_targets:
    slot_idx = (target - (plt_start + 0x20)) // 0x10
    if 0 <= slot_idx < len(rela_data) // 24:
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', rela_data, slot_idx * 24)
        sym_idx = r_info >> 32
        st_name = struct.unpack_from('<I', sym_data, sym_idx * 24)[0]
        name_end = str_data.find(b'\0', st_name)
        sym_name = str_data[st_name:name_end].decode('latin-1')
        print(f'PLT 0x{target:x} (slot {slot_idx}) -> {sym_name}')
    else:
        print(f'PLT 0x{target:x} outside .plt')
