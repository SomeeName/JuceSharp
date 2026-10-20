#pragma once

// Idle sleep for a LIVE seat's tone rig (2026-09-24).
//
// WHY. A restored rig runs on every device block from engine start, on every
// screen, whatever the input: the owner's gate -> two NAM captures -> cab -> eq
// -> gain measured ~360 us per 128-frame block at 48 kHz, 13.5 % of one core,
// and a guitar idling at -70 dBFS hum cost exactly what a played note costs.
//
// WHAT. The DEVICE callback (AudioEngine::processDspRigLive) skips
// `DspChain::process` for a seat whose rig has provably been producing nothing
// audible, and zeroes the rig's output instead:
//
//   * FALL ASLEEP when what the seat's rig contributes to the device output has
//     stayed below kQuietPeak (-90 dBFS) for the chain's HOLD — at least
//     kBaseHoldSeconds (5 s), longer when a node declares a longer settle (a
//     delay line that can still emit an echo, a gate's release) — while the
//     rig's INPUT stayed below -40 dBFS and did not fall by more than
//     kWakeRatio between the two halves of the hold (a stationary floor, not
//     the tail of a note the rig muted). The largest input peak of the hold is
//     remembered as the idle floor.
//   * WHILE ASLEEP the rig outputs zeros, which is what it was outputting to
//     within -90 dBFS.
//   * WAKE on the SAME block whose input peak exceeds kWakeRatio x the idle
//     floor (never below kWakeFloor). That block is processed, so no attack is
//     lost. Also wake — and restart the hold — on anything that could change
//     what the rig or the stages after it do: a chain edit or param write (the
//     chain's edit generation), an input-VST attach, the input trim, the
//     monitor gain, the input EQ's route / trim / coefficients, the sandbox
//     bridge, and an engine rebuild.
//
// The OFFLINE renders (dspProcessOffline / dspProcessOfflineAutomated) call
// DspChain::process directly and never see any of this: a bounce is exact.
//
// WHY RESUMING IS RIGHT (and a reset is not). A sleeping rig's modules keep the
// state they had at the end of the last awake block: filter memories, gate and
// compressor envelopes, NAM receptive-field history, delay lines. A rig only
// sleeps after its output has been quiet for the whole hold, and the hold is
// at least as long as the longest settle any node declares, so that state is
// already the QUIET state — the one a never-slept rig is also in when the pick
// arrives, up to differences at the idle-noise level. A reset would instead
// hand the attack a state no never-slept rig is ever in (a cold NAM history, a
// gate at its initial envelope), which is a real difference, not a
// noise-level one. `juce_sharp_rig_idle_sweep` measures exactly this for every
// type in kIdleSleepTypes: a slept and a never-slept instance are driven
// through the same pluck -> idle -> pluck, and the outputs from the waking
// block on must agree within kWakeDiffTolerance.
//
// WHAT MAY SLEEP IS AN ALLOW-LIST. A node whose type is NOT in kIdleSleepTypes
// keeps the whole seat awake. The default is the safe direction on purpose: a
// module nobody measured (a free-running LFO whose phase a sleep would freeze,
// a looper, a freeze pedal) must not be skipped on a guess. Adding a type means
// adding it here AND the sweep proving it.
//
// Header-only and free of JUCE so the sweep and the RT gate drive the SAME
// decision logic the engine runs.

