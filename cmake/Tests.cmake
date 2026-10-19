function(tompouce_test name)
    add_executable(test_${name} tests/test_${name}.c)
    set_target_properties(test_${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    # Assertions also execute setup in these harnesses; Release must retain them.
    target_compile_options(test_${name} PRIVATE -UNDEBUG)
    target_link_libraries(test_${name} PRIVATE tompouce_core)
    if(NOT name STREQUAL "terminal_render")
        add_test(NAME ${name} COMMAND test_${name})
        set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
    endif()
endfunction()
foreach(name ai websamp editor terminal_render javascript dsp examples csound
        audio audio_performance prep_profile prep_deadlines prep_cpu room duck_onset terminal_graphics audio_output native_synth live_sliders)
    tompouce_test(${name})
endforeach()

set(test_environment "TOMPOUCE_TEST_BINARY=$<TARGET_FILE:tompouce>")
foreach(name terminal_render_pty install sound_import sounds companion osc tui_pty)
    add_test(NAME ${name} COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${name}.py")
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        ENVIRONMENT "${test_environment};TOMPOUCE_RENDER_TEST_BINARY=$<TARGET_FILE:test_terminal_render>" TIMEOUT 300)
endforeach()
set_tests_properties(tui_pty PROPERTIES LABELS "ui" RUN_SERIAL TRUE)
set_tests_properties(terminal_render_pty PROPERTIES LABELS "ui")
add_test(NAME live_controls_reference COMMAND node js/test-live-controls.mjs)
set_tests_properties(live_controls_reference PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30
    ENVIRONMENT "TOMPOUCE_LIVE_CONTROLS_BINARY=$<TARGET_FILE:test_live_sliders>")
add_custom_target(test-controls-reference-web
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_LIVE_CONTROLS_BINARY=$<TARGET_FILE:test_live_sliders>"
        node js/test-live-controls.mjs
    COMMAND node scripts/controls-reference-qa.mjs
    DEPENDS test_live_sliders WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(test-csound-reference-web
    COMMAND $<TARGET_FILE:test_csound>
    COMMAND node scripts/csound-reference-qa.mjs
    DEPENDS test_csound WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(test-voice-reference-web
    COMMAND node scripts/voice-pool-reference-qa.mjs
    COMMAND node scripts/voice-eviction-reference-qa.mjs
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_VOICE_BINARY=$<TARGET_FILE:test_audio_performance>"
        node scripts/voice-edge-reference-qa.mjs
    DEPENDS test_audio_performance WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(test-duck-reference-web
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_LIVE_CONTROLS_BINARY=$<TARGET_FILE:test_live_sliders>"
        node scripts/duck-preparation-reference-qa.mjs
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_LIVE_CONTROLS_BINARY=$<TARGET_FILE:test_live_sliders>"
        node scripts/duck-timing-reference-qa.mjs
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_LIVE_CONTROLS_BINARY=$<TARGET_FILE:test_live_sliders>"
        node scripts/duck-timer-reference-qa.mjs
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_DUCK_ONSET_BINARY=$<TARGET_FILE:test_duck_onset>"
        node scripts/duck-onset-reference-qa.mjs
    DEPENDS test_live_sliders test_duck_onset WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(profile-prep-deadlines
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_PREP_DEADLINE_BINARY=$<TARGET_FILE:test_prep_deadlines>"
        node scripts/prep-deadline-qa.mjs
    DEPENDS test_prep_deadlines WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(profile-prep-backend
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_PREP_DEADLINE_BINARY=$<TARGET_FILE:test_prep_deadlines>"
        "TOMPOUCE_AUDIO_PROBE=$<TARGET_FILE:probe_audio_output>"
        node scripts/prep-backend-qa.mjs
    DEPENDS test_prep_deadlines probe_audio_output WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_custom_target(test-live-reference-web
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_LIVE_ONSET_BINARY=$<TARGET_FILE:test_live_sliders>"
        node scripts/live-change-reference-qa.mjs
    DEPENDS test_live_sliders WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    USES_TERMINAL VERBATIM)

add_executable(render_audio EXCLUDE_FROM_ALL tests/render_audio.c)
set_target_properties(render_audio PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
target_compile_options(render_audio PRIVATE -UNDEBUG)
target_link_libraries(render_audio PRIVATE tompouce_core)
add_custom_target(compare-audio
    COMMAND "${CMAKE_COMMAND}" -E env "TOMPOUCE_RENDER_AUDIO=$<TARGET_FILE:render_audio>"
        node scripts/compare-audio.mjs
    DEPENDS render_audio WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
add_custom_target(test-examples COMMAND $<TARGET_FILE:test_examples> DEPENDS test_examples
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(test-tui
    COMMAND "${CMAKE_COMMAND}" -E env "${test_environment}"
        "${Python3_EXECUTABLE}" tests/test_tui_pty.py
    DEPENDS tompouce WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)

# These tests include audio_output.c with stubs, or deliberately disable native
# servers. Keep the PortAudio probe's feature flags separate from the core.
add_executable(probe_audio_output EXCLUDE_FROM_ALL tests/probe_audio_output.c)
target_link_libraries(probe_audio_output PRIVATE tompouce_core)
add_executable(probe_audio_portaudio EXCLUDE_FROM_ALL tests/probe_audio_output.c src/audio_output.c)
target_compile_options(probe_audio_portaudio PRIVATE -UHAVE_PIPEWIRE -UHAVE_PULSE)
target_link_libraries(probe_audio_portaudio PRIVATE tompouce_core)
foreach(name probe_audio_output probe_audio_portaudio)
    set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
endforeach()
add_custom_target(test-audio-backends
    COMMAND $<TARGET_FILE:test_audio_output>
    COMMAND "${CMAKE_COMMAND}" -E env "${test_environment}"
        "TOMPOUCE_AUDIO_PROBE=$<TARGET_FILE:probe_audio_output>"
        "TOMPOUCE_PORTAUDIO_PROBE=$<TARGET_FILE:probe_audio_portaudio>"
        "${Python3_EXECUTABLE}" tests/test_audio_backends.py
    DEPENDS tompouce test_audio_output probe_audio_output probe_audio_portaudio
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)

foreach(name audio audio_performance room duck_onset native_synth editor terminal_render live_sliders csound)
    set(sources tests/test_${name}.c)
    if(NOT name STREQUAL "editor" AND NOT name STREQUAL "terminal_render" AND NOT name STREQUAL "csound")
        list(APPEND sources src/stretch.c)
    endif()
    if(name STREQUAL "native_synth")
        list(APPEND sources src/native_synth.c src/gm_synth.c)
    endif()
    if(name STREQUAL "live_sliders")
        list(APPEND sources src/slider.c src/javascript.c src/js_audio.c src/duck_onset.c)
    endif()
    add_executable(test_${name}_sanitize EXCLUDE_FROM_ALL ${sources})
    set_target_properties(test_${name}_sanitize PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
    target_compile_options(test_${name}_sanitize PRIVATE -UNDEBUG -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(test_${name}_sanitize PRIVATE -fsanitize=address,undefined)
    target_link_libraries(test_${name}_sanitize PRIVATE tompouce_core)
    if(name STREQUAL "live_sliders")
        target_compile_definitions(test_live_sliders_sanitize PRIVATE
            TOMPOUCE_TEST_CSOUND_WORKER="$<TARGET_FILE:test_live_sliders>")
        add_dependencies(test_live_sliders_sanitize test_live_sliders)
    endif()
    if(name STREQUAL "csound")
        target_compile_definitions(test_csound_sanitize PRIVATE
            TOMPOUCE_TEST_CSOUND_WORKER="$<TARGET_FILE:test_csound>")
        add_dependencies(test_csound_sanitize test_csound)
    endif()
endforeach()
add_custom_target(test-sanitize
    COMMAND $<TARGET_FILE:test_audio_sanitize>
    COMMAND $<TARGET_FILE:test_audio_performance_sanitize>
    COMMAND $<TARGET_FILE:test_room_sanitize>
    COMMAND $<TARGET_FILE:test_duck_onset_sanitize>
    COMMAND $<TARGET_FILE:test_native_synth_sanitize>
    COMMAND $<TARGET_FILE:test_editor_sanitize>
    COMMAND $<TARGET_FILE:test_live_sliders_sanitize>
    COMMAND $<TARGET_FILE:test_csound_sanitize>
    DEPENDS test_audio_sanitize test_audio_performance_sanitize test_room_sanitize test_duck_onset_sanitize test_native_synth_sanitize test_editor_sanitize test_live_sliders_sanitize test_csound_sanitize
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL)
add_custom_target(test-terminal-sanitize
    COMMAND "${CMAKE_COMMAND}" -E env
        "TOMPOUCE_RENDER_TEST_BINARY=$<TARGET_FILE:test_terminal_render_sanitize>"
        "${Python3_EXECUTABLE}" tests/test_terminal_render_pty.py
    DEPENDS test_terminal_render_sanitize
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" USES_TERMINAL VERBATIM)
