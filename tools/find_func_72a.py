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

for i in range(len(sym_data) // 24):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', sym_data, i * 24)
    name_end = str_data.find(b'\0', st_name)
    name = str_data[st_name:name_end].decode('latin-1')
    if st_value <= 0x72ad1c < st_value + max(st_size, 1):
        print(f'0x72ad1c is inside {name} (0x{st_value:x}..0x{st_value+st_size:x})')
