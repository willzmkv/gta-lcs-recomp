import struct

with open('gtalcs/gamefiles/android-libs/libopenal.so', 'rb') as f:
    elf = f.read()

# Let's find what function called 0x130614 in libopenal.so
# 0x130614 is locale::id::__init()
# Who calls locale::id::__init()?
# Let's search for BL to 0x130614
print('Searching for BL to 0x130614 or 0x130610:')
for i in range(0, len(elf) - 4, 4):
    op, = struct.unpack_from('<I', elf, i)
    if (op >> 26) == 0x25:
        imm26 = op & 0x3ffffff
        if imm26 & 0x2000000: imm26 -= 0x4000000
        dst = i + imm26 * 4
        if dst in [0x130614, 0x130610]:
            print(f'  BL at 0x{i:x}')

# And who references pointer 0x130614?
target_ptr = 0x7f7a130614
target_rel = 0x130614
for i in range(0, len(elf) - 8, 8):
    val, = struct.unpack_from('<Q', elf, i)
    if val in [target_ptr, target_rel]:
        print(f'  Pointer at 0x{i:x}: 0x{val:x}')
