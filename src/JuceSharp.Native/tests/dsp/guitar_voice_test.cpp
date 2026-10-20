// juce_sharp_guitar_voice_test — the chart voice (src/dsp/voice/), device-free.
//
// Every check measures the RENDERED SIGNAL (pitch by a windowed DTFT peak
// search, level by RMS windows), never the model's own state, so a check can
// only pass if the sound does what it claims. Each was shown non-vacuous by
// breaking the feature on purpose (see the PR description for the list).
#include "dsp/voice/dcvoice_excitation_data.h"
#include "dsp/voice/dcvoice_plan.h"
#include "dsp/voice/dcvoice_timeline.h"

#include <array>
#include <cfloat>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace dcvoice;

namespace {

int failures = 0;
int checks = 0;

void check(bool ok, const std::string& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

#ifdef _MSC_VER
thread_local bool armed = false;
thread_local long allocations = 0;
int __cdecl allocHook(int kind, void*, size_t, int block, long, const unsigned char*, int)
{
    if (armed && (kind == _HOOK_ALLOC || kind == _HOOK_REALLOC || kind == _HOOK_FREE) && block != _CRT_BLOCK)
        ++allocations;
    return 1;
}
#endif

// ---- signal measurement ------------------------------------------------------

double dtftMag(const std::vector<float>& x, size_t a, size_t n, double f, double fs)
{
    // Hann-windowed DTFT at one frequency, phasor recurrence.
    const double w = kTwoPi * f / fs;
    double cr = 1.0, ci = 0.0;
    const double sr = std::cos(w), si = -std::sin(w);
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(kTwoPi * (i + 0.5) / n);
        const double v = win * x[a + i];
        re += v * cr;
        im += v * ci;
        const double t = cr * sr - ci * si;
        ci = cr * si + ci * sr;
        cr = t;
    }
    return std::sqrt(re * re + im * im);
}

/// Strongest frequency within +-rel of `around`, golden-section refined.
double peakFreq(const std::vector<float>& x, double t0, double t1, double around, double fs, double rel = 0.03)
{
    const size_t a = (size_t) (t0 * fs);
    const size_t n = std::min((size_t) ((t1 - t0) * fs), x.size() - a);
    double best = around, bestMag = -1.0;
    for (int k = 0; k <= 40; ++k)
    {
        const double f = around * (1.0 - rel + 2.0 * rel * k / 40.0);
        const double m = dtftMag(x, a, n, f, fs);
        if (m > bestMag)
        {
            bestMag = m;
            best = f;
        }
    }
    double lo = best - around * rel / 20.0, hi = best + around * rel / 20.0;
    for (int it = 0; it < 40; ++it)
    {
        const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
        if (dtftMag(x, a, n, m1, fs) < dtftMag(x, a, n, m2, fs))
            lo = m1;
        else
            hi = m2;
    }
    return 0.5 * (lo + hi);
}

double magAt(const std::vector<float>& x, double t0, double t1, double f, double fs)
{
    const size_t a = (size_t) (t0 * fs);
    const size_t n = std::min((size_t) ((t1 - t0) * fs), x.size() - a);
    double best = 0.0;
    for (int k = -4; k <= 4; ++k)   // tolerate inharmonicity
        best = std::max(best, dtftMag(x, a, n, f * (1.0 + 0.004 * k), fs));
    return best;
}

double rms(const std::vector<float>& x, double t0, double t1, double fs)
{
    const size_t a = std::min((size_t) (t0 * fs), x.size());
    const size_t b = std::min((size_t) (t1 * fs), x.size());
    double s = 0.0;
    for (size_t i = a; i < b; ++i)
        s += (double) x[i] * x[i];
    return b > a ? std::sqrt(s / (double) (b - a)) : 0.0;
}

double db(double v) { return 20.0 * std::log10(v + 1e-30); }
double cents(double f, double ref) { return 1200.0 * std::log2(f / ref); }

// ---- rendering -------------------------------------------------------------------

struct Song
{
    std::vector<ChartNote> notes;
    std::vector<ChartBend> bends;
    std::vector<double> beats;

