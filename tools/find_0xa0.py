import struct

def search_0xa0(filename):
    with open(filename, 'rb') as f:
        elf = f.read()
    print(f'Instructions with offset 0xa0 in {filename}:')
    for i in range(0, len(elf) - 4, 4):
        op, = struct.unpack_from('<I', elf, i)
        # STR 64-bit with imm12 = 0xa0 / 8 = 20 (0x14)
        if (op & 0xffc00000) == 0xf9000000:
            imm = ((op >> 10) & 0xfff) * 8
            if imm == 0xa0:
                rn = (op >> 5) & 0x1f
                rt = op & 0x1f
                print(f'  0x{i:x}: STR X{rt}, [X{rn}, #0xa0]')
        # STR 32-bit with imm12 = 0xa0 / 4 = 40 (0x28)
        elif (op & 0xbfc00000) == 0xb9000000:
            imm = ((op >> 10) & 0xfff) * 4
            if imm == 0xa0:
                rn = (op >> 5) & 0x1f
                rt = op & 0x1f
                print(f'  0x{i:x}: STR W{rt}, [X{rn}, #0xa0]')

search_0xa0('gtalcs/gamefiles/android-libs/libGame.so')
