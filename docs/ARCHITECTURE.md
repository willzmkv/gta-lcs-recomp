# Technical Architecture — GTA: Liberty City Stories Native PC Port

## 1. Overview
This project provides a native desktop compatibility runtime for the ARM64 Android release of *Grand Theft Auto: Liberty City Stories* (v2.4.379), enhanced with high-resolution assets from the iOS/iPad 1.14 release.

## 2. CPU Execution Model (Dynarmic JIT)
Rather than emulating a complete mobile platform, the runtime behaves as a lightweight execution harness:
- **Guest Binary:** `libGame.so` (ELF64 AArch64).
- **CPU Translation:** Dynarmic JIT compiles AArch64 basic blocks directly into host x86_64 instructions with minimal runtime overhead.
- **Fast Callbacks:** Import relocations (`R_AARCH64_JUMP_SLOT`, `R_AARCH64_GLOB_DAT`) within `libGame.so` point to custom trap trampolines. When hit, Dynarmic returns execution to host C++ functions immediately.

## 3. Render Pipeline (Direct3D 12 & Vulkan)
- **Direct3D 12:**
  - Designed for low-latency Windows 10/11 execution.
  - Implements Root Signatures mapping to standard RenderWare Leeds pipeline stages (World, Skin, Particle, Billboard).
  - Explicit pipeline state object (PSO) caches.
- **Vulkan 1.3:**
  - Modern Vulkan architecture with dynamic rendering (`VK_KHR_dynamic_rendering`).
  - Native to Linux and available as an alternative on Windows.

## 4. LogicalFS Virtual File System
`libGame.so` accesses files through its Leeds/War Drum `LogicalFS` subsystem:
- `LogicalFS_OpenBundleFile(const char* path, int mode)`
- `LogicalFS_AddBundleRoot(const char* path)`
- `LogicalFS_MountWadFromPhysicalFilePath(...)`

Our custom VFS intercepts these calls, routing lookups in this priority order:
1. `modloader/` (active user mods)
2. `assets/ipad/` (converted Retina textures and high-poly models)
3. `data_main.wad` (original baseline Android container)
