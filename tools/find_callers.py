import struct

def find_calls(filename, target_addr):
    with open(filename, 'rb') as f:
        elf = f.read()
    print(f'Calls to 0x{target_addr:x} in {filename}:')
    for i in range(0, len(elf) - 4, 4):
        op, = struct.unpack_from('<I', elf, i)
        if (op >> 26) == 0x25:
            imm26 = op & 0x3ffffff
            if imm26 & 0x2000000: imm26 -= 0x4000000
            dst = i + imm26 * 4
            if dst == target_addr:
                print(f'  BL at 0x{i:x}')

find_calls('gtalcs/gamefiles/android-libs/libopenal.so', 0x130614)
find_calls('gtalcs/gamefiles/android-libs/libopenal.so', 0x130610)
