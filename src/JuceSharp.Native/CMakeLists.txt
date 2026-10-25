cmake_minimum_required(VERSION 3.22)

project(JuceSharpNative
    # Tracks the WRAPPER's version (PackageId JuceSharp), which is what a user
    # sees. It sat at 0.3.0 for five releases while dca_version_string()
    # hard-coded the same stale number a second time; the string is generated
    # from this now, so there is one place to bump.
    VERSION 1.3.0
    DESCRIPTION "JuceSharp native audio engine (JUCE-based)"
    # C is required as of JUCE 8.0.15 (and by 9.x, which builds its zlib/FLAC as C) — its own CMakeLists hard-errors with
    # "A C compiler is required to build targets that depend on JUCE" when the
    # consuming project declares CXX only. This is the ONE source change the
    # 8.0.13 -> 8.0.15 bump needed; we compile no C of our own.
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Newer CMake (3.30+) deprecates FetchContent_Populate() in favour of
# FetchContent_MakeAvailable() + SOURCE_SUBDIR. Pin the policy explicitly so
# our deliberate populate-without-add_subdirectory pattern still works either
# way (see demucs.cpp block below).
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

# ---------------------------------------------------------------------------
# C++ runtime: linked STATICALLY (DotCharter #176)
# ---------------------------------------------------------------------------
# The DLL and juce_vst_host.exe used to link the DYNAMIC CRT (/MD), which made
# them import VCRUNTIME140.dll, VCRUNTIME140_1.dll, MSVCP140.dll and
# MSVCP140_ATOMIC_WAIT.dll. A release zip ships none of those, so on a Windows
# without a recent VC++ 2015-2022 x64 redistributable (a clean install or VM)
# the engine failed to load and there was no audio. /MT removes the dependency:
# the only imports left are Windows system DLLs (the guard is DotCharter's
# NativeImportAuditTests, over the runtimes/win-x64/native set this build deploys,
# and its release preflight over the staged payload).
#
# Set BEFORE any target exists: it is the default for JUCE, Rubber Band and
# NeuralAudio too (a static library built /MD cannot be linked into a /MT image:
# LNK2038 RuntimeLibrary mismatch, and mixing would give the process two heaps).
# Needs policy CMP0091 NEW (cmake_minimum_required >= 3.15 above).
# What it changes for a consumer: this DLL has ITS OWN CRT heap, so memory it
# allocates must be freed by an export of this DLL, never by the host's free()
# (the ABI already works that way: every returned buffer has a dca_*_free).
# Every target below that needs another CRT sets MSVC_RUNTIME_LIBRARY itself
# (the RT gate's debug CRT); tests that link the shipping NeuralAudio use
# JUCESHARP_SHIPPING_CRT so they follow this switch.
option(JUCESHARP_STATIC_CRT "Link the MSVC C/C++ runtime statically (no VC++ redistributable needed)" ON)
if(JUCESHARP_STATIC_CRT)
    set(JUCESHARP_SHIPPING_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(JUCESHARP_SHIPPING_CRT "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "${JUCESHARP_SHIPPING_CRT}")

# ---------------------------------------------------------------------------
# JUCE
# ---------------------------------------------------------------------------
option(DOTCHARTER_USE_SYSTEM_JUCE "Use a local JUCE checkout instead of fetching" OFF)
option(JUCESHARP_USE_SYSTEM_JUCE "Alias of DOTCHARTER_USE_SYSTEM_JUCE for the standalone build" OFF)
if(JUCESHARP_USE_SYSTEM_JUCE)
    set(DOTCHARTER_USE_SYSTEM_JUCE ON)
endif()
# p1-13: pin to a tagged release, fetched by the commit that tag names
# (JUCE_COMMIT below: a tag CAN be moved upstream, a commit cannot). Override at
# configure time via -DJUCE_TAG=... -DJUCE_COMMIT=... for a different release.
# 9.0.3 (2026-09). A PURE PORT from 8.0.15: same behaviour, no new feature. What
# 9.0.x changes underneath THIS engine, read off `git diff 8.0.15 9.0.3`, and
# what was done about each:
#   * juce_audio_formats: registerBasicFormats() now registers OpusAudioFormat
#     (JUCE_USE_OPUS defaults to 1, 9.0.3) and the software MP3AudioFormat
#     (JUCE_USE_MP3AUDIOFORMAT defaults to 1, 9.0.2) AHEAD of the Windows Media
#     reader that decoded every .mp3 on 8.x. Both are pinned to 0 in
#     JUCESHARP_CORE_DEFINITIONS below, so the engine opens exactly the files it
#     opened before, with the same decoder. Turning either on is its own change:
#     JUCE's MP3 decoder puts audio 624 samples EARLIER than Windows Media.
#     With the flags as they are, WAV / FLAC / AIFF / MP3 decode bit-identical
#     to 8.0.15 and Ogg Vorbis within 6.3e-7. The one exception is JUCE's own
#     hardening: a WAV whose chunk claims more bytes than the file holds (a
#     truncated or pipe-streamed file) now opens with ZERO samples.
#   * AudioDeviceManager::setAudioDeviceSetup picks the NEAREST available rate /
#     buffer size instead of "exact, else the device default". Only a request the
#     device does not list lands differently (e.g. 100 frames: 128, not 512).
#   * ASIO (juce_ASIO_windows.cpp, 60+/64-): openDevice's probe now disposes the
#     dummy buffers it creates, and the rate probe list was renamed. Still the
#     BUNDLED ASIO 2.3 headers, JUCE_ASIO_USE_EXTERNAL_SDK still 0.
#   * WASAPI: the stream joins the process's default audio session (it passed an
#     uninitialised GUID before) and >32-channel masks no longer overflow.
#   * MIDI (Win32 backend; Windows MIDI Services and WinRT MIDI stay 0): output
#     no longer retries MIDIERR_NOTREADY for 50 ms or an ALLOCATED device for
#     400 ms, and MidiOutput::sendMessageNow now returns bool. MidiOutputDevice.h
#     restores both tolerances itself. Input re-queues sysex buffers from a
#     per-device thread rather than the driver callback.
#   * VST3 hosting: performEdit updates the host-side parameter without echoing
#     it back into the plugin; a shell plugin's factory gets its host context
#     before its classes are enumerated. Bundled VST3 SDK is still 3.8.0 (MIT).
#   * juce_graphics rewrite (Drawable no longer a Component, SVG via lunasvg) and
#     Windows multi-touch OFF by default: this engine draws nothing; the out-of-
#     process host's DocumentWindow only wraps each plugin's own HWND, so neither
#     reaches plugin UI rendering — but re-check real editors (HiDPI included).
#   * Licence: JUCE 9 modules are AGPLv3 or the JUCE 9 commercial EULA.
#
# Source-only: the C ABI exposes no JUCE type, so DCA_ABI_VERSION does NOT move.
# The DLL must still be rebuilt and redeployed before managed code runs.
#
# **A CHANGED TAG IS NOT A CHANGED TREE when FETCHCONTENT_SOURCE_DIR_JUCE is
# set** — FetchContent then uses that directory as-is and never checks it out.
# The owner's main build tree carried that variable pointing at its own
# _deps/juce-src and went on compiling 8.0.13 for the whole 8.0.15 release while
# JUCESHARP_JUCE_TAG told dca_version_string() "8.0.15". The version check after
# FetchContent_MakeAvailable below now refuses that mismatch at configure time.
set(JUCE_TAG "9.0.3" CACHE STRING "JUCE git tag/branch to fetch")
# The fetch is pinned by COMMIT, not by the tag (a tag can be moved upstream):
# JUCE_COMMIT is the commit JUCE_TAG named when it was pinned. Override BOTH
# together. JUCE_TAG stays the release name the version check below compares.
# No GIT_SHALLOW: CMake can only shallow-clone a branch or tag, never a SHA.
set(JUCE_COMMIT "be29c81492b6151c8ea8d14c840e1311963b3a83" CACHE STRING "JUCE commit of JUCE_TAG")
if(JUCE_TAG MATCHES "^(main|master|develop|HEAD)$")
    message(WARNING "JuceSharp: JUCE_TAG='${JUCE_TAG}' is a mutable ref; builds are not reproducible. "
                    "Set -DJUCE_TAG=<tag-or-sha> for a pinned build.")
endif()

if(DOTCHARTER_USE_SYSTEM_JUCE)
    if(NOT JUCE_DIR)
        message(FATAL_ERROR "DOTCHARTER_USE_SYSTEM_JUCE=ON but JUCE_DIR not set")
    endif()
    add_subdirectory(${JUCE_DIR} juce-build)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${JUCE_COMMIT})
    FetchContent_MakeAvailable(JUCE)
endif()

# The tree actually compiled must be the release JUCE_TAG names (see the note
# above the tag: a stale FETCHCONTENT_SOURCE_DIR_JUCE kept 8.0.13 alive under an
# 8.0.15 label). Read the version out of the sources, not out of the tag.
if(DOTCHARTER_USE_SYSTEM_JUCE)
    set(_jucesharp_juce_src "${JUCE_DIR}")
else()
    set(_jucesharp_juce_src "${juce_SOURCE_DIR}")
endif()
file(STRINGS "${_jucesharp_juce_src}/modules/juce_core/system/juce_StandardHeader.h"
     _jucesharp_juce_version_lines
     REGEX "^#define JUCE_(MAJOR_VERSION|MINOR_VERSION|BUILDNUMBER) +[0-9]+")
set(_jucesharp_juce_version "")
foreach(_part IN ITEMS MAJOR_VERSION MINOR_VERSION BUILDNUMBER)
    foreach(_line IN LISTS _jucesharp_juce_version_lines)
        if(_line MATCHES "^#define JUCE_${_part} +([0-9]+)")
            list(APPEND _jucesharp_juce_version "${CMAKE_MATCH_1}")
        endif()
    endforeach()
endforeach()
list(JOIN _jucesharp_juce_version "." _jucesharp_juce_version)
message(STATUS "JuceSharp: JUCE sources at ${_jucesharp_juce_src} are version ${_jucesharp_juce_version} (JUCE_TAG=${JUCE_TAG})")
if(JUCE_TAG MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" AND NOT _jucesharp_juce_version STREQUAL JUCE_TAG)
    message(FATAL_ERROR
        "JuceSharp: JUCE_TAG is ${JUCE_TAG} but the JUCE sources at ${_jucesharp_juce_src} are "
        "${_jucesharp_juce_version}. FetchContent does not re-checkout a directory given by "
        "FETCHCONTENT_SOURCE_DIR_JUCE (or JUCE_DIR). Clear that cache entry, or point it at a "
        "${JUCE_TAG} checkout, and configure again.")
endif()

# ---------------------------------------------------------------------------
# Rubber Band Library (single-file build, GPLv2 / commercial)
# ---------------------------------------------------------------------------
# p1-14: GPLv2 dependency. By linking Rubber Band into juce_sharp.dll the
# resulting binary inherits GPLv2 unless you hold a commercial licence from
# Particular Programs Ltd. Closed-source / proprietary redistribution
# requires the commercial Rubber Band licence — see
# https://breakfastquay.com/rubberband/licensing.html
set(RUBBERBAND_TAG "v4.0.0" CACHE STRING "Rubber Band git tag")
# Fetched by the commit RUBBERBAND_TAG named when pinned (see JUCE_COMMIT).
set(RUBBERBAND_COMMIT "1d95888bec3ae0a17c0c4af791810d5a63f6bc35" CACHE STRING "Rubber Band commit of RUBBERBAND_TAG")
if(RUBBERBAND_TAG MATCHES "^(main|master|default-branch|HEAD)$")
    message(WARNING "JuceSharp: RUBBERBAND_TAG='${RUBBERBAND_TAG}' is a mutable ref; builds are not reproducible.")
endif()
message(STATUS "JuceSharp: linking Rubber Band ${RUBBERBAND_TAG} — juce_sharp.dll inherits GPLv2 unless a commercial Rubber Band licence is in place.")
include(FetchContent)
FetchContent_Declare(
    rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG        ${RUBBERBAND_COMMIT})
FetchContent_MakeAvailable(rubberband)

# ---------------------------------------------------------------------------
# Shared source list
# ---------------------------------------------------------------------------
# The shipping DLL and the native test executables compile the SAME
# translation units, only with different flags/definitions (the RT gate needs
# the debug CRT for its malloc hook, and DCA_RTCHECK=1 to switch on the
# audio-thread lock/wait marks in src/RtDiag.h). Do NOT convert this into an
# OBJECT library: the rtcheck target genuinely needs a different CRT, so it has
# to recompile.
# ABSOLUTE paths: tests/CMakeLists.txt consumes this list from a different
# directory scope, where relative paths would resolve against tests/.
# The tone-rig factory plus its per-category gear files. ONE list, because
# three test executables under tests/ compile the factory by hand rather than
# through JUCESHARP_CORE_SOURCES, and a gear file missing from one of them is
# nine unresolved externals at link time.
set(JUCESHARP_RIG_FACTORY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/DspModuleFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/DynamicsGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/DriveGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/AmpCabGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/FilterGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/ModulationGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/PitchGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/DelayGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/ReverbGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/UtilityGear.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/gear/RsAmpGear.cpp
)

# The chart source's voices (ABI v38, src/synth/). Plain C++17 with no JUCE, so
# the scheduler tests (tests/synth/) build from this list alone. A new voice's
# .cpp goes HERE, or it links clean into the tests and is missing from the DLL.
set(JUCESHARP_SYNTH_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/ChartVoiceFactory.cpp
)

set(JUCESHARP_CORE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dotcharter_audio.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AudioEngine.cpp
    ${JUCESHARP_RIG_FACTORY_SOURCES}
    ${JUCESHARP_SYNTH_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RubberBandAudioSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PanningAudioSource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MirrorOutput.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RemoteVstBridge.cpp
    # ABI v47: the external process insert (a seat exchanged with another process).
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ExternalProcessInsert.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StemSeparator.cpp
    # ABI v42 (#426): the native .wem decoder and its juce::AudioFormat.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemVorbisDecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemAudioFormat.cpp
    # ABI v44 (DotCharter #317): the Wwise-free .wem encoder.
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemVorbisEncoder.cpp
    ${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp)

set(JUCESHARP_CORE_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AbiArgs.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Utf8Truncate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AudioEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RtDiag.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/DspModule.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/DspChain.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/IdleSleep.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/Scrub.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/DspModuleFactory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/GearEntry.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/IrWavLoader.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/ChartVoice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/ChartNoteTable.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/ChartScheduler.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/ChartVoiceFactory.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/synth/PlaceholderPluckVoice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RubberBandAudioSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PanningAudioSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/MirrorOutput.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/NullAudioIODevice.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/RemoteVstBridge.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ExternalProcessInsert.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/StemSeparator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemVorbisDecoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemAudioFormat.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemVorbisCommon.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/WemVorbisEncoder.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/wem/wem_codebooks.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/dotcharter_audio.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/dotcharter_vst_bridge.h)

# The JUCE modules every core-source consumer needs, in one place so the test
# targets cannot silently drift from the DLL.
set(JUCESHARP_CORE_JUCE_MODULES
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_dsp
    juce::juce_events
    juce::juce_gui_basics
    juce::juce_gui_extra)

set(JUCESHARP_CORE_DEFINITIONS
    JUCE_STANDALONE_APPLICATION=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    # JUCE 9 turned both of these ON by default, and registerBasicFormats
    # registers them AHEAD of the Windows Media reader. Off, the format manager
    # is the 8.x one: WAV, AIFF, FLAC, Ogg Vorbis, and .mp3 through Windows Media.
    # Native Opus is a separate, later change; swapping the MP3 decoder would
    # move an MP3 song's length and first sample, which a chart timed against
    # the old decoder would hear. No parentheses in this comment: a lint in
    # AbiSentinelAndAliasTests reads this set block up to the first one.
    JUCE_USE_OPUS=0
    JUCE_USE_MP3AUDIOFORMAT=0
    DOTCHARTER_AUDIO_EXPORTS=1
    # dca_version_string() is built from these — see dotcharter_audio.cpp. In
    # JUCESHARP_CORE_DEFINITIONS rather than on the juce_sharp target alone so
    # the test executables, which recompile the same TU, report the same thing.
    JUCESHARP_NATIVE_VERSION="${PROJECT_VERSION}"
    JUCESHARP_JUCE_TAG="${JUCE_TAG}")

# ---------------------------------------------------------------------------
# Native DLL
# ---------------------------------------------------------------------------
add_library(juce_sharp SHARED
    ${JUCESHARP_CORE_SOURCES}
    ${JUCESHARP_CORE_HEADERS})

# p2-28: warning-disable flags are MSVC-only. GCC/Clang reject /wd... silently
# but spam warnings. Gate them on the compiler.
if(MSVC)
    set_source_files_properties(${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp
        PROPERTIES COMPILE_FLAGS "/wd4100 /wd4244 /wd4267 /wd4305 /wd4146 /wd4458 /wd4459 /wd4127 /DNOMINMAX /D_USE_MATH_DEFINES")
else()
    set_source_files_properties(${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp
        PROPERTIES COMPILE_FLAGS "-Wno-conversion -Wno-sign-conversion -Wno-unused-but-set-variable -DNOMINMAX -D_USE_MATH_DEFINES")
endif()

target_include_directories(juce_sharp
    PRIVATE
        ${rubberband_SOURCE_DIR}
        ${rubberband_SOURCE_DIR}/rubberband)

target_include_directories(juce_sharp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(juce_sharp
    PRIVATE
        ${JUCESHARP_CORE_JUCE_MODULES}
        # p3-50: recommended JUCE config / warning / LTO flags are build-time
        # tooling for *this* target only. PUBLIC would forward them to any
        # downstream consumer of juce_sharp, polluting their build with JUCE's
        # warning suppressions and forcing them into LTO. Keep them PRIVATE.
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# NOTE: JUCESHARP_CORE_DEFINITIONS is applied ONCE, below, and the test targets
# read the same list (W4-N21). Never append a second /D for a macro already in
# it: MSVC keeps the last one (command-line warning D9025, which /WX does not
# catch), so the DLL and the RT gate can silently disagree.

# WARNINGS ARE ERRORS IN THE SHIPPING DLL. Measured 2026-09-11: a clean
# /WX build of this target is silent. The only thing that stood in the way was
# C4100 in Rubber Band's own Thread.h, reached through RubberBandSingle.cpp —
# vendored code, in the one file that already carries a suppression list for
# exactly this reason, so /wd4100 joined it above.
#
# Scoped to THIS target on purpose: the test executables still compile
# juce::AudioFormat::createWriterFor and strncpy (four C4996s and two C4100s,
# all in tests/rt/rt_main.cpp), and turning those into errors would block the
# gate on deprecations rather than on real-time faults.
if(MSVC)
    target_compile_options(juce_sharp PRIVATE /WX)
endif()

# ASIO: JUCE_ASIO=1 compiles against the ASIO headers JUCE itself bundles
# (modules/juce_audio_devices/native/asio/, Steinberg's 2025 dual licence —
# proprietary or GPLv3 — since JUCE 8.0.15). JUCE_ASIO_USE_EXTERNAL_SDK stays at
# its default of 0, so no Steinberg SDK is vendored here or needed at build
# time. The ASIOSDK2.3 copy this repo carried until 2026-09-28 had not been
# read by the build since the 8.0.15 bump; it was deleted rather than updated.

# JUCESHARP_CORE_DEFINITIONS IS FINAL — apply it, once, to the shipping
# target. add_subdirectory(tests) further down reads the same resolved list, so
# the DLL and the RT gate are compiled from ONE source of truth instead of
# arriving at the same answer by two mechanisms, one of which was ordering luck.
target_compile_definitions(juce_sharp PRIVATE ${JUCESHARP_CORE_DEFINITIONS})

set_target_properties(juce_sharp PROPERTIES
    OUTPUT_NAME "juce_sharp"
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ---------------------------------------------------------------------------
# Sandboxed out-of-process VST3 host (juce_vst_host.exe)
# ---------------------------------------------------------------------------
# Small GUI-subsystem executable that hosts the guitar FX chain / instrument
# VSTi in a separate crash domain. Audio is bridged over the shared-memory
# block described in include/dotcharter_vst_bridge.h; control rides a named
# pipe. Windows-only for now (the bridge uses Win32 kernel objects).
if(WIN32)
    add_executable(juce_vst_host WIN32
        src/host/VstHostMain.cpp
        include/dotcharter_vst_bridge.h)

    target_include_directories(juce_vst_host
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    target_link_libraries(juce_vst_host
        PRIVATE
            juce::juce_audio_basics
            juce::juce_audio_processors
            juce::juce_core
            juce::juce_events
            juce::juce_gui_basics
            juce::juce_gui_extra
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)

    target_compile_definitions(juce_vst_host
        PRIVATE
            JUCE_STANDALONE_APPLICATION=1
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
            JUCE_MODAL_LOOPS_PERMITTED=1
            JUCE_PLUGINHOST_VST3=1
            JUCE_PLUGINHOST_AU=0)

    set_target_properties(juce_vst_host PROPERTIES
        OUTPUT_NAME "juce_vst_host"
        WIN32_EXECUTABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# AI stem separation backend (sevagh/demucs.cpp, htdemucs + hdemucs_mmi)
# ---------------------------------------------------------------------------
# Off by default: the inference path pulls Eigen + a few KLoC of C++ and
# (more importantly) requires the user to download model weights at runtime.
# Turn ON to ship a self-contained native htdemucs implementation. When OFF
# the dca_stems_* C ABI stays available but reports "not supported", so the
# managed side gracefully falls back to the Python `demucs` CLI service.
option(DOTCHARTER_STEMS_DEMUCS_CPP "Build native htdemucs (demucs.cpp) inference into juce_sharp" OFF)
# p1-13: demucs.cpp has no released tags, so the default is PINNED to a commit
# SHA (upstream main as of 2026-08-29, read with `git ls-remote`). A branch
# name here floats and makes the stems build unreproducible; the DotCharter
# gate lints this line (Infrastructure/DemucsCppPinLintTests) and refuses
# anything that is not a 40-hex SHA or a v* tag. Override at configure time
# with -DDEMUCS_CPP_TAG=<commit-sha> to test a newer upstream; bump the
# default only deliberately. We still warn when a mutable ref is in use.
set(DEMUCS_CPP_TAG "f1206e9adeea103aef4a636b9e62297cf1f8e34e" CACHE STRING "demucs.cpp git tag/branch/sha to fetch")

if(DOTCHARTER_STEMS_DEMUCS_CPP)
    if(DEMUCS_CPP_TAG MATCHES "^(main|master|HEAD)$")
        message(WARNING "JuceSharp: DEMUCS_CPP_TAG='${DEMUCS_CPP_TAG}' is a mutable ref; pin to a commit SHA for reproducible stems builds.")
    endif()
    include(FetchContent)
    # Fetched without its own add_subdirectory: we link a hand-picked subset
    # of its src/ files directly so we don't drag in libnyquist (we use JUCE
    # WAV I/O instead) or the CLI driver mains.
    # demucs.cpp's top-level CMakeLists.txt defaults to USE_OPENBLAS=ON which
    # hard-requires a system BLAS that doesn't exist on a stock Windows box.
    # Force it OFF so the configure step succeeds; we don't actually link the
    # generated demucs.cpp.lib target anyway — we just compile its src/*.cpp
    # straight into juce_sharp with vendored Eigen.
    set(USE_OPENBLAS OFF CACHE BOOL "Disable OpenBLAS in demucs.cpp" FORCE)
    set(USE_AMD_AOCL OFF CACHE BOOL "" FORCE)
    set(USE_INTEL_MKL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        demucs_cpp
        GIT_REPOSITORY https://github.com/sevagh/demucs.cpp.git
        GIT_TAG        ${DEMUCS_CPP_TAG}
        # NO GIT_SHALLOW here, deliberately: DEMUCS_CPP_TAG is a COMMIT SHA (the
        # project has no released tags), and a shallow clone can only fetch a
        # ref the server advertises. Git servers advertise branches and tags,
        # not arbitrary commits, so `--depth 1` of a SHA fails on most hosts —
        # and where it succeeds it does so by accident of the SHA still being a
        # branch tip. JUCE and Rubber Band keep GIT_SHALLOW because both are
        # pinned to real TAGS.
        GIT_SUBMODULES "vendor/eigen")
    # p2-29: FetchContent_Populate is deprecated in CMake 3.30+. The new
    # idiom for "populate without adding to the build" is GetProperties +
    # MakeAvailable with the CMP0169 OLD policy set above. On 3.30+ we'd
    # instead use SOURCE_SUBDIR pointing at a non-existent dir, but that
    # syntax doesn't exist in 3.22 (our minimum), so we keep this pattern
    # and rely on the policy override.
    FetchContent_GetProperties(demucs_cpp)
    if(NOT demucs_cpp_POPULATED)
        FetchContent_Populate(demucs_cpp)
    endif()

    # p2-30: file(GLOB) caches at configure time and won't notice new
    # demucs.cpp source files if they appear after a 'git fetch' updates the
    # checkout. CONFIGURE_DEPENDS forces CMake to re-glob on every build,
    # which is exactly what we want for upstream-tracked sources we don't
    # own.
    file(GLOB DEMUCS_CPP_SOURCES CONFIGURE_DEPENDS
        "${demucs_cpp_SOURCE_DIR}/src/*.cpp")
    target_sources(juce_sharp PRIVATE
        ${DEMUCS_CPP_SOURCES}
        "${demucs_cpp_SOURCE_DIR}/cli-apps/threaded_inference.hpp")
    # p4.3: demucs's VENDORED Eigen must stay scoped to the demucs TUs (plus
    # StemSeparator.cpp, which includes demucs headers). Target-wide it would
    # shadow the Eigen that NeuralAudio/RTNeural pin for the namamp path when
    # both neural options are enabled — two vendored Eigens, one include
    # order bug. Per-source INCLUDE_DIRECTORIES keeps each stack on its own
    # copy.
    set_source_files_properties(
        ${DEMUCS_CPP_SOURCES}
        "${CMAKE_CURRENT_SOURCE_DIR}/src/StemSeparator.cpp"
        PROPERTIES INCLUDE_DIRECTORIES
        "${demucs_cpp_SOURCE_DIR}/src;${demucs_cpp_SOURCE_DIR}/cli-apps;${demucs_cpp_SOURCE_DIR}/vendor/eigen")
    target_compile_definitions(juce_sharp PRIVATE DOTCHARTER_STEMS_DEMUCS_CPP=1)

    # Demucs inference is heavily SIMD/parallel-friendly. Give the relevant
    # translation units AVX2 + fast-math + max-speed and (on MSVC) the LLVM
    # OpenMP runtime so Eigen's parallel kernels actually thread out.
    if(MSVC)
        set_source_files_properties(
            ${DEMUCS_CPP_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/StemSeparator.cpp"
            PROPERTIES COMPILE_FLAGS "/O2 /Ot /fp:fast /arch:AVX2 /openmp:llvm /DEIGEN_VECTORIZE /DEIGEN_NO_DEBUG")
    else()
        set_source_files_properties(
            ${DEMUCS_CPP_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/src/StemSeparator.cpp"
            PROPERTIES COMPILE_FLAGS "-O3 -mavx2 -mfma -ffast-math -fopenmp -DEIGEN_VECTORIZE -DEIGEN_NO_DEBUG")
    endif()

    # demucs.cpp uses OpenMP for the inner GEMM loops; link the runtime when
    # available. On MSVC the /openmp:llvm switch above already brings it in.
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(juce_sharp PRIVATE OpenMP::OpenMP_CXX)
    endif()

    message(STATUS "JuceSharp: native stem separation ENABLED (demucs.cpp @ ${DEMUCS_CPP_TAG})")
else()
    target_compile_definitions(juce_sharp PRIVATE DOTCHARTER_STEMS_DEMUCS_CPP=0)
    message(STATUS "JuceSharp: native stem separation DISABLED (Python demucs fallback will be used)")
endif()

# ---------------------------------------------------------------------------
# Neural amp capture playback ("namamp": mikeoliphant/NeuralAudio, MIT)
# ---------------------------------------------------------------------------
# Brings NAM Core (MIT) + RTNeural (BSD-3) + Eigen (MPL-2) — all
# closed-source-safe — and plays .nam AND .aidax captures through one API.
# Default ON: the shipped DLL must contain the namamp module or the managed
# catalogue mirror breaks, and the documented rebuild command passes no
# flags. Turn OFF for a network-free / minimal build (the module simply
# vanishes from the catalogue). NOTE: the DLL stays on the SSE2 baseline for
# distribution (plan §4.4) EXCEPT NeuralAudio + RTNeural, which are AVX2 since
# 2026-09-24 — see JUCESHARP_NEURAL_AVX2 below for what that costs.
option(DOTCHARTER_DSP_NEURAL "Build the namamp neural-capture module (NeuralAudio) into juce_sharp" ON)
# Pinned to a RELEASE TAG. (This used to read "NeuralAudio has no release tags"
# and pin the SHA 41d0cf6dddfbda55f2b5c7e02b8e0feb9fe58155 — that was wrong:
# upstream tags v0.0.1..v0.1.3, and the old SHA sat 9 days and 15 commits
# BEHIND v0.1.3.) Override with -DNEURALAUDIO_TAG=... if needed.
set(NEURALAUDIO_TAG "v0.1.3" CACHE STRING "NeuralAudio git sha/tag to fetch")
# Fetched by the commit NEURALAUDIO_TAG named when pinned (see JUCE_COMMIT).
set(NEURALAUDIO_COMMIT "b1137e1db2d43670e7dcf6345ade7808613cac20" CACHE STRING "NeuralAudio commit of NEURALAUDIO_TAG")

if(DOTCHARTER_DSP_NEURAL)
    include(FetchContent)
    # BUILD_NAMCORE stays pinned ON. It used to be forced because the old SHA
    # did not COMPILE with it off (an unbalanced #ifdef in NeuralModel.cpp,
    # fixed upstream by 7ac6a39 before v0.1.3); it stays on for the real
    # reason: NAM Core is the loader NeuralAudio falls back to for any A2
    # capture that is not one of the standard architectures its internal
    # static kernels cover, so turning it off would silently refuse a subset
    # of .nam files. FORCE also heals stale caches.
    set(BUILD_NAMCORE ON CACHE BOOL "NAM Core backend (fallback loader for non-standard A2 captures)" FORCE)
    # Submodule strategy: NON-recursive. NeuralAmpModelerCore's own nested
    # Dependencies (gitlab-hosted Eigen clones) fail intermittently AND are
    # never used — NeuralAudio compiles the NAM sources against RTNeural's
    # in-tree Eigen + json (see its SYSTEM include dirs). The only nested
    # submodule the build touches is RTNeural's xsimd (GitHub), which the
    # PATCH_COMMAND inits idempotently. Whole fetch stays on GitHub.
    # SOURCE_SUBDIR adds ONLY the NeuralAudio object library (not the C API /
    # utils).
    FetchContent_Declare(
        neuralaudio
        GIT_REPOSITORY https://github.com/mikeoliphant/NeuralAudio.git
        GIT_TAG        ${NEURALAUDIO_COMMIT}
        GIT_SUBMODULES "deps/NeuralAmpModelerCore;deps/RTNeural;deps/math_approx"
        GIT_SUBMODULES_RECURSE OFF
        PATCH_COMMAND  git -C deps/RTNeural submodule update --init modules/xsimd
        SOURCE_SUBDIR  NeuralAudio)
    FetchContent_MakeAvailable(neuralaudio)

    # NeuralAudio + RTNeural at AVX2 (MSVC). Every Eigen / RTNeural translation
    # unit — NeuralAudio's own sources (the WaveNet / LSTM kernels, NAM Core) and
    # RTNeural's one .cpp — gets the SAME flag, so no inline Eigen template is
    # instantiated at two different vector widths and no fixed-size Eigen type
    # has two alignments (EIGEN_MAX_ALIGN_BYTES follows __AVX__). PRIVATE on
    # purpose: RTNeural's own RTNEURAL_USE_AVX option adds /arch:AVX2 as a
    # PUBLIC option, which travels through NeuralAudio's LINK_PUBLIC into EVERY
    # juce_sharp source — so it stays OFF, and RTNEURAL_DEFAULT_ALIGNMENT keeps
    # the one value (16) every consumer sees. juce_sharp's single NeuralAudio
    # caller, NamAmpModule.cpp, sees only NeuralModel.h (no Eigen), a virtual
    # interface over float pointers.
    # THE COST: this code — including NeuralAudio's static registrations, which
    # run when the DLL LOADS — now needs an AVX2 CPU (Intel Haswell 2013+, AMD
    # Excavator 2015+ / every Zen). The rest of the DLL stays on the SSE2
    # baseline; -DJUCESHARP_NEURAL_AVX2=OFF restores it for NeuralAudio too.
    option(JUCESHARP_NEURAL_AVX2 "Compile NeuralAudio and RTNeural with /arch:AVX2 (MSVC)" ON)
    if(MSVC AND JUCESHARP_NEURAL_AVX2)
        target_compile_options(NeuralAudio PRIVATE /arch:AVX2)
        target_compile_options(RTNeural PRIVATE /arch:AVX2)
        message(STATUS "JuceSharp: NeuralAudio + RTNeural compiled with /arch:AVX2")
    endif()

    target_sources(juce_sharp PRIVATE
        src/dsp/NamAmpModule.cpp
        src/dsp/NamAmpModule.h)
    target_link_libraries(juce_sharp PRIVATE NeuralAudio)
    # NeuralModel.h pulls in nlohmann json; scope that include to the one
    # adapter TU (mirror of the demucs Eigen scoping above).
    set_source_files_properties(src/dsp/NamAmpModule.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${neuralaudio_SOURCE_DIR}/deps/RTNeural/modules/json")
    target_compile_definitions(juce_sharp PRIVATE DOTCHARTER_DSP_NEURAL=1)
    # The test targets compile the same TU. (This append does NOT "only reach
    # tests/", which is what the comment used to claim: JUCESHARP_CORE_SOURCES
    # was already consumed by add_library above, so appending here changes only
    # what tests/CMakeLists.txt sees — and tests/CMakeLists.txt then REMOVES
    # this very entry again, because the RT gate needs the debug CRT and
    # NeuralAudio is built with the release one. The append is therefore inert
    # today; it is kept so a future test target that CAN take the neural module
    # inherits it rather than having to rediscover the path.)
    list(APPEND JUCESHARP_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/NamAmpModule.cpp)
    list(APPEND JUCESHARP_CORE_DEFINITIONS DOTCHARTER_DSP_NEURAL=1)
    set(JUCESHARP_NEURAL_JSON_INCLUDE "${neuralaudio_SOURCE_DIR}/deps/RTNeural/modules/json")
    message(STATUS "JuceSharp: namamp neural module ENABLED (NeuralAudio @ ${NEURALAUDIO_TAG})")
else()
    target_compile_definitions(juce_sharp PRIVATE DOTCHARTER_DSP_NEURAL=0)
    list(APPEND JUCESHARP_CORE_DEFINITIONS DOTCHARTER_DSP_NEURAL=0)
    message(STATUS "JuceSharp: namamp neural module DISABLED")
endif()

# Follow the OPTION. This was hard-coded to 0, so a build configured with
# -DDOTCHARTER_STEMS_DEMUCS_CPP=ON compiled the DLL with the native backend
# (the target-level definition below the option handles that) and every TEST
# executable — which recompiles the same translation units from this list —
# with the stub. Two different StemSeparator.cpp behaviours from one configure.
if(DOTCHARTER_STEMS_DEMUCS_CPP)
    list(APPEND JUCESHARP_CORE_DEFINITIONS DOTCHARTER_STEMS_DEMUCS_CPP=1)
else()
    list(APPEND JUCESHARP_CORE_DEFINITIONS DOTCHARTER_STEMS_DEMUCS_CPP=0)
endif()

# ---------------------------------------------------------------------------
# Native tests (CTest)
# ---------------------------------------------------------------------------
# The RT gate lives here. It compiles JUCESHARP_CORE_SOURCES a second time with
# DCA_RTCHECK=1 and the debug CRT, drives the real device callback through the
# null device's manual-pump mode, and fails the build when the audio thread
# allocates, blocks, or grows a new lock. Nothing it needs is exported from the
# DLL — there is no test-only ABI surface to maintain.
option(JUCESHARP_BUILD_NATIVE_TESTS "Build the native test executables (RT gate)" ON)
if(JUCESHARP_BUILD_NATIVE_TESTS)
    if(DOTCHARTER_STEMS_DEMUCS_CPP)
        message(STATUS "JuceSharp: native tests DISABLED (demucs.cpp build needs per-source "
                       "include scoping that does not cross a subdirectory)")
    else()
        include(CTest)   # brings enable_testing()
        add_subdirectory(tests)
    endif()
endif()

# ---------------------------------------------------------------------------
# tools/tone_response — the frequency-response probe, EXCLUDE_FROM_ALL
# ---------------------------------------------------------------------------
# Header-only, no JUCE, no DLL: it includes the dcdsp cores directly and prints
# tables. It is how the "an in-path bass filter is a BOUNDED SHELF, never an
# unbounded high-pass — they SUM, so never add a fourth without measuring the
# cascade" rule is actually measured.
#
# It had NO CMake target at all (W4-D63) and the file said so on purpose —
# "NOT in CMake, on purpose" with a hand `cl` line — which meant nothing ever
# compiled it. It went on referring to a hand-transcribed copy of AmpCore's
# private voicing table for as long as that was true, because a copy nobody
# builds cannot fail to build.
#
# EXCLUDE_FROM_ALL, so `cmake --build .` does not pay for it and CI does not
# either, and NO add_test: this prints measurements for a human to read, it
# asserts nothing, and a ctest entry would imply a pass/fail it does not have.
# Build it deliberately:
#
#   cmake --build build --config Release --target juce_sharp_tone_response
#
# The point of the target is that the compiler now sees this file whenever
# somebody asks, which is what stops the next transcribed table.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/tone_response.cpp")
    add_executable(juce_sharp_tone_response EXCLUDE_FROM_ALL tools/tone_response.cpp)
    target_compile_features(juce_sharp_tone_response PRIVATE cxx_std_17)
    target_include_directories(juce_sharp_tone_response PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(juce_sharp_tone_response PRIVATE DOTCHARTER_DSP_NEURAL=0)
    if(MSVC)
        target_compile_options(juce_sharp_tone_response PRIVATE /O2)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(juce_sharp_tone_response PRIVATE Threads::Threads)
    endif()
endif()

# ---------------------------------------------------------------------------
# tools/voice_render — the chart voice's offline renderer, EXCLUDE_FROM_ALL
# ---------------------------------------------------------------------------
# Header-only like tone_response: plans and renders a note list (or the whole
# listening set) through the same src/dsp/voice/ code the engine uses and
# writes 16-bit WAVs. No add_test — tests/dsp/guitar_voice_test.cpp asserts.
#
#   cmake --build build --config Release --target juce_sharp_voice_render
#   juce_sharp_voice_render --listening-set <dir>
#   juce_sharp_voice_render --models <dir> --pack-dir <packs>   (#50: every model x pickup)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/voice_render.cpp")
    add_executable(juce_sharp_voice_render EXCLUDE_FROM_ALL tools/voice_render.cpp)
    target_compile_features(juce_sharp_voice_render PRIVATE cxx_std_17)
    target_include_directories(juce_sharp_voice_render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_options(juce_sharp_voice_render PRIVATE /O2 /bigobj)
    endif()
endif()

# p2-27: pick the .NET RID subdir from the actual target platform/arch
# instead of hard-coding win-x64. Mirrors `dotnet --list-runtimes` naming so
# the DLL lands in the right runtimes/<rid>/native folder for the C# wrapper
# to pick up via NativeFileLoadingContext.
if(WIN32)
    set(_JUCESHARP_RID_OS "win")
elseif(APPLE)
    set(_JUCESHARP_RID_OS "osx")
elseif(UNIX)
    set(_JUCESHARP_RID_OS "linux")
else()
    set(_JUCESHARP_RID_OS "unknown")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
        set(_JUCESHARP_RID_ARCH "arm64")
    else()
        set(_JUCESHARP_RID_ARCH "x64")
    endif()
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm|ARM)")
        set(_JUCESHARP_RID_ARCH "arm")
    else()
        set(_JUCESHARP_RID_ARCH "x86")
    endif()
endif()
set(JUCESHARP_RID "${_JUCESHARP_RID_OS}-${_JUCESHARP_RID_ARCH}" CACHE STRING ".NET RID for runtime deploy path")
set(JUCESHARP_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JuceSharp/runtimes/${JUCESHARP_RID}/native")
message(STATUS "JuceSharp: deploying native DLL to runtimes/${JUCESHARP_RID}/native")
add_custom_command(TARGET juce_sharp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${JUCESHARP_RUNTIME_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:juce_sharp>"
            "${JUCESHARP_RUNTIME_DIR}/$<TARGET_FILE_NAME:juce_sharp>"
    COMMENT "Deploying juce_sharp.dll to JuceSharp/runtimes/${JUCESHARP_RID}/native")

if(WIN32)
    add_custom_command(TARGET juce_vst_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${JUCESHARP_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:juce_vst_host>"
                "${JUCESHARP_RUNTIME_DIR}/$<TARGET_FILE_NAME:juce_vst_host>"
        COMMENT "Deploying juce_vst_host.exe to JuceSharp/runtimes/${JUCESHARP_RID}/native")
endif()

# ---------------------------------------------------------------------------
# Install (p3-49)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS juce_sharp
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
# The sandbox child ships WITH the DLL or the VST sandbox silently degrades to
# in-process hosting: the engine looks for juce_vst_host.exe next to
# juce_sharp.dll. The POST_BUILD deploy above put it in runtimes/ and `install`
# did not, so an installed tree was missing half the feature.
if(WIN32)
    install(TARGETS juce_vst_host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
