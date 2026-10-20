// dcdsp_micpos.h — cab MIC POSITION: the microphone layer that goes AFTER a
// cab or dual-cab node. It loads no impulse response; it models what moving
// the mic does to one.
//
// Models: five classic guitar mics (a dynamic 57, a ribbon 121, a large
// dynamic 421, a small condenser and, since 2026-10, a large-diaphragm TUBE
// condenser) as fixed EQ curves; capsule position from dust cap to cone edge;
// the mic's ANGLE off the speaker axis (0-60 degrees, 2026-10); the proximity
// effect and the floor bounce as a function of distance; a handful of early
// reflections; and a second, ribbon, mic blended in.
// Topology: delay line -> [mic EQ bank -> its off-axis stage] -> position
// blend -> proximity blend -> floor-bounce tap -> second-mic tap through its
// own bank -> room multitap -> level. Everything is LINEAR; the module never
// clips.
//
// POSITION and ANGLE are two different mechanisms (DotCharter #75). Position
// moves the capsule across the cone, still pointing straight at it: the dust
// cap's top end falls away toward the edge (`position`). Angle turns the
// capsule off the axis at the same spot: the MIC's polar response decides what
// is lost, so it is per mic and read off the manufacturers' published polar
// charts (kOffAxis below, sources there), and it also thins the proximity
// effect, which only the pressure-GRADIENT part of a directional capsule
// produces (cardioid = 1/2 pressure + 1/2 gradient: the gradient term goes as
// cos(angle), so its share of the output is cos/(A + (1 - A) cos), A the
// pressure share — Eargle, "The Microphone Book", ch. 5). angle = 0 runs no
// part of the stage, so a preset that never sets it is bit-identical.
//
// Easy to get wrong: the two mics sit at DIFFERENT distances, so summing them
// straight is a comb filter nobody asked for. The FIRST capsule therefore owns
// the reference plane — its tap is a fixed `alignSamples_`, whatever the
// distance knob says — and the second capsule is read RELATIVE to it, at
// `alignSamples_ + (kSecondMicCm - distanceCm) * samplesPerCm_`. Two things
// fall out of that and both are load-bearing:
//   * `latencySamples()` is genuinely `alignSamples_`, so branch PDC
//     compensates the delay the dry path actually has. Referring BOTH taps to
//     the 30 cm plane instead (what this core shipped with) made the dry tap
//     `alignSamples_ - d*spc` while the reported number stayed `alignSamples_`
//     — 7 samples of PDC error at the 5 cm default and 42 at the far end.
//     `MicPos_Latency_IsTheDelayTheDryPathActuallyHas` measures a Dirac.
//   * the relative SIGN is right: at `distanceCm < kSecondMicCm` the near mic
//     is the early one. Referred to a common far plane the CLOSER capsule came
//     out LATER, which no amount of PDC can fix because both report the same
//     number.
// The knob therefore delays the FAR mic instead of un-delaying the near one,
// and the second tap is the one that ramps.
// The second trap: `room = 0` must be BIT
// identical to no room at all, which is why the taps are summed with a weight
// of exactly zero rather than skipped behind a branch that could drift.
#pragma once

#include "dcdsp_primitives.h"
#include "dcdsp_detail_os.h"

#include <vector>
#include <cmath>
#include <algorithm>

namespace dcdsp {

struct MicPosParams {
    int   mic        = 0;    // 0 "57" / 1 "121" / 2 "421" / 3 Condenser / 4 Tube
    float position   = 0.3f; // 0..1 dust cap -> cone edge
    float distanceCm = 5.f;  // 0..30 cm
    float room       = 0.f;  // 0..1 early reflections
    float blend2     = 0.f;  // 0..1 the second (ribbon at 10 cm) mic
    float level      = 0.7f; // 0..1
    float angleDeg   = 0.f;  // 0..60 degrees off the speaker axis (2026-10)
};

class MicPosCore {
public:
    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_       = (sampleRate > 0.0) ? sampleRate : 48000.0;
        maxBlock_ = std::max(1, maxBlockSize);

        // Everything the RATE owns, re-derived: samples per centimetre of air,
        // the reference plane, the floor bounce and every room tap.
        samplesPerCm_ = static_cast<float>(sr_ / kSpeedOfSoundCmPerSec);
        alignSamples_ = static_cast<int>(std::lround(kReferenceCm * samplesPerCm_));
        floorExtra_   = kFloorExtraCm * samplesPerCm_;
        for (int i = 0; i < kNumRoomTaps; ++i)
            roomTap_[i] = static_cast<float>(kRoomTapMs[i] * 0.001 * sr_);

