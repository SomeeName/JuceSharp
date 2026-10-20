// Drive gear — the "drive" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendDriveGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include <cmath>
#include <cstring>

#include "../cores/dcdsp_rat.h"
#include "../cores/dcdsp_ds1.h"
#include "../cores/dcdsp_hm2.h"
#include "../cores/dcdsp_tapesat.h"
#include "../cores/dcdsp_bassod.h"
#include "../cores/dcdsp_tubepre.h"
#include "../cores/dcdsp_octfuzz.h"

namespace dcrig
{
namespace
{

// A NAMED/ORDINAL discrete param reaches an int core field, so the adapter
// lrounds it here and getParam hands the INT back. A NaN must never reach
// std::lround (the result is unspecified, and it raises FE_INVALID), so it is
// refused first - the chain rejects a NaN before this anyway, but a core is not
// allowed to depend on a caller it cannot see. The core's own clampIndex would
// defend the SOUND; what it cannot defend is getParam, which hands the raw
// stored int back to the UI as (float) INT_MIN.
//
// Deliberately duplicated per gear file rather than hoisted into GearEntry.h:
// each drawer is one translation unit and `quantise` is four tokens.
// GearFilterTests.EveryDiscreteParam_RefusesANaNBeforeItReachesLround lints all
// four copies, so a fifth bare std::lround cannot be added quietly.
int quantise(float v, int fallback)
{
    return std::isfinite(v) ? (int) std::lround(v) : fallback;
}


// --- rat -------------------------------------------------------------------
class RatModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "distortion")) p_.distortion = v;
        else if (!std::strcmp(id, "filter")) p_.filter = v;
        else if (!std::strcmp(id, "volume")) p_.volume = v;
        else if (!std::strcmp(id, "clip")) p_.clip = quantise(v, 0);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "distortion")) out = p_.distortion;
        else if (!std::strcmp(id, "filter")) out = p_.filter;
        else if (!std::strcmp(id, "volume")) out = p_.volume;
        else if (!std::strcmp(id, "clip")) out = (float) p_.clip;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "rat"; }
private:
    dcdsp::RatCore core_; dcdsp::RatParams p_{}; ParamMailbox<dcdsp::RatParams> mail_;
};

// --- ds1 -------------------------------------------------------------------
class Ds1Module : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "dist")) p_.dist = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "dist")) out = p_.dist;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "ds1"; }
private:
    dcdsp::Ds1Core core_; dcdsp::Ds1Params p_{}; ParamMailbox<dcdsp::Ds1Params> mail_;
};

// --- hm2 -------------------------------------------------------------------
class Hm2Module : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "dist")) p_.dist = v;
        else if (!std::strcmp(id, "lowDb")) p_.lowDb = v;
        else if (!std::strcmp(id, "highDb")) p_.highDb = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "dist")) out = p_.dist;
        else if (!std::strcmp(id, "lowDb")) out = p_.lowDb;
        else if (!std::strcmp(id, "highDb")) out = p_.highDb;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "hm2"; }
private:
    dcdsp::Hm2Core core_; dcdsp::Hm2Params p_{}; ParamMailbox<dcdsp::Hm2Params> mail_;
};

// --- tapesat ---------------------------------------------------------------
class TapeSatModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        latency_ = core_.latencySamples();
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "bias")) p_.bias = v;
        else if (!std::strcmp(id, "hfRolloffKHz")) p_.hfRolloffKHz = v;
        else if (!std::strcmp(id, "hiss")) p_.hiss = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "bias")) out = p_.bias;
        else if (!std::strcmp(id, "hfRolloffKHz")) out = p_.hfRolloffKHz;
        else if (!std::strcmp(id, "hiss")) out = p_.hiss;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // Owner-thread read of a PREPARE-time geometry, never of callback state:
    // the dry leg is always compensated, so this does not move with `mix`.
    int latencySamples() const override { return latency_; }
    const char* typeId() const override { return "tapesat"; }
private:
    int latency_ = 0;
    dcdsp::TapeSatCore core_; dcdsp::TapeSatParams p_{}; ParamMailbox<dcdsp::TapeSatParams> mail_;
};

// --- bassod ----------------------------------------------------------------
class BassOdModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        latency_ = core_.latencySamples();
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "blend")) p_.blend = v;
        else if (!std::strcmp(id, "bass")) p_.bass = v;
        else if (!std::strcmp(id, "treble")) p_.treble = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "blend")) out = p_.blend;
        else if (!std::strcmp(id, "bass")) out = p_.bass;
        else if (!std::strcmp(id, "treble")) out = p_.treble;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    int latencySamples() const override { return latency_; }
    const char* typeId() const override { return "bassod"; }
private:
    int latency_ = 0;
    dcdsp::BassOdCore core_; dcdsp::BassOdParams p_{}; ParamMailbox<dcdsp::BassOdParams> mail_;
};

// --- tubepre ---------------------------------------------------------------
class TubePreModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "gain")) p_.gain = v;
        else if (!std::strcmp(id, "sag")) p_.sag = v;
        else if (!std::strcmp(id, "bright")) p_.bright = quantise(v, 0);
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else if (!std::strcmp(id, "cabSim")) p_.cabSim = quantise(v, 0);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "gain")) out = p_.gain;
        else if (!std::strcmp(id, "sag")) out = p_.sag;
        else if (!std::strcmp(id, "bright")) out = (float) p_.bright;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else if (!std::strcmp(id, "cabSim")) out = (float) p_.cabSim;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "tubepre"; }
