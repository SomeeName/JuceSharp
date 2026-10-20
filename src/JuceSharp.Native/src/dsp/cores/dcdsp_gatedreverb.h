// dcdsp_gatedreverb.h — the 80s gated-reverb drum sound (pure dcdsp core).
//
// Models a dense hall recorded through a noise gate keyed off the SOURCE: the
// tail is enormous while the drum is sounding and is cut dead a fixed time
// after it stops, so the reverb reads as part of the hit rather than as a
// space. Topology: predelay -> diffusers -> 8-line FDN (dcdsp_hallreverb.h's
// tank, reused whole) -> VCA -> mix. The VCA's control signal comes from the
// DRY input's envelope, never from the wet — that is what makes it a gate and
// not a ducker.
//
// Easy to get wrong: the hold is measured from when the input FALLS BELOW the
// threshold, not from when it rose above it, and the release must run to
// EXACTLY zero. A gate that closes to 0.001 instead of 0 leaves the tail
// audible at -60 dB, which is precisely the artefact the effect exists to
// remove, and no level-based test notices.
#pragma once
#include "dcdsp_hallreverb.h"

#include <algorithm>
#include <cmath>

namespace dcdsp {

struct GatedReverbParams {
    float decayS      = 3.0f;    // 0.5 .. 10 s  (the tank's RT60 behind the gate)
    float gateMs      = 250.f;   // 20 .. 1000 ms hold after the input drops
    float thresholdDb = -30.f;   // -60 .. 0 dBFS
    float releaseMs   = 20.f;    // 5 .. 200 ms
    float predelayMs  = 0.f;     // 0 .. 100 ms
    float dampingKHz  = 8.f;     // 1 .. 16 kHz
    float mix         = 0.5f;    // 0 .. 1
};

class GatedReverbCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int /*numChannels*/)
    {
        sr_       = sampleRate > 0.0 ? sampleRate : 48000.0;
        maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 512;

        pre_.prepare(sr_, kMaxPredelayMs);
        diff_.prepare(sr_);
        tank_.prepare(sr_);
        tank_.setGeometry(kGeometryScale);
        env_.prepare(sr_);
        env_.setTimes(kEnvAttackMs, kEnvReleaseMs);

        wet_.reset(sr_, kGainRampSeconds);
        dry_.reset(sr_, kGainRampSeconds);

        applyAll();                       // everything the RATE owns, re-derived
        wet_.snapToTarget();
        dry_.snapToTarget();
        pre_.snap();
        reset();
    }

    void reset()
    {
        pre_.reset(); diff_.reset(); tank_.reset(); env_.reset();
        gate_ = 0.f;
        hold_ = 0;
    }

    void setParams(const GatedReverbParams& p)
    {
        GatedReverbParams q;
        q.decayS      = revdetail::clampNaN(p.decayS, 0.5f, 10.f);
        q.gateMs      = revdetail::clampNaN(p.gateMs, 20.f, 1000.f);
        q.thresholdDb = revdetail::clampNaN(p.thresholdDb, -60.f, 0.f);
        q.releaseMs   = revdetail::clampNaN(p.releaseMs, 5.f, 200.f);
        q.predelayMs  = revdetail::clampNaN(p.predelayMs, 0.f, kMaxPredelayMs);
        q.dampingKHz  = revdetail::clampNaN(p.dampingKHz, 1.f, 16.f);
        q.mix         = revdetail::clampNaN(p.mix, 0.f, 1.f);
        p_ = q;
        applyAll();
    }

    void process(float* const* channels, int numChannels, int numFrames)
    {
        if (numChannels <= 0 || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;          // oversized: skipped
        float* L = channels[0];
        float* R = numChannels > 1 ? channels[1] : nullptr;
        if (L == nullptr) return;

        for (int n = 0; n < numFrames; ++n)
        {
            const float inL  = L[n];
            const float inR  = R != nullptr ? R[n] : inL;
            const float mono = 0.5f * (inL + inR);

            // ---- the key: the DRY input, never the wet ----------------------
            const float e = env_.process(std::fabs(mono));
            if (e >= openLevel_) hold_ = holdSamples_;     // re-arm while it sounds
            else if (hold_ > 0)  --hold_;                  // …then count the hold out
            const bool open = (e >= openLevel_) || hold_ > 0;
            gate_ += open ? attackStep_ : -releaseStep_;
            if (gate_ > 1.f) gate_ = 1.f;
            if (gate_ < 0.f) gate_ = 0.f;                  // CLOSED means zero

            float wL = 0.f, wR = 0.f;
            tank_.process(diff_.process(pre_.process(mono)), wL, wR);
            wL *= gate_;
            wR *= gate_;

            const float w = wet_.next(), d = dry_.next();
            const float outL = wL * w + inL * d;
            const float outR = wR * w + inR * d;
            if (R != nullptr) { L[n] = outL; R[n] = outR; }
            else              L[n] = 0.5f * (outL + outR);
        }
    }

    int latencySamples() const { return 0; }

    /// Test hook (native only): the VCA's current position, 0 shut .. 1 open.
    float gateGainForTest() const { return gate_; }

private:
    void applyAll()
    {
        tank_.setDecaySeconds(p_.decayS);
        tank_.setDampingHz(p_.dampingKHz * 1000.f);
        pre_.setDelayMs(p_.predelayMs);
        openLevel_   = std::pow(10.f, p_.thresholdDb * 0.05f);
        holdSamples_ = (int) std::lround((double) p_.gateMs * 0.001 * sr_);
        releaseStep_ = 1.f / std::max(1.f, (float) ((double) p_.releaseMs * 0.001 * sr_));
        attackStep_  = 1.f / std::max(1.f, (float) (kAttackMs * 0.001 * sr_));
        wet_.setTarget(p_.mix);
        dry_.setTarget(1.f - p_.mix);
    }

    static constexpr float  kMaxPredelayMs    = 100.f;
    static constexpr float  kGeometryScale    = 1.5f;   // one dense room, no picker
    static constexpr float  kGainRampSeconds  = 0.010f;
    static constexpr float  kEnvAttackMs      = 1.0f;
    static constexpr float  kEnvReleaseMs     = 5.0f;
    static constexpr double kAttackMs         = 2.0;    // the VCA opening, not the hold

    revdetail::RampedPredelay     pre_;
    revdetail::AllpassDiffuser<4> diff_;
    revdetail::Fdn8               tank_;
    EnvelopeFollower              env_;
    SmoothedValue                 wet_, dry_;
    GatedReverbParams             p_{};
    double                        sr_          = 48000.0;
    int                           maxBlock_    = 512;
    float                         gate_        = 0.f;
    int                           hold_        = 0;
    int                           holdSamples_ = 12000;
    float                         openLevel_   = 0.0316f;
    float                         attackStep_  = 0.01f;
    float                         releaseStep_ = 0.001f;
};

} // namespace dcdsp