        // The second capsule can sit up to kSecondMicCm BEHIND the reference
        // plane (at distanceCm = 0), so its tap is the one that can exceed
        // alignSamples_ + the room taps. Allocated for that worst case, never
        // for the current knob.
        const int maxDelay = alignSamples_
                           + static_cast<int>(std::ceil(kSecondMicCm * samplesPerCm_))
                           + static_cast<int>(std::ceil(floorExtra_))
                           + static_cast<int>(std::ceil(roomTap_[kNumRoomTaps - 1]))
                           + 8;

        ch_.assign(static_cast<size_t>(std::max(1, numChannels)), Channel{});
        for (auto& c : ch_) {
            // Allocated for the WORST case: the longest room tap at the
            // highest rate, not for the current knob positions.
            c.line.prepare(maxDelay);
            c.line.reset();
            c.reset();
        }

        levelBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        posBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        proxBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        floorBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        roomBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        blend2Buf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        mic2DelayBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        vmixBuf_.assign(static_cast<size_t>(maxBlock_), 1.f);
        angleBuf_.assign(static_cast<size_t>(maxBlock_), 0.f);
        for (auto& b : angGainBuf_) b.assign(static_cast<size_t>(maxBlock_), 1.f);

        level_.reset(sr_, kRampSeconds);
        pos_.reset(sr_, kRampSeconds);
        prox_.reset(sr_, kRampSeconds);
        floor_.reset(sr_, kRampSeconds);
        room_.reset(sr_, kRampSeconds);
        blend2_.reset(sr_, kRampSeconds);
        mic2Delay_.reset(sr_, kRampSeconds);
        vmix_.reset(sr_, kRampSeconds);
        angle_.reset(sr_, kAngleRampSeconds);
        for (auto& g : angGain_) g.reset(sr_, kAngleRampSeconds);

        curMic_ = -1;
        // Each channel's off-axis shelves start stale (Channel{} holds -1), so
        // the first frame after a prepare designs them at the new rate.
        angleLive_ = false;
        setParams(params_);
        level_.snapToTarget();
        pos_.snapToTarget();
        prox_.snapToTarget();
        floor_.snapToTarget();
        room_.snapToTarget();
        blend2_.snapToTarget();
        mic2Delay_.snapToTarget();
        angle_.snapToTarget();
        for (auto& g : angGain_) g.snapToTarget();
        bankMic_[bank_ ^ 1] = curMic_;
        for (auto& c : ch_) configureMic(c.v[bank_ ^ 1], kMics[curMic_]);
        vmix_.setCurrentAndTarget(1.f);
    }

    void reset() {
        for (auto& c : ch_) { c.line.reset(); c.reset(); }
    }

    void setParams(const MicPosParams& p) {
        params_ = p;
        const int mi = clampIndex(p.mic, kNumMics);

        level_.setTarget(clamp01(p.level) * 1.3f);
        pos_.setTarget(clamp01(p.position));

        const float d = clampRange(p.distanceCm, 0.f, kReferenceCm);
        const float angle = clampRange(p.angleDeg, 0.f, kMaxAngleDeg);
        angle_.setTarget(angle);
        // Proximity falls off with distance; by 30 cm it is gone. Off axis
        // it thins too: only the gradient share of the capsule makes it (see
        // the header). 1.0 exactly on axis, so angle 0 changes nothing.
        prox_.setTarget((1.f - d / kReferenceCm) * gradientShare(mi, angle));
        // The floor bounce only matters once the mic is far enough off the
        // grille to hear the room at all, and it comes in gently.
        const float f = (d - kFloorOnsetCm) / (kReferenceCm - kFloorOnsetCm);
        const float fc = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
        floor_.setTarget(fc * fc * (3.f - 2.f * fc));   // smoothstep
        room_.setTarget(clamp01(p.room));
        blend2_.setTarget(clamp01(p.blend2));
        // The FIRST capsule owns the reference plane and never moves, so the
        // module's latency is a constant. The knob moves the SECOND capsule
        // relative to it: positive when the second mic is further away.
        mic2Delay_.setTarget(static_cast<float>(alignSamples_)
                             + (kSecondMicCm - d) * samplesPerCm_);

        if (mi != curMic_) {
            bank_ ^= 1;
            bankMic_[bank_] = mi;
            for (auto& c : ch_) {
                configureMic(c.v[bank_], kMics[mi]);
                configureAngle(c.v[bank_], mi, c.designedAngle < 0.f ? 0.f : c.designedAngle);
                c.v[bank_].reset();
            }
            // The incoming bank's broadband off-axis gain starts at the angle
            // the stage is at now, so the crossfade blends two settled mics.
            angGain_[bank_].setCurrentAndTarget(offAxisGain(mi, angle_.current()));
            vmix_.setCurrentAndTarget(0.f);
            vmix_.setTarget(1.f);
            curMic_ = mi;
        }
        // Each bank's broadband off-axis gain ramps to ITS mic's value at the
        // target angle (per sample, never a per-block step).
        for (int b = 0; b < 2; ++b)
            if (bankMic_[b] >= 0) angGain_[b].setTarget(offAxisGain(bankMic_[b], angle));

        for (auto& c : ch_) {
            // Fixed-shape, BOUNDED branches; the knob is the blend weight.
            c.posSh.setHighShelf(sr_, kPositionHz, 0.707, kPositionMaxDb);
            c.posBump.setPeaking(sr_, kPositionBumpHz, 1.0, kPositionBumpDb);
            c.proxSh.setLowShelf(sr_, kProximityHz, 0.8, kProximityMaxDb);
            configureMic(c.mic2, kMics[kSecondMicIndex]);
        }
    }

