#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Texture Converter
Converts iPad PVRTC (.pvr) textures into PC-compatible formats (DXT1/DXT5/BC7 DDS or PNG).

Usage:
    python tools/texture_convert.py
    python tools/texture_convert.py --format dds
    python tools/texture_convert.py --format png
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS_DIR = os.path.join(ROOT, "assets", "ipad")
OUTPUT_DIR = os.path.join(ROOT, "assets", "converted_textures")

def find_converter():
    tool = shutil.which("PVRTexToolCLI")
    if tool:
        return tool
    # Check default Imagination Technologies install locations
    candidates = [
        r"C:\Program Files\Imagination Technologies\PowerVR_Graphics\PowerVR_Tools\PVRTexTool\CLI\Windows_x86_64\PVRTexToolCLI.exe",
        r"C:\Imagination\PowerVR_Graphics\PowerVR_Tools\PVRTexTool\CLI\Windows_x86_64\PVRTexToolCLI.exe",
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None

def convert_textures(src_dir, out_dir, fmt="dds"):
    tool = find_converter()
    if not tool:
        print("NOTE: PVRTexToolCLI not found in PATH or standard install paths.")
        print("To convert 9,176 PVRTC textures at maximum GPU fidelity, download the free PowerVR Tools:")
        print("  https://developer.imaginationtech.com/pvrtextool/")
        print("Alternatively, the runtime will decode textures dynamically or use PNG fallbacks.")
        return

    os.makedirs(out_dir, exist_ok=True)
    print(f"Using converter: {tool}")
    print(f"Source folder:   {src_dir}")
    print(f"Output folder:   {out_dir}")
    print(f"Target format:   {fmt.upper()}")

    count = 0
    for root, _, files in os.walk(src_dir):
        for f in files:
            if f.lower().endswith(".pvr"):
                src_path = os.path.join(root, f)
                rel_dir = os.path.relpath(root, src_dir)
                dest_folder = os.path.join(out_dir, rel_dir)
                os.makedirs(dest_folder, exist_ok=True)
                dest_name = os.path.splitext(f)[0] + ("." + fmt)
                dest_path = os.path.join(dest_folder, dest_name)

                # Command for PVRTexToolCLI
                cmd = [
                    tool,
                    "-i", src_path,
                    "-o", dest_path,
                    "-f", "BC3,UBN,sRGB" if fmt == "dds" else "r8g8b8a8,UBN,sRGB",
                    "-m"  # generate mipmaps
                ]
                try:
                    subprocess.check_call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    count += 1
                    if count % 100 == 0:
                        print(f"  Converted {count} textures...")
                except Exception as e:
                    print(f"  Warning: failed to convert {f}: {e}")

    print(f"Conversion complete! Converted {count} textures.")

def main():
    parser = argparse.ArgumentParser(description="Convert PVRTC textures to PC-native DDS or PNG")
    parser.add_argument("--src", default=ASSETS_DIR, help="Source assets folder (default: assets/ipad)")
    parser.add_argument("--out", default=OUTPUT_DIR, help="Output folder (default: assets/converted_textures)")
    parser.add_argument("--format", choices=["dds", "png"], default="dds", help="Target texture format")
    args = parser.parse_args()

    if not os.path.isdir(args.src):
        print(f"Source directory '{args.src}' does not exist. Run 'python tools/extract_ipa.py' first.", file=sys.stderr)
        sys.exit(1)

    convert_textures(args.src, args.out, args.format)

if __name__ == "__main__":
    main()
