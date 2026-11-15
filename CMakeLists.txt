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
    src/input/gamepad.cpp

    # Graphics abstraction
    src/graphics/render_bridge.cpp
    src/graphics/window.cpp
    src/graphics/shader_compiler.cpp
)

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

message(STATUS "GTA: Liberty City Stories Native PC Port configured.")
message(STATUS "  Platform:     ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")
message(STATUS "  Direct3D 12:  ${ENABLE_D3D12}")
message(STATUS "  Vulkan 1.4:   ${ENABLE_VULKAN}")
