import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

def disasm(start, count):
    print(f'=== Disassembly at 0x{start:x} ===')
    for i in range(start - 24, start + count, 4):
        op, = struct.unpack_from('<I', elf, i)
        extra = ""
        if (op >> 26) == 0x25:
            imm26 = op & 0x3ffffff
            if imm26 & 0x2000000: imm26 -= 0x4000000
            dst = i + imm26 * 4
            extra = f'BL 0x{dst:x}'
        elif (op & 0x9f000000) == 0x90000000:
            immlo = (op >> 29) & 3
            immhi = (op >> 5) & 0x7ffff
            imm = ((immhi << 2) | immlo) << 12
            if imm & 0x100000000: imm -= 0x200000000
            dst = (i & ~0xfff) + imm
            extra = f'ADRP X{op & 0x1f}, 0x{dst:x}'
        elif (op & 0xffc00000) == 0xf9400000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'LDR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        print(f'{"=> " if i == start else "   "}0x{i:06x}: 0x{op:08x}  {extra}')

disasm(0x5d041c, 24)
disasm(0x5f8a6c, 24)
