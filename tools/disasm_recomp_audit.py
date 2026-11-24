"""
ARM64 ELF64 Disassembly & Xref Audit Tool for GTA: Liberty City Stories (libGame.so).

Disassembles and inspects the exact guest implementations required by Phase 1:
  - _ZN6CTimer6UpdateEv & _ZN5cTime7RefreshEv
  - Xrefs to _ZN12CMenuManager19m_PrefsFrameLimiterE and usleep PLT callers
  - _ZN4CPad6UpdateEv, _ZN4CPad10UpdatePadsEv, Java_..._onJoyAxis, Java_..._setJoyAxis
  - _ZN12CMenuManager33InitialiseChangedLanguageSettingsEv & _ZN5CText4LoadEv
"""
from __future__ import annotations

import os
import struct
import sys

try:
    import capstone
except ImportError:
    capstone = None


def parse_elf(path: str):
    with open(path, "rb") as f:
        elf = f.read()

    e_shoff = struct.unpack_from("<Q", elf, 0x28)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", elf, 0x3A)
    shstr_off = struct.unpack_from("<Q", elf, e_shoff + e_shstrndx * e_shentsize + 0x18)[0]

    sections = {}
    for i in range(e_shnum):
        sh_name_i, sh_type, sh_flags, sh_addr, sh_off, sh_size, sh_link, sh_info, sh_align, sh_entsize = struct.unpack_from(
            "<IIQQQQIIQQ", elf, e_shoff + i * e_shentsize
        )
        end = elf.find(b"\0", shstr_off + sh_name_i)
        name = elf[shstr_off + sh_name_i : end].decode("latin-1")
        sections[name] = {
            "type": sh_type,
            "addr": sh_addr,
            "off": sh_off,
            "size": sh_size,
            "link": sh_link,
            "entsize": sh_entsize,
        }

    addr_to_sym: dict[int, list[str]] = {}
    sym_to_info: dict[str, tuple[int, int]] = {}
    for sname, strname in ((".dynsym", ".dynstr"), (".symtab", ".strtab")):
        if sname not in sections or strname not in sections:
            continue
        sec = sections[sname]
        strsec = sections[strname]
        sdata = elf[sec["off"] : sec["off"] + sec["size"]]
        stdata = elf[strsec["off"] : strsec["off"] + strsec["size"]]
        for i in range(0, len(sdata), 24):
            st_name, st_info, _st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", sdata, i)
            end = stdata.find(b"\0", st_name)
            name = stdata[st_name:end].decode("latin-1")
            if name and st_value:
                addr_to_sym.setdefault(st_value, []).append(name)
                if name not in sym_to_info or (sym_to_info[name][1] == 0 and st_size > 0):
                    sym_to_info[name] = (st_value, st_size)

    # Map PLT stubs to imported symbol names via .rela.plt
    plt_map: dict[int, str] = {}
    if ".rela.plt" in sections and ".plt" in sections:
        rela = sections[".rela.plt"]
        plt = sections[".plt"]
        dynsym = sections[".dynsym"]
        dynstr = sections[".dynstr"]
        sdata = elf[dynsym["off"] : dynsym["off"] + dynsym["size"]]
        stdata = elf[dynstr["off"] : dynstr["off"] + dynstr["size"]]
        rdata = elf[rela["off"] : rela["off"] + rela["size"]]
        got_to_sym: dict[int, str] = {}
        for i in range(0, len(rdata), 24):
            r_offset, r_info, _r_addend = struct.unpack_from("<QQq", rdata, i)
            sym_idx = r_info >> 32
            if sym_idx * 24 + 24 <= len(sdata):
                st_name = struct.unpack_from("<I", sdata, sym_idx * 24)[0]
                end = stdata.find(b"\0", st_name)
                got_to_sym[r_offset] = stdata[st_name:end].decode("latin-1")
        # Each AArch64 PLT stub (after header) has adrp x16, page; ldr x17, [x16, #off]
        plt_off = plt["off"]
        plt_addr = plt["addr"]
        plt_end = plt_off + plt["size"]
        for off in range(plt_off + 0x20, plt_end - 8, 16):
            va = plt_addr + (off - plt_off)
            w0, w1 = struct.unpack_from("<II", elf, off)
            if (w0 & 0x9F00001F) == 0x90000010 and (w1 & 0xFFC003FF) == 0xF9400211:
                immlo = (w0 >> 29) & 3
                immhi = (w0 >> 5) & 0x7FFFF
                imm = ((immhi << 2) | immlo) << 12
                if imm & 0x100000000:
                    imm -= 0x200000000
                page = (va & ~0xFFF) + imm
                lo12 = ((w1 >> 10) & 0xFFF) * 8
                got = page + lo12
                if got in got_to_sym:
                    plt_map[va] = got_to_sym[got] + "@plt"

    # Build .got map from .rela.dyn (GLOB_DAT)
    got_globals: dict[int, str] = {}
    if ".rela.dyn" in sections:
        rela = sections[".rela.dyn"]
        dynsym = sections[".dynsym"]
        dynstr = sections[".dynstr"]
        sdata = elf[dynsym["off"] : dynsym["off"] + dynsym["size"]]
        stdata = elf[dynstr["off"] : dynstr["off"] + dynstr["size"]]
        rdata = elf[rela["off"] : rela["off"] + rela["size"]]
        for i in range(0, len(rdata), 24):
            r_offset, r_info, _r_addend = struct.unpack_from("<QQq", rdata, i)
            rtype = r_info & 0xFFFFFFFF
            sym_idx = r_info >> 32
            if rtype == 1025 and sym_idx * 24 + 24 <= len(sdata):  # R_AARCH64_GLOB_DAT
                st_name = struct.unpack_from("<I", sdata, sym_idx * 24)[0]
                end = stdata.find(b"\0", st_name)
                got_globals[r_offset] = stdata[st_name:end].decode("latin-1")

    return elf, sections, addr_to_sym, sym_to_info, plt_map, got_globals


