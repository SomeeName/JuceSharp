// dcvoice_timeline.h — ChartTimelineVoice: plays a planned VoiceTimeline
// (dcvoice_plan.h) against a transport clock through the StringInstrument
// core. This is what the offline render tool and the native tests drive; the
// engine's chart source drives the same core through dcvoice_chartvoice.h.
//
// It owns the two curves the chart only sketches:
//   * BENDS — smoothstep between the chart's points, reaching each point AT
//     its time and taking at most 280 ms to get there (a player bends quickly
//     and holds; a straight line over a long note sounds like a whammy dive);
//     a first point on the onset is a pre-bend.
//   * SLIDES — the finger moves from 35 % to 95 % of the sustain (45 % for an
//     unpitched slide out, towards slideToFret or, without one, a fifth down /
//     a fourth up), slideInFrets glides in over the first 60 ms. The core
//     quantises the position to frets.
//
// Events start on their exact sample (the block is split at each one).
#pragma once

#include "dcvoice_instrument.h"
#include "dcvoice_sampler.h"
#include "dcvoice_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace dcvoice {

class ChartTimelineVoice final : public IChartVoice
{
public:
    static constexpr int kControlSamples = 16;

    /// The waveguide core (no sample pack).
    explicit ChartTimelineVoice(const InstrumentSpec& spec, const ExcitationBank& bank = {})
        : core_(std::make_unique<StringInstrument>(spec, bank)) {}
    /// The sample core; `pack` must outlive the voice.
    ChartTimelineVoice(const InstrumentSpec& spec, const SamplePack* pack, const SamplePack* arts = nullptr)
        : core_(std::make_unique<SampleInstrument>(spec, pack, arts)) {}

    IVoiceCore& core() noexcept { return *core_; }
    const InstrumentSpec& spec() const noexcept { return core_->spec(); }

    void prepare(double sampleRate, int maxBlockSize) override
    {
        core_->prepare(sampleRate, maxBlockSize);
        hardReset();
    }

    void silence() noexcept override
    {
        core_->allNotesOff(true);
        slots_.fill(-1);
    }

    void hardReset() noexcept
    {
        core_->reset();
        slots_.fill(-1);
        cursor_ = 0;
        wasRolling_ = false;
        lastGeneration_ = 0xFFFFFFFFu;
        lastNotes_ = nullptr;
        lastCount_ = -1;
        ctl_ = 0;
    }

    int activeStrings() const noexcept { return core_->activeNotes(); }

    void render(float* mono, int numSamples, const VoiceClock& clock, const VoiceTimeline& tl) noexcept override
    {
        if (mono == nullptr || numSamples <= 0)
            return;
        std::fill(mono, mono + numSamples, 0.0f);
        const double spp = clock.songSecondsPerSample;
        const bool rolling = clock.rolling && spp > 0.0;

        const bool changed = tl.generation != lastGeneration_ || tl.notes != lastNotes_ || tl.count != lastCount_;
        if (changed)
        {
            if (lastCount_ >= 0)
                silence();   // the old note ids mean nothing in the new table
            lastGeneration_ = tl.generation;
            lastNotes_ = tl.notes;
            lastCount_ = tl.count;
        }

        spp_ = spp;
        if (! rolling)
        {
            if (wasRolling_)
                silence();
            wasRolling_ = false;
            core_->render(mono, numSamples);
            return;
        }

        const bool jumped = wasRolling_
            && (clock.songTime < expected_ - 0.005 || clock.songTime > expected_ + 0.020);
        if (jumped)
            silence();
        if (! wasRolling_ || jumped || changed)
            cursor_ = lowerBound(tl, clock.songTime - 0.25 * spp);
        wasRolling_ = true;

        int i = 0;
        while (i < numSamples)
        {
            int end = std::min(numSamples, i + (kControlSamples - ctl_));
            const double tNow = clock.songTime + i * spp;
            while (cursor_ < tl.count)
            {
                const VoiceNote& ev = tl.notes[cursor_];
                const double off = (ev.startTime - tNow) / spp;
                if (off < 0.5)
                {
                    start(ev, cursor_, tl, tNow, spp);
                    ++cursor_;
                    continue;
                }
                const int at = i + (int) std::ceil(off - 0.5);
                if (at < end)
                    end = std::max(at, i + 1);
                break;
            }
            // Targets for the end of the whole control STEP, whatever cuts this
            // segment short — so the block size never changes the output.
            if (ctl_ == 0)
                drive(tl, tNow, tNow + kControlSamples * spp, kControlSamples);
            core_->render(mono + i, end - i);
            ctl_ = (ctl_ + (end - i)) % kControlSamples;
            i = end;
        }
        expected_ = clock.songTime + numSamples * spp;
    }

    // ---- the curves (public: the tests and the tool read them) --------------

    static double smoothstep(double x) noexcept
    {
        x = std::clamp(x, 0.0, 1.0);
        return x * x * (3.0 - 2.0 * x);
    }

