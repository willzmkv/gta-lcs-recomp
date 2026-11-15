# Developer & Agent Guidelines — GTA: Liberty City Stories Native PC Port

Welcome to **gta-lcs-recomp**. This document defines the engineering conventions, architecture invariants, and tooling workflows for agents and developers working in this codebase.

---

## 1. Core Architecture Invariants

1. **Rendering Backends:**
   - **Direct3D 12** on Windows (`src/graphics/r_d3d12/`).
   - **Vulkan 1.4** on Linux & Windows (`src/graphics/r_vulkan/`).
   - **No legacy OpenGL / OpenGL ES:** Do NOT introduce OpenGL or EGL contexts into the PC runtime. All mobile GLES calls must route through the modern abstract render bridge (`src/graphics/render_bridge.cpp`).

2. **CPU Execution:**
   - Powered by **Dynarmic ARM64 JIT** (`src/cpu/`).
   - The Android `libGame.so` must be loaded as an unmodified ELF64 image in virtual memory.
   - External library calls (libc, libm, OpenAL, GLES) are hooked via Dynarmic SVC traps and redirected to native C++ implementations.
   - Do NOT attempt to emulate an entire Android OS. Keep the environment minimalist: Bionic libc/libm stubs + JNI method callbacks.

3. **Asset Handling:**
   - The user supplies legal copies of the Android game (`base.apk` / `libGame.so` / `data_main.wad`) and the iPad release (`GTALcs.ipa`).
   - **Never commit game assets or binaries into git.**
   - All asset substitutions must pass through the `LogicalFS` asset router (`src/vfs/`).

4. **Modding & Overrides:**
   - The `modloader/` directory operates with folder-based priority, matching the behavior of `gta-vcs-recomp`.
   - Textures placed in `modloader/<mod_name>/` override base game textures on the fly.

---

## 2. Directory Layout

- `src/core/`: Executable entry point, configuration parsing, memory management.
- `src/cpu/`: Dynarmic ARM64 CPU harness, ELF64 loader, memory mapping.
- `src/os/`: Android Bionic libc/libm stubs, thread scheduler, JNI bridge.
- `src/graphics/`: D3D12 and Vulkan rendering engines, shader translation, window management.
- `src/audio/`: Low-latency audio mixer (WASAPI / PipeWire) with OpenAL Soft bridge.
- `src/vfs/`: Virtual file system, LogicalFS hooks, WAD parser, Mod Loader.
- `src/input/`: Mouse camera controller, keyboard input, XInput and DualSense drivers.
- `tools/`: Python 3 scripts for building, environment checks, and asset processing.
- `docs/`: Technical specifications and architectural deep dives.

---

## 3. Tooling Conventions

- All tools in `tools/` must be pure Python 3 scripts requiring only the Python standard library whenever possible.
- `python tools/doctor.py`: Runs a non-destructive check of compilers, memory, CPU instruction sets (AVX2/AVX/SSE4), and available game files.
- `python tools/build.py`: Manages CMake and Ninja builds cleanly without requiring manual CMake command invocations.
- Always run `python tools/doctor.py` before and after major environment or asset changes.

---

## 4. Coding Standards

- **Language:** C++20 standard (`std::span`, `std::format`, concepts, designated initializers).
- **Style:** Clean, readable, well-commented code. Prefer explicit types for binary structures (`uint32_t`, `uint64_t`).
- **Memory Safety:** Use RAII for all host OS resources (handles, memory mappings, GPU descriptors).
- **Error Handling:** Fail gracefully with descriptive error logs rather than silent crashes.
