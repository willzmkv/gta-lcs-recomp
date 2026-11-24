#!/usr/bin/env python3
"""
Extract and polish the community PT-BR GXT translation from
Tradução Definitiva GTA LCS Android\Android\patch.15.com.rockstargames.gtalcs.obb
into assets/lang/portuguese.gxt without modifying any original game files.
Requires only the Python standard library.
"""
import argparse
import os
import re
import struct
import sys

OBB_REL_PATH = os.path.join(
    "Tradução Definitiva GTA LCS Android",
    "Tradução Definitiva GTA LCS Android",
    "Android",
    "patch.15.com.rockstargames.gtalcs.obb",
)
PT_GXT_OFFSET = 0x14A306
PT_GXT_SIZE = 593464
DAWL_KEY = 0x66AF66AF

# Rockstar LCS Latin-1 special codepoints (matches native font1.png/font2.png & lcs_trainer)
UTF_TO_ROCKSTAR = {
    'À': 0x80, 'Á': 0x81, 'Â': 0x82, 'Ã': 0x83, 'Ä': 0x84, 'Å': 0x85,
    'Æ': 0x86, 'Ç': 0x87, 'È': 0x88, 'É': 0x89, 'Ê': 0x8A, 'Ë': 0x8B,
    'Ì': 0x8C, 'Í': 0x8D, 'Î': 0x8E, 'Ï': 0x8F, 'Ð': 0x90, 'Ñ': 0x91,
    'Ò': 0x92, 'Ó': 0x93, 'Ô': 0x94, 'Õ': 0x95, 'Ö': 0x96, '×': 0x97,
    'Ø': 0x98, 'Ù': 0x99, 'Ú': 0x9A, 'Û': 0x9B, 'Ü': 0x9C, 'Ý': 0x9D,
    'Þ': 0x9E, 'ß': 0x9F, 'à': 0xA0, 'á': 0xA1, 'â': 0xA2, 'ã': 0xA3,
    'ä': 0xA4, 'å': 0xA5, 'æ': 0xA6, 'ç': 0xA7, 'è': 0xA8, 'é': 0xA9,
    'ê': 0xAA, 'ë': 0xAB, 'ì': 0xAC, 'í': 0xAD, 'î': 0xAE, 'ï': 0xAF,
    'ð': 0xB0, 'ñ': 0xB1, 'ò': 0xB2, 'ó': 0xB3, 'ô': 0xB4, 'õ': 0xB5,
    'ö': 0xB6, '÷': 0xB7, 'ø': 0xB8, 'ù': 0xB9, 'ú': 0xBA, 'û': 0xBB,
    'ü': 0xBC, 'ý': 0xBD, 'þ': 0xBE, 'ÿ': 0xBF,
    '¡': 0xD7, '¿': 0xD8, '™': 0xDA,
}


def encode_rockstar_utf16(text: str) -> bytes:
    codes = [UTF_TO_ROCKSTAR.get(ch, ord(ch)) for ch in text]
    codes.append(0)
    return struct.pack("<" + "H" * len(codes), *codes)


def decrypt_obb_gxt(obb_path: str) -> bytes:
    with open(obb_path, "rb") as f:
        f.seek(PT_GXT_OFFSET)
        enc = f.read(PT_GXT_SIZE)
    if len(enc) != PT_GXT_SIZE:
        raise RuntimeError(f"Expected {PT_GXT_SIZE} bytes at 0x{PT_GXT_OFFSET:x}, got {len(enc)}")
    nwords = len(enc) // 4
    out = bytearray()
    for w in struct.unpack_from(f"<{nwords}I", enc, 0):
        out.extend(struct.pack("<I", w ^ DAWL_KEY))
    for i, b in enumerate(enc[nwords * 4 :]):
        out.append(b ^ ((DAWL_KEY >> ((i % 4) * 8)) & 0xFF))
    if out[:4] != b"TABL":
        raise RuntimeError(f"Decrypted header mismatch: {out[:4]!r}")
    return bytes(out)


