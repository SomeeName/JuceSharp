// dcdsp_poweramp.h — valve POWER stage with supply sag, presence, resonance
// and speaker damping. The half of an amplifier a drive pedal does not give
// you: put this after any preamp or dirt box and it behaves like a real head's
// output section.
//
// Models: five output valves (EL84 / 6V6 / EL34 / 6L6 / KT88) differing in
// headroom, knee softness and bias asymmetry. Sag is a SLOW follower on the
// stage's own drive level that droops the supply rail — the clip ceiling
// shrinks over ~200 ms under a sustained chord and recovers over ~200 ms.
// Topology: input trim -> sag detector -> [4x oversampled: droop-scaled ADAA
// tanh] -> DC block -> presence / resonance / damping blends -> level.
//
// Easy to get wrong: sag must scale the CEILING, not the input — y = droop *
// tanh(x / droop) leaves a quiet passage untouched (it never reaches the
// ceiling) and squashes a loud one, which is what the effect actually is.
// Scaling the input instead turns the knob into a slow volume control that
// ducks clean playing too. The second trap: the amp core in dcdsp_amp.h has a
// sag model of its own with different time constants; this is a deliberate
// re-implementation, not a shared one, because that core's numbers belong to
// its own voicing table. The third: `sag` itself is a per-sample multiplier on
// the clip CEILING, so it ramps like every other one here. It did not until
// 2026-09-12, and a knob drag with the follower charged stepped the ceiling —
// the same splice dcdsp_rat.h refuses for its `clip` footswitch.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

/// The rig's power-amp RESONANCE shelf: a bounded low shelf standing in for
/// the low-frequency NFB reduction a "resonance"/"depth" control makes. One
/// definition, used here and by the `amp` core's `resonance` param
/// (dcdsp_amp.h) so the two controls cannot drift apart.
struct PowerAmpResonance {
    static constexpr double kHz    = 110.0;
    static constexpr double kQ     = 0.9;
    static constexpr double kMaxDb = 8.0;
};

struct PowerAmpParams {
    int   tubes      = 2;    // 0 EL84 / 1 6V6 / 2 EL34 / 3 6L6 / 4 KT88
    float drive      = 0.3f; // 0..1
    float sag        = 0.3f; // 0..1 supply-sag depth
    float presence   = 0.5f; // 0..1 negative-feedback high shelf
    float resonance  = 0.5f; // 0..1 output-transformer / speaker low shelf
    float damping    = 0.5f; // 0..1 speaker impedance peak
    float level      = 0.6f; // 0..1
};

class PowerAmpCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            c.dc.prepare(sr_, 12.0);
            c.dc.reset();
            // Rate-owned: the follower's coefficients are derived from sr.
            c.sagEnv.prepare(sr_);
            c.sagEnv.setTimes(kSagAttackMs, kSagReleaseMs);
            c.reset();
        }

        driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        levelBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        softBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        biasBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        headBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        makeupBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        presBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        resoBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        dampBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        sagBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        sagKBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);

        drive_.reset(sr_, kRampSeconds);
        level_.reset(sr_, kRampSeconds);
        soft_.reset(sr_, kRampSeconds);
        bias_.reset(sr_, kRampSeconds);
        head_.reset(sr_, kRampSeconds);
        makeup_.reset(sr_, kRampSeconds);
        pres_.reset(sr_, kRampSeconds);
        reso_.reset(sr_, kRampSeconds);
        damp_.reset(sr_, kRampSeconds);
        sagK_.reset(sr_, kRampSeconds);

        setParams(params_);
        drive_.snapToTarget();
        level_.snapToTarget();
        soft_.snapToTarget();
        bias_.snapToTarget();
        head_.snapToTarget();
        makeup_.snapToTarget();
        pres_.snapToTarget();
        reso_.snapToTarget();
        damp_.snapToTarget();
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);
        sagK_.snapToTarget();
        primeOnSilence();
    }

    void reset() {
        for (auto& c : ch_) { c.os.reset(); c.dc.reset(); c.sagEnv.reset(); c.reset(); }
    primeOnSilence();
    }

    void setParams(const PowerAmpParams& p) {
        params_ = p;
        const Tube& t = kTubes[clampIndex(p.tubes, kNumTubes)];

        const float d  = clamp01(p.drive);
        const float dg = std::pow(10.f, d * kDriveRangeDb / 20.f);
        drive_.setTarget(dg);
        head_.setTarget(t.headroom);
        soft_.setTarget(t.softness);
        bias_.setTarget(t.bias);
        // Keep the drive knob a drive knob: compensate most of its gain back.
        makeup_.setTarget(std::pow(dg, -0.7f) * t.makeup);
        level_.setTarget(clamp01(p.level) * 1.2f);
        pres_.setTarget(clamp01(p.presence));
        reso_.setTarget(clamp01(p.resonance));
        damp_.setTarget(clamp01(p.damping));
        sagK_.setTarget(clamp01(p.sag) * kSagMaxK);

        for (auto& c : ch_) {
            // All three colour branches are FIXED shapes whose depth is the
            // blend weight, so every one of them ramps and every one is
            // BOUNDED — a power amp's low end is a transformer resonance, not
            // a high-pass, and a presence control is a shelf, not a tilt into
            // infinity.
            c.presSh.setHighShelf(sr_, kPresenceHz, 0.707, kPresenceMaxDb);
            c.resoSh.setLowShelf(sr_, PowerAmpResonance::kHz, PowerAmpResonance::kQ, PowerAmpResonance::kMaxDb);
            c.dampLo.setPeaking(sr_, kDampLowHz, kDampLowQ, kDampLowMaxDb);
            c.dampHi.setPeaking(sr_, kDampHighHz, 1.0, kDampHighMaxDb);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip whole, never half-process
        const int nch    = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            driveBuf_[b]  = drive_.next();
            levelBuf_[b]  = level_.next();
            softBuf_[b]   = soft_.next();
            biasBuf_[b]   = bias_.next();
            headBuf_[b]   = head_.next();
            makeupBuf_[b] = makeup_.next();
            presBuf_[b]   = pres_.next();
            resoBuf_[b]   = reso_.next();
            dampBuf_[b]   = damp_.next();
            sagKBuf_[b]   = sagK_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                const float s = detail::sanitize(x[i]) * driveBuf_[b];
                // The rail droops with how hard the stage is being driven.
                const float env = st.sagEnv.process(std::min(std::fabs(s), 4.f));
                const float k = sagKBuf_[b];
                sagBuf_[b] = (k > 0.f) ? (1.f / (1.f + k * env)) : 1.f;
                x[i] = s;
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                // Ceiling = headroom * droop. Dividing in and multiplying out
                // is what makes sag act on loud playing only.
                const float ceil = headBuf_[b] * sagBuf_[b];
                const float inv  = 1.f / ceil;
                const float h    = softBuf_[b];
                const float y = adaaTanh(h * (up[k] * inv + biasBuf_[b]), st.st);
                up[k] = detail::sanitize(y * (ceil / h) * makeupBuf_[b]);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = st.dc.process(x[i]);
                const float pw = presBuf_[b];
                s = s + pw * (st.presSh.process(s) - s);
                const float rw = resoBuf_[b];
                s = s + rw * (st.resoSh.process(s) - s);
                const float dw = dampBuf_[b];
                const float dsig = st.dampHi.process(st.dampLo.process(s));
                s = s + dw * (dsig - s);
                x[i] = detail::sanitize(s * levelBuf_[b]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float levelCurrentForTest() const { return level_.current(); }

private:
    static constexpr int    kOs              = 4;
    static constexpr float  kRampSeconds     = 0.010f;
    static constexpr float  kDriveRangeDb    = 20.f;
    static constexpr float  kSagAttackMs     = 200.f;  // the rail droops...
    static constexpr float  kSagReleaseMs    = 200.f;  // ...and recovers
    static constexpr float  kSagMaxK         = 3.5f;
    /// Long enough that the primed state IS the steady state, to well below
    /// any audible floor.
    static constexpr double kPrimeSeconds = 0.25;

    /// Leave every stage in the state SILENCE really produces.
    ///
    /// The valve stage carries a grid-BIAS offset, so with nothing playing it
    /// emits constant DC into the blocker behind it. A `DcBlocker` reset to
    /// zero state answers a DC step with a FULL-AMPLITUDE decaying pulse -
    /// y[0] = x[0] - so every prepare(), every device change and every
    /// un-bypass thumped out of digital silence. Measured before this:
    /// -50.0 dBFS RMS (poweramp) and -49.0 (bassamp) over the first 500 ms,
    /// and the same shape cost the drive drawer five cores.
    ///
    /// Running the module's OWN process() over a quarter second of silence is
    /// what primes it, rather than a hand-derived DC constant: the value the
    /// blocker must start on is the output of an oversampled ADAA cascade, and
    /// a constant copied out of that arithmetic would be a second source of
    /// truth the next voicing change silently falsifies. Message thread only
    /// (prepare / reset); it allocates nothing, the scratch is sized in
    /// prepare().
    void primeOnSilence() {
        if (ch_.empty() || maxBlock_ <= 0 || primePtrs_.size() != ch_.size()) return;
        const int nch   = static_cast<int>(ch_.size());
        const int total = static_cast<int>(sr_ * kPrimeSeconds);
        for (int done = 0; done < total; done += maxBlock_) {
            const int n = (maxBlock_ < total - done) ? maxBlock_ : (total - done);
            std::fill(primeBuf_.begin(), primeBuf_.end(), 0.f);
            process(primePtrs_.data(), nch, n);
        }
    }
    static constexpr double kPresenceHz      = 3500.0;
    static constexpr double kPresenceMaxDb   = 9.0;
    static constexpr double kDampLowHz       = 95.0;
    static constexpr double kDampLowQ        = 2.2;
    static constexpr double kDampLowMaxDb    = 5.0;
    static constexpr double kDampHighHz      = 2500.0;
    static constexpr double kDampHighMaxDb   = 2.5;

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static int clampIndex(int v, int n) { return v < 0 ? 0 : (v > n - 1 ? n - 1 : v); }

    // Output-valve voicings. Everything here is a SCALAR, so a tube change is
    // four smoothers walking to new targets over 10 ms — no filter switches.
    struct Tube {
        float headroom;  // clip ceiling before sag
        float softness;  // knee sharpness (higher = harder)
        float bias;      // grid bias -> second-harmonic asymmetry
        float makeup;    // level trim so the five voicings are comparable
    };
    static const int kNumTubes = 5;
    static constexpr Tube kTubes[kNumTubes] = {
        { 0.45f, 1.6f, 0.09f, 1.10f },  // EL84 — small, early, chimey breakup
        { 0.55f, 1.3f, 0.07f, 1.05f },  // 6V6  — soft, warm
        { 0.75f, 2.0f, 0.05f, 0.95f },  // EL34 — the classic British crunch
        { 0.90f, 1.7f, 0.03f, 0.92f },  // 6L6  — American, more headroom
        { 1.15f, 2.4f, 0.02f, 0.88f },  // KT88 — huge, tight, late and hard
    };

    struct Channel {
        detail::OversamplerNx os;
        Biquad           presSh, resoSh, dampLo, dampHi;
        DcBlocker        dc;
        EnvelopeFollower sagEnv;
        float            st = 0.f;   // ADAA memory
        void reset() {
            presSh.reset(); resoSh.reset(); dampLo.reset(); dampHi.reset();
            st = 0.f;
        }
    };

    double sr_       = 48000.0;
    int    maxBlock_ = 512;
    PowerAmpParams params_{};
    SmoothedValue drive_, level_, soft_, bias_, head_, makeup_, pres_, reso_,
                  damp_, sagK_;
    std::vector<float> driveBuf_, levelBuf_, softBuf_, biasBuf_, headBuf_,
                       makeupBuf_, presBuf_, resoBuf_, dampBuf_, sagBuf_, sagKBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
