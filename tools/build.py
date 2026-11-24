#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Build Pipeline
Configures and compiles lcs.exe or LCSLauncher.exe with CMake and MSVC/Ninja.

Usage:
    python tools/build.py
    python tools/build.py --launcher
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
LAUNCHER_BUILD_DIR = os.path.join(ROOT, "build", "launcher")


def find_cmake():
    cmake = shutil.which("cmake")
    if cmake:
        return cmake
    if sys.platform == "win32":
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


def configure_and_build_launcher(cmake_exe, opt_mode="quick"):
    os.makedirs(LAUNCHER_BUILD_DIR, exist_ok=True)
    cfg = "Debug" if opt_mode == "debug" else "Release"
    cmd_cfg = [
        cmake_exe,
        "-S", os.path.join(ROOT, "launcher"),
        "-B", LAUNCHER_BUILD_DIR,
        f"-DCMAKE_BUILD_TYPE={cfg}",
    ]
    if sys.platform == "win32":
        cmd_cfg.extend(["-A", "x64"])
    print(f"[build] Configuring LCSLauncher: {' '.join(cmd_cfg)}", flush=True)
    subprocess.check_call(cmd_cfg)

    cmd_bld = [
        cmake_exe,
        "--build", LAUNCHER_BUILD_DIR,
        "--config", cfg,
        "--parallel",
    ]
    print(f"[build] Building LCSLauncher: {' '.join(cmd_bld)}", flush=True)
    subprocess.check_call(cmd_bld)

    # Ensure LCSLauncher.exe is in project root (multi-config generators put it in root/Release)
    exe_name = "LCSLauncher.exe" if sys.platform == "win32" else "LCSLauncher"
    for cand in (
        os.path.join(ROOT, cfg, exe_name),
        os.path.join(LAUNCHER_BUILD_DIR, cfg, exe_name),
        os.path.join(LAUNCHER_BUILD_DIR, exe_name),
    ):
        if os.path.isfile(cand):
            dst = os.path.join(ROOT, exe_name)
            shutil.copy2(cand, dst)
            print(f"[build] Deployed launcher to project root: {dst}", flush=True)
            break


def configure(cmake_exe, opt_mode="lto", build_dir=None):
    bdir = build_dir or BUILD_DIR
    os.makedirs(bdir, exist_ok=True)
    cmd = [
        cmake_exe,
        "-S", ROOT,
        "-B", bdir,
        f"-DCMAKE_BUILD_TYPE={'Release' if opt_mode != 'debug' else 'Debug'}",
        f"-DENABLE_LTO={'ON' if opt_mode == 'lto' else 'OFF'}",
    ]
    if sys.platform == "win32":
        cmd.extend(["-A", "x64"])
    print(f"[build] Configuring lcs: {' '.join(cmd)}", flush=True)
    subprocess.check_call(cmd)


def build(cmake_exe, opt_mode="lto", build_dir=None):
    bdir = build_dir or BUILD_DIR
    cfg = "Release" if opt_mode != "debug" else "Debug"
    cmd = [
        cmake_exe,
        "--build", bdir,
        "--config", cfg,
        "--parallel",
    ]
    print(f"[build] Building lcs: {' '.join(cmd)}", flush=True)
    subprocess.check_call(cmd)

    exe_name = "lcs.exe" if sys.platform == "win32" else "lcs"
    built_exe = os.path.join(bdir, "bin", cfg, exe_name)
    root_exe = os.path.join(ROOT, exe_name)
    if os.path.isfile(built_exe):
        try:
            shutil.copy2(built_exe, root_exe)
            print(f"[build] Deployed executable to project root: {root_exe}", flush=True)
        except Exception as e:
            print(f"Warning: could not copy to {root_exe}: {e}", flush=True)


def clean():
    if os.path.exists(BUILD_DIR):
        print(f"Removing build directory: {BUILD_DIR}")
        shutil.rmtree(BUILD_DIR)
    print("Clean completed.")


def main():
    parser = argparse.ArgumentParser(description="Build pipeline for gta-lcs-recomp")
    parser.add_argument("--launcher", action="store_true", help="Build LCSLauncher.exe")
    parser.add_argument("--quick", action="store_true", help="Build without Link-Time Optimization (faster compile)")
    parser.add_argument("--opt", choices=["lto", "quick", "debug"], default="lto", help="Optimization profile")
    parser.add_argument("--clean", action="store_true", help="Remove existing build directory")
    parser.add_argument("--build-dir", default=None, help="Custom build directory")
    args, _unknown = parser.parse_known_args()

    if args.clean:
        clean()
        return

    opt = "quick" if args.quick else args.opt

    cmake = find_cmake()
    if not cmake:
        print("ERROR: CMake was not found. Please install CMake or Visual Studio C++ CMake tools.", file=sys.stderr)
        sys.exit(1)

    print(f"Using CMake: {cmake}", flush=True)
    if args.launcher:
        configure_and_build_launcher(cmake, opt)
        return

    configure(cmake, opt, args.build_dir)
    build(cmake, opt, args.build_dir)


if __name__ == "__main__":
    main()
