// dcdsp_mikustomp.h — MIKU STOMP recreation (0.16 §1.19b, "mikustomp").
//
// A faithful-as-legally-possible recreation of the KORG MIKU STOMP (2014), the
// guitar pedal that makes Hatsune Miku sing what you play. Sibling of, NOT a
// replacement for, `dcdsp_miku.h` (v1) — the owner keeps both:
//
//   * `miku`      — our own fun, robotic 3-formant vocal stomp. Kept as-is.
//   * `mikustomp` — this one: modelled on the real hardware's architecture,
//                   phonetics (Japanese moras), controls and behaviour.
//
// ---------------------------------------------------------------------------
// WHAT THE REAL PEDAL DOES (research summary; sources in the module docs)
// ---------------------------------------------------------------------------
// The Miku Stomp is a guitar front-end bolted to a Yamaha NSX-1 (YMW820) sound
// LSI running *eVocaloid* — an embedded VOCALOID whose singing channel (MIDI
// ch. 1) is MONOPHONIC. Signal path:
//
//   guitar in -> monophonic pitch tracker (spec'd E1 41 Hz .. E6 1319 Hz)
//             +  pick/onset detector
//             -> "note on" + phoneme-buffer advance
//             -> eVocaloid voice (Articulation Element Modeling, AEM)
//             -> out (wet; the pedal replaces your guitar sound)
//
// eVocaloid consumes a STREAM OF PHONETIC SYMBOLS and cuts each phoneme into
// head / body / tail fragments; when two notes are joined it synthesises a
// JOINT that replaces the head of the second note. Every Note-On advances the
// phoneme buffer by one symbol. NSX-1 exposes NRPNs for vibrato type/rate/
// delay, portamento timing, and a "phoneme unit connect type" whose default is
// a FIXED 50 ms transition. Lyrics are Japanese only (hiragana/katakana/romaji)
// and load from the iOS Lyric Editor over the pickup. One knob picks one of 11
// lyric patterns: Ahh, Lahh, Looh, Pahh, Nyan, Scat, Phrase 1-3 (user lyrics),
// Random 1, Random 2. Reviewers universally report: monophonic (hates chords),
// audible latency, finicky tracking, and a bright/nasal "kawaii" timbre.
//
// ---------------------------------------------------------------------------
// HOW THIS DIFFERS FROM THE HARDWARE (be honest)
// ---------------------------------------------------------------------------
// The hardware is SAMPLE-BASED: it concatenates recorded diphone fragments from
// Crypton's Hatsune Miku voicebank (Saki Fujita's voice). We cannot ship that
// voicebank — so we synthesise the same STRUCTURE parametrically, using the
// model VOCALOID itself was built on: **EpR (Excitation plus Resonances)**
// (Bonada/Loscos/Serra, MTG-UPF, ICMC 2001; Yamaha's own patents state
//   OrgMag_dB(f) = ExcitationCurve_dB(f) + ChestResonance_dB(f)
//                + VocalTractResonance_dB(f) + SpectralShapeDiff_dB(f)
// with the excitation curve parameterised by EGain / ESlope / ESlopeDepth).
//
// So instead of splicing samples we run a source-filter singer:
//
//   1. EXCITATION: polyBLEP glottal-derivative pulse at the tracked pitch,
//      spectral tilt (ESlope/ESlopeDepth as a one-pole), a chest/glottal
//      resonance (the EpR "excitation resonance"), pitch-synchronous
//      aspiration noise (breathiness), jitter + shimmer.
//   2. RESONANCES: a 4-resonator Klatt CASCADE (F1..F4) + a nasal antiformant.
//      A cascade is the time-domain equivalent of EpR's dB-sum: vowel formant
//      amplitudes fall out of the resonances instead of an amp table.
//   3. AEM-style ARTICULATION: a per-consonant table of *loci* (Delattre F1/F2/
//      F3 locus targets) plus closure / burst / aspiration / frication segments.
//      A syllable starts at the consonant's locus and the formants GLIDE to the
//      vowel -> that is a synthesised diphone. A re-picked note gets the full
//      HEAD (closure + burst); a legato note change gets a JOINT (no head,
//      formants + pitch glide) — exactly the AEM head/joint split.
//
// Consequences (documented, not bugs): our voice is parametric, so it is
// cleaner and more stable than the real pedal, has no voicebank "grain", and
// cannot reproduce Miku's exact timbre — only her formant class (young female
// Japanese, bright/nasal). We are also *better* at tracking than the hardware,
// which is a feature; `mix` lets you keep the dry guitar (the hardware can't).
//
// ---------------------------------------------------------------------------
// 0.18.1 INTELLIGIBILITY PASS — read this before touching the articulation
// ---------------------------------------------------------------------------
// Three complaints ("hard to hear the syllables", "struggles with some letters",
// "a weird blob at the end") were three BUGS, all measured with the standalone
// harness in tools/miku_measure.cpp. Each fix carries its reasoning at the site;
// the short version, so nobody re-introduces them:
//
//   * THE GLIDE IS ARMED AT THE RELEASE, NOT AT THE SYLLABLE START. The
//     locus->vowel transition IS the consonant cue. It used to start when the
//     syllable did, so the whole sweep ran during the (silent) closure and burst.
//     Measured: F2 at the first VOICED frame sat at the vowel's own ~1650 Hz no
//     matter what the locus was — /b/, /d/ and /g/ were the same sound.
//   * THE NASAL ZERO HAS A POLE. An antiresonator alone is 1/H_res: unity at DC
//     and rising +12 dB/oct forever (+45 dB at 8 kHz, measured). Klatt pairs RNZ
//     with RNP and parks the zero ON the pole when not nasalised, where they
//     cancel EXACTLY — which needs both to share kNasalBw. Do not "optimise away"
//     the pole because it looks like a no-op on vowels: that no-op is the point.
//   * NOTE-OFF IS ITS OWN FADE. Three envelope releases in series (50 ms gate ->
//     150 ms level -> 90 ms amp) kept the voice singing for 715 ms after the
//     input hit digital silence. That was the "blob".
//
// Also: stops have ASPIRATION (Klatt AH / VOT) so the formant sweep is audible in
// it; frication has TWO noise poles (one is a whistle, not a sibilant) and Klatt's
// ramp-IN envelope; /h/ is a "voiceless vowel" (glottal noise through the FOLLOWING
// vowel's formants); velars pick their locus per-vowel (the velar pinch); and the
// moraic /N/ is a hum that never opens into a vowel.
//
// ---------------------------------------------------------------------------
// 0.20 SYLLABLE PASS — "troubles with syllables and certain letters"
// ---------------------------------------------------------------------------
// Six more measured defects (harness T11-T16; full numbers in
// docs/miku-stomp-research.md in DotCharter). No param changes.
//
//   * EVERY PHRASE STARTED ON ITS SECOND MORA. The syllable index is "last
//     sung" with a pre-increment advance and it initialised to 0, so the first
//     pick sang table[1]: "nyan" opened on the /N/ hum. Index starts at -1 now;
//     a pattern-knob change (and re-loading the active Phrase slot) restarts
//     the phrase.
//   * THE OUTPUT SOFT-CLIP WAS A TONE STAGE BY ACCIDENT. tanh(2.2*amp*v) put
//     the nominal signal at |x|~1.1; /o/'s F2..F3 valley carried cubic products
//     only 22 dB under F2 and LPC read them as an EXTRA FORMANT at ~1.7 kHz.
//     The 2.2 makeup now lives AFTER the tanh. (Drive-ratio proof at the site.)
//   * THE ONSET DETECTOR WENT DEAF AT RIFF SPEED: at 16 picks/s only 1 of 48
//     picks fired it (envelope release too slow, lookback on the previous
//     pick's tail). 25 ms release / 30 ms lookback / 45 ms refractory: 48/48.
//   * AFFRICATES ROSE LIKE FRICATIVES: one shared 16 ms attack erased the
//     /s/-vs-/ts/ rise-time cue (Howell & Rosen). Affricates attack in 3 ms.
//   * THE /N/ HUM WAS LOUDER THAN THE VOWELS and the old saturating output
//     stage had been hiding it. Normalise at max(|H(F1)|, |H(pole)|) and use a
//     real nostril loss (0.85) — the hum sits ~5 dB under the vowel now.
//   * THE VOICE VANISHED ON HIGH NOTES (/i/ -24.6 dBFS at a sung 659 Hz):
//     above F1 no harmonic excites it. F1 is floored at ~1.05*f0 (soprano
//     formant tuning, Sundberg), inactive below a sung ~340 Hz, with a
//     loudness-continuity trim. Ablation guard: DCDSP_MIKU_NO_F1FLOOR.
//
// THE CEILING, honestly: real Miku is a concatenative SAMPLER. A formant synth
// cannot converge on a sampler's timbre — that is architecture, not tuning. What
// IS reachable is everything above, because it is about speech, not about one
// singer's larynx. Do not chase "identical to Miku" here. (Also measured and
// accepted: vowel IDENTITY above a sung ~450 Hz degrades for human singers and
// the sample-based hardware alike — 1-2 harmonics under 1 kHz undersample any
// envelope. The F1 floor recovers the energy; nothing recovers the identity.)
//
// ---------------------------------------------------------------------------
// PURE header-only dcdsp core, C++17, standard library only. RT contract: all
// allocation in prepare(); process() never allocates, locks or throws; output
// is always finite and |y| <= 8. setPhrase/setParams are message-thread; each
// phrase table is published behind a per-slot SEQLOCK (the ParamMailbox pattern
// from DspModule.h) and its syllables are stored as lock-free atomics, so the
// audio thread can never read a half-written table and never spins or frees.
// It used to be a two-buffer flip behind an index, which is ABA-unsafe: two
// setPhrase calls in a row return to the buffer a reader is still holding and
// overwrite it under them.
#ifndef DCDSP_MIKUSTOMP_H
#define DCDSP_MIKUSTOMP_H

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

struct MikuStompParams {
    // Voice/dry blend. The HARDWARE is wet-only (it replaces your guitar), so
    // 1 is the faithful default; turn it down to blend. [0,1, default 1]
    float mix = 1.0f;
    // Voice character: vocal-tract length / brightness. 0 = dark & adult,
    // 0.5 = the "kawaii" Miku scale (formants x1.08 + presence lift),
    // 1 = tiny & piercing. [0,1, default 0.5]
    float character = 0.5f;
    // Breathiness: trades glottal-pulse brightness for aspiration noise —
    // the breathy-female cue that dominates Miku's timbre. [0,1, default 0.35]
    float breath = 0.35f;
    // Voice amplitude attack, ms. The pedal is famously dynamics-sensitive.
    // [1,200, default 12]
    float attackMs = 12.0f;
    // Portamento time between LEGATO notes, ms (NSX-1 "portamento timing").
    // A re-picked note always attacks fresh regardless. [0,300, default 50]
    float portamentoMs = 50.0f;
    // Vibrato depth (0 = off, 1 = +-70 cents). Develops after ~300 ms of
    // sustain, like the NSX-1 vibrato delay. [0,1, default 0.45]
    float vibrato = 0.45f;
    // Vibrato rate, Hz. [3,8, default 5.8]
    float vibratoRateHz = 5.8f;
    // Octave shift of the sung pitch vs. the tracked guitar pitch, in whole
    // octaves (rounded). +1 lands the guitar in Miku's register.
    // [-1,2, default 1]
    float octave = 1.0f;
    // Lyric pattern 0..10 (rounded), in the hardware's own order:
    //   0 Ahh  1 Lahh  2 Looh  3 Pahh  4 Nyan  5 Scat
    //   6 Phrase 1  7 Phrase 2  8 Phrase 3   (user lyrics; see setPhrase)
    //   9 Random 1  10 Random 2
    // An empty Phrase slot falls back to "Ahh".
    float pattern = 0.0f;
};

class MikuStompCore {
public:
    // ---- Phonetics ---------------------------------------------------------
    // Japanese moraic consonant inventory (the pedal is Japanese-only). Index
    // into kCons below; keep in sync.
    enum Cons {
        C_None = 0, C_K, C_G, C_S, C_SH, C_Z, C_J, C_T, C_TS, C_CH, C_D,
        C_N, C_NY, C_H, C_F, C_B, C_P, C_M, C_Y, C_R, C_W, C_NN,
        kNumCons
    };
    // Vowels: a i u e o (Japanese five-vowel system).
    enum Vowel { V_A = 0, V_I, V_U, V_E, V_O, kNumVowels };

