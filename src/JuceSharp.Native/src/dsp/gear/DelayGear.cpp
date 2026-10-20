// Delay gear — the "delay" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendDelayGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include "../cores/dcdsp_bbddelay.h"
#include "../cores/dcdsp_duckdelay.h"
#include "../cores/dcdsp_granular.h"
#include "../cores/dcdsp_multitap.h"
#include "../cores/dcdsp_rhythmdelay.h"

#include <cmath>
#include <cstring>

namespace dcrig
{
namespace
{

// Every adapter here rejects a NON-FINITE value at the door and returns false.
// The shipping path clamps twice before this (the ABI rejects non-finite, and
// DspChain::setParam rejects NaN and clamps to the catalogue range), but a
// direct DspModule::setParam caller — the native tests are exactly that — goes
// through neither, and a NaN reaching a core's clamp is silent for good: every
// `v < lo` / `v > hi` is false for a NaN, so the poison lands in the
// coefficients and stays there.
inline bool finite(float v) { return std::isfinite(v); }

// ---- multitap -------------------------------------------------------------
class MultitapModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "taps")) p_.taps = (int) std::lround(v);
        else if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "spread")) p_.spread = (int) std::lround(v);
        else if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "pan")) p_.pan = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "taps")) out = (float) p_.taps;
        else if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "spread")) out = (float) p_.spread;
        else if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "pan")) out = p_.pan;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "multitap"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::MultitapCore core_; dcdsp::MultitapParams p_{}; ParamMailbox<dcdsp::MultitapParams> mail_;
};

// ---- duckdelay ------------------------------------------------------------
class DuckDelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "duckDb")) p_.duckDb = v;
        else if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "duckDb")) out = p_.duckDb;
        else if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "duckdelay"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::DuckDelayCore core_; dcdsp::DuckDelayParams p_{}; ParamMailbox<dcdsp::DuckDelayParams> mail_;
};

// ---- bbddelay -------------------------------------------------------------
class BbdDelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "modRateHz")) p_.modRateHz = v;
        else if (!std::strcmp(id, "modDepth")) p_.modDepth = v;
        else if (!std::strcmp(id, "age")) p_.age = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "modRateHz")) out = p_.modRateHz;
        else if (!std::strcmp(id, "modDepth")) out = p_.modDepth;
        else if (!std::strcmp(id, "age")) out = p_.age;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "bbddelay"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::BbdDelayCore core_; dcdsp::BbdDelayParams p_{}; ParamMailbox<dcdsp::BbdDelayParams> mail_;
};

// ---- granular -------------------------------------------------------------
class GranularModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "grainMs")) p_.grainMs = v;
        else if (!std::strcmp(id, "density")) p_.density = (int) std::lround(v);
        else if (!std::strcmp(id, "pitchSemitones")) p_.pitchSemitones = (int) std::lround(v);
        else if (!std::strcmp(id, "spray")) p_.spray = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "freeze")) p_.freeze = v >= 0.5f;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "grainMs")) out = p_.grainMs;
        else if (!std::strcmp(id, "density")) out = (float) p_.density;
        else if (!std::strcmp(id, "pitchSemitones")) out = (float) p_.pitchSemitones;
        else if (!std::strcmp(id, "spray")) out = p_.spray;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "freeze")) out = p_.freeze ? 1.0f : 0.0f;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "granular"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::GranularCore core_; dcdsp::GranularParams p_{}; ParamMailbox<dcdsp::GranularParams> mail_;
};

// ---- rhythmdelay ----------------------------------------------------------
class RhythmDelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "pattern")) p_.pattern = (int) std::lround(v);
        else if (!std::strcmp(id, "stepMs")) p_.stepMs = v;
        else if (!std::strcmp(id, "gateDepth")) p_.gateDepth = v;
        else if (!std::strcmp(id, "gateShape")) p_.gateShape = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "pattern")) out = (float) p_.pattern;
        else if (!std::strcmp(id, "stepMs")) out = p_.stepMs;
        else if (!std::strcmp(id, "gateDepth")) out = p_.gateDepth;
        else if (!std::strcmp(id, "gateShape")) out = p_.gateShape;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "rhythmdelay"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::RhythmDelayCore core_; dcdsp::RhythmDelayParams p_{}; ParamMailbox<dcdsp::RhythmDelayParams> mail_;
};

