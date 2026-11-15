import struct

with open('gtalcs/gamefiles/android-libs/libopenal.so', 'rb') as f:
    elf = f.read()

def disasm_range(start, count):
    print(f'=== Disassembling from 0x{start:x} ({count} instructions): ===')
    for i in range(count):
        addr = start + i * 4
        op, = struct.unpack_from('<I', elf, addr)
        extra = ""
        if (op >> 26) == 0x25:
            imm26 = op & 0x3ffffff
            if imm26 & 0x2000000: imm26 -= 0x4000000
            dst = addr + imm26 * 4
            extra = f'BL 0x{dst:x}'
        elif (op & 0xfffffc1f) == 0xd63f0000:
            extra = f'BLR X{(op >> 5) & 0x1f}'
        elif (op & 0x9f000000) == 0x90000000:
            immlo = (op >> 29) & 3
            immhi = (op >> 5) & 0x7ffff
            imm = ((immhi << 2) | immlo) << 12
            if imm & 0x100000000: imm -= 0x200000000
            dst = (addr & ~0xfff) + imm
            extra = f'ADRP X{op & 0x1f}, 0x{dst:x}'
        elif (op & 0xffc00000) == 0xf9400000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'LDR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        elif (op & 0xffc00000) == 0xf9000000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'STR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        print(f'0x{addr:06x}: 0x{op:08x}  {extra}')

disasm_range(0x164600, 30)
