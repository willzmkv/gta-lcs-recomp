# LCSLauncher: Native launcher for GTA: Liberty City Stories (PC)
# Builds standalone executable LCSLauncher(.exe) in the repository root:
#   python tools/build.py --launcher
cmake_minimum_required(VERSION 3.21)
project(lcs_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LCS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(LCS_LAUNCHER_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${LCS_LAUNCHER_SHADERS}")
set(LCS_UI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/render/d3d11/ui.hlsl")

set(LCS_LAUNCHER_COMMON
    main.cpp
    core/png.cpp
    render/draw.cpp
    render/font.cpp
    render/icons.cpp
    ui/ui.cpp
    ui/widgets.cpp
    screens/screens.cpp
    screens/backdrop.cpp
    screens/titlebar.cpp
    screens/setup.cpp
    screens/home.cpp
    screens/task.cpp
    screens/sheets.cpp
    screens/overlays.cpp
    screens/gallery.cpp
    screens/gamemenu.cpp
    model/tasks.cpp
    model/game_config.cpp
    model/hardware.cpp
    model/scskiller.cpp
    model/sound.cpp
    model/process.cpp
    model/game_files_check.cpp
    model/json.cpp
)

if(WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    foreach(stage vs ps)
        add_custom_command(
            OUTPUT "${LCS_LAUNCHER_SHADERS}/ui_${stage}.h"
            COMMAND "${Python3_EXECUTABLE}" -I "${LCS_ROOT}/tools/hlsl_dxbc.py" --profile ${stage}_4_0 --entry ${stage}_main
                    --name g_ui_${stage} --out "${LCS_LAUNCHER_SHADERS}/ui_${stage}.h" "${LCS_UI_HLSL}"
            DEPENDS "${LCS_UI_HLSL}" "${LCS_ROOT}/tools/hlsl_dxbc.py"
            COMMENT "hlsl_dxbc ui.hlsl ${stage}_main"
            VERBATIM)
    endforeach()

    add_executable(LCSLauncher WIN32
        ${LCS_LAUNCHER_COMMON}
        platform/platform_win32.cpp
        render/font_dwrite.cpp
        render/d3d11/renderer_d3d11.cpp
        model/process_win32.cpp
        model/system_win32.cpp
        "${LCS_LAUNCHER_SHADERS}/ui_vs.h"
        "${LCS_LAUNCHER_SHADERS}/ui_ps.h"
        launcher.rc
    )
    set_source_files_properties(launcher.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/launcher.ico")
    target_link_libraries(LCSLauncher PRIVATE d3d11 dxgi dwrite dwmapi ole32 shell32 uuid winmm advapi32 d3dcompiler)
    target_compile_definitions(LCSLauncher PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(MINGW)
        target_compile_definitions(LCSLauncher PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    pkg_check_modules(LCS_LAUNCHER_LIBS REQUIRED IMPORTED_TARGET
        x11 xcursor xrandr wayland-client wayland-cursor xkbcommon freetype2 fontconfig dbus-1)
    set(LCS_WAYLAND_PROTOCOLS_DIR "" CACHE PATH "wayland-protocols' XML folder")
    if(NOT LCS_WAYLAND_PROTOCOLS_DIR)
        pkg_check_modules(LCS_WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
        pkg_get_variable(LCS_WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    endif()
    find_program(LCS_WAYLAND_SCANNER wayland-scanner REQUIRED)
    find_path(LCS_VULKAN_INCLUDE vulkan/vulkan_core.h HINTS "$ENV{VULKAN_SDK}/include")
    find_program(LCS_GLSLANG NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/bin")

    foreach(stage vs ps)
        if(stage STREQUAL "vs")
            set(_glsl_stage vert)
        else()
            set(_glsl_stage frag)
        endif()
        add_custom_command(
            OUTPUT "${LCS_LAUNCHER_SHADERS}/ui_${stage}.spv.h"
            COMMAND "${Python3_EXECUTABLE}" -I "${LCS_ROOT}/tools/hlsl_spirv.py" --glslang "${LCS_GLSLANG}"
                    --target-env vulkan1.1 --stage ${_glsl_stage} --entry ${stage}_main --name g_ui_${stage}_spv
                    --out "${LCS_LAUNCHER_SHADERS}/ui_${stage}.spv.h" "${LCS_UI_HLSL}"
            DEPENDS "${LCS_UI_HLSL}" "${LCS_ROOT}/tools/hlsl_spirv.py"
            COMMENT "glslang ui.hlsl ${stage}_main"
            VERBATIM)
    endforeach()

    add_custom_command(
        OUTPUT "${LCS_LAUNCHER_SHADERS}/launcher_icon.h"
        COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/make_icon.py" --header "${LCS_LAUNCHER_SHADERS}/launcher_icon.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/make_icon.py"
        COMMENT "make_icon.py --header"
        VERBATIM)

    add_executable(LCSLauncher
        ${LCS_LAUNCHER_COMMON}
        platform/platform_linux.cpp
        platform/linux/x11.cpp
        platform/linux/wayland.cpp
        platform/linux/dbus.cpp
        render/font_freetype.cpp
        render/vulkan/renderer_vulkan.cpp
        model/process_posix.cpp
        model/system_linux.cpp
        "${LCS_LAUNCHER_SHADERS}/ui_vs.spv.h"
        "${LCS_LAUNCHER_SHADERS}/ui_ps.spv.h"
        "${LCS_LAUNCHER_SHADERS}/launcher_icon.h"
    )
    target_include_directories(LCSLauncher PRIVATE "${LCS_VULKAN_INCLUDE}")
    target_link_libraries(LCSLauncher PRIVATE PkgConfig::LCS_LAUNCHER_LIBS ${CMAKE_DL_LIBS})
    target_compile_options(LCSLauncher PRIVATE -Wall -Wextra -Wno-missing-field-initializers -fvisibility=hidden)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LCS_ROOT}/VERSION")
file(STRINGS "${LCS_ROOT}/VERSION" LCS_VERSION LIMIT_COUNT 1)
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _lcs_version_numbers "${LCS_VERSION}")
if(NOT _lcs_version_numbers)
    set(LCS_VERSION "1.0.0")
    set(CMAKE_MATCH_1 1)
    set(CMAKE_MATCH_2 0)
    set(CMAKE_MATCH_3 0)
endif()
target_compile_definitions(LCSLauncher PRIVATE
    VCS_VERSION_MAJOR=${CMAKE_MATCH_1}
    VCS_VERSION_MINOR=${CMAKE_MATCH_2}
    VCS_VERSION_PATCH=${CMAKE_MATCH_3})
file(CONFIGURE OUTPUT "${LCS_LAUNCHER_SHADERS}/launcher_version.h" CONTENT "#define VCS_VERSION_TEXT \"${LCS_VERSION}\"\n")

target_include_directories(LCSLauncher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${LCS_ROOT}/src" "${LCS_LAUNCHER_SHADERS}")
set_target_properties(LCSLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${LCS_ROOT}")
if(MSVC)
    set_property(TARGET LCSLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_options(LCSLauncher PRIVATE /W3 /utf-8)
elseif(MINGW)
    target_compile_options(LCSLauncher PRIVATE -Wall -Wno-missing-field-initializers -Wno-unknown-pragmas)
    target_link_options(LCSLauncher PRIVATE -municode -static $<$<CONFIG:Release>:-s>)
endif()
