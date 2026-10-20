// NamAmpModule.h — "namamp": neural amp capture playback (0.16 §4.4).
//
// Wraps mikeoliphant/NeuralAudio (MIT; NAM Core MIT + RTNeural BSD-3 +
// Eigen MPL-2 under the hood) as a dcrig DspModule. Loads .nam / .aidax
// captures via the v22 string param "modelPath". The model file parse +
// prewarm happens on a dedicated worker thread (message-thread setStringParam
// only queues it); the audio thread swaps to the new model via an atomic
// shared_ptr and NEVER allocates, locks, or frees a model.
//
// AMP FACE (0.36). A capture is one amp at ONE knob setting and has no
// controls of its own, so the module wraps it in an honest face:
// gain (pre-capture drive) / bass / mid / treble / presence / resonance /
// master, plus the original inputDb / outputDb calibration trims. Every knob's
// default is NEUTRAL and neutral is a STRICT no-op — bit-identical to the
// pre-0.36 path, so existing presets and captures are unchanged. The DSP and
// the reasoning (why the tone stack is POST-capture, why gain is PRE) live in
// cores/dcdsp_ampface.h.
//
// SAMPLE RATE (0.36). The module used to dry-BYPASS whenever the engine rate
// was not the capture's own rate (48 kHz for every .nam), which was survivable
// for an optional pedal and fatal for a default amp. It now RESAMPLES around
// the capture (cores/dcdsp_detail_src.h): the converters are built on the
// worker thread as part of the loaded model, so the audio thread still never
// allocates, and the capture always runs at its native rate. Measured round
// trip 44.1 -> 48 -> 44.1 kHz: flat within 0.005 dB from 20 Hz to 16 kHz,
// +1.42 ms latency. "rateMismatch" survives as a poll-only param but now only
// fires if the converters could not be built at all; "resampling" reports
// whether they are in the path.
//
// Only compiled when DOTCHARTER_DSP_NEURAL is enabled (CMake, default ON).
#pragma once

#include "DspModule.h"

#include <memory>

namespace dcrig
{

// Catalogue metadata + factory hook for DspModuleFactory.cpp (kept out of
// the factory TU so NeuralAudio's headers stay scoped to NamAmpModule.cpp).
ModuleTypeInfo namAmpTypeInfo();
std::unique_ptr<DspModule> createNamAmpModule();

// ---- log sink (W4-D38) -----------------------------------------------------
//
// `RetirementQueue::takeOverflowWarning()` has existed since the queue did and
// had NO CALLER: past `kSoftCap` parked captures the queue raised an
// edge-triggered flag that nothing ever read, so the one thing the unbounded
// queue's own comment says must be visible ("unbounded with no way to SEE it is
// the part that was wrong") still was not.
//
// A sink OF ITS OWN rather than reaching for `DspChain::logSink()`, and the
// reason is a LINK constraint, not taste: `juce_sharp_nam_lifetime` compiles
// exactly two translation units — nam_lifetime.cpp and NamAmpModule.cpp — with
// no DspModuleFactory.cpp and no JUCE include dirs. `DspChain.h` calls
// `moduleCatalogue()`, which lives in the factory TU, so including it here
// would break that target's link outright.
//
// Same shape as `AudioEngine::setLogSink` and `DspChain::setLogSink`:
// dotcharter_audio.cpp installs a trampoline into `emitLog` at static-init
// time, and unset it is a no-op so the native test executables are unaffected.
// `level` mirrors dca_log_level (0 trace … 4 error) and the ABI TU
// static_asserts that kLogWarn agrees.
using NamLogSink = void (*)(int level, const char* message);
void setNamLogSink(NamLogSink sink) noexcept;
constexpr int kNamLogWarn = 3;

// Test seam: how many overflow warnings have been EMITTED (not raised). The
// flag is edge-triggered, so "once per excursion" is the property worth
// asserting and a counter is the only way to see it from outside.
unsigned long namOverflowWarningsForTest() noexcept;

} // namespace dcrig
