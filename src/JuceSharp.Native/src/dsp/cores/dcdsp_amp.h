// dcdsp_amp.h — guitar amplifier model (preamp + tone stack + power amp).
//
// 0.16 W6 HI-GAIN OVERHAUL. The whole signal path from the first grid to the
// output transformer now runs INSIDE one 4x oversampled region:
//
//   input band-limit (base rate)
//   |-- 4x upsample ------------------------------------------------------|
//   |  N gain stages, each: drive -> level-dependent bias shift -> ADAA    |
//   |                       tanh -> coupling HPF -> MILLER LPF             |
//   |  cathode follower (asymmetric, compressive) [hi-gain/crunch only]    |
//   |  tone stack (bass/mid/treble) + presence  <- shapes what clips next  |
//   |  post-tone-stack recovery stage [5150-family only]                   |
//   |  supply sag                                                          |
//   |  power amp with negative feedback (1-sample-lagged, LPF'd loop)      |
//   |  output transformer: LF limit -> soft saturation -> HF bandwidth     |
//   |-- 4x downsample ----------------------------------------------------|
//   DC block -> master (base rate)
//
// WHY (each item fixes a measured defect in the pre-0.16-W6 model):
//  * The old model oversampled ONLY the preamp waveshapers, at 2x, and ran the
//    tone stack AND the power-amp tanh at base rate. Measured on models
//    4/14/15/16 at gain 0.85: worst alias product -43..-46 dBc, total alias
//    power -32..-34 dBc, and a *power chord* (whose every legitimate product
//    lands on the 41.2 Hz grid) came out of the cab with -28.6 dBc of off-grid
//    energy. That inharmonic in-band hash — worst products at 3.7-9.5 kHz, i.e.
//    BELOW the cab's low-pass, so the cab cannot remove them — is the "fizzy,
//    buzzy, not-like-a-real-amp" sound. 4x + ADAA over the WHOLE cascade, with
//    the elliptic OversamplerNx, is the fix.
//  * There was NO inter-stage low-pass at all: the coupling network was HPF
//    only, so every stage's harmonics hit the next stage's tanh at full level
//    and got multiplied again. Real preamps have a plate-load bypass /
//    "sizzle" cap and the grid-Miller capacitance, putting a one-pole after
//    EVERY stage. `millerHz` restores it: it starves the later stages of the
//    ultrasonic content that was feeding the folding. NOTE the corners are
//    9-18 kHz, NOT the 5-8 kHz a first cut of this used — at 5-8 kHz the
//    cascade also ate the 2-6 kHz PRESENCE harmonics the tone actually lives
//    on (Recto Dual lost 4.8 dB of 2-6 kHz energy). Oversampling, not the
//    Miller low-pass, is what kills the aliasing; the Miller corner only has to
//    stop the ultrasonics.
//  * THE COUPLING NETWORK IS A BOUNDED SHELF, NOT A STACK OF HIGH-PASSES
//    (0.35 low-end fix). `cplHz[]` used to configure a full first-order HPF per
//    stage, and 2-4 of them ran in series, so the response kept falling at
//    6-24 dB/oct below the corners — i.e. across the entire register the
//    instrument's FUNDAMENTALS live in. Measured on the real core (sine-probe
//    describing function, dB re the 200 Hz-4 kHz log-mean, at the low E, E2 =
//    82 Hz), small-signal: Plexi 50 -11.3 dB, of which 9.7 dB was cpl[0]+cpl[1]
//    alone; Five-Oh-One -30.1 dB, of which ~29 dB was its four coupling HPFs.
//    At playing level (-18 dBFS) the clipping compresses that back to -7.1 and
//    -15.0 dB — still 10-20 dB under a real rig, and it is why every preset read
//    as thin. The circuit those corners stand in for is a CATHODE-BYPASS
//    network, which is a shelf of BOUNDED depth; the actual coupling caps sit at
//    7-20 Hz and are out of band. So each stage now runs
//    `OnePole::setHighpassShelf`: unchanged above the corner (the corner is
//    still what makes a Djent tighter than a Recto), flattening out below it.
//    `cplCutDb` is the TOTAL cut the whole network may take, split evenly over
//    the stages present — 3 dB loose/bass/DI, 4 dB American clean, 6 dB classic
//    crunch, 9 dB tight hi-gain. Do NOT go back to an unbounded HPF here, and do
//    not add a second one elsewhere in the cascade: the failure mode is that
//    each one looks individually reasonable and they SUM.
//  * `biasDepth` moves each stage's operating point with the grid current: the
//    excess above the grid clamp is rectified and RC-averaged, dragging the
//    grid toward cutoff, so the clipping asymmetry (and its duty-cycle shift =
//    blocking) GROWS with how hard you play. That is what makes chugs breathe
//    instead of sounding like a static waveshaper.
//  * `cfDrive` adds the cathode follower that sits before the tone stack in
//    every Marshall/Recto/5150-family preamp: near-unity, huge headroom, then
//    an abrupt asymmetric squash. `postDrive` adds the 5150/Peavey post-tone-
//    stack recovery stage, so the tone knobs change the DISTORTION, not just
//    the EQ.
//  * `nfb` closes a real negative-feedback loop around the power amp
//    (1-sample lag + a loop LPF at `nfbHz`, kept above the audio band so the
//    loop contributes DYNAMICS, not EQ — presence stays modelled as the NFB
//    high-shelf so voicings 0-6 keep their frequency response). `otDrive` /
//    `otLpfHz` / `otHpfHz` add output-transformer soft saturation and its
//    bandwidth limit.
//
// Voicing (0..23) selects a circuits-inspired coefficient table (0.16 gear
// roster — same topology for every voicing, so CPU stays flat). Rows 0-6 keep
// their original drive/tone/sag numbers, and their NEW fields are set
// conservatively (gentle Miller corners, little bias travel, no cathode
// follower on the cleans) so they stay themselves. Soundalike display names
// live in the ABI v24 label table (DspModuleFactory.cpp):
//   0 Blackface Twin 85   1 Plexi 50           2 Five-Oh-One Lead
//   3 AC-Thirty           4 Recto Dual         5 Jazz Solid 120
//   6 Brown Variac        7 Tweed Champ 5W     8 Tweed Twin 40
//   9 Blackface Deluxe 22 10 Plexi 100         11 JMP-Eight-Hundred
//   12 Two-Oh-Three       13 AC-Fifteen        14 Recto Triple
//   15 Modern Djent 8-Str 16 PowerBall Lead    17 Overdrive Special
//   18 Class-A Boutique 30 19 Bass Three-Sixty 20 Bass Flip-Top
//   21 SVT Bass 300W      22 Acoustic Pre      23 Studio Direct
// Wattage siblings (Champ 5W vs Twin 40, Plexi 50 vs 100, AC-Fifteen vs
// AC-Thirty, Recto Dual vs Triple, Bass Flip-Top vs SVT 300W) differ measurably
// in headroom, breakup point and supply sag — not just level.
//
// Sag model: an envelope follower (12 ms attack / 220 ms release) on the
// pre-power-amp signal drives a gain droop g = 1/(1 + k*env) — attack
// transients compress then "bloom" back as the supply recovers. k comes from
// the voicing table; 0 disables it (jazz is sag-free).
//
// Filters that run inside the oversampled region are OnePole (exp coefficient,
// well-conditioned at any fc/sr ratio) or BiquadD (double coefficients AND
// double state). The float `Biquad` is NOT safe at 4x — see dcdsp_primitives.h.
// Pure dcdsp core, RT-safe: all allocation in prepare(), NaN-guarded output.
#pragma once

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif
#include "dcdsp_detail_os.h"
#include "dcdsp_tonestack.h"
#include "dcdsp_poweramp.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct AmpParams {
    float gain     = 0.5f; // 0..1 preamp drive
    float bass     = 0.5f; // 0..1 tone stack low   (0.5 = flat)
    float mid      = 0.5f; // 0..1 tone stack mid    (0.5 = flat)
    float treble   = 0.5f; // 0..1 tone stack high   (0.5 = flat)
    float presence = 0.3f; // 0..1 NFB high shelf (boost-only)
    float master   = 0.6f; // 0..1 power-amp drive + output
    int   voicing  = 1;    // 0..26 gear-roster model (see kVoicings + the ABI
                           // v24 label table)
    // 2026-10 (DotCharter #70). Both default to the state every pre-existing
    // preset was saved in, and at that state the core skips their code, so a
    // saved tone renders bit-identically.
    int   bright    = 0;    // 0/1 bright cap across the gain (volume) pot
    float resonance = 0.f;  // 0..1 power-amp low-end ("resonance"/"depth")
};

class AmpCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = sampleRate;
        maxBlock_ = std::max(1, maxBlockSize);
        int n = std::max(1, numChannels);
        ch_.resize(n);
        const double os = sr_ * kOs;
        for (auto& c : ch_) {
            c.os.prepare(sampleRate, maxBlock_, kOs);
            c.sagEnv.prepare(os);
            c.sagEnv.setTimes(12.0f, 220.0f);
            // Grid-leak RC: one-pole charge/discharge of the rectified grid
            // excess (see kGridLeakMs).
            gridCoef_ = (float)(1.0 - std::exp(-1.0 / (kGridLeakMs * 0.001 * os)));
            c.preDc.prepare(os, 12.0);
            c.outDc.prepare(sr_, 15.0);
            c.reset();
        }
        masterBuf_.assign(maxBlock_, 0.f);
        master_.reset(sampleRate, 0.01f);
        // `gain` spans up to +58 dB (~790x) on voicing 15 and lands as ONE
        // constant per block into the FIRST tanh; `master` drives the power amp
        // as a second stepped non-linearity while its own output level was the
        // only ramped one in this core. Both ramp now, at the base rate,
        // pre-filled into a block buffer exactly like masterBuf_ — the loop that
        // consumes them is CHANNEL-outer and oversampled, so a per-sample
        // next() inside it would walk one smoother nch*kOs times per block.
        preStage_.reset(sampleRate, 0.01f);
        power_.reset(sampleRate, 0.01f);
        preStageBuf_.assign(maxBlock_, 0.f);
        powerBuf_.assign(maxBlock_, 0.f);
        setParams(params_);
        // A freshly prepared amp starts AT its master level. reset() preserves
        // mCurrent, which is 0 on a first prepare, so without this the first
        // 10 ms after a prepare/device change fades up from silence.
        master_.snapToTarget();
        preStage_.snapToTarget();   // a freshly prepared amp starts AT its drive
        power_.snapToTarget();
    }

    void reset() { for (auto& c : ch_) c.reset(); }

    // Test-only: the smoother's CURRENT value, so a gate case can assert a
    // freshly prepared core starts AT its configured value rather than
    // ramping up to it from 0 (SmoothedValue::reset() preserves mCurrent).
    float masterCurrentForTest() const { return master_.current(); }

    // Test/tooling-only: the LF-relevant slice of the private `kVoicings` table
    // (W4-D63). tools/tone_response.cpp used to carry a HAND-TRANSCRIBED copy of
    // these numbers so its `filters` mode could attribute low-end loss to a
    // named filter — a second source of truth for 24 rows, whose own comment
    // admitted "a mismatch means this table drifted from the header". It reads
    // the real table through here now.
    //
    // A narrow PROJECTION rather than exposing `Voicing` itself: that struct is
    // private and has 30 fields, and publishing all of them to give a tool ten
    // would turn an internal layout into API. Names are deliberately NOT here —
    // they live in the `//` comments on each row and are the tool's business.
    struct VoicingLfInfo {
        int   stages;
        float cplHz[3];
        float postDrive;
        float bassFc, bassOfsDb;
        float midFc, midQ, midOfsDb;
        float otHpfHz;
        float cplCutDb;
    };

    static constexpr int numVoicingsForTest() noexcept { return kNumVoicings; }

    /// Clamped, never out of range — a tool walking [0, numVoicingsForTest())
    /// cannot fall off the end, and one that miscounts gets row 0 rather than
    /// whatever follows the array.
    static constexpr VoicingLfInfo voicingLfForTest(int index) noexcept
    {
        const int i = index < 0 ? 0 : (index > kNumVoicings - 1 ? kNumVoicings - 1 : index);
        const Voicing& v = kVoicings[i];
        return VoicingLfInfo{ v.stages,
                              { v.cplHz[0], v.cplHz[1], v.cplHz[2] },
                              v.postDrive,
                              v.bassFc, v.bassOfsDb,
                              v.midFc, v.midQ, v.midOfsDb,
                              v.otHpfHz,
                              v.cplCutDb };
    }

    void setParams(const AmpParams& p) {
        params_ = p;
        const int voicing = p.voicing < 0 ? 0 : (p.voicing > kNumVoicings - 1
                                                     ? kNumVoicings - 1 : p.voicing);
        const Voicing& v = kVoicings[voicing];
        const float gain = clamp01(p.gain);

        // Voicing -> stage count + drive range (split evenly across stages).
        numStages_ = v.stages;
        const float driveDb  = v.driveBaseDb + gain * v.driveRangeDb;
        const float preGain  = std::pow(10.f, driveDb / 20.f);
        const float perStage = std::pow(preGain, 1.f / (float)numStages_);
        for (int s = 0; s < kMaxStages; ++s) stageGain_[s] = perStage;
        preStage_.setTarget(perStage);
        bias_      = v.bias;
        biasDepth_ = v.biasDepth;
        cfDrive_   = v.cfDrive;
        postDrive_ = v.postDrive;
        nfb_       = v.nfb;
        otDrive_   = v.otDrive;

        // Tone stack. It now sits INSIDE the oversampled cascade, after the
        // cathode follower and before the recovery stage / power amp — so the
        // knobs shape what the next non-linearity clips, which is why a real
        // amp's bass knob changes the *character* of the distortion and not
        // just the low end. 0.5 = flat *relative to the voicing's fixed tilt*.
        const float bassDb   = (clamp01(p.bass)   - 0.5f) * 2.f * 12.f + v.bassOfsDb;
        const float midDb    = (clamp01(p.mid)    - 0.5f) * 2.f * 10.f + v.midOfsDb;
        const float trebleDb = (clamp01(p.treble) - 0.5f) * 2.f * 12.f + v.trebOfsDb;
        // Presence: boost-only shelf; the base term models reduced NFB
        // (a Vox-class amp sits "open" even with the knob at zero).
        const float presDb = v.presBaseDb
                             + clamp01(p.presence) * (v.presMaxDb - v.presBaseDb);
        // Power-amp drive scales with master; master also sets output level.
        powerDrive_ = v.powerBase + v.powerRange * clamp01(p.master);
        power_.setTarget(powerDrive_);
        sagK_ = v.sag * 3.0f;
        // outTrim keeps each voicing's OUTPUT LEVEL where it was before the W6
        // overhaul (the added cathode follower / OT saturation / Miller
        // low-passes cost 0.2-2.7 dB depending on the model). It is a fixed
        // per-voicing number, not a new parameter: `master` still means what it
        // meant, so saved presets — and the managed loudness estimator, which
        // models master/level/outputTrimDb statically — keep their assumptions.
        master_.setTarget(clamp01(p.master) * 1.2f * v.outTrim);

        const double os = sr_ * kOs;

        // CIRCUIT TONE STACK (rows 24-26): the passive TMB network from its
        // component values (dcdsp_tonestack.h), in place of the shelf trio. The
        // knobs are the pots' ROTATIONS, so 0.5 is noon on the real panel —
        // not "flat", which a passive stack has no setting for.
        stackOn_ = v.toneStack > 0 && v.toneStack <= kNumTmbStacks;
        double stackB[4] = { 1.0, 0.0, 0.0, 0.0 }, stackA[4] = { 1.0, 0.0, 0.0, 0.0 };
        if (stackOn_) {
            const TmbComponents& k = kTmbStacks[v.toneStack - 1];
            const TmbAnalog h = tmbAnalog(k, clamp01(p.treble), clamp01(p.mid), clamp01(p.bass));
            stackOn_ = bilinear3(h, os, stackB, stackA);
            const double g = normalisationGain(k);
            for (double& b : stackB) b *= g;
        }

        // BRIGHT: a cap across the upper section of the gain (volume) pot —
        // H(s) = (1 + s C Rup) / (1 + s C (Rup || Rlo)) relative to the pot's
        // own division, so the boost is 1/fraction at the top and vanishes at
        // full gain, exactly as on the amp (the cap is shorted out at 10). The
        // values are Fender's AB763 bright circuit: 120 pF across a 1 MOhm
        // audio-taper volume pot (AB763 schematic). No other bright circuit in
        // this roster is documented, so every voicing uses that one.
        brightOn_ = false;
        if (p.bright != 0) {
            const double frac = potFraction(clamp01(p.gain), PotTaper::Audio);
            const double rLo  = std::max(frac * kBrightPotOhm, 1.0);   // never a short
            const double rUp  = (1.0 - frac) * kBrightPotOhm;
            if (rUp > 1.0) {
                const double tz = kBrightCapF * rUp;
                const double tp = kBrightCapF * (rUp * rLo / (rUp + rLo));
                const double c  = 2.0 * os;                              // bilinear
                const double a0 = 1.0 + c * tp;
                brightB0_ = (1.0 + c * tz) / a0;
                brightB1_ = (1.0 - c * tz) / a0;
                brightA1_ = (1.0 - c * tp) / a0;
                brightOn_ = std::isfinite(brightB0_) && std::isfinite(brightB1_)
                            && std::isfinite(brightA1_);
            }
        }
        // The pot sits between the first and second triode (V1 -> volume -> V2
        // on the AB763 and the Marshalls); a one-stage voicing has no second
        // grid, so it goes in front of the only one.
        brightStage_ = numStages_ > 1 ? 1 : 0;

        // RESONANCE: the rig's own power-amp resonance shelf (dcdsp_poweramp.h,
        // PowerAmpResonance), in front of the power stage. 0 = the stage as it
        // always was (the shelf is skipped).
        const float reso = clamp01(p.resonance);
        resoOn_ = reso > 0.f;

        // Coupling network: a BOUNDED shelf, not a stack of high-passes.
        // `cplCutDb` is the TOTAL bass the whole network may remove, split
        // evenly over the coupling stages actually present (see kVoicings).
        const int nCpl = numStages_ + (v.postDrive > 0.f ? 1 : 0);
        const float cplFloor = std::pow(10.f, -(v.cplCutDb / (float) nCpl) / 20.f);
        for (auto& c : ch_) {
            c.inHpf.setHighpass(sr_, 40.0, 0.707);
            c.inLpf.setLowpass(sr_, v.inLpfHz, 0.707);
            // Inter-stage network, at the OS rate. OnePole (exp coefficient)
            // stays well-conditioned at these fc/sr ratios; a float biquad here
            // would lose precision (poles at z~=1).
            for (int s = 0; s < kMaxStages; ++s) {
                c.cpl[s].setHighpassShelf(os, v.cplHz[s], cplFloor); // coupling shelf
                c.miller[s].setLowpass(os, v.millerHz[s]);  // plate/Miller LPF
            }
            c.cfLpf.setLowpass(os, v.cfLpfHz);
            c.postCpl.setHighpassShelf(os, v.cplHz[kMaxStages - 1], cplFloor);
            c.postMiller.setLowpass(os, v.millerHz[kMaxStages - 1]);
            // Tone stack + presence: BiquadD, safe at the oversampled rate.
            c.bass.setLowShelf   (os, v.bassFc, 0.707, bassDb);
            c.mid .setPeaking    (os, v.midFc,  v.midQ, midDb);
            c.treble.setHighShelf(os, v.trebFc, 0.707, trebleDb);
            c.presence.setHighShelf(os, v.presFc, 0.707, presDb);
            if (stackOn_) c.stack.setCoeffs(stackB, stackA);
            if (resoOn_)
                c.reso.setLowShelf(os, PowerAmpResonance::kHz, PowerAmpResonance::kQ,
                                   (double) reso * PowerAmpResonance::kMaxDb);
            // Power-amp NFB loop lag. Kept ABOVE the guitar band on purpose:
            // the loop is here for dynamics (it linearises small signals and
            // firms up the onset of clipping), not for EQ — presence still owns
            // the frequency response so voicings 0-6 keep theirs.
            c.nfbLpf.setLowpass(os, v.nfbHz);
            c.otHpf.setHighpass(os, v.otHpfHz);   // transformer LF limit
            c.otLpf.setLowpass (os, v.otLpfHz);   // transformer HF bandwidth
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        // SKIP an oversized block rather than processing part of it. Clamping to
        // maxBlock_ left the tail UNTOUCHED, i.e. full-level clean DI spliced
        // onto the middle of a high-gain patch — the one outcome worse than
        // either growing the buffer (which the callback may not do) or dropping
        // the block.
        if (numFrames > maxBlock_) return;
        int nch = std::min(numChannels, (int)ch_.size());
        int frames = std::min(numFrames, maxBlock_);
        for (int i = 0; i < frames; ++i) {
            masterBuf_[i]   = master_.next();
            preStageBuf_[i] = preStage_.next();
            powerBuf_[i]    = power_.next();
        }

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (!x) continue;
            Channel& st = ch_[c];

            // input band-limit (base rate)
            for (int i = 0; i < frames; ++i)
                x[i] = detail::sanitize(st.inLpf.process(st.inHpf.process(x[i])));

            // --- everything from the first grid to the OT, at 4x ------------
            st.os.upsample(x, frames);
            float* up = st.os.data();
            int m = st.os.upSize(frames);
            for (int i = 0; i < m; ++i) {
                float s = up[i];
                // The oversampled index maps back onto the base-rate ramp.
                const int base = i / kOs;
                const float stageG  = preStageBuf_[base];
                const float powerG  = powerBuf_[base];

                // preamp cascade
                for (int stg = 0; stg < numStages_; ++stg) {
                    if (brightOn_ && stg == brightStage_) {
                        const double y = brightB0_ * s + st.brightZ;
                        st.brightZ = brightB1_ * s - brightA1_ * y;
                        s = (float) y;
                    }
                    const float a = stageG * s;
                    // Grid conduction -> bias travel: the operating point drifts
                    // toward cutoff as the stage is driven, so the clipping
                    // asymmetry (and its duty-cycle shift = blocking) tracks how
                    // hard you play. The static term's alternating sign models
                    // the inverting stage-to-stage flip.
                    s = adaaTanh(a + stageBias(a, st.gridQ[stg], stg),
                                 st.preState[stg]);
                    s = st.cpl[stg].process(s);      // coupling HPF
                    s = st.miller[stg].process(s);   // Miller / plate-load LPF
                }

                // cathode follower: unity-ish and clean until it runs out of
                // grid, then an abrupt asymmetric squash. Feeds the tone stack.
                if (cfDrive_ > 0.f) {
                    const float y = adaaTanh(cfDrive_ * s + kCfBias, st.cfState);
                    s = st.cfLpf.process(y * (1.f / cfDrive_));
                }

                s = st.preDc.process(s);

                // tone stack + presence (inside the cascade, OS rate)
                if (stackOn_)
                    s = (float) st.stack.processD(s);
                else
                    s = (float)st.treble.processD(
                            st.mid.processD(st.bass.processD(s)));
                s = st.presence.process(s);

                // post-tone-stack recovery stage (5150/Peavey family)
                if (postDrive_ > 0.f) {
                    const float a = postDrive_ * s;
                    s = adaaTanh(a + stageBias(a, st.postQ, 0), st.postState);
                    s = st.postMiller.process(st.postCpl.process(s));
                }

                // supply sag: attack compresses, release blooms back.
                if (sagK_ > 0.f) {
                    const float env = st.sagEnv.process(s);
                    s *= 1.0f / (1.0f + sagK_ * env);
                }

                if (resoOn_) s = (float) st.reso.processD(s);

                // power amp with negative feedback. Small-signal gain stays
                // powerDrive_ (the (1+nfb) pre-gain cancels the loop's
                // attenuation); what the loop buys is the firm, corrected feel
                // right up to the point where it runs out of authority.
                const float fb = st.nfbLpf.process(st.powerY1);
                s = adaaTanh(powerG * (1.f + nfb_) * s - nfb_ * fb,
                             st.powerState);
                st.powerY1 = s;

                // output transformer: LF limit -> soft core saturation -> HF
                // bandwidth. Unity small-signal gain; it only bites on peaks.
                s = st.otHpf.process(s);
                if (otDrive_ > 0.f)
                    s = adaaTanh(otDrive_ * s, st.otState) * (1.f / otDrive_);
                s = st.otLpf.process(s);

                up[i] = detail::sanitize(s);
            }
            st.os.downsample(x, frames);

            for (int i = 0; i < frames; ++i)
                x[i] = detail::sanitize(st.outDc.process(x[i]) * masterBuf_[i]);
        }
    }

    int latencySamples() const { return 0; }

