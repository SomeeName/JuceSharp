// dcdsp_bassamp.h — bass amplifier head (preamp, optical compressor, four-band
// tone stack, oversampled valve stage).
//
// Models: four bass heads — a small flip-top combo, a big all-valve 300 W
// fridge, a solid-state 800 W head and a modern hi-fi/scooped voicing. A model
// carries a preamp gain curve, the four tone-stack CENTRE frequencies, a
// brightness shelf and a clip character (hardness + grid bias).
// Topology: preamp gain -> optical compressor -> [tone-stack bank] ->
// [4x oversampled: biased ADAA tanh] -> DC block -> master.
//
// Easy to get wrong: NOTHING in this path may be an unbounded high-pass. A
// bass guitar's low B is 31 Hz and its low E 41 Hz, so a single "reasonable"
// 80 Hz high-pass anywhere here removes the instrument's fundamentals. Every
// model-voicing filter is a BOUNDED shelf and the only high-pass in the core
// is the 10 Hz DC blocker. The second trap is the model switch: the tone-stack
// centres move with it, so the stack lives in TWO banks and a switch
// crossfades them over 10 ms — reconfiguring one bank in place would step
// every coefficient between two samples.
//
// 2026-10 (DotCharter #72) — the controls Rocksmith's bass heads carry and
// this head folded. Every one defaults to the state a pre-existing preset was
// saved in, and at that state its filter is SKIPPED, so a saved tone renders
// bit-identically:
//   * `mid` + `midHz` + `midQ`: a fifth, SWEEPABLE bell. Its default Q 1 is
//     Eden's own figure for its World Tour semi-parametric bands ("one octave
//     fixed bandwidth (Q=1)", Eden WT-500/600/800 operation manual); a converted
//     head may set its own.
//   * `lowMidOct` / `hiMidOct`: move the two fixed mid bells off the model's
//     centre, in octaves (a frequency knob in Hz would have needed a default
//     per MODEL — the centres differ — and would have moved every saved tone).
//   * `lowShift` / `highShift`: the Ultra Lo / Ultra Hi switches of an
//     SVT-style head, at the figures Ampeg PUBLISHES for them (Ampeg SVT-CL
//     owner's manual: Ultra Lo "+2 dB at 40 Hz and -10 dB cut at 500 Hz",
//     Ultra Hi "+9 dB at 8 kHz"). Ampeg gives no bandwidth, so each bell's Q
//     (0.7) is ours; each runs as an RBJ peaking section. No value here is
//     read out of Rocksmith's files: a converted Rocksmith head runs the game's
//     own chain only through `rsamp`, whose table the user's own install
//     supplies at run time (JuceSharp #25). clampCutoff caps a centre at a
//     quarter of the rate (DotCharter #87).
//   * the four band ranges widen from +/-12 to +/-20 dB, so a converted bass
//     tone's deep cuts and boosts fit; Eden's bands are +/-15 dB.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct BassAmpParams {
    int   model    = 0;    // 0 Flip-Top / 1 Tube 300 / 2 Solid 800 / 3 Modern
    float gain     = 0.4f; // 0..1   preamp gain
    float bass     = 0.f;  // -12..12 dB, bounded low shelf
    float lowMid   = 0.f;  // -12..12 dB, bell
    float hiMid    = 0.f;  // -12..12 dB, bell
    float treble   = 0.f;  // -12..12 dB, bounded high shelf
    float drive    = 0.2f; // 0..1   post-EQ valve stage
    float compress = 0.3f; // 0..1   optical compressor depth
    float master   = 0.6f; // 0..1   output level
    // 2026-10 (#72), see the header comment. Defaults = the pre-existing sound.
    float mid       = 0.f;    // -20..20 dB, sweepable bell (Q 1)
    float midHz     = 800.f;  // 100..5000 Hz, its centre
    float midQ      = 1.f;    // 0.3..8, its Q
    float lowMidOct = 0.f;    // -4..4 octaves off the model's low-mid centre
    float hiMidOct  = 0.f;    // -4..4 octaves off the model's high-mid centre
    int   lowShift  = 0;      // 0/1 Ultra Lo
    int   highShift = 0;      // 0/1 Ultra Hi
};

class BassAmpCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);
        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            c.os.prepare(sr_, maxBlock_, kOs);
            // 10 Hz: the ONLY high-pass in the core, and it sits a full octave
            // below the low B so the instrument keeps its fundamental.
            c.dc.prepare(sr_, 10.0);
            c.dc.reset();
            c.compEnv.prepare(sr_);
            c.compEnv.setTimes(kCompAttackMs, kCompReleaseMs);
            c.v[0].reset(); c.v[1].reset();
            c.st = 0.f;
        }

        gainBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        masterBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        driveBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        biasBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        hardBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        makeupBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        compBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        vmixBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);

        gain_.reset(sr_, kRampSeconds);
        master_.reset(sr_, kRampSeconds);
        drive_.reset(sr_, kRampSeconds);
        bias_.reset(sr_, kRampSeconds);
        hard_.reset(sr_, kRampSeconds);
        makeup_.reset(sr_, kRampSeconds);
        compK_.reset(sr_, kRampSeconds);
        vmix_.reset(sr_, kRampSeconds);

        curModel_ = -1;                 // force a full (re)configure below
        setParams(params_);
        // A freshly prepared head starts AT its settings rather than fading up
        // from whatever the smoothers held: snapToTarget, never
        // setCurrentAndTarget(current()), which pins a first prepare at zero.
        gain_.snapToTarget();
        master_.snapToTarget();
        drive_.snapToTarget();
        bias_.snapToTarget();
        hard_.snapToTarget();
        makeup_.snapToTarget();
        primeBuf_.assign(ch_.size() * static_cast<size_t>(maxBlock_), 0.f);
        primePtrs_.resize(ch_.size());
        for (size_t c = 0; c < ch_.size(); ++c)
            primePtrs_[c] = primeBuf_.data() + c * static_cast<size_t>(maxBlock_);
        compK_.snapToTarget();
        primeOnSilence();
        // ...and both tone-stack banks hold the same voicing, so the first
        // block is not a crossfade against an unconfigured bank.
        for (auto& c : ch_) configureVoice(c.v[bank_ ^ 1], kModels[curModel_], params_);
        vmix_.setCurrentAndTarget(1.f);
    }

    void reset() {
        for (auto& c : ch_) {
            c.os.reset(); c.dc.reset(); c.compEnv.reset();
            c.v[0].reset(); c.v[1].reset();
            c.st = 0.f;
        }
    primeOnSilence();
    }

    void setParams(const BassAmpParams& p) {
        params_ = p;
        const int mi = clampIndex(p.model, kNumModels);
        const Model& md = kModels[mi];

        gain_.setTarget(std::pow(10.f, clamp01(p.gain) * md.preGainDb / 20.f));
        master_.setTarget(clamp01(p.master) * 1.2f);

        const float d  = clamp01(p.drive);
        const float dg = std::pow(10.f, d * kDriveRangeDb / 20.f);
        drive_.setTarget(dg * md.stageDrive);
        // Makeup keeps the knob a DRIVE control and not a volume control.
        makeup_.setTarget(std::pow(dg * md.stageDrive, -0.62f));
        bias_.setTarget(md.bias);
        hard_.setTarget(md.hardness);
        compK_.setTarget(clamp01(p.compress) * kCompMaxK);

        if (mi != curModel_) {
            // The centres MOVE with the model, so the incoming bank is a fresh
            // filter set faded in over 10 ms while the outgoing one keeps its
            // coefficients AND its state. Reconfiguring in place would step
            // four biquads between two samples.
            bank_ ^= 1;
            for (auto& c : ch_) {
                configureVoice(c.v[bank_], md, p);
                c.v[bank_].reset();
            }
            vmix_.setCurrentAndTarget(0.f);
            vmix_.setTarget(1.f);
            curModel_ = mi;
        } else {
            for (auto& c : ch_) configureVoice(c.v[bank_], md, p);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip whole, never half-process
        const int nch    = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        // One walk of every smoother per BLOCK, shared by every channel — a
        // per-channel walk runs the ramp numChannels times too fast.
        const bool fading = vmix_.isRamping() || vmix_.current() < 1.f;
        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            gainBuf_[b]   = gain_.next();
            masterBuf_[b] = master_.next();
            driveBuf_[b]  = drive_.next();
            biasBuf_[b]   = bias_.next();
            hardBuf_[b]   = hard_.next();
            makeupBuf_[b] = makeup_.next();
            compBuf_[b]   = compK_.next();
            vmixBuf_[b]   = vmix_.next();
        }

        const int other = bank_ ^ 1;
        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                float s = detail::sanitize(x[i]) * gainBuf_[b];

                // Optical compressor: a SLOW follower (30 ms / 300 ms) and a
                // division-law gain computer, so no logarithm runs per sample.
                const float env = st.compEnv.process(std::fabs(s));
                const float over = env - kCompThreshold;
                if (over > 0.f && compBuf_[b] > 0.f)
                    s /= (1.f + compBuf_[b] * over);

                // Tone stack. Both banks run only while a model switch is
                // crossfading; at rest the idle bank costs nothing.
                const float a = st.v[bank_].process(s);
                if (fading) {
                    const float bsig = st.v[other].process(s);
                    const float m = vmixBuf_[b];
                    s = m * a + (1.f - m) * bsig;
                } else {
                    s = a;
                }
                x[i] = detail::sanitize(s);
            }

            st.os.upsample(x, frames);
            float* up = st.os.data();
            const int m = st.os.upSize(frames);
            for (int k = 0; k < m; ++k) {
                const size_t b = static_cast<size_t>(k / kOs);
                const float h = hardBuf_[b];
                const float y = adaaTanh(h * (driveBuf_[b] * up[k] + biasBuf_[b]), st.st);
                up[k] = detail::sanitize(y / h * makeupBuf_[b]);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                x[i] = detail::sanitize(st.dc.process(x[i]) * masterBuf_[b]);
            }
        }
    }

    int latencySamples() const { return 0; }

    float gainCurrentForTest()   const { return gain_.current(); }
    float masterCurrentForTest() const { return master_.current(); }

