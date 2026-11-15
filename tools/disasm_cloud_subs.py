import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

funcs = [0x7d8780, 0x7c8be0, 0x7c8b30, 0x7c8ca0, 0x7e1380, 0x7c8b50, 0x7dd9f0]

for fn in funcs:
    print(f'=== Function at 0x{fn:x} ===')
    for i in range(fn, fn + 64, 4):
        op, = struct.unpack_from('<I', elf, i)
        extra = ""
        if (op >> 26) == 0x25:
            imm26 = op & 0x3ffffff
            if imm26 & 0x2000000: imm26 -= 0x4000000
            dst = i + imm26 * 4
            extra = f'BL 0x{dst:x}'
        elif (op & 0xfffffc1f) == 0xd63f0000:
            extra = f'BLR X{(op >> 5) & 0x1f}'
        elif (op & 0xbfc00000) == 0xb9000000:
            imm = ((op >> 10) & 0xfff) * 4
            extra = f'STR W{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        elif (op & 0xffc00000) == 0xf9000000:
            imm = ((op >> 10) & 0xfff) * 8
            extra = f'STR X{op & 0x1f}, [X{(op >> 5) & 0x1f}, #0x{imm:x}]'
        elif op == 0xd65f03c0:
            extra = 'RET'
        print(f'  0x{i:x}: 0x{op:08x}  {extra}')
        if extra == 'RET': break
