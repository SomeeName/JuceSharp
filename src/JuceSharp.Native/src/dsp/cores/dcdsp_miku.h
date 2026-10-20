// dcdsp_miku.h — MIKU STOMP vocal-synth pedal (0.16 §1.19, v1 core).
//
// A vocal synthesizer that SINGS phrase patterns pitch-tracked to the guitar
// input, NSX-1 style: three composable stages —
//
//   1. mono pitch tracker + onset detector on the raw input (ch0): decimated
//      (~12 kHz) normalized autocorrelation once per block, median-of-3 +
//      confidence gating for octave stability; onsets from a fast/slow
//      envelope ratio with an 80 ms refractory.
//   2. lyric sequencer: one syllable per onset, cycling through either a
//      built-in preset pattern (8: Looh/Lahh/Ahh/Pahh/Nyan/Scat/Random 1/2)
//      or a CUSTOM phrase table fed through the v22 string param ("phrases",
//      parsed by parsePhrases below) — string-param-fed from day one per the
//      owner decision "FULL custom lyrics in 0.16".
//   3. source-filter voice: sawtooth glottal source at the tracked pitch
//      (octave-shifted, late-onset vibrato), 3 formant band-pass resonators
//      (per-vowel Klatt-style freq/amp/bandwidth tables, `character` shifts
//      the formant frequencies for chipmunk..giant voice), consonant noise
//      heads (plosive burst / fricative hiss), amplitude following the
//      player's own envelope, soft-limited.
//
// v1 is deliberately ROBOTIC-Miku-ish: stable, bounded, RT-safe and fun;
// fidelity iterates later. The dry path stays zero-latency (the voice rides
// on top via `mix`).
//
// PURE header-only dcdsp core, C++17, standard library only. RT contract:
// all allocation in prepare(); process() never allocates, locks, or throws;
// output is always finite and |y| <= 8. setPhrase/setParams are message-
// thread; the phrase table is published behind a SEQLOCK (the ParamMailbox
// pattern from DspModule.h) with its syllables stored as lock-free atomics, so
// the audio thread can never read a half-written table and never spins or
// frees. It used to be a two-buffer flip behind an index, which is ABA-unsafe:
// two setPhrase calls in a row return to the buffer a reader is still holding
// and rewrite it under them — and here the reader holds that pointer for a
// WHOLE BLOCK.
#ifndef DCDSP_MIKU_H
#define DCDSP_MIKU_H

#ifdef DCDSP_TEST_STUBS
  #include "test_stubs.h"
#else
  #include "dcdsp_primitives.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dcdsp {

struct MikuParams {
    // Voice/dry blend. 0 = pedal off (dry), 1 = voice only. [0,1, default 0.6]
    float mix = 0.6f;
    // Voice character: shifts the formant frequencies 0.75x..1.5x
    // (0 = dark/giant, 0.5 = neutral, 1 = bright/chipmunk). [0,1, default 0.5]
    float character = 0.5f;
    // Octave shift of the sung pitch vs. the tracked guitar pitch, in whole
    // octaves (rounded). +1 lands guitar range in a Miku-ish register.
    // [-1,2, default 1]
    float octave = 1.0f;
    // Preset phrase pattern 0..7 (rounded): Looh, Lahh, Ahh, Pahh, Nyan,
    // Scat, Random 1, Random 2. Ignored while a custom phrase table is set.
    float pattern = 0.0f;
};

class MikuCore {
public:
    // Consonant classes for a syllable head.
    enum Cons { ConsNone = 0, ConsPlosive = 1, ConsFricative = 2, ConsNasal = 3, ConsLiquid = 4 };

    struct Syllable {
        int cons  = ConsNone; // Cons
        int vowel = 0;        // 0..4 = A E I O U
    };

    static constexpr int kMaxSyllables = 64;
    static constexpr int kNumCons      = 5;   // ConsNone..ConsLiquid
    static constexpr int kNumVowels    = 5;   // 0..4 = A E I O U

    // The custom phrase table is shared between the message thread (setPhrase)
    // and the audio thread, so its syllables live as packed 32-bit atomics: one
    // lock-free load per onset instead of a two-int struct copy that can tear.
    // Both halves are RANGE-CLAMPED here — `cons` picks a burst type and
    // `vowel` scales a smoother target, so an out-of-range value read out of a
    // racing publish would leave the voice somewhere it can never be set to.
    static constexpr uint32_t packSyllable(const Syllable& s) noexcept {
        const uint32_t c = (uint32_t) ((s.cons  >= 0 && s.cons  < kNumCons)   ? s.cons  : (int) ConsNone);
        const uint32_t v = (uint32_t) ((s.vowel >= 0 && s.vowel < kNumVowels) ? s.vowel : 0);
        return (c << 8) | v;
    }
    static constexpr Syllable unpackSyllable(uint32_t packed) noexcept {
        const int c = (int) ((packed >> 8) & 0xFFu);
        const int v = (int) (packed & 0xFFu);
        return Syllable{ (c < kNumCons) ? c : (int) ConsNone, (v < kNumVowels) ? v : 0 };
    }
    // Bounded, because the audio thread may never spin. The give-up path costs
    // one repeated syllable, not a glitch.
    static constexpr int kPhraseReadTries = 3;

