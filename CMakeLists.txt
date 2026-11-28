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
option(WITH_LTO "Use link-time optimization for Release when supported" ON)
if(WITH_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT TOMPOUCE_IPO_SUPPORTED LANGUAGES C)
    if(TOMPOUCE_IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    endif()
endif()

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
pkg_check_modules(LILV QUIET IMPORTED_TARGET lilv-0)
if(LILV_FOUND)
    set(TOMPOUCE_HAVE_LV2 1)
else()
    set(TOMPOUCE_HAVE_LV2 0)
endif()
message(STATUS "LV2 discovery: ${TOMPOUCE_HAVE_LV2}")
pkg_check_modules(REQUIRED_DEPS REQUIRED IMPORTED_TARGET
    notcurses-core>=3.0.9 portaudio-2.0 libcurl libcjson alsa)
find_library(LEVELDB_LIBRARY NAMES leveldb REQUIRED)
find_path(LEVELDB_INCLUDE_DIR leveldb/c.h REQUIRED)

# Collab client floor (docs/multiplayer.md section 6.5, decision D4): the
# WebSocket transport needs libcurl >= 8.11.0 with wss. Below that the
# client stays out of the build with a message instead of breaking.
pkg_check_modules(LIBCURL_COLLAB QUIET IMPORTED_TARGET libcurl>=8.11.0)
if(LIBCURL_COLLAB_FOUND)
    set(WITH_COLLAB_DEFAULT ON)
else()
    set(WITH_COLLAB_DEFAULT OFF)
endif()
option(WITH_COLLAB "Build the collab client (libcurl WebSocket transport)" ${WITH_COLLAB_DEFAULT})
if(WITH_COLLAB AND NOT LIBCURL_COLLAB_FOUND)
    message(FATAL_ERROR "WITH_COLLAB needs libcurl >= 8.11.0 with WebSocket support")
endif()
if(NOT WITH_COLLAB AND NOT LIBCURL_COLLAB_FOUND)
    message(STATUS "collab client: off, libcurl older than 8.11.0")
endif()

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
tompouce_optional(OPENAL openal src/audio_output.c)
# Optional RTKit client for promoting the active audio render thread.
if(WITH_OPENAL OR WITH_PIPEWIRE)
    pkg_check_modules(SYSTEMD QUIET IMPORTED_TARGET libsystemd)
    if(SYSTEMD_FOUND)
        target_compile_definitions(tompouce_options INTERFACE HAVE_RTKIT)
        target_link_libraries(tompouce_options INTERFACE PkgConfig::SYSTEMD)
    endif()
endif()
tompouce_optional(FLUIDSYNTH "fluidsynth>=2.2" src/gm_synth.c)

# Opt-in native V8 query worker. QuickJS stays the default backend; this only
# builds a separate process that speaks the existing worker JSON protocol.
option(WITH_V8 "Build the native V8 query worker" OFF)
if(WITH_V8)
    enable_language(CXX)
    pkg_check_modules(V8 REQUIRED IMPORTED_TARGET v8)
    add_executable(tompouce-v8-worker src/v8_worker.cc)
    target_compile_features(tompouce-v8-worker PRIVATE cxx_std_20)
    target_compile_definitions(tompouce-v8-worker PRIVATE
        V8_COMPRESS_POINTERS V8_COMPRESS_POINTERS_IN_SHARED_CAGE
        V8_31BIT_SMIS_ON_64BIT_ARCH)
    target_compile_options(tompouce-v8-worker PRIVATE ${V8_CFLAGS_OTHER})
    # The imported target can collapse nested include dirs; V8's headers use
    # both the include root and its parent in quoted includes.
    foreach(v8_include IN LISTS V8_INCLUDE_DIRS)
        target_compile_options(tompouce-v8-worker PRIVATE -I${v8_include})
        target_compile_options(tompouce-v8-worker PRIVATE -I${v8_include}/include)
    endforeach()
    # V8 16 emits experimental CREL relocations that GNU ld cannot read.
    target_link_options(tompouce-v8-worker PRIVATE -fuse-ld=lld)
    target_link_directories(tompouce-v8-worker PRIVATE ${V8_LIBRARY_DIRS})
    target_link_libraries(tompouce-v8-worker PRIVATE PkgConfig::V8)
    # The embedder locates icudtl.dat beside the worker binary at runtime.
    list(GET V8_LIBRARY_DIRS 0 v8_first_libdir)
    if(EXISTS "${v8_first_libdir}/icudtl.dat")
        add_custom_command(TARGET tompouce-v8-worker POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${v8_first_libdir}/icudtl.dat"
                "$<TARGET_FILE_DIR:tompouce-v8-worker>/icudtl.dat")
    endif()
endif()

# Standalone collab director. Deliberately outside tompouce_core: a server
# must not link the engine. Shares only the protocol codec sources.
option(WITH_DIRECTOR "Build the standalone tompouce-director server" OFF)
if(WITH_DIRECTOR)
    pkg_check_modules(LIBWEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets>=4.5.8)
    pkg_check_modules(LIBSODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.20)
    pkg_check_modules(LIBCJSON REQUIRED IMPORTED_TARGET libcjson)
    add_executable(tompouce-director director/main.c director/session.c
        director/room.c src/collab_proto.c)
    target_include_directories(tompouce-director PRIVATE include director)
    target_compile_options(tompouce-director PRIVATE -Wall -Wextra -Werror -pedantic)
    target_link_libraries(tompouce-director PRIVATE PkgConfig::LIBWEBSOCKETS PkgConfig::LIBSODIUM PkgConfig::LIBCJSON m)
    add_test(NAME director_starts COMMAND tompouce-director --version)
    set_tests_properties(director_starts PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
endif()

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
    src/prebake_store.c
    src/js_audio.c src/dsp.c src/dsp_supersaw.c src/render_block.c src/voice_soa_dsp.c src/soa_voice.c src/periodic.c src/csound_host.c src/convolution.c src/convolution_mix.c src/companion.c
    src/sounds.c src/sound_store.c src/terminal_graphics.c src/visual.c src/visual_render.c src/visual_fft_simd.c
    src/audio_output.c src/native_synth.c src/osc_output.c src/reverb_impulse.c src/duck_onset.c
    src/stretch.c src/vocoder.c src/vocoder_tails_simd.c src/gm_synth.c src/sample_interp.c src/envelope_simd.c src/oscillator_simd.c src/reverb_simd.c src/delay_simd.c src/stretch_fft_simd.c src/stretch_real_fft_simd.c src/prep_profile.c src/prep_pool.c src/prep_cpu.c
    src/rational.c src/signal_wave.c src/js_worker.c src/lv2_host.c src/lv2_worker.c src/live_lv2.c src/carla_host.c)
add_dependencies(tompouce_core embedded_resources)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/convolution_mix.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off;-funroll-loops")
    set_source_files_properties(src/dsp_supersaw.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off;-funroll-loops")
    set_source_files_properties(src/render_block.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off;-funroll-loops")
    set_source_files_properties(src/voice_soa_dsp.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off;-funroll-loops")
    set_source_files_properties(src/soa_voice.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off;-funroll-loops")
    set_source_files_properties(src/sample_interp.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/envelope_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/oscillator_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/reverb_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/delay_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/stretch_fft_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/vocoder_tails_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/stretch_real_fft_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/visual_fft_simd.c PROPERTIES COMPILE_OPTIONS "-O3;-ffp-contract=off")
    set_source_files_properties(src/rational.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
endif()
target_compile_definitions(tompouce_core PUBLIC TOMPOUCE_HAVE_LV2=${TOMPOUCE_HAVE_LV2})
if(LILV_FOUND)
    target_include_directories(tompouce_core PUBLIC ${LILV_INCLUDE_DIRS})
    target_link_libraries(tompouce_core PUBLIC PkgConfig::LILV)
endif()
target_link_libraries(tompouce_core PUBLIC tompouce_options quickjs tree_sitter_javascript)
if(WITH_COLLAB)
    pkg_check_modules(LIBSODIUM_COLLAB REQUIRED IMPORTED_TARGET libsodium>=1.0.20)
    target_sources(tompouce_core PRIVATE src/collab_transport.c src/collab.c src/collab_crypto.c src/collab_proto.c)
    target_link_libraries(tompouce_core PRIVATE PkgConfig::LIBSODIUM_COLLAB)
    target_compile_definitions(tompouce_core PUBLIC HAVE_COLLAB)
    message(STATUS "collab client: on (libcurl WebSocket transport)")
endif()
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
    configure_file(alsoft.conf "${runtime_directory}/alsoft.conf" COPYONLY)
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