    ChartNote& add(double t, double sus, int string, int fret)
    {
        ChartNote n;
        n.time = t;
        n.sustain = sus;
        n.string = string;
        n.fret = fret;
        notes.push_back(n);
        return notes.back();
    }
};

struct RenderOpts
{
    double rate = 48000.0;
    int block = 128;
    double speed = 1.0;
    double seconds = 2.0;
    bool bass = false;
    int bassStrings = 4;
};

std::vector<float> render(const Song& s, ChartTimelineVoice& v, const RenderOpts& o)
{
    PlanOptions po;
    po.bass = v.spec().bass;
    PlannedTimeline plan;
    planChart(s.notes.data(), (int) s.notes.size(), s.bends.data(), (int) s.bends.size(),
              s.beats.empty() ? nullptr : s.beats.data(), (int) s.beats.size(), po, plan);
    const VoiceTimeline tl = plan.view(7);
    v.prepare(o.rate, o.block);
    std::vector<float> out((size_t) (o.seconds * o.rate));
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = o.speed / o.rate;
    for (size_t i = 0; i < out.size(); i += (size_t) o.block)
    {
        c.songTime = (double) i * c.songSecondsPerSample;
        v.render(out.data() + i, (int) std::min<size_t>((size_t) o.block, out.size() - i), c, tl);
    }
    return out;
}

ChartTimelineVoice guitar() { return ChartTimelineVoice(InstrumentSpec::guitar(), guitarExcitations()); }
ChartTimelineVoice bass(int n = 4) { return ChartTimelineVoice(InstrumentSpec::bassGuitar(n), bassExcitations()); }

double fretHz(const ChartTimelineVoice& v, int s, double fret)
{
    return v.spec().openHz[(size_t) s] * std::pow(2.0, fret / 12.0);
}

// ---- tests -----------------------------------------------------------------------

// The fallback bank's size budget (#61): the generated header was 3.6 MB of
// int16 text; band-limited 12-bit digits are ~0.6 MB. Re-measure before
// raising either number — the point is that it cannot grow back unnoticed.
constexpr long kBankSampleBudget = 290000;     // 274,702 samples measured
constexpr long kBankHeaderByteBudget = 655360; // 621,806 bytes measured (640 KiB)

void testBankIsShipped()
{
    const ExcitationBank g = guitarExcitations(), b = bassExcitations();
    check(g.count >= 60, "the guitar excitation bank is populated (" + std::to_string(g.count) + " clips)");
    check(b.count >= 60, "the bass excitation bank is populated (" + std::to_string(b.count) + " clips)");
    long samples = 0;
    int kinds[4] = {};
    for (const ExcitationBank* bank : { &g, &b })
        for (int i = 0; i < bank->count; ++i)
        {
            samples += bank->clips[i].length;
            ++kinds[bank->clips[i].kind & 3];
            check(bank->clips[i].f0Hz > 20.0f && bank->clips[i].t60Low > 0.0f, "clip " + std::to_string(i) + " has a fitted profile");
        }
    std::printf("  excitation bank: %ld samples (%ld bytes of digits)\n", samples, samples * 2);
    check(samples <= kBankSampleBudget, "the excitation bank stays inside its budget (" + std::to_string(samples) + " samples > "
                                            + std::to_string(kBankSampleBudget) + ")");
    check(kinds[0] > 0 && kinds[1] > 0 && kinds[2] > 0 && kinds[3] > 0, "pick, finger, ghost and dead clips all present");
#ifdef JUCESHARP_VOICE_EXCITATION_DATA
    // The bytes in git (LF; a CR from a CRLF checkout is not counted).
    long fileBytes = -1;
    if (std::FILE* f = std::fopen(JUCESHARP_VOICE_EXCITATION_DATA, "rb"))
    {
        fileBytes = 0;
        for (int ch; (ch = std::fgetc(f)) != EOF;)
            fileBytes += ch != '\r' ? 1 : 0;
        std::fclose(f);
    }
    std::printf("  dcvoice_excitation_data.h: %ld bytes\n", fileBytes);
    check(fileBytes > 0, "the generated excitation header is readable at " JUCESHARP_VOICE_EXCITATION_DATA);
    check(fileBytes <= kBankHeaderByteBudget, "the generated excitation header stays inside its budget ("
                                                  + std::to_string(fileBytes) + " bytes > " + std::to_string(kBankHeaderByteBudget) + ")");
#else
    check(false, "JUCESHARP_VOICE_EXCITATION_DATA is defined by tests/CMakeLists.txt");
#endif
}

void testBankDecodes()
{
    // Every clip, as the voice reads it: a band-limited rate, digits from the
    // alphabet only, exactly `length` samples in its literal, 12-bit full scale
    // and a sum of EXACTLY zero (a DC component would be integrated by the loop).
    int bad = 0;
    for (const ExcitationBank& bank : { guitarExcitations(), bassExcitations() })
        for (int i = 0; i < bank.count; ++i)
        {
            const ExcitationClip& c = bank.clips[i];
            static const char* const kKind[4] = { "pick", "finger", "ghost", "dead" };
            const std::string id = std::string(kKind[c.kind & 3]) + " clip " + std::to_string(i);
            const bool rateOk = c.sampleRate == 22050.0f || c.sampleRate == 11025.0f;
            const bool lengthOk = c.length >= 16 && std::strlen(c.data) == (size_t) c.length * 2;
            bool digitsOk = true;
            for (int k = 0; lengthOk && k < 2 * c.length; ++k)
            {
                const int u = (unsigned char) c.data[k];
                digitsOk = digitsOk && u >= '0' && u <= 'q' && u != '?' && u != '\\';
            }
            long sum = 0;
            int peak = 0;
            for (int k = 0; lengthOk && digitsOk && k < c.length; ++k)
            {
                const int v = clipSample(c, k);
                sum += v;
                peak = std::max(peak, std::abs(v));
            }
            const bool ok = rateOk && lengthOk && digitsOk && sum == 0 && peak >= 2040 && peak <= 2047 && c.scale > 0.0f;
            if (! ok && bad++ < 8)
                check(false, id + ": rate " + std::to_string(c.sampleRate) + ", length " + std::to_string(c.length) + " vs "
                                 + std::to_string(std::strlen(c.data)) + " digits, digits " + (digitsOk ? "ok" : "BAD")
                                 + ", sum " + std::to_string(sum) + ", peak " + std::to_string(peak));
        }
    check(bad == 0, "every excitation clip decodes (" + std::to_string(bad) + " bad)");
}

void testFallbackAttacks()
{
    // Every string and every RECORDED articulation of the fallback, from the
    // shipped bank: the attack is finite, on time and at a playing level, and
    // it is the recorded residual — it differs from the same note on an EMPTY
    // bank, which plays the synthetic pluck instead.
    struct Art
    {
        const char* name;
        bool bass;
        uint8_t mute;
        bool picked;
        double minDb, maxDb;   // attack peak, dBFS
    };
    // Bounds: the measured range (2026-10-04) +- ~8 dB.
    const Art arts[] = { { "guitar pick", false, 0, false, -24.0, -4.0 }, { "guitar palm mute", false, 2, false, -27.0, -3.0 },
                         { "guitar dead", false, 1, false, -21.0, -4.0 }, { "bass finger", true, 0, false, -23.0, -3.0 },
                         { "bass pick", true, 0, true, -26.0, -1.0 },     { "bass ghost", true, 1, false, -24.0, -7.0 } };
    RenderOpts o;
    o.seconds = 0.3;
    const double t0 = 0.05;
    for (const Art& a : arts)
    {
        const InstrumentSpec spec = a.bass ? InstrumentSpec::bassGuitar(4) : InstrumentSpec::guitar();
        ChartTimelineVoice shipped(spec, a.bass ? bassExcitations() : guitarExcitations());
        ChartTimelineVoice synthetic(spec, ExcitationBank {});
        double lo = 1e9, hi = -1e9, leastDiff = 1e9, lateMs = -1e9;
        for (int s = 0; s < spec.numStrings; ++s)
            for (int fret : { 0, 7 })
            {
                Song song;
                ChartNote& n = song.add(t0, a.mute == 1 ? 0.0 : 0.2, s, fret);
                n.mute = a.mute;
                n.picked = a.picked;
                const auto x = render(song, shipped, o);
                const auto y = render(song, synthetic, o);
                const std::string id = std::string(a.name) + " string " + std::to_string(s) + " fret " + std::to_string(fret);
                bool finite = true;
                double peak = 0.0;
                for (size_t i = 0; i < x.size(); ++i)
                {
                    finite = finite && std::isfinite(x[i]);
                    if (i >= (size_t) (t0 * o.rate) && i < (size_t) ((t0 + 0.06) * o.rate))
                        peak = std::max(peak, (double) std::fabs(x[i]));
                }
                size_t onset = 0;
                while (onset < x.size() && std::fabs(x[onset]) < 0.02 * peak)   // -34 dB: where it STARTS (a finger rises slowly)
                    ++onset;
                double err = 0.0, ref = 0.0;
                for (size_t i = (size_t) (t0 * o.rate); i < (size_t) ((t0 + 0.06) * o.rate); ++i)
                {
                    err += ((double) x[i] - y[i]) * ((double) x[i] - y[i]);
                    ref += (double) y[i] * y[i];
                }
                const double diffDb = 10.0 * std::log10(err / (ref + 1e-30) + 1e-30);
                const double onsetMs = ((double) onset / o.rate - t0) * 1000.0;
                lo = std::min(lo, db(peak));
                hi = std::max(hi, db(peak));
                leastDiff = std::min(leastDiff, diffDb);
                lateMs = std::max(lateMs, onsetMs);
                check(finite, id + ": the attack is finite");
                check(db(peak) >= a.minDb && db(peak) <= a.maxDb, id + ": attack peak " + std::to_string(db(peak)) + " dBFS");
                check(onsetMs >= -0.5 && onsetMs <= 5.0, id + ": the attack starts on time (" + std::to_string(onsetMs) + " ms)");
                check(diffDb > 0.0, id + ": the attack is the recorded residual, not the synthetic pluck ("
                                         + std::to_string(diffDb) + " dB from it)");
            }
        std::printf("  fallback %-16s attack peak %.1f..%.1f dBFS, starts <= %.2f ms late, >= %.1f dB from the synthetic pluck\n",
                    a.name, lo, hi, lateMs, leastDiff);
    }
}

void testPitchAcrossTheNeck(const char* name, ChartTimelineVoice v, std::initializer_list<int> frets, double tolCents)
{
    double worst = 0.0;
    for (int s = 0; s < v.spec().numStrings; ++s)
        for (int fret : frets)
        {
            Song song;
            song.add(0.01, 2.0, s, fret);
            RenderOpts o;
            o.seconds = 1.0;
            const auto x = render(song, v, o);
            const double target = fretHz(v, s, fret);
            const double f = peakFreq(x, 0.2, 0.95, target, o.rate);
            const double c = cents(f, target);
            worst = std::max(worst, std::fabs(c));
            check(std::fabs(c) <= tolCents, std::string(name) + " string " + std::to_string(s) + " fret " + std::to_string(fret)
                                                + " sounds at " + std::to_string(c) + " cents");
        }
    std::printf("  %s: worst pitch error %.3f cents\n", name, worst);
}

void testSampleRateIndependence()
{
    double ref[3] = {};
    double decay[3] = {};
    int k = 0;
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        ChartTimelineVoice v = guitar();
        Song s;
        s.add(0.01, 3.0, 2, 5);
        RenderOpts o;
        o.rate = rate;
        o.seconds = 1.6;
        const auto x = render(s, v, o);
        const double target = fretHz(v, 2, 5);
        const double c = cents(peakFreq(x, 0.3, 1.0, target, rate), target);
        check(std::fabs(c) < 1.0, "pitch at " + std::to_string((int) rate) + " Hz within 1 cent (" + std::to_string(c) + ")");
        ref[k] = db(magAt(x, 0.1, 0.5, target, rate) / (0.4 * rate));
        decay[k] = db(rms(x, 1.2, 1.5, rate) / rms(x, 0.05, 0.35, rate));
        ++k;
    }
    for (int i = 1; i < 3; ++i)
    {
        check(std::fabs(ref[i] - ref[1]) < 1.5, "fundamental level at rate #" + std::to_string(i) + " within 1.5 dB of 48 kHz ("
                                                     + std::to_string(ref[i] - ref[1]) + ")");
        check(std::fabs(decay[i] - decay[1]) < 1.5, "decay over 1.2 s at rate #" + std::to_string(i) + " within 1.5 dB of 48 kHz ("
                                                         + std::to_string(decay[i] - decay[1]) + ")");
    }
    check(std::fabs(ref[0] - ref[1]) < 1.5, "fundamental level at 44.1 kHz within 1.5 dB of 48 kHz");
}

