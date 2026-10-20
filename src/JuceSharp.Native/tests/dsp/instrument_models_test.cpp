// juce_sharp_instrument_models_test — the chart voice's INSTRUMENT MODELS and
// PICKUP POSITIONS (DotCharter #50): src/dsp/voice/dcvoice_models.h (the table),
// dcvoice_pickup.h (the conversion) and their use in dcvoice_sampler.h and
// synth/GuitarChartVoice.h. Device-free.
//
// The real packs are built outside git, so the packs here are SYNTHESISED and
// go through the real parser: flat harmonic tones COLOURED with the pickup
// comb each pack is said to have recorded (sin(pi n d / L) per harmonic), so a
// "recorded" position carries its colour exactly as a real recording does and
// a converted one has to produce it.
#include "dsp/voice/dcvoice_models.h"
#include "dsp/voice/dcvoice_pack.h"
#include "dsp/voice/dcvoice_pack_store.h"
#include "dsp/voice/dcvoice_pickup.h"
#include "dsp/voice/dcvoice_sampler.h"
#include "synth/GuitarChartVoice.h"
#include "voice_pack_fixture.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

using namespace dcvoice;
namespace fs = std::filesystem;
namespace vf = voicefixture;

namespace {

int failures = 0, checks = 0;
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

constexpr double kFs = 48000.0;
double db(double v) { return 20.0 * std::log10(v + 1e-30); }
std::string num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }

double dtftMag(const std::vector<float>& x, double t0, double t1, double f)
{
    const size_t a = (size_t) (t0 * kFs), n = (size_t) ((t1 - t0) * kFs);
    const double w = vf::kTau * f / kFs;
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < n && a + i < x.size(); ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(vf::kTau * (i + 0.5) / n);
        re += win * x[a + i] * std::cos(w * i);
        im -= win * x[a + i] * std::sin(w * i);
    }
    return std::sqrt(re * re + im * im);
}

/// Spectral centroid of a window, from the magnitudes at the first 24
/// harmonics of f0 (the tones have nothing else in them).
double centroid(const std::vector<float>& x, double t0, double t1, double f0)
{
    double num = 0.0, den = 0.0;
    for (int n = 1; n <= 96 && n * f0 < 12000.0; ++n)
    {
        const double m = dtftMag(x, t0, t1, n * f0);
        num += m * m * n * f0;
        den += m * m;
    }
    return den > 0.0 ? num / den : 0.0;
}

/// "Air": the energy of the harmonics above 2 kHz against all of them, dB —
/// where a pickup's resonance and a humbucker's two-coil sum show.
double airDb(const std::vector<float>& x, double t0, double t1, double f0)
{
    double hi = 0.0, all = 0.0;
    for (int n = 1; n <= 96 && n * f0 < 12000.0; ++n)
    {
        const double m = dtftMag(x, t0, t1, n * f0);
        all += m * m;
        if (n * f0 >= 2000.0) hi += m * m;
    }
    return 10.0 * std::log10((hi + 1e-30) / (all + 1e-30));
}

double rms(const std::vector<float>& x, double t0, double t1)
{
    const size_t a = std::min((size_t) (t0 * kFs), x.size()), b = std::min((size_t) (t1 * kFs), x.size());
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return b > a ? std::sqrt(s / (double) (b - a)) : 0.0;
}

// ---- coloured fixture packs ---------------------------------------------------

/// The pickup comb |sum w sin(pi n d / L)| of `p` for harmonic n of a note on
/// the string whose open pitch is `openMidi`, at `fret`.
double combAt(const PickupSound& p, double openMidi, int fret, int n, float scale, float midiLo, float midiHi)
{
    const double L = scale * std::pow(2.0, -fret / 12.0);
    double c = 0.0;
    for (int i = 0; i < p.count; ++i)
        c += p.pt[i].weight * std::sin(pickup::kPi * n * pickup::senseDistance(p.pt[i], openMidi, midiLo, midiHi) / L);
    return std::fabs(c);
}