    void process(float* const* channels, int numChannels, int numFrames) {
        if (channels == nullptr || numFrames <= 0) return;
        if (numFrames > maxBlock_) return;   // skip whole, never half-process
        const int nch    = std::min(numChannels, static_cast<int>(ch_.size()));
        const int frames = numFrames;

        const bool fading = vmix_.isRamping() || vmix_.current() < 1.f;
        // The off-axis stage runs only while the angle is, or is ramping
        // from, something other than 0 — so angle 0 is the pre-2026-10 path
        // sample for sample.
        const bool angleLive = angle_.isRamping() || angle_.current() != 0.f
                            || angGain_[0].isRamping() || angGain_[1].isRamping();
        for (int i = 0; i < frames; ++i) {
            const size_t b = static_cast<size_t>(i);
            levelBuf_[b]    = level_.next();
            posBuf_[b]      = pos_.next();
            proxBuf_[b]     = prox_.next();
            floorBuf_[b]    = floor_.next();
            roomBuf_[b]     = room_.next();
            blend2Buf_[b]   = blend2_.next();
            mic2DelayBuf_[b] = mic2Delay_.next();
            vmixBuf_[b]     = vmix_.next();
            angGainBuf_[0][b] = angGain_[0].next();
            angGainBuf_[1][b] = angGain_[1].next();
            angleBuf_[b]      = angle_.next();
        }
        if (angleLive && !angleLive_) {
            // Waking the stage: its filters have not run since it last slept.
            for (auto& c : ch_) { c.v[0].resetAngle(); c.v[1].resetAngle(); }
        }
        angleLive_ = angleLive;

        // The dry tap IS the reported latency, by construction.
        const float md = static_cast<float>(alignSamples_);
        const int other = bank_ ^ 1;

        for (int c = 0; c < nch; ++c) {
            float* x = channels[c];
            if (x == nullptr) continue;
            Channel& st = ch_[static_cast<size_t>(c)];

            for (int i = 0; i < frames; ++i) {
                const size_t b = static_cast<size_t>(i);
                st.line.push(detail::sanitize(x[i]));

                const float dry = st.line.readAt(md);

                // The two off-axis SHELVES follow the ramping angle every
                // kAngleDesignStride frames (a fraction of a dB per step); the
                // broadband part, which is the audible level, ramps per sample.
                // Designed only when the angle actually moved, so a still knob
                // costs one compare per stride.
                if (angleLive && (i % kAngleDesignStride) == 0 && angleBuf_[b] != st.designedAngle) {
                    for (int bk = 0; bk < 2; ++bk)
                        if (bankMic_[bk] >= 0) configureAngle(st.v[bk], bankMic_[bk], angleBuf_[b]);
                    st.designedAngle = angleBuf_[b];
                }

                float s = st.v[bank_].process(dry);
                if (angleLive) s = angGainBuf_[bank_][b] * st.v[bank_].processAngle(s);
                if (fading) {
                    float o = st.v[other].process(dry);
                    if (angleLive) o = angGainBuf_[other][b] * st.v[other].processAngle(o);
                    const float m = vmixBuf_[b];
                    s = m * s + (1.f - m) * o;
                }

                // Off-axis: the top comes down and a little low mid comes up.
                const float pw = posBuf_[b];
                const float off = st.posBump.process(st.posSh.process(s));
                s = s + pw * (off - s);

                // Proximity: a bounded low shelf that fades out with distance.
                const float xw = proxBuf_[b];
                s = s + xw * (st.proxSh.process(s) - s);

                // Floor bounce: one late, quiet, polarity-correct reflection.
                s += floorBuf_[b] * kFloorGain * st.line.readAt(md + floorExtra_);

                // The second capsule, read at ITS plane-referred tap so the
                // two mics sum coherently instead of combing.
                const float m2 = st.mic2.process(st.line.readAt(mic2DelayBuf_[b]));
                s += blend2Buf_[b] * kSecondMicGain * m2;

                // Early reflections. `room == 0` adds exactly zero.
                float er = 0.f;
                for (int t = 0; t < kNumRoomTaps; ++t)
                    er += kRoomTapGain[t] * st.line.readAt(md + roomTap_[t]);
                s += roomBuf_[b] * er;

                x[i] = detail::sanitize(s * levelBuf_[b]);
            }
        }
    }