void testBends()
{
    ChartTimelineVoice v = guitar();
    const double base = fretHz(v, 3, 7);
    {
        Song s;   // bend up a whole step at 0.3 s, hold, release at 1.2 s
        ChartNote& n = s.add(0.0, 2.0, 3, 7);
        n.bendFirst = 0;
        n.bendCount = 3;
        s.bends = { { 0.3, 2.0f }, { 0.9, 2.0f }, { 1.3, 0.0f } };
        RenderOpts o;
        o.seconds = 1.9;
        const auto x = render(s, v, o);
        const double held = cents(peakFreq(x, 0.35, 0.85, base * std::pow(2.0, 2.0 / 12.0), o.rate, 0.05), base);
        const double released = cents(peakFreq(x, 1.35, 1.85, base, o.rate, 0.05), base);
        check(std::fabs(held - 200.0) < 5.0, "a whole-step bend lands on +200 cents (" + std::to_string(held) + ")");
        check(std::fabs(released) < 5.0, "the release returns to the fret (" + std::to_string(released) + ")");
    }
    {
        Song s;   // prebend: a point at the onset
        ChartNote& n = s.add(0.0, 1.5, 3, 7);
        n.bendFirst = 0;
        n.bendCount = 2;
        s.bends = { { 0.0, 1.0f }, { 1.0, 0.0f } };
        RenderOpts o;
        o.seconds = 1.5;
        const auto x = render(s, v, o);
        const double pre = cents(peakFreq(x, 0.02, 0.5, base * std::pow(2.0, 1.0 / 12.0), o.rate, 0.05), base);
        check(std::fabs(pre - 100.0) < 5.0, "a prebend sounds bent from the first sample window (" + std::to_string(pre) + ")");
    }
    {
        // vibrato moves the pitch, depth grows with strength
        double spread[2] = {};
        int i = 0;
        for (int strength : { 40, 120 })
        {
            Song t;
            ChartNote& n = t.add(0.0, 2.0, 4, 9);
            n.vibrato = true;
            n.vibratoStrength = strength;
            RenderOpts o;
            o.seconds = 1.6;
            const auto x = render(t, v, o);
            const double f = fretHz(v, 4, 9);
            double lo = 1e9, hi = -1e9;
            for (double t0 = 0.5; t0 < 1.4; t0 += 0.03)
            {
                const double c = cents(peakFreq(x, t0, t0 + 0.06, f, o.rate, 0.06), f);
                lo = std::min(lo, c);
                hi = std::max(hi, c);
            }
            spread[i++] = hi - lo;
            check(lo > -15.0, "guitar vibrato bends UP from the fret (min " + std::to_string(lo) + " cents)");
        }
        check(spread[0] > 8.0, "vibrato 40 moves the pitch (" + std::to_string(spread[0]) + " cents)");
        check(spread[1] > spread[0] * 1.8, "vibrato 120 is wider than 40 (" + std::to_string(spread[1]) + " vs "
                                                + std::to_string(spread[0]) + ")");
    }
}

void testSlides()
{
    ChartTimelineVoice v = guitar();
    {
        Song s;
        ChartNote& n = s.add(0.0, 2.0, 2, 5);
        n.slideTo = 12;
        RenderOpts o;
        o.seconds = 2.1;
        const auto x = render(s, v, o);
        const double start = cents(peakFreq(x, 0.05, 0.6, fretHz(v, 2, 5), o.rate), fretHz(v, 2, 5));
        const double end = cents(peakFreq(x, 1.91, 2.01, fretHz(v, 2, 12), o.rate, 0.02), fretHz(v, 2, 12));
        check(std::fabs(start) < 3.0, "a slide starts on its fret (" + std::to_string(start) + ")");
        check(std::fabs(end) < 10.0, "a slide 5 -> 12 ends on fret 12 (" + std::to_string(end) + " cents)");
        // Fret-quantised: the pitch sits ON a fret most of the time. A
        // portamento would be within +-15 cents of a fret 30 % of the time.
        int onFret = 0, windows = 0;
        for (double t0 = 0.75; t0 < 1.85; t0 += 0.02, ++windows)
        {
            const double est = cents(peakFreq(x, t0, t0 + 0.025, fretHz(v, 2, 8.5), o.rate, 0.25), fretHz(v, 2, 0));
            if (std::fabs(est - 100.0 * std::round(est / 100.0)) < 15.0)
                ++onFret;
        }
        const double maxOff = 100.0 * (1.0 - (double) onFret / windows);
        check(maxOff < 40.0, "the slide moves fret by fret (" + std::to_string(100.0 - maxOff) + " % of windows on a fret)");
    }
    {
        Song s;   // unpitched slide out: falls and fades
        ChartNote& n = s.add(0.0, 1.0, 1, 12);
        n.unpitchedSlide = true;
        RenderOpts o;
        o.seconds = 1.4;
        const auto x = render(s, v, o);
        check(rms(x, 0.85, 0.95, o.rate) < 0.25 * rms(x, 0.15, 0.25, o.rate), "an unpitched slide fades out");
    }
}