std::vector<float> colouredTone(double f0, double openMidi, int fret, const SourceSound& src, double seconds)
{
    // Every harmonic up to 12 kHz (so a pickup's resonance has something to
    // shape), each a rotating phasor: cheap enough for a few thousand notes.
    std::vector<float> y((size_t) (seconds * 44100.0), 0.0f);
    const double decay = std::pow(10.0, -3.0 / (6.0 * 44100.0));
    for (int n = 1; n <= 96 && n * f0 < 12000.0; ++n)
    {
        const double a = 0.05 * combAt(src.pickup, openMidi, fret, n, src.scaleMm, src.midiLow, src.midiHigh) / n;   // a plucked string: ~1/n
        const double w = vf::kTau * n * f0 / 44100.0, ph = 0.7 * n * n;
        double re = std::cos(ph) * a, im = std::sin(ph) * a;
        const double cr = std::cos(w) * decay, ci = std::sin(w) * decay;
        for (size_t i = 0; i < y.size(); ++i)
        {
            y[i] += (float) im;
            const double t = re * cr - im * ci;
            im = re * ci + im * cr;
            re = t;
        }
    }
    return y;
}

/// A pack recorded the way `pack` says (sourceFor): guitar packs every string
/// and fret 0..12, the bass pack chromatic with both variants.
std::vector<uint8_t> colouredPack(const char* pack, double seconds = 1.6)
{
    std::vector<vf::Zone> zs;
    if (std::string(pack) == "bass")
    {
        static const int open[4] = { 28, 33, 38, 43 };
        for (int variant = 0; variant < 2; ++variant)
        {
            SourceSound src;
            sourceFor("bass", variant, src);
            for (int s = 0; s < 4; ++s)
                for (int f = 0; f <= 5; ++f)
                {
                    vf::Zone z;
                    z.variant = variant; z.root = vf::hzOf(open[s] + f); z.string = s; z.fret = f;
                    z.y = colouredTone(z.root, open[s], f, src, seconds);
                    zs.push_back(std::move(z));
                }
        }
        return vf::writePack(1, zs, "coloured bass");
    }
    SourceSound src;
    sourceFor(pack, 0, src);
    static const int open[6] = { 40, 45, 50, 55, 59, 64 };
    for (int s = 0; s < 6; ++s)
        for (int f = 0; f <= 12; ++f)
        {
            vf::Zone z;
            z.root = vf::hzOf(open[s] + f); z.string = s; z.fret = f;
            z.y = colouredTone(z.root, open[s], f, src, seconds);
            zs.push_back(std::move(z));
        }
    return vf::writePack(0, zs, std::string("coloured ") + pack);
}

fs::path packDir()
{
    static fs::path dir;
    if (dir.empty())
    {
        dir = fs::temp_directory_path() / "dcvoice_instrument_models_test";
        fs::create_directories(dir);
        for (const char* n : { "strat-neck", "strat-neck-middle", "strat-middle", "strat-middle-bridge", "strat-bridge", "guitar", "bass" })
            vf::writeFile(dir / (std::string(n) + ".dcvp"), colouredPack(n));
    }
    return dir;
}

void setVoiceDir(const fs::path& p)
{
#ifdef _WIN32
    _wputenv_s(L"DOTCHARTER_VOICE_DIR", p.wstring().c_str());
#else
    setenv("DOTCHARTER_VOICE_DIR", p.string().c_str(), 1);
#endif
}