    // ---- Phrase-string parser (message thread; also used by the adapter) --
    // Splits on whitespace / comma / hyphen / '|' into syllables; per token
    // the first consonant letter picks the noise head, the first vowel letter
    // picks the vowel (defaults: no consonant head, vowel U for vowel-less
    // tokens like "n"). ASCII-insensitive; non-ASCII bytes are skipped so a
    // UTF-8 string can never mis-index. Returns the syllable count (<= cap).
    static int parsePhrases(const char* utf8, Syllable* out, int cap)
    {
        if (utf8 == nullptr || out == nullptr || cap < 1) return 0;
        int count = 0;
        const char* p = utf8;
        while (*p != '\0' && count < cap)
        {
            while (*p != '\0' && isSep(*p)) ++p;
            if (*p == '\0') break;
            int cons = ConsNone, vowel = -1;
            bool sawCons = false;
            for (; *p != '\0' && !isSep(*p); ++p)
            {
                const unsigned char u = (unsigned char) *p;
                if (u >= 0x80) continue; // non-ASCII: skip byte, stay bounded
                const char c = (char) std::tolower(u);
                const int v = vowelIndex(c);
                if (v >= 0) { if (vowel < 0) vowel = v; continue; }
                if (!sawCons && vowel < 0)
                {
                    const int k = consClass(c);
                    if (k != ConsNone) { cons = k; sawCons = true; }
                }
            }
            out[count].cons  = cons;
            out[count].vowel = (vowel >= 0) ? vowel : 4; // vowel-less -> closed U
            ++count;
        }
        return count;
    }

    // ---- Lifecycle ---------------------------------------------------------

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        (void) maxBlockSize;
        nch_ = (numChannels > 0) ? numChannels : 1;

        // Decimated analysis stream (~12 kHz) for the autocorrelation.
        dec_ = std::max(1, (int) std::lround(sr_ / 12000.0));
        srDec_ = sr_ / (double) dec_;
        maxLag_ = (int) (srDec_ / 65.0);            // lowest trackable ~65 Hz (covers drop C#)
        minLag_ = std::max(2, (int) (srDec_ / 500.0)); // highest ~500 Hz
        acWin_  = (int) (srDec_ * 0.030);           // 30 ms correlation window
        ringSize_ = 1;
        while (ringSize_ < acWin_ + maxLag_ + 8) ringSize_ <<= 1;
        ring_.assign((size_t) ringSize_, 0.0f);
        ringMask_ = ringSize_ - 1;
        ringPos_ = 0;
        decPhase_ = 0;
        decLp_.setLowpass(sr_, 4500.0);
        rBuf_.assign((size_t) maxLag_ + 2, 0.0f);

        fastEnv_.prepare(sr_); fastEnv_.setTimes(1.0f, 60.0f);
        slowEnv_.prepare(sr_); slowEnv_.setTimes(12.0f, 160.0f);
        // Onset reference: the fast envelope DELAYED ~20 ms — a pick attack
        // is a jump over where the envelope was a moment ago, which stays
        // robust for replucks over a still-ringing string (a fresh slow
        // envelope rises too fast to serve as the reference).
        feDelayLen_ = (int) (sr_ * 0.020) + 4;
        feDelay_.assign((size_t) feDelayLen_, 0.0f);
        feDelayPos_ = 0;
        refractory_ = (int) (sr_ * 0.080);
        cooldown_ = 0;
        onsetAge_ = 1 << 30;

        f0Smooth_.reset(sr_, 0.020f);
        f0Smooth_.setCurrentAndTarget(0.0f);
        ampSmooth_.reset(sr_, 0.015f);
        ampSmooth_.setCurrentAndTarget(0.0f);
        charSmooth_.reset(sr_, kMixRampSeconds);
        charSmooth_.setTarget(0.75f * std::pow(2.0f, clamp01(params_.character)));
        charSmooth_.snapToTarget();   // a freshly prepared voice starts AT it
        vowelSmooth_.reset(sr_, 0.025f);
        vowelSmooth_.setCurrentAndTarget(0.0f);
        // `mix` RAMPS (kMixRampSeconds); as a per-block constant it hard-spliced
        // a full dry/synth-voice swap at every block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();   // a freshly prepared voice starts AT its mix
        vibLfo_.prepare(sr_);
        vibLfo_.setShape(Lfo::Sine);
        vibLfo_.setFrequency(5.5f);
        srcLp_.setLowpass(sr_, 5000.0);
        for (auto& f : svf_) f.reset();
        burstBp_.setBandpass(sr_, 1800.0, 1.2);
        burstHp_.setHighpass(sr_, 3200.0, 0.707);
        voiceDc_.prepare(sr_);

