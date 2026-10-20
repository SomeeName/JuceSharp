#pragma once

// PLACEHOLDER voice of the chart source (ABI v38, DCA_CHART_VOICE_PLACEHOLDER).
//
// This is NOT the guitar voice. It exists so the pipe — upload, song clock,
// scheduler, seat input, rig — is testable and audible end to end before the
// real waveguide guitar (#39 step 1) lands. It is a textbook Karplus-Strong
// plucked string: a noise burst circulating in a fractional delay line one
// period long, through a two-point loss filter. It sounds like a clean,
// slightly nylon-ish electric DI. What it does with the articulation bits is
// deliberately crude:
//
//   palm mute -> short decay, darker loop     fret-hand mute -> a 30 ms thump
//   harmonic / pinch harmonic -> +12 / +19 semitones, brighter
//   hammer-on / pull-off / tap -> softer excitation     accent -> harder
//   ghost -> very soft      vibrato -> 5.5 Hz, +-0.25 semitone
//   tremolo -> re-pluck every 1/12 s (wall clock)      bends / slides -> the
//   delay length follows the scheduler's controls       slap / pop -> brighter
//
// RT contract (ChartVoice.h): prepare() allocates every delay line for the
// lowest pitch at the prepared rate; nothing below it allocates.

#include "ChartVoice.h"
#include "../../include/dotcharter_audio.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dcsynth {

class PlaceholderPluckVoice final : public ChartVoice
{
public:
    static constexpr int    kVoices        = 16;
    static constexpr double kLowestHz      = 18.0;   // below a bass's low B even bent down
    static constexpr float  kReleaseSec    = 0.06f;  // natural release fade
    static constexpr float  kChokeSec      = 0.004f; // choke fade (<= 5 ms per the interface)
    static constexpr float  kRestrikeSec   = 0.010f; // a new note on the same string

    const char* name() const noexcept override { return "placeholder-pluck"; }