/// One note through a GuitarChartVoice built for `config`.
std::vector<float> renderNote(bool bass, const InstrumentConfig& config, int string, int fret, double openMidi,
                              double seconds = 0.9, std::string* packName = nullptr)
{
    dcsynth::GuitarChartVoice v(bass, config);
    v.prepare(kFs, 256);
    if (packName) *packName = v.packName();
    dcsynth::ChartNoteOn n;
    n.noteId = 1; n.stringIndex = string; n.fret = fret; n.pitch = (float) (openMidi + fret); n.velocity = 0.8f;
    n.durationSeconds = seconds; n.sustainSeconds = seconds;
    v.noteOn(n);
    std::vector<float> out((size_t) (seconds * kFs), 0.0f);
    for (size_t i = 0; i < out.size(); i += 256)
        v.render(out.data() + i, (int) std::min<size_t>(256, out.size() - i));
    return out;
}

// ---- tests ------------------------------------------------------------------------

void testTable()
{
    for (int32_t id : kAllModels)
    {
        const ModelSpec* m = findModel(id);
        check(m != nullptr, "model " + std::to_string(id) + " exists");
        if (m == nullptr) continue;
        check(m->bass == (id >= 16), std::string(m->key) + ": the family follows the id");
        check(m->positionCount >= 1 && findPosition(*m, m->defaultSel) != nullptr, std::string(m->key) + ": its default pickup is on its selector");
        InstrumentConfig c; c.model = id; c.pickup = m->defaultSel;
        check(validConfig(c, m->bass) && ! validConfig(c, ! m->bass), std::string(m->key) + ": valid for its family only");
        for (int i = 0; i < m->positionCount; ++i)
        {
            SourceSound s;
            check(sourceFor(m->pos[i].sourcePack, 0, s), std::string(m->key) + ": every position names a known pack");
        }
    }
    check(findModel(5) == nullptr && findModel(-1) == nullptr && findModel(20) == nullptr, "unknown models are unknown");
    InstrumentConfig bad; bad.model = kModelStratSSS; bad.pickup = kPickNeckBridge;
    check(! validConfig(bad, false), "a Strat has no neck+bridge position on its 5-way");
    bad.model = kModelPBass; bad.pickup = kPickNeck;
    check(! validConfig(bad, true), "a P bass has only its one pickup");
    check(findModel(kModelStratSSS)->positionCount == 5 && findModel(kModelStratHSS)->positionCount == 5, "Strats are 5-way");
    check(findModel(kModelTele)->positionCount == 3 && findModel(kModelLesPaul)->positionCount == 3, "two-pickup guitars are 3-way");
    check(hasCoilSplit(*findModel(kModelStratHSS)) && hasCoilSplit(*findModel(kModelLesPaul)), "humbucker models have coil split");
    check(! hasCoilSplit(*findModel(kModelStratSSS)) && ! hasCoilSplit(*findModel(kModelTele)) && ! hasCoilSplit(*findModel(kModelJazzBass)),
          "single-coil models do not");
    const InstrumentConfig g = defaultConfig(false), b = defaultConfig(true);
    check(g.model == kModelStratHSS && g.pickup == kPickBridge && ! g.coilSplit, "the guitar default is the owner's HSS, bridge humbucker");
    check(b.model == kModelJazzBass && b.pickup == kPickNeckBridge, "the bass default is a Jazz on both pickups");
}

void testIdentity()
{
    auto ident = [](int32_t model, int32_t sel, int variant, bool split = false) {
        const ModelSpec* m = findModel(model);
        const PositionSpec* p = findPosition(*m, sel);
        return makeConversionPlan(*m, split ? p->split : p->full, p->sourcePack, variant).identity;
    };
    for (int32_t sel : { kPickNeck, kPickNeckMiddle, kPickMiddle, kPickMiddleBridge, kPickBridge })
        check(ident(kModelStratSSS, sel, 0), "Strat SSS position " + std::to_string(sel) + " plays its recordings unconverted");
    for (int32_t sel : { kPickNeck, kPickNeckMiddle, kPickMiddle })
        check(ident(kModelStratHSS, sel, 0), "Strat HSS single-coil position " + std::to_string(sel) + " is the real Strat");
    check(! ident(kModelStratHSS, kPickBridge, 0) && ! ident(kModelStratHSS, kPickBridge, 0, true), "the HSS bridge humbucker (and its split) is simulated");
    check(! ident(kModelTele, kPickBridge, 0) && ! ident(kModelLesPaul, kPickNeck, 0), "Tele and Les Paul are simulated");
    check(ident(kModelSemiHollow, kPickBridge, 0) && ! ident(kModelSemiHollow, kPickNeck, 0) && ! ident(kModelSemiHollow, kPickNeckBridge, 0),
          "the Gretsch plays as recorded on its bridge pickup only");
    check(ident(kModelJazzBass, kPickNeckBridge, 1) && ! ident(kModelJazzBass, kPickNeckBridge, 0), "Jazz both: the pick (Growlybass) is recorded, the fingers converted");
    check(ident(kModelHollowBass, kPickNeck, 0) && ! ident(kModelHollowBass, kPickNeck, 1), "the hollow bass fingers are recorded");
}