#include "DspModule.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dcrig
{
namespace idle
{
/// -90 dBFS. What a seat's rig contributes to the device output must stay below
/// this for the whole hold before the rig may sleep, and it is therefore the
/// bound on what the zeroed output leaves out.
inline constexpr float kQuietPeak = 3.16227766e-5f;
/// The rig wakes on the block whose input peak exceeds this multiple of the
/// idle floor. sqrt(2) (+3 dB), not the 2x (+6 dB) first proposed: the only
/// input a sleeping rig can miss is one between the idle floor and this ratio,
/// and halving that band costs nothing measurable — the peak of stationary
/// noise over a 2 s hold is ~4.4 sigma, and one sample above 6.2 sigma is a
/// ~3e-10 event.
inline constexpr float kWakeRatio = 1.41421356f;
/// 2^-24 (-144 dBFS): below the resolution of any 24-bit converter, so a real
/// interface's noise floor always governs the wake level and only an input of
/// exact digital silence falls back to this. It keeps float rounding dust from
/// waking a rig that idled on zeros; any real signal clears it.
inline constexpr float kWakeFloor = 5.9604645e-8f;
/// -40 dBFS. A rig whose INPUT is louder than this is being played, whatever
/// its output says (a volume pedal at zero, a gate set above the pick): it
/// never sleeps, so "the idle floor" always means noise, never a performance.
inline constexpr float kMaxIdleInputPeak = 0.01f;
/// The minimum hold, in seconds, before a quiet rig may sleep. FIVE, not the
/// two first proposed, and measured, not guessed: a module's slow internal
/// states (a DC blocker, an amp's sag follower) are still decaying at 2 s, and
/// a high-gain stage after them resurfaces that residue at the next pick — the
/// in-house gate -> overdrive -> amp -> cab -> eq -> gain chain differed from a
/// never-slept one by -66 dBFS after a 2 s hold, -79 dBFS after 3 s and not at
/// all (bit-exact) after 5-6 s of digital silence. With FTZ on, a decaying
/// state reaches exact zero, so a long enough hold makes the frozen state the
/// SAME state, not a nearby one.
inline constexpr double kBaseHoldSeconds = 5.0;
/// The sweep's bar for "the waking block onward is the same sound": the
/// largest sample difference between a slept and a never-slept instance, as a
/// linear peak. -80 dBFS — see juce_sharp_rig_idle_sweep for the measured
/// numbers per type, which sit far below it.
inline constexpr float kWakeDiffTolerance = 1.0e-4f;
} // namespace idle

/// One module type that may sleep, and how long its input has to have been
/// quiet before skipping it cannot change anything it outputs later: the
/// longest delay line that could still emit, or the longest release still
/// settling. `settle` reads the module's CURRENT params (message thread, via
/// getParam) so a gate at 70 ms release does not pay for one at 1000 ms;
/// nullptr means the base hold covers every setting.
struct IdleSleepType
{
    const char* typeId;
    double (*settle)(const DspModule& m);
};

namespace idle
{
inline double param(const DspModule& m, const char* id, double fallback) noexcept
{
    float v = 0.0f;
    return m.getParam(id, v) ? (double) v : fallback;
}
/// A gate's envelope heads for its CLOSED floor (-120 dB): after 10 tau the
/// frozen gain differs from the settled one by 1e-6 x e^-10 in absolute terms,
/// which nothing downstream can resurface.
inline constexpr double kGateSettleTaus = 10.0;
/// A compressor's or limiter's gain heads for UNITY, so a residue there is a
/// RELATIVE gain error on the whole next note — and a high-gain stage after it
/// turns that into shifted clipping edges (compressor -> rat measured -60 dBFS
/// peak difference after a 10 tau settle). 40 tau is where a double-precision
/// gain has reached 1.0 exactly (e^-37 < 2^-53).
inline constexpr double kUnitySettleTaus = 40.0;
inline double gateSettle(const DspModule& m)
{
    return (param(m, "holdMs", 500.0) + kGateSettleTaus * param(m, "releaseMs", 1000.0) + 20.0) * 0.001;
}
inline double compressorSettle(const DspModule& m)
{
    return (kUnitySettleTaus * (param(m, "releaseMs", 1000.0) + param(m, "attackMs", 100.0)) + 50.0) * 0.001;
}
inline double limiterSettle(const DspModule& m)
{
    return (kUnitySettleTaus * param(m, "releaseMs", 500.0) + param(m, "lookaheadMs", 5.0) + 50.0) * 0.001;
}
} // namespace idle

/// THE ALLOW-LIST: module types whose memory is FINITE and shorter than the
/// hold. After a hold of idle input, such a module's whole state is a function
/// of that idle input and nothing older — an EQ's biquads, a cab's 85 ms IR, a
/// NAM capture's receptive field, a drive's oversampling filters, a gate's
/// envelope once its release has run out. That is what makes resuming it the
/// same sound as never having stopped.
///
/// Deliberately NOT here (the sweep's `--discover` has the numbers):
///   * long or infinite memory — delays and echo trains, every reverb,
///     resonant filters (svf, comb, resonator, wah, envfilter, autowah), the
///     feedbacker. Each passes ALONE, but its frozen tail sits BELOW the
///     threshold at its own output and a stage after it can reveal it: a delay
///     in front of a closed gate froze its last echoes and replayed them into
///     the next pick (random-chain sweep: hallreverb -> drives -57 dBFS,
///     convreverb -> amp -79 dBFS).
///   * free-running LFOs and modulated tanks (tremolo, chorus, phaser, rotary,
///     the modulated reverbs) — a sleep would freeze their phase;
///   * pitch shifters and trackers (grain / window / PLL phase);
///   * capture devices (looper, freeze, glitch, granular, the reverse windows,
///     tape stop) — they play back what they hold;
///   * envelope stages whose frozen state measured above tolerance somewhere
///     (expander — -67 dBFS through two fuzzes even after a 6.6 s settle —
///     optocomp, multiband, ducker, noisereducer, swell, the vocoders).
/// A type added here must pass juce_sharp_rig_idle_sweep alone AND in its
/// random chains.
inline constexpr IdleSleepType kIdleSleepTypes[] = {
    // Gain, static filters, routing.
    { "gain", nullptr },       { "volume", nullptr },     { "eq", nullptr },
    { "graphiceq", nullptr },  { "tilt", nullptr },       { "midside", nullptr },
    { "widener", nullptr },    { "splitter", nullptr },   { "merger", nullptr },
    // Cabs, mics, acoustic body.
    { "cab", nullptr },        { "micpos", nullptr },     { "acousticsim", nullptr },
    // Drives, fuzzes, amps, the neural capture.
    { "overdrive", nullptr },  { "distortion", nullptr }, { "fuzz", nullptr },
    { "boost", nullptr },      { "klon", nullptr },       { "bigmuff", nullptr },
    { "rat", nullptr },        { "ds1", nullptr },        { "hm2", nullptr },
    { "tapesat", nullptr },    { "bassod", nullptr },     { "octfuzz", nullptr },
    { "amp", nullptr },        { "acpre", nullptr },      { "poweramp", nullptr },
    { "namamp", nullptr },
    // Dynamics: the hold follows the release so the envelope has landed.
    { "gate", &idle::gateSettle },
    { "compressor", &idle::compressorSettle },
    { "limiter", &idle::limiterSettle },
    { "transient", nullptr },
    { "sustainer", nullptr },
};

/// Settle for a node, or a negative number when its type may not sleep.
/// Message thread (it reads params through getParam).
inline double idleSettleSecondsFor(const DspModule& m)
{
    const char* typeId = m.typeId();
    if (typeId == nullptr) return -1.0;
    for (const auto& t : kIdleSleepTypes)
        if (std::strcmp(t.typeId, typeId) == 0) return t.settle != nullptr ? t.settle(m) : 0.0;
    return -1.0;
}

/// Is this type on the allow-list at all?
inline bool idleSleepAllowed(const char* typeId) noexcept
{
    if (typeId == nullptr) return false;
    for (const auto& t : kIdleSleepTypes)
        if (std::strcmp(t.typeId, typeId) == 0) return true;
    return false;
}

/// Hold in samples for a chain whose longest declared settle is
/// `maxSettleSeconds` (negative = some node may not sleep -> -1, never).
inline long long idleHoldSamples(double maxSettleSeconds, double sampleRate) noexcept
{
    if (maxSettleSeconds < 0.0 || ! (sampleRate > 0.0)) return -1;
    const double s = maxSettleSeconds > idle::kBaseHoldSeconds ? maxSettleSeconds : idle::kBaseHoldSeconds;
    return (long long) std::ceil(s * sampleRate);
}

/// Everything downstream of the rig, or around it, whose change must wake a
/// sleeping seat and restart its hold. Compared BITWISE (a float that moved by
/// one ulp is a change; NaN compares equal to itself).
struct RigIdleContext
{
    uint64_t chainGeneration      = 0;
    uint64_t inputChainGeneration = 0;
    float    inputGain            = 0.0f;
    float    monitorGain          = 0.0f;
    float    eqTrim               = 0.0f;
    int      eqRoute              = 0;
    int      captureChannel       = 0;
    int      players              = 0;
    int      bridgeEnabled        = 0;

    bool sameAs(const RigIdleContext& o) const noexcept
    {
        return chainGeneration == o.chainGeneration
            && inputChainGeneration == o.inputChainGeneration
            && bitsEqual(inputGain, o.inputGain)
            && bitsEqual(monitorGain, o.monitorGain)
            && bitsEqual(eqTrim, o.eqTrim)
            && eqRoute == o.eqRoute
            && captureChannel == o.captureChannel
            && players == o.players
            && bridgeEnabled == o.bridgeEnabled;
    }

private:
    static bool bitsEqual(float a, float b) noexcept
    {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, &a, sizeof x);
        std::memcpy(&y, &b, sizeof y);
        return x == y;
    }
};