    void prepare(double sampleRate, int /*maxBlockSize*/) override
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        size_t len = 1;
        const size_t need = (size_t) std::ceil(sr_ / kLowestHz) + 4;
        while (len < need) len <<= 1;
        for (auto& v : voices_)
        {
            v.line.assign(len, 0.0f);
            v.mask = (int) len - 1;
        }
        reset();
    }

    void reset() noexcept override
    {
        for (auto& v : voices_)
        {
            std::fill(v.line.begin(), v.line.end(), 0.0f);
            v.active = false;
            v.noteId = -1;
            v.write  = 0;
            v.lpPrev = 0.0f;
        }
    }

    void noteOn(const ChartNoteOn& n) noexcept override
    {
        if (voices_[0].line.empty()) return;   // not prepared
        // One pitch per string: a new note restrikes the string.
        for (auto& v : voices_)
            if (v.active && v.stringIndex == n.stringIndex && v.noteId != n.noteId && ! v.releasing)
                beginFade(v, kRestrikeSec, true);

        Voice& v = pickVoice();
        const uint32_t f = n.flags;
        v.active      = true;
        v.releasing   = false;
        v.noteId      = n.noteId;
        v.stringIndex = n.stringIndex;
        v.age         = ++ageCounter_;
        v.basePitch   = n.pitch;
        if (f & DCA_CHART_NOTE_PINCH_HARMONIC) v.basePitch += 19.0f;
        else if (f & DCA_CHART_NOTE_HARMONIC)  v.basePitch += 12.0f;
        v.bendCur  = v.bendTarget  = n.initialBendSemitones;
        v.slideCur = v.slideTarget = n.initialSlideSemitones;
        v.vibratoDepth = (f & DCA_CHART_NOTE_VIBRATO) ? 0.25f : 0.0f;
        v.vibratoPhase = 0.0f;
        v.tremoloEvery = (f & DCA_CHART_NOTE_TREMOLO) ? (int) (sr_ / 12.0) : 0;
        v.tremoloCount = v.tremoloEvery;

        // Decay (T60, seconds) and loop brightness by articulation.
        float t60    = 2.6f - 0.02f * std::clamp(v.basePitch - 40.0f, 0.0f, 50.0f);
        float bright = 0.62f;
        float amp    = 0.15f + 0.35f * std::clamp(n.velocity, 0.0f, 1.0f);
        if (f & DCA_CHART_NOTE_PALM_MUTE)      { t60 = 0.18f; bright = 0.52f; }
        if (f & DCA_CHART_NOTE_FRET_HAND_MUTE) { t60 = 0.03f; bright = 0.5f; }
        if (f & (DCA_CHART_NOTE_HARMONIC | DCA_CHART_NOTE_PINCH_HARMONIC)) { bright = 0.85f; amp *= 0.7f; }
        if (f & (DCA_CHART_NOTE_HAMMER_ON | DCA_CHART_NOTE_PULL_OFF | DCA_CHART_NOTE_TAP)) amp *= 0.55f;
        if (f & DCA_CHART_NOTE_ACCENT)         amp *= 1.25f;
        if (f & DCA_CHART_NOTE_GHOST)          amp *= 0.3f;
        if (f & (DCA_CHART_NOTE_SLAP | DCA_CHART_NOTE_POP)) bright = 0.9f;
        v.t60    = t60;
        v.bright = bright;
        v.amp    = amp;
        v.damped = (f & DCA_CHART_NOTE_END_DAMPED) != 0;

        // A CHASED note enters mid-sustain: a softer, darker burst and a 10 ms
        // fade-in instead of a pick attack.
        const bool chased = n.chaseSeconds > 0.0;
        v.env       = chased ? 0.0f : 1.0f;
        v.envTarget = 1.0f;
        v.envStep   = chased ? (float) (1.0 / (0.010 * sr_)) : 0.0f;
        if (chased) { v.amp *= 0.6f; v.bright = std::min(v.bright, 0.55f); }

        v.rng = 0x9E3779B9u ^ (uint32_t) (n.noteId * 2654435761u + 1u);
        excite(v);
    }

    void noteOff(int32_t noteId, NoteOffKind kind) noexcept override
    {
        for (auto& v : voices_)
            if (v.active && v.noteId == noteId && ! v.releasing)
                beginFade(v, kind == NoteOffKind::Choke || v.damped ? kChokeSec * 2.0f : kReleaseSec,
                          kind == NoteOffKind::Choke);
    }

    void allNotesOff(NoteOffKind kind) noexcept override
    {
        for (auto& v : voices_)
            if (v.active && ! v.releasing)
                beginFade(v, kind == NoteOffKind::Choke ? kChokeSec : kReleaseSec, kind == NoteOffKind::Choke);
    }

    void setNoteControls(int32_t noteId, const ChartNoteControls& c) noexcept override
    {
        for (auto& v : voices_)
            if (v.active && v.noteId == noteId && ! v.releasing)
            {
                v.bendTarget  = std::isfinite(c.bendSemitones)  ? c.bendSemitones  : 0.0f;
                v.slideTarget = std::isfinite(c.slideSemitones) ? c.slideSemitones : 0.0f;
            }
    }

    void render(float* out, int numSamples) noexcept override
    {
        if (numSamples <= 0 || voices_[0].line.empty()) return;
        const float inv = 1.0f / (float) numSamples;
        const float vibInc = (float) (2.0 * 3.14159265358979323846 * 5.5 / sr_);
        for (auto& v : voices_)
        {
            if (! v.active) continue;
            // Delay length at both ends of this call; linear in between. The
            // segment is at most ChartScheduler::kControlStepSamples long, so the
            // pitch path is a smooth polyline.
            const float pStart = v.basePitch + v.bendCur + v.slideCur
                               + v.vibratoDepth * std::sin(v.vibratoPhase);
            const float vibEnd = v.vibratoPhase + vibInc * (float) numSamples;
            const float pEnd   = v.basePitch + v.bendTarget + v.slideTarget
                               + v.vibratoDepth * std::sin(vibEnd);
            const float lenA = periodSamples(pStart);
            const float lenB = periodSamples(pEnd);
            const float hz   = (float) (sr_ / std::max(2.0f, lenB));
            const float g    = std::pow(10.0f, -3.0f / (std::max(0.01f, v.t60) * std::max(1.0f, hz)));
            const float s    = v.bright;
            for (int i = 0; i < numSamples; ++i)
            {
                const float len = lenA + (lenB - lenA) * ((float) i * inv);
                float rp = (float) v.write - len;
                while (rp < 0.0f) rp += (float) (v.mask + 1);
                const int   i0 = (int) rp;
                const float fr = rp - (float) i0;
                const float a  = v.line[(size_t) (i0 & v.mask)];
                const float b  = v.line[(size_t) ((i0 + 1) & v.mask)];
                const float y  = a + (b - a) * fr;
                // Two-point loss filter, blended toward the dry tap by brightness.
                const float lp = s * y + (1.0f - s) * v.lpPrev;
                v.lpPrev = y;
                float in = 0.0f;
                if (v.exciteLeft > 0) { in = nextNoise(v) * v.amp; --v.exciteLeft; }
                v.line[(size_t) (v.write & v.mask)] = in + lp * g;
                v.write = (v.write + 1) & v.mask;

                if (v.envStep != 0.0f)
                {
                    v.env += v.envStep;
                    if ((v.envStep > 0.0f && v.env >= v.envTarget) || (v.envStep < 0.0f && v.env <= v.envTarget))
                    { v.env = v.envTarget; v.envStep = 0.0f; }
                }
                out[i] += y * v.env;

                if (v.tremoloEvery > 0 && ! v.releasing && --v.tremoloCount <= 0)
                {
                    v.tremoloCount = v.tremoloEvery;
                    excite(v);
                }
            }
            v.bendCur      = v.bendTarget;
            v.slideCur     = v.slideTarget;
            v.vibratoPhase = std::fmod(vibEnd, (float) (2.0 * 3.14159265358979323846));
            if (v.releasing && v.env <= 0.0f)
            {
                v.active = false;
                v.noteId = -1;
                std::fill(v.line.begin(), v.line.end(), 0.0f);
                v.lpPrev = 0.0f;
            }
        }
    }

    int activeNotes() const noexcept override
    {
        int c = 0;
        for (const auto& v : voices_) if (v.active) ++c;
        return c;
    }

