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

target_got = 0x7ff278

for sec_name in ['.rela.dyn', '.rela.plt']:
    rela = sections[sec_name]
    rela_data = elf[rela[2]:rela[2] + rela[3]]
    for j in range(len(rela_data) // 24):
        r_offset, r_info, r_addend = struct.unpack_from('<QQq', rela_data, j * 24)
        if r_offset == target_got:
            sym_idx = r_info >> 32
            st_name = struct.unpack_from('<I', sym_data, sym_idx * 24)[0]
            name_end = str_data.find(b'\0', st_name)
            sym_name = str_data[st_name:name_end].decode('latin-1')
            print(f'GOT 0x{target_got:x} relocated by {sec_name} to symbol {sym_idx}: {sym_name} (type={r_info & 0xffffffff})')
