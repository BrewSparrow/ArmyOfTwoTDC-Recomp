# ao2dc - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ao2dc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/default.xex")
    message(FATAL_ERROR
        "assets/default.xex not found.\n"
        "Dump your Xbox 360 disc with ABGX360 (or extract your ISO with extract-xiso) and copy "
        "the extracted files into assets/, so that default.xex is at assets/default.xex. "
        "See README.md.")
endif()

include(cmake/apply_sdk_patches.cmake)
set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk")
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "" FORCE)

# Optional Title Update 1: the update package exactly as copied from the console (for example
# TU_12K22DB_000001C000000.000000000008B) in title_update/. The recompiler then loads each XEX with
# the update's patch (UPDATE:\<name>.xexp, read straight from the package) and uses the update's
# codegen hints; the game mounts the same package as UPDATE: at run time (src/ao2dc_app.h).
# generated/rexglue.cmake is rewritten by the recompiler, so the choice is made here: the manifest
# includes config/ao4_main_functions_active.toml (a copy of the disc or the update hints), and the
# package path reaches the recompiler as its update_data_root setting (REX_UPDATE_DATA_ROOT).
set(AO2DC_TU1_SHA256 "12a417208649711a53687ea33be0049409b965cae7cd82d9406ed2bd536f5675")
set(AO2DC_MAIN_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/config/ao4_main_functions.toml")
file(GLOB AO2DC_TU_FILES LIST_DIRECTORIES false "${CMAKE_CURRENT_SOURCE_DIR}/title_update/*")
list(FILTER AO2DC_TU_FILES EXCLUDE REGEX "\\.(md|txt)$")
list(LENGTH AO2DC_TU_FILES AO2DC_TU_COUNT)
if(AO2DC_TU_COUNT GREATER 1)
    message(FATAL_ERROR "title_update/ must hold exactly one title update package, found: ${AO2DC_TU_FILES}")
elseif(AO2DC_TU_COUNT EQUAL 1)
    file(SHA256 "${AO2DC_TU_FILES}" AO2DC_TU_HASH)
    if(NOT AO2DC_TU_HASH STREQUAL AO2DC_TU1_SHA256)
        message(FATAL_ERROR
            "Unsupported title update ${AO2DC_TU_FILES} (SHA-256 ${AO2DC_TU_HASH}).\n"
            "Only Title Update 1 (0.0.1.11, SHA-256 ${AO2DC_TU1_SHA256}) is supported: the "
            "recompiler needs hook addresses made for that exact update. Remove the file to build "
            "without the update.")
    endif()
    set(AO2DC_TITLE_UPDATE_PACKAGE "${AO2DC_TU_FILES}")
    set(AO2DC_MAIN_HINTS "${CMAKE_CURRENT_SOURCE_DIR}/config/ao4_main_functions_tu1.toml")
    # Prefixes the codegen command (the only custom command in this directory).
    set_property(DIRECTORY PROPERTY RULE_LAUNCH_CUSTOM
        "\"${CMAKE_COMMAND}\" -E env \"REX_UPDATE_DATA_ROOT=${AO2DC_TITLE_UPDATE_PACKAGE}\"")
    message(STATUS "Title Update 1: ${AO2DC_TITLE_UPDATE_PACKAGE}")
else()
    message(STATUS "Title update: none (title_update/ is empty)")
endif()
# Only rewritten when the content changes, so the recompiler reruns exactly when the choice does.
configure_file("${AO2DC_MAIN_HINTS}" "${CMAKE_CURRENT_SOURCE_DIR}/config/ao4_main_functions_active.toml" COPYONLY)

include(generated/rexglue.cmake)

# Built in-tree, the SDK writes its DLLs into its own source folder and they are only copied next
# to ao2dc.exe when the exe relinks. Build them next to the exe instead, so an SDK-only change
# (GPU plugin, runtime) is never left stale. The recompiler (rexglue) goes there too: it loads
# rexruntime.dll from its own folder, and otherwise Windows would pick up any other copy on PATH.
foreach(_ao2dc_sdk_dll rexruntime rexgpu-xenos rexglue)
    if(TARGET ${_ao2dc_sdk_dll})
        set_target_properties(${_ao2dc_sdk_dll} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    endif()
endforeach()

# Sources
set(AO2DC_SOURCES
    src/main.cpp
    src/launcher_hook.cpp
)

if(WIN32)
    add_executable(ao2dc WIN32 ${AO2DC_SOURCES})
else()
    add_executable(ao2dc ${AO2DC_SOURCES})
endif()

rexglue_setup_target(ao2dc GPU_PLUGINS xenos)

# Player-facing executable name (the CMake target stays ao2dc).
set_target_properties(ao2dc PROPERTIES OUTPUT_NAME "ArmyOfTwoTDC X360 Recomp")

# The SDK's public UI headers include <imgui.h>. An installed SDK ships it in its include folder;
# built in-tree, only the SDK's own UI library sees imgui's include path.
if(TARGET imgui)
    target_include_directories(ao2dc PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Hooks into AO4.Main's recompiled code live in its module target. With the Title Update 1
# package in title_update/, codegen recompiles the patched code and the hooks use its data
# addresses (AO2DC_TITLE_UPDATE).
if(TARGET ao2dc_AO4_Main AND AO2DC_TITLE_UPDATE_PACKAGE)
    target_compile_definitions(ao2dc_AO4_Main PRIVATE AO2DC_TITLE_UPDATE=1)
endif()
if(TARGET ao2dc_AO4_Main)
    target_sources(ao2dc_AO4_Main PRIVATE
        src/main_hooks/task_wait_hook.cpp
        src/main_hooks/stack_sampler.cpp
        src/main_hooks/frame_rate_hook.cpp
        src/main_hooks/xas_check_hook.cpp
        src/main_hooks/filter_recorder.cpp
        src/main_hooks/mix_writer_probe.cpp
        src/main_hooks/centre_recorder.cpp
        src/main_hooks/ping_capture.cpp)
endif()
