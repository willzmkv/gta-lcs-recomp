#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - iPad Asset Extractor
Extracts Retina textures, high-poly models, shaders, and lossless audio from GTALcs.ipa.

Usage:
    python tools/extract_ipa.py
    python tools/extract_ipa.py path/to/GTALcs.ipa
    python tools/extract_ipa.py --info
"""
import argparse
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST_DIR = os.path.join(ROOT, "assets", "ipad")

TARGET_PREFIXES = (
    "Payload/GTALcs.app/Textures_PVR_LOD/",
    "Payload/GTALcs.app/Textures_PVR/",
    "Payload/GTALcs.app/Textures_Shared/",
    "Payload/GTALcs.app/Textures_Shared_PVR/",
    "Payload/GTALcs.app/AUDIO/",
    "Payload/GTALcs.app/Data/",
    "Payload/GTALcs.app/Models/",
    "Payload/GTALcs.app/SPLASH/",
    "Payload/GTALcs.app/TEXT/",
)

def find_ipa(provided_path=None):
    if provided_path and os.path.isfile(provided_path):
        return provided_path
    for fname in os.listdir(ROOT):
        if fname.lower().endswith(".ipa"):
            return os.path.join(ROOT, fname)
    return None

def inspect_ipa(ipa_path):
    print(f"Opening IPA: {ipa_path}")
    with zipfile.ZipFile(ipa_path, "r") as z:
        entries = z.namelist()
        total = len(entries)
        pvr_count = sum(1 for e in entries if e.lower().endswith(".pvr"))
        audio_count = sum(1 for e in entries if "/audio/" in e.lower() or e.lower().endswith((".mp3", ".wav", ".m4a")))
        glsl_count = sum(1 for e in entries if e.lower().endswith(".glsl"))
        data_count = sum(1 for e in entries if "/data/" in e.lower())
        
        print(f"Total entries in IPA: {total}")
        print(f"  • PVR Retina Textures: {pvr_count}")
        print(f"  • Audio Streams/SFX:   {audio_count}")
        print(f"  • Custom GLSL Shaders: {glsl_count}")
        print(f"  • Data/Map Definition: {data_count}")

def extract_assets(ipa_path, out_dir):
    print(f"Extracting assets from: {ipa_path}")
    print(f"Destination folder:    {out_dir}")
    os.makedirs(out_dir, exist_ok=True)

    with zipfile.ZipFile(ipa_path, "r") as z:
        extracted = 0
        for entry in z.infolist():
            # Check target prefix or GLSL shader files
            is_target = any(entry.filename.startswith(p) for p in TARGET_PREFIXES) or \
                        (entry.filename.startswith("Payload/GTALcs.app/") and entry.filename.endswith(".glsl"))
            
            if not is_target:
                continue

            rel_path = entry.filename[len("Payload/GTALcs.app/"):].lstrip("/")
            if not rel_path:
                continue

            target_path = os.path.join(out_dir, rel_path)
            if entry.is_dir():
                os.makedirs(target_path, exist_ok=True)
            else:
                os.makedirs(os.path.dirname(target_path), exist_ok=True)
                with z.open(entry) as src, open(target_path, "wb") as dst:
                    dst.write(src.read())
                extracted += 1
                if extracted % 500 == 0:
                    print(f"  Extracted {extracted} files...")

        print(f"Done! Successfully extracted {extracted} files into {out_dir}.")

def main():
    parser = argparse.ArgumentParser(description="Extract definitive edition assets from GTALcs.ipa")
    parser.add_argument("ipa", nargs="?", help="Path to GTALcs.ipa (default: auto-detected in project root)")
    parser.add_argument("--info", action="store_true", help="Display IPA asset summary without extracting")
    parser.add_argument("--out", default=DEST_DIR, help="Destination folder (default: assets/ipad)")
    args = parser.parse_args()

    ipa_path = find_ipa(args.ipa)
    if not ipa_path:
        print("ERROR: No .ipa file found in project root or specified path.", file=sys.stderr)
        sys.exit(1)

    if args.info:
        inspect_ipa(ipa_path)
    else:
        inspect_ipa(ipa_path)
        extract_assets(ipa_path, args.out)

if __name__ == "__main__":
    main()
