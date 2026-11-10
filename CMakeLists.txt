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

include(generated/rexglue.cmake)

# Built in-tree, the SDK writes its DLLs into its own source folder and they are only copied next
# to ao2dc.exe when the exe relinks. Build them next to the exe instead, so an SDK-only change
# (GPU plugin, runtime) is never left stale.
foreach(_ao2dc_sdk_dll rexruntime rexgpu-xenos)
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

# Hooks into AO4.Main's recompiled code live in its module target.
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