def disasm_func(elf, addr_to_sym, sym_to_info, plt_map, got_globals, name: str, max_bytes: int = 0x500):
    if name not in sym_to_info:
        print(f"MISSING SYMBOL: {name}")
        return
    start, size = sym_to_info[name]
    span = min(size if size > 0 else max_bytes, max_bytes)
    print(f"\n================ {name} @ 0x{start:x} (size=0x{size:x}) ================")
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    code = elf[start : start + span]
    adrp_pages: dict[str, int] = {}

    for insn in md.disasm(code, start):
        note = ""
        op_s = insn.op_str
        mnem = insn.mnemonic

        if mnem == "adrp":
            parts = op_s.split(",")
            reg = parts[0].strip()
            imm = int(parts[1].strip().lstrip("#"), 0)
            adrp_pages[reg] = imm
            if imm in addr_to_sym:
                note += f" ; page:{addr_to_sym[imm][0]}"
        elif mnem in ("add", "ldr", "str", "ldrb", "strb", "ldrh", "strh", "ldrsw"):
            # Track simple [reg, #imm] or reg, reg, #imm page+off resolutions
            if "[" in op_s:
                inside = op_s.split("[", 1)[1].split("]", 1)[0]
                sub = [x.strip() for x in inside.split(",")]
                base_reg = sub[0]
                if base_reg in adrp_pages and len(sub) == 2 and sub[1].startswith("#"):
                    try:
                        off = int(sub[1].lstrip("#"), 0)
                        target = adrp_pages[base_reg] + off
                        if target in got_globals:
                            note += f" ; GOT:{got_globals[target]} (0x{target:x})"
                        elif target in addr_to_sym:
                            note += f" ; &{addr_to_sym[target][0]} (0x{target:x})"
                        else:
                            note += f" ; [0x{target:x}]"
                            if mnem == "ldr" and target + 4 <= len(elf):
                                fval = struct.unpack_from("<f", elf, target)[0]
                                uval = struct.unpack_from("<I", elf, target)[0]
                                note += f" (u32=0x{uval:x}, f32={fval:.6g})"
                    except ValueError:
                        pass
            else:
                parts = [x.strip() for x in op_s.split(",")]
                if len(parts) == 3 and parts[1] in adrp_pages and parts[2].startswith("#"):
                    try:
                        off = int(parts[2].lstrip("#"), 0)
                        target = adrp_pages[parts[1]] + off
                        if target in addr_to_sym:
                            note += f" ; &{addr_to_sym[target][0]}"
                        else:
                            note += f" ; 0x{target:x}"
                            if 0 <= target < len(elf):
                                end = elf.find(b"\0", target, min(len(elf), target + 64))
                                if end > target:
                                    raw = elf[target:end]
                                    if all(32 <= b < 127 for b in raw):
                                        note += f' "{raw.decode()}"'
                    except ValueError:
                        pass

        if mnem in ("bl", "b"):
            try:
                dst = int(op_s.lstrip("#"), 0)
                if dst in plt_map:
                    note += f" ; -> {plt_map[dst]}"
                elif dst in addr_to_sym:
                    note += f" ; -> {addr_to_sym[dst][0]}"
            except ValueError:
                pass

        print(f"  +0x{insn.address - start:03x} (0x{insn.address:06x}) [{insn.bytes.hex()}]: {mnem:<7} {op_s}{note}")
        if mnem == "ret" and size == 0:
            break


