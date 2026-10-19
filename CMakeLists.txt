cmake_minimum_required(VERSION 3.20)
# Preserve the previous build's optimization level; callers can override it.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C compiler flags")
project(Tompouce VERSION 0.1.0 LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use a separate build directory: cmake -S . -B build -G Ninja")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WITH_CCACHE "Use ccache when available" ON)
if(WITH_CCACHE AND NOT DEFINED CMAKE_C_COMPILER_LAUNCHER)
    find_program(CCACHE_EXECUTABLE ccache)
    if(CCACHE_EXECUTABLE)
        set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}")
        message(STATUS "C compiler cache: ${CCACHE_EXECUTABLE}")
    endif()
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_program(XXD_EXECUTABLE xxd REQUIRED)
pkg_check_modules(REQUIRED_DEPS REQUIRED IMPORTED_TARGET
    notcurses-core>=3.0.9 portaudio-2.0 libcurl libcjson alsa)
find_library(LEVELDB_LIBRARY NAMES leveldb REQUIRED)
find_path(LEVELDB_INCLUDE_DIR leveldb/c.h REQUIRED)

# A changed prefix must invalidate find_* results from the previous configure.
function(tompouce_dependency_prefix name prefix)
    if(NOT DEFINED TOMPOUCE_PREVIOUS_${name}_PREFIX OR
       NOT "${prefix}" STREQUAL "${TOMPOUCE_PREVIOUS_${name}_PREFIX}")
        unset(${name}_INCLUDE_DIR CACHE)
        unset(${name}_LIBRARY CACHE)
    endif()
    set(TOMPOUCE_PREVIOUS_${name}_PREFIX "${prefix}" CACHE INTERNAL "Previous ${name} prefix" FORCE)
endfunction()

