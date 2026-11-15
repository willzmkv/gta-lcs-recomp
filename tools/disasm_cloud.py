import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

# Read section headers
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

target_addr = None
target_size = None
for i in range(len(sym_data) // 24):
    st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', sym_data, i * 24)
    name_end = str_data.find(b'\0', st_name)
    name = str_data[st_name:name_end].decode('latin-1')
    if name == 'Java_com_rockstargames_gtalcs_GTAJNIlib_initCloud':
        target_addr = st_value
        target_size = st_size
        print(f'{name} at 0x{st_value:x} (size {st_size})')
        break

if target_addr:
    for i in range(target_addr, target_addr + target_size, 4):
        op, = struct.unpack_from('<I', elf, i)
        extra = ""
        if (op >> 26) == 0x25:
            imm26 = op & 0x3ffffff
            if imm26 & 0x2000000: imm26 -= 0x4000000
            dst = i + imm26 * 4
            extra = f'BL 0x{dst:x}'
        elif (op & 0xfffffc1f) == 0xd63f0000:
            extra = f'BLR X{(op >> 5) & 0x1f}'
        elif (op & 0x9f000000) == 0x90000000:
            extra = 'ADRP'
        elif (op & 0xffc00000) == 0xf9400000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'LDR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        elif (op & 0xffc00000) == 0xf9000000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'STR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        elif op == 0xd65f03c0:
            extra = 'RET'
        if extra:
            print(f'0x{i:06x}: {op:08x}  {extra}')
