#!/usr/bin/env python3
"""GTA: Liberty City Stories Native PC Port - Doctor Tool
Checks hardware, compiler toolchain, Python version, and legal game files.

Usage:
    python tools/doctor.py
    python tools/doctor.py --json
"""
import argparse
import ctypes
import json
import os
import shutil
import subprocess
import sys

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
        return {"total_gb": ram_gb, "ok": stat.ullTotalPhys >= NEED_RAM_GB * GB}
    else:
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        kb = int(line.split()[1])
                        ram_gb = round((kb * 1024) / GB, 1)
                        return {"total_gb": ram_gb, "ok": (kb * 1024) >= NEED_RAM_GB * GB}
        except Exception:
            pass
        return {"total_gb": 0, "ok": True}

def check_compiler():
    if sys.platform == "win32":
        vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
        if os.path.exists(vswhere):
            cmd = [vswhere, "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"]
            try:
                out = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
                if out:
                    return {"compiler": "Visual Studio 2022 (MSVC / clang-cl)", "path": out, "ok": True}
            except Exception:
                pass
        return {"compiler": "MSVC / Clang", "path": None, "ok": False, "message": "Visual Studio C++ build tools not found"}
    else:
        clang = shutil.which("clang++")
        gcc = shutil.which("g++")
        if clang:
            return {"compiler": "Clang", "path": clang, "ok": True}
        elif gcc:
            return {"compiler": "GCC", "path": gcc, "ok": True}
        return {"compiler": "Clang/GCC", "path": None, "ok": False, "message": "No C++ compiler found (install clang or g++)"}

def check_game_files():
    files = {
        "ipa_source": None,
        "apk_source": None,
        "libgame": None,
        "data_main": None,
        "data_music": None,
    }
    # Check for IPA
    for name in os.listdir(ROOT):
        if name.lower().endswith(".ipa"):
            files["ipa_source"] = os.path.join(ROOT, name)
            break
    # Check for APK or extracted files
    apk_candidate = os.path.join(ROOT, "gtalcs", "gamedata", "gta-liberty-city-stories-2.4.379-mod-menu-5play.apk")
    if os.path.isfile(apk_candidate):
        files["apk_source"] = apk_candidate
    
    lib_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "android-libs", "libGame.so")
    if os.path.isfile(lib_candidate):
        files["libgame"] = lib_candidate

    main_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "assets", "data_main.wad")
    if os.path.isfile(main_candidate):
        files["data_main"] = main_candidate

    music_candidate = os.path.join(ROOT, "gtalcs", "gamefiles", "assets", "data_music.wad")
    if os.path.isfile(music_candidate):
        files["data_music"] = music_candidate

    has_all = bool(files["libgame"] and files["data_main"] and files["ipa_source"])
    return {"files": files, "ok": has_all}

def run_doctor():
    folder = check_folder()
    memory = check_memory()
    compiler = check_compiler()
    game_files = check_game_files()
    
    overall_ok = folder["ok"] and compiler["ok"] and game_files["ok"]
    
    return {
        "folder": folder,
        "memory": memory,
        "compiler": compiler,
        "game_files": game_files,
        "overall_ok": overall_ok,
    }

def print_report(rep):
    def tag(ok):
        return "[OK]" if ok else "[!!]"

    print("=" * 60)
    print(" GTA: Liberty City Stories Native PC Port — System Doctor")
    print("=" * 60)
    print(f"Project Root: {rep['folder']['path']}")
    print(f"{tag(rep['folder']['ok'])} Disk Space:     {rep['folder']['free_gb']} GB free")
    for w in rep['folder']['warnings']:
        print(f"       Warning: {w}")
    print(f"{tag(rep['memory']['ok'])} System Memory:  {rep['memory']['total_gb']} GB RAM")
    
    comp = rep['compiler']
    print(f"{tag(comp['ok'])} Compiler:       {comp['compiler']} ({comp.get('path') or comp.get('message')})")
    
    gf = rep['game_files']
    print(f"{tag(gf['ok'])} Legal Game Files:")
    print(f"       - Android Binary (libGame.so): {'FOUND' if gf['files']['libgame'] else 'MISSING'}")
    print(f"       - Android Data (data_main.wad): {'FOUND' if gf['files']['data_main'] else 'MISSING'}")
    print(f"       - iPad Definitive IPA:          {'FOUND' if gf['files']['ipa_source'] else 'MISSING'}")

    print("-" * 60)
    if rep["overall_ok"]:
        print("Status: READY. All prerequisites and legal game files are available.")
    else:
        print("Status: ACTION REQUIRED. Review the missing items above.")
    print("=" * 60)

def main():
    parser = argparse.ArgumentParser(description="Environment diagnostic tool for gta-lcs-recomp")
    parser.add_argument("--json", action="store_true", help="Print diagnostic output as JSON")
    args = parser.parse_args()

    report = run_doctor()
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)

if __name__ == "__main__":
    main()