std::unique_ptr<DspModule> makeMultitap()    { return std::make_unique<MultitapModule>(); }
std::unique_ptr<DspModule> makeDuckDelay()   { return std::make_unique<DuckDelayModule>(); }
std::unique_ptr<DspModule> makeBbdDelay()    { return std::make_unique<BbdDelayModule>(); }
std::unique_ptr<DspModule> makeGranular()    { return std::make_unique<GranularModule>(); }
std::unique_ptr<DspModule> makeRhythmDelay() { return std::make_unique<RhythmDelayModule>(); }

} // namespace

void appendDelayGear(std::vector<Entry>& out)
{
    out.push_back(
        { { "multitap", "Multitap Delay",
            // dcdsp_multitap.h: `int taps` [1..4] — an ordinal COUNT, so it
            // snaps; a count change is a weighted fade, not a branch.
            { { "taps", "Taps", 1.0f, 4.0f, 3.0f, {}, 1.0f },
              { "timeMs", "Time (ms)", 10.0f, 2000.0f, 375.0f },
              // A NAMED choice of tap-offset ratio set, not a number.
              { "spread", "Spread", 0.0f, 3.0f, 0.0f,
                { "Even", "Dotted", "Triplet", "Golden" } },
              { "decay", "Decay", 0.0f, 1.0f, 0.6f },
              { "pan", "Pan Width", 0.0f, 1.0f, 0.7f },
              { "feedback", "Feedback", 0.0f, 0.9f, 0.2f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.4f } } },
          &makeMultitap, kCategoryDelay });

    out.push_back(
        { { "duckdelay", "Ducking Delay",
            { { "timeMs", "Time (ms)", 10.0f, 2000.0f, 400.0f },
              { "feedback", "Feedback", 0.0f, 0.9f, 0.35f },
              { "duckDb", "Duck (dB)", 0.0f, 40.0f, 12.0f },
              { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
              { "releaseMs", "Release (ms)", 20.0f, 2000.0f, 300.0f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.4f } } },
          &makeDuckDelay, kCategoryDelay });

    out.push_back(
        { { "bbddelay", "Analog BBD Delay",
            { { "timeMs", "Time (ms)", 20.0f, 600.0f, 300.0f },
              // 1.0 is deliberate: self-oscillation is the sound. The soft
              // limiter in the loop is what keeps it bounded.
              { "feedback", "Feedback", 0.0f, 1.0f, 0.4f },
              { "modRateHz", "Mod Rate (Hz)", 0.1f, 5.0f, 0.5f },
              { "modDepth", "Mod Depth", 0.0f, 1.0f, 0.1f },
              { "age", "Age", 0.0f, 1.0f, 0.3f },
              { "mix", "Mix", 0.0f, 1.0f, 0.4f } } },
          &makeBbdDelay, kCategoryDelay });

    out.push_back(
        { { "granular", "Granular Cloud",
            { { "grainMs", "Grain (ms)", 10.0f, 500.0f, 80.0f },
              // dcdsp_granular.h: CONCURRENT grains, an ordinal count.
              { "density", "Density", 1.0f, 32.0f, 8.0f, {}, 1.0f },
              { "pitchSemitones", "Pitch (semitones)", -12.0f, 12.0f, 0.0f, {}, 1.0f },
              { "spray", "Spray", 0.0f, 1.0f, 0.3f },
              { "feedback", "Feedback", 0.0f, 0.9f, 0.2f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f },
              { "freeze", "Freeze", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
          &makeGranular, kCategoryDelay });

    out.push_back(
        { { "rhythmdelay", "Rhythmic Tremolo Delay",
            { { "timeMs", "Time (ms)", 10.0f, 2000.0f, 500.0f },
              { "feedback", "Feedback", 0.0f, 0.9f, 0.4f },
              // The labels ARE the patterns: 'x' is a step the repeats are
              // heard through, '.' one they are gated out of.
              { "pattern", "Pattern", 0.0f, 7.0f, 0.0f,
                { "x.x.x.x.", "xx.xx.x.", "x..x..x.", "xxx.xxx.",
                  "x.xx.x.x", "x...x...", "xxxx....", "x.x.xx.." } },
              { "stepMs", "Step (ms)", 50.0f, 1000.0f, 125.0f },
              { "gateDepth", "Gate Depth", 0.0f, 1.0f, 1.0f },
              { "gateShape", "Gate Shape", 0.0f, 1.0f, 0.2f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } },
          &makeRhythmDelay, kCategoryDelay });
}

} // namespace dcrig
