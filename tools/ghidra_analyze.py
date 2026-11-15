#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Ghidra Analysis Pipeline
Runs headless Ghidra analysis on libGame.so and lcs_runtime to decompile
and inspect JNI callbacks, LogicalFS, WadArchive, and RenderWare bridges.

Usage:
    python tools/ghidra_analyze.py
    python tools/ghidra_analyze.py --target runtime
    python tools/ghidra_analyze.py --target libgame
    python tools/ghidra_analyze.py --script path/to/script.java
"""
import argparse
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_GHIDRA = r"D:\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC"
GHIDRA_HOME = os.environ.get("GHIDRA_HOME", DEFAULT_GHIDRA)

TARGETS = {
    "libgame": os.path.join(ROOT, "gtalcs", "gamefiles", "android-libs", "libGame.so"),
    "runtime": os.path.join(ROOT, "gtalcs", "lcs_runtime"),
}

PROJECT_DIR = os.path.join(ROOT, "build", "ghidra", "project")
EXPORT_DIR = os.path.join(ROOT, "build", "ghidra", "export")

def find_analyze_headless():
    bat = os.path.join(GHIDRA_HOME, "support", "analyzeHeadless.bat" if sys.platform == "win32" else "analyzeHeadless")
    if os.path.isfile(bat):
        return bat
    return None

def run_analysis(target_name, post_script=None, max_cpu=4):
    headless = find_analyze_headless()
    if not headless:
        print(f"ERROR: analyzeHeadless not found at: {GHIDRA_HOME}\\support", file=sys.stderr)
        print("Please verify that GHIDRA_HOME is set correctly.", file=sys.stderr)
        sys.exit(1)

    binary_path = TARGETS.get(target_name)
    if not binary_path or not os.path.isfile(binary_path):
        print(f"ERROR: Target binary '{target_name}' not found: {binary_path}", file=sys.stderr)
        sys.exit(1)

    os.makedirs(PROJECT_DIR, exist_ok=True)
    os.makedirs(EXPORT_DIR, exist_ok=True)

    print("=" * 60)
    print(" GTA: Liberty City Stories — Ghidra Headless Analysis")
    print("=" * 60)
    print(f"Ghidra Path:   {GHIDRA_HOME}")
    print(f"Target Binary: {binary_path}")
    print(f"Project Dir:   {PROJECT_DIR}")
    print(f"Export Dir:    {EXPORT_DIR}")
    print(f"Max CPU Cores: {max_cpu}")
    print("-" * 60)

    cmd = [
        headless,
        PROJECT_DIR,
        f"lcs_{target_name}",
        "-import", os.path.abspath(binary_path),
        "-overwrite",
        "-max-cpu", str(max_cpu),
        "-scriptPath", os.path.abspath(os.path.join(ROOT, "tools", "ghidra")),
    ]

    if post_script:
        cmd.extend(["-postScript", post_script])

    print("Executing Ghidra analysis...")
    # On Windows, stdin needs to be NUL so analyzeHeadless doesn't hang on pause
    ret = subprocess.run(cmd, stdin=subprocess.DEVNULL)
    if ret.returncode != 0:
        print(f"Ghidra exited with code: {ret.returncode}", file=sys.stderr)
        sys.exit(ret.returncode)

    print("Ghidra analysis finished successfully.")

def main():
    parser = argparse.ArgumentParser(description="Headless Ghidra analyzer for gta-lcs-recomp")
    parser.add_argument("--target", choices=["libgame", "runtime"], default="runtime",
                        help="Target binary to analyze (default: runtime)")
    parser.add_argument("--script", help="Path to Ghidra Java/Python post-script")
    parser.add_argument("--max-cpu", type=int, default=4, help="Maximum CPU cores to use (default: 4)")
    args = parser.parse_args()

    run_analysis(args.target, args.script, args.max_cpu)

if __name__ == "__main__":
    main()
