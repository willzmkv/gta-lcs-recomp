import struct

with open('gtalcs/lcs_runtime', 'rb') as f:
    elf = f.read()

target = 0x0001e884

for i in range(0, len(elf) - 4, 4):
    op, = struct.unpack_from('<I', elf, i)
    # BL
    if (op >> 26) == 0x25:
        imm26 = op & 0x3ffffff
        if imm26 & 0x2000000:
            imm26 -= 0x4000000
        dst = i + imm26 * 4
        if dst == target:
            print(f'BL to lcs_game_api_bind at 0x{i:x}')