void testMutesAndRelease()
{
    ChartTimelineVoice v = guitar();
    RenderOpts o;
    o.seconds = 1.5;
    Song open, palm, dead, released;
    open.add(0.0, 0.0, 0, 3).sustain = 1.4;
    palm.add(0.0, 1.4, 0, 3).mute = 2;
    dead.add(0.0, 0.0, 0, 3).mute = 1;
    released.add(0.0, 0.5, 0, 3);
    const auto xo = render(open, v, o), xp = render(palm, v, o), xd = render(dead, v, o), xr = render(released, v, o);
    const double dropOpen = db(rms(xo, 0.45, 0.55, o.rate) / rms(xo, 0.02, 0.1, o.rate));
    const double dropPalm = db(rms(xp, 0.45, 0.55, o.rate) / rms(xp, 0.02, 0.1, o.rate));
    std::printf("  decay to 0.5 s: open %.1f dB, palm mute %.1f dB\n", dropOpen, dropPalm);
    check(dropPalm < dropOpen - 15.0, "a palm mute decays at least 15 dB faster than the open note by 0.5 s");
    check(rms(xp, 0.02, 0.1, o.rate) > 0.15 * rms(xo, 0.02, 0.1, o.rate), "a palm mute still has an attack");
    const double deadTail = db(rms(xd, 0.25, 0.4, o.rate) / rms(xd, 0.0, 0.05, o.rate));
    check(deadTail < -60.0, "a dead note is gone 250 ms later (" + std::to_string(deadTail) + " dB)");
    check(rms(xd, 0.0, 0.05, o.rate) > 1e-3, "a dead note still makes a sound");
    const double relTail = db(rms(xr, 1.0, 1.2, o.rate) / rms(xr, 0.1, 0.3, o.rate));
    check(relTail < -50.0, "a held note stops after its sustain (" + std::to_string(relTail) + " dB)");
    check(db(rms(xr, 0.4, 0.5, o.rate) / rms(xr, 0.1, 0.3, o.rate)) > -12.0, "...but rings until then");
}

void testVoicePerString()
{
    ChartTimelineVoice v = guitar();
    RenderOpts o;
    o.seconds = 1.6;
    {
        Song s;   // same string: fret 0 then fret 7 — the 7 replaces the 0
        s.add(0.0, 1.5, 1, 0);
        s.add(0.5, 1.0, 1, 7);
        const auto x = render(s, v, o);
        const double f0 = fretHz(v, 1, 0), f7 = fretHz(v, 1, 7);
        const double a0 = magAt(x, 0.8, 1.4, f0, o.rate), a7 = magAt(x, 0.8, 1.4, f7, o.rate);
        check(db(a0 / a7) < -25.0, "a new note on a ringing string replaces it (" + std::to_string(db(a0 / a7)) + " dB)");
    }
    {
        Song s;   // different strings: both ring
        s.add(0.0, 1.5, 1, 0);
        s.add(0.5, 1.0, 3, 2);
        const auto x = render(s, v, o);
        const double a = magAt(x, 0.8, 1.4, fretHz(v, 1, 0), o.rate), b = magAt(x, 0.8, 1.4, fretHz(v, 3, 2), o.rate);
        check(db(a / b) > -15.0, "a note on another string does not stop the first (" + std::to_string(db(a / b)) + " dB)");
    }
    {
        Song s;   // hammer-on: pitch moves, no new pick
        s.add(0.0, 0.0, 2, 5);
        s.add(0.5, 0.8, 2, 7).hopo = 1;
        Song p;
        p.add(0.0, 0.0, 2, 5);
        p.add(0.5, 0.8, 2, 7);
        const auto xh = render(s, v, o), xp = render(p, v, o);
        const double f7 = fretHz(v, 2, 7);
        check(std::fabs(cents(peakFreq(xh, 0.6, 1.1, f7, o.rate), f7)) < 3.0, "a hammer-on sounds the new fret");
        double pkH = 0.0, pkP = 0.0;
        for (size_t i = (size_t) (0.5 * o.rate); i < (size_t) (0.52 * o.rate); ++i)
        {
            pkH = std::max(pkH, (double) std::fabs(xh[i]));
            pkP = std::max(pkP, (double) std::fabs(xp[i]));
        }
        check(pkH < 0.7 * pkP, "a hammer-on has no pick attack (" + std::to_string(pkH) + " vs picked " + std::to_string(pkP) + ")");
    }
}

void testHarmonics()
{
    ChartTimelineVoice v = guitar();
    RenderOpts o;
    o.seconds = 1.2;
    for (auto [fret, k] : { std::pair { 12, 2 }, std::pair { 7, 3 }, std::pair { 5, 4 } })
    {
        Song s;
        s.add(0.0, 1.2, 1, fret).harmonic = 1;
        const auto x = render(s, v, o);
        const double f0 = v.spec().openHz[1];
        const double fund = magAt(x, 0.3, 1.0, f0, o.rate), harm = magAt(x, 0.3, 1.0, k * f0, o.rate);
        check(db(fund / harm) < -25.0, "natural harmonic at fret " + std::to_string(fret) + " leaves harmonic " + std::to_string(k)
                                           + " (fundamental " + std::to_string(db(fund / harm)) + " dB)");
        // A stiff string's upper modes are sharp (f_k = k f0 sqrt((1+B k^2)/(1+B))),
        // and so is a real harmonic: +0.8 / +2 / +4 cents for k = 2 / 3 / 4 at the
        // A string's fitted B. Allow that, nothing more.
        const double hc = cents(peakFreq(x, 0.3, 1.0, k * f0, o.rate), k * f0);
        std::printf("  natural harmonic k=%d: %+.2f cents from k*f0\n", k, hc);
        check(hc > -1.0 && hc < 1.5 * k * k, "...in tune, stiffness-sharp at most (" + std::to_string(hc) + " cents)");
    }
    check(harmonicNumberForFret(12) == 2 && harmonicNumberForFret(19) == 3 && harmonicNumberForFret(24) == 4
              && harmonicNumberForFret(9) == 5 && harmonicNumberForFret(16) == 5 && harmonicNumberForFret(3) == 6,
          "harmonic number per fret (12/19/24/9/16/3)");
    {
        Song plain, pinch;
        plain.add(0.0, 1.2, 1, 5);
        pinch.add(0.0, 1.2, 1, 5).harmonic = 2;
        const auto xp = render(plain, v, o), xq = render(pinch, v, o);
        const double f0 = fretHz(v, 1, 5);
        const int k = std::clamp((int) std::lround(1400.0 / f0), 3, 6);
        const double rPlain = db(magAt(xp, 0.3, 1.0, k * f0, o.rate) / magAt(xp, 0.3, 1.0, f0, o.rate));
        const double rPinch = db(magAt(xq, 0.3, 1.0, k * f0, o.rate) / magAt(xq, 0.3, 1.0, f0, o.rate));
        check(rPinch > rPlain + 10.0, "a pinch harmonic lifts harmonic " + std::to_string(k) + " over the fundamental by >10 dB vs a plain pick ("
                                          + std::to_string(rPinch) + " vs " + std::to_string(rPlain) + ")");
    }
}