/// The per-seat decision. AUDIO THREAD ONLY (plain members, no atomics): the
/// device callback owns it, and the engine resets it in prepareMasterBus, where
/// no callback is in flight. No allocation, no lock, no logging.
///
/// THE HOLD IS TWO HALVES, and the second half has to be as loud as the first
/// (within kWakeRatio). That is what makes the idle floor a STATIONARY floor —
/// hum, hiss, digital silence — and never the tail of a note the rig itself
/// muted: a rig whose output is quiet while its input is still decaying (a
/// closed gate, a volume pedal at zero) keeps sliding the window until the
/// input has settled, so everything a module remembers from the window is at
/// the level of the floor it will wake above. Without it a delay line in front
/// of a closed gate froze the last echoes of the note and replayed them into
/// the next pick (the sweep's chain case measured -67 dBFS of it).
class RigIdleSleep
{
public:
    void reset() noexcept { *this = RigIdleSleep{}; }

    /// Top of the seat's block, BEFORE the rig. `inputPeak` is the peak of the
    /// rig's input block. Returns true when the rig must be SKIPPED this block
    /// (the caller zeroes the rig's output channels).
    bool beginBlock(const RigIdleContext& ctx, float inputPeak, bool forceAwake) noexcept
    {
        began_ = true;
        skippedThisBlock_ = false;
        pendingInputPeak_ = inputPeak;
        const bool changed = ! haveContext_ || ! ctx.sameAs(context_);
        context_ = ctx;
        haveContext_ = true;
        if (changed || forceAwake)
        {
            wake();
            return false;
        }
        if (asleep_)
        {
            // `!(a <= b)` so a NaN input peak wakes rather than sleeps on.
            if (! (inputPeak <= wakeLevel_))
            {
                wake();
                return false;
            }
            ++skippedBlocks_;
            skippedThisBlock_ = true;
            return true;
        }
        return false;
    }

