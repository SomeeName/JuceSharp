#pragma once

#include <algorithm>
#include <cmath>

namespace dotcharter {

/// Pure, framework-free clock/drift controller for the streaming-mirror
/// consumer. It owns NO audio buffers and has ZERO JUCE (or any other)
/// dependency so the exact math can be unit-tested in isolation
/// (`tests/JuceSharp.Tests/MirrorClockControllerTests.cpp`).
///
/// The mirror runs two independent audio clocks: the PRIMARY engine device
/// feeds a lock-free FIFO at `engineRate`, and a separate WASAPI mirror device
/// drains it at `deviceRate`. Left alone, any rate difference (48k vs a 44.1k
/// virtual cable) or crystal drift makes the FIFO fill or empty continuously →
/// over/underruns → crackle. This controller turns the drain into an
/// asynchronous sample-rate conversion with a target-latency pre-roll and a
/// fill-locked proportional drift trim, so the buffer sits at `targetFrames`
/// and never walks into either wall.
///
/// Per mirror callback the consumer calls `step(currentFill, outFrames)` and
/// acts on the returned decision: it runs a per-channel interpolator at
/// `effectiveRatio`, reading up to `inputFramesNeeded` engine frames from the
/// FIFO. `effectiveRatio` is the interpolator "speedRatio": the number of
/// engine (input) frames consumed per device (output) frame.
struct MirrorClockDecision
{
    /// true  -> run the resampler this block.
    /// false -> emit silence (still priming, or a post-prime underrun gap).
    bool   produce           = false;
    /// Interpolator speedRatio = engine input frames per device output frame.
    double effectiveRatio    = 1.0;
    /// Engine-rate frames the consumer must have available / read into scratch
    /// to produce `outFrames` cleanly (includes interpolator look-ahead guard).
    /// 0 when `produce` is false.
    int    inputFramesNeeded = 0;
    /// Controller primed state AFTER this decision (exposed for tests/telemetry).
    bool   primed            = false;
};

class MirrorClockController
{
public:
    /// (Re)configure for a new clock pair. Resets the priming state so the
    /// consumer rebuilds the target pre-roll from scratch.
    void configure(double engineRate, double deviceRate, int targetFrames) noexcept
    {
        engineRate_   = (engineRate > 0.0) ? engineRate : 48000.0;
        deviceRate_   = (deviceRate > 0.0) ? deviceRate : 48000.0;
        // W4-N18: fall back to the DEFAULT depth, not to 0. The two rates above
        // already substitute a sane value for a nonsense one; this line used to
        // substitute a value that DISABLES THE CLASS PERMANENTLY, because
        // `decide()` only primes on `targetFrames_ > 0 && fill >= targetFrames_`
        // and then returns produce=false for ever. A silent mirror with no
        // error anywhere is the worst of the three possible answers, and the
        // argument is one a host can pass by simply not having computed the
        // depth yet. Every shipping caller passes targetFramesForRate(...),
        // which is why this was a latent trap rather than a live bug.
        targetFrames_ = (targetFrames > 0) ? targetFrames : targetFramesForRate(engineRate_);
        reset();
    }

    void setEngineRate(double r) noexcept { if (r > 0.0) engineRate_ = r; }
    void setDeviceRate(double r) noexcept { if (r > 0.0) deviceRate_ = r; }

    /// Drop back to the un-primed state (used on a stream break / re-open).
    void reset() noexcept { primed_ = false; underrunFrames_ = 0; }

    double nominalRatio() const noexcept { return engineRate_ / deviceRate_; }
    bool   primed()       const noexcept { return primed_; }
    int    targetFrames() const noexcept { return targetFrames_; }
    double engineRate()   const noexcept { return engineRate_; }
    double deviceRate()   const noexcept { return deviceRate_; }

    /// Decide what the consumer should do for one device callback.
    ///  @param currentFill  engine-rate frames currently readable in the FIFO.
    ///  @param outFrames     device-rate output frames requested this callback.
    MirrorClockDecision step(int currentFill, int outFrames) noexcept
    {
        MirrorClockDecision d;
        d.effectiveRatio = nominalRatio();
        d.primed         = primed_;
        if (outFrames <= 0)
            return d;

        // --- Pre-roll: don't drain until the FIFO holds the full target ---
        // pre-roll. While filling we emit silence and consume nothing.
        if (!primed_)
        {
            if (targetFrames_ > 0 && currentFill >= targetFrames_)
            {
                primed_         = true;
                underrunFrames_ = 0;
            }
            else
            {
                d.primed = false;
                return d; // produce == false, inputFramesNeeded == 0
            }
        }

        // --- Fill-locked drift trim (asynchronous SRC) --------------------
        // Proportional to the normalised fill error, saturating at one full
        // target's worth of error, clamped to +/-kMaxTrim. When the FIFO is
        // over-full (fill > target) the trim is POSITIVE, so the effective
        // ratio rises and the consumer eats MORE engine frames per output
        // frame -> the buffer drains back toward target. Under-full -> the
        // ratio dips and it drains slower -> the buffer refills. This absorbs
        // both the nominal rate error and slow crystal drift; at steady state
        // the buffer parks at `targetFrames`.
        const double nominal = nominalRatio();
        const double normErr = (targetFrames_ > 0)
            ? static_cast<double>(currentFill - targetFrames_) / static_cast<double>(targetFrames_)
            : 0.0;
        const double trim = std::clamp(normErr, -1.0, 1.0) * kMaxTrim;
        const double eff  = nominal * (1.0 + trim);

        const int needed = static_cast<int>(std::ceil(static_cast<double>(outFrames) * eff)) + kInterpGuard;

        if (currentFill >= needed)
        {
            underrunFrames_      = 0;
            d.produce            = true;
            d.effectiveRatio     = eff;
            d.inputFramesNeeded  = needed;
            d.primed             = true;
            return d;
        }

        // --- Post-prime underrun ------------------------------------------
        // Not enough engine input to fill this block. Emit silence for the
        // gap and consume NOTHING so the little data we have accumulates. Do
        // NOT thrash the primed state on a brief hiccup: only fall back to the
        // full pre-roll once starvation has persisted for kReprimeSeconds
        // (e.g. the producer genuinely stopped), so the jitter margin is
        // rebuilt when audio returns.
        underrunFrames_ += outFrames;
        if (underrunFrames_ >= static_cast<int>(deviceRate_ * kReprimeSeconds))
        {
            primed_         = false;
            underrunFrames_ = 0;
        }
        d.effectiveRatio = eff;
        d.primed         = primed_;
        return d; // produce == false, inputFramesNeeded == 0
    }

    /// Convenience: target pre-roll frames for a given engine rate.
    static int targetFramesForRate(double engineRate) noexcept
    {
        return static_cast<int>(std::lround(((engineRate > 0.0) ? engineRate : 48000.0) * kTargetSeconds));
    }

    // Tunables (exposed so the consumer + tests share one source of truth).
    static constexpr double kTargetSeconds  = 0.150; // pre-roll depth (~150 ms) — hides jitter for streaming
    static constexpr double kMaxTrim        = 0.005; // +/-0.5% ratio nudge cap (~8.5 cents worst case — inaudible)
    static constexpr double kReprimeSeconds = 0.250; // sustained silence before rebuilding the pre-roll
    static constexpr int    kInterpGuard    = 4;     // spare input frames for the interpolator look-ahead

private:
    double engineRate_     = 48000.0;
    double deviceRate_     = 48000.0;
    int    targetFrames_   = 0;
    bool   primed_         = false;
    int    underrunFrames_ = 0; // device frames emitted as silence during the current starvation run
};

} // namespace dotcharter
