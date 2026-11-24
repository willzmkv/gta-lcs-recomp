cmake_minimum_required(VERSION 3.25)

project(gta-lcs-recomp
    VERSION 0.1.0
    DESCRIPTION "Native PC port of GTA Liberty City Stories with Dynarmic ARM64 JIT, D3D12/Vulkan 1.4, and iPad Definitive Assets"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Platform detection
if(WIN32)
    set(LCS_PLATFORM_WINDOWS ON)
    add_compile_definitions(LCS_PLATFORM_WINDOWS=1)
elseif(UNIX AND NOT APPLE)
    set(LCS_PLATFORM_LINUX ON)
    add_compile_definitions(LCS_PLATFORM_LINUX=1)
else()
    message(FATAL_ERROR "Unsupported target platform. Only Windows and Linux are supported.")
endif()

# Configuration options
option(ENABLE_D3D12 "Enable Direct3D 12 rendering backend (Windows only)" ON)
option(ENABLE_VULKAN "Enable Vulkan 1.4 rendering backend" ON)
option(ENABLE_LTO "Enable Link-Time Optimization (LTO) for release builds" OFF)
option(ENABLE_TESTS "Build internal test harness" OFF)

if(ENABLE_D3D12 AND NOT WIN32)
    set(ENABLE_D3D12 OFF)
    message(STATUS "Disabling Direct3D 12 on non-Windows platform.")
endif()

# Compiler options
if(MSVC)
    add_compile_options(/W4 /WX- /wd4100 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Source files
set(SOURCES
    # Core
    src/core/main.cpp
    src/core/config.cpp
    src/core/memory.cpp
    src/core/logging.cpp

    # CPU & Dynarmic
    src/cpu/dynarmic_core.cpp
    src/cpu/elf_loader.cpp
    src/cpu/callbacks.cpp

    # OS & Bionic
    src/os/bionic_libc.cpp
    src/os/bionic_libm.cpp
    src/os/jni_bridge.cpp
    src/os/thread_manager.cpp

    # Audio
    src/audio/audio_system.cpp
    src/audio/openal_bridge.cpp

    # VFS & Mod Loader
    src/vfs/logical_fs.cpp
    src/vfs/wad_archive.cpp
    src/vfs/mod_loader.cpp

    # Input
    src/input/input_system.cpp
    src/input/mouse_aim.cpp
    src/input/pc_controls.cpp
    src/input/gamepad.cpp

    # Graphics abstraction
    src/graphics/render_bridge.cpp
    src/graphics/window.cpp
    src/graphics/shader_compiler.cpp
    src/graphics/texture_pipeline.cpp
    src/graphics/video_player.cpp

    # Game world enhancements, PS2 Effects Pipeline & Trainer
    src/game/lcs_world_hd.cpp
    src/game/lcs_popin_probe.cpp
    src/game/lcs_batched_fast.cpp
    src/game/lcs_distance.cpp
    src/game/lcs_memory.cpp
    src/game/lcs_ps2_effects.cpp
    src/game/lcs_physics_timing.cpp
    src/game/lcs_stochastic.cpp
    src/game/lcs_dialogue_speakers.cpp
    src/game/lcs_subtitles.cpp
    src/ui/ui_loc.cpp
    src/game/lcs_free_aim.cpp
    src/game/lcs_prompts.cpp
    src/game/lcs_trainer.cpp

    # Settings & Unified GTA 6-style Global In-Game Menu
    src/settings/settings.cpp
    src/ui/ui_theme.cpp
    src/ui/global_menu.cpp
    src/ui/imgui_overlay.cpp
    src/ui/diag_overlay.cpp

    # Dear ImGui core + backends
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)

if(WIN32)
    list(APPEND SOURCES
        third_party/imgui/backends/imgui_impl_win32.cpp
        third_party/imgui/backends/imgui_impl_dx12.cpp
    )
endif()

# Renderers
if(ENABLE_D3D12)
    list(APPEND SOURCES
        src/graphics/r_d3d12/d3d12_backend.cpp
        src/graphics/r_d3d12/d3d12_pipeline.cpp
        src/graphics/r_d3d12/d3d12_resources.cpp
    )
    add_compile_definitions(LCS_ENABLE_D3D12=1)
endif()

if(ENABLE_VULKAN)
    list(APPEND SOURCES
        src/graphics/r_vulkan/vulkan_backend.cpp
        src/graphics/r_vulkan/vulkan_pipeline.cpp
        src/graphics/r_vulkan/vulkan_resources.cpp
    )
    add_compile_definitions(LCS_ENABLE_VULKAN=1)
endif()

# Executable target
add_executable(lcs ${SOURCES})

target_include_directories(lcs PRIVATE
    src
    src/core
    src/cpu
    src/graphics
    src/os
    src/audio
    src/vfs
    src/input
    third_party
    third_party/imgui
    third_party/imgui/backends
)

# System libraries
if(WIN32)
    target_link_libraries(lcs PRIVATE
        ntdll
        user32
        gdi32
        shell32
        ole32
        uuid
        dxgi
        mfplat
        mfreadwrite
        mfuuid
    )
    if(ENABLE_D3D12)
        target_link_libraries(lcs PRIVATE d3d12 d3dcompiler)
    endif()
elseif(UNIX)
    target_link_libraries(lcs PRIVATE
        pthread
        dl
        m
    )
endif()

# SDL3 gamepad, DualSense & audio layer (vendored in third_party/SDL3, see VENDORED.md),
# built as a static library from this tree.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL3 EXCLUDE_FROM_ALL)
target_link_libraries(lcs PRIVATE SDL3::SDL3-static)
add_compile_definitions(LCS_HAS_SDL3=1)

# Dynarmic ARM64 JIT (vendored in third_party/dynarmic, see VENDORED.md).
# Its bundled externals (fmt, mcl, xbyak, zydis, robin-map) and a header-only Boost
# subset (third_party/boost) are all built from this tree: no paths outside the repo.
set(Boost_DIR "${CMAKE_SOURCE_DIR}/third_party/boost/cmake" CACHE PATH "Vendored header-only Boost" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_IGNORE_ASSERTS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS ON CACHE BOOL "" FORCE)
set(_lcs_saved_skip_install ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)
set(CMAKE_SKIP_INSTALL_RULES ${_lcs_saved_skip_install})
target_link_libraries(lcs PRIVATE dynarmic)
add_compile_definitions(LCS_HAS_DYNARMIC=1)

# Link-time optimization for the game executable (build.py --opt lto, the default)
if(ENABLE_LTO)
    set_property(TARGET lcs PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "  LTO:          ON (lcs)")
endif()

# Deploy executable to project root for direct execution
add_custom_command(TARGET lcs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:lcs>
    "${CMAKE_SOURCE_DIR}/lcs.exe"
)

message(STATUS "GTA: Liberty City Stories Native PC Port configured.")
message(STATUS "  Platform:     ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Direct3D 12:  ${ENABLE_D3D12}")
message(STATUS "  Vulkan 1.4:   ${ENABLE_VULKAN}")