    struct Syllable {
        int cons  = C_None; // Cons
        int vowel = V_A;    // Vowel
    };

    static constexpr int kMaxSyllables = 96;
    static constexpr int kNumSlots     = 3;   // Phrase 1..3, like the hardware
    static constexpr int kNumPatterns  = 11;

    // A user phrase slot is shared between the message thread (setPhrase) and
    // the audio thread, so its syllables live as packed 32-bit atomics: one
    // lock-free load per note instead of a two-int struct copy that can tear.
    // Both halves are RANGE-CLAMPED here, at the only two places they cross —
    // `cons`/`vowel` index kCons/kVowF, so an out-of-range value read out of a
    // racing publish would be an out-of-bounds table read, not a wrong sound.
    static constexpr uint32_t packSyllable(const Syllable& s) noexcept {
        const uint32_t c = (uint32_t) ((s.cons  >= 0 && s.cons  < kNumCons)   ? s.cons  : (int) C_None);
        const uint32_t v = (uint32_t) ((s.vowel >= 0 && s.vowel < kNumVowels) ? s.vowel : (int) V_A);
        return (c << 8) | v;
    }
    static constexpr Syllable unpackSyllable(uint32_t packed) noexcept {
        const int c = (int) ((packed >> 8) & 0xFFu);
        const int v = (int) (packed & 0xFFu);
        return Syllable{ (c < kNumCons) ? c : (int) C_None,
                         (v < kNumVowels) ? v : (int) V_A };
    }
    // Bounded, because the audio thread may never spin. Three tries is already
    // far more than a publish window (a memcpy of at most 96 words); the give-up
    // path costs one repeated mora, not a glitch.
    static constexpr int kPhraseReadTries = 3;

    // ---- Romaji phrase parser (message thread) -----------------------------
    // Splits on whitespace / comma / hyphen / '|' / ';'. Each token is a mora:
    // an optional consonant (2-char digraphs first: ky sh ch ts ny hy my ry gy
    // by py dy zy jy sy ty), then the first vowel letter. A vowel-less token is
    // the moraic nasal /N/ ("n" -> a hummed nasal). ASCII-insensitive; non-ASCII
    // bytes are skipped so a UTF-8 string can never mis-index. This is a strict
    // SUPERSET of the v1 `miku` parser's contract, so the same "phrases" string
    // works in both modules. Returns the syllable count (<= cap).
    static int parsePhrases(const char* utf8, Syllable* out, int cap)
    {
        if (utf8 == nullptr || out == nullptr || cap < 1) return 0;
        int count = 0;
        const char* p = utf8;
        while (*p != '\0' && count < cap)
        {
            while (*p != '\0' && isSep(*p)) ++p;
            if (*p == '\0') break;

            // Collect the ASCII letters of this token (bounded).
            char tok[16];
            int  tlen = 0;
            for (; *p != '\0' && !isSep(*p); ++p)
            {
                const unsigned char u = (unsigned char) *p;
                if (u >= 0x80) continue;                 // stay bounded on UTF-8
                if (tlen < 15) tok[tlen++] = (char) std::tolower(u);
            }
            tok[tlen] = '\0';
            if (tlen == 0) continue;

            int cons = C_None, vowel = -1, i = 0;
            // Leading consonant (digraph first).
            if (vowelIndex(tok[0]) < 0)
            {
                if (tlen >= 2)
                {
                    const int d = digraph(tok[0], tok[1]);
                    if (d != C_None) { cons = d; i = 2; }
                }
                if (cons == C_None)
                {
                    const int s = singleCons(tok[0]);
                    if (s != C_None) { cons = s; i = 1; }
                    else i = 1;                          // unknown letter: skip it
                }
            }
            // First vowel after the onset.
            for (; i < tlen; ++i)
            {
                const int v = vowelIndex(tok[i]);
                if (v >= 0) { vowel = v; break; }
            }
            if (vowel < 0)
            {
                // Vowel-less mora: "n"/"nn" is the moraic nasal; anything else
                // takes the closed /u/ that Japanese epenthesises.
                if (cons == C_N || cons == C_NN) { cons = C_NN; vowel = V_U; }
                else                             { vowel = V_U; }
            }
            out[count].cons  = cons;
            out[count].vowel = vowel;
            ++count;
        }
        return count;
    }

    // ---- Lifecycle ---------------------------------------------------------

    void prepare(double sampleRate, int maxBlockSize, int numChannels) {
        sr_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        (void) maxBlockSize;
        nch_ = (numChannels > 0) ? numChannels : 1;

        // --- analysis: decimated stream for the autocorrelation -------------
        // The hardware spec's E1 (41 Hz) .. E6 (1319 Hz); match it. 16 kHz of
        // analysis rate keeps a usable lag resolution at E6 (lag ~12).
        dec_    = std::max(1, (int) std::lround(sr_ / 16000.0));
        srDec_  = sr_ / (double) dec_;
        maxLag_ = (int) (srDec_ / 40.0);              // ~E1
        minLag_ = std::max(3, (int) (srDec_ / 1400.0)); // ~E6
        acLong_  = (int) (srDec_ * 0.026);            // 26 ms: 1 period at E1
        acShort_ = (int) (srDec_ * 0.013);            // 13 ms: fast post-pick lock
        hopDec_  = std::max(1, (int) (srDec_ * 0.004)); // analyse every ~4 ms
        hopCount_ = 0;
        ringSize_ = 1;
        while (ringSize_ < acLong_ + maxLag_ + 8) ringSize_ <<= 1;
        ring_.assign((size_t) ringSize_, 0.0f);
        ringMask_ = ringSize_ - 1;
        ringPos_ = 0;
        decPhase_ = 0;
        decLp_.setLowpass(sr_, 6000.0);
        rBuf_.assign((size_t) maxLag_ + 2, 0.0f);

        // --- analysis: onset ------------------------------------------------
        // The onset feature is a HIGH-FREQUENCY envelope, not a broadband one:
        // a pick transient is broadband, a still-ringing string is not. Over a
        // decaying note the broadband env only jumps ~1.4x on a re-pick (misses
        // half of them — this is exactly the "finicky tracking" the real pedal
        // is panned for); the >1.2 kHz envelope jumps 2.1-4.0x and peaks within
        // 1-5 ms of the pick instead of 6-20 ms. Measured, not guessed.
        onsetHp_.setHighpass(sr_, 1200.0, 0.707);
        // Release 25 ms (was 40) and lookback 30 ms (was 15): at 16 picks/s the
        // envelope only decayed to ~0.31 of the previous peak by the next pick and
        // the 15 ms-ago reference still sat ON that pick's tail, so the >1.8x rise
        // test went DEAF — measured (T15 probe): 1 of 48 tremolo picks fired the
        // onset path; the other advances were LEGATO jumps, half of them eaten by
        // the 90 ms legato lock. With 25/30 the reference decays to ~0.3 by the
        // lookback point and every pick clears the ratio. The lookback MUST stay
        // below the 45 ms refractory (the droptune core's hard-won rule: if `past`
        // is still pre-pick when the lock expires, the detector free-runs).
        hfEnv_.prepare(sr_);   hfEnv_.setTimes(1.0f, 25.0f);
        // The GATE's release was 50 ms, which meant it had to coast ~170 ms down to
        // the -46 dB floor before it would even admit the player had stopped. 30 ms
        // still rides out one period of a low E without chattering (measured: the
        // sustained-note AM check in tools/miku_measure.cpp) and cuts a third of
        // the hangover. The LEVEL follower keeps its slow 150 ms release — the
        // ripple-free level is what the voice's amplitude tracks.
        fastEnv_.prepare(sr_); fastEnv_.setTimes(1.0f, 30.0f);
        slowEnv_.prepare(sr_); slowEnv_.setTimes(10.0f, 150.0f);
        feDelayLen_ = (int) (sr_ * 0.030) + 4;
        feDelay_.assign((size_t) feDelayLen_, 0.0f);
        feDelayPos_ = 0;
        // 45 ms, not 60: at 60 ms a 16-picks/s tremolo riff (62.5 ms spacing) sat
        // right on the lock and pick-timing jitter dropped a third of the
        // syllables (measured, T15: 32 advances for 48 picks). 45 ms keeps every
        // pick to 16/s and still rides out the pick's own messy attack; T6's
        // noisy-input test stays at 0 stray advances.
        refractory_ = (int) (sr_ * 0.045);
        cooldown_ = 0;
        sinceOnset_ = 1 << 28;
        legatoLock_ = 0;

        // --- voice ----------------------------------------------------------
        for (auto& r : form_) r.reset();
        nasalPole_.reset();
        nasalZero_.reset();
        chest_.setPeaking(sr_, 420.0, 1.1, 4.0);
        srcLp_.setLowpass(sr_, 3000.0);
        envLp_.setLowpass(sr_, 50.0);
        envLp2_.setLowpass(sr_, 50.0);
        aspHp_.setHighpass(sr_, 500.0, 0.707);
        aspStopHp_.setHighpass(sr_, 400.0, 0.707);
        fricBp_.setBandpass(sr_, 3000.0, 2.0);
        fricBp2_.setBandpass(sr_, 5000.0, 2.0);
        presence_.setHighShelf(sr_, 3200.0, 0.707, 3.0);
        voiceDc_.prepare(sr_);
        vibLfo_.prepare(sr_);
        vibLfo_.setShape(Lfo::Sine);
        vibLfo_.setFrequency(5.8f);

        // Retuning does not CLEAR: every set*/prepare above writes coefficients
        // only (dcdsp_primitives.h), so without these the core carried the
        // previous rate's filter and envelope memory across a device change.
        // form_/nasalPole_/nasalZero_ were already reset above; these are the
        // ones the list missed.
        decLp_.reset();
        onsetHp_.reset();
        hfEnv_.reset(); fastEnv_.reset(); slowEnv_.reset();
        chest_.reset(); srcLp_.reset();
        envLp_.reset(); envLp2_.reset();
        aspHp_.reset(); aspStopHp_.reset();
        fricBp_.reset(); fricBp2_.reset();
        presence_.reset();
        voiceDc_.reset();
        vibLfo_.reset();
        adaa_ = 0.0f;

        // Articulator / envelope rates. These were bare literals tuned at 48 kHz;
        // deriving them from the rate keeps the articulation identical at 96 kHz.
        voicedC_  = onePoleCoef(5.0f);    // voicing on/off
        gateC_    = onePoleCoef(20.0f);   // "is the player still playing"
        bwC_      = onePoleCoef(12.0f);   // tract losses
        nasalC_   = onePoleCoef(25.0f);   // the velum takes ~25 ms to move
        aspC_     = onePoleCoef(4.0f);    // aspiration on/off
        noteOffC_ = onePoleCoef(30.0f);   // the dedicated note-off fade
        fricAtkC_ = onePoleCoef(16.0f);
        fricRelC_ = onePoleCoef(10.0f);
        // ...and these two, which the original sweep missed. Both were the bare
        // 48 kHz literals 0.001f / 0.002f, so shimmer and jitter ran at HALF
        // speed at 96 kHz while every coefficient above them stayed put.
        shimmerC_ = onePoleCoef(20.8f);   // ~= 0.001 at 48 kHz
        jitterC_  = onePoleCoef(10.4f);   // ~= 0.002 at 48 kHz

        f0Hist_[0] = f0Hist_[1] = f0Hist_[2] = 0.0f;
        for (int i = 0; i < kRefRing; ++i) refRing_[i] = 0.0f;
        refPos_ = 0;
        pitchArmed_ = false; pendingLegato_ = false;
        f0Raw_ = 0.0f;
        f0Cur_ = 0.0f; f0Target_ = 0.0f; f0Glide_ = 0.0f; f0GlideLeft_ = 0;
        sungHz_ = 0.0f;
        phase_ = 0.0f;
        amp_ = 0.0f; ampTarget_ = 0.0f;
        voiced_ = 0.0f; voicedTarget_ = 0.0f;
        jitter_ = 0.0f; jitterTarget_ = 0.0f; jitterLeft_ = 0;
        shimmer_ = 0.0f;
        segLeft_ = 0; seg_ = SegIdle;
        fricLeft_ = 0; fricLen_ = 1; fricGain_ = 0.0f; fricSustain_ = false;
        fricAspirate_ = false; fricEnv_ = 0.0f; fricTarget_ = 0.0f;
        aspLeft_ = 0; aspEnv_ = 0.0f; aspTarget_ = 0.0f;
        gate_ = 0.0f; voiceSilent_ = false;
        nasalAmt_ = 0.0f; nasalTarget_ = 0.0f;
        bwMul_ = 1.0f; bwMulTarget_ = 1.0f;
        curCons_ = C_None; curVowel_ = V_A;
        nzRawHz_ = nasalPoleHz(); nzHz_ = nzRawHz_;
        rng_ = 0xB5297A4Du;
        coeffCountdown_ = 0;
        cascadeNorm_ = 1.0f;
        for (int i = 0; i < 4; ++i) { fCur_[i] = kVowF[i][V_A]; fTgt_[i] = fCur_[i]; fInc_[i] = 0.0f; }
        for (int i = 0; i < 4; ++i) bCur_[i] = kVowB[i][V_A];
        formLeft_ = 0;
        lastScale_ = -1.0f; lastSig_ = -1.0f;

        // The three Phrase slots are USER CONTENT, not rate-derived state, and
        // prepare() must not touch them. It used to zero the whole phrase table
        // and reset the buffer selector, so every device change, sample-rate
        // change and watchdog recovery silently deleted the user's lyrics —
        // DspChain::prepare re-prepares the EXISTING module objects, so the
        // adapter's `phrases_` string survived and went on reporting text the
        // core no longer had. The pedal reverted to "Ahh" and getStringParam
        // lied about why.
        //
        // `slots_`, `slotCount_` and `phraseSeq_` are zero-initialised at
        // construction, which is the only time they should be.
        // -1 = "nothing sung yet". The index is the syllable LAST sung and the
        // advance is pre-increment, so it MUST start one before the first mora.
        // It used to start at 0, which made the FIRST pick sing table[1]: "nyan"
        // opened on the /N/ hum ("n-nya n-nya"), Scat on "bi" instead of "du",
        // and every user phrase on its SECOND mora. Measured in
        // tools/miku_measure.cpp's syllable-advance timelines.
        sylIndex_.store(-1, std::memory_order_relaxed);
        trackedHz_.store(0.0f, std::memory_order_relaxed);
        prepared_ = true;
        // `mix` RAMPS (kMixRampSeconds). It defaults to 1.0 (wet only), so any
        // drag off or onto 1.0 hard-spliced the whole guitar signal in or out at
        // a block edge.
        mixSmooth_.reset(sr_, kMixRampSeconds);
        charSmooth_.reset(sr_, kMixRampSeconds);
        charSmooth_.setTarget(params_.character);
        charSmooth_.snapToTarget();   // a freshly prepared pedal starts AT it
        mixSmooth_.setTarget(params_.mix);
        mixSmooth_.snapToTarget();   // a freshly prepared voice starts AT its mix
        applyParams();
        updateFormantCoeffs(true);
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        std::fill(feDelay_.begin(), feDelay_.end(), 0.0f);
        ringPos_ = 0; decPhase_ = 0; feDelayPos_ = 0; hopCount_ = 0;
        onsetHp_.reset(); hfEnv_.reset();
        fastEnv_.reset(); slowEnv_.reset(); decLp_.reset(); srcLp_.reset();
        envLp_.reset(); envLp2_.reset();
        for (auto& r : form_) r.reset();
        nasalPole_.reset(); nasalZero_.reset();
        chest_.reset(); aspHp_.reset(); aspStopHp_.reset();
        fricBp_.reset(); fricBp2_.reset(); presence_.reset();
        voiceDc_.reset(); vibLfo_.reset();
        f0Hist_[0] = f0Hist_[1] = f0Hist_[2] = 0.0f;
        for (int i = 0; i < kRefRing; ++i) refRing_[i] = 0.0f;
        refPos_ = 0; pitchArmed_ = false; pendingLegato_ = false;
        f0Raw_ = 0.0f; f0Cur_ = 0.0f; f0Target_ = 0.0f; f0Glide_ = 0.0f; f0GlideLeft_ = 0;
        sungHz_ = 0.0f;
        phase_ = 0.0f; amp_ = 0.0f; ampTarget_ = 0.0f;
        voiced_ = 0.0f; voicedTarget_ = 0.0f;
        jitter_ = 0.0f; jitterTarget_ = 0.0f; jitterLeft_ = 0; shimmer_ = 0.0f;
        segLeft_ = 0; seg_ = SegIdle;
        fricLeft_ = 0; fricLen_ = 1; fricGain_ = 0.0f; fricSustain_ = false;
        fricAspirate_ = false; fricEnv_ = 0.0f; fricTarget_ = 0.0f;
        aspLeft_ = 0; aspEnv_ = 0.0f; aspTarget_ = 0.0f;
        gate_ = 0.0f; voiceSilent_ = false; adaa_ = 0.0f;
        nasalAmt_ = 0.0f; nasalTarget_ = 0.0f;
        bwMul_ = 1.0f; bwMulTarget_ = 1.0f;
        nzRawHz_ = nasalPoleHz(); nzHz_ = nzRawHz_;
        curCons_ = C_None; curVowel_ = V_A;
        for (int i = 0; i < 4; ++i) { fCur_[i] = kVowF[i][V_A]; fTgt_[i] = fCur_[i]; fInc_[i] = 0.0f; }
        cooldown_ = 0; sinceOnset_ = 1 << 28; legatoLock_ = 0;
        coeffCountdown_ = 0; formLeft_ = 0; lastScale_ = -1.0f; lastSig_ = -1.0f;
        cascadeNorm_ = 1.0f;
        sylIndex_.store(-1, std::memory_order_relaxed);   // a reset restarts the phrase
    }

