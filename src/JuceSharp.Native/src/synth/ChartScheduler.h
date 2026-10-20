#pragma once

// The chart source's SCHEDULER (ABI v38): maps one device block onto the song
// timeline and drives a ChartVoice with every note event that falls inside it,
// at its exact sample. AUDIO THREAD ONLY: no allocation, no locks, bounded work.
// Pure C++17 so its tests (tests/synth/chart_scheduler_test.cpp) need no engine.
//
// THE CLOCK. Once per block the engine samples the bound song source AFTER it
// was pulled (SongClockSample): its audible position at the END of the block,
// whether it is playing, its seek generation and its tempo. This scheduler turns
// consecutive samples into the half-open window [blockStart, blockEnd) of song
// time the block covers:
//
//   * continuous play: blockStart = the previous block's end, blockEnd = the new
//     position. Consecutive windows TILE the timeline, so no note is ever
//     skipped or played twice. A stretcher's read-ahead can wobble the position
//     a few samples backwards; the window then has zero width and the block
//     carries no events (the next one catches up) instead of re-firing notes.
//   * a DISCONTINUITY — the source's seek generation moved (a seek, a loop
//     wrap, which is a seek), the clock resumed after a pause, the table,
//     voice or offset changed, or the position jumped by more than a block plus 250 ms
//     with nobody announcing it — chokes what is sounding and re-seats the
//     window as [pos - block * tempo, pos). Notes that started before the
//     window and are still held there are CHASED: started at sample 0 with
//     `chaseSeconds` saying how far into the note they are.
//   * not playing / no clock: every note is released and nothing starts.
//
// Inside the window a song time t lands on sample floor((t - blockStart) /
// (blockEnd - blockStart) * n). The voice renders at the device rate whatever
// the tempo, so practice speed changes when notes sound and never their pitch.
// Every note time is shifted by `offsetSeconds` (positive = later).

#include "ChartNoteTable.h"
#include "ChartVoice.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace dcsynth {

/// One reading of the song clock, taken on the audio thread after the pull.
struct SongClockSample
{
    bool     valid          = false; // a source is bound and was read this block
    bool     playing        = false;
    double   position       = 0.0;   // audible song seconds at the END of the block
    uint32_t seekGeneration = 0;     // bumped by every seek / stop of the source
    double   tempo          = 1.0;   // song seconds per wall second
};

/// Counters the control thread may read at any time (relaxed atomics).
struct ChartSchedulerStats
{
    std::atomic<int64_t> notesStarted { 0 };
    std::atomic<int64_t> notesChased  { 0 };
    std::atomic<int64_t> notesDropped { 0 };
    std::atomic<int32_t> activeVoices { 0 };
    std::atomic<int32_t> clockPlaying { 0 };
    std::atomic<double>  songSeconds  { 0.0 };
};

class ChartScheduler
{
public:
    static constexpr int kMaxActive          = 64;  // notes this scheduler tracks at once
    static constexpr int kMaxNoteOnsPerBlock = 64;  // a denser block drops (and counts) the rest
    static constexpr int kMaxChaseScan       = 512; // notes looked at behind a seek point
    static constexpr int kControlStepSamples = 32;  // bend/slide resolution
    /// An unannounced jump bigger than one block plus this is a discontinuity.
    static constexpr double kJumpToleranceSeconds = 0.25;

    ChartSchedulerStats stats;

    /// Forget everything: the next block is a discontinuity. Audio thread, or a
    /// control thread with no callback in flight (device start).
    void reset() noexcept
    {
        activeCount_ = 0;
        needResync_  = true;
        tableSerial_ = 0;
        voiceSerial_ = 0;
        stats.activeVoices.store(0, std::memory_order_relaxed);
    }

