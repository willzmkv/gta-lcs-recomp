import struct

with open('gtalcs/lcs_runtime', 'rb') as f:
    elf = f.read()

offsets = [
    0x25748, 0x257b8, 0x257d0, 0x257e0, 0x25828, 0x25860,
    0x27258, 0x25898, 0x258b0, 0x258b8, 0x258c8, 0x258f8,
    0x25910, 0x25990, 0x259a8
]

for off in offsets:
    end = elf.find(b'\0', off)
    s = elf[off:end].decode('latin-1', errors='ignore')
    print(f'0x{off:x}: "{s}"')