void testBassArticulations()
{
    ChartTimelineVoice v = bass();
    RenderOpts o;
    o.seconds = 1.2;
    Song finger, slap, pop, ghost;
    finger.add(0.0, 1.0, 0, 5);
    slap.add(0.0, 1.0, 0, 5).bassPicking = 2;
    pop.add(0.0, 1.0, 2, 7).bassPicking = 1;
    ghost.add(0.0, 0.0, 0, 5).mute = 1;
    const auto xf = render(finger, v, o), xs = render(slap, v, o), xp = render(pop, v, o), xg = render(ghost, v, o);
    const double f0 = fretHz(v, 0, 5);
    // Brightness: energy above 1.5 kHz in the first 60 ms against the fundamental.
    auto bright = [&](const std::vector<float>& x, double fund) {
        double hi = 0.0;
        for (double f = 1500.0; f < 6000.0; f += 125.0)
            hi += dtftMag(x, 0, (size_t) (0.06 * o.rate), f, o.rate);
        return db(hi / dtftMag(x, 0, (size_t) (0.06 * o.rate), fund, o.rate));
    };
    const double bf = bright(xf, f0), bs = bright(xs, f0), bp = bright(xp, fretHz(v, 2, 7));
    std::printf("  bass brightness (HF/fundamental, first 60 ms): finger %.1f dB, slap %.1f dB, pop %.1f dB\n", bf, bs, bp);
    check(bs > bf + 6.0, "a slap is clearly brighter than a finger pluck");
    check(bp > bf + 6.0, "a pop is clearly brighter than a finger pluck");
    check(std::fabs(cents(peakFreq(xs, 0.3, 1.0, f0, o.rate), f0)) < 3.0, "a slap is in tune");
    check(db(rms(xg, 0.3, 0.5, o.rate) / rms(xg, 0.0, 0.05, o.rate)) < -50.0, "a bass ghost note is short");
    for (int s = 0; s < 4; ++s)
    {
        Song t;
        t.add(0.0, 1.0, s, 3);
        const auto x = render(t, v, o);
        const double f = fretHz(v, s, 3);
        check(std::fabs(cents(peakFreq(x, 0.3, 1.0, f, o.rate), f)) < 3.0, "bass string " + std::to_string(s) + " fret 3 in tune");
    }
}

void testTimingAndTransport()
{
    ChartTimelineVoice v = guitar();
    for (int block : { 64, 128, 333 })
    {
        Song s;
        s.add(0.123456, 0.3, 2, 3);
        RenderOpts o;
        o.block = block;
        o.seconds = 0.4;
        const auto x = render(s, v, o);
        size_t first = x.size();
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0.0f)
            {
                first = i;
                break;
            }
        const size_t want = (size_t) std::ceil(0.123456 * o.rate - 0.5);
        check(first >= want && first <= want + 1, "block " + std::to_string(block) + ": the note starts on its sample ("
                                                       + std::to_string(first) + " vs " + std::to_string(want) + ")");
    }
    {
        // Same song, different block sizes: the same samples.
        Song s;
        s.add(0.05, 0.6, 0, 3);
        s.add(0.3, 0.6, 3, 5).hopo = 0;
        ChartNote& b = s.add(0.5, 1.0, 4, 8);
        b.bendFirst = 0;
        b.bendCount = 1;
        s.bends = { { 0.8, 2.0f } };
        RenderOpts a, c;
        a.block = 128;
        c.block = 97;
        a.seconds = c.seconds = 1.6;
        const auto xa = render(s, v, a), xc = render(s, v, c);
        double maxDiff = 0.0;
        for (size_t i = 0; i < xa.size(); ++i)
            maxDiff = std::max(maxDiff, (double) std::fabs(xa[i] - xc[i]));
        check(maxDiff < 1e-4, "block size does not change the output (max diff " + std::to_string(maxDiff) + ")");
    }
    {
        // Half speed: same pitch, the note starts at twice the real time.
        Song s;
        s.add(0.25, 1.0, 1, 2);
        RenderOpts o;
        o.speed = 0.5;
        o.seconds = 1.5;
        const auto x = render(s, v, o);
        size_t first = x.size();
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0.0f)
            {
                first = i;
                break;
            }
        check(std::llabs((long long) first - (long long) (0.5 * o.rate)) <= 1, "at half speed a note at 0.25 s song time starts at 0.5 s");
        const double f = fretHz(v, 1, 2);
        check(std::fabs(cents(peakFreq(x, 0.7, 1.4, f, o.rate), f)) < 1.0, "practice speed does not change the pitch");
    }
    {
        // Seek back: the ringing string is damped and the note plays again.
        Song s;
        s.add(0.1, 2.0, 1, 0);
        PlannedTimeline plan;
        PlanOptions po;
        planChart(s.notes.data(), 1, nullptr, 0, nullptr, 0, po, plan);
        const auto tl = plan.view(1);
        v.prepare(48000.0, 128);
        std::vector<float> out((size_t) (48000 * 1.0));
        VoiceClock c;
        c.rolling = true;
        c.songSecondsPerSample = 1.0 / 48000.0;
        double song = 0.0;
        for (size_t i = 0; i < out.size(); i += 128)
        {
            if (i == 128 * 200)
                song = 0.0;   // seek to the top
            c.songTime = song;
            v.render(out.data() + i, 128, c, tl);
            song += 128.0 / 48000.0;
        }
        const size_t seekAt = 128 * 200;
        const double before = rms(out, (seekAt - 2400) / 48000.0, seekAt / 48000.0, 48000.0);
        const double gap = rms(out, (seekAt + 2400) / 48000.0, (seekAt + 4400) / 48000.0, 48000.0);
        const double again = rms(out, (seekAt + 5600) / 48000.0, (seekAt + 9600) / 48000.0, 48000.0);
        check(gap < 0.05 * before, "a seek damps the ringing string");
        check(again > 0.3 * before, "after a seek back the note plays again");
    }
    {
        // Stopped transport: nothing starts, ringing strings die.
        Song s;
        s.add(0.0, 2.0, 1, 0);
        PlannedTimeline plan;
        PlanOptions po;
        planChart(s.notes.data(), 1, nullptr, 0, nullptr, 0, po, plan);
        const auto tl = plan.view(1);
        v.prepare(48000.0, 128);
        std::vector<float> out(48000);
        VoiceClock c;
        c.songSecondsPerSample = 1.0 / 48000.0;
        for (size_t i = 0; i < out.size(); i += 128)
        {
            c.rolling = i < 9600;
            c.songTime = (double) i / 48000.0;
            v.render(out.data() + i, 128, c, tl);
        }
        check(rms(out, 0.6, 1.0, 48000.0) < 1e-4, "stopping the transport silences the voice");
        check(v.activeStrings() == 0, "...and frees the strings");
    }
}