    /// End of the seat's block. `heardPeak` is what the rig contributed to the
    /// device output this block (after every stage downstream of it), `mayRest`
    /// is false whenever something makes sleeping unsafe regardless of level
    /// (an input VST in the chain, metering, a fade in progress, ...). Ignored
    /// for a block the rig skipped.
    void endBlock(float heardPeak, int numFrames, long long holdSamples, bool mayRest) noexcept
    {
        if (! began_) return;
        began_ = false;
        if (skippedThisBlock_) return;
        if (! mayRest || holdSamples < 0 || ! (heardPeak < idle::kQuietPeak)
            || ! (pendingInputPeak_ <= idle::kMaxIdleInputPeak))
        {
            restartWindow();
            return;
        }
        if (pendingInputPeak_ > segPeak_) segPeak_ = pendingInputPeak_;
        segSamples_ += numFrames > 0 ? numFrames : 0;
        const long long half = (holdSamples + 1) / 2;
        if (segSamples_ < half) return;

        // A full half is in. With the previous half, that is a whole hold.
        if (haveFirstHalf_ && ! (firstHalfPeak_ > segPeak_ * idle::kWakeRatio))
        {
            asleep_ = true;
            idlePeak_ = firstHalfPeak_ > segPeak_ ? firstHalfPeak_ : segPeak_;
            const float floor = idlePeak_ > idle::kWakeFloor ? idlePeak_ : idle::kWakeFloor;
            wakeLevel_ = floor * idle::kWakeRatio;
            ++sleeps_;
            return;
        }
        // Either this was the first half, or the input was still falling:
        // slide, so the half just finished becomes the first half of the next
        // window.
        firstHalfPeak_ = segPeak_;
        haveFirstHalf_ = true;
        segPeak_ = 0.0f;
        segSamples_ = 0;
    }

    bool      asleep()        const noexcept { return asleep_; }
    float     wakeLevel()     const noexcept { return wakeLevel_; }
    float     idlePeak()      const noexcept { return idlePeak_; }
    uint64_t  sleeps()        const noexcept { return sleeps_; }
    uint64_t  wakes()         const noexcept { return wakes_; }
    uint64_t  skippedBlocks() const noexcept { return skippedBlocks_; }

private:
    void restartWindow() noexcept
    {
        haveFirstHalf_ = false;
        firstHalfPeak_ = 0.0f;
        segPeak_ = 0.0f;
        segSamples_ = 0;
    }

    void wake() noexcept
    {
        if (asleep_) ++wakes_;
        asleep_ = false;
        skippedThisBlock_ = false;
        idlePeak_ = 0.0f;
        restartWindow();
    }

    RigIdleContext context_{};
    bool      haveContext_      = false;
    bool      asleep_           = false;
    bool      began_            = false;
    bool      skippedThisBlock_ = false;
    bool      haveFirstHalf_    = false;
    float     pendingInputPeak_ = 0.0f;
    float     firstHalfPeak_    = 0.0f;
    float     segPeak_          = 0.0f;
    long long segSamples_       = 0;
    float     idlePeak_         = 0.0f;
    float     wakeLevel_        = 0.0f;
    uint64_t  sleeps_           = 0;
    uint64_t  wakes_            = 0;
    uint64_t  skippedBlocks_    = 0;
};

/// Peak |x| over `numChannels` x `numFrames`. A NaN sample is skipped
/// (`fabs(NaN) > p` is false); every buffer this reads has been scrubbed by the
/// chain or the engine before it gets here.
inline float blockPeak(const float* const* ch, int numChannels, int numFrames) noexcept
{
    float p = 0.0f;
    for (int c = 0; c < numChannels; ++c)
    {
        const float* x = ch[c];
        if (x == nullptr) continue;
        for (int i = 0; i < numFrames; ++i)
        {
            const float a = std::fabs(x[i]);
            if (a > p) p = a;
        }
    }
    return p;
}

} // namespace dcrig