/// The conversion filter reproduces its analytic transfer, harmonic by harmonic.
void testCombMatchesTheory()
{
    const fs::path dir = packDir();
    std::string err;
    SamplePack pack;
    check(loadPackFile((dir / "strat-bridge.dcvp").string(), pack, err), "fixture pack loads: " + err);
    check(pack.multiString, "a pack with every string recorded is multi-string");
    const ModelSpec& m = *findModel(kModelTele);
    const PickupSound target = findPosition(m, kPickNeck)->full;
    ConversionPlan plan = makeConversionPlan(m, target, "strat-bridge", 0);
    // Isolate the comb: same resonance, no body, no level change.
    plan.target.resonanceHz = plan.source.pickup.resonanceHz;
    plan.target.resonanceQ = plan.source.pickup.resonanceQ;
    plan.target.levelDb = plan.source.pickup.levelDb;
    plan.targetBody = {};
    check(! plan.identity, "Tele neck from the Strat bridge is a conversion");

    for (int fret : { 0, 7 })
    {
        const int string = 2;
        const double open = 50.0, f0 = vf::hzOf(open + fret);
        auto play = [&](bool convert) {
            SampleInstrument s(InstrumentSpec::guitar(), &pack);
            if (convert) s.setConversion(-1, plan);
            s.prepare(kFs, 256);
            NoteStart n; n.id = 1; n.string = string; n.fret = fret; n.pitch = open + fret; n.velocity = 0.8f;
            s.noteOn(n);
            std::vector<float> out((size_t) (0.9 * kFs), 0.0f);
            for (size_t i = 0; i < out.size(); i += 256) s.render(out.data() + i, (int) std::min<size_t>(256, out.size() - i));
            return out;
        };
        const auto dry = play(false), wet = play(true);
        // The prediction: target comb / regularised source comb, normalised as NoteComb does.
        NoteComb nc;
        const double L = 648.0 * std::pow(2.0, -fret / 12.0);
        nc.set(plan, open, L, open, L);
        double worst = 0.0;
        int compared = 0;
        std::vector<double> ratio;
        const double g = pickup::kRegularise;
        double centre = 0.0;
        for (int i = 0; i < nc.targetCount; ++i) centre = std::max(centre, 0.5 * nc.targetRho[i]);
        for (int n = 1; n <= 12; ++n)
        {
            // The leaky target comb and the regularised source inverse, in periods.
            double re = 0.0, im = 0.0;
            for (int i = 0; i < nc.targetCount; ++i)
            {
                const double a = 0.5 * nc.targetRho[i];
                const double p1 = 2.0 * pickup::kPi * n * (centre - a), p2 = 2.0 * pickup::kPi * n * (centre + a);
                re += 0.5 * nc.targetWeight[i] * (std::cos(p1) - g * std::cos(p2));
                im -= 0.5 * nc.targetWeight[i] * (std::sin(p1) - g * std::sin(p2));
            }
            const double t = std::hypot(re, im);
            const double gs = plan.source.inverseG;
            const double w = 2.0 * pickup::kPi * n * nc.sourceSinRho;
            const double inv = 2.0 / std::hypot(1.0 - gs * std::cos(w), gs * std::sin(w));
            const double predicted = db(t * inv * nc.gain);
            const double measured = db(dtftMag(wet, 0.25, 0.85, n * f0) / dtftMag(dry, 0.25, 0.85, n * f0));
            ratio.push_back(measured);
            if (predicted > -25.0)
            {
                worst = std::max(worst, std::fabs(measured - predicted));
                ++compared;
            }
        }
        check(compared >= 6, "fret " + std::to_string(fret) + ": enough harmonics to compare (" + std::to_string(compared) + ")");
        check(worst < 1.5, "fret " + std::to_string(fret) + ": the rendered comb matches its transfer within 1.5 dB (worst " + num(worst) + " dB)");
        // Non-vacuous: moving the pickup from the bridge to the neck really changes the harmonics.
        double spread = 0.0;
        for (double r : ratio) spread = std::max(spread, std::fabs(r - ratio[0]));
        check(spread > 10.0, "fret " + std::to_string(fret) + ": the conversion reshapes the harmonics (" + num(spread) + " dB spread)");
    }
}

