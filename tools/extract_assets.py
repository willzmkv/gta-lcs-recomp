#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Frankenstein Asset Extractor
Extracts Android (.apk/.obb/.zip) and iPad (.ipa) game assets into the expected layout:
  - lib/arm64-v8a/libGame.so  -> gtalcs/gamefiles/android-libs/libGame.so
  - data_main.wad             -> gtalcs/gamefiles/assets/data_main.wad
  - data_music.wad            -> gtalcs/gamefiles/assets/data_music.wad
  - Payload/GTALcs.app/*.glsl -> assets/ipad/
  - Payload/GTALcs.app/Data/TIMECYC.DAT -> assets/ipad/Data/TIMECYC.DAT

Usage:
    python tools/extract_assets.py --apk path/to/gtalcs.apk --obb path/to/main.obb
    python tools/extract_assets.py --ipa path/to/GTALcs.ipa
    python tools/extract_assets.py --file path/to/any.apk|.obb|.ipa
"""
import argparse
import json
import os
import shutil
import struct
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def emit(event: dict):
    print("@lcs " + json.dumps(event, ensure_ascii=False), flush=True)


def validate_elf_arm64(path: str) -> tuple[bool, str]:
    try:
        with open(path, "rb") as f:
            hdr = f.read(20)
        if len(hdr) < 20 or hdr[0:4] != b"\x7fELF":
            return False, "Not an ELF binary"
        if hdr[4] != 2:
            return False, "Not a 64-bit ELF (ELF64 required)"
        e_machine = struct.unpack_from("<H", hdr, 18)[0]
        if e_machine != 183:
            return False, f"Wrong architecture (e_machine={e_machine}, expected EM_AARCH64=183)"
        return True, "Valid ARM64 ELF64 (libGame.so)"
    except Exception as e:
        return False, str(e)


def extract_android_archive(archive_path: str, force: bool = False) -> dict:
    result = {"ok": False, "extracted": [], "errors": []}
    if not os.path.isfile(archive_path):
        result["errors"].append(f"File not found: {archive_path}")
        return result

    libs_dir = os.path.join(ROOT, "gtalcs", "gamefiles", "android-libs")
    assets_dir = os.path.join(ROOT, "gtalcs", "gamefiles", "assets")
    os.makedirs(libs_dir, exist_ok=True)
    os.makedirs(assets_dir, exist_ok=True)

    try:
        with zipfile.ZipFile(archive_path, "r") as zf:
            names = zf.namelist()
            targets = []
            for n in names:
                low = n.lower().replace("\\", "/")
                base = os.path.basename(low)
                if low.endswith("lib/arm64-v8a/libgame.so") or base == "libgame.so":
                    targets.append((n, os.path.join(libs_dir, "libGame.so")))
                elif base == "data_main.wad":
                    targets.append((n, os.path.join(assets_dir, "data_main.wad")))
                elif base == "data_music.wad":
                    targets.append((n, os.path.join(assets_dir, "data_music.wad")))

            if not targets:
                result["errors"].append("Archive contains neither libGame.so nor data_main.wad / data_music.wad")
                return result

            emit({"stage": "android", "title": "Extracting Android assets", "total": len(targets), "done": 0})
            for idx, (src_name, dst_path) in enumerate(targets, 1):
                if os.path.isfile(dst_path) and not force:
                    print(f"[extract] Skipping existing {os.path.relpath(dst_path, ROOT)}", flush=True)
                else:
                    print(f"[extract] {src_name} -> {os.path.relpath(dst_path, ROOT)}", flush=True)
                    with zf.open(src_name) as src, open(dst_path, "wb") as dst:
                        shutil.copyfileobj(src, dst, 1024 * 1024)
                if os.path.basename(dst_path) == "libGame.so":
                    ok_elf, msg_elf = validate_elf_arm64(dst_path)
                    if not ok_elf:
                        result["errors"].append(f"Invalid libGame.so: {msg_elf}")
                        return result
                result["extracted"].append(dst_path)
                emit({"progress": idx, "total": len(targets), "note": os.path.basename(dst_path)})
        result["ok"] = True
    except zipfile.BadZipFile:
        result["errors"].append("File is not a valid ZIP/APK/OBB archive")
    except Exception as e:
        result["errors"].append(str(e))
    return result


def extract_ipad_ipa(ipa_path: str, force: bool = False) -> dict:
    result = {"ok": False, "extracted_count": 0, "errors": []}
    if not os.path.isfile(ipa_path):
        result["errors"].append(f"File not found: {ipa_path}")
        return result

    ipad_dir = os.path.join(ROOT, "assets", "ipad")
    os.makedirs(ipad_dir, exist_ok=True)

    try:
        with zipfile.ZipFile(ipa_path, "r") as zf:
            app_prefix = None
            for n in zf.namelist():
                if n.startswith("Payload/") and ".app/" in n:
                    app_prefix = n.split(".app/", 1)[0] + ".app/"
                    break
            if not app_prefix:
                result["errors"].append("Not a valid iPad IPA (missing Payload/*.app/)")
                return result

            members = []
            has_world_ps = False
            has_timecyc = False
            for info in zf.infolist():
                if info.is_dir() or not info.filename.startswith(app_prefix):
                    continue
                rel = info.filename[len(app_prefix):]
                low = rel.lower()
                if low == "worldps.glsl":
                    has_world_ps = True
                if low == "data/timecyc.dat":
                    has_timecyc = True
                if low.endswith(".glsl") or low == "data/timecyc.dat" or low.endswith(".dat") or low.endswith(".cfg") or low.endswith(".wad"):
                    members.append((info, rel))

            if not has_world_ps:
                result["errors"].append("IPA is missing Payload/GTALcs.app/WorldPs.glsl")
                return result

            emit({"stage": "ipad", "title": "Extracting iPad HD assets & shaders", "total": len(members), "done": 0})
            for idx, (info, rel) in enumerate(members, 1):
                dst_path = os.path.join(ipad_dir, *rel.split("/"))
                os.makedirs(os.path.dirname(dst_path), exist_ok=True)
                if not os.path.isfile(dst_path) or force:
                    with zf.open(info) as src, open(dst_path, "wb") as dst:
                        shutil.copyfileobj(src, dst, 1024 * 1024)
                    result["extracted_count"] += 1
                if idx % 10 == 0 or idx == len(members):
                    emit({"progress": idx, "total": len(members), "note": rel})
            result["has_timecyc"] = has_timecyc
            result["ok"] = True
            print(f"[extract] iPad IPA verified ({len(members)} assets checked, {result['extracted_count']} written)", flush=True)
    except zipfile.BadZipFile:
        result["errors"].append("File is not a valid ZIP/IPA archive")
    except Exception as e:
        result["errors"].append(str(e))
    return result


def main():
    parser = argparse.ArgumentParser(description="Extract Android and iPad assets for GTA: LCS PC Port")
    parser.add_argument("--apk", help="Path to Android .apk")
    parser.add_argument("--obb", help="Path to Android .obb")
    parser.add_argument("--ipa", help="Path to iPad .ipa")
    parser.add_argument("--file", help="Auto-detect and extract a single .apk/.obb/.ipa file")
    parser.add_argument("--force", action="store_true", help="Overwrite existing extracted files")
    parser.add_argument("--json", action="store_true", help="Output JSON summary")
    args = parser.parse_args()

    summary = {"ok": True, "android": [], "ipad": None}

    if args.file:
        ext = os.path.splitext(args.file)[1].lower()
        if ext == ".ipa":
            args.ipa = args.file
        else:
            args.apk = args.file

    for p in (args.apk, args.obb):
        if p:
            res = extract_android_archive(p, force=args.force)
            summary["android"].append(res)
            if not res["ok"]:
                summary["ok"] = False

    if args.ipa:
        res = extract_ipad_ipa(args.ipa, force=args.force)
        summary["ipad"] = res
        if not res["ok"]:
            summary["ok"] = False

    if args.json:
        print(json.dumps(summary, indent=2))
    elif not summary["ok"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