private:
    static constexpr int    kOs             = 4;
    static constexpr float  kRampSeconds    = 0.010f;
    static constexpr float  kDriveRangeDb   = 22.f;
    static constexpr float  kCompAttackMs   = 30.f;   // optical: slow in...
    static constexpr float  kCompReleaseMs  = 300.f;  // ...and slower out
    static constexpr float  kCompThreshold  = 0.05f;
    static constexpr float  kCompMaxK       = 9.f;
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

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static int clampIndex(int v, int n) { return v < 0 ? 0 : (v > n - 1 ? n - 1 : v); }
    static double clampDb(float v) {
        if (!std::isfinite(v)) return 0.0;
        return v < -20.f ? -20.0 : (v > 20.f ? 20.0 : static_cast<double>(v));
    }
    static double clampRange(float v, double lo, double hi, double fallback) {
        if (!std::isfinite(v)) return fallback;
        return v < lo ? lo : (v > hi ? hi : static_cast<double>(v));
    }

    // One model voicing. Everything here is either a SCALAR (crossfaded by its
    // own smoother) or a filter CENTRE (crossfaded by the two-bank switch).
    struct Model {
        float  preGainDb;    // preamp gain at knob 1
        double bassHz, lowMidHz, hiMidHz, trebleHz;
        double brightHz;     // bounded brightness shelf, always ABOVE 2 kHz so
        double brightDb;     // it cannot colour the instrument's fundamentals
        float  hardness;     // knee sharpness of the valve stage
        float  bias;         // grid bias -> second-harmonic asymmetry
        float  stageDrive;   // the model's own drive scale
    };
    static const int kNumModels = 4;
    static constexpr Model kModels[kNumModels] = {
        // 0 Flip-Top — small valve combo: soft, warm, early breakup.
        { 30.f,  80.0, 250.0,  800.0, 3000.0, 4000.0, -2.0, 0.9f, 0.06f, 1.2f },
        // 1 Tube 300 — the 300 W fridge: huge headroom, slow soft clip.
        { 34.f,  80.0, 250.0,  900.0, 3500.0, 5000.0,  1.0, 0.7f, 0.03f, 1.5f },
        // 2 Solid 800 — solid state: hard knee, no bias asymmetry, bright.
        { 30.f, 100.0, 300.0, 1000.0, 4000.0, 6000.0,  3.0, 2.6f, 0.00f, 2.2f },
        // 3 Modern — scooped hi-fi voicing, aggressive top.
        { 36.f,  70.0, 220.0,  900.0, 5000.0, 6000.0,  5.0, 1.6f, 0.02f, 1.8f },
    };

    // The four user bands plus the model's brightness shelf. BOTH shelves are
    // RBJ shelves (bounded by construction); the two mids are bells at Q 2.0,
    // which is what keeps a 12 dB boost at 800 Hz under 3 dB of movement at
    // 250 Hz.
    struct Voice {
        Biquad bright, bassSh, lowMidBq, hiMidBq, trebleSh;
        Biquad midBq, ultraLoShelf, ultraLoBell, ultraHiBell;
        // Which of the 2026-10 filters are in the path. Off = SKIPPED, which is
        // what keeps a preset that never set them bit-identical.
        bool midOn = false, ultraLoOn = false, ultraHiOn = false;
        void reset() {
            bright.reset(); bassSh.reset(); lowMidBq.reset();
            hiMidBq.reset(); trebleSh.reset();
            midBq.reset(); ultraLoShelf.reset(); ultraLoBell.reset(); ultraHiBell.reset();
        }
        float process(float x) {
            x = bright.process(x);
            x = bassSh.process(x);
            x = lowMidBq.process(x);
            if (midOn) x = midBq.process(x);
            x = hiMidBq.process(x);
            x = trebleSh.process(x);
            if (ultraLoOn) x = ultraLoBell.process(ultraLoShelf.process(x));
            if (ultraHiOn) x = ultraHiBell.process(x);
            return x;
        }
    };

    void configureVoice(Voice& v, const Model& md, const BassAmpParams& p) const {
        v.bright.setHighShelf(sr_, md.brightHz, 0.707, md.brightDb);
        v.bassSh.setLowShelf(sr_, md.bassHz, kShelfQ, clampDb(p.bass));
        const double loOct = clampRange(p.lowMidOct, -kMaxOct, kMaxOct, 0.0);
        const double hiOct = clampRange(p.hiMidOct, -kMaxOct, kMaxOct, 0.0);
        v.lowMidBq.setPeaking(sr_, md.lowMidHz * std::pow(2.0, loOct), kBellQ, clampDb(p.lowMid));
        v.hiMidBq.setPeaking(sr_, md.hiMidHz * std::pow(2.0, hiOct), kBellQ, clampDb(p.hiMid));
        v.trebleSh.setHighShelf(sr_, md.trebleHz, kShelfQ, clampDb(p.treble));

        const double midDb = clampDb(p.mid);
        const bool wasMid = v.midOn;
        v.midOn = midDb != 0.0;
        if (v.midOn) {
            v.midBq.setPeaking(sr_, clampRange(p.midHz, kMidHzMin, kMidHzMax, 800.0),
                               clampRange(p.midQ, kMidQMin, kMidQMax, 1.0), midDb);
            if (!wasMid) v.midBq.reset();
        }
        const bool wasLo = v.ultraLoOn, wasHi = v.ultraHiOn;
        v.ultraLoOn = p.lowShift != 0;
        v.ultraHiOn = p.highShift != 0;
        if (v.ultraLoOn) {
            v.ultraLoShelf.setPeaking(sr_, kUltraLoLowHz, kUltraLoLowQ, kUltraLoLowDb);
            v.ultraLoBell.setPeaking(sr_, kUltraLoMidHz, kUltraLoMidQ, kUltraLoMidDb);
            if (!wasLo) { v.ultraLoShelf.reset(); v.ultraLoBell.reset(); }
        }
        if (v.ultraHiOn) {
            v.ultraHiBell.setPeaking(sr_, kUltraHiHz, kUltraHiQ, kUltraHiDb);
            if (!wasHi) v.ultraHiBell.reset();
        }
    }

    static constexpr double kShelfQ = 0.8;
    static constexpr double kBellQ  = 2.0;
    // 2026-10 (#72): the sweepable bell and the shift switches (header comment).
    static constexpr double kMidQMin        = 0.3;
    static constexpr double kMidQMax        = 8.0;
    static constexpr double kMidHzMin       = 100.0;
    static constexpr double kMidHzMax       = 5000.0;
    static constexpr double kMaxOct         = 4.0;
    // Ampeg's published Ultra Lo / Ultra Hi figures (SVT-CL owner's manual);
    // the Q of each bell is ours (Ampeg publishes none).
    static constexpr double kUltraLoLowHz   = 40.0;    // +2 dB at 40 Hz
    static constexpr double kUltraLoLowQ    = 0.7;
    static constexpr double kUltraLoLowDb   = 2.0;
    static constexpr double kUltraLoMidHz   = 500.0;   // -10 dB at 500 Hz
    static constexpr double kUltraLoMidQ    = 0.7;
    static constexpr double kUltraLoMidDb   = -10.0;
    static constexpr double kUltraHiHz      = 8000.0;  // +9 dB at 8 kHz
    static constexpr double kUltraHiQ       = 0.7;
    static constexpr double kUltraHiDb      = 9.0;

    struct Channel {
        detail::OversamplerNx os;
        Voice            v[2];
        EnvelopeFollower compEnv;
        DcBlocker        dc;
        float            st = 0.f;   // ADAA memory
    };

    double sr_       = 48000.0;
    int    maxBlock_ = 512;
    int    bank_     = 0;
    int    curModel_ = -1;
    BassAmpParams params_{};
    SmoothedValue gain_, master_, drive_, bias_, hard_, makeup_, compK_, vmix_;
    std::vector<float> gainBuf_, masterBuf_, driveBuf_, biasBuf_, hardBuf_,
                       makeupBuf_, compBuf_, vmixBuf_;
    /// Scratch for primeOnSilence(); sized in prepare(), never on the callback.
    std::vector<float>  primeBuf_;
    std::vector<float*> primePtrs_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