/// A note sounds at its pitch through every conversion (the comb must not detune).
void testPitchSurvives()
{
    setVoiceDir(packDir());
    for (int32_t id : kAllModels)
    {
        const ModelSpec& m = *findModel(id);
        for (int i = 0; i < m.positionCount; ++i)
        {
            InstrumentConfig c; c.model = id; c.pickup = m.pos[i].sel; c.coilSplit = m.pos[i].splitDiffers;
            const double open = m.bass ? 33.0 : 45.0;
            const auto x = renderNote(m.bass, c, 1, 3, open, 0.7);
            const double f0 = vf::hzOf(open + 3);
            // The fundamental or the second harmonic must stand above their neighbours.
            const double on = std::max(dtftMag(x, 0.2, 0.7, f0), dtftMag(x, 0.2, 0.7, 2 * f0));
            const double off = std::max(dtftMag(x, 0.2, 0.7, f0 * 1.06), dtftMag(x, 0.2, 0.7, 2 * f0 * 1.06));
            check(on > 20.0 * off, std::string(m.key) + " pickup " + std::to_string(c.pickup) + " sounds at its pitch");
            check(rms(x, 0.1, 0.6) > 1e-3, std::string(m.key) + " pickup " + std::to_string(c.pickup) + " is audible (rms "
                                               + std::to_string(rms(x, 0.1, 0.6)) + ")");
            bool finite = true;
            for (float v : x) finite = finite && std::isfinite(v) && std::fabs(v) < 4.0f;
            check(finite, std::string(m.key) + " pickup " + std::to_string(c.pickup) + " renders finite and bounded");
        }
    }
}