        // None of the set*/prepare calls above CLEARS the filter memory it
        // retunes (see dcdsp_primitives.h — every one of them writes
        // coefficients only), so a sample-rate change left this core running
        // the previous rate's state through the new rate's coefficients. The
        // svf_ bank was already reset here; these were the ones the list
        // missed.
        decLp_.reset();
        fastEnv_.reset();
        slowEnv_.reset();
        vibLfo_.reset();
        srcLp_.reset();
        burstBp_.reset();
        burstHp_.reset();
        voiceDc_.reset();

        f0Hist_[0] = f0Hist_[1] = f0Hist_[2] = 0.0f;
        f0Raw_ = 0.0f;
        phase_ = 0.0f;
        adaa_ = 0.0f;
        burstLeft_ = 0;
        burstType_ = ConsNone;
        rng_ = 0x9E3779B9u;
        lastCoeffV_ = -1.0f;
        coeffCountdown_ = 0;
        // -1, not 0: advanceSyllable() takes idx = (prev + 1) % n on both the
        // custom-phrase and preset paths, so 0 makes the FIRST note sing
        // table[1] and every multi-syllable phrase open on its SECOND syllable.
        // MikuStompCore carries the same -1 and the same reason.
        sylIndex_.store(-1, std::memory_order_relaxed);
        trackedHz_.store(0.0f, std::memory_order_relaxed);
        prepared_ = true;
        applyParams();
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        std::fill(feDelay_.begin(), feDelay_.end(), 0.0f);
        feDelayPos_ = 0;
        ringPos_ = 0; decPhase_ = 0;
        fastEnv_.reset(); slowEnv_.reset();
        decLp_.reset(); srcLp_.reset();
        for (auto& f : svf_) f.reset();
        burstBp_.reset(); burstHp_.reset(); voiceDc_.reset();
        f0Smooth_.setCurrentAndTarget(0.0f);
        ampSmooth_.setCurrentAndTarget(0.0f);
        vibLfo_.reset();
        phase_ = 0.0f; adaa_ = 0.0f;
        burstLeft_ = 0; cooldown_ = 0; onsetAge_ = 1 << 30;
        f0Hist_[0] = f0Hist_[1] = f0Hist_[2] = 0.0f;
        f0Raw_ = 0.0f;
        lastCoeffV_ = -1.0f; coeffCountdown_ = 0;
        // ...and the seven members prepare() clears that reset() used to miss.
        // reset()'s one caller is the un-bypass of a PARKED node, so without
        // these the voice came back holding the previous vowel target, the
        // previous consonant burst type and the previous syllable position.
        // MikuStompCore::reset() already clears its equivalents, sylIndex_
        // included ("a reset restarts the phrase").
        vowelSmooth_.setCurrentAndTarget(0.0f);
        burstType_ = ConsNone;
        burstNorm_ = 0.0f;
        rng_ = 0x9E3779B9u;
        sylIndex_.store(-1, std::memory_order_relaxed);   // a reset restarts the phrase
        curSyl_.store(packSyllable(Syllable{}), std::memory_order_relaxed);
        trackedHz_.store(0.0f, std::memory_order_relaxed);
    }

    void setParams(const MikuParams& p) {
        params_.mix       = clamp01(p.mix);
        mixSmooth_.setTarget(params_.mix);
        params_.character = clamp01(p.character);
        // Smooth charMul ITSELF, not `character`: the loop below would otherwise
        // need a std::pow PER SAMPLE, which measurably slowed the core (the
        // phrase-seqlock scenario's own throughput guard caught it: 6,697 reads
        // against the ~30,000 it needs to have raced anything). A linear ramp of
        // the multiplier instead of the exponent is indistinguishable over 10 ms.
        // clamp01 is NaN-TRANSPARENT, so guard the smoother: a NaN target poisons
        // mCurrent and every later setTarget computes its increment from it, i.e.
        // the voice would be silent for good -- which is the exact failure
        // RtNaNCannotLatchACoreCoefficientOrGain exists to catch.
        const float wantCharMul = 0.75f * std::pow(2.0f, params_.character);
        if (std::isfinite(wantCharMul)) {
            charSmooth_.setTarget(wantCharMul);
            if (!std::isfinite(charSmooth_.current())) charSmooth_.snapToTarget();
        }
        params_.octave    = clampf(p.octave, -1.0f, 2.0f);
        params_.pattern   = clampf(p.pattern, 0.0f, 7.0f);
        if (prepared_) applyParams();
    }

    // Custom phrase table (message thread). count 0 (or null) reverts to the
    // preset patterns.
    //
    // SEQLOCK, not a buffer flip — see the header comment. An odd `phraseSeq_`
    // marks a write in flight; the audio thread takes (count, syllable) between
    // two EQUAL EVEN reads of it and gives up after a bounded number of tries
    // rather than waiting.
    void setPhrase(const Syllable* syl, int count) {
        int n = 0;
        if (syl != nullptr)
        {
            n = std::min(count, kMaxSyllables);
            if (n < 0) n = 0;
        }
        phraseSeq_.fetch_add(1, std::memory_order_acq_rel);       // odd: in flight
        for (int i = 0; i < n; ++i)
            custom_[(size_t) i].store(packSyllable(syl[i]), std::memory_order_relaxed);
        customCount_.store(n, std::memory_order_relaxed);
        phraseSeq_.fetch_add(1, std::memory_order_release);       // even: stable
    }

    /// Outcome of a custom-phrase read on the audio thread. Ok = a coherent
    /// (count, syllable) pair; Empty = no custom phrase, so the caller falls
    /// back to the preset table; Busy = a setPhrase was in flight for longer
    /// than the bounded retry, so the caller must NOT advance.
    enum class PhraseRead { Ok, Empty, Busy };

    /// Reads the syllable after `prevIndex` out of the custom phrase table.
    /// Audio thread — and the RT gate's coherence probe, which drives it at a
    /// rate no callback reaches. Never allocates, locks or spins.
    PhraseRead nextCustomSyllable(int prevIndex, int& idxOut, Syllable& out) const noexcept {
        for (int attempt = 0; attempt < kPhraseReadTries; ++attempt)
        {
            const uint32_t s0 = phraseSeq_.load(std::memory_order_acquire);
            if ((s0 & 1u) != 0u) continue;                        // write in flight
            const int n = customCount_.load(std::memory_order_relaxed);
            if (n <= 0)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
                if (phraseSeq_.load(std::memory_order_relaxed) != s0) continue;
                return PhraseRead::Empty;
            }
            const int idx = (prevIndex + 1) % n;
            const uint32_t packed = custom_[(size_t) idx].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (phraseSeq_.load(std::memory_order_relaxed) != s0) continue;   // torn
            idxOut = idx;
            out    = unpackSyllable(packed);
            return PhraseRead::Ok;
        }
        return PhraseRead::Busy;
    }

    /// The syllable the sequencer last committed to sing. Poll-only
    /// introspection, and the only way a test can see a TORN phrase read.
    Syllable currentSyllable() const {
        return unpackSyllable(curSyl_.load(std::memory_order_relaxed));
    }

    // ---- Introspection (poll-only; message thread or tests) ----------------
    float trackedHz() const { return trackedHz_.load(std::memory_order_relaxed); }
    int   syllableIndex() const { return sylIndex_.load(std::memory_order_relaxed); }
    /// TEST SURFACE. The vowel smoother's CURRENT position (0 = A .. 1 = U).
    /// A nasal head snaps this to the closed U and it must then walk to the
    /// syllable's own vowel; it used to be pinned at U for good.
    float vowelPosition() const { return vowelSmooth_.current(); }

    // ---- Audio thread -------------------------------------------------------

    void process(float* const* channels, int numChannels, int numFrames) {
        if (!prepared_ || channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        float* x0 = channels[0];
        if (x0 == nullptr) return;
        const int nc = std::min(numChannels, nch_);

        // The PRESET tables are immutable statics and safe to hoist; the custom
        // one is not, and is resolved at the onset instead (see advanceSyllable).
        int presetLen = 0;
        const Syllable* preset = presetTable((int) std::lround(params_.pattern), presetLen);

        const float octMul   = std::pow(2.0f, (float) std::lround(params_.octave));
        // charMul is walked PER FRAME below: it multiplies every formant centre
        // and the resonator gains follow it, so a per-block constant stepped the
        // timbre and the level together.

        for (int i = 0; i < numFrames; ++i)
        {
            const float charMul = charSmooth_.next();   // 0.75 .. 1.5
            float in = x0[i];
            if (!std::isfinite(in)) in = 0.0f;

            // ---- analysis feeds (full rate) -----------------------------
            const float fe = fastEnv_.process(in);
            const float se = slowEnv_.process(in);

            // decimate into the autocorrelation ring (anti-alias LP first)
            const float lp = decLp_.process(in);
            if (++decPhase_ >= dec_)
            {
                decPhase_ = 0;
                ring_[(size_t) (ringPos_ & ringMask_)] = lp;
                ++ringPos_;
            }

            // ---- onset -> advance the lyric sequencer --------------------
            const float feDel = feDelay_[(size_t) feDelayPos_];
            feDelay_[(size_t) feDelayPos_] = fe;
            if (++feDelayPos_ >= feDelayLen_) feDelayPos_ = 0;
            if (cooldown_ > 0) --cooldown_;
            // SATURATE. A free-running int starting at 1<<30 overflows after
            // ~6.2 h of continuous processing; mikustomp saturates its own
            // sinceOnset_ the same way and MikuCore never did.
            if (onsetAge_ < (1 << 30)) ++onsetAge_;
            if (cooldown_ == 0 && fe > 2.0f * kGateLin && fe > 2.0f * feDel)
            {
                cooldown_ = refractory_;
                onsetAge_ = 0;
                advanceSyllable(preset, presetLen);
            }

            // ---- voice synthesis ----------------------------------------
            // pitch: smoothed toward the tracked f0 (block-rate update below)
            const float f0 = f0Smooth_.next();
            float voice = 0.0f;
            if (f0 > 20.0f)
            {
                // late-onset vibrato (period-authentic: swells in after ~180 ms)
                const float vib = vibLfo_.process();
                const float vibAmt = (onsetAge_ > (int) (0.18 * sr_)) ? 0.012f : 0.0f;
                float hz = f0 * octMul * (1.0f + vibAmt * vib);
                hz = clampf(hz, 40.0f, 1400.0f);

                phase_ += (float) (hz / sr_);
                if (phase_ >= 1.0f) phase_ -= 1.0f;
                float src = 2.0f * phase_ - 1.0f;      // glottal-ish saw
                src = srcLp_.process(src);

                // vowel morph position (0..1 across A E I O U)
                const float vpos = clamp01(vowelSmooth_.next());
                if (--coeffCountdown_ <= 0)
                {
                    coeffCountdown_ = kCoeffInterval;
                    // The charMul half is load-bearing: the coefficients are
                    // CACHED, so gating on the vowel alone would freeze a
                    // character ramp at whatever value it held when setParams
                    // forced its one update -- the knob would stop working,
                    // which is worse than the zipper it replaces.
                    if (std::fabs(vpos - lastCoeffV_) > 1e-4f
                     || std::fabs(charMul - lastCoeffChar_) > 1e-4f)
                    {
                        lastCoeffV_    = vpos;
                        lastCoeffChar_ = charMul;
                        updateFormants(vpos, charMul);
                    }
                }

                svf_[0].process(src);
                svf_[1].process(src);
                svf_[2].process(src);
                voice = g_[0] * svf_[0].bp() + g_[1] * svf_[1].bp() + g_[2] * svf_[2].bp();
            }

            // consonant noise head
            if (burstLeft_ > 0)
            {
                --burstLeft_;
                const float nz = nextNoise();
                const float envN = (float) burstLeft_ * burstNorm_;
                if (burstType_ == ConsPlosive)        voice += 1.4f * envN * burstBp_.process(nz);
                else if (burstType_ == ConsFricative) voice += 0.5f * envN * burstHp_.process(nz);
            }

            // The voice follows the player's own dynamics. Gate on the FAST
            // envelope so the voice shuts up promptly when the strings do.
            ampSmooth_.setTarget(fe > kGateLin ? std::min(1.0f, se * 4.0f) : 0.0f);
            const float amp = ampSmooth_.next();
            voice = adaaTanh(kMakeup * amp * voice, adaa_);
            voice = voiceDc_.process(voice);
            // RECOVER the voice, do not merely scrub its output. svf_[] (TPT),
            // voiceDc_ and adaa_ all feed their own state back, so a NaN that
            // reaches them is PERMANENT and the scrub below then prints silence
            // for ever with nothing saying why. MikuStompCore does exactly this
            // at its own output stage; MikuCore only had the scrub.
            if (!std::isfinite(voice)) { voice = 0.0f; hardReset(); }

            // ---- mix (dry path untouched & zero-latency) -----------------
            const float mix    = mixSmooth_.next();
            const float dryAmt = 1.0f - mix;
            for (int c = 0; c < nc; ++c)
            {
                float* x = channels[c];
                if (x == nullptr) continue;
                float dry = x[i];
                if (!std::isfinite(dry)) dry = 0.0f;
                float y = dryAmt * dry + mix * voice;
                if (!std::isfinite(y)) y = 0.0f;
                else if (y >  8.0f) y =  8.0f;
                else if (y < -8.0f) y = -8.0f;
                x[i] = y;
            }
        }

        // ---- block-rate pitch track (decimated autocorrelation) ----------
        trackPitch();
    }

    int latencySamples() const { return 0; } // dry path is untouched

private:
    // Clear every stage of the synthesis path that feeds its own state back, so
    // a non-finite sample cannot latch the voice for the life of the node. This
    // is MikuStompCore::hardReset()'s counterpart; it deliberately does NOT
    // touch the input ring, the pitch tracker, the phrase position or any
    // param, because none of those is part of the latch.
    void hardReset() {
        for (auto& f : svf_) f.reset();
        burstBp_.reset(); burstHp_.reset(); voiceDc_.reset();
        srcLp_.reset(); decLp_.reset();
        fastEnv_.reset(); slowEnv_.reset();
        adaa_ = 0.0f;
        ampSmooth_.setCurrentAndTarget(0.0f);
    }

    static constexpr float kGateLin = 0.005f;   // ~-46 dB input gate
    static constexpr float kMakeup  = 2.4f;
    static constexpr int   kCoeffInterval = 16;

    static bool isSep(char c) {
        return c == ' ' || c == ',' || c == '-' || c == '|' || c == '\n' || c == '\r' || c == '\t' || c == ';';
    }
    static int vowelIndex(char c) {
        switch (c) { case 'a': return 0; case 'e': return 1; case 'i': return 2;
                     case 'o': return 3; case 'u': return 4; default: return -1; }
    }
    static int consClass(char c) {
        switch (c) {
            case 'p': case 't': case 'k': case 'b': case 'd': case 'g': return ConsPlosive;
            case 's': case 'z': case 'f': case 'h': case 'c': case 'x': return ConsFricative;
            case 'n': case 'm': return ConsNasal;
            case 'l': case 'r': case 'w': case 'y': case 'j': case 'v': return ConsLiquid;
            default: return ConsNone;
        }
    }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

    // The 8 built-in patterns (Looh Lahh Ahh Pahh Nyan Scat Random1 Random2).
    // A new note: take the next syllable from the custom table if there is one,
    // else from the (immutable) preset table, and drive the voice with it.
    void advanceSyllable(const Syllable* preset, int presetLen) {
        const int prev = sylIndex_.load(std::memory_order_relaxed);
        Syllable s;
        int idx = 0;
        bool have = false;
        switch (nextCustomSyllable(prev, idx, s))
        {
            case PhraseRead::Ok:   have = true; break;
            // A setPhrase that never settled inside the bounded retry: sing the
            // current syllable one note longer rather than wait or guess.
            case PhraseRead::Busy: return;
            case PhraseRead::Empty: break;   // -> preset table
        }
        if (!have)
        {
            if (preset == nullptr || presetLen <= 0) return;
            idx = (prev + 1) % presetLen;
            s   = preset[idx];
        }
        sylIndex_.store(idx, std::memory_order_relaxed);
        curSyl_.store(packSyllable(s), std::memory_order_relaxed);
        vowelSmooth_.setTarget((float) s.vowel * 0.25f);
        startBurst(s.cons);
    }

    static const Syllable* presetTable(int pattern, int& lenOut) {
        static const Syllable looh[]  = { { ConsLiquid, 4 } };
        static const Syllable lahh[]  = { { ConsLiquid, 0 } };
        static const Syllable ahh[]   = { { ConsNone, 0 } };
        static const Syllable pahh[]  = { { ConsPlosive, 0 } };
        static const Syllable nyan[]  = { { ConsNasal, 0 }, { ConsNasal, 0 } };
        static const Syllable scat[]  = { { ConsPlosive, 4 }, { ConsPlosive, 0 }, { ConsPlosive, 2 }, { ConsPlosive, 3 } };
        static const Syllable rnd1[]  = { { ConsLiquid, 0 }, { ConsNone, 2 }, { ConsNasal, 0 }, { ConsPlosive, 3 },
                                          { ConsLiquid, 2 }, { ConsNone, 4 }, { ConsPlosive, 0 }, { ConsNasal, 1 } };
        static const Syllable rnd2[]  = { { ConsFricative, 0 }, { ConsPlosive, 2 }, { ConsNone, 3 }, { ConsLiquid, 1 },
                                          { ConsNasal, 4 }, { ConsPlosive, 1 }, { ConsFricative, 2 }, { ConsNone, 0 } };
        switch (pattern < 0 ? 0 : (pattern > 7 ? 7 : pattern)) {
            case 0: lenOut = 1; return looh;
            case 1: lenOut = 1; return lahh;
            case 2: lenOut = 1; return ahh;
            case 3: lenOut = 1; return pahh;
            case 4: lenOut = 2; return nyan;
            case 5: lenOut = 4; return scat;
            case 6: lenOut = 8; return rnd1;
            default: lenOut = 8; return rnd2;
        }
    }

    void applyParams() {
        // nothing rate-dependent to rebuild; formants refresh via the
        // block-rate coefficient path (character folds into updateFormants).
        lastCoeffV_ = -1.0f;
        coeffCountdown_ = 0;
    }

    void startBurst(int cons) {
        burstType_ = cons;
        if (cons == ConsPlosive)        burstLeft_ = (int) (0.012 * sr_);
        else if (cons == ConsFricative) burstLeft_ = (int) (0.060 * sr_);
        else                            burstLeft_ = 0;   // nasal/liquid/none: no noise head
        burstNorm_ = burstLeft_ > 0 ? 1.0f / (float) burstLeft_ : 0.0f;
        if (cons == ConsNasal) {
            // "hum from closed U, opens to the vowel" -- which is what the old
            // single setCurrentAndTarget(1.0f) did NOT do. advanceSyllable()
            // sets the vowel target one line before calling this, and
            // setCurrentAndTarget overwrites the TARGET as well as the current
            // value, so a nasal mora hummed on U and never opened into its
            // vowel at all. Snap the CURRENT value to the closed U and re-arm
            // the ramp toward the syllable's own vowel.
            const float vowelTarget = vowelSmooth_.target();
            vowelSmooth_.setCurrentAndTarget(1.0f);
            vowelSmooth_.setTarget(vowelTarget);
        }
    }

    float nextNoise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return ((float) (rng_ & 0xFFFFFF) * (1.0f / 8388608.0f)) - 1.0f;
    }

    // Interpolate the vowel tables at pos (0..1 over A E I O U), shift by the
    // character multiplier, and load all three resonators.
    void updateFormants(float pos, float charMul) {
        float seg = pos * 4.0f;
        int idx = (int) seg;
        if (idx < 0) idx = 0;
        if (idx > 3) idx = 3;
        float t = clamp01(seg - (float) idx);
        for (int f = 0; f < 3; ++f)
        {
            float fc = kFreq[f][idx] + (kFreq[f][idx + 1] - kFreq[f][idx]) * t;
            const float bw = kBw[f][idx] + (kBw[f][idx + 1] - kBw[f][idx]) * t;
            const float db = kAmpDb[f][idx] + (kAmpDb[f][idx + 1] - kAmpDb[f][idx]) * t;
            fc = clampf(fc * charMul, 120.0f, (float) (0.45 * sr_));
            double Q = (double) fc / (double) ((bw < 20.0f) ? 20.0f : bw);
            if (Q < 1.0) Q = 1.0;
            if (Q > 40.0) Q = 40.0;
            g_[f] = std::pow(10.0f, db * (1.0f / 20.0f)) * (float) (1.0 / Q);
            svf_[(size_t) f].set(sr_, fc, Q);
        }
    }

    // Normalized autocorrelation over the decimated ring, once per block.
    void trackPitch() {
        if (ringPos_ < acWin_ + maxLag_) return;
        // Window = the newest acWin_ decimated samples.
        const long long newest = ringPos_ - 1;
        float energy0 = 1e-9f;
        for (int k = 0; k < acWin_; ++k)
        {
            const float v = ring_[(size_t) ((newest - k) & ringMask_)];
            energy0 += v * v;
        }
        if (energy0 < 1e-6f) { publishF0(0.0f); return; }

        // Normalized correlation per lag (preallocated buffer).
        float rMax = 0.0f;
        for (int lag = minLag_; lag <= maxLag_; ++lag)
        {
            float acc = 0.0f, energyL = 1e-9f;
            for (int k = 0; k < acWin_; ++k)
            {
                const float a = ring_[(size_t) ((newest - k) & ringMask_)];
                const float b = ring_[(size_t) ((newest - k - lag) & ringMask_)];
                acc += a * b;
                energyL += b * b;
            }
            const float r = acc / std::sqrt(energy0 * energyL);
            rBuf_[(size_t) lag] = r;
            if (r > rMax) rMax = r;
        }

        float f0 = 0.0f;
        if (rMax > 0.5f)
        {
            // Pick the FIRST local peak within 12% of the global max — the
            // global max alone loves the 2x-period subharmonic (octave-down
            // error on clean plucks). MPM-style.
            const float thresh = 0.88f * rMax;
            int pick = 0;
            for (int lag = minLag_ + 1; lag < maxLag_; ++lag)
            {
                const float r = rBuf_[(size_t) lag];
                if (r >= thresh && r >= rBuf_[(size_t) (lag - 1)] && r >= rBuf_[(size_t) (lag + 1)])
                {
                    pick = lag;
                    break;
                }
            }
            if (pick == 0)
            {
                // no interior peak (max at a boundary): fall back to argmax
                for (int lag = minLag_; lag <= maxLag_; ++lag)
                    if (rBuf_[(size_t) lag] >= rMax) { pick = lag; break; }
            }
            if (pick > 0)
            {
                float lag = (float) pick;
                if (pick > minLag_ && pick < maxLag_)
                {
                    const float rp = rBuf_[(size_t) (pick - 1)];
                    const float rc = rBuf_[(size_t) pick];
                    const float rn = rBuf_[(size_t) (pick + 1)];
                    const float denom = rp - 2.0f * rc + rn;
                    if (std::fabs(denom) > 1e-9f)
                    {
                        const float d = 0.5f * (rp - rn) / denom;
                        if (d > -1.0f && d < 1.0f) lag += d;
                    }
                }
                f0 = (float) (srDec_ / (double) lag);
            }
        }
        publishF0(f0);
    }

    void publishF0(float f0) {
        // median-of-3 kills single-block octave flips
        f0Hist_[0] = f0Hist_[1]; f0Hist_[1] = f0Hist_[2]; f0Hist_[2] = f0;
        float a = f0Hist_[0], b = f0Hist_[1], c = f0Hist_[2];
        const float med = std::max(std::min(a, b), std::min(std::max(a, b), c));
        if (med > 20.0f)
        {
            f0Raw_ = med;
            f0Smooth_.setTarget(med);
        }
        // (unvoiced: HOLD the last pitch — the amp gate silences the voice)
        trackedHz_.store(f0Raw_, std::memory_order_relaxed);
    }

    // Vowel formant tables, A E I O U (same Klatt/Csound-style data set as
    // dcdsp_formant.h; duplicated so both cores stay standalone-compilable).
    static constexpr float kFreq[3][5] = {
        {  700.f,  530.f,  270.f,  570.f,  300.f },   // F1
        { 1220.f, 1840.f, 2290.f,  840.f,  870.f },   // F2
        { 2600.f, 2480.f, 3010.f, 2410.f, 2240.f },   // F3
    };
    static constexpr float kAmpDb[3][5] = {
        {   0.f,   0.f,   0.f,   0.f,   0.f },
        {  -6.f, -14.f, -15.f, -10.f, -20.f },
        {  -7.f, -12.f, -18.f, -12.f, -17.f },
    };
    static constexpr float kBw[3][5] = {
        {  80.f,  70.f,  40.f,  40.f,  40.f },
        {  90.f,  80.f,  90.f,  80.f,  60.f },
        { 120.f, 100.f, 100.f, 100.f, 100.f },
    };

    MikuParams params_{};
    double sr_ = 48000.0;
    int    nch_ = 1;
    bool   prepared_ = false;

    // analysis
    OnePole decLp_;
    std::vector<float> ring_;
    int ringSize_ = 0, ringMask_ = 0;
    // 64-bit: a monotonic int ring cursor overflows after ~49.7 h of continuous
    // processing, after which trackPitch's "ringPos_ < acWin_ + maxLag_" guard
    // is true for ever and pitch tracking stops for good. dcdsp_droptune.h
    // documents and fixed this exact failure the same way.
    long long ringPos_ = 0;
    int dec_ = 4, decPhase_ = 0;
    double srDec_ = 12000.0;
    int minLag_ = 24, maxLag_ = 184, acWin_ = 360;
    std::vector<float> rBuf_;
    EnvelopeFollower fastEnv_, slowEnv_;
    std::vector<float> feDelay_;
    int feDelayLen_ = 1, feDelayPos_ = 0;
    int refractory_ = 0, cooldown_ = 0, onsetAge_ = 1 << 30;
    float f0Hist_[3] = { 0, 0, 0 };
    float f0Raw_ = 0.0f;
    std::atomic<float> trackedHz_{ 0.0f };

    // sequencer
    // Custom phrase table, seqlock-published (see setPhrase). Packed into
    // lock-free atomics so a read that races a publish is well-defined.
    std::array<std::atomic<uint32_t>, kMaxSyllables> custom_{};
    std::atomic<int>      customCount_{ 0 };
    std::atomic<uint32_t> phraseSeq_{ 0 };                       // even = stable
    std::atomic<uint32_t> curSyl_{ packSyllable(Syllable{}) };   // poll-only
    std::atomic<int> sylIndex_{ -1 };   // syllable LAST sung; -1 = none yet

    // voice
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue f0Smooth_, ampSmooth_, vowelSmooth_;
    SmoothedValue charSmooth_;      // params_.character stays the TARGET
    float lastCoeffChar_ = -1.0f;   // the charMul the cached formants were built for
    SmoothedValue mixSmooth_;
    Lfo vibLfo_;
    OnePole srcLp_;
    std::array<StateVariableTPT, 3> svf_{};
    float g_[3] = { 0, 0, 0 };
    float lastCoeffV_ = -1.0f;
    int   coeffCountdown_ = 0;
    float phase_ = 0.0f;
    float adaa_ = 0.0f;
    Biquad burstBp_, burstHp_;
    DcBlocker voiceDc_;
    int burstLeft_ = 0, burstType_ = ConsNone;
    float burstNorm_ = 0.0f;
    uint32_t rng_ = 0x9E3779B9u;
};

// Out-of-line constexpr definitions (C++17 ODR).
constexpr float MikuCore::kFreq[3][5];
constexpr float MikuCore::kAmpDb[3][5];
constexpr float MikuCore::kBw[3][5];

} // namespace dcdsp

#endif // DCDSP_MIKU_H