    /// Constant by construction AND equal to the delay the dry path really
    /// has: the first capsule's tap is exactly `alignSamples_` at every
    /// distance, and the knob moves the SECOND capsule relative to it.
    int latencySamples() const { return alignSamples_; }

    float levelCurrentForTest() const { return level_.current(); }

private:
    static constexpr float  kRampSeconds        = 0.010f;
    static constexpr double kSpeedOfSoundCmPerSec = 34300.0;
    static constexpr float  kReferenceCm        = 30.f;
    static constexpr float  kSecondMicCm        = 10.f;
    static constexpr int    kSecondMicIndex     = 1;      // the ribbon
    static constexpr float  kSecondMicGain      = 0.85f;
    static constexpr float  kFloorOnsetCm       = 15.f;
    static constexpr float  kFloorExtraCm       = 55.f;   // up and back off the floor
    static constexpr float  kFloorGain          = 0.28f;
    static constexpr double kPositionHz         = 3500.0;
    static constexpr double kPositionMaxDb      = -12.0;
    static constexpr double kPositionBumpHz     = 220.0;
    static constexpr double kPositionBumpDb     = 2.0;
    static constexpr double kProximityHz        = 150.0;
    static constexpr double kProximityMaxDb     = 8.0;
    static constexpr float  kMaxAngleDeg        = 60.f;
    static constexpr float  kAngleRampSeconds   = 0.020f;
    static constexpr int    kAngleDesignStride  = 32;     // frames between shelf designs

    static const int kNumRoomTaps = 5;
    static constexpr double kRoomTapMs[kNumRoomTaps]   = { 7.0, 13.0, 19.0, 29.0, 41.0 };
    static constexpr float  kRoomTapGain[kNumRoomTaps] = { 0.30f, 0.22f, -0.17f, 0.12f, -0.09f };

    static float clamp01(float v) {
        if (!std::isfinite(v)) return 0.f;
        return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    }
    static float clampRange(float v, float lo, float hi) {
        if (!std::isfinite(v)) return lo;
        return v < lo ? lo : (v > hi ? hi : v);
    }
    static int clampIndex(int v, int n) { return v < 0 ? 0 : (v > n - 1 ? n - 1 : v); }