    /// Render one block. `table` and `voice` may be null (then the block is
    /// silent and nothing is tracked). `tableSerial` / `voiceSerial` identify
    /// them — a change is a discontinuity (never compare POINTERS: a freed table
    /// and its replacement can share an address). ADDS into `mono`.
    void process(const ChartNoteTable* table, uint64_t tableSerial,
                 ChartVoice* voice, uint64_t voiceSerial,
                 const SongClockSample& clk, double offsetSeconds, double sampleRate,
                 float* mono, int n) noexcept
    {
        if (n <= 0 || mono == nullptr) return;
        if (voice == nullptr)
        {
            activeCount_ = 0;
            needResync_  = true;
            stats.activeVoices.store(0, std::memory_order_relaxed);
            return;
        }
        if (voiceSerial != voiceSerial_)
        {
            // A NEW voice: whatever the old one was sounding belongs to it.
            activeCount_ = 0;
            voiceSerial_ = voiceSerial;
            needResync_  = true;
        }
        if (tableSerial != tableSerial_)
        {
            chokeAll(*voice);
            tableSerial_ = tableSerial;
            needResync_  = true;
        }

        const bool runnable = table != nullptr && table->count() > 0 && clk.valid && clk.playing
                              && std::isfinite(clk.position) && sampleRate > 0.0;
        stats.clockPlaying.store(clk.valid && clk.playing ? 1 : 0, std::memory_order_relaxed);
        if (! runnable)
        {
            releaseAll(*voice);
            needResync_ = true;
            if (clk.valid && std::isfinite(clk.position))
            {
                lastEnd_ = clk.position;
                stats.songSeconds.store(clk.position, std::memory_order_relaxed);
            }
            voice->render(mono, n);
            stats.activeVoices.store(voice->activeNotes(), std::memory_order_relaxed);
            return;
        }

        const double tempo    = clk.tempo > 0.0 && std::isfinite(clk.tempo) ? clk.tempo : 1.0;
        const double expected = (double) n * tempo / sampleRate;
        // A moved offset re-maps every note under the playhead: treat it like a seek.
        bool disc = needResync_ || ! haveLastEnd_ || clk.seekGeneration != lastGen_
                    || offsetSeconds != lastOffset_;
        if (! disc)
        {
            const double jump = clk.position - lastEnd_;
            if (jump < -kJumpToleranceSeconds || jump > expected + kJumpToleranceSeconds) disc = true;
        }

        double blockStart, blockEnd;
        if (disc)
        {
            chokeAll(*voice);
            blockEnd   = clk.position;
            blockStart = blockEnd - expected;
        }
        else
        {
            blockStart = lastEnd_;
            blockEnd   = std::max(clk.position, lastEnd_);
        }
        lastEnd_     = blockEnd;
        haveLastEnd_ = true;
        lastGen_     = clk.seekGeneration;
        lastOffset_  = offsetSeconds;
        needResync_  = false;
        stats.songSeconds.store(blockEnd, std::memory_order_relaxed);

        // The window in CHART time (note times are chart times).
        w0_ = blockStart - offsetSeconds;
        w1_ = blockEnd   - offsetSeconds;
        n_  = n;
        speed_ = (float) tempo;

        if (disc)
        {
            cursor_ = table->lowerBound(w0_);
            chase(*table, *voice);
        }

        int ons = 0;
        int pos = 0;
        for (;;)
        {
            fireOffs(*table, *voice, pos);
            fireOns(*table, *voice, pos, ons);
            fireOffs(*table, *voice, pos);   // a note whose release falls on its own attack sample
            if (pos >= n) break;

            int next = std::min(n, pos + kControlStepSamples);
            const int ev = nextEventOffset(*table, pos);
            if (ev > pos && ev < next) next = ev;

            // Controls aim at the END of the segment; the voice glides there.
            const double tEnd = timeAt(next);
            for (int a = 0; a < activeCount_; ++a)
            {
                const int32_t idx = active_[(size_t) a];
                ChartNoteControls c;
                c.bendSemitones  = table->bendAt(idx, tEnd);
                c.slideSemitones = table->slideAt(idx, tEnd);
                voice->setNoteControls(idx, c);
            }
            voice->render(mono + pos, next - pos);
            pos = next;
        }
        stats.activeVoices.store(voice->activeNotes(), std::memory_order_relaxed);
    }

    /// Notes this scheduler believes are held (test hook).
    int trackedNotes() const noexcept { return activeCount_; }

private:
    double timeAt(int sample) const noexcept
    {
        return w0_ + (w1_ - w0_) * ((double) sample / (double) n_ + 1.0e-6);
    }

    /// Sample offset of chart time t inside the current window, clamped.
    int offsetOf(double t) const noexcept
    {
        const double width = w1_ - w0_;
        if (width <= 0.0) return 0;
        // The epsilon keeps a time that lies EXACTLY on a sample boundary on
        // that sample despite the division's rounding (1e-6 of a sample).
        const double s = std::floor((t - w0_) / width * (double) n_ + 1.0e-6);
        if (s <= 0.0) return 0;
        if (s >= (double) (n_ - 1)) return n_ - 1;
        return (int) s;
    }

    bool inWindow(double t) const noexcept { return t >= w0_ && t < w1_; }

    int nextEventOffset(const ChartNoteTable& table, int pos) const noexcept
    {
        int best = n_;
        if (cursor_ < table.count())
        {
            const double s = table.notes[(size_t) cursor_].start_seconds;
            if (inWindow(s)) { const int o = offsetOf(s); if (o > pos) best = std::min(best, o); }
        }
        for (int a = 0; a < activeCount_; ++a)
        {
            const double r = table.notes[(size_t) active_[(size_t) a]].release_seconds;
            if (r < w1_) { const int o = offsetOf(r); if (o > pos) best = std::min(best, o); }
        }
        return best;
    }