/// Neck darker than bridge on every model; a humbucker darker than its split;
/// a P bass darker than a J bridge. Measured as the spectral centroid of the
/// same notes on several strings and frets.
void testSpectralDirection()
{
    setVoiceDir(packDir());
    struct Tone { double centroid = 0.0, air = 0.0; };
    auto riffTone = [](bool bass, const InstrumentConfig& c) {
        static const int g[3][2] = { { 1, 2 }, { 3, 5 }, { 4, 8 } };   // (string, fret)
        static const int b[3][2] = { { 0, 3 }, { 1, 5 }, { 2, 2 } };
        static const double gOpen[6] = { 40, 45, 50, 55, 59, 64 }, bOpen[4] = { 28, 33, 38, 43 };
        Tone t;
        for (int k = 0; k < 3; ++k)
        {
            const int s = bass ? b[k][0] : g[k][0], f = bass ? b[k][1] : g[k][1];
            const double open = bass ? bOpen[s] : gOpen[s];
            const auto x = renderNote(bass, c, s, f, open, 0.7);
            t.centroid += centroid(x, 0.15, 0.65, vf::hzOf(open + f)) / 3.0;
            t.air += airDb(x, 0.15, 0.65, vf::hzOf(open + f)) / 3.0;
        }
        return t;
    };
    std::map<std::string, Tone> table;
    auto tone = [&](int32_t model, int32_t sel, bool split = false) {
        InstrumentConfig c; c.model = model; c.pickup = sel; c.coilSplit = split;
        const Tone v = riffTone(findModel(model)->bass, c);
        table[std::string(findModel(model)->key) + "/" + std::to_string(sel) + (split ? "/split" : "")] = v;
        return v;
    };
    auto at = [&](int32_t model, int32_t sel, bool split = false) { return tone(model, sel, split).centroid; };
    auto air = [&](int32_t model, int32_t sel, bool split = false) { return tone(model, sel, split).air; };
    for (int32_t id : { kModelStratSSS, kModelStratHSS, kModelTele, kModelLesPaul, kModelSemiHollow })
    {
        const double n = at(id, kPickNeck), b = at(id, kPickBridge);
        check(n < b * 0.85, std::string(findModel(id)->key) + ": neck (" + num(n) + " Hz) is darker than bridge (" + num(b) + " Hz)");
    }
    // The Strat 5-way: neck < middle < bridge, the in-between positions in between their pickups' extremes.
    const double sn = at(kModelStratSSS, kPickNeck), sm = at(kModelStratSSS, kPickMiddle), sb = at(kModelStratSSS, kPickBridge);
    check(sn < sm && sm < sb, "Strat: neck < middle < bridge (" + num(sn) + " / " + num(sm) + " / " + num(sb) + ")");
    // A humbucker has less air than one of its coils (its resonance is an
    // octave lower and its two coils cancel the top): the coil split is BRIGHTER.
    for (int32_t id : { kModelStratHSS, kModelLesPaul })
        for (int32_t sel : { kPickNeck, kPickBridge })
        {
            if (id == kModelStratHSS && sel == kPickNeck) continue;   // a single coil there
            const double hb = air(id, sel, false), split = air(id, sel, true);
            const double hbC = at(id, sel, false), splitC = at(id, sel, true);
            check(hb < split - 1.5 && hbC < splitC, std::string(findModel(id)->key) + " pickup " + std::to_string(sel) + ": the humbucker ("
                                        + num(hb) + " dB air, " + num(hbC) + " Hz) is darker than its split (" + num(split) + " dB, "
                                        + num(splitC) + " Hz)");
        }
    const double hssB = air(kModelStratHSS, kPickBridge), sssB = air(kModelStratSSS, kPickBridge);
    check(hssB < sssB - 3.0, "the HSS bridge humbucker (" + num(hssB) + " dB air) is darker than the SSS bridge single coil (" + num(sssB) + ")");
    const double jn = at(kModelJazzBass, kPickNeck), jb = at(kModelJazzBass, kPickBridge), jnb = at(kModelJazzBass, kPickNeckBridge);
    check(jn < jb * 0.85, "Jazz bass: neck (" + num(jn) + ") darker than bridge (" + num(jb) + ")");
    check(jnb > jn * 0.95 && jnb < jb * 1.05, "Jazz bass: both pickups sit between (" + num(jnb) + ")");
    const double p = at(kModelPBass, kPickMiddle), pjJ = at(kModelPJBass, kPickBridge), pjP = at(kModelPJBass, kPickMiddle);
    check(p < jb * 0.85 && pjP < pjJ * 0.85, "the P pickup (" + num(p) + ", " + num(pjP) + ") is darker than a J bridge (" + num(jb) + ", " + num(pjJ) + ")");
    std::printf("%-28s %10s %10s\n", "model/pickup", "centroid", "air dB");
    for (const auto& kv : table) std::printf("%-28s %10.1f %10.1f\n", kv.first.c_str(), kv.second.centroid, kv.second.air);
}