    void setParams(const MikuStompParams& p) {
        params_.mix           = clamp01(p.mix);
        mixSmooth_.setTarget(params_.mix);
        params_.character     = clamp01(p.character);
        // clamp01 is NaN-TRANSPARENT, and a NaN target poisons the smoother's
        // current value for good -- so guard it, and recover one that was.
        if (std::isfinite(params_.character)) {
            charSmooth_.setTarget(params_.character);
            if (!std::isfinite(charSmooth_.current())) charSmooth_.snapToTarget();
        }
        params_.breath        = clamp01(p.breath);
        params_.attackMs      = clampf(p.attackMs, 1.0f, 200.0f);
        params_.portamentoMs  = clampf(p.portamentoMs, 0.0f, 300.0f);
        params_.vibrato       = clamp01(p.vibrato);
        params_.vibratoRateHz = clampf(p.vibratoRateHz, 3.0f, 8.0f);
        params_.octave        = clampf(p.octave, -1.0f, 2.0f);
        params_.pattern       = clampf(p.pattern, 0.0f, (float) (kNumPatterns - 1));
        if (prepared_) applyParams();
    }

    // Custom lyrics for Phrase slot 0..2 (message thread), mirroring the
    // hardware's three Lyric-Editor slots. count 0 (or null) empties the slot
    // (the pattern then falls back to "Ahh").
    //
    // SEQLOCK, not a buffer flip. The old shape was a two-deep double buffer
    // behind `slotSel_`, and a two-deep flip is ABA-unsafe by construction: the
    // audio thread resolves `sel` and then reads out of that buffer LATER in
    // the block, and the second setPhrase after it flips straight back to the
    // buffer the reader is holding and rewrites it in place. `Syllable` is two
    // ints, so the tear is a real one — a cons from the new phrase against a
    // vowel from the old, which then indexes `kCons`/`kVowF` with whatever it
    // landed on. Now: an odd `phraseSeq_` marks a write in flight, the reader
    // takes (count, syllable) between two EQUAL EVEN reads of it, and gives up
    // after a bounded number of tries rather than spinning (the audio thread
    // never waits — it just sings the current mora one note longer). The
    // syllables themselves are lock-free `atomic<uint32_t>`, so even the
    // abandoned attempts are races only in the benign, well-defined sense.
    void setPhrase(int slot, const Syllable* syl, int count) {
        if (slot < 0 || slot >= kNumSlots) return;
        int n = 0;
        if (syl != nullptr)
        {
            n = std::min(count, kMaxSyllables);
            if (n < 0) n = 0;
        }
        auto& seq = phraseSeq_[(size_t) slot];
        seq.fetch_add(1, std::memory_order_acq_rel);              // odd: in flight
        for (int i = 0; i < n; ++i)
            slots_[(size_t) slot][(size_t) i].store(packSyllable(syl[i]),
                                                    std::memory_order_relaxed);
        slotCount_[(size_t) slot].store(n, std::memory_order_relaxed);
        seq.fetch_add(1, std::memory_order_release);              // even: stable
        // New lyrics in the slot being SUNG start at their first mora.
        if (curPattern_.load(std::memory_order_relaxed) == 6 + slot)
            sylIndex_.store(-1, std::memory_order_relaxed);
    }

    // ---- Introspection (poll-only; message thread or tests) ----------------
    float trackedHz() const { return trackedHz_.load(std::memory_order_relaxed); }
    int   syllableIndex() const { return sylIndex_.load(std::memory_order_relaxed); }
    /// Syllables the audio thread would sing for `slot` right now. Exists so a
    /// test can assert user lyrics SURVIVE a prepare() — the adapter's cached
    /// `phrases_` string cannot, because it is what went on lying when they did
    /// not.
    int   phraseSyllableCount(int slot) const {
        if (slot < 0 || slot >= kNumSlots) return 0;
        return slotCount_[(size_t) slot].load(std::memory_order_acquire);
    }
    /// The syllable the sequencer last committed to sing. Poll-only
    /// introspection, and the only way a test can see a TORN phrase read: a
    /// coherent read of a phrase built from one repeated mora always returns
    /// that mora, so a consonant paired with the wrong vowel is proof the
    /// publish is not atomic. Same motivation as phraseSyllableCount above.
    Syllable currentSyllable() const {
        return unpackSyllable(curSyl_.load(std::memory_order_relaxed));
    }

    /// Outcome of a user-phrase read on the audio thread. Ok = a coherent
    /// (count, syllable) pair; Empty = the slot has no lyrics, so the caller
    /// falls back to the preset table; Busy = a setPhrase was in flight for
    /// longer than the bounded retry, so the caller must NOT advance.
    enum class PhraseRead { Ok, Empty, Busy };

