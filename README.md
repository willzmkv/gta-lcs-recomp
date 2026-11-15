# GTA: Liberty City Stories — Native PC port (ARM64 Dynarmic Runtime & iOS Definitive Assets)

[![Build Status](https://img.shields.io/badge/build-in%20development-blue.svg)](#)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey.svg)](#)
[![Graphics](https://img.shields.io/badge/renderers-Direct3D%2012%20%7C%20Vulkan%201.3-orange.svg)](#)

A native Windows (`.exe`) and Linux PC port of **Grand Theft Auto: Liberty City Stories**, running the 64-bit Android game logic (`libGame.so` v2.4.379) through the **Dynarmic ARM64 JIT** engine, rendered exclusively via **Direct3D 12** and **Vulkan 1.3**, and enriched with the **full Retina assets, high-poly models, uncompressed audio, and real-time shadow shaders from the iPad 1.14 Definitive Edition**.

> [!NOTE]
> This project follows the philosophy, tooling, and modding architecture established by [gothamor/gta-vcs-recomp](https://github.com/gothamor/gta-vcs-recomp) and [willzmkv/gta-vcs-recomp](https://github.com/willzmkv/gta-vcs-recomp).
> 
> Detailed architecture, memory specifications, and technical milestones can be found in [PROJECT_PLAN.md](PROJECT_PLAN.md).

---

## Highlights

### ⚡ Pure Native PC Performance
- **Zero Emulators / Zero Virtual Machines**: Runs directly on bare metal x86_64 hardware via an embedded Dynarmic ARM64 JIT compiler with fast direct host callbacks.
- **Direct3D 12 (Windows Default)**: Fully native D3D12 backend built for modern GPUs, featuring low CPU draw-call overhead, descriptor heaps, and explicit barrier synchronization.
- **Vulkan 1.3 (Linux & Windows)**: Modern Vulkan rendering pipeline utilizing dynamic rendering (`VK_KHR_dynamic_rendering`) and persistent on-disk pipeline caches.
- **Unlocked Framerate**: Decoupled engine tick and framerate allowing ultra-smooth gameplay at 60 FPS, 120 FPS, 144 FPS, and 240+ FPS without physics breaks.
- **Ultrawide Support**: Automatic camera FOV calculation and HUD scaling for 16:9, 16:10, 21:9, and 32:9 monitors.

### 🎨 iPad 1.14 Definitive Assets
- **Retina Textures**: Complete extraction and ingestion of 9,176 high-resolution textures from the iOS/iPad release, converted from PVRTC to native desktop formats (BC1/BC3/BC7 / DXT and PNG) using GPU-accelerated texture pipelines.
- **Enhanced Geometry**: Higher-poly vehicle models, detailed character meshes, and increased draw-distance placement files (`.ide`, `.ipl`).
- **Real-Time Shaders & Lighting**: Restored real-time projected shadow shaders (`BlobShadowVs.glsl`), vehicle headlight projections, wet-road reflections, and post-process lighting.
- **Lossless Audio**: Crystal clear stereo radio stations and environmental sound effects loaded directly from the uncompressed iOS `AUDIO/` stream packages.

### 🎮 Modern PC Controls & Modding
- **Raw Mouse Input**: Authentic PC first-person and third-person camera aiming with zero analog smoothing or deadzone distortion.
- **Controller Support**: Full support for Xbox (XInput) and PlayStation DualSense/DualShock 4 controllers with native button glyphs.
- **Built-in Mod Loader**: Drop mods into individual folders in `modloader/` with priority handling and live asset replacement.

---

## Requirements

### To Build
- **Windows**:
  - Windows 10 (version 1909+) or Windows 11
  - Visual Studio 2022 (Community or higher) with:
    - *Desktop development with C++*
    - *C++ Clang tools for Windows* (`clang-cl`)
    - *C++ CMake tools for Windows*
  - Python 3.10 or newer (standard library only)
- **Linux**:
  - 64-bit Linux distribution (Ubuntu 22.04+, Arch Linux, Fedora 38+, CachyOS)
  - Clang 18+ or GCC 13+
  - CMake 3.25+, Ninja, Vulkan SDK / headers, SDL2/SDL3 dev packages

### Game Files Needed (User-Supplied)
No copyrighted binaries or game data are included in this repository. You must provide files from your legally purchased copies:
1. **Android Game Binary & Base Data**: `libGame.so` (v2.4.379 ARM64) and `data_main.wad` from the Google Play Store Android release.
2. **iPad Definitive Assets**: `GTALcs.app` payload extracted from the iOS/iPad App Store package (`.ipa`).

---

## Quick Start

### 1. Check your environment
Run the doctor tool to verify your toolchain, memory, and game files:
```powershell
python tools/doctor.py
```

### 2. Extract and Convert iPad Assets
Process the iPad `.ipa` to generate high-resolution textures and audio packs:
```powershell
python tools/extract_ipa.py path/to/GTALcs.ipa
python tools/texture_convert.py
```

### 3. Build the Game
Compile the native executable:
```powershell
python tools/build.py
```

### 4. Run
Launch the game directly:
- **Windows**: `build\bin\lcs.exe`
- **Linux**: `./build/bin/lcs`

---

## Developer Commands

| Command | Action |
|---|---|
| `python tools/doctor.py` | Complete system, compiler, GPU, and asset diagnostic |
| `python tools/build.py` | Build debug/release binary with CMake & Ninja |
| `python tools/build.py --quick` | Fast incremental relink without full LTO |
| `python tools/build.py --opt lto` | Fully optimized release build with Link-Time Optimization |
| `python tools/extract_ipa.py [IPA]` | Extract models, audio, and textures from iPad release |
| `python tools/wad_tool.py unpack [WAD]` | Inspect and unpack Android `data_main.wad` archives |
| `python tools/texture_convert.py` | Batch convert PVRTC textures to PC-native DXT/BC7 |

---

## Repository Structure

```
gta-lcs-recomp/
├── CMakeLists.txt        # Top-level CMake project configuration
├── AGENTS.md             # Developer guidelines & coding standards
├── PROJECT_PLAN.md       # Full architecture & milestone documentation
├── README.md             # This document
│
├── src/
│   ├── core/             # Application entry point, memory & configuration
│   ├── cpu/              # Dynarmic ARM64 JIT execution harness
│   ├── os/               # Android Bionic libc/libm stubs & JNI bridge
│   ├── graphics/         # Direct3D 12 and Vulkan 1.3 renderers
│   ├── audio/            # Low-latency WASAPI / PipeWire audio engine
│   ├── vfs/              # LogicalFS asset redirection & Mod Loader
│   └── input/            # Mouse aiming, keyboard and gamepad controllers
│
├── tools/                # Python developer and asset extraction tools
│   ├── build.py
│   ├── doctor.py
│   ├── extract_ipa.py
│   ├── texture_convert.py
│   └── wad_tool.py
│
├── docs/                 # Detailed technical specifications
└── modloader/            # Directory for user-installed mods
```

---

## License

This compatibility runtime and toolchain are licensed under the **GNU General Public License v3.0 (GPL-3.0)**.  
Grand Theft Auto, Grand Theft Auto: Liberty City Stories, and all associated trademarks, assets, audio, and game code are the intellectual property of **Rockstar Games / Take-Two Interactive**. This project does not contain, distribute, or circumvent protection for any copyrighted game material.
