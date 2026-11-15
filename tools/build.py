#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Build Pipeline
Configures and compiles the project with CMake and Ninja/MSVC.

Usage:
    python tools/build.py
    python tools/build.py --quick
    python tools/build.py --opt lto
    python tools/build.py --clean
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(ROOT, "build")

def find_cmake():
    cmake = shutil.which("cmake")
    if cmake:
        return cmake
    if sys.platform == "win32":
        # Check standard Visual Studio CMake location
        vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
        if os.path.exists(vswhere):
            cmd = [vswhere, "-latest", "-property", "installationPath"]
            try:
                vs_path = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
                candidate = os.path.join(vs_path, r"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
                if os.path.isfile(candidate):
                    return candidate
            except Exception:
                pass
    return None

def configure(cmake_exe, opt_mode="lto"):
    os.makedirs(BUILD_DIR, exist_ok=True)
    cmd = [
        cmake_exe,
        "-S", ROOT,
        "-B", BUILD_DIR,
        f"-DCMAKE_BUILD_TYPE={'Release' if opt_mode != 'debug' else 'Debug'}",
        f"-DENABLE_LTO={'ON' if opt_mode == 'lto' else 'OFF'}",
    ]
    if sys.platform == "win32":
        cmd.extend(["-A", "x64"])
    print(f"Configuring with: {' '.join(cmd)}")
    subprocess.check_call(cmd)

def build(cmake_exe, opt_mode="lto"):
    cmd = [
        cmake_exe,
        "--build", BUILD_DIR,
        "--config", "Release" if opt_mode != "debug" else "Debug",
        "--parallel"
    ]
    print(f"Building with: {' '.join(cmd)}")
    subprocess.check_call(cmd)

def clean():
    if os.path.exists(BUILD_DIR):
        print(f"Removing build directory: {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)
    print("Clean completed.")

def main():
    parser = argparse.ArgumentParser(description="Build pipeline for gta-lcs-recomp")
    parser.add_argument("--quick", action="store_true", help="Build without Link-Time Optimization (faster compile)")
    parser.add_argument("--opt", choices=["lto", "quick", "debug"], default="lto", help="Optimization profile")
    parser.add_argument("--clean", action="store_true", help="Remove existing build directory")
    args = parser.parse_args()

    if args.clean:
        clean()
        return

    opt = "quick" if args.quick else args.opt

    cmake = find_cmake()
    if not cmake:
        print("ERROR: CMake was not found. Please install CMake or Visual Studio C++ CMake tools.", file=sys.stderr)
        sys.exit(1)

    print(f"Using CMake: {cmake}")
    configure(cmake, opt)
    build(cmake, opt)

if __name__ == "__main__":
    main()