void testPlanner()
{
    // Strum direction and spread.
    Song s;
    for (double b = 0.0; b < 8.0; b += 0.5)
        s.beats.push_back(b);
    for (double t : { 1.0, 1.25 })
    {
        for (int str = 0; str < 6; ++str)
        {
            ChartNote& n = s.add(t, 0.0, str, 2);
            n.chordId = t == 1.0 ? 0 : 1;
        }
    }
    PlannedTimeline plan;
    PlanOptions po;
    planChart(s.notes.data(), (int) s.notes.size(), nullptr, 0, s.beats.data(), (int) s.beats.size(), po, plan);
    double down[6] = {}, up[6] = {};
    bool downFlag = true, upFlag = true;
    for (const VoiceNote& n : plan.notes)
    {
        if (n.anchorTime == 1.0)
        {
            down[n.string] = n.startTime;
            downFlag &= (n.flags & kFlagUpStroke) == 0;
        }
        else
        {
            up[n.string] = n.startTime;
            upFlag &= (n.flags & kFlagUpStroke) != 0;
        }
    }
    check(downFlag && upFlag, "a chord on the beat is a down-stroke, on the off-beat 8th an up-stroke");
    check(down[0] < down[5] && down[5] - down[0] > 0.025 && down[5] - down[0] < 0.07, "a down-strum reaches low E first, 25-70 ms across");
    check(up[5] < up[0], "an up-strum reaches high E first");
    // Release policy.
    Song r;
    r.add(0.0, 0.0, 0, 3);
    r.add(0.4, 0.0, 2, 5);
    r.add(1.0, 0.6, 3, 5);
    PlannedTimeline rp;
    planChart(r.notes.data(), 3, nullptr, 0, nullptr, 0, po, rp);
    check(std::fabs(rp.notes[0].releaseTime - 0.43) < 1e-6, "a short note rings to the next note + 30 ms");
    check(std::fabs(rp.notes[2].releaseTime - 1.62) < 1e-6, "a sustained note is held to its sustain end");
    // Tremolo expands to alternating re-picks.
    Song t;
    t.add(0.0, 1.0, 5, 12).tremolo = true;
    PlannedTimeline tp;
    planChart(t.notes.data(), 1, nullptr, 0, nullptr, 0, po, tp);
    check(tp.notes.size() >= 12 && tp.notes.size() <= 16, "tremolo at 14/s gives 12-16 picks per second (" + std::to_string(tp.notes.size()) + ")");
    check((tp.notes[1].flags & kFlagUpStroke) && ! (tp.notes[2].flags & kFlagUpStroke), "tremolo picks alternate");
    // Garbage in, nothing out of range.
    Song g;
    g.add(0.0, 1.0, 9, 3);
    g.add(std::numeric_limits<double>::quiet_NaN(), 1.0, 1, 3);
    g.add(0.1, 1.0, 1, 99);
    PlannedTimeline gp;
    planChart(g.notes.data(), 3, nullptr, 0, nullptr, 0, po, gp);
    check(gp.notes.size() == 2, "a note with a non-finite time is dropped");
    ChartTimelineVoice v = guitar();
    v.prepare(48000.0, 128);
    std::vector<float> out(48000);
    VoiceClock c;
    c.rolling = true;
    c.songSecondsPerSample = 1.0 / 48000.0;
    const auto tl = gp.view(3);
    bool finite = true;
    for (size_t i = 0; i < out.size(); i += 128)
    {
        c.songTime = (double) i / 48000.0;
        v.render(out.data() + i, 128, c, tl);
    }
    for (float x : out)
        finite &= std::isfinite(x) && std::fabs(x) < 1.0f;
    check(finite, "an out-of-range string and fret 99 render finite and bounded");
}

/// Everything at once, long, at three rates: finite, bounded, no denormals,
/// deterministic, and allocation-free on the render path.
void testTortureDeterminismAndRt()
{
    Song s;
    for (double b = 0.0; b < 30.0; b += 0.5)
        s.beats.push_back(b);
    uint32_t r = 12345;
    auto rnd = [&]() { r ^= r << 13; r ^= r >> 17; r ^= r << 5; return (double) (r >> 8) / 16777216.0; };
    int chord = 0;
    for (double t = 0.05; t < 20.0; t += 0.125)
    {
        const double u = rnd();
        if (u < 0.15)
        {
            for (int str = 0; str < 6; ++str)
            {
                ChartNote& n = s.add(t, rnd() < 0.5 ? 0.0 : 0.4, str, (int) (rnd() * 5));
                n.chordId = chord;
                n.mute = rnd() < 0.2 ? 2 : (rnd() < 0.1 ? 1 : 0);
            }
            ++chord;
            continue;
        }
        ChartNote& n = s.add(t, rnd() < 0.5 ? 0.0 : rnd() * 1.5, (int) (rnd() * 6), (int) (rnd() * 25));
        const double a = rnd();
        if (a < 0.08) n.hopo = 1;
        else if (a < 0.16) n.hopo = 2;
        else if (a < 0.2) n.hopo = 3;
        else if (a < 0.26) n.mute = 2;
        else if (a < 0.3) n.mute = 1;
        else if (a < 0.34) n.harmonic = 1;
        else if (a < 0.38) n.harmonic = 2;
        else if (a < 0.44) n.slideTo = (int) (rnd() * 20);
        else if (a < 0.48) n.unpitchedSlide = true;
        else if (a < 0.52) n.tremolo = true;
        else if (a < 0.56) { n.vibrato = true; n.vibratoStrength = 120; }
        else if (a < 0.62)
        {
            n.bendFirst = (int) s.bends.size();
            n.bendCount = 2;
            s.bends.push_back({ t + 0.05, 3.0f });
            s.bends.push_back({ t + 0.3, 0.0f });
            n.sustain = std::max(n.sustain, 0.4);
        }
        n.accent = rnd() < 0.1;
        n.linkNext = rnd() < 0.05;
    }
    for (double rate : { 44100.0, 48000.0, 96000.0 })
    {
        ChartTimelineVoice a = guitar(), b = guitar();
        RenderOpts o;
        o.rate = rate;
        o.seconds = 21.0;
        const auto xa = render(s, a, o);
        // Same voice again after a hard reset: bit-identical.
        a.hardReset();
        const auto xb = render(s, a, o);
        const auto xc = render(s, b, o);
        bool finite = true, bounded = true, denormal = false;
        for (float x : xa)
        {
            finite &= std::isfinite(x);
            bounded &= std::fabs(x) < 1.0f;
            denormal |= x != 0.0f && std::fabs(x) < FLT_MIN;
        }
        const std::string at = " at " + std::to_string((int) rate) + " Hz";
        check(finite, "torture song renders finite" + at);
        check(bounded, "torture song stays below full scale" + at);
        check(! denormal, "torture song emits no denormals" + at);
        check(std::memcmp(xa.data(), xb.data(), xa.size() * sizeof(float)) == 0, "the same voice renders the same song bit-identically" + at);
        check(std::memcmp(xa.data(), xc.data(), xa.size() * sizeof(float)) == 0, "two voices render the same song bit-identically" + at);
        check(rms(xa, 1.0, 20.0, rate) > 1e-3, "torture song is audible" + at);
    }
    // Bass torture (slap/pop/ghost) through the same checks, once.
    {
        Song bsong = s;
        for (auto& n : bsong.notes)
        {
            n.string %= 4;
            n.fret %= 20;
            if (n.harmonic == 2) { n.harmonic = 0; n.bassPicking = 2; }
            if (n.tremolo) { n.tremolo = false; n.bassPicking = 1; }
        }
        ChartTimelineVoice v = bass(5);
        RenderOpts o;
        o.seconds = 21.0;
        const auto x = render(bsong, v, o);
        bool ok = true;
        for (float f : x)
            ok &= std::isfinite(f) && std::fabs(f) < 1.0f && ! (f != 0.0f && std::fabs(f) < FLT_MIN);
        check(ok, "bass torture song renders finite, bounded, no denormals");
    }
#ifdef _MSC_VER
    {
        // Allocation-free: the whole torture song, block by block, hook armed.
        PlanOptions po;
        PlannedTimeline plan;
        planChart(s.notes.data(), (int) s.notes.size(), s.bends.data(), (int) s.bends.size(), s.beats.data(),
                  (int) s.beats.size(), po, plan);
        const auto tl = plan.view(1);
        ChartTimelineVoice v = guitar();
        v.prepare(48000.0, 128);
        std::vector<float> buf(128);
        VoiceClock c;
        c.rolling = true;
        c.songSecondsPerSample = 1.0 / 48000.0;
        allocations = 0;
        armed = true;
        long blocks = 0;
        for (double t = 0.0; t < 20.0; t += 128.0 / 48000.0, ++blocks)
        {
            c.songTime = t;
            if (blocks == 3000)
                c.songTime = 2.0;   // a seek inside the armed window
            v.render(buf.data(), 128, c, tl);
        }
        armed = false;
        check(allocations == 0, "render() allocated " + std::to_string(allocations) + " times over " + std::to_string(blocks) + " blocks");
    }
#endif
}