private:
    dcdsp::TubePreCore core_; dcdsp::TubePreParams p_{}; ParamMailbox<dcdsp::TubePreParams> mail_;
};

// --- octfuzz ---------------------------------------------------------------
class OctFuzzModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "fuzz")) p_.fuzz = v;
        else if (!std::strcmp(id, "octave")) p_.octave = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "volume")) p_.volume = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "fuzz")) out = p_.fuzz;
        else if (!std::strcmp(id, "octave")) out = p_.octave;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "volume")) out = p_.volume;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "octfuzz"; }
private:
    dcdsp::OctFuzzCore core_; dcdsp::OctFuzzParams p_{}; ParamMailbox<dcdsp::OctFuzzParams> mail_;
};

std::unique_ptr<DspModule> makeRat()     { return std::make_unique<RatModule>(); }
std::unique_ptr<DspModule> makeDs1()     { return std::make_unique<Ds1Module>(); }
std::unique_ptr<DspModule> makeHm2()     { return std::make_unique<Hm2Module>(); }
std::unique_ptr<DspModule> makeTapeSat() { return std::make_unique<TapeSatModule>(); }
std::unique_ptr<DspModule> makeBassOd()  { return std::make_unique<BassOdModule>(); }
std::unique_ptr<DspModule> makeTubePre() { return std::make_unique<TubePreModule>(); }
std::unique_ptr<DspModule> makeOctFuzz() { return std::make_unique<OctFuzzModule>(); }

} // namespace

void appendDriveGear(std::vector<Entry>& out)
{
    out.push_back({ { "rat", "Rodent Distortion",
        { { "distortion", "Distortion", 0.0f, 1.0f, 0.5f },
          // The real pedal's Filter knob is a passive LOW-PASS: clockwise is
          // DARKER. Do not "fix" the taper.
          { "filter", "Filter (Open-Dark)", 0.0f, 1.0f, 0.4f },
          { "volume", "Volume", 0.0f, 1.0f, 0.6f },
          // A NAMED discrete choice: the diode pair in the clipping leg.
          { "clip", "Clipping", 0.0f, 2.0f, 0.0f, { "Silicon", "LED", "Turbo" } } } },
        &makeRat, kCategoryDrive });

    out.push_back({ { "ds1", "Orange Distortion",
        { { "dist", "Dist", 0.0f, 1.0f, 0.5f },
          { "tone", "Tone (Dark-Bright)", 0.0f, 1.0f, 0.5f },
          { "level", "Level", 0.0f, 1.0f, 0.6f } } },
        &makeDs1, kCategoryDrive });

    out.push_back({ { "hm2", "Chainsaw Distortion",
        { { "dist", "Dist", 0.0f, 1.0f, 0.7f },
          { "lowDb", "Low Colour (dB)", 0.0f, 12.0f, 10.0f },
          { "highDb", "High Colour (dB)", 0.0f, 12.0f, 10.0f },
          { "level", "Level", 0.0f, 1.0f, 0.5f } } },
        &makeHm2, kCategoryDrive });

    out.push_back({ { "tapesat", "Tape Saturation",
        { { "drive", "Drive", 0.0f, 1.0f, 0.3f },
          { "bias", "Bias (Asymmetry)", 0.0f, 1.0f, 0.5f },
          { "hfRolloffKHz", "HF Roll-off (kHz)", 4.0f, 20.0f, 12.0f },
          { "hiss", "Hiss", 0.0f, 1.0f, 0.0f },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makeTapeSat, kCategoryDrive });

    out.push_back({ { "bassod", "Bass Overdrive",
        { { "drive", "Drive", 0.0f, 1.0f, 0.4f },
          { "blend", "Blend (Clean-Drive)", 0.0f, 1.0f, 0.5f },
          { "bass", "Bass (dB)", -12.0f, 12.0f, 0.0f },
          { "treble", "Treble (dB)", -12.0f, 12.0f, 0.0f },
          { "level", "Level", 0.0f, 1.0f, 0.6f } } },
        &makeBassOd, kCategoryDrive });

    out.push_back({ { "tubepre", "Tube Preamp / DI",
        { { "gain", "Gain", 0.0f, 1.0f, 0.4f },
          { "sag", "Sag", 0.0f, 1.0f, 0.3f },
          { "bright", "Bright", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
          { "tone", "Tone (Dark-Bright)", 0.0f, 1.0f, 0.5f },
          { "level", "Level", 0.0f, 1.0f, 0.6f },
          { "cabSim", "Cab Sim", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
        &makeTubePre, kCategoryDrive });

    out.push_back({ { "octfuzz", "Octave Fuzz",
        { { "fuzz", "Fuzz", 0.0f, 1.0f, 0.7f },
          { "octave", "Octave", 0.0f, 1.0f, 0.6f },
          { "tone", "Tone (Dark-Bright)", 0.0f, 1.0f, 0.5f },
          { "volume", "Volume", 0.0f, 1.0f, 0.5f } } },
        &makeOctFuzz, kCategoryDrive });
}

} // namespace dcrig