    /// Reads the syllable after `prevIndex` out of user Phrase slot `slot`.
    /// Audio thread — and the RT gate's coherence probe, which drives this at a
    /// rate no callback reaches, which is the only way the seqlock gets
    /// exercised hard enough to prove it. Never allocates, locks or spins.
    PhraseRead nextUserSyllable(int slot, int prevIndex,
                                int& idxOut, Syllable& out) const noexcept {
        if (slot < 0 || slot >= kNumSlots) return PhraseRead::Empty;
        const auto& seq = phraseSeq_[(size_t) slot];
        for (int attempt = 0; attempt < kPhraseReadTries; ++attempt)
        {
            const uint32_t s0 = seq.load(std::memory_order_acquire);
            if ((s0 & 1u) != 0u) continue;                   // write in flight
            const int n = slotCount_[(size_t) slot].load(std::memory_order_relaxed);
            if (n <= 0)
            {
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq.load(std::memory_order_relaxed) != s0) continue;
                return PhraseRead::Empty;
            }
            const int idx = (prevIndex + 1) % n;
            const uint32_t packed =
                slots_[(size_t) slot][(size_t) idx].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq.load(std::memory_order_relaxed) != s0) continue;  // torn: retry
            idxOut = idx;
            out    = unpackSyllable(packed);
            return PhraseRead::Ok;
        }
        return PhraseRead::Busy;
    }

    // ---- Audio thread ------------------------------------------------------

    void process(float* const* channels, int numChannels, int numFrames) {
        if (!prepared_ || channels == nullptr || numChannels <= 0 || numFrames <= 0) return;
        float* x0 = channels[0];
        if (x0 == nullptr) return;
        const int nc = std::min(numChannels, nch_);

        const float octMul = std::pow(2.0f, (float) std::lround(params_.octave));
        const float vibDepth = params_.vibrato * (70.0f / 1200.0f);  // 1 -> +-70 cents
        const float breath   = params_.breath;
        // No aspiration FLOOR: breath 0 must mean a pure (robotic) glottal pulse.
        // A floor put a broadband noise bed ~10-17 dB above the F3 valley, which
        // audibly veiled the third formant (measured).
        const float aspGain  = 0.60f * breath;
        const float pulseAmt = 1.0f - 0.35f * breath;
        const float ampAtk   = onePoleCoef(params_.attackMs);
        const float ampRel   = onePoleCoef(90.0f);
        const int   glideN   = (int) (params_.portamentoMs * 0.001 * sr_);

        for (int i = 0; i < numFrames; ++i)
        {
            float in = x0[i];
            if (!std::isfinite(in)) in = 0.0f;

            // ---- analysis feeds ------------------------------------------
            const float fe = fastEnv_.process(in);            // amp gate
            const float se = slowEnv_.process(in);            // amp level
            const float hf = hfEnv_.process(onsetHp_.process(in)); // onset feature

            const float lp = decLp_.process(in);
            if (++decPhase_ >= dec_)
            {
                decPhase_ = 0;
                ring_[(size_t) (ringPos_ & ringMask_)] = lp;
                ++ringPos_;
                if (++hopCount_ >= hopDec_) { hopCount_ = 0; trackPitch(); }
            }

            // ---- onset: a PICK -> new note HEAD --------------------------
            const float hfDel = feDelay_[(size_t) feDelayPos_];
            feDelay_[(size_t) feDelayPos_] = hf;
            if (++feDelayPos_ >= feDelayLen_) feDelayPos_ = 0;
            if (cooldown_ > 0) --cooldown_;
            if (legatoLock_ > 0) --legatoLock_;
            if (sinceOnset_ < (1 << 28)) ++sinceOnset_;

            if (cooldown_ == 0 && fe > 2.0f * kGateLin
                && hf > kOnsetRatio * (hfDel + kHfFloor))
            {
                cooldown_   = refractory_;
                legatoLock_ = (int) (sr_ * 0.090); // legato can't fire on the pick tail
                sinceOnset_ = 0;
                pitchArmed_ = true;                 // take the next confident f0 raw
                pendingLegato_ = false;
                // The pick's own pitch jump must not later read as a legato.
                for (int k = 0; k < kRefRing; ++k) refRing_[(size_t) k] = 0.0f;
                advanceSyllable(/*legato=*/false, glideN);
            }

            // ---- voice ----------------------------------------------------
            const float voice = renderVoice(fe, se, octMul, vibDepth, aspGain,
                                            pulseAmt, ampAtk, ampRel);

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
    }

    int latencySamples() const { return 0; } // dry path is untouched

    // Test-only: the two articulation coefficients the rate-derivation sweep
    // originally missed, so a gate case can assert they MOVE with the rate.
    float shimmerCoefForTest() const { return shimmerC_; }
    float jitterCoefForTest()  const { return jitterC_; }

private:
    // =======================================================================
    // Tables
    // =======================================================================
    static constexpr float kGateLin    = 0.005f;  // ~-46 dB input gate
    static constexpr float kMakeup     = 2.2f;  // post-normalisation staging
    static constexpr int   kCoeffInterval = 16;
    static constexpr float kOnsetRatio = 1.8f;    // HF env jump that counts as a pick
    static constexpr float kHfFloor    = 1e-4f;   // keeps the ratio finite in silence
    static constexpr int   kRefRing    = 24;      // pitch history for legato detection
    static constexpr int   kRefLag     = 10;      // ~40 ms back at the 4 ms hop
    // The nasal pole and the nasal zero MUST share this bandwidth: that is the only
    // thing that makes them cancel exactly when the zero slides onto the pole.
    static constexpr double kNasalBw   = 200.0;

    // Vowel formants — young female Japanese singer (a i u e o). Japanese /u/
    // is the compressed unrounded [ɯ], so its F2 sits far above English /u/
    // (Yazawa & Kondo 2019 measure Japanese female /a/ F2 ~1530 Hz, which this
    // table matches). These are the NEUTRAL targets; `character` scales them.
    static constexpr float kVowF[4][kNumVowels] = {
        {  850.f,  330.f,  380.f,  520.f,  470.f },  // F1
        { 1530.f, 2790.f, 1350.f, 2350.f,  850.f },  // F2
        { 2900.f, 3400.f, 2950.f, 3050.f, 2900.f },  // F3
        { 4200.f, 4300.f, 4150.f, 4200.f, 4100.f },  // F4
    };
    static constexpr float kVowB[4][kNumVowels] = {
        {   80.f,   55.f,   60.f,   70.f,   65.f },
        {  100.f,   95.f,   95.f,   95.f,   90.f },
        {  140.f,  150.f,  150.f,  145.f,  140.f },
        {  220.f,  220.f,  220.f,  220.f,  220.f },
    };

    enum CKind { K_None = 0, K_Stop, K_Affricate, K_Fricative, K_Nasal, K_Approx, K_Aspirate };

    struct ConsInfo {
        int   kind;
        int   voiced;      // 1 = voicing runs through the closure (the "voice bar")
        float closureMs;   // silence (stop) / murmur (nasal) before the release
        float noiseMs;     // burst (stop) or frication (fricative/affricate) length
        // TWO noise poles, not one. A single 2-pole bandpass is a whistle; every
        // real sibilant has two spectral peaks (Oxford, Acoustic structure of
        // consonants: /s/ peaks at 4.5 and 7.5 kHz, /S/ at ~2.5 and 4 kHz), and
        // that pair IS what separates /s/ from /sh/.
        float noiseHz,  noiseQ;
        float noiseHz2, noiseQ2;
        float noiseGain;
        float aspMs;       // VOT: aspiration AFTER the burst, before voicing (Klatt AH)
        float f1loc, f2loc, f3loc;  // Delattre loci the vowel glides FROM
        float transMs;     // voiced diphone transition (NSX-1 default: a fixed 50 ms)
        float nasal;       // 0..1 velum opening
        float nzHz;        // the nasal ANTIFORMANT (Klatt FNZ) for this place
        int   velar;       // 1 = the F2 locus depends on the vowel (the velar pinch)
        int   syllabic;    // 1 = moraic nasal /N/: a hum that NEVER opens to a vowel
    };

    // Japanese moraic consonants.
    //
    // Loci (Delattre): labial F2 ~750 Hz, alveolar ~1750, palatal ~2400; F1 is low
    // (~250 Hz) for every closure. VELARS have no single locus — F2 rides up to
    // meet F3 before a front vowel and drops before a back one (the "velar pinch"),
    // so k/g carry `velar` and pick their locus per vowel (locusF2 below).
    //
    // aspMs is the VOT. Japanese voiceless stops run ~30-60 ms of aspiration
    // between the burst and voicing; the voiced ones ~10. This is not decoration:
    // the formant transition SWEEPS THROUGH the aspiration, which is what makes a
    // voiceless stop's place of articulation audible at all.
    //
    // nzHz is the nasal antiformant: labial /m/ ~1000 Hz, alveolar /n/ ~1700,
    // palatal ~2200, moraic /N/ ~1800 (Hoole; Fujimura, Analysis of Nasal
    // Consonants, JASA 1962 — the ZERO is what identifies the nasal's place).
    static const ConsInfo& cons(int c) {
        static const ConsInfo t[kNumCons] = {
            //           kind        vc  clos  nois   nHz1    Q1   nHz2    Q2   gain  asp   f1l    f2l     f3l   trans nas   nzHz  vel syl
            /* none */ { K_None,      1,  0.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f,   0.f,    0.f,    0.f, 25.f, 0.f,   0.f, 0, 0 },
            /* k    */ { K_Stop,      0, 45.f, 12.f, 2000.f, 3.0f, 3000.f, 3.0f, 0.85f, 45.f, 260.f, 2200.f, 2800.f, 50.f, 0.f,   0.f, 1, 0 },
            /* g    */ { K_Stop,      1, 25.f,  8.f, 2000.f, 3.0f, 3000.f, 3.0f, 0.35f, 10.f, 260.f, 2200.f, 2800.f, 55.f, 0.f,   0.f, 1, 0 },
            /* s    */ { K_Fricative, 0,  0.f, 90.f, 4700.f, 2.0f, 7200.f, 3.0f, 0.90f,  0.f, 250.f, 1750.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* sh   */ { K_Fricative, 0,  0.f, 95.f, 2400.f, 2.5f, 3900.f, 2.5f, 0.95f,  0.f, 280.f, 2400.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* z    */ { K_Fricative, 1,  0.f, 65.f, 4600.f, 2.0f, 7000.f, 3.0f, 0.40f,  0.f, 250.f, 1750.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* j    */ { K_Affricate, 1, 18.f, 45.f, 2400.f, 2.5f, 3900.f, 2.5f, 0.50f,  0.f, 280.f, 2400.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* t    */ { K_Stop,      0, 40.f, 10.f, 4000.f, 2.0f, 6500.f, 2.0f, 1.00f, 35.f, 250.f, 1750.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* ts   */ { K_Affricate, 0, 25.f, 55.f, 4700.f, 2.0f, 7200.f, 3.0f, 0.95f,  0.f, 250.f, 1750.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* ch   */ { K_Affricate, 0, 25.f, 60.f, 2400.f, 2.5f, 3900.f, 2.5f, 0.95f,  0.f, 280.f, 2400.f, 2900.f, 50.f, 0.f,   0.f, 0, 0 },
            /* d    */ { K_Stop,      1, 22.f,  7.f, 4000.f, 2.0f, 6500.f, 2.0f, 0.35f,  8.f, 250.f, 1750.f, 2900.f, 55.f, 0.f,   0.f, 0, 0 },
            /* n    */ { K_Nasal,     1, 55.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 250.f, 1700.f, 2600.f, 45.f, 1.f, 1700.f, 0, 0 },
            /* ny   */ { K_Nasal,     1, 45.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 250.f, 2300.f, 2900.f, 45.f, 1.f, 2200.f, 0, 0 },
            /* h    */ { K_Aspirate,  0,  0.f, 60.f, 1000.f, 0.5f, 3800.f, 0.6f, 1.30f,  0.f,   0.f,    0.f,    0.f, 30.f, 0.f,   0.f, 0, 0 },
            /* f    */ { K_Fricative, 0,  0.f, 70.f, 1800.f, 0.8f, 5500.f, 0.8f, 0.50f,  0.f, 250.f,  750.f, 2300.f, 50.f, 0.f,   0.f, 0, 0 },
            /* b    */ { K_Stop,      1, 25.f,  8.f,  700.f, 1.0f, 1600.f, 0.8f, 0.30f, 10.f, 250.f,  750.f, 2300.f, 60.f, 0.f,   0.f, 0, 0 },
            /* p    */ { K_Stop,      0, 45.f, 10.f,  700.f, 1.0f, 1600.f, 0.8f, 0.75f, 40.f, 250.f,  750.f, 2300.f, 60.f, 0.f,   0.f, 0, 0 },
            /* m    */ { K_Nasal,     1, 60.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 250.f,  900.f, 2200.f, 50.f, 1.f, 1000.f, 0, 0 },
            /* y    */ { K_Approx,    1,  0.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 280.f, 2300.f, 3000.f, 65.f, 0.f,   0.f, 0, 0 },
            /* r    */ { K_Approx,    1, 15.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 300.f, 1600.f, 2600.f, 30.f, 0.f,   0.f, 0, 0 },
            /* w    */ { K_Approx,    1,  0.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 300.f,  700.f, 2300.f, 65.f, 0.f,   0.f, 0, 0 },
            /* N    */ { K_Nasal,     1,110.f,  0.f,    0.f, 1.0f,    0.f, 1.0f, 0.00f,  0.f, 250.f, 1500.f, 2400.f, 40.f, 1.f, 1800.f, 0, 1 },
        };
        return t[(c < 0 || c >= kNumCons) ? 0 : c];
    }

    // The velar pinch. /k/ and /g/ have no single F2 locus: before /i/ and /e/ the
    // tongue body is front and F2 climbs toward F3; before /u/ and /o/ it is back
    // and F2 dives. A fixed 2200 Hz locus makes `ku` and `ko` sound like `ki`.
    static float locusF2(const ConsInfo& ci, int vowel) {
        if (ci.velar == 0) return ci.f2loc;
        switch (vowel) {
            case V_I: return 2900.f;
            case V_E: return 2500.f;
            case V_A: return 1900.f;
            case V_U: return 1400.f;
            default:  return 1250.f;   // V_O
        }
    }

    // The hardware's 11 lyric patterns, in its own knob order. Slots 6/7/8 are
    // the user Phrase slots (handled in advanceSyllable/nextUserSyllable).
    static const Syllable* presetTable(int pattern, int& lenOut) {
        static const Syllable ahh[]  = { { C_None, V_A } };
        static const Syllable lahh[] = { { C_R,    V_A } };  // Japanese ra ~ [ɾa]
        static const Syllable looh[] = { { C_R,    V_U } };
        static const Syllable pahh[] = { { C_P,    V_A } };
        static const Syllable nyan[] = { { C_NY,   V_A },  { C_NN, V_U } };  // "nya-n"
        static const Syllable scat[] = { { C_D,    V_U },  { C_B,  V_I },
                                         { C_D,    V_A },  { C_B,  V_A },
                                         { C_D,    V_U },  { C_W,  V_A } };
        // "Random" = a long pseudo-word chain of legal Japanese moras; the
        // hardware chains dictionary words, we chain moras (same effect: you
        // never hear the same syllable twice in a row).
        static const Syllable rnd1[] = {
            { C_M, V_I }, { C_K, V_U }, { C_H, V_A }, { C_TS, V_U }, { C_N, V_E },
            { C_S, V_O }, { C_R, V_A }, { C_Y, V_U }, { C_K,  V_I }, { C_T, V_O },
            { C_NY,V_A }, { C_Z, V_E }, { C_B, V_U }, { C_SH, V_I }, { C_M, V_A },
            { C_G, V_O },
        };
        static const Syllable rnd2[] = {
            { C_CH, V_I }, { C_W, V_A }, { C_D, V_E }, { C_N,  V_O }, { C_P, V_U },
            { C_R,  V_I }, { C_G, V_A }, { C_M, V_U }, { C_SH, V_E }, { C_T, V_A },
            { C_J,  V_O }, { C_H, V_I }, { C_K, V_A }, { C_S,  V_U }, { C_B, V_E },
            { C_NN, V_U },
        };
        switch (pattern) {
            case 0:  lenOut = 1;  return ahh;
            case 1:  lenOut = 1;  return lahh;
            case 2:  lenOut = 1;  return looh;
            case 3:  lenOut = 1;  return pahh;
            case 4:  lenOut = 2;  return nyan;
            case 5:  lenOut = 6;  return scat;
            case 9:  lenOut = 16; return rnd1;
            case 10: lenOut = 16; return rnd2;
            default: lenOut = 1;  return ahh;   // empty Phrase slot -> Ahh
        }
    }

    // =======================================================================
    // Klatt cascade resonator / antiresonator (Klatt 1980, eqs. for R and AR).
    //
    // DOUBLE STATE, not just double coefficients. These two are used as an
    // EXACTLY CANCELLING PAIR (see the nasal note below): the antiresonator's
    // leading coefficient is 1/a of the resonator's, which at a 280 Hz nasal pole
    // is ~700. Float state would push ~-94 dBFS of quantisation noise through
    // that gain; double state costs nothing on x64 and makes the cancellation
    // clean.
    // =======================================================================
    struct Res {
        double a = 1.0, b = 0.0, c = 0.0;
        double y1 = 0.0, y2 = 0.0;
        void reset() { y1 = y2 = 0.0; }
        void set(double sr, double f, double bw) {
            const double r = std::exp(-3.14159265358979 * bw / sr);
            c = -(r * r);
            b = 2.0 * r * std::cos(2.0 * 3.14159265358979 * f / sr);
            a = 1.0 - b - c;
        }
        float process(float x) {
            const double y = a * (double) x + b * y1 + c * y2;
            y2 = y1; y1 = y;
            return (float) y;
        }
    };
    // H_anti(z) = 1 / H_res(z) for the SAME f and bw. Unity at DC — and rising at
    // +12 dB/oct above f, FOREVER. On its own it is not a notch, it is a treble
    // ramp with a dip in it. Klatt therefore never uses one alone.
    struct AntiRes {
        double a = 1.0, b = 0.0, c = 0.0;
        double x1 = 0.0, x2 = 0.0;
        void reset() { x1 = x2 = 0.0; }
        void set(double sr, double f, double bw) {
            const double r  = std::exp(-3.14159265358979 * bw / sr);
            const double cc = -(r * r);
            const double bb = 2.0 * r * std::cos(2.0 * 3.14159265358979 * f / sr);
            const double aa = 1.0 - bb - cc;
            a = 1.0 / ((std::fabs(aa) > 1e-12) ? aa : 1e-12);
            b = -bb * a;
            c = -cc * a;
        }
        float process(float x) {
            const double y = a * (double) x + b * x1 + c * x2;
            x2 = x1; x1 = (double) x;
            return (float) y;
        }
    };

    // =======================================================================
    // Sequencer / articulation
    // =======================================================================
    enum Seg { SegIdle = 0, SegClosure, SegBurst, SegAspirate, SegTransition, SegVowel };

    // A new note. legato = pitch changed with no pick (AEM "joint": no head,
    // formants + pitch GLIDE). Pick = full head (closure + burst), pitch jumps.
    //
    // The table is resolved HERE rather than once at the top of the block: a
    // user Phrase slot is shared with the message thread, and the only safe way
    // to read it is to take the (count, syllable) pair together, at the moment
    // it is used. Hoisting a raw pointer to the block top is what made the old
    // double buffer ABA-unsafe.
    void advanceSyllable(bool legato, int glideN) {
        const int prev = sylIndex_.load(std::memory_order_relaxed);
        const int pat  = (int) std::lround(params_.pattern);
        Syllable s;
        int idx = 0;
        bool have = false;
        if (pat >= 6 && pat <= 8)
        {
            switch (nextUserSyllable(pat - 6, prev, idx, s))
            {
                case PhraseRead::Ok:   have = true; break;
                // A setPhrase that never settled inside the bounded retry: sing
                // the current mora one note longer rather than wait or guess.
                case PhraseRead::Busy: return;
                case PhraseRead::Empty: break;   // -> preset fallback ("Ahh")
            }
        }
        if (!have)
        {
            int tableLen = 0;
            const Syllable* table = presetTable(pat, tableLen);
            if (table == nullptr || tableLen <= 0) return;
            idx = (prev + 1) % tableLen;
            s   = table[idx];
        }
        sylIndex_.store(idx, std::memory_order_relaxed);
        curSyl_.store(packSyllable(s), std::memory_order_relaxed);
        curCons_  = s.cons;
        curVowel_ = s.vowel;
        const ConsInfo& ci = cons(curCons_);

        vibLfo_.setPhase(0.0f);   // the NSX-1 vibrato delay restarts per note
        shimmer_ = 0.0f;

        // ---- where the formants START -------------------------------------
        const float scale = formantScale();
        if (ci.kind == K_Aspirate)
        {
            // /h/ is "acoustically a voiceless vowel" (Coleman): the aspiration is
            // excited at the GLOTTIS and shaped by the FOLLOWING VOWEL's own
            // formants. It has no locus of its own — parking it on a fixed 1500 Hz
            // band (what this used to do) makes /ha/ /hi/ /hu/ the same sound, and
            // measured 1 dB of HF over a plain vowel: no /h/ at all.
            for (int i = 0; i < 4; ++i) fCur_[i] = kVowF[i][curVowel_] * scale;
        }
        else if (ci.kind != K_None && !(legato && ci.kind == K_Stop))
        {
            fCur_[0] = ci.f1loc * scale;
            fCur_[1] = locusF2(ci, curVowel_) * scale;
            fCur_[2] = ci.f3loc * scale;
            fCur_[3] = kVowF[3][curVowel_] * scale;   // F4 barely articulates
        }

        // ---- and when they MOVE (this was the intelligibility bug) ---------
        // The locus->vowel glide IS the consonant cue (Delattre). It used to be
        // armed HERE, at the syllable start — so the whole 50 ms sweep ran while
        // the tract was CLOSED and the voice was muted, and by the time you could
        // hear anything the formants had already arrived. Measured: 69-177 Hz of
        // audible dF2 for b/d/g/n/r where 550-900 Hz is required. The glide is now
        // armed at the RELEASE (enterBurst / enterTransition), never here.
        formLeft_ = 0;
        for (int i = 0; i < 4; ++i) fTgt_[i] = fCur_[i];

        if (legato)
        {
            // JOINT: no head. Formants + pitch glide; the velum stays shut (you
            // cannot make a nasal closure without stopping the vowel).
            enterTransition(ci);
            fricLeft_ = 0; aspLeft_ = 0; fricTarget_ = 0.0f; aspTarget_ = 0.0f;
            setFormantTargets(ci.transMs);   // a joint ALWAYS glides
            beginGlide(glideN);
        }
        else
        {
            // HEAD: closure -> burst -> aspiration (VOT) -> transition -> vowel.
            if      (ci.closureMs > 0.0f) enterClosure(ci);
            else if (ci.noiseMs   > 0.0f) enterBurst(ci);
            else if (ci.aspMs     > 0.0f) enterAspiration(ci);
            else                          enterTransition(ci);
            f0GlideLeft_ = 0;   // a re-picked note attacks at pitch, no portamento
        }
    }

    // ---- the segments ------------------------------------------------------
    void enterClosure(const ConsInfo& ci) {
        seg_ = SegClosure;
        segLeft_ = msToSamples(ci.closureMs);
        voicedTarget_ = (ci.kind == K_Nasal) ? 0.75f
                      : (ci.voiced != 0 ? 0.22f : 0.0f);   // the voice bar
        // A closed tract is a LOSSY tract: the oral formants are heavily damped.
        bwMulTarget_ = 2.0f;
        nasalTarget_ = ci.nasal;
        nzRawHz_     = (ci.nasal > 0.0f) ? ci.nzHz * formantScale() : nasalPoleHz();
    }

    void enterBurst(const ConsInfo& ci) {
        startFrication(ci);
        seg_ = SegBurst;
        segLeft_ = fricLen_;
        voicedTarget_ = (ci.kind == K_Stop) ? 0.0f : ((ci.voiced != 0) ? 0.5f : 0.0f);
        bwMulTarget_ = 1.0f;
        // THE RELEASE. For a stop the formant sweep starts HERE and runs across the
        // burst AND the aspiration into the vowel — so the place of articulation is
        // audible in the aspiration even though there is no voicing yet.
        if (ci.kind == K_Stop) setFormantTargets(ci.noiseMs + ci.aspMs + ci.transMs);
    }

    void enterAspiration(const ConsInfo& ci) {
        aspLeft_ = msToSamples(ci.aspMs);
        if (aspLeft_ < 1) { enterTransition(ci); return; }
        seg_ = SegAspirate;
        segLeft_ = aspLeft_;
        // Klatt AH: turbulence at the glottis, into the CASCADE, so the moving
        // formants shape it. Voiced stops resume voicing almost at once (short VOT).
        aspTarget_ = (ci.voiced != 0) ? 0.20f : 0.55f;
        voicedTarget_ = (ci.voiced != 0) ? 0.35f : 0.0f;
        bwMulTarget_ = 1.0f;
    }

    void enterTransition(const ConsInfo& ci) {
        seg_ = SegTransition;
        segLeft_ = msToSamples(ci.transMs);
        voicedTarget_ = 1.0f;
        aspTarget_ = 0.0f;
        if (ci.syllabic != 0)
        {
            // A moraic /N/ is a HUM. It must not open into a vowel — that release
            // ("...n-uuuu") is the blob at the end of "nyan".
            bwMulTarget_ = 1.0f;
            nasalTarget_ = ci.nasal;
            nzRawHz_     = ci.nzHz * formantScale();
            return;                       // no glide: the tract stays nasal
        }
        bwMulTarget_ = 1.0f;
        nasalTarget_ = 0.0f;              // the velum closes as the vowel opens
        nzRawHz_     = nasalPoleHz();
        if (ci.kind != K_Stop) setFormantTargets(ci.transMs);  // stops armed at the burst
    }

    void enterVowel(const ConsInfo& ci) {
        seg_ = SegVowel;
        segLeft_ = 1 << 28;
        voicedTarget_ = 1.0f;
        aspTarget_ = 0.0f;
        if (ci.syllabic == 0) { nasalTarget_ = 0.0f; nzRawHz_ = nasalPoleHz(); }
    }

    void startFrication(const ConsInfo& ci) {
        fricLen_  = std::max(1, msToSamples(ci.noiseMs));
        fricLeft_ = fricLen_;
        fricGain_ = ci.noiseGain;
        fricSustain_  = (ci.kind == K_Fricative || ci.kind == K_Affricate || ci.kind == K_Aspirate);
        fricAspirate_ = (ci.kind == K_Aspirate);
        fricBp_.setBandpass(sr_, clampd(ci.noiseHz, 200.0, 0.45 * sr_), ci.noiseQ);
        const double h2 = (ci.noiseHz2 > 0.0f) ? (double) ci.noiseHz2 : (double) ci.noiseHz;
        const double q2 = (ci.noiseQ2  > 0.0f) ? (double) ci.noiseQ2  : (double) ci.noiseQ;
        fricBp2_.setBandpass(sr_, clampd(h2, 200.0, 0.45 * sr_), q2);
        // Klatt: AF ramps IN over ~90 ms for a fricative and hits full ABRUPTLY for
        // a plosive burst. The old envelope did the opposite (it DECAYED across the
        // fricative from 1.0 to 0.35, then cut to zero — backwards, and it clicked).
        //
        // AFFRICATES RISE FAST — that is the /s/-vs-/ts/ cue beyond the closure.
        // Howell & Rosen (JASA 1983): fricative rise ~76 ms, affricate ~33 ms.
        // Sharing one 16 ms attack measured 70 ms vs 48 ms of 10-90% rise (T16) —
        // no contrast; a 3 ms affricate attack restores the ~2:1 the ear needs.
        const bool affric = (ci.kind == K_Affricate);
        fricAtkC_ = onePoleCoef(fricSustain_ ? (affric ? 3.0f : 16.0f) : 0.6f);
        fricRelC_ = onePoleCoef(fricSustain_ ? 10.0f : 3.0f);
        fricTarget_ = fricGain_;
    }

    void setFormantTargets(float transMs) {
        const float scale = formantScale();
        const int n = std::max(1, msToSamples(transMs));
        formLeft_ = n;
        const float inv = 1.0f / (float) n;
        for (int i = 0; i < 4; ++i)
        {
            fTgt_[i] = kVowF[i][curVowel_] * scale;
            fInc_[i] = (fTgt_[i] - fCur_[i]) * inv;
        }
    }

    // Klatt's nasal POLE (FNP ~300 Hz). It is the half that was missing.
    float nasalPoleHz() const { return 280.0f * formantScale(); }

    void beginGlide(int glideN) {
        if (glideN <= 0 || f0Cur_ <= 20.0f) { f0GlideLeft_ = 0; return; }
        f0GlideLeft_ = glideN;
        f0Glide_ = (f0Target_ - f0Cur_) / (float) glideN;
    }

    int msToSamples(float ms) const { return (int) (ms * 0.001f * (float) sr_); }
    /// The character multiplier, read from the SMOOTHER. It moves F1..F4, the
    /// nasal pole/zero and cascadeNorm_ -- the per-sample output scalar at the
    /// end of the cascade -- so a raw read stepped the level as well as the
    /// timbre. updateFormantCoeffs()'s own guard already compares this, so the
    /// coefficients follow the ramp with no further change.
    float formantScale() const { return 0.88f + 0.40f * charSmooth_.current(); }
    static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }
    float onePoleCoef(float ms) const {
        const float t = std::max(0.1f, ms) * 0.001f;
        return 1.0f - std::exp(-1.0f / (t * (float) sr_));
    }

    void applyParams() {
        vibLfo_.setFrequency(params_.vibratoRateHz);
        srcLp_.setLowpass(sr_, 1200.0 + 3000.0 * (1.0 - (double) params_.breath));
        presence_.setHighShelf(sr_, 3200.0, 0.707, -3.0 + 9.0 * (double) params_.character);
        lastScale_ = -1.0f;   // force a formant coefficient refresh
        // Turning the pattern knob selects a NEW phrase, and a new phrase starts
        // at its first mora — not wherever the previous pattern happened to stop.
        const int pat = (int) std::lround(params_.pattern);
        // Publish it for setPhrase(), which runs on the MESSAGE thread while
        // params_ is written by setParams() on the AUDIO thread (the adapter
        // routes it through ParamMailbox::applyPending). Reading the plain float
        // from there was a formal data race and a stale read that could skip the
        // phrase restart; every other cross-thread field in this class is an
        // atomic, and now so is this one.
        curPattern_.store(pat, std::memory_order_relaxed);
        if (pat != lastPattern_) {
            lastPattern_ = pat;
            sylIndex_.store(-1, std::memory_order_relaxed);
        }
    }

    float nextNoise() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return ((float) (rng_ & 0xFFFFFFu) * (1.0f / 8388608.0f)) - 1.0f;
    }

    void updateFormantCoeffs(bool force) {
        // PITCH-ADAPTIVE F1 (soprano formant tuning, Sundberg). Above ~F1 the
        // fundamental no longer excites the first formant at all — there is no
        // harmonic under it — and every vowel collapses toward the same dull
        // spectrum (measured, T14: at a sung 494 Hz, /a i e o/ all classified as
        // /u/). A real singer raises F1 to meet the fundamental; so do we: F1 is
        // floored at ~1.05*f0 (capped — beyond ~1 kHz identity is carried by F2
        // alone and chasing further would just detune /a/). Inactive below a sung
        // ~340 Hz, so the whole guitar's normal range is untouched.
#ifdef DCDSP_MIKU_NO_F1FLOOR   // ablation guard for the measurement harness
        const float f1Floor = 0.0f;
#else
        const float f1Floor = (sungHz_ > 20.0f) ? std::min(1.05f * sungHz_, 1050.0f) : 0.0f;
#endif
        // Cheap change detector over ALL four formants, the bandwidth multiplier
        // and the nasal zero, so a pure F2 move (e.g. /o/ -> /u/) or a velum
        // movement still refreshes the coefficients.
        const float sig = fCur_[0] + 3.0f * fCur_[1] + 7.0f * fCur_[2] + 11.0f * fCur_[3]
                        + 137.0f * bwMul_ + 19.0f * nzHz_ + 211.0f * nasalAmt_
                        + 0.13f * f1Floor;
        if (!force && std::fabs(sig - lastSig_) < 0.5f && lastScale_ == formantScale())
            return;
        lastSig_ = sig;
        lastScale_ = formantScale();
        const float nyq = (float) (0.45 * sr_);
        // Nasals are heavily damped (Fujimura: "relatively high damping factors"),
        // and so is a closed oral tract. Both widen the oral formants.
        const float bwMul = bwMul_ * (1.0f + 1.6f * nasalAmt_);
        for (int i = 0; i < 4; ++i) bCur_[i] = kVowB[i][curVowel_] * bwMul;
        // Cascade GAIN NORMALISATION (a pure SCALAR — it moves no formant and
        // changes no spectral shape, only the level).
        //
        // A Klatt resonator has ~+16..+30 dB of gain at its own resonance, so an
        // un-normalised 4-deep cascade swings 1.2..7.4 peak depending on the
        // vowel and the pitch (measured). That (a) kept the output soft-clipper
        // permanently saturated — its distortion products sat 10-17 dB ABOVE the
        // F3 valley and audibly veiled the third formant — and (b) made /a/ ~6x
        // louder than /i/.
        //
        // The normaliser is F1's peak gain, NOT the product of all four: the
        // resonances peak at different frequencies and the source's energy is
        // concentrated in the low harmonics, so F1 alone sets the level. Dividing
        // by it lands every vowel at 0.43..0.59 peak (measured); dividing by the
        // product would over-normalise by ~10^4 and leave the voice inaudible.
        float fEff0 = clampf(std::max(fCur_[0], f1Floor), 120.0f, nyq);
        for (int i = 0; i < 4; ++i)
        {
            const float f  = (i == 0) ? fEff0 : clampf(fCur_[i], 120.0f, nyq);
            const float bw = std::max(30.0f, bCur_[i]);
            form_[(size_t) i].set(sr_, f, bw);
        }
        // The nasal pair is set BEFORE the gain normaliser below, which reads its
        // fresh coefficients (see the pair's own comment block further down).
        {
            const double fpair = clampd((double) nasalPoleHz(), 150.0, 0.45 * sr_);
            nasalPole_.set(sr_, fpair, kNasalBw);
            nasalZero_.set(sr_, clampd((double) nzHz_, 150.0, 0.45 * sr_), kNasalBw);
        }
        {
            // NORMALISE BY THE ENVELOPE'S TRUE PEAK, WHICH THE NASAL PAIR CAN MOVE.
            // For a plain vowel the peak is F1's own resonance (the original rule:
            // divide by F1's peak gain — a pure scalar, no formant moves). But when
            // the velum opens, the ~300 Hz nasal POLE is no longer cancelled by its
            // zero, and the composite envelope can peak AT THE POLE, below F1 —
            // measured: the sustained /N/ hum came out ~11 dB HOTTER than the
            // vowels once the output stage was restaged clean (the old saturating
            // stage had been quietly limiting the excess, which is why nobody
            // heard it as a level bug). So evaluate |H| at BOTH candidates — F1
            // and the pole — and divide by the larger. Not nasalised, the pair is
            // exactly 1 at every frequency, the pole candidate loses, and vowels
            // keep the original F1 normalisation bit for bit.
            auto resMag = [&](const Res& R, double th) {
                const double re = 1.0 - R.b * std::cos(th) - R.c * std::cos(2.0 * th);
                const double im =       R.b * std::sin(th) + R.c * std::sin(2.0 * th);
                return R.a / std::max(1e-9, std::sqrt(re * re + im * im));
            };
            auto antiMag = [&](const AntiRes& A, double th) {
                const double re = A.a + A.b * std::cos(th) + A.c * std::cos(2.0 * th);
                const double im = -A.b * std::sin(th) - A.c * std::sin(2.0 * th);
                return std::sqrt(re * re + im * im);
            };
            const double thF1 = 6.283185307179586 * (double) fEff0 / sr_;
            const double fp   = clampd((double) nasalPoleHz(), 150.0, 0.45 * sr_);
            const double thFp = 6.283185307179586 * fp / sr_;
            const double gF1  = resMag(form_[0], thF1)
                              * resMag(nasalPole_, thF1) * antiMag(nasalZero_, thF1);
            const double gFp  = resMag(form_[0], thFp)
                              * resMag(nasalPole_, thFp) * antiMag(nasalZero_, thFp);
            const double g0 = std::max(gF1, gFp);
            cascadeNorm_ = (std::isfinite(g0) && g0 > 1e-6) ? (float) (1.0 / g0) : 1.0f;
            cascadeNorm_ = clampf(cascadeNorm_, 1e-3f, 10.0f);
            // Loudness continuity for the F1 pitch floor: with F1 tuned onto the
            // fundamental, the FULL H1 rides the (unity-normalised) envelope peak,
            // where a low-pitch vowel's peak sits on a mid harmonic the source has
            // already rolled off — unchecked, high notes measured up to ~14 dB
            // hotter than the same vowel an octave down. Trim as the floor rises.
            if (f1Floor > fCur_[0]) {
                const float rise = std::min(1.0f, (f1Floor - fCur_[0]) / std::max(120.0f, fCur_[0]));
                cascadeNorm_ *= 1.0f - 0.55f * rise;
            }
        }
        // ---- the nasal branch: a POLE and a ZERO, as a PAIR --------------------
        // This used to be a lone antiresonator, and an antiresonator is the INVERSE
        // of a resonator: unity at DC and rising +12 dB/oct above its centre with
        // nothing to stop it. Measured on this very recipe: +21 dB at 2 kHz, +33 dB
        // at 4 kHz, +45 dB at 8 kHz. Every murmur came out 5-8 dB BRIGHTER than a
        // plain vowel when a real nasal murmur is 15-25 dB DARKER -- which is why
        // "n" and "m" did not read as n and m.
        //
        // Klatt 1980 always pairs RNZ with RNP and moves the ZERO onto the POLE
        // when the sound is not nasalised, where they cancel EXACTLY (H_anti = 1/H_res
        // for the same f and bw, so the product is 1 -- which is why both must keep
        // the same bandwidth, kNasalBw). Nasalising just slides the zero away, up to
        // the antiformant of the place (/m/ 1000, /n/ 1700, palatal 2200, /N/ 1800).
        // Below the zero the pole's -12 dB/oct is uncancelled => the dark, damped,
        // low-formant murmur a nasal actually is.
        // (The pair's coefficients are set ABOVE the gain normaliser, which folds
        // their response at F1 into cascadeNorm_.)
    }

    // ---- one sample of the singing voice ----------------------------------
    float renderVoice(float fe, float se, float octMul, float vibDepth,
                      float aspGain, float pulseAmt, float ampAtk, float ampRel)
    {
        // --- segment machine (closure -> burst -> aspiration -> transition -> vowel) ---
        if (segLeft_ > 0) --segLeft_;
        if (segLeft_ <= 0)
        {
            const ConsInfo& ci = cons(curCons_);
            switch (seg_)
            {
                case SegClosure:
                    if      (ci.noiseMs > 0.0f) enterBurst(ci);
                    else if (ci.aspMs   > 0.0f) enterAspiration(ci);
                    else                        enterTransition(ci);
                    break;
                case SegBurst:
                    if (ci.aspMs > 0.0f) enterAspiration(ci);
                    else                 enterTransition(ci);
                    break;
                case SegAspirate:  enterTransition(ci); break;
                case SegTransition: enterVowel(ci);     break;
                default:            enterVowel(ci);     break;
            }
        }

        // --- formant glide (this IS the synthesised diphone) ---
        if (formLeft_ > 0)
        {
            --formLeft_;
            for (int i = 0; i < 4; ++i) fCur_[i] += fInc_[i];
            if (formLeft_ == 0) for (int i = 0; i < 4; ++i) fCur_[i] = fTgt_[i];
        }
        // --- articulator smoothing: the velum and the tract losses are not steps ---
        nasalAmt_ += (nasalTarget_ - nasalAmt_) * nasalC_;
        bwMul_    += (bwMulTarget_ - bwMul_)    * bwC_;
        nzHz_      = nasalPoleHz() + (nzRawHz_ - nasalPoleHz()) * nasalAmt_;
        // The sung fundamental the pitch-adaptive F1 floor keys on: pre-vibrato,
        // pre-jitter, so the floor cannot wobble with the LFO.
        if (f0Cur_ > 20.0f) sungHz_ = clampf(f0Cur_ * octMul, 60.0f, 1400.0f);
        charSmooth_.next();
        if (--coeffCountdown_ <= 0) { coeffCountdown_ = kCoeffInterval; updateFormantCoeffs(false); }

        // --- amplitude: the player's own dynamics, shaped by `attack` ---
        // The envelope follower ripples at 2*f0 (a 12 ms period at E2 is on the
        // order of its own attack time). Feeding that straight into the voice
        // gain amplitude-modulates it and dumps a strong 2*f0 buzz into the
        // output — smooth the LEVEL (not the gate) before using it.
        const float seSm = envLp2_.process(envLp_.process(se));
        const bool  open = (fe > kGateLin);
        gate_ += ((open ? 1.0f : 0.0f) - gate_) * gateC_;
        if (open)
        {
            ampTarget_ = std::min(1.0f, seSm * 4.0f);
            const float k = (ampTarget_ > amp_) ? ampAtk : ampRel;
            amp_ += (ampTarget_ - amp_) * k;
            voiceSilent_ = false;
        }
        else
        {
            // NOTE-OFF — this is the "blob at the end".
            // The player stopped, and a singer would stop too. What used to happen:
            // THREE release times in series (fastEnv 50 ms gate -> slowEnv 150 ms
            // level -> amp 90 ms) meant the voice sat within 3 dB of full level for
            // 100 ms AFTER the input hit digital silence and needed 715 ms to reach
            // -60 dB — 0.7 s of a vibrato-wobbling vowel hanging in the air with the
            // guitar already dead. Note-off is now its OWN fast fade, not the sum of
            // three envelope followers, and the tract is reset once it is inaudible
            // so nothing can ring on.
            ampTarget_ = 0.0f;
            amp_ -= amp_ * noteOffC_;
            if (amp_ < 2e-4f)
            {
                amp_ = 0.0f;
                if (!voiceSilent_) { voiceSilent_ = true; hardReset(); }
            }
        }
        shimmer_ += (0.02f * nextNoise() - shimmer_) * shimmerC_;

        // --- consonant noise (frication / burst) -------------------------
        // Generated at the CONSTRICTION, so it bypasses the tract (Klatt's parallel
        // branch) — except /h/, which is glottal and therefore enters BEFORE the
        // cascade. Two poles, because one is a whistle and not a sibilant.
        if (fricLeft_ > 0) { if (--fricLeft_ == 0) fricTarget_ = 0.0f; }
        float fricSrc = 0.0f, fricBypass = 0.0f;
        {
            const float kf = (fricTarget_ > fricEnv_) ? fricAtkC_ : fricRelC_;
            fricEnv_ += (fricTarget_ - fricEnv_) * kf;
            if (fricEnv_ > 1e-5f)
            {
                const float n  = nextNoise();
                const float nz = fricEnv_ * (fricBp_.process(n) + fricBp2_.process(n));
                if (fricAspirate_) fricSrc    = 1.8f * nz;   // glottal: THROUGH the tract
                else               fricBypass = 2.0f * nz;   // at the lips/teeth: around it
            }
        }

        // --- aspiration / VOT (Klatt AH) ---------------------------------
        // Turbulence at the glottis AFTER a stop's burst and BEFORE voicing. It
        // enters the cascade, so the formant sweep armed at the release is audible
        // in it — that is what carries the place of articulation of a voiceless
        // stop, which has no voicing to carry it.
        if (aspLeft_ > 0) { if (--aspLeft_ == 0) aspTarget_ = 0.0f; }
        aspEnv_ += (aspTarget_ - aspEnv_) * aspC_;
        if (aspEnv_ > 1e-5f) fricSrc += aspEnv_ * aspStopHp_.process(nextNoise());

        // --- pitch: glide (legato joint) or jump (fresh pick) ---
        if (f0GlideLeft_ > 0) { --f0GlideLeft_; f0Cur_ += f0Glide_; if (f0GlideLeft_ == 0) f0Cur_ = f0Target_; }
        else f0Cur_ = f0Target_;

        // --- voicing gate (silent through a voiceless closure/burst) ---
        voiced_ += (voicedTarget_ - voiced_) * voicedC_;

        float src = fricSrc;
        if (f0Cur_ > 20.0f)
        {
            // vibrato: NSX-1 style delay (~300 ms) + ramp-in on a held note. It is
            // gated by `gate_` so a released note does not wobble as it dies.
            const float vlfo = vibLfo_.process();
            const float vibEnv = clamp01(((float) sinceOnset_ - 0.30f * (float) sr_)
                                         / (0.40f * (float) sr_)) * gate_;
            const float vibOct = vibDepth * vibEnv * vlfo;

            // jitter (micro pitch flutter — the "alive" cue)
            if (--jitterLeft_ <= 0) { jitterLeft_ = (int) (sr_ * 0.010); jitterTarget_ = 0.003f * nextNoise(); }
            jitter_ += (jitterTarget_ - jitter_) * jitterC_;

            float hz = f0Cur_ * octMul * std::exp2(vibOct) * (1.0f + jitter_);
            hz = clampf(hz, 60.0f, 1400.0f);

            // glottal source: polyBLEP saw ~ the differentiated glottal pulse
            const float inc = hz / (float) sr_;
            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
            float g = 2.0f * phase_ - 1.0f;
            g -= polyBlep(phase_, inc);
            g = srcLp_.process(g);       // EpR excitation tilt (ESlope/ESlopeDepth)
            g = chest_.process(g);       // EpR chest / excitation resonance
            g *= pulseAmt;

            // aspiration: pitch-synchronous breath, added at the glottis so it
            // is shaped by the tract (the breathy-female cue).
            const float openPhase = 0.55f + 0.45f * std::sin(6.2831853f * phase_);
            g += aspGain * openPhase * aspHp_.process(nextNoise());

            src += voiced_ * g;
        }
        else
        {
            vibLfo_.process();  // keep the LFO running so it never jumps
        }

        // --- vocal tract: nasal pole/zero PAIR, then the Klatt cascade ---
        // POLE FIRST: the zero's leading coefficient is ~1/a of the pole's (~700 at
        // a 280 Hz nasal pole), so pole-then-zero keeps the intermediate signal
        // small and lets the +12 dB/oct of the zero land on a signal the pole has
        // already rolled off. Zero-then-pole would put a +60 dB HF intermediate on
        // the wire for nothing. When nzHz_ == nasalPoleHz() the two cancel exactly
        // and this is a no-op — which is every non-nasal sound.
        float v = src;
        v = nasalPole_.process(v);
        v = nasalZero_.process(v);
        v = form_[0].process(v);
        v = form_[1].process(v);
        v = form_[2].process(v);
        v = form_[3].process(v);
        // A nasal murmur is also QUIETER than the vowel it precedes (the radiating
        // aperture is the nostrils, not the mouth; Fujimura/Hoole put the murmur
        // 10-15 dB under the adjacent vowel). 0.35 was far too gentle and it never
        // showed because the OLD saturating output stage limited the excess: a
        // sustained /N/ hum's H1 lands exactly ON the nasal pole (unity after
        // normalisation) while a vowel's envelope peak sits mid-spectrum where the
        // source has already rolled off ~10 dB — measured, the hum came out 8 dB
        // LOUDER than the vowel. 0.85 lands it ~7 dB below (transient m/n murmurs,
        // whose velum only half-opens in their 45-60 ms, sit higher).
        v *= cascadeNorm_ * (1.0f - 0.85f * nasalAmt_);
        v += fricBypass;

        v = presence_.process(v);
        // The soft-clip is a SAFETY, not a tone stage — drive it at unity and make
        // up the level AFTER it. It used to run at kMakeup (2.2x) INSIDE the tanh,
        // which put the nominal signal at |x| ~ 1.1: audibly saturated. Measured
        // (T13 in tools/miku_measure.cpp): /o/'s F2..F3 valley carried cubic
        // products only 22 dB below F2 — loud enough that LPC reported them as an
        // EXTRA FORMANT at ~1.7 kHz (F2 read +53% off) and /o/ stopped being /o/.
        // The products fall 15.5 dB when the input drops 11.8 dB — the signature
        // of 3rd-order distortion, and the proof the peak was not the synthesis.
        // /a/ hides the same products under F3, which is why only /o/ (and /u/,
        // with its wide empty F2..F3 valley) sounded wrong.
        v = kMakeup * adaaTanh(amp_ * (1.0f + shimmer_) * v, adaa_);
        v = voiceDc_.process(v);
        if (!std::isfinite(v)) { v = 0.0f; hardReset(); }
        return v;
    }

    void hardReset() {
        for (auto& r : form_) r.reset();
        nasalPole_.reset(); nasalZero_.reset();
        chest_.reset(); srcLp_.reset(); aspHp_.reset(); aspStopHp_.reset();
        fricBp_.reset(); fricBp2_.reset(); presence_.reset(); voiceDc_.reset();
        adaa_ = 0.0f; amp_ = 0.0f; voiced_ = 0.0f;
        fricEnv_ = 0.0f; fricTarget_ = 0.0f; aspEnv_ = 0.0f; aspTarget_ = 0.0f;
    }

    static float polyBlep(float t, float dt) {
        if (dt <= 0.0f) return 0.0f;
        if (t < dt)          { const float x = t / dt - 1.0f;        return -(x * x); }
        if (t > 1.0f - dt)   { const float x = (t - 1.0f) / dt + 1.0f; return  (x * x); }
        return 0.0f;
    }

    // =======================================================================
    // Pitch tracking (normalized autocorrelation, coarse->fine, hop-limited)
    // =======================================================================
    void trackPitch() {
        // Right after a pick, a SHORT window locks fast (~13 ms); if it is not
        // confident (low notes need a longer window) fall back to the long one.
        const bool fast = (sinceOnset_ < (int) (sr_ * 0.060));
        float conf = 0.0f;
        float f0 = 0.0f;
        if (fast && ringPos_ >= acShort_ + maxLag_)
        {
            f0 = estimate(acShort_, conf);
            if (conf < 0.72f) f0 = 0.0f;
        }
        if (f0 <= 0.0f)
        {
            if (ringPos_ < acLong_ + maxLag_) return;
            f0 = estimate(acLong_, conf);
            if (conf < 0.55f) f0 = 0.0f;
        }
        publishF0(f0, conf);
    }

    float estimate(int win, float& confOut) {
        confOut = 0.0f;
        const long long newest = ringPos_ - 1;
        float energy0 = 1e-9f;
        for (int k = 0; k < win; ++k)
        {
            const float v = ring_[(size_t) ((newest - k) & ringMask_)];
            energy0 += v * v;
        }
        if (energy0 < 1e-6f) return 0.0f;

        float rMax = 0.0f;
        int   best = 0;
        for (int lag = minLag_; lag <= maxLag_; ++lag)
        {
            const float r = ncf(newest, win, lag, energy0);
            rBuf_[(size_t) lag] = r;
            if (r > rMax) { rMax = r; best = lag; }
        }
        if (rMax < 0.4f || best == 0) return 0.0f;

        // Pick the FIRST peak within 14% of the global max — the global max
        // loves the 2x-period subharmonic (octave-down error on clean plucks).
        const float thresh = 0.86f * rMax;
        int pick = 0;
        for (int lag = minLag_ + 1; lag < maxLag_; ++lag)
        {
            const float r = rBuf_[(size_t) lag];
            if (r >= thresh && r >= rBuf_[(size_t) (lag - 1)] && r >= rBuf_[(size_t) (lag + 1)])
            { pick = lag; break; }
        }
        if (pick == 0) pick = best;

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
        confOut = rBuf_[(size_t) pick];
        if (lag < 1.0f) return 0.0f;
        return (float) (srDec_ / (double) lag);
    }

    float ncf(long long newest, int win, int lag, float energy0) const {
        float acc = 0.0f, energyL = 1e-9f;
        for (int k = 0; k < win; ++k)
        {
            const float a = ring_[(size_t) ((newest - k) & ringMask_)];
            const float b = ring_[(size_t) ((newest - k - lag) & ringMask_)];
            acc += a * b;
            energyL += b * b;
        }
        return acc / std::sqrt(energy0 * energyL);
    }

    void publishF0(float f0, float conf) {
        f0Hist_[0] = f0Hist_[1]; f0Hist_[1] = f0Hist_[2]; f0Hist_[2] = f0;
        const float a = f0Hist_[0], b = f0Hist_[1], c = f0Hist_[2];
        const float med = std::max(std::min(a, b), std::min(std::max(a, b), c));

        if (f0 > 20.0f && pitchArmed_ && conf > 0.6f)
        {
            // First confident estimate after a pick: take it RAW (no median lag,
            // no glide) so the note starts in tune immediately.
            pitchArmed_ = false;
            f0Raw_ = f0; f0Target_ = f0; f0Cur_ = f0; f0GlideLeft_ = 0;
            f0Hist_[0] = f0Hist_[1] = f0Hist_[2] = f0;
            trackedHz_.store(f0Raw_, std::memory_order_relaxed);
            return;
        }
        if (med > 20.0f)
        {
            f0Raw_ = med;
            f0Target_ = med;

            // LEGATO (an AEM "joint"): the pitch JUMPED with no pick, the note
            // is still sounding -> new syllable, pitch + formants GLIDE, no
            // consonant head. The reference is the pitch ~40 ms ago, NOT the
            // previous hop: the tracker's own analysis window makes a hammer-on
            // arrive as several small steps (~3-6% each), so a per-hop ratio
            // test never trips. Comparing across 40 ms also separates a
            // hammer-on (>80 cents inside one window) from a BEND (a slow,
            // continuous slide, which must keep its syllable and just glide).
            const float ref = refRing_[(size_t) ((refPos_ + kRefRing - kRefLag) % kRefRing)];
            refRing_[(size_t) refPos_] = med;
            refPos_ = (refPos_ + 1) % kRefRing;

            if (ref > 20.0f && legatoLock_ == 0 && amp_ > 0.08f)
            {
                const float cents = 1200.0f * std::log2(med / ref);
                if (cents > 80.0f || cents < -80.0f)
                {
                    legatoLock_ = (int) (sr_ * 0.090);
                    sinceOnset_ = 0;
                    pendingLegato_ = true;
                    for (int i = 0; i < kRefRing; ++i) refRing_[(size_t) i] = med;
                }
            }
        }
        trackedHz_.store(f0Raw_, std::memory_order_relaxed);

        if (pendingLegato_)
        {
            pendingLegato_ = false;
            advanceSyllable(/*legato=*/true,
                            (int) (params_.portamentoMs * 0.001f * (float) sr_));
        }
    }

    // =======================================================================
    // State
    // =======================================================================
    MikuStompParams params_{};
    double sr_ = 48000.0;
    static constexpr float kMixRampSeconds = 0.010f;
    SmoothedValue mixSmooth_;
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
    int dec_ = 3, decPhase_ = 0;
    double srDec_ = 16000.0;
    int minLag_ = 12, maxLag_ = 400, acLong_ = 416, acShort_ = 208;
    int hopDec_ = 64, hopCount_ = 0;
    std::vector<float> rBuf_;
    Biquad onsetHp_;
    EnvelopeFollower hfEnv_, fastEnv_, slowEnv_;
    std::vector<float> feDelay_;   // history of the HF onset envelope
    int feDelayLen_ = 1, feDelayPos_ = 0;
    int refractory_ = 0, cooldown_ = 0;
    int sinceOnset_ = 1 << 28, legatoLock_ = 0;
    bool pitchArmed_ = false, pendingLegato_ = false;
    float f0Hist_[3] = { 0, 0, 0 };
    float refRing_[kRefRing] = { 0 };
    int   refPos_ = 0;
    float f0Raw_ = 0.0f;
    std::atomic<float> trackedHz_{ 0.0f };

    // sequencer (3 Phrase slots, seqlock-published — see setPhrase).
    // The syllables are stored PACKED into lock-free atomics rather than as
    // `Syllable`, so a read that races a publish is well-defined (it just loses
    // the seqlock check and retries) instead of a torn two-int struct.
    std::array<std::array<std::atomic<uint32_t>, kMaxSyllables>, kNumSlots> slots_{};
    std::array<std::atomic<int>, kNumSlots>      slotCount_{};
    std::array<std::atomic<uint32_t>, kNumSlots> phraseSeq_{};  // even = stable
    std::atomic<int> sylIndex_{ -1 };   // syllable LAST sung; -1 = none yet (see prepare)
    std::atomic<uint32_t> curSyl_{ packSyllable(Syllable{}) };  // poll-only introspection
    int lastPattern_ = 0;               // pattern-knob change restarts the phrase
    // The effective pattern, published by applyParams() for the message thread.
    std::atomic<int> curPattern_{ 0 };

    // articulation
    int   seg_ = SegIdle, segLeft_ = 0;
    int   curCons_ = C_None, curVowel_ = V_A;
    float fCur_[4] = { 850.f, 1530.f, 2900.f, 4200.f };
    float fTgt_[4] = { 850.f, 1530.f, 2900.f, 4200.f };
    float fInc_[4] = { 0, 0, 0, 0 };
    float bCur_[4] = { 80.f, 100.f, 140.f, 220.f };
    int   formLeft_ = 0;
    float lastSig_ = -1.0f, lastScale_ = -1.0f;
    SmoothedValue charSmooth_;   // params_.character stays the TARGET
    int   coeffCountdown_ = 0;
    float nasalAmt_ = 0.0f, nasalTarget_ = 0.0f;   // the velum, smoothed
    float nzRawHz_ = 280.0f, nzHz_ = 280.0f;       // the nasal ANTIformant
    float bwMul_ = 1.0f, bwMulTarget_ = 1.0f;      // tract losses (closure / nasal)
    float cascadeNorm_ = 1.0f;

    // voice
    std::array<Res, 4> form_{};
    Res     nasalPole_{};      // Klatt RNP — the half that used to be missing
    AntiRes nasalZero_{};      // Klatt RNZ — cancels RNP exactly when not nasalised
    Biquad  chest_, aspHp_, aspStopHp_, fricBp_, fricBp2_, presence_;
    OnePole srcLp_;
    OnePole envLp_, envLp2_;   // de-ripple the amplitude envelope (2*f0 buzz)
    DcBlocker voiceDc_;
    Lfo vibLfo_;
    float phase_ = 0.0f, adaa_ = 0.0f;
    float f0Cur_ = 0.0f, f0Target_ = 0.0f, f0Glide_ = 0.0f;
    float sungHz_ = 0.0f;      // pre-vibrato sung f0; feeds the F1 pitch floor
    int   f0GlideLeft_ = 0;
    float amp_ = 0.0f, ampTarget_ = 0.0f;
    float gate_ = 0.0f;        // 1 while the player is sounding (note-off detector)
    bool  voiceSilent_ = false;
    float voiced_ = 0.0f, voicedTarget_ = 0.0f;
    float jitter_ = 0.0f, jitterTarget_ = 0.0f;
    int   jitterLeft_ = 0;
    float shimmer_ = 0.0f;
    int   fricLeft_ = 0, fricLen_ = 1;
    float fricGain_ = 0.0f, fricEnv_ = 0.0f, fricTarget_ = 0.0f;
    bool  fricSustain_ = false, fricAspirate_ = false;
    int   aspLeft_ = 0;
    float aspEnv_ = 0.0f, aspTarget_ = 0.0f;
    // sample-rate-derived one-pole coefficients (set in prepare)
    float voicedC_ = 0.004f, gateC_ = 0.001f, bwC_ = 0.002f, nasalC_ = 0.001f;
    float aspC_ = 0.005f, noteOffC_ = 0.0007f, fricAtkC_ = 0.001f, fricRelC_ = 0.002f;
    float shimmerC_ = 0.001f, jitterC_ = 0.002f;   // rate-derived in prepare()
    uint32_t rng_ = 0xB5297A4Du;

    // ---- romaji helpers ----------------------------------------------------
    static bool isSep(char c) {
        return c == ' ' || c == ',' || c == '-' || c == '|' || c == '\n'
            || c == '\r' || c == '\t' || c == ';' || c == '.' || c == '\'';
    }
    static int vowelIndex(char c) {
        switch (c) { case 'a': return V_A; case 'i': return V_I; case 'u': return V_U;
                     case 'e': return V_E; case 'o': return V_O; default: return -1; }
    }
    static int digraph(char a, char b) {
        if (a == 's' && b == 'h') return C_SH;
        if (a == 'c' && b == 'h') return C_CH;
        if (a == 't' && b == 's') return C_TS;
        if (a == 'n' && b == 'y') return C_NY;
        if (a == 'n' && b == 'n') return C_NN;
        // Palatalised moras (kya, gyu, ...) keep the base consonant but take the
        // palatal glide's brighter locus -> map to the closest voiced pair.
        if (b == 'y')
        {
            switch (a) {
                case 'k': return C_K;  case 'g': return C_G;  case 'h': return C_H;
                case 'm': return C_M;  case 'r': return C_R;  case 'b': return C_B;
                case 'p': return C_P;  case 'd': return C_J;  case 'z': return C_J;
                case 's': return C_SH; case 't': return C_CH; case 'j': return C_J;
                default: break;
            }
        }
        return C_None;
    }
    static int singleCons(char c) {
        switch (c) {
            case 'k': return C_K;  case 'g': return C_G;  case 's': return C_S;
            case 'z': return C_Z;  case 'j': return C_J;  case 't': return C_T;
            case 'c': return C_CH; case 'd': return C_D;  case 'n': return C_N;
            case 'h': return C_H;  case 'f': return C_F;  case 'b': return C_B;
            case 'p': return C_P;  case 'm': return C_M;  case 'y': return C_Y;
            case 'r': return C_R;  case 'l': return C_R;  case 'w': return C_W;
            case 'v': return C_B;  case 'x': return C_S;  case 'q': return C_K;
            default:  return C_None;
        }
    }
};

// Out-of-line constexpr definitions (C++17 ODR).
constexpr float MikuStompCore::kVowF[4][MikuStompCore::kNumVowels];
constexpr float MikuStompCore::kVowB[4][MikuStompCore::kNumVowels];

} // namespace dcdsp

#endif // DCDSP_MIKUSTOMP_H