/// The event API the engine's chart source drives (dcvoice_chartvoice.h maps
/// issue #39 step 0's ChartVoice onto it one call per call).
void testEventCore()
{
    const double fs = 48000.0;
    auto make = [&]() {
        StringInstrument c(InstrumentSpec::guitar(), guitarExcitations());
        c.prepare(fs, 128);
        return c;
    };
    auto run = [&](StringInstrument& c, std::vector<float>& out, size_t from, size_t to) {
        for (size_t i = from; i < to; i += 128)
            c.render(out.data() + i, (int) std::min<size_t>(128, to - i));
    };
    auto firstAbove = [](const std::vector<float>& x, size_t from, float thr) {
        for (size_t i = from; i < x.size(); ++i)
            if (std::fabs(x[i]) > thr)
                return i;
        return x.size();
    };
    {
        // render() ADDS into the buffer.
        StringInstrument c = make();
        std::vector<float> out(4800, 0.25f);
        run(c, out, 0, out.size());
        check(out[100] == 0.25f && out[4000] == 0.25f, "an idle core adds nothing and overwrites nothing");
    }
    {
        // Strum delay: the attack waits delaySamples (to the control step).
        StringInstrument c = make();
        NoteStart n;
        n.id = 1;
        n.string = 2;
        n.fret = 2;
        n.pitch = c.pitchOf(2, 2);
        n.delaySamples = 480;
        c.noteOn(n);
        std::vector<float> out(9600, 0.0f);
        run(c, out, 0, out.size());
        const size_t first = firstAbove(out, 0, 0.0f);
        check(first == 480 || first == 481, "a strummed string starts on its delayed sample (" + std::to_string(first) + ")");
    }
    {
        // Choke: gone within 5 ms, no click (no sample above the level before it).
        StringInstrument c = make();
        NoteStart n;
        n.id = 7;
        n.string = 1;
        n.pitch = c.pitchOf(1, 0);
        c.noteOn(n);
        std::vector<float> out(24000, 0.0f);
        run(c, out, 0, 9600);
        float before = 0.0f;
        for (size_t i = 7200; i < 9600; ++i)
            before = std::max(before, std::fabs(out[i]));
        c.noteOff(7, true);
        run(c, out, 9600, out.size());
        // What is left after the ramp is the 12 Hz DC blocker's own subsonic
        // tail (its low-pass state holds ~fc/f of the signal). Judge the
        // AUDIBLE band by first differences, which pass 110 Hz ~9x more than a
        // 12 Hz exponential: -60 dB of it after 5 ms, and -60 dB raw at 60 ms.
        float dBefore = 0.0f, dAfter = 0.0f, late = 0.0f, during = 0.0f;
        for (size_t i = 7201; i < 9600; ++i)
            dBefore = std::max(dBefore, std::fabs(out[i] - out[i - 1]));
        for (size_t i = 9600; i < 9600 + 240; ++i)
            during = std::max(during, std::fabs(out[i]));
        for (size_t i = 9600 + 240; i < out.size(); ++i)
            dAfter = std::max(dAfter, std::fabs(out[i] - out[i - 1]));
        for (size_t i = 9600 + 2880; i < out.size(); ++i)
            late = std::max(late, std::fabs(out[i]));
        check(dAfter < 1e-3f * dBefore, "a choke is 60 dB down in the audible band after 5 ms (" + std::to_string(db(dAfter / dBefore + 1e-12)) + " dB)");
        check(late < 1e-3f * before, "...and 60 dB down altogether after 60 ms (" + std::to_string(db(late / before + 1e-12)) + " dB)");
        check(during <= before * 1.05f, "a choke does not click");
        check(c.activeNotes() == 0, "a choked string is freed");
    }
    {
        // A stale noteOff (for a note the string already replaced) is ignored.
        StringInstrument c = make();
        NoteStart a, b;
        a.id = 1; a.string = 3; a.pitch = c.pitchOf(3, 0);
        b.id = 2; b.string = 3; b.fret = 5; b.pitch = c.pitchOf(3, 5);
        c.noteOn(a);
        std::vector<float> out(48000, 0.0f);
        run(c, out, 0, 4800);
        c.noteOn(b);
        run(c, out, 4800, 9600);
        c.noteOff(1, false);
        run(c, out, 9600, out.size());
        check(rms(out, 0.6, 0.9, fs) > 0.2 * rms(out, 0.12, 0.18, fs), "a noteOff for the replaced note does not stop the new one");
    }
    {
        // Tremolo: the core re-picks by itself, ~14 per second at 1x.
        StringInstrument c = make();
        NoteStart n;
        n.id = 3; n.string = 5; n.fret = 12; n.pitch = c.pitchOf(5, 12);
        n.flags = kFlagTremolo;
        c.noteOn(n);
        std::vector<float> out(48000, 0.0f);
        run(c, out, 0, out.size());
        // Count attacks as rises of the 2 ms envelope.
        int attacks = 0;
        double prev = 0.0;
        for (size_t i = 0; i + 96 < out.size(); i += 96)
        {
            double e = 0.0;
            for (size_t k = i; k < i + 96; ++k)
                e = std::max(e, (double) std::fabs(out[k]));
            if (e > prev * 1.6 && e > 0.02)
                ++attacks;
            prev = e;
        }
        check(attacks >= 11 && attacks <= 17, "a tremolo note re-picks ~14 times a second (" + std::to_string(attacks) + ")");
    }
    {
        // Chase: entering mid-sustain has no pick attack.
        StringInstrument picked = make(), chased = make();
        NoteStart n;
        n.id = 4; n.string = 2; n.fret = 7; n.pitch = picked.pitchOf(2, 7);
        picked.noteOn(n);
        n.chaseSeconds = 0.5;
        chased.noteOn(n);
        std::vector<float> a(24000, 0.0f), b(24000, 0.0f);
        run(picked, a, 0, a.size());
        run(chased, b, 0, b.size());
        double pa = 0.0, pb = 0.0;
        for (size_t i = 0; i < 960; ++i)
        {
            pa = std::max(pa, (double) std::fabs(a[i]));
            pb = std::max(pb, (double) std::fabs(b[i]));
        }
        check(pb < 0.5 * pa, "a chased note enters without the pick attack");
        const double f = 440.0 * std::pow(2.0, (n.pitch - 69.0) / 12.0);
        check(std::fabs(cents(peakFreq(b, 0.1, 0.45, f, fs), f)) < 2.0, "...at its pitch");
    }
}

