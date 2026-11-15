#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - WAD Tool
Inspects, tests, and parses War Drum Studios data_main.wad archives.

Usage:
    python tools/wad_tool.py info path/to/data_main.wad
"""
import argparse
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_WAD = os.path.join(ROOT, "gtalcs", "gamefiles", "assets", "data_main.wad")

def inspect_wad(wad_path):
    if not os.path.isfile(wad_path):
        print(f"ERROR: File not found: {wad_path}", file=sys.stderr)
        return

    size_mb = round(os.path.getsize(wad_path) / (1024 * 1024), 2)
    print(f"WAD Archive: {wad_path}")
    print(f"Size:        {size_mb} MB")

    with open(wad_path, "rb") as f:
        magic = f.read(4)
        print(f"Magic (Hex): {magic.hex().upper()}")
        f.seek(0)
        sample = f.read(64)
        print(f"Header Preview (Hex): {sample[:32].hex().upper()}")

def main():
    parser = argparse.ArgumentParser(description="Inspect and process GTA LCS data_main.wad archives")
    subparsers = parser.add_subparsers(dest="command")

    info_p = subparsers.add_parser("info", help="Inspect WAD header and size")
    info_p.add_argument("wad", nargs="?", default=DEFAULT_WAD, help="Path to data_main.wad")

    args = parser.parse_args()
    if args.command == "info" or not args.command:
        wad_file = getattr(args, "wad", DEFAULT_WAD)
        inspect_wad(wad_file)

if __name__ == "__main__":
    main()
