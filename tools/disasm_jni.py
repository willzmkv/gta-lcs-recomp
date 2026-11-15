# Simple ARM64 instruction decoder for JNI_OnLoad
import struct

with open('gtalcs/gamefiles/android-libs/libGame.so', 'rb') as f:
    elf = f.read()

st_value = 0x56ad5c
st_size = 344

insns = struct.unpack_from(f'<{st_size//4}I', elf, st_value)

for idx, op in enumerate(insns):
    addr = st_value + idx * 4
    # Check for BLR
    if (op & 0xfffffc1f) == 0xd63f0000:
        rn = (op >> 5) & 0x1f
        print(f'0x{addr:x} (+0x{idx*4:03x}): BLR X{rn}')
    # Check for BL
    elif (op >> 26) == 0x25:
        imm26 = op & 0x3ffffff
        if imm26 & 0x2000000:
            imm26 -= 0x4000000
        target = addr + imm26 * 4
        print(f'0x{addr:x} (+0x{idx*4:03x}): BL 0x{target:x}')
    # Check for LDR (register/immediate)
    elif (op & 0xffc00000) == 0xf9400000:
        imm12 = ((op >> 10) & 0xfff) * 8
        rn = (op >> 5) & 0x1f
        rt = op & 0x1f
        print(f'0x{addr:x} (+0x{idx*4:03x}): LDR X{rt}, [X{rn}, #0x{imm12:x}]')
    # Check for ADRP
    elif (op & 0x9f000000) == 0x90000000:
        immlo = (op >> 29) & 3
        immhi = (op >> 5) & 0x7ffff
        imm = ((immhi << 2) | immlo) << 12
        if imm & 0x100000000:
            imm -= 0x200000000
        target = (addr & ~0xfff) + imm
        rt = op & 0x1f
        print(f'0x{addr:x} (+0x{idx*4:03x}): ADRP X{rt}, 0x{target:x}')
    elif op == 0xd65f03c0:
        print(f'0x{addr:x} (+0x{idx*4:03x}): RET')
