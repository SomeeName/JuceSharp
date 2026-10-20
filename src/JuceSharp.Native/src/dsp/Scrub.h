#pragma once

#include <cmath>

// One implementation of "a non-finite sample never reaches a stateful stage".
//
// WHY THIS IS A SHARED HEADER AND NOT A DspChain PRIVATE
// -----------------------------------------------------
// A single NaN or Inf sample entering an IIR biquad is permanent: the filter
// feeds its own output back, so every subsequent sample is NaN too and the
// band is silent for the rest of the session. juce::dsp::IIR::Filter::reset()
// is the way out, and the v29 EQ only calls it on an OFF -> ON transition
// (AudioEngine::applyEqBank, deliberately — resetting a running band on every
// knob tick clicks), so nothing in a normal session ever clears it.
//
// DspChain::process has scrubbed its input and every stage boundary since the
// chain existed. The stages the ENGINE owns did not, and two of them take
// samples from code this project does not control:
//
//   * AudioEngine::processInputChain runs the user's VST3 input plugins,
//   * RemoteVstBridge::process memcpys a block straight out of a child
//     process's shared-memory mapping.
//
// Either can hand back a NaN — a bad plugin, a denormal blow-up, a child that
// wrote a partial block — and the parametric EQ, the three fixed master filters
// and the streaming mirror all sat downstream of that with nothing in between.
//
// So the rule is: EVERY UNTRUSTED STAGE IS FOLLOWED BY A SCRUB. Cheap relative
// to the work either stage just did (one compare per sample, no branch taken in
// the normal case), and the alternative is a guitar that goes silent until the
// app is restarted.
namespace dcdsp
{

/// Replace any non-finite (NaN/Inf) sample in `ch[0..nch)` with 0.
/// Audio-thread safe: no allocation, no lock, no syscall.
inline void scrubNonFinite(float* const* ch, int nch, int n) noexcept
{
    if (ch == nullptr) return;
    for (int c = 0; c < nch; ++c)
        if (ch[c] != nullptr)
            for (int i = 0; i < n; ++i)
                if (! std::isfinite(ch[c][i])) ch[c][i] = 0.0f;
}

} // namespace dcdsp