    // A mic curve: a bounded low shelf, one bell and a bounded high shelf.
    // `hiNearNyquist` lets the top shelf sit above a quarter of the rate
    // (clampCutoffNearNyquist) — the tube mic's air roll-off is published at
    // 14-18 kHz. The four older curves keep the quarter-rate ceiling they were
    // voiced against, coefficient for coefficient.
    struct Mic {
        double loHz, loDb;
        double bellHz, bellQ, bellDb;
        double hiHz, hiDb;
        bool   hiNearNyquist;
    };
    static const int kNumMics = 5;
    static constexpr Mic kMics[kNumMics] = {
        // 0 "57" — the presence peak and the dynamic's top-end limit.
        { 120.0, -2.0,  5000.0, 1.0,  5.0,  9000.0, -6.0, false },
        // 1 "121" — ribbon: warm, no top, a touch of body.
        { 150.0,  2.0,   300.0, 0.9,  1.0,  4500.0, -9.0, false },
        // 2 "421" — large dynamic: forward mids, gentler top roll-off.
        { 120.0, -1.0,  3000.0, 1.2,  4.0, 10000.0, -4.0, false },
        // 3 Condenser — flat and open with a small low-mid scoop.
        { 100.0,  0.0,   800.0, 0.9, -2.0,  8000.0,  4.0, false },
        // 4 Tube — a large-diaphragm TUBE condenser, fitted (least squares,
        // RBJ forms at 48 kHz) to the Neumann U 67's published CARDIOID
        // frequency response (U 67 operating manual / quick guide,
        // Neumann.Berlin 2018, "Specifications U 67" page, cardioid chart,
        // read off the vector drawing): flat 315 Hz-5 kHz, a gentle low
        // roll-off (-2.2 dB at 100 Hz, -4.2 at 50 Hz), a broad +1.5 dB rise
        // at 9-10 kHz and the air falling away above it (-4.7 dB at 16 kHz).
        // Holds the chart within 0.55 dB from 50 Hz to 18 kHz. Unlike the
        // small "Condenser" above it has no top shelf boost; what makes it
        // the tube mic is the broad 10 kHz rise into a rolled-off top and the
        // softened low end, which close-miking (`prox_`) then fills back in.
        // DotCharter #75; `filter_ceiling`/`micpos` native tests pin it.
        { 117.668, -3.618, 10388.094, 1.235, 1.561, 15833.85, -9.406, true },
    };