/// Each selection loads ONLY its own pack; the memory goes with the last voice.
void testPackLoading()
{
    setVoiceDir(packDir());
    check(livePackBytes() == 0, "no pack is held before any voice exists (" + std::to_string(livePackBytes()) + ")");
    for (int32_t id : kAllModels)
    {
        const ModelSpec& m = *findModel(id);
        for (int i = 0; i < m.positionCount; ++i)
        {
            InstrumentConfig c; c.model = id; c.pickup = m.pos[i].sel;
            auto v = std::make_unique<dcsynth::GuitarChartVoice>(m.bass, c);
            v->prepare(kFs, 256);
            check(v->sampled() && v->packName() == m.pos[i].sourcePack,
                  std::string(m.key) + " pickup " + std::to_string(c.pickup) + " loads " + m.pos[i].sourcePack + " (got " + v->packName() + ")");
            check(livePackBytes() == v->packBytes() && v->packBytes() > 0,
                  std::string(m.key) + ": exactly one pack is held (" + std::to_string(livePackBytes()) + " vs " + std::to_string(v->packBytes()) + ")");
            v.reset();
            check(livePackBytes() == 0, std::string(m.key) + ": destroying the voice releases its pack");
        }
    }
    // Two voices on the same pack share it (counted once).
    InstrumentConfig a; a.model = kModelStratSSS; a.pickup = kPickBridge;
    InstrumentConfig b; b.model = kModelTele; b.pickup = kPickBridge;
    {
        dcsynth::GuitarChartVoice va(false, a), vb(false, b);
        va.prepare(kFs, 256);
        vb.prepare(kFs, 256);
        check(livePackBytes() == va.packBytes(), "Strat bridge and Tele bridge share the strat-bridge pack");
    }
    check(livePackBytes() == 0, "both gone, nothing held");
    // A Strat position that is not installed falls back to the Gretsch set, converted.
    const fs::path partial = fs::temp_directory_path() / "dcvoice_instrument_models_partial";
    fs::create_directories(partial);
    fs::copy_file(packDir() / "guitar.dcvp", partial / "guitar.dcvp", fs::copy_options::overwrite_existing);
    fs::remove(partial / "strat-bridge.dcvp");
    setVoiceDir(partial);
    {
        dcsynth::GuitarChartVoice v(false, a);
        v.prepare(kFs, 256);
        check(v.sampled() && v.packName() == "guitar" && ! v.recordedAsIs(), "a missing Strat pack falls back to the Gretsch set, converted");
    }
    setVoiceDir(packDir());
    // An invalid selection for the family is the family's legacy voice.
    InstrumentConfig wrong; wrong.model = kModelJazzBass; wrong.pickup = kPickNeck;
    dcsynth::GuitarChartVoice g(false, wrong);
    check(g.config() == legacyConfig(false), "a bass model on the guitar voice falls back to the legacy guitar");
}

/// The replaced voice fades out in ~10 ms without a click.
void testFadeOut()
{
    setVoiceDir(packDir());
    InstrumentConfig c; c.model = kModelStratSSS; c.pickup = kPickNeck;
    dcsynth::GuitarChartVoice v(false, c);
    v.prepare(kFs, 64);
    dcsynth::ChartNoteOn n;
    n.noteId = 1; n.stringIndex = 2; n.fret = 5; n.pitch = 55.0f; n.velocity = 0.9f; n.durationSeconds = 2.0;
    v.noteOn(n);
    std::vector<float> out((size_t) (0.6 * kFs), 0.0f);
    const size_t fadeAt = (size_t) (0.3 * kFs);
    for (size_t i = 0; i < out.size(); i += 64)
    {
        if (i == fadeAt) v.fadeOutAll((int) (0.010 * kFs));
        v.render(out.data() + i, 64);
    }
    double stepBefore = 0.0, stepAfter = 0.0;
    for (size_t i = fadeAt - 4800; i < fadeAt; ++i) stepBefore = std::max(stepBefore, (double) std::fabs(out[i] - out[i - 1]));
    for (size_t i = fadeAt; i < fadeAt + 960; ++i) stepAfter = std::max(stepAfter, (double) std::fabs(out[i] - out[i - 1]));
    check(rms(out, 0.2, 0.3) > 1e-3, "the note sounds before the fade");
    check(stepAfter <= stepBefore * 1.05, "the fade adds no step larger than the note's own (" + num(stepAfter * 1e3) + " vs " + num(stepBefore * 1e3) + " e-3)");
    check(rms(out, 0.3 + 0.012, 0.6) < 1e-6, "silent 12 ms after fadeOutAll (" + std::to_string(rms(out, 0.312, 0.6)) + ")");
    check(v.activeNotes() == 0, "no note survives the fade");
}

