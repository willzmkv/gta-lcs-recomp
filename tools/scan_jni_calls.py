import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

# Let's search for patterns:
# LDR X8, [X0]
# LDR X8, [X8, #offset]
# BLR X8

# Or more generally, LDR Xt, [Xn, #imm] followed by BLR Xt
calls = {}
for i in range(0, len(elf) - 8, 4):
    op1, op2 = struct.unpack_from('<II', elf, i)
    # check if op2 is BLR
    if (op2 & 0xfffffc1f) == 0xd63f0000:
        rn2 = (op2 >> 5) & 0x1f
        # check if op1 is LDR Xt, [Xn, #imm] where Xt == rn2
        if (op1 & 0xffc00000) == 0xf9400000:
            rt1 = op1 & 0x1f
            if rt1 == rn2:
                imm = ((op1 >> 10) & 0xfff) * 8
                slot = imm // 8
                calls[slot] = calls.get(slot, 0) + 1

print('JNI slots called via (LDR X8, [Xn, #imm]; BLR X8):')
for slot in sorted(calls.keys()):
    count = calls[slot]
    print(f'  Slot {slot:3d} (+0x{slot*8:03x}): {count} calls')
