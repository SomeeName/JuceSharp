# Native test executables.
#
# Each target recompiles ${JUCESHARP_CORE_SOURCES} — the SAME translation units
# the shipped DLL is built from — so a test can #include private headers and
# call internal C++ directly. No test-only ABI export exists, and none should
# ever be added: an export is permanent, a recompile is not.

# ---------------------------------------------------------------------------
# KNOWN COVERAGE GAP: the namamp module is NOT in the gate.
# ---------------------------------------------------------------------------
# NeuralAudio is a FetchContent static library, and CMake builds it once, with
# the release CRT that juce_sharp uses. The RT gate needs the DEBUG CRT for its
# malloc hook, and one image cannot hold both (LNK2038 on RuntimeLibrary and
# _ITERATOR_DEBUG_LEVEL — and mixing them really would give the process two
# heaps). Forcing NeuralAudio to /MDd would change the SHIPPED DLL's CRT, which
# is not an acceptable price for a test.
#
# So the gate compiles with DOTCHARTER_DSP_NEURAL=0: `namamp` is absent from the
# catalogue it sweeps (56 of 57 modules). The practical loss is small — with no
# model loaded namamp is a clean bypass — but it IS a hole, and closing it means
# either compiling NeuralAudio's sources into the gate or giving it its own
# /MDd target.
# ...except juce_sharp_rig_idle_nam (bottom of this file), which is the one
# target built in the SHIPPING configuration — release CRT, NeuralAudio — so it
# keeps a copy of the lists from before the removal.
set(_JUCESHARP_SHIPPING_SOURCES     ${JUCESHARP_CORE_SOURCES})
set(_JUCESHARP_SHIPPING_DEFINITIONS ${JUCESHARP_CORE_DEFINITIONS})
list(REMOVE_ITEM JUCESHARP_CORE_SOURCES     ${CMAKE_SOURCE_DIR}/src/dsp/NamAmpModule.cpp)
list(REMOVE_ITEM JUCESHARP_CORE_DEFINITIONS DOTCHARTER_DSP_NEURAL=1)
list(APPEND      JUCESHARP_CORE_DEFINITIONS DOTCHARTER_DSP_NEURAL=0)

# ---------------------------------------------------------------------------
# juce_sharp_rtcheck — the real-time allocation / lock / wait gate
# ---------------------------------------------------------------------------
add_executable(juce_sharp_rtcheck
    ${JUCESHARP_CORE_SOURCES}
    rt/rt_sentinel.cpp
    rt/rt_sentinel.h
    rt/rt_main.cpp)

target_include_directories(juce_sharp_rtcheck
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/rt
        ${rubberband_SOURCE_DIR}
        ${rubberband_SOURCE_DIR}/rubberband)