set(TREE_SITTER_PREFIX "" CACHE PATH "Private tree-sitter installation (0.25 or newer)")
tompouce_dependency_prefix(TREE_SITTER "${TREE_SITTER_PREFIX}")
if(TREE_SITTER_PREFIX)
    find_path(TREE_SITTER_INCLUDE_DIR tree_sitter/api.h PATHS "${TREE_SITTER_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
    find_library(TREE_SITTER_LIBRARY tree-sitter PATHS "${TREE_SITTER_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    add_library(tompouce_tree_sitter INTERFACE)
    target_include_directories(tompouce_tree_sitter SYSTEM INTERFACE "${TREE_SITTER_INCLUDE_DIR}")
    target_link_libraries(tompouce_tree_sitter INTERFACE "${TREE_SITTER_LIBRARY}")
else()
    pkg_check_modules(TREE_SITTER REQUIRED IMPORTED_TARGET tree-sitter>=0.25)
    add_library(tompouce_tree_sitter ALIAS PkgConfig::TREE_SITTER)
endif()

add_library(tompouce_options INTERFACE)
target_compile_options(tompouce_options INTERFACE -Wall -Wextra -Werror -pedantic)
target_include_directories(tompouce_options INTERFACE include thirdparty "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(tompouce_options SYSTEM INTERFACE thirdparty/quickjs)
target_include_directories(tompouce_options SYSTEM INTERFACE "${LEVELDB_INCLUDE_DIR}")
target_link_libraries(tompouce_options INTERFACE PkgConfig::REQUIRED_DEPS
    tompouce_tree_sitter Threads::Threads "${LEVELDB_LIBRARY}" m)

# Missing optional packages disable their backend by default. An explicit ON
# requires the package; OFF never probes it. Keep adapter flags local.
function(tompouce_optional name package source)
    if(NOT DEFINED WITH_${name})
        pkg_check_modules(${name} QUIET "${package}")
        set(WITH_${name} "${${name}_FOUND}" CACHE BOOL "Enable ${name} support")
    endif()
    if(WITH_${name})
        pkg_check_modules(${name} REQUIRED "${package}")
        target_compile_definitions(tompouce_options INTERFACE HAVE_${name})
        target_link_libraries(tompouce_options INTERFACE ${${name}_LINK_LIBRARIES})
        target_link_directories(tompouce_options INTERFACE ${${name}_LIBRARY_DIRS})
        target_link_options(tompouce_options INTERFACE ${${name}_LDFLAGS_OTHER})
        set(flags ${${name}_CFLAGS_OTHER})
        foreach(directory IN LISTS ${name}_INCLUDE_DIRS)
            list(APPEND flags -isystem "${directory}")
        endforeach()
        set_property(SOURCE ${source} APPEND PROPERTY COMPILE_OPTIONS ${flags})
        if(name STREQUAL "PIPEWIRE")
            set_property(SOURCE tests/test_audio_output.c APPEND PROPERTY COMPILE_OPTIONS ${flags})
        endif()
    endif()
    message(STATUS "${name}: ${WITH_${name}}")
endfunction()
tompouce_optional(PIPEWIRE "libpipewire-0.3>=0.3.7" src/audio_output.c)
tompouce_optional(PULSE libpulse src/audio_output.c)
tompouce_optional(SAMPLERATE samplerate src/audio_output.c)
tompouce_optional(FLUIDSYNTH "fluidsynth>=2.2" src/gm_synth.c)

option(WITH_CSOUND "Enable Csound when available" ON)
set(default_csound "")
foreach(candidate "$ENV{HOME}/.local/opt/tompouce-csound7" "$ENV{HOME}/.local/opt/strudelterm-csound7")
    if(NOT default_csound AND EXISTS "${candidate}/include/csound/csound.h")
        set(default_csound "${candidate}")
    endif()
endforeach()
set(CSOUND_PREFIX "${default_csound}" CACHE PATH "Private Csound installation")
tompouce_dependency_prefix(CSOUND "${CSOUND_PREFIX}")
if(WITH_CSOUND)
    if(CSOUND_PREFIX)
        find_path(CSOUND_INCLUDE_DIR csound/csound.h PATHS "${CSOUND_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
        find_library(CSOUND_LIBRARY csound64 PATHS "${CSOUND_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    else()
        find_path(CSOUND_INCLUDE_DIR csound/csound.h)
        find_library(CSOUND_LIBRARY csound64)
    endif()
    if(CSOUND_INCLUDE_DIR AND CSOUND_LIBRARY)
        target_compile_definitions(tompouce_options INTERFACE HAVE_CSOUND)
        file(STRINGS "${CSOUND_INCLUDE_DIR}/csound/version.h" csound_version REGEX "^#define[ \t]+CS_VERSION[ \t]+7([ \t]|$)")
        if(csound_version)
            target_compile_definitions(tompouce_options INTERFACE HAVE_CSOUND7)
            if(CSOUND_PREFIX)
                target_compile_definitions(tompouce_options INTERFACE CSOUND7_OPCODEDIR="${CSOUND_PREFIX}/plugins")
            endif()
        endif()
        target_include_directories(tompouce_options SYSTEM INTERFACE "${CSOUND_INCLUDE_DIR}")
        target_link_libraries(tompouce_options INTERFACE "${CSOUND_LIBRARY}")
        # The installer relocates the executable but keeps private libraries here.
        get_filename_component(csound_libdir "${CSOUND_LIBRARY}" DIRECTORY)
        list(APPEND CMAKE_BUILD_RPATH "${csound_libdir}")
        list(APPEND CMAKE_INSTALL_RPATH "${csound_libdir}")
        message(STATUS "Csound: ${CSOUND_LIBRARY}")
    else()
        message(STATUS "Csound: unavailable (native audio remains enabled)")
    endif()
endif()
if(TREE_SITTER_PREFIX)
    get_filename_component(tree_libdir "${TREE_SITTER_LIBRARY}" DIRECTORY)
    list(APPEND CMAKE_BUILD_RPATH "${tree_libdir}")
    list(APPEND CMAKE_INSTALL_RPATH "${tree_libdir}")
endif()

function(tompouce_embed input output symbol)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${output}"
        COMMAND "${CMAKE_COMMAND}" -DXXD=${XXD_EXECUTABLE}
            -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/${input}
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/${output}
            -DSYMBOL=${symbol} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake"
        DEPENDS "${input}" cmake/Embed.cmake VERBATIM)
endfunction()
tompouce_embed(js/dist/runtime.js runtime.inc strudel_js_bundle)
tompouce_embed(thirdparty/strudel-csound/presets.orc presets.inc strudel_csound_presets)
add_custom_target(embedded_resources DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/generated/runtime.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/presets.inc")

add_library(quickjs STATIC thirdparty/quickjs/quickjs.c thirdparty/quickjs/cutils.c
    thirdparty/quickjs/libregexp.c thirdparty/quickjs/libunicode.c thirdparty/quickjs/dtoa.c)
target_compile_options(quickjs PRIVATE -w -fwrapv)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
add_library(tree_sitter_javascript STATIC thirdparty/tree-sitter-javascript/src/parser.c
    thirdparty/tree-sitter-javascript/src/scanner.c)
target_include_directories(tree_sitter_javascript PRIVATE thirdparty/tree-sitter-javascript/src)
target_compile_options(tree_sitter_javascript PRIVATE -w -fwrapv)

add_library(tompouce_core STATIC
    src/common.c src/audio.c src/slider.c
    src/track.c src/scheduler.c src/ai.c src/tui.c src/mcp.c src/wavetable.c
    src/banks.c src/sampler.c src/websamp.c src/midi.c src/songfile.c src/javascript.c
    src/js_audio.c src/dsp.c src/csound_host.c src/convolution.c src/companion.c
    src/sounds.c src/sound_store.c src/terminal_graphics.c src/visual.c src/visual_render.c
    src/audio_output.c src/native_synth.c src/osc_output.c src/reverb_impulse.c src/duck_onset.c
    src/stretch.c src/gm_synth.c)
add_dependencies(tompouce_core embedded_resources)
target_link_libraries(tompouce_core PUBLIC tompouce_options quickjs tree_sitter_javascript)
add_executable(tompouce src/main.c)
target_link_libraries(tompouce PRIVATE tompouce_core)

# Helpers and UI assets are located relative to /proc/self/exe. Keep that layout
# in each build tree without copying credentials, user state or sample banks.
set(runtime_directories "${CMAKE_CURRENT_BINARY_DIR}")
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    list(APPEND runtime_directories "${CMAKE_CURRENT_BINARY_DIR}/${config}")
endforeach()
foreach(runtime_directory IN LISTS runtime_directories)
    file(MAKE_DIRECTORY "${runtime_directory}")
    configure_file(strudeltutor.json "${runtime_directory}/examples/strudeltutor.json" COPYONLY)
    foreach(directory scripts web assets)
        file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/${directory}"
            "${runtime_directory}/${directory}" SYMBOLIC)
    endforeach()
endforeach()

# Installer's offline verification is available even with BUILD_TESTING=OFF.
add_executable(test_install_runtime EXCLUDE_FROM_ALL tests/test_install_runtime.c)
set_target_properties(test_install_runtime PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
target_compile_options(test_install_runtime PRIVATE -UNDEBUG)
target_link_libraries(test_install_runtime PRIVATE tompouce_core)
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    include(cmake/Tests.cmake)
endif()

# Bundle rebuilding is explicit; ordinary builds use the committed JS bundles.
foreach(bundle js companion)
    if(bundle STREQUAL "js")
        set(bundle_directory js)
    else()
        set(bundle_directory web)
    endif()
    add_custom_target(${bundle}-bundle
        COMMAND npm ci --ignore-scripts --no-audit --no-fund
        COMMAND npm run build WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${bundle_directory}"
        USES_TERMINAL VERBATIM)
endforeach()