def parse_gxt(data: bytes):
    tabl_size = struct.unpack_from("<I", data, 4)[0]
    tables = []
    for i in range(tabl_size // 12):
        raw_name, toff = struct.unpack_from("<8sI", data, 8 + i * 12)
        tname = raw_name.rstrip(b"\x00").decode("latin1")
        hdr = toff if i == 0 else toff + 8
        assert data[hdr : hdr + 4] == b"TKEY", f"Missing TKEY at {tname}"
        tkey_sz = struct.unpack_from("<I", data, hdr + 4)[0]
        base = hdr + 8 + tkey_sz + 8
        entries = []
        for k in range(tkey_sz // 12):
            off, raw_key = struct.unpack_from("<I8s", data, hdr + 8 + k * 12)
            kname = raw_key.rstrip(b"\x00").decode("latin1")
            end = data.find(b"\x00\x00", base + off)
            if (end - (base + off)) % 2:
                end += 1
            raw_u16 = struct.unpack_from("<" + "H" * ((end - (base + off)) // 2), data, base + off)
            entries.append((kname, raw_u16))
        tables.append((tname, entries))
    return tables


# Curated fixes for PC/Controller compatibility & untranslated/typo lines
OVERRIDES = {
    "FEQ_SRW": "~w~Tem certeza que deseja iniciar um Novo Jogo? Todo o progresso não salvo será perdido.",
    "FET_LAN": "Idioma",
    "FET_ENG": "Português (Brasil)",
    "FEO_LAN": "Idioma",
    "FEO_ENG": "Português (Brasil)",
}


def build_gxt(tables) -> bytes:
    # Rebuild valid GTA LCS UTF-16LE GXT file preserving table/key ordering
    tabl_entries = []
    body = bytearray()
    tabl_header_size = 8 + len(tables) * 12

    for i, (tname, entries) in enumerate(tables):
        toff = tabl_header_size + len(body)
        tabl_entries.append((tname, toff))
        if i != 0:
            body.extend(tname.encode("latin1")[:8].ljust(8, b"\x00"))
        tkey_buf = bytearray()
        tdat_buf = bytearray()
        for kname, raw_u16 in entries:
            offset = len(tdat_buf)
            tkey_buf.extend(struct.pack("<I8s", offset, kname.encode("latin1")[:8].ljust(8, b"\x00")))
            if kname in OVERRIDES:
                tdat_buf.extend(encode_rockstar_utf16(OVERRIDES[kname]))
            else:
                tdat_buf.extend(struct.pack("<" + "H" * len(raw_u16), *raw_u16) + b"\x00\x00")
        body.extend(b"TKEY" + struct.pack("<I", len(tkey_buf)) + tkey_buf)
        body.extend(b"TDAT" + struct.pack("<I", len(tdat_buf)) + tdat_buf)

    header = bytearray(b"TABL" + struct.pack("<I", len(tables) * 12))
    for tname, toff in tabl_entries:
        header.extend(struct.pack("<8sI", tname.encode("latin1")[:8].ljust(8, b"\x00"), toff))
    return bytes(header) + bytes(body)


def main() -> int:
    parser = argparse.ArgumentParser(description="Extract PT-BR GXT from community OBB patch.")
    parser.add_argument("--root", default=".", help="Workspace root directory")
    parser.add_argument("--out", default="assets/lang/portuguese.gxt", help="Output GXT path")
    args = parser.parse_args()

    obb_path = os.path.join(args.root, OBB_REL_PATH)
    out_path = os.path.join(args.root, args.out)
    if not os.path.isfile(obb_path):
        print(f"[error] Community OBB not found: {obb_path}", file=sys.stderr)
        return 1

    raw_gxt = decrypt_obb_gxt(obb_path)
    tables = parse_gxt(raw_gxt)
    rebuilt = build_gxt(tables)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(rebuilt)
    print(f"[pt-br] Extracted & verified {len(tables)} GXT tables -> {out_path} ({len(rebuilt)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
