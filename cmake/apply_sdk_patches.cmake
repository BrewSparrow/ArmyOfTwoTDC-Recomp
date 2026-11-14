# Applies thirdparty/patches/*.patch to the ReXGlue SDK submodule at configure time.
#
# Idempotent: a patch that already reverse-applies cleanly is treated as applied, so re-running
# configure (or configuring after a manual apply) is harmless. The submodule's commit is never
# changed; the patches only touch its working tree.
#
# Also replaces the placeholder files that Windows checkouts without symlink support leave for
# libmspack's symlinked sources (each holds only the link target as text).

set(AO2DC_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/rexglue-sdk")
set(AO2DC_PATCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/patches")

if(NOT EXISTS "${AO2DC_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "ReXGlue SDK submodule is missing. Run:\n"
        "  git submodule update --init --recursive")
endif()

find_package(Git REQUIRED)

file(GLOB AO2DC_SDK_PATCHES "${AO2DC_PATCH_DIR}/*.patch")
list(SORT AO2DC_SDK_PATCHES)

# A later patch can change lines next to an earlier patch's hunks (0010 does, next to 0009). Once
# the whole set is applied, that earlier patch no longer reverse-applies on its own. So a stamp
# holds the hash of the applied set, and later configure runs skip the set instead of
# re-applying it.
set(AO2DC_PATCH_SET_HASH "")
foreach(patch IN LISTS AO2DC_SDK_PATCHES)
    get_filename_component(patch_name "${patch}" NAME)
    file(SHA256 "${patch}" patch_hash)
    string(APPEND AO2DC_PATCH_SET_HASH "${patch_name}:${patch_hash};")
endforeach()
string(SHA256 AO2DC_PATCH_SET_HASH "${AO2DC_PATCH_SET_HASH}")
set(AO2DC_PATCH_STAMP "${AO2DC_SDK_DIR}/.ao2dc_patches_applied")
set(AO2DC_PATCH_STAMP_HASH "")
if(EXISTS "${AO2DC_PATCH_STAMP}")
    file(READ "${AO2DC_PATCH_STAMP}" AO2DC_PATCH_STAMP_HASH)
    string(STRIP "${AO2DC_PATCH_STAMP_HASH}" AO2DC_PATCH_STAMP_HASH)
endif()
if(AO2DC_PATCH_STAMP_HASH STREQUAL AO2DC_PATCH_SET_HASH)
    message(STATUS "SDK patches already applied (stamp matches this patch set)")
    set(AO2DC_SDK_PATCHES "")
endif()

foreach(patch IN LISTS AO2DC_SDK_PATCHES)
    get_filename_component(patch_name "${patch}" NAME)
    set(already_applied 1)
    if(patch_name STREQUAL "0006-audio-underrun-fade-and-diagnostics.patch")
        file(READ "${AO2DC_SDK_DIR}/src/audio/xma_context.cpp" ao2dc_audio_patch_marker)
        string(FIND "${ao2dc_audio_patch_marker}" "Ao2dcXmaPeakCheck" ao2dc_audio_patch_marker_offset)
        if(NOT ao2dc_audio_patch_marker_offset EQUAL -1)
            set(already_applied 0)
        endif()
    endif()
    if(already_applied EQUAL 1)
        execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --ignore-whitespace "${patch}"
        WORKING_DIRECTORY "${AO2DC_SDK_DIR}"
        RESULT_VARIABLE already_applied
        OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(already_applied EQUAL 0)
        message(STATUS "SDK patch already applied: ${patch_name}")
        continue()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch}"
        WORKING_DIRECTORY "${AO2DC_SDK_DIR}"
        RESULT_VARIABLE apply_result
        ERROR_VARIABLE apply_error)
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply SDK patch ${patch_name}:\n${apply_error}\n"
            "If thirdparty/rexglue-sdk was patched by an older version of this project, restore it "
            "(git -C thirdparty/rexglue-sdk checkout . , or extract the kit's SDK folder again), "
            "then configure again.")
    endif()
    message(STATUS "Applied SDK patch: ${patch_name}")
endforeach()
if(AO2DC_SDK_PATCHES)
    file(WRITE "${AO2DC_PATCH_STAMP}" "${AO2DC_PATCH_SET_HASH}\n")
endif()

# libmspack symlink placeholders (Windows without core.symlinks).
set(AO2DC_MSPACK_DIR "${AO2DC_SDK_DIR}/thirdparty/libmspack/cabextract/mspack")
if(EXISTS "${AO2DC_MSPACK_DIR}")
    file(GLOB mspack_files "${AO2DC_MSPACK_DIR}/*")
    foreach(placeholder IN LISTS mspack_files)
        file(SIZE "${placeholder}" placeholder_size)
        if(placeholder_size GREATER 200)
            continue()
        endif()
        file(READ "${placeholder}" link_target)
        string(STRIP "${link_target}" link_target)
        if(NOT link_target MATCHES "^\\.\\.?/")
            continue()
        endif()
        get_filename_component(target_path "${AO2DC_MSPACK_DIR}/${link_target}" ABSOLUTE)
        if(EXISTS "${target_path}" AND NOT IS_DIRECTORY "${target_path}")
            configure_file("${target_path}" "${placeholder}" COPYONLY)
            message(STATUS "Replaced libmspack symlink placeholder: ${placeholder}")
        endif()
    endforeach()
endif()