/// Natural harmonics count their node from the CAPO, not the nut (#51): capo c +
/// a harmonic charted at fret c + h sounds k * (open + c), k from h. Standard,
/// Drop D and Eb tunings, capo 0 / 2 / 5, h = 12 / 7 / 5 / 4 (k = 2 / 3 / 4 / 5).
/// Measured on the rendered signal through the core's own event API (NoteStart
/// carries `pitch` from the nut and `capo`, exactly what the engine hands over).
template <typename MakeCore>
void checkHarmonicsWithCapo(const char* name, MakeCore makeCore, double tolCents)
{
    const double fs = 48000.0;
    const std::pair<const char*, std::array<int, 6>> tunings[] = {
        { "standard", { 0, 0, 0, 0, 0, 0 } }, { "drop D", { -2, 0, 0, 0, 0, 0 } }, { "Eb", { -1, -1, -1, -1, -1, -1 } } };
    const std::pair<int, int> nodes[] = { { 12, 2 }, { 7, 3 }, { 5, 4 }, { 4, 5 } };
    double worst = 0.0;
    for (const auto& [tname, offs] : tunings)
        for (int capo : { 0, 2, 5 })
            for (const auto& [h, k] : nodes)
                for (int s : { 0, 3 })
                {
                    InstrumentSpec spec = InstrumentSpec::guitar();
                    spec.applyTuningOffsets(offs.data(), 6);
                    auto core = makeCore(spec);
                    core->prepare(fs, 128);
                    const double open = 69.0 + 12.0 * std::log2(spec.openHz[(size_t) s] / 440.0);
                    NoteStart n;
                    n.id = 1;
                    n.string = s;
                    n.fret = capo + h;
                    n.capo = capo;
                    n.pitch = open + n.fret;   // from the nut, as the engine's builder computes it
                    n.flags = kFlagHarmonic;
                    n.velocity = 0.7f;
                    core->noteOn(n);
                    std::vector<float> out((size_t) (0.9 * fs), 0.0f);
                    for (size_t i = 0; i < out.size(); i += 128)
                        core->render(out.data() + i, (int) std::min<size_t>(128, out.size() - i));
                    const double expect = 440.0 * std::pow(2.0, (open + capo - 69.0) / 12.0) * k;
                    const double c = cents(peakFreq(out, 0.2, 0.85, expect, fs, 0.04), expect);
                    worst = std::max(worst, std::fabs(c));
                    check(std::fabs(c) < tolCents, std::string(name) + " " + tname + " capo " + std::to_string(capo) + " harmonic at fret "
                                                       + std::to_string(capo + h) + " (k=" + std::to_string(k) + ") string "
                                                       + std::to_string(s) + ": " + std::to_string(c) + " cents");
                }
    std::printf("  %s: harmonics above a capo, worst %.2f cents\n", name, worst);
}

void reportCpu()
{
    // Informational, never asserted: six strings ringing at once.
    Song s;
    for (int rep = 0; rep < 10; ++rep)
        for (int str = 0; str < 6; ++str)
        {
            ChartNote& n = s.add(rep * 1.0 + str * 0.005, 0.95, str, (rep * 3 + str) % 12);
            n.chordId = rep;
            if (rep % 2) { n.vibrato = true; }
        }
    ChartTimelineVoice v = guitar();
    RenderOpts o;
    o.seconds = 10.0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto x = render(s, v, o);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  CPU (informational): 6 ringing strings, 48 kHz / 128 frames: %.4f s for 10 s audio = real-time factor %.5f\n",
                secs, secs / 10.0);
    (void) x;
}

} // namespace

int main()
{
#ifdef _MSC_VER
    _CrtSetAllocHook(allocHook);
#endif
    std::printf("chart voice tests\n");
    testBankIsShipped();
    testBankDecodes();
    testFallbackAttacks();
    testPitchAcrossTheNeck("guitar", guitar(), { 0, 1, 5, 7, 12, 17, 22, 24 }, 3.0);
    testPitchAcrossTheNeck("bass4", bass(4), { 0, 1, 5, 12, 17, 24 }, 3.0);
    testPitchAcrossTheNeck("bass5", bass(5), { 0, 3, 12 }, 3.0);
    {
        InstrumentSpec drop = InstrumentSpec::guitar();
        const int offsets[6] = { -2, 0, 0, 0, 0, 0 };   // drop D
        drop.applyTuningOffsets(offsets, 6);
        ChartTimelineVoice v(drop, guitarExcitations());
        testPitchAcrossTheNeck("guitar drop D", std::move(v), { 0, 12 }, 3.0);
    }
    testSampleRateIndependence();
    testBends();
    testSlides();
    testMutesAndRelease();
    testVoicePerString();
    testHarmonics();
    testBassArticulations();
    testTimingAndTransport();
    testPlanner();
    testTortureDeterminismAndRt();
    testEventCore();
    // The waveguide's harmonics are stiffness-sharp (+4 / +6 / +8 / +11 cents for
    // k = 2..5 measured at capo 0), so the band is wider than the sampler's.
    checkHarmonicsWithCapo("waveguide", [](const InstrumentSpec& spec) {
        return std::make_unique<StringInstrument>(spec, guitarExcitations()); }, 18.0);
    reportCpu();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