def scan_text_xrefs(elf, sections, addr_to_sym, plt_map, got_globals):
    text = sections[".text"]
    t_off, t_end = text["off"], text["off"] + text["size"]
    t_addr = text["addr"]

    # Map VA ranges of functions from .dynsym/.symtab to attribute xrefs cleanly
    funcs = []
    # Find targets of interest: usleep@plt & GOT for _ZN12CMenuManager19m_PrefsFrameLimiterE
    usleep_vas = [va for va, nm in plt_map.items() if nm.startswith("usleep@")]
    flim_gots = [va for va, nm in got_globals.items() if nm == "_ZN12CMenuManager19m_PrefsFrameLimiterE"]
    print(f"\n[Audit] usleep@plt VAs: {[hex(x) for x in usleep_vas]}")
    print(f"[Audit] m_PrefsFrameLimiter GOT VAs: {[hex(x) for x in flim_gots]}")

    # Build sorted symbol table by address
    sorted_syms = sorted((va, names[0]) for va, names in addr_to_sym.items() if t_addr <= va < t_addr + text["size"])
    import bisect

    sym_addrs = [x[0] for x in sorted_syms]

    def owner_sym(va: int) -> str:
        idx = bisect.bisect_right(sym_addrs, va) - 1
        if idx >= 0:
            sva, sname = sorted_syms[idx]
            return f"{sname}+0x{va - sva:x}"
        return f"0x{va:x}"

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    print("\n--- Direct BL callers of usleep@plt / nanosleep@plt ---")
    sleep_vas = {va: nm for va, nm in plt_map.items() if nm in ("usleep@plt", "nanosleep@plt")}
    for off in range(t_off, t_end, 4):
        op = struct.unpack_from("<I", elf, off)[0]
        va = t_addr + (off - t_off)
        if (op >> 26) == 0x25:
            imm26 = op & 0x3FFFFFF
            if imm26 & 0x2000000:
                imm26 -= 0x4000000
            dst = va + imm26 * 4
            if dst in sleep_vas:
                print(f"  0x{va:06x} ({owner_sym(va)}) -> BL {sleep_vas[dst]}")

    print("\n--- Xrefs to GOT:_ZN12CMenuManager19m_PrefsFrameLimiterE ---")
    target_set = set(flim_gots)
    last_adrp: dict[int, tuple[int, int]] = {}
    for off in range(t_off, t_end, 4):
        op = struct.unpack_from("<I", elf, off)[0]
        va = t_addr + (off - t_off)
        if (op & 0x9F000000) == 0x90000000:  # adrp
            rd = op & 0x1F
            immlo = (op >> 29) & 3
            immhi = (op >> 5) & 0x7FFFF
            imm = ((immhi << 2) | immlo) << 12
            if imm & 0x100000000:
                imm -= 0x200000000
            page = (va & ~0xFFF) + imm
            last_adrp[rd] = (va, page)
        elif (op & 0xFFC00000) == 0xF9400000:  # ldr x_rt, [x_rn, #lo12]
            rn = (op >> 5) & 0x1F
            if rn in last_adrp and va - last_adrp[rn][0] <= 32:
                lo12 = ((op >> 10) & 0xFFF) * 8
                tgt = last_adrp[rn][1] + lo12
                if tgt in target_set:
                    print(f"  0x{va:06x} ({owner_sym(va)}) loads GOT m_PrefsFrameLimiter (0x{tgt:x})")