private:
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    // Grid offset for one stage: the static operating point (sign alternates
    // with the inverting stage-to-stage flip) plus the blocking term — the
    // rectified grid excess, RC-averaged in `q`, dragging the grid negative.
    inline float stageBias(float a, float& q, int stg) const {
        const float excess = (a > kGridClamp) ? (a - kGridClamp) : 0.f;
        q += gridCoef_ * (excess - q);                      // linear one-pole RC
        return ((stg & 1) ? -bias_ : bias_) - biasDepth_ * q;
    }

    // 4x, not 2x — and measurably not 8x either. Rebuilding at 8x moved the
    // dual-tone alias floor by 0.3 dB (Djent -62.3 vs -61.0 dBc, Recto Triple
    // -46.1 vs -45.8) for 2.0x the CPU: once the Miller low-passes have starved
    // the cascade of the HF that was folding, 4x + ADAA is already past the
    // point where oversampling is the limiting factor. The residual at 4x is
    // high-order intermodulation, not aliasing.
    static const int kOs = 4;
    static const int kMaxStages = 3;
    static const int kNumVoicings = 27;

    // The bright circuit (see setParams): Fender AB763, 120 pF across a 1 MOhm
    // audio-taper volume pot.
    static constexpr double kBrightCapF   = 120e-12;
    static constexpr double kBrightPotOhm = 1e6;

    // Cathode-follower grid offset — small, fixed; the asymmetry it creates is
    // what the tone stack downstream turns into the classic crunch bark.
    static constexpr float kCfBias = 0.12f;

    // Grid conduction / blocking distortion. When a stage is driven hard enough
    // that the grid swings positive it conducts, and the resulting current
    // charges the coupling cap through the grid-leak resistor — dragging the
    // operating point toward cutoff. So the bias travel is: rectify the excess
    // above the grid clamp, then a first-order RC.
    //
    // The rectifier MUST be memoryless and the averager MUST be linear. The
    // first cut of this used the shared peak `EnvelopeFollower`, whose
    // attack/release switch is a NONLINEAR RECURSION: driven by a periodic
    // chord it locked into a period-2 orbit and sprayed ghost partials at
    // half-grid frequencies. Measured on the Djent model, a dual-tone alias
    // floor of -66.9 dBc (no bias travel) collapsed to -29.8 dBc with it, and
    // the junk moved from 17-19 kHz (where the cab kills it) down to 1.5-4 kHz
    // (where it does not). max(0, x - clamp) into a one-pole is both the actual
    // circuit and subharmonic-free: a memoryless non-linearity followed by an
    // LTI filter cannot manufacture a subharmonic.
    static constexpr float kGridClamp  = 1.0f;   // grid conducts above this
    static constexpr float kGridLeakMs = 40.f;   // coupling-cap / grid-leak RC

    // Circuits-inspired per-voicing coefficient table. Same topology
    // everywhere — only numbers change, so switching voicings costs nothing.
    struct Voicing {
        int   stages;                    // preamp waveshaper stages (1..3)
        float driveBaseDb, driveRangeDb; // preamp drive = base + gain*range
        float bias;                      // static grid offset (even harmonics)
        float biasDepth;                 // level-dependent bias travel (blocking)
        float cplHz[3];                  // inter-stage coupling HPFs (OS rate)
        float millerHz[3];               // inter-stage plate/Miller LPFs (OS rate)
        float inLpfHz;                   // input band-limit top
        float cfDrive, cfLpfHz;          // cathode follower (0 = bypassed)
        float bassFc;                    // tone-stack low-shelf corner
        float midFc, midQ;               // tone-stack mid peak
        float trebFc;                    // tone-stack high-shelf corner
        float bassOfsDb, midOfsDb, trebOfsDb; // fixed voicing tilt (knobs at noon)
        float presFc, presBaseDb, presMaxDb;  // presence shelf (base = less NFB)
        float postDrive;                 // post-tone-stack recovery stage (0 = none)
        float powerBase, powerRange;     // power drive = base + range*master
        float nfb, nfbHz;                // power-amp negative feedback + loop lag
        float otDrive, otHpfHz, otLpfHz; // output transformer
        float sag;                       // 0..1 supply-sag depth
        float outTrim;                   // output level match vs the pre-W6 model
        // TOTAL bass (dB) the coupling network may remove, split evenly over the
        // stages present. This is a BUDGET, not a filter corner — `cplHz[]`
        // still sets where the shelf turns over. 0 restores the old unbounded
        // high-pass and must never be used on a guitar voicing.
        float cplCutDb;
        // 2026-10: 0 = the shelf trio above (bassFc..trebOfsDb); k > 0 = the
        // passive circuit stack kTmbStacks[k - 1] (dcdsp_tonestack.h), which
        // then ignores the shelf fields. Rows 0-23 leave it out of their
        // initialiser, i.e. 0: their sound does not move.
        int   toneStack;
    };

    // 0.16 W6: rows 0-6 keep their original drive/tone/sag numbers; their new
    // fields are deliberately gentle (high Miller corners, small bias travel,
    // no cathode follower on the cleans, light NFB) so the voicings people have
    // saved still sound like themselves — they just stop aliasing. Rows 7-23
    // are the named roster; the hi-gain rows (2/4/14/15/16) get the full
    // treatment: tight Miller corners, real bias travel, a cathode follower and
    // — for the 5150 family — the post-tone-stack recovery stage.
    static constexpr Voicing kVoicings[kNumVoicings] = {
        // 0 Blackface Twin 85 — clean, stiff, lots of NFB, no CF.
        {  1,  0.f,  10.f, 0.10f, 0.06f, {100.f, 150.f, 220.f}, {16000.0f, 16000.0f, 16000.0f}, 11000.f,
           0.00f, 16000.0f, 100.f,  650.f, 0.70f, 3000.f,  0.0f, -2.0f,  1.0f, 4500.f, 0.0f,  6.0f,
           0.0f, 0.60f, 0.80f, 0.70f, 15000.f, 0.9f, 28.f, 12000.f, 0.05f, 1.02f, 4.0f },
        // 1 Plexi 50 — CF into the stack (that IS a Plexi), medium NFB.
        {  2,  8.f,  22.f, 0.10f, 0.14f, {100.f, 150.f, 220.f}, {14000.0f, 12500.0f, 11000.0f}, 11000.f,
           1.10f, 12500.0f, 110.f,  800.f, 0.80f, 3200.f,  0.0f,  1.5f,  0.0f, 4500.f, 0.0f,  6.0f,
           0.0f, 0.60f, 0.80f, 0.55f, 15000.f, 1.0f, 30.f, 12000.f, 0.12f, 1.16f, 6.0f },
        // 2 Five-Oh-One Lead — 5150 family: CF + post-tone-stack recovery stage.
        {  3, 14.f,  30.f, 0.10f, 0.34f, {120.f, 180.f, 250.f}, {13000.0f, 11000.0f,  9500.0f}, 10000.f,
           1.30f, 12000.0f, 120.f,  650.f, 0.90f, 3500.f,  0.0f, -1.5f,  0.0f, 4800.f, 0.0f,  6.0f,
           2.2f, 0.60f, 0.80f, 0.55f, 15000.f, 1.1f, 32.f,  9500.f, 0.04f, 1.22f, 9.0f },
        // 3 AC-Thirty — cathode-biased, NO negative feedback (that's the point).
        {  2,  6.f,  18.f, 0.18f, 0.16f, { 90.f, 140.f, 200.f}, {14500.0f, 13500.0f, 12500.0f}, 12500.f,
           0.90f, 13500.0f,  90.f,  900.f, 0.70f, 2500.f, -1.0f,  0.0f,  3.0f, 5500.f, 3.0f,  9.0f,
           0.0f, 0.70f, 0.90f, 0.00f, 15000.f, 1.05f, 32.f, 13000.f, 0.18f, 1.17f, 6.0f },
        // 4 Recto Dual — loose lows into the clippers, big bias travel, low NFB.
        {  3, 16.f,  34.f, 0.12f, 0.30f, { 60.f, 110.f, 160.f}, {12500.0f, 10500.0f,  9000.0f}, 10000.f,
           1.40f, 11500.0f,  90.f,  550.f, 0.80f, 3800.f,  1.5f, -5.0f,  1.0f, 4800.f, 0.0f,  7.0f,
           0.0f, 0.70f, 0.90f, 0.35f, 15000.f, 1.2f, 30.f, 11000.f, 0.22f, 1.24f, 3.0f },
        // 5 Jazz Solid 120 — solid state: no CF, no OT, heavy NFB, no sag.
        //   Re-staged 2026-10 (DotCharter #71): it used to ATTENUATE (preamp
        //   -6..+2 dB, power 0.25..0.60) and rendered ~15 LU under the Twin at
        //   equal knobs. Its preamp and power stage now take the Twin's gain
        //   staging — still one clean stage with the heavy NFB, so it stays the
        //   cleanest amp in the roster; it just stops being the quietest.
        {  1,  0.f,  10.f, 0.00f, 0.00f, { 80.f, 120.f, 180.f}, {18000.0f, 18000.0f, 18000.0f}, 14000.f,
           0.00f, 18000.0f, 100.f,  800.f, 0.60f, 4000.f,  0.0f,  0.0f,  0.0f, 6000.f, 0.0f,  4.0f,
           0.0f, 0.60f, 0.80f, 0.90f, 15000.f, 0.0f, 20.f, 14000.f, 0.00f, 0.97f, 3.0f },
        // 6 Brown Variac — starved supply, big sag, loose OT.
        {  2, 10.f,  26.f, 0.14f, 0.24f, {110.f, 160.f, 230.f}, {14000.0f, 12500.0f, 11000.0f},  9500.f,
           1.20f, 12000.0f, 100.f,  750.f, 0.80f, 3000.f,  0.5f,  3.0f, -1.0f, 4000.f, 0.0f,  6.0f,
           0.0f, 1.00f, 0.80f, 0.45f, 15000.f, 1.4f, 34.f, 10500.f, 0.35f, 1.23f, 6.0f },
        // --- American cleans -------------------------------------------------
        // 7 Tweed Champ 5W — no NFB at all, tiny OT that folds early.
        {  1,  2.f,  16.f, 0.16f, 0.20f, { 70.f, 110.f, 160.f}, {13500.0f, 13000.0f, 12500.0f},  9000.f,
           0.00f, 13000.0f, 120.f,  700.f, 0.60f, 2600.f,  1.0f,  1.0f, -1.0f, 3500.f, 1.0f,  5.0f,
           0.0f, 0.55f, 1.10f, 0.00f, 15000.f, 1.6f, 45.f,  7500.f, 0.30f, 1.29f, 4.0f },
        // 8 Tweed Twin 40
        {  2,  0.f,  14.f, 0.14f, 0.12f, { 80.f, 120.f, 180.f}, {15000.0f, 14000.0f, 13000.0f}, 11000.f,
           0.00f, 14000.0f, 110.f,  750.f, 0.60f, 3000.f,  0.5f,  1.5f,  0.0f, 3800.f, 0.5f,  5.0f,
           0.0f, 0.50f, 0.85f, 0.35f, 15000.f, 1.0f, 32.f, 10500.f, 0.14f, 1.06f, 4.0f },
        // 9 Blackface Deluxe 22
        {  1,  1.f,  13.f, 0.11f, 0.10f, { 90.f, 140.f, 200.f}, {16000.0f, 16000.0f, 16000.0f}, 11500.f,
           0.00f, 15000.0f, 100.f,  620.f, 0.70f, 3200.f,  0.0f, -2.5f,  1.5f, 4200.f, 0.5f,  6.5f,
           0.0f, 0.55f, 0.95f, 0.65f, 15000.f, 1.0f, 30.f, 11000.f, 0.14f, 1.06f, 4.0f },
        // --- British ---------------------------------------------------------
        // 10 Plexi 100
        {  2,  5.f,  20.f, 0.10f, 0.12f, {110.f, 160.f, 230.f}, {14500.0f, 13500.0f, 12000.0f}, 12000.f,
           1.00f, 13000.0f, 110.f,  820.f, 0.80f, 3400.f,  0.0f,  1.0f,  0.5f, 4800.f, 0.5f,  7.0f,
           0.0f, 0.70f, 0.70f, 0.55f, 15000.f, 0.9f, 28.f, 11000.f, 0.07f, 1.12f, 6.0f },
        // 11 JMP-Eight-Hundred — tighter, more NFB, hotter preamp.
        {  2, 12.f,  26.f, 0.10f, 0.20f, {130.f, 190.f, 260.f}, {13500.0f, 12000.0f, 10500.0f}, 10500.f,
           1.20f, 12500.0f, 120.f,  780.f, 0.90f, 3600.f, -0.5f,  2.5f,  0.0f, 4600.f, 0.0f,  7.0f,
           0.0f, 0.65f, 0.85f, 0.60f, 15000.f, 1.0f, 32.f, 11500.f, 0.10f, 1.19f, 6.0f },
        // 12 Two-Oh-Three Crunch
        {  2, 10.f,  24.f, 0.12f, 0.18f, {100.f, 150.f, 210.f}, {14000.0f, 12500.0f, 11000.0f}, 10000.f,
           1.10f, 13000.0f, 130.f,  600.f, 0.75f, 3200.f,  1.0f,  2.0f, -0.5f, 4200.f, 0.0f,  6.5f,
           0.0f, 0.70f, 0.80f, 0.50f, 15000.f, 1.1f, 30.f, 10000.f, 0.13f, 1.19f, 6.0f },
        // 13 AC-Fifteen — cathode-biased, no NFB, folds early.
        {  2,  8.f,  20.f, 0.20f, 0.20f, { 95.f, 145.f, 205.f}, {15000.0f, 14000.0f, 13000.0f}, 12500.f,
           0.90f, 14000.0f,  95.f,  950.f, 0.70f, 2400.f, -1.0f,  0.5f,  3.5f, 5800.f, 3.5f,  9.5f,
           0.0f, 0.75f, 1.05f, 0.00f, 15000.f, 1.25f, 38.f, 12000.f, 0.28f, 1.22f, 6.0f },
        // --- hi-gain ---------------------------------------------------------
        // 14 Recto Triple — more headroom than the Dual, same loose voicing.
        {  3, 18.f,  36.f, 0.10f, 0.30f, { 75.f, 125.f, 175.f}, {12500.0f, 10500.0f,  9000.0f}, 10500.f,
           1.40f, 11500.0f,  95.f,  560.f, 0.85f, 4000.f,  1.0f, -5.5f,  1.5f, 5000.f, 0.0f,  7.5f,
           0.0f, 0.80f, 0.85f, 0.40f, 15000.f, 1.0f, 28.f, 11000.f, 0.16f, 1.23f, 3.0f },
        // 15 Modern Djent 8-String — tight coupling caps, high NFB, stiff OT.
        {  3, 20.f,  38.f, 0.08f, 0.30f, {150.f, 220.f, 300.f}, {13500.0f, 11500.0f, 10000.0f}, 11000.f,
           1.30f, 12500.0f,  85.f,  500.f, 1.00f, 4500.f,  2.0f, -6.0f,  2.5f, 5500.f, 0.0f,  8.0f,
           2.0f, 0.85f, 0.75f, 0.75f, 15000.f, 0.8f, 34.f, 10000.f, 0.06f, 1.36f, 9.0f },
        // 16 PowerBall Lead — 5150 family: CF + post-tone-stack recovery stage.
        {  3, 17.f,  34.f, 0.10f, 0.36f, {120.f, 180.f, 250.f}, {13000.0f, 11000.0f,  9500.0f}, 10500.f,
           1.35f, 12000.0f, 100.f,  900.f, 0.80f, 3800.f,  0.0f, -1.0f,  1.0f, 4900.f, 0.0f,  7.5f,
           2.2f, 0.75f, 0.85f, 0.60f, 15000.f, 1.1f, 32.f,  9500.f, 0.10f, 1.26f, 9.0f },
        // --- boutique / other ------------------------------------------------
        // 17 Overdrive Special
        {  2,  6.f,  22.f, 0.12f, 0.18f, { 85.f, 130.f, 190.f}, {14000.0f, 13000.0f, 11500.0f}, 11500.f,
           1.10f, 13000.0f, 105.f,  720.f, 0.90f, 3400.f,  0.0f,  3.0f,  0.5f, 4700.f, 1.0f,  7.0f,
           0.0f, 0.65f, 0.90f, 0.60f, 15000.f, 1.0f, 30.f, 10500.f, 0.16f, 1.16f, 6.0f },
        // 18 Class-A Boutique 30 — little NFB.
        {  2,  5.f,  19.f, 0.16f, 0.18f, { 90.f, 140.f, 200.f}, {14500.0f, 13500.0f, 12500.0f}, 12000.f,
           0.90f, 13500.0f, 100.f,  850.f, 0.75f, 2800.f, -0.5f,  1.0f,  2.0f, 5200.f, 2.0f,  8.0f,
           0.0f, 0.68f, 0.95f, 0.15f, 15000.f, 1.1f, 34.f, 12000.f, 0.20f, 1.17f, 6.0f },
        // --- bass / DI --------------------------------------------------------
        // 19 Bass Three-Sixty
        {  1, -4.f,  10.f, 0.00f, 0.04f, { 35.f,  60.f, 100.f}, {11000.0f, 10000.0f,  9000.0f},  6000.f,
           0.00f, 10000.0f,  60.f,  500.f, 0.50f, 2200.f,  3.0f,  0.5f, -2.0f, 3000.f, 0.0f,  3.0f,
           0.0f, 0.35f, 0.45f, 0.70f, 15000.f, 0.6f, 22.f,  8000.f, 0.03f, 0.98f, 3.0f },
        // 20 Bass Flip-Top
        {  2,  2.f,  16.f, 0.10f, 0.14f, { 40.f,  70.f, 110.f}, {11000.0f, 10000.0f,  9000.0f},  7000.f,
           0.00f,  9000.0f,  70.f,  550.f, 0.60f, 2400.f,  2.5f,  1.5f, -1.5f, 3200.f, 0.0f,  4.0f,
           0.0f, 0.55f, 0.75f, 0.40f, 15000.f, 1.3f, 30.f,  7500.f, 0.18f, 1.14f, 3.0f },
        // 21 SVT Bass 300W
        {  2,  0.f,  18.f, 0.10f, 0.08f, { 45.f,  80.f, 120.f}, {12000.0f, 11000.0f, 10000.0f},  8000.f,
           0.00f, 10000.0f,  65.f,  700.f, 0.70f, 2800.f,  2.0f,  2.0f, -1.0f, 3400.f, 0.0f,  5.0f,
           0.0f, 0.60f, 0.90f, 0.60f, 15000.f, 0.8f, 25.f,  9000.f, 0.06f, 1.03f, 3.0f },
        // 22 Acoustic Pre — clean DI-ish, no OT, no CF.
        {  1, -8.f,   6.f, 0.00f, 0.00f, { 30.f,  50.f,  90.f}, {18000.0f, 18000.0f, 18000.0f}, 15000.f,
           0.00f, 18000.0f,  90.f,  500.f, 0.80f, 5000.f,  0.0f, -2.0f,  2.5f, 6500.f, 1.0f,  3.0f,
           0.0f, 0.20f, 0.30f, 0.90f, 15000.f, 0.0f, 20.f, 15000.f, 0.00f, 0.97f, 3.0f },
        // 23 Studio Direct — flat, transparent.
        {  1, -6.f,   7.f, 0.00f, 0.00f, { 40.f,  70.f, 110.f}, {18000.0f, 18000.0f, 18000.0f}, 16000.f,
           0.00f, 18000.0f, 100.f, 1000.f, 0.70f, 5500.f,  0.0f,  0.0f,  0.0f, 6000.f, 0.0f,  2.0f,
           0.0f, 0.30f, 0.35f, 0.90f, 15000.f, 0.0f, 20.f, 16000.f, 0.00f, 0.97f, 3.0f },
        // --- CIRCUIT voicings (2026-10, DotCharter #70) -----------------------
        // Each is an existing row of the same family with its tone stack
        // replaced by the documented passive circuit (dcdsp_tonestack.h), plus
        // only the deltas a source states. The nonlinear numbers are the
        // family row's, NOT a new calibration — so these sound like their
        // family, voiced through the real stack. The shelf fields stay as the
        // family row's and are unused (toneStack > 0).
        //
        // 24 Tweed Bass 45 — Fender 5F6-A topology: row 8 (Tweed Twin 40) with
        //   * the 5F6-A stack (kTmbStacks[0], Yeh & Smith 2006);
        //   * a cathode follower in front of it (5F6-A V2B; Yeh & Smith 1.2:
        //     "the tone stack is preceded by a cathode follower"), with row 1's
        //     follower numbers — the same 12AX7 follower on both schematics;
        //   * NFB 0.60 = row 1's 0.55 / 0.91: the 50 W Marshall gets "9% less
        //     NFB than the Bassman" (robrobinette.com, "How the Marshall Plexi,
        //     2204 and JCM800 Amplifiers Work": 56 k from the 2 Ohm tap vs
        //     100 k from the 4 Ohm tap).
        {  2,  0.f,  14.f, 0.14f, 0.12f, { 80.f, 120.f, 180.f}, {15000.0f, 14000.0f, 13000.0f}, 11000.f,
           1.10f, 12500.0f, 110.f,  750.f, 0.60f, 3000.f,  0.5f,  1.5f,  0.0f, 3800.f, 0.5f,  5.0f,
           0.0f, 0.50f, 0.85f, 0.60f, 15000.f, 1.0f, 32.f, 10500.f, 0.14f, 1.06f, 4.0f, 1 },
        // 25 Plexi 50 Circuit — row 1 (Plexi 50) through the Marshall JMP50
        //   Model 1987 stack (kTmbStacks[1], Kuehnel). Nothing else changes.
        {  2,  8.f,  22.f, 0.10f, 0.14f, {100.f, 150.f, 220.f}, {14000.0f, 12500.0f, 11000.0f}, 11000.f,
           1.10f, 12500.0f, 110.f,  800.f, 0.80f, 3200.f,  0.0f,  1.5f,  0.0f, 4500.f, 0.0f,  6.0f,
           0.0f, 0.60f, 0.80f, 0.55f, 15000.f, 1.0f, 30.f, 12000.f, 0.12f, 1.16f, 6.0f, 2 },
        // 26 Plexi 100 Circuit — row 10 (Plexi 100) through the same 1987
        //   stack (Kuehnel documents the 50 W 1987; the 100 W Super Lead's stack
        //   is taken as the same 33 k / 500 pF network — that is an assumption,
        //   not a second source), with NFB 0.78 = 0.55 x 1.41: "The 100 Watt
        //   Marshall amps use the same NFB circuit as the 50 watt amps but ...
        //   they get 41% more NFB" (robrobinette.com, as above).
        {  2,  5.f,  20.f, 0.10f, 0.12f, {110.f, 160.f, 230.f}, {14500.0f, 13500.0f, 12000.0f}, 12000.f,
           1.00f, 13000.0f, 110.f,  820.f, 0.80f, 3400.f,  0.0f,  1.0f,  0.5f, 4800.f, 0.5f,  7.0f,
           0.0f, 0.70f, 0.70f, 0.78f, 15000.f, 0.9f, 28.f, 11000.f, 0.07f, 1.12f, 6.0f, 2 },
    };

    struct Channel {
        detail::OversamplerNx os;
        float preState[kMaxStages] = {0, 0, 0};
        float gridQ[kMaxStages] = {0, 0, 0}; // grid-leak charge (bias travel)
        float postQ = 0.f;
        float cfState = 0.f, postState = 0.f, powerState = 0.f, otState = 0.f;
        float powerY1 = 0.f;                 // NFB loop: 1-sample lag
        double brightZ = 0.0;                // bright-cap first-order state
        ThirdOrderD stack;                   // circuit tone stack (rows 24+)
        BiquadD reso;                        // power-amp resonance shelf
        Biquad inHpf, inLpf;                 // base rate — float is fine
        BiquadD bass, mid, treble, presence; // OS rate — needs double state
        OnePole cpl[kMaxStages];             // inter-stage coupling HPFs
        OnePole miller[kMaxStages];          // inter-stage plate/Miller LPFs
        OnePole cfLpf, postCpl, postMiller;
        OnePole nfbLpf, otHpf, otLpf;
        DcBlocker preDc;                     // OS rate (before the tone stack)
        DcBlocker outDc;                     // base rate (after downsampling)
        EnvelopeFollower sagEnv;             // supply-sag detector
        void reset() {
            os.reset();
            for (int s = 0; s < kMaxStages; ++s) {
                preState[s] = 0.f; gridQ[s] = 0.f;
                cpl[s].reset(); miller[s].reset();
            }
            postQ = 0.f;
            cfState = postState = powerState = otState = powerY1 = 0.f;
            brightZ = 0.0;
            stack.reset(); reso.reset();
            inHpf.reset(); inLpf.reset();
            bass.reset(); mid.reset(); treble.reset(); presence.reset();
            cfLpf.reset(); postCpl.reset(); postMiller.reset();
            nfbLpf.reset(); otHpf.reset(); otLpf.reset();
            preDc.reset(); outDc.reset();
            sagEnv.reset();
        }
    };

    double sr_ = 48000.0;
    int maxBlock_ = 512;
    int numStages_ = 2;
    float stageGain_[kMaxStages] = {1, 1, 1};
    float powerDrive_ = 1.f;
    float bias_ = 0.10f;
    float biasDepth_ = 0.f;
    float gridCoef_ = 0.f;   // grid-leak RC coefficient (set in prepare)
    float cfDrive_ = 0.f;
    float postDrive_ = 0.f;
    float nfb_ = 0.f;
    float otDrive_ = 0.f;
    float sagK_ = 0.f;
    bool  stackOn_ = false;    // circuit tone stack in place of the shelf trio
    bool  brightOn_ = false;
    int   brightStage_ = 0;    // the stage the bright cap feeds
    double brightB0_ = 1.0, brightB1_ = 0.0, brightA1_ = 0.0;
    bool  resoOn_ = false;
    AmpParams params_;
    SmoothedValue master_;
    std::vector<float> masterBuf_;
    /// The preamp drive (per stage) and the power-amp drive, both base-rate
    /// ramps pre-filled per block. stageGain_[]/powerDrive_ remain the TARGETS.
    SmoothedValue preStage_, power_;
    std::vector<float> preStageBuf_, powerBuf_;
    std::vector<Channel> ch_;
};

} // namespace dcdsp