    // The OFF-AXIS stage, per mic, at 30 / 45 / 60 degrees (0 = on axis = no
    // stage): a broadband gain plus a bounded low shelf and a bounded high
    // shelf at fixed corners, interpolated linearly in angle between rows.
    // `pressureShare` is the capsule's pressure (omni) share A: 0.5 for a
    // first-order cardioid, 0 for a figure-8 — it decides how fast the
    // proximity effect thins off axis (gradientShare()).
    //
    // Sources (read off the published polar charts at 0/30/45/60 degrees, then
    // fitted with the RBJ forms at 48 kHz; worst residual noted per mic):
    //  57        Shure SM57 user guide 27A12820 (2009), "Typical polar
    //            patterns" 125 Hz-8 kHz (vector drawing). Shelves at 600 Hz /
    //            8 kHz; <= 0.69 dB at every charted point. At 45 deg the chart
    //            reads -0.3 / -0.7 / -1.2 / -1.8 / -0.6 / -2.4 dB at
    //            125 / 500 / 1k / 2k / 4k / 8k Hz.
    //  Tube      Neumann U 67 operating manual (as above), cardioid polar
    //            pattern 125 Hz-16 kHz. Shelves at 300 Hz / 16 kHz (near-
    //            Nyquist: the large diaphragm's loss is at 16 kHz: -3.9 /
    //            -8.9 / -15 dB at 30 / 45 / 60 deg); <= 1.03 dB.
    //  121       Royer R-121 technical specification: figure-8, the 200 Hz /
    //            1 kHz / 10 kHz polar curves coincide — so the ideal velocity
    //            pattern cos(angle), with no shelves.
    //  421,
    //  Condenser no polar chart is transcribed for these two generic models,
    //            so they take the ideal first-order cardioid (1 + cos) / 2 —
    //            the textbook pattern (Eargle, ch. 5) — with no shelves.
    // The range stops at 60 degrees: past it the charts above run into their
    // rear nulls, which the published curves do not resolve.
    struct OffAxis {
        double loHz, hiHz;
        float  gainDb[3], loDb[3], hiDb[3];   // at 30, 45, 60 degrees
        float  pressureShare;
    };
    static constexpr OffAxis kOffAxis[kNumMics] = {
        // 0 "57"
        { 600.0,  8000.0, { -0.27f, -1.23f, -2.71f }, { 0.19f, 0.86f, 1.54f }, { -1.05f,  -2.31f,  -2.75f }, 0.5f },
        // 1 "121": 20*log10(cos 30/45/60)
        { 600.0,  8000.0, { -1.249f, -3.010f, -6.021f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, 0.0f },
        // 2 "421": 20*log10((1 + cos 30/45/60) / 2)
        { 600.0,  8000.0, { -0.624f, -1.371f, -2.499f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, 0.5f },
        // 3 Condenser: same ideal cardioid
        { 600.0,  8000.0, { -0.624f, -1.371f, -2.499f }, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f }, 0.5f },
        // 4 Tube (U 67 cardioid)
        { 300.0, 16000.0, { -0.80f, -1.78f, -2.86f }, { 0.00f, 0.09f, 0.06f }, { -6.20f, -14.23f, -24.07f }, 0.5f },
    };

    // Linear interpolation of one off-axis row in angle (0 -> 0 dB).
    static float lerpAngle(const float (&row)[3], float angle) {
        static constexpr float kAt[3] = { 30.f, 45.f, 60.f };
        if (!(angle > 0.f)) return 0.f;
        if (angle <= kAt[0]) return row[0] * (angle / kAt[0]);
        if (angle <= kAt[1]) return row[0] + (row[1] - row[0]) * ((angle - kAt[0]) / (kAt[1] - kAt[0]));
        if (angle <= kAt[2]) return row[1] + (row[2] - row[1]) * ((angle - kAt[1]) / (kAt[2] - kAt[1]));
        return row[2];
    }
    static float offAxisGain(int mic, float angle) {
        const int mi = clampIndex(mic, kNumMics);
        return static_cast<float>(dbToGain(static_cast<double>(lerpAngle(kOffAxis[mi].gainDb, angle))));
    }
    // The gradient term's share of the output at `angle`, relative to on
    // axis: cos / (A + (1 - A) cos). Exactly 1 at angle 0.
    static float gradientShare(int mic, float angle) {
        if (!(angle > 0.f)) return 1.f;
        const double a = kOffAxis[clampIndex(mic, kNumMics)].pressureShare;
        const double c = std::cos(static_cast<double>(angle) * (kPi / 180.0));
        const double den = a + (1.0 - a) * c;
        return den > 1e-6 ? static_cast<float>(c / den) : 0.f;
    }

    struct MicVoice {
        Biquad lo, bell, hi;
        Biquad angLo, angHi;   // the off-axis stage (processAngle)
        void reset() { lo.reset(); bell.reset(); hi.reset(); resetAngle(); }
        void resetAngle() { angLo.reset(); angHi.reset(); }
        float process(float x) { return hi.process(bell.process(lo.process(x))); }
        float processAngle(float x) { return angHi.process(angLo.process(x)); }
    };

    void configureMic(MicVoice& v, const Mic& m) const {
        v.lo.setLowShelf(sr_, m.loHz, 0.8, m.loDb);
        v.bell.setPeaking(sr_, m.bellHz, m.bellQ, m.bellDb);
        v.hi.setHighShelf(sr_, m.hiHz, 0.8, m.hiDb, m.hiNearNyquist);
    }

    void configureAngle(MicVoice& v, int mic, float angle) const {
        const OffAxis& o = kOffAxis[clampIndex(mic, kNumMics)];
        v.angLo.setLowShelf(sr_, o.loHz, 0.8, lerpAngle(o.loDb, angle));
        v.angHi.setHighShelf(sr_, o.hiHz, 0.8, lerpAngle(o.hiDb, angle), true);
    }

    struct Channel {
        InterpolatedDelayLine line;
        MicVoice v[2], mic2;
        Biquad   posSh, posBump, proxSh;
        float    designedAngle = -1.f;   // what v[]'s off-axis shelves are designed for
        void reset() {
            v[0].reset(); v[1].reset(); mic2.reset();
            posSh.reset(); posBump.reset(); proxSh.reset();
        }
    };

    double sr_        = 48000.0;
    int    maxBlock_  = 512;
    int    bank_      = 0;
    int    curMic_    = -1;
    int    alignSamples_ = 42;
    float  samplesPerCm_ = 1.4f;
    float  floorExtra_   = 77.f;
    float  roomTap_[kNumRoomTaps] = { 0.f, 0.f, 0.f, 0.f, 0.f };
    MicPosParams params_{};
    SmoothedValue level_, pos_, prox_, floor_, room_, blend2_, mic2Delay_, vmix_;
    SmoothedValue angle_;          // degrees; drives the off-axis shelves
    SmoothedValue angGain_[2];     // per BANK: its mic's broadband off-axis gain
    int   bankMic_[2]    = { -1, -1 };
    bool  angleLive_     = false;
    std::vector<float> levelBuf_, posBuf_, proxBuf_, floorBuf_, roomBuf_,
                       blend2Buf_, mic2DelayBuf_, vmixBuf_, angleBuf_;
    std::vector<float> angGainBuf_[2];
    std::vector<Channel> ch_;
};

} // namespace dcdsp