def inspect_gxt_strings_and_wad(elf, sections):
    rodata = sections[".rodata"]
    rdata = elf[rodata["off"] : rodata["off"] + rodata["size"]]
    print("\n--- .gxt filenames referenced in .rodata ---")
    idx = 0
    found = set()
    while True:
        pos = rdata.find(b".gxt", idx)
        if pos < 0:
            pos = rdata.find(b".GXT", idx)
        if pos < 0:
            break
        start = pos
        while start > 0 and rdata[start - 1] >= 32 and rdata[start - 1] < 127:
            start -= 1
        end = pos + 4
        while end < len(rdata) and rdata[end] >= 32 and rdata[end] < 127:
            end += 1
        found.add(rdata[start:end].decode("latin-1"))
        idx = pos + 4
    for s in sorted(found):
        print(" ", s)

    wad_path = "gtalcs/gamefiles/assets/data_main.wad"
    if os.path.exists(wad_path):
        print("\n--- .gxt files inside data_main.wad (header string scan) ---")
        with open(wad_path, "rb") as f:
            head = f.read(1024 * 1024)  # WAD directory is at start of archive
        gxt_entries = set()
        for ext in (b".gxt", b".GXT"):
            i = 0
            while True:
                p = head.find(ext, i)
                if p < 0:
                    break
                st = p
                while st > 0 and 32 <= head[st - 1] < 127:
                    st -= 1
                en = p + 4
                gxt_entries.add(head[st:en].decode("latin-1"))
                i = p + 4
        for s in sorted(gxt_entries):
            print(" ", s)


def main():
    if capstone is None:
        print("capstone is required")
        sys.exit(1)

    elf_path = "gtalcs/gamefiles/android-libs/libGame.so"
    elf, sections, addr_to_sym, sym_to_info, plt_map, got_globals = parse_elf(elf_path)

    targets = [
        ("_ZN6CTimer6UpdateEv", 0x300),
        ("_ZN5cTime7RefreshEv", 0x100),
        ("OS_TimeMS", 0x100),
        ("Java_com_rockstargames_gtalcs_GTAJNIlib_setJoyAxis", 0x100),
        ("Java_com_rockstargames_gtalcs_GTAJNIlib_onJoyAxis", 0x200),
        ("_ZN4CPad10UpdatePadsEv", 0x200),
        ("_ZN4CPad6UpdateEv", 0x500),
        ("_ZN12CMenuManager33InitialiseChangedLanguageSettingsEv", 0x300),
        ("_ZN5CText4LoadEv", 0x300),
    ]
    for sym, limit in targets:
        disasm_func(elf, addr_to_sym, sym_to_info, plt_map, got_globals, sym, limit)

    scan_text_xrefs(elf, sections, addr_to_sym, plt_map, got_globals)
    inspect_gxt_strings_and_wad(elf, sections)


if __name__ == "__main__":
    main()