    void fireOffs(const ChartNoteTable& table, ChartVoice& voice, int pos) noexcept
    {
        for (int a = 0; a < activeCount_;)
        {
            const int32_t idx = active_[(size_t) a];
            const double r = table.notes[(size_t) idx].release_seconds;
            if (r < w1_ && offsetOf(r) <= pos)
            {
                voice.noteOff(idx, NoteOffKind::Release);
                active_[(size_t) a] = active_[(size_t) (--activeCount_)];
            }
            else
            {
                ++a;
            }
        }
    }

    void fireOns(const ChartNoteTable& table, ChartVoice& voice, int pos, int& ons) noexcept
    {
        while (cursor_ < table.count())
        {
            const double s = table.notes[(size_t) cursor_].start_seconds;
            if (s >= w1_ || offsetOf(s) > pos) return;
            const int32_t idx = cursor_++;
            if (s < w0_) continue;   // behind the window (only after a re-seat): never fire late
            if (ons >= kMaxNoteOnsPerBlock)
            {
                stats.notesDropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            ++ons;
            start(table, voice, idx, 0.0, s);
        }
    }

    /// After a re-seat: start every note that began before the window and is
    /// still held at its start. Bounded by kMaxChaseScan and the table's
    /// longest note, so a huge chart costs the same as a small one.
    void chase(const ChartNoteTable& table, ChartVoice& voice) noexcept
    {
        int scanned = 0;
        for (int32_t j = cursor_ - 1; j >= 0 && scanned < kMaxChaseScan; --j, ++scanned)
        {
            const auto& nt = table.notes[(size_t) j];
            if (nt.start_seconds < w0_ - table.maxDurationSeconds) break;
            if (nt.release_seconds > w0_)
            {
                start(table, voice, j, w0_ - nt.start_seconds, w0_);
                stats.notesChased.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    void start(const ChartNoteTable& table, ChartVoice& voice, int32_t idx,
               double chaseSeconds, double atTime) noexcept
    {
        if (activeCount_ >= kMaxActive)
        {
            // Pool full: steal the oldest tracked note rather than refuse the new one.
            voice.noteOff(active_[0], NoteOffKind::Choke);
            for (int a = 1; a < activeCount_; ++a) active_[(size_t) (a - 1)] = active_[(size_t) a];
            --activeCount_;
            stats.notesDropped.fetch_add(1, std::memory_order_relaxed);
        }
        const auto& nt = table.notes[(size_t) idx];
        ChartNoteOn on;
        on.noteId       = idx;
        on.stringIndex  = nt.string_index;
        on.fret         = nt.fret;
        on.capo         = nt.capo;
        on.pitch        = nt.pitch;
        on.velocity     = nt.velocity;
        on.flags        = nt.flags;
        on.chordId      = nt.chord_id;
        on.chordSize    = table.chordSize[(size_t) idx];
        on.chordIndex   = table.chordIndex[(size_t) idx];
        on.strum        = nt.strum;
        on.slideToPitch = nt.slide_to_fret >= 0.0f
                              ? nt.pitch + (nt.slide_to_fret - (float) nt.fret) : nt.pitch;
        on.initialBendSemitones  = table.bendAt(idx, atTime);
        on.initialSlideSemitones = table.slideAt(idx, atTime);
        on.speed           = speed_;
        on.durationSeconds = nt.release_seconds - nt.start_seconds;
        on.sustainSeconds  = nt.sustain_seconds;
        on.chaseSeconds    = chaseSeconds;
        voice.noteOn(on);
        active_[(size_t) (activeCount_++)] = idx;
        stats.notesStarted.fetch_add(1, std::memory_order_relaxed);
    }

    void chokeAll(ChartVoice& voice) noexcept
    {
        if (activeCount_ > 0) voice.allNotesOff(NoteOffKind::Choke);
        activeCount_ = 0;
    }

    void releaseAll(ChartVoice& voice) noexcept
    {
        if (activeCount_ > 0) voice.allNotesOff(NoteOffKind::Release);
        activeCount_ = 0;
    }

    std::array<int32_t, kMaxActive> active_{};
    int      activeCount_ = 0;
    int32_t  cursor_      = 0;
    double   lastEnd_     = 0.0;
    bool     haveLastEnd_ = false;
    bool     needResync_  = true;
    uint32_t lastGen_     = 0;
    double   lastOffset_  = 0.0;
    uint64_t tableSerial_ = 0;
    uint64_t voiceSerial_ = 0;
    double   w0_ = 0.0, w1_ = 0.0;
    int      n_  = 1;
    float    speed_ = 1.0f;
};

} // namespace dcsynth