    /// Bend (semitones) of `n` at song time t.
    static double bendAt(const VoiceNote& n, const VoiceTimeline& tl, double t) noexcept
    {
        if (n.bendCount <= 0 || tl.bends == nullptr || n.bendFirst < 0 || n.bendFirst >= tl.bendCount)
            return 0.0;
        const int cnt = std::min(n.bendCount, tl.bendCount - n.bendFirst);
        const BendPoint* b = tl.bends + n.bendFirst;
        const double start = n.anchorTime;
        double prevT = start;
        double prevV = (b[0].time <= start + 0.005) ? (double) b[0].semitones : 0.0;
        for (int i = 0; i < cnt; ++i)
        {
            const double pt = std::max(b[i].time, start);
            const double pv = b[i].semitones;
            if (t < pt)
            {
                const double window = std::min(pt - prevT, 0.28);
                if (window <= 1.0e-6)
                    return pv;
                return prevV + (pv - prevV) * smoothstep((t - (pt - window)) / window);
            }
            prevT = pt;
            prevV = pv;
        }
        return prevV;
    }

    /// Where a slide ends, relative to the fret (0 = no slide).
    static double slideTarget(const VoiceNote& n) noexcept
    {
        const bool unpitched = (n.flags & kFlagUnpitchedSlide) != 0;
        if (n.slideToFret < 0 && ! unpitched)
            return 0.0;
        double target = n.slideToFret;
        if (target < 0 || (unpitched && target == n.fret))
            target = n.fret >= 7 ? n.fret - 7 : n.fret + 5;
        return target - n.fret;
    }

    /// Continuous slide position (semitones from the fret) at song time t.
    static double slideAt(const VoiceNote& n, double t) noexcept
    {
        const double target = slideTarget(n);
        if (target == 0.0)
            return 0.0;
        const bool unpitched = (n.flags & kFlagUnpitchedSlide) != 0;
        double sus = n.sustainEnd - n.anchorTime;
        if (sus < 0.06)
            sus = 0.15;
        const double t0 = n.anchorTime + sus * (unpitched ? 0.45 : 0.35);
        const double t1 = n.anchorTime + sus * 0.95;
        const double x = (t - t0) / std::max(t1 - t0, 1.0e-3);
        return target * std::clamp(x, 0.0, 1.0);
    }

private:
    static int lowerBound(const VoiceTimeline& tl, double t) noexcept
    {
        int lo = 0, hi = tl.count;
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (tl.notes[mid].startTime < t)
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    }

    void start(const VoiceNote& ev, int index, const VoiceTimeline& tl, double tNow, double spp) noexcept
    {
        const int s = ev.string;
        if (s < 0 || s >= core_->spec().numStrings)
            return;
        NoteStart n;
        n.id = index;
        n.string = s;
        n.fret = ev.fret;
        n.capo = core_->spec().capo;
        n.pitch = core_->pitchOf(s, ev.fret);
        n.flags = ev.flags & ~(uint32_t) kFlagTremolo;   // the planner already expanded it
        n.velocity = ev.velocity;
        n.vibratoSemis = ev.vibratoSemis;
        n.slideToSemis = (float) slideTarget(ev);
        n.slideInSemis = (float) ev.slideInFrets;
        n.initialBend = (float) bendAt(ev, tl, tNow);
        n.initialSlide = 0.0f;
        n.speed = (float) (spp * core_->sampleRate());
        core_->noteOn(n);
        slots_[(size_t) s] = index;
    }

    /// Per control step: each string's latest note gets its bend / slide for
    /// the END of the step, and is released once its time has come.
    void drive(const VoiceTimeline& tl, double tNow, double tEnd, int len) noexcept
    {
        for (int s = 0; s < core_->spec().numStrings; ++s)
        {
            const int idx = slots_[(size_t) s];
            if (idx < 0 || idx >= tl.count)
                continue;
            const VoiceNote& n = tl.notes[idx];
            // Half a sample of tolerance, like the starts: a release on a block
            // boundary must not depend on how the block's time was summed.
            if (tNow + 0.5 * spp_ >= n.releaseTime)
            {
                core_->noteOff(idx, false);
                slots_[(size_t) s] = -1;
                continue;
            }
            core_->setControls(idx, (float) bendAt(n, tl, tEnd), (float) slideAt(n, tEnd), len);
        }
    }

    std::unique_ptr<IVoiceCore> core_;
    std::array<int, IVoiceCore::kMaxStrings> slots_ {};
    int cursor_ = 0;
    bool wasRolling_ = false;
    double expected_ = 0.0;
    uint32_t lastGeneration_ = 0xFFFFFFFFu;
    const VoiceNote* lastNotes_ = nullptr;
    int lastCount_ = -1;
    int ctl_ = 0;
    double spp_ = 0.0;
};

} // namespace dcvoice