target_link_libraries(juce_sharp_rtcheck
    PRIVATE
        ${JUCESHARP_CORE_JUCE_MODULES}
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

target_compile_definitions(juce_sharp_rtcheck
    PRIVATE
        ${JUCESHARP_CORE_DEFINITIONS}
        DCA_RTCHECK=1)

if(MSVC)
    # THE DEBUG CRT IS THE POINT. _CrtSetAllocHook only exists in it, and it is
    # the only hook that sees malloc — which is how juce::HeapBlock and
    # juce::AudioBuffer::setSize allocate. An operator-new replacement alone
    # would miss that entire class of bug.
    set_property(TARGET juce_sharp_rtcheck PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    # Keep the optimiser on so the measured lock/wait timings are realistic,
    # and emit debug info so a violation reports file:line instead of a bare
    # address (rt_sentinel resolves stacks through DbgHelp).
    target_compile_options(juce_sharp_rtcheck PRIVATE /O2 /Ob1 /Zi)
    target_link_options(juce_sharp_rtcheck PRIVATE /DEBUG /INCREMENTAL:NO)
    target_link_libraries(juce_sharp_rtcheck PRIVATE dbghelp)
    # _DEBUG turns on the debug CRT AND juce's JUCE_DEBUG. jassert only logs
    # (there is no debugger attached), so that is survivable; NDEBUG stays on so
    # <cassert> and Eigen remain in release mode.
    target_compile_definitions(juce_sharp_rtcheck PRIVATE _DEBUG NDEBUG)
endif()

if(MSVC)
    set(_JUCESHARP_RB_FLAGS "/wd4244 /wd4267 /wd4305 /wd4146 /wd4458 /wd4459 /wd4127 /DNOMINMAX /D_USE_MATH_DEFINES")
else()
    set(_JUCESHARP_RB_FLAGS "-Wno-conversion -Wno-sign-conversion -Wno-unused-but-set-variable -DNOMINMAX -D_USE_MATH_DEFINES")
endif()
set_source_files_properties(${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    PROPERTIES COMPILE_FLAGS "${_JUCESHARP_RB_FLAGS}")

add_test(NAME rt_alloc_gate
         COMMAND juce_sharp_rtcheck
                 --baseline    "${CMAKE_CURRENT_SOURCE_DIR}/rt/rt_baseline.txt"
                 --lock-sites  "${CMAKE_CURRENT_SOURCE_DIR}/rt/rt_lock_sites.txt")
set_tests_properties(rt_alloc_gate PROPERTIES TIMEOUT 900)

# The device-rate WAV (ABI v43, DotCharter #435): writer, probe, stored-step
# adopt and the streaming gain source. JUCE formats only, no device.
add_executable(juce_sharp_device_rate_wav_test device_rate_wav_test.cpp)
target_include_directories(juce_sharp_device_rate_wav_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(juce_sharp_device_rate_wav_test
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_core
        juce::juce_recommended_config_flags)
target_compile_definitions(juce_sharp_device_rate_wav_test PRIVATE ${JUCESHARP_CORE_DEFINITIONS})
target_compile_features(juce_sharp_device_rate_wav_test PRIVATE cxx_std_17)
add_test(NAME device_rate_wav COMMAND juce_sharp_device_rate_wav_test)
set_tests_properties(device_rate_wav PROPERTIES TIMEOUT 120)

# Header-only in-house DSP regressions: deterministic, no device or JUCE startup.
add_executable(juce_sharp_tone_regressions dsp/tone_regressions.cpp ${JUCESHARP_RIG_FACTORY_SOURCES})
target_compile_definitions(juce_sharp_tone_regressions PRIVATE DOTCHARTER_DSP_NEURAL=0)
target_include_directories(juce_sharp_tone_regressions PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_tone_regressions PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_tone_regressions PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_tone_regressions PRIVATE /O2)
    target_compile_definitions(juce_sharp_tone_regressions PRIVATE _DEBUG)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(juce_sharp_tone_regressions PRIVATE Threads::Threads)
endif()
add_test(NAME tone_regressions COMMAND juce_sharp_tone_regressions)
set_tests_properties(tone_regressions PROPERTIES TIMEOUT 120)

# Rocksmith tone fidelity, native half (DotCharter #87 / #75 / #74): filter
# corners past a quarter of the rate against the RBJ / TPT formulas, the micpos
# tube curve and off-axis angle against the published mic charts, and the cab
# bank's new rows against their drivers' datasheets. Header-only cores.
add_executable(juce_sharp_tone_fidelity_cores dsp/tone_fidelity_cores_test.cpp)
target_include_directories(juce_sharp_tone_fidelity_cores PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_tone_fidelity_cores PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(juce_sharp_tone_fidelity_cores PRIVATE /O2 /bigobj)
endif()
add_test(NAME tone_fidelity_cores COMMAND juce_sharp_tone_fidelity_cores)
set_tests_properties(tone_fidelity_cores PROPERTIES TIMEOUT 300)
# Chart source (ABI v38): the scheduler's clock mapping, the note table and the
# placeholder voice. Deterministic, no device, no JUCE.
add_executable(juce_sharp_chart_scheduler synth/chart_scheduler_test.cpp ${JUCESHARP_SYNTH_SOURCES})
target_include_directories(juce_sharp_chart_scheduler PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_features(juce_sharp_chart_scheduler PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_chart_scheduler PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_chart_scheduler PRIVATE /O2)
    target_compile_definitions(juce_sharp_chart_scheduler PRIVATE _DEBUG)
endif()
add_test(NAME chart_scheduler COMMAND juce_sharp_chart_scheduler)
set_tests_properties(chart_scheduler PROPERTIES TIMEOUT 120)

# NAM loader ordering needs the real NeuralAudio model, but not the debug-CRT
# allocation sentinel. Keep this small executable on the shipping /MD runtime.
if(DOTCHARTER_DSP_NEURAL)
    add_executable(juce_sharp_nam_lifetime
        nam/nam_lifetime.cpp
        ${CMAKE_SOURCE_DIR}/src/dsp/NamAmpModule.cpp)
    target_compile_features(juce_sharp_nam_lifetime PRIVATE cxx_std_17)
    target_compile_definitions(juce_sharp_nam_lifetime PRIVATE DCA_NAM_TEST_HOOKS=1)
    target_include_directories(juce_sharp_nam_lifetime PRIVATE
        ${CMAKE_SOURCE_DIR}/src ${JUCESHARP_NEURAL_JSON_INCLUDE})
    target_link_libraries(juce_sharp_nam_lifetime PRIVATE NeuralAudio)
    if(MSVC)
        set_property(TARGET juce_sharp_nam_lifetime PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
    add_test(NAME juce_sharp_nam_lifetime COMMAND juce_sharp_nam_lifetime
        "${neuralaudio_SOURCE_DIR}/Utils/Models/BossLSTM-1x16.nam")
    set_tests_properties(juce_sharp_nam_lifetime PROPERTIES TIMEOUT 180)
endif()

# Pure routing mixer contract; no JUCE engine/device or native DLL required.
add_executable(juce_sharp_lane_mixer_test lane_mixer_test.cpp)
target_compile_features(juce_sharp_lane_mixer_test PRIVATE cxx_std_17)
add_test(NAME lane_mixer COMMAND juce_sharp_lane_mixer_test)

# Dual-IR convolution: impulse numerics, compatibility and callback allocation.
add_executable(juce_sharp_dualcab_test dualcab_test.cpp ${JUCESHARP_RIG_FACTORY_SOURCES})
target_include_directories(juce_sharp_dualcab_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_dualcab_test PRIVATE cxx_std_17)
target_compile_definitions(juce_sharp_dualcab_test PRIVATE DOTCHARTER_DSP_NEURAL=0)
if(MSVC)
    set_property(TARGET juce_sharp_dualcab_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_dualcab_test PRIVATE /O2)
    target_compile_definitions(juce_sharp_dualcab_test PRIVATE _DEBUG)
endif()
add_test(NAME dualcab COMMAND juce_sharp_dualcab_test)
set_tests_properties(dualcab PROPERTIES TIMEOUT 120)
# Actual delay factory + DspChain bypass integration, no JUCE/device dependency.
add_executable(juce_sharp_delay_sync_trails
    dsp/delay_sync_trails.cpp ${JUCESHARP_RIG_FACTORY_SOURCES})
target_compile_definitions(juce_sharp_delay_sync_trails PRIVATE DOTCHARTER_DSP_NEURAL=0)
target_include_directories(juce_sharp_delay_sync_trails PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_delay_sync_trails PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_delay_sync_trails PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_definitions(juce_sharp_delay_sync_trails PRIVATE _DEBUG)
    target_compile_options(juce_sharp_delay_sync_trails PRIVATE /O2)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(juce_sharp_delay_sync_trails PRIVATE Threads::Threads)
endif()
add_test(NAME delay_sync_trails COMMAND juce_sharp_delay_sync_trails)
set_tests_properties(delay_sync_trails PROPERTIES TIMEOUT 120)

# Idle sleep (src/dsp/IdleSleep.h): every allow-listed module type, and random
# chains of them, slept vs never-slept through pluck -> idle -> pluck. No
# JUCE, no device; namamp is covered by juce_sharp_rig_idle_nam below.
# `juce_sharp_rig_idle_sweep --discover` prints the whole catalogue's numbers.
add_executable(juce_sharp_rig_idle_sweep
    dsp/rig_idle_sweep.cpp ${JUCESHARP_RIG_FACTORY_SOURCES})
target_compile_definitions(juce_sharp_rig_idle_sweep PRIVATE DOTCHARTER_DSP_NEURAL=0)
target_include_directories(juce_sharp_rig_idle_sweep PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_rig_idle_sweep PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(juce_sharp_rig_idle_sweep PRIVATE /O2)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(juce_sharp_rig_idle_sweep PRIVATE Threads::Threads)
endif()
add_test(NAME rig_idle_sweep COMMAND juce_sharp_rig_idle_sweep)
set_tests_properties(rig_idle_sweep PROPERTIES TIMEOUT 1500)

# Idle sleep with REAL neural captures over the real device callback. The RT
# gate and the sweep cannot load a capture (no NeuralAudio under the debug
# CRT), so this recompiles the engine in the shipping configuration and drives
# the manual-pump null device. `--bench` times the owner's rig per block.
if(DOTCHARTER_DSP_NEURAL)
    add_executable(juce_sharp_rig_idle_nam
        nam/rig_idle_nam.cpp
        ${_JUCESHARP_SHIPPING_SOURCES})
    target_include_directories(juce_sharp_rig_idle_nam PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${rubberband_SOURCE_DIR}
        ${rubberband_SOURCE_DIR}/rubberband
        ${JUCESHARP_NEURAL_JSON_INCLUDE})
    target_link_libraries(juce_sharp_rig_idle_nam PRIVATE
        ${JUCESHARP_CORE_JUCE_MODULES}
        NeuralAudio
        juce::juce_recommended_config_flags)
    target_compile_definitions(juce_sharp_rig_idle_nam PRIVATE ${_JUCESHARP_SHIPPING_DEFINITIONS})
    if(MSVC)
        set_property(TARGET juce_sharp_rig_idle_nam PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()
    add_test(NAME rig_idle_nam COMMAND juce_sharp_rig_idle_nam
        "${neuralaudio_SOURCE_DIR}/Utils/Models/BossLSTM-1x16.nam")
    set_tests_properties(rig_idle_nam PROPERTIES TIMEOUT 600)
endif()

# The chart voice (src/dsp/voice/, issue #39 step 1): pitch per string and
# fret, bends, slides, mutes, harmonics, voice stealing, timing, determinism,
# sample-rate independence, and an allocation hook over the render path.
# Header-only: no JUCE, no device, no factory.
add_executable(juce_sharp_guitar_voice_test dsp/guitar_voice_test.cpp)
target_include_directories(juce_sharp_guitar_voice_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_guitar_voice_test PRIVATE cxx_std_17)
if(MSVC)
    # Debug CRT for the _CrtSetAllocHook allocation probe, optimised for speed.
    set_property(TARGET juce_sharp_guitar_voice_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_guitar_voice_test PRIVATE /O2 /bigobj)
    target_compile_definitions(juce_sharp_guitar_voice_test PRIVATE _DEBUG)
endif()
# The fallback bank's size budget reads the generated header itself (#61).
target_compile_definitions(juce_sharp_guitar_voice_test PRIVATE
    "JUCESHARP_VOICE_EXCITATION_DATA=\"${CMAKE_SOURCE_DIR}/src/dsp/voice/dcvoice_excitation_data.h\"")
add_test(NAME guitar_voice COMMAND juce_sharp_guitar_voice_test)
set_tests_properties(guitar_voice PROPERTIES TIMEOUT 300)

# The SAMPLE-based voice (dcvoice_sampler.h + dcvoice_pack.h): the pack format
# through the real parser, pitch, gestures, note-offs vs let-ring, the
# recorded articulations, determinism and the allocation hook. Its packs are
# synthesised in memory — the real ones are built outside git.
add_executable(juce_sharp_sample_voice_test dsp/sample_voice_test.cpp)
target_include_directories(juce_sharp_sample_voice_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include)
target_compile_features(juce_sharp_sample_voice_test PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_sample_voice_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_sample_voice_test PRIVATE /O2 /bigobj)
    target_compile_definitions(juce_sharp_sample_voice_test PRIVATE _DEBUG)
endif()
add_test(NAME sample_voice COMMAND juce_sharp_sample_voice_test)
set_tests_properties(sample_voice PROPERTIES TIMEOUT 300)

# The INSTRUMENT MODELS (#50: dcvoice_models.h, dcvoice_pickup.h): the model
# table, which positions play their recordings unconverted, the conversion
# filter against its analytic transfer, pitch through every model and pickup,
# the spectral DIRECTION (neck darker than bridge, humbucker darker than its
# split, P darker than a J bridge), one pack loaded per selection and released
# with the voice, the cross-fade-out, and the allocation hook. Packs are
# synthesised (tests/dsp/voice_pack_fixture.h) and coloured with the pickup
# comb each is said to have recorded.
add_executable(juce_sharp_instrument_models_test dsp/instrument_models_test.cpp)
target_include_directories(juce_sharp_instrument_models_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
target_compile_features(juce_sharp_instrument_models_test PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_instrument_models_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_instrument_models_test PRIVATE /O2 /bigobj)
    target_compile_definitions(juce_sharp_instrument_models_test PRIVATE _DEBUG)
endif()
add_test(NAME instrument_models COMMAND juce_sharp_instrument_models_test)
set_tests_properties(instrument_models PROPERTIES TIMEOUT 600)

# The documented-circuit amp voicings and the amp / bass-head switches
# (DotCharter #70 / #72): dcdsp_tonestack.h against an independent nodal solve
# of its netlist and against Kuehnel's published estimates, the circuit rows of
# dcdsp_amp.h, Bright / Resonance, and bassamp's Ultra Lo / Ultra Hi / mid
# selector at the figures Ampeg publishes for its SVT-CL. Header-only cores, no
# JUCE, plus the allocation hook.
add_executable(juce_sharp_amp_circuits_test dsp/amp_circuits_test.cpp)
target_include_directories(juce_sharp_amp_circuits_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_features(juce_sharp_amp_circuits_test PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_amp_circuits_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_amp_circuits_test PRIVATE /O2)
    target_compile_definitions(juce_sharp_amp_circuits_test PRIVATE _DEBUG)
endif()
add_test(NAME amp_circuits COMMAND juce_sharp_amp_circuits_test)
set_tests_properties(amp_circuits PROPERTIES TIMEOUT 300)

# The rsamp chain table loaded at RUN TIME (ABI v45, dcdsp_rsamp_table.h): the
# parser's refusals, the registry, and the core's passthrough without a table and
# its own pick-up of a loaded one through the dip, allocation-free. Every table
# is SYNTHETIC (dsp/rsamp_synthetic_table.h): Rocksmith's numbers are read on the
# user's machine and are in no repository.
add_executable(juce_sharp_rsamp_table_test dsp/rsamp_table_test.cpp)
target_include_directories(juce_sharp_rsamp_table_test PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/dsp)
target_compile_features(juce_sharp_rsamp_table_test PRIVATE cxx_std_17)
if(MSVC)
    set_property(TARGET juce_sharp_rsamp_table_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")
    target_compile_options(juce_sharp_rsamp_table_test PRIVATE /O2)
    target_compile_definitions(juce_sharp_rsamp_table_test PRIVATE _DEBUG)
endif()
add_test(NAME rsamp_table COMMAND juce_sharp_rsamp_table_test)
set_tests_properties(rsamp_table PROPERTIES TIMEOUT 120)
