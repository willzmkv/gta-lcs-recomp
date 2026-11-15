import struct

with open('gtalcs/gamefiles/android-libs/libopenal.so', 'rb') as f:
    elf = f.read()

for addr in range(0x130614, 0x13064c, 4):
    op, = struct.unpack_from('<I', elf, addr)
    print(f'0x{addr:06x}: 0x{op:08x}')
