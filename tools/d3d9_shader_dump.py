"""Dump the D3D9 shaders (vs/ps 2.0, 2.x, 3.0 bytecode) embedded in a binary and disassemble them
(Windows d3dcompiler_47 D3DDisassemble). Reads the file only; nothing in it is executed.

usage: python tools/d3d9_shader_dump.py <binary> <out_dir>

Prints the pixel shaders that sample a texture 3+ times with frc (stochastic-texturing candidates).
"""
import struct, sys, ctypes, os, re
src = sys.argv[1]; out = sys.argv[2]
os.makedirs(out, exist_ok=True)
d = open(src, 'rb').read()
dc = ctypes.WinDLL('d3dcompiler_47.dll')
class Blob(ctypes.Structure): pass
dis = dc.D3DDisassemble
dis.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
found = []
for off in range(0, len(d) - 8, 4):
    v, = struct.unpack_from('<I', d, off)
    if v in (0xFFFF0200, 0xFFFF0201, 0xFFFF0300, 0xFFFE0200, 0xFFFE0201, 0xFFFE0300):
        end = d.find(b'\xff\xff\x00\x00', off + 4)
        while end != -1 and (end - off) % 4: end = d.find(b'\xff\xff\x00\x00', end + 1)
        if end == -1 or end - off > 65536: continue
        blob = d[off:end + 4]
        p = ctypes.c_void_p()
        if dis(blob, len(blob), 0, None, ctypes.byref(p)) != 0 or not p.value: continue
        vt = ctypes.cast(ctypes.cast(p, ctypes.POINTER(ctypes.c_void_p))[0], ctypes.POINTER(ctypes.c_void_p))
        getptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3]); getsz = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])
        txt = ctypes.string_at(getptr(p), getsz(p)).decode('latin-1')
        n = len(found); open(os.path.join(out, f'sh_{n:03d}.txt'), 'w').write(txt)
        texld = len(re.findall(r'^\s*texld', txt, re.M)); frc = len(re.findall(r'^\s*frc', txt, re.M))
        found.append((n, off, len(blob), 'ps' if v >> 16 == 0xFFFF else 'vs', texld, frc, '43758' in txt or 'sincos' in txt))
print(len(found), 'shaders')
for f in found:
    if f[3] == 'ps' and f[4] >= 3 and f[5] >= 1: print('candidate', f)
