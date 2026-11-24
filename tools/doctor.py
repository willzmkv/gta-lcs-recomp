#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Doctor Tool
Checks hardware, compiler toolchain, Python version, build status, and Frankenstein game files (Android + iPad).

Usage:
    python tools/doctor.py
    python tools/doctor.py --json
"""
import argparse
import ctypes
import json
import os
import platform
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GB = 1 << 30
NEED_FREE_GB = 4
NEED_RAM_GB = 8


def check_folder():
    total, used, free = shutil.disk_usage(ROOT)
    warnings = []
    low = ROOT.lower()
    for var in ("ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"):
        base = os.environ.get(var)
        if base and low.startswith(base.lower().rstrip("\\") + "\\"):
            warnings.append("Project is inside Program Files. Move to e.g. D:\\Games\\LibertyStories to avoid permission issues.")
            break
    return {
        "path": ROOT,
        "free_gb": round(free / GB, 2),
        "free_bytes": free,
        "ok": free >= NEED_FREE_GB * GB,
        "warnings": warnings,
    }


def check_memory():
    if sys.platform == "win32":
        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [
                ("dwLength", ctypes.c_ulong),
                ("dwMemoryLoad", ctypes.c_ulong),
                ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong),
                ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong),
                ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong),
                ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
            ]
        stat = MEMORYSTATUSEX()
        stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(stat))
        ram_gb = round(stat.ullTotalPhys / GB, 1)
        ok = stat.ullTotalPhys >= NEED_RAM_GB * GB
        return {"total_gb": ram_gb, "ok": ok, "message": f"{ram_gb} GB RAM"}
    else:
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        kb = int(line.split()[1])
                        ram_gb = round((kb * 1024) / GB, 1)
                        ok = (kb * 1024) >= NEED_RAM_GB * GB
                        return {"total_gb": ram_gb, "ok": ok, "message": f"{ram_gb} GB RAM"}
        except Exception:
            pass
        return {"total_gb": 0, "ok": True, "message": "Unknown RAM"}


def check_cpu():
    proc = platform.processor() or platform.machine() or "x64 CPU"
    return {"ok": True, "message": proc}


def check_compiler():
    if sys.platform == "win32":
        vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
        if os.path.exists(vswhere):
            cmd = [vswhere, "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]
            try:
                out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
                if out:
                    return {
                        "compiler": "Visual Studio (MSVC / clang-cl)",
                        "toolchain": "msvc",
                        "path": out,
                        "ok": True,
                        "code": "ok",
                        "message": "Visual Studio C++ tools found",
                    }
            except Exception:
                pass
        return {
            "compiler": "MSVC / Clang",
            "toolchain": "msvc",
            "path": None,
            "ok": False,
            "code": "no_vs",
            "message": "Visual Studio C++ build tools not found",
        }
    else:
        clang = shutil.which("clang++")
        gcc = shutil.which("g++")
        if clang:
            return {"compiler": "Clang", "toolchain": "clang", "path": clang, "ok": True, "code": "ok", "message": clang}
        elif gcc:
            return {"compiler": "GCC", "toolchain": "gcc", "path": gcc, "ok": True, "code": "ok", "message": gcc}
        return {"compiler": "Clang/GCC", "toolchain": "clang", "path": None, "ok": False, "code": "missing_packages", "message": "No C++ compiler found"}


def validate_libgame_elf(path):
    if not path or not os.path.isfile(path):
        return False
    try:
        with open(path, "rb") as f:
            hdr = f.read(20)
        return len(hdr) >= 20 and hdr[0:4] == b"\x7fELF" and hdr[4] == 2 and struct.unpack_from("<H", hdr, 18)[0] == 183
    except Exception:
        return False


def validate_ipa_zip(path):
    if not path or not os.path.isfile(path):
        return False
    try:
        with zipfile.ZipFile(path, "r") as zf:
            names = [n.lower() for n in zf.namelist()]
            return any(n.endswith("worldps.glsl") for n in names)
    except Exception:
        return False


def check_game_files():
    files = {
        "ipa_source": None,
        "ipad_extracted": None,
        "apk_source": None,
        "libgame": None,
        "data_main": None,
        "data_music": None,
    }
    for name in os.listdir(ROOT):
        if name.lower().endswith(".ipa"):
            cand = os.path.join(ROOT, name)
            if validate_ipa_zip(cand):
                files["ipa_source"] = cand
                break

    ipad_dir = os.path.join(ROOT, "assets", "ipad")
    if os.path.isfile(os.path.join(ipad_dir, "WorldPs.glsl")):
        files["ipad_extracted"] = ipad_dir

    gamedata = os.path.join(ROOT, "gtalcs", "gamedata")
    if os.path.isdir(gamedata):
        for name in os.listdir(gamedata):
            if name.lower().endswith(".apk"):
                files["apk_source"] = os.path.join(gamedata, name)
                break

    lib_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "android-libs", "libGame.so")
    if validate_libgame_elf(lib_candidate):
        files["libgame"] = lib_candidate

    main_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "assets", "data_main.wad")
    if os.path.isfile(main_candidate):
        files["data_main"] = main_candidate

    music_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "assets", "data_music.wad")
    if os.path.isfile(music_candidate):
        files["data_music"] = music_candidate

    android_ok = bool(files["libgame"] and files["data_main"])
    ipad_ok = bool(files["ipa_source"] or files["ipad_extracted"])
    has_all = android_ok and ipad_ok
    return {"files": files, "android_ok": android_ok, "ipad_ok": ipad_ok, "ok": has_all}


def check_build():
    exe_name = "lcs.exe" if sys.platform == "win32" else "lcs"
    root_exe = os.path.join(ROOT, exe_name)
    build_exe = os.path.join(ROOT, "build", "bin", "Release", exe_name)
    built = os.path.isfile(root_exe) or os.path.isfile(build_exe)
    exe_path = root_exe if os.path.isfile(root_exe) else (build_exe if os.path.isfile(build_exe) else "")
    mtime = os.path.getmtime(exe_path) if exe_path else 0
    return {
        "built": built,
        "current": built,
        "exe": exe_path,
        "info": {"opt": "Standard", "time": mtime, "cpu": platform.machine()},
    }


def run_doctor():
    folder = check_folder()
    memory = check_memory()
    cpu = check_cpu()
    compiler = check_compiler()
    game_files = check_game_files()
    build = check_build()
    downloads = {"ok": True, "bundled": True, "message": "Local toolchain ready"}
    py_info = {
        "version": f"{sys.version_info.major}.{sys.version_info.minor}.{sys.version_info.micro}",
        "bundled": False,
    }

    overall_ok = folder["ok"] and compiler["ok"] and game_files["ok"]
    if not game_files["ok"]:
        next_step = "disc"
    elif not compiler["ok"]:
        next_step = "tools"
    elif not build["built"]:
        next_step = "build"
    else:
        next_step = "play"

    return {
        "folder": folder,
        "memory": memory,
        "cpu": cpu,
        "compiler": compiler,
        "downloads": downloads,
        "python": py_info,
        "game_files": game_files,
        "build": build,
        "next": next_step,
        "overall_ok": overall_ok,
    }


def print_report(rep):
    def tag(ok):
        return "[OK]" if ok else "[!!]"

    print("=" * 60)
    print(" GTA: Liberty City Stories Native PC Port - System Doctor")
    print("=" * 60)
    print(f"Project Root: {rep['folder']['path']}")
    print(f"{tag(rep['folder']['ok'])} Disk Space:     {rep['folder']['free_gb']} GB free")
    for w in rep["folder"]["warnings"]:
        print(f"       Warning: {w}")
    print(f"{tag(rep['memory']['ok'])} System Memory:  {rep['memory']['total_gb']} GB RAM")

    comp = rep["compiler"]
    print(f"{tag(comp['ok'])} Compiler:       {comp['compiler']} ({comp.get('path') or comp.get('message')})")

    gf = rep["game_files"]
    print(f"{tag(gf['ok'])} Frankenstein Game Files (Android + iPad):")
    print(f"       - Android Binary (libGame.so ARM64): {'FOUND' if gf['files']['libgame'] else 'MISSING'}")
    print(f"       - Android Data (data_main.wad):      {'FOUND' if gf['files']['data_main'] else 'MISSING'}")
    print(f"       - Android Music (data_music.wad):    {'FOUND' if gf['files']['data_music'] else 'OPTIONAL'}")
    print(f"       - iPad Definitive IPA / Shaders:     {'FOUND' if gf['ipad_ok'] else 'MISSING'}")

    print("-" * 60)
    if rep["overall_ok"]:
        print(f"Status: READY (next: {rep['next']}).")
    else:
        print(f"Status: ACTION REQUIRED (next: {rep['next']}).")
    print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="Environment diagnostic tool for gta-lcs-recomp")
    parser.add_argument("--json", action="store_true", help="Print diagnostic output as JSON")
    args = parser.parse_args()

    report = run_doctor()
    if args.json:
        print(json.dumps(report))
    else:
        print_report(report)


if __name__ == "__main__":
    main()