private:
    struct Voice
    {
        std::vector<float> line;
        int      mask = 0, write = 0;
        bool     active = false, releasing = false;
        int32_t  noteId = -1;
        int32_t  stringIndex = 0;
        uint32_t age = 0;
        float    basePitch = 60.0f;
        float    bendCur = 0.0f, bendTarget = 0.0f, slideCur = 0.0f, slideTarget = 0.0f;
        float    vibratoDepth = 0.0f, vibratoPhase = 0.0f;
        int      tremoloEvery = 0, tremoloCount = 0;
        float    t60 = 2.0f, bright = 0.6f, amp = 0.4f;
        bool     damped = false;   // its release is a damp by the next note: cut, do not ring
        float    lpPrev = 0.0f;
        float    env = 1.0f, envTarget = 1.0f, envStep = 0.0f;
        int      exciteLeft = 0;
        float    noiseLp = 0.0f;
        uint32_t rng = 1;
    };

    float periodSamples(float midi) const noexcept
    {
        const double hz = 440.0 * std::pow(2.0, ((double) midi - 69.0) / 12.0);
        const double len = sr_ / std::max(kLowestHz, hz) - 0.5;   // the loss filter adds ~half a sample
        return (float) std::clamp(len, 2.0, (double) (voices_[0].mask - 2));
    }

    void excite(Voice& v) noexcept
    {
        // One period of noise, low-passed by brightness: the "pick".
        v.exciteLeft = std::max(2, (int) periodSamples(v.basePitch + v.bendCur + v.slideCur));
        v.noiseLp = 0.0f;
    }

    float nextNoise(Voice& v) noexcept
    {
        v.rng ^= v.rng << 13; v.rng ^= v.rng >> 17; v.rng ^= v.rng << 5;
        const float white = (float) (v.rng & 0xFFFFFF) / (float) 0x800000 - 1.0f;
        v.noiseLp += (0.25f + 0.7f * v.bright) * (white - v.noiseLp);
        return v.noiseLp;
    }

    void beginFade(Voice& v, float seconds, bool hard) noexcept
    {
        v.releasing = true;
        v.envTarget = 0.0f;
        v.envStep   = -std::max(v.env, 1.0e-3f) / (float) std::max(1.0, seconds * sr_);
        v.exciteLeft = 0;
        if (! hard) v.t60 = std::min(v.t60, 0.08f);
    }

    Voice& pickVoice() noexcept
    {
        for (auto& v : voices_) if (! v.active) return v;
        // Steal: the oldest RELEASING voice first, then the oldest of all.
        Voice* best = nullptr;
        for (auto& v : voices_)
            if (v.releasing && (best == nullptr || v.age < best->age)) best = &v;
        if (best == nullptr)
            for (auto& v : voices_)
                if (best == nullptr || v.age < best->age) best = &v;
        std::fill(best->line.begin(), best->line.end(), 0.0f);
        best->lpPrev = 0.0f;
        return *best;
    }

    std::array<Voice, kVoices> voices_{};
    double   sr_ = 48000.0;
    uint32_t ageCounter_ = 0;
};

} // namespace dcsynth
