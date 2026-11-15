import struct

with open('gtalcs/lcs_runtime', 'rb') as f:
    elf = f.read()

e_entry, e_phoff, e_shoff = struct.unpack_from('<QQQ', elf, 0x18)
e_flags, e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<IHHHHHH', elf, 0x30)
shstr_offset = struct.unpack_from('<QQ', elf, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

def va_to_offset(va):
    for i in range(e_shnum):
        sh_name_idx, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<IIQQQQ', elf, e_shoff + i * e_shentsize)
        if sh_addr <= va < sh_addr + sh_size:
            return sh_offset + (va - sh_addr)
    return None

def get_str(va):
    off = va_to_offset(va)
    if off is None: return ""
    end = elf.find(b'\0', off)
    if end == -1: return ""
    try:
        return elf[off:end].decode('utf-8')
    except Exception:
        return ""

class_names = [
    "com/rockstargames/gtalcs/CommonAPI",
    "com/rockstargames/gtalcs/RockstarJNIlib",
    "com/rockstargames/hal/andHttp",
    "com/rockstargames/hal/andFile",
    "com/rockstargames/hal/andView"
]

methods_off = va_to_offset(0x00051448)
print('idx | class          | static | ret  | name                       | signature')
print('----+----------------+--------+------+----------------------------+----------')
for i in range(66):
    moff = methods_off + i * 64
    magic, kind, next_ptr, full_id, ret_type, name_va, sig_va, flags = struct.unpack_from('<QQQQQQQQ', elf, moff)
    class_id = (full_id >> 32) & 0xffffffff
    method_id = full_id & 0xffffffff
    cname = class_names[class_id] if class_id < len(class_names) else f"Class{class_id}"
    mname = get_str(name_va)
    msig = get_str(sig_va)
    is_static = (flags & 1) != 0
    ret_map = {0: 'void', 1: 'bool', 2: 'int', 3: 'long', 4: 'float', 5: 'obj'}
    ret_str = ret_map.get(ret_type, str(ret_type))
    print(f'{i:2d}  | {cname.split("/")[-1]:14s} | {str(is_static):6s} | {ret_str:4s} | {mname:26s} | {msig}')