/// A dense riff with bends, slides, harmonics, palm mutes and chords through a
/// CONVERTING voice: finite, bounded, and allocation-free on the render path.
void testRtSafety()
{
    setVoiceDir(packDir());
    for (int32_t id : { kModelLesPaul, kModelPJBass })
    {
        const ModelSpec& m = *findModel(id);
        InstrumentConfig c; c.model = id; c.pickup = m.pos[1].sel; c.coilSplit = true;
        dcsynth::GuitarChartVoice v(m.bass, c);
        v.prepare(kFs, 128);
        std::vector<float> out((size_t) (3.0 * kFs), 0.0f);
        int id2 = 0;
#ifdef _MSC_VER
        allocations = 0;
        _CrtSetAllocHook(allocHook);
        armed = true;
#endif
        for (size_t i = 0, k = 0; i < out.size(); i += 128, ++k)
        {
            if (k % 20 == 0)
            {
                dcsynth::ChartNoteOn n;
                n.noteId = ++id2; n.stringIndex = (int) (k / 20) % (m.bass ? 4 : 6); n.fret = (int) (k / 20) % 9;
                n.pitch = (float) ((m.bass ? 28 : 40) + 5 * n.stringIndex + n.fret); n.velocity = 0.8f; n.durationSeconds = 0.4;
                const uint32_t kinds[] = { 0u, DCA_CHART_NOTE_PALM_MUTE, DCA_CHART_NOTE_HARMONIC, DCA_CHART_NOTE_VIBRATO, DCA_CHART_NOTE_HAMMER_ON,
                                           DCA_CHART_NOTE_PINCH_HARMONIC, DCA_CHART_NOTE_FRET_HAND_MUTE };
                n.flags = kinds[(k / 20) % 7];
                n.slideToPitch = (k / 20) % 5 == 4 ? n.pitch + 5.0f : n.pitch;
                v.noteOn(n);
            }
            if (k % 20 == 10)
            {
                dcsynth::ChartNoteControls ctl; ctl.bendSemitones = 1.5f; ctl.slideSemitones = 0.0f;
                v.setNoteControls(id2, ctl);
            }
            if (k % 20 == 18) v.noteOff(id2, dcsynth::NoteOffKind::Release);
            v.render(out.data() + i, 128);
        }
#ifdef _MSC_VER
        armed = false;
        _CrtSetAllocHook(nullptr);
        check(allocations == 0, std::string(m.key) + ": the converting render path allocated " + std::to_string(allocations) + " times");
#endif
        bool ok = true;
        for (float x : out) ok = ok && std::isfinite(x) && std::fabs(x) < 4.0f;
        check(ok, std::string(m.key) + ": a dense converted riff is finite and bounded");
        check(rms(out, 0.0, 3.0) > 1e-3, std::string(m.key) + ": and audible");
    }
}

} // namespace

int main(int argc, char** argv)
{
    std::printf("instrument model tests\n");
    testTable();
    testIdentity();
    testCombMatchesTheory();
    testPitchSurvives();
    testSpectralDirection();
    testPackLoading();
    testFadeOut();
    testRtSafety();
    (void) argc; (void) argv;
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
