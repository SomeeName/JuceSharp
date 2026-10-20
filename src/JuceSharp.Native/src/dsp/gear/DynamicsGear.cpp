// Dynamics gear — the "dynamics" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendDynamicsGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include "../cores/dcdsp_ducker.h"
#include "../cores/dcdsp_expander.h"
#include "../cores/dcdsp_limiter.h"
#include "../cores/dcdsp_multiband.h"
#include "../cores/dcdsp_optocomp.h"
#include "../cores/dcdsp_transient.h"

#include <atomic>
#include <cmath>
#include <cstring>

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


// ---------------------------------------------------------------------------
// limiter — look-ahead brick wall (dcdsp_limiter.h)
// ---------------------------------------------------------------------------
class LimiterModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        sr_ = sr > 0.0 ? sr : 48000.0;
        publishLatency();
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "ceilingDb")) p_.ceilingDb = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "lookaheadMs")) { p_.lookaheadMs = v; publishLatency(); }
        else if (!std::strcmp(id, "gainDb")) p_.gainDb = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "ceilingDb")) out = p_.ceilingDb;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "lookaheadMs")) out = p_.lookaheadMs;
        else if (!std::strcmp(id, "gainDb")) out = p_.gainDb;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The look-ahead IS latency, so the chain's PDC has to see it. The figure
    // is derived on the MESSAGE thread from the same arithmetic the core runs,
    // because DspChain re-derives branch PDC on every accepted param write and
    // would otherwise read a number the audio thread has not applied yet.
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    const char* typeId() const override { return "limiter"; }
    void* coreForTest() override { return &core_; }

private:
    void publishLatency()
    {
        float ms = p_.lookaheadMs;
        if (! std::isfinite(ms)) ms = 0.0f;
        if (ms < 0.0f) ms = 0.0f;
        if (ms > dcdsp::LimiterCore::kMaxLookaheadMs) ms = dcdsp::LimiterCore::kMaxLookaheadMs;
        int samples = quantise((float) (sr_ * (double) ms / 1000.0), 1);
        if (samples < 1) samples = 1;   // the core floors the tap at one sample
        latency_.store(samples, std::memory_order_relaxed);
    }

    dcdsp::LimiterCore   core_;
    dcdsp::LimiterParams p_{}; ParamMailbox<dcdsp::LimiterParams> mail_;
    double sr_ = 48000.0;
    std::atomic<int> latency_{ 48 };
};

// ---------------------------------------------------------------------------
// transient — two-envelope attack/sustain designer (dcdsp_transient.h)
// ---------------------------------------------------------------------------
class TransientModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "attackDb")) p_.attackDb = v;
        else if (!std::strcmp(id, "sustainDb")) p_.sustainDb = v;
        else if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "attackDb")) out = p_.attackDb;
        else if (!std::strcmp(id, "sustainDb")) out = p_.sustainDb;
        else if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "transient"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::TransientCore   core_;
    dcdsp::TransientParams p_{}; ParamMailbox<dcdsp::TransientParams> mail_;
};

// ---------------------------------------------------------------------------
// multiband — three-band compressor over LR4 crossovers (dcdsp_multiband.h)
// ---------------------------------------------------------------------------
class MultibandModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "lowXoverHz")) p_.lowXoverHz = v;
        else if (!std::strcmp(id, "highXoverHz")) p_.highXoverHz = v;
        else if (!std::strcmp(id, "lowThresholdDb")) p_.lowThresholdDb = v;
        else if (!std::strcmp(id, "lowRatio")) p_.lowRatio = v;
        else if (!std::strcmp(id, "lowGainDb")) p_.lowGainDb = v;
        else if (!std::strcmp(id, "midThresholdDb")) p_.midThresholdDb = v;
        else if (!std::strcmp(id, "midRatio")) p_.midRatio = v;
        else if (!std::strcmp(id, "midGainDb")) p_.midGainDb = v;
        else if (!std::strcmp(id, "highThresholdDb")) p_.highThresholdDb = v;
        else if (!std::strcmp(id, "highRatio")) p_.highRatio = v;
        else if (!std::strcmp(id, "highGainDb")) p_.highGainDb = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "lowXoverHz")) out = p_.lowXoverHz;
        else if (!std::strcmp(id, "highXoverHz")) out = p_.highXoverHz;
        else if (!std::strcmp(id, "lowThresholdDb")) out = p_.lowThresholdDb;
        else if (!std::strcmp(id, "lowRatio")) out = p_.lowRatio;
        else if (!std::strcmp(id, "lowGainDb")) out = p_.lowGainDb;
        else if (!std::strcmp(id, "midThresholdDb")) out = p_.midThresholdDb;
        else if (!std::strcmp(id, "midRatio")) out = p_.midRatio;
        else if (!std::strcmp(id, "midGainDb")) out = p_.midGainDb;
        else if (!std::strcmp(id, "highThresholdDb")) out = p_.highThresholdDb;
        else if (!std::strcmp(id, "highRatio")) out = p_.highRatio;
        else if (!std::strcmp(id, "highGainDb")) out = p_.highGainDb;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "multiband"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::MultibandCore   core_;
    dcdsp::MultibandParams p_{}; ParamMailbox<dcdsp::MultibandParams> mail_;
};

// ---------------------------------------------------------------------------
// expander — downward expander, sidechain-capable (dcdsp_expander.h)
// ---------------------------------------------------------------------------
class ExpanderModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = v;
        else if (!std::strcmp(id, "ratio")) p_.ratio = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "rangeDb")) p_.rangeDb = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "ratio")) out = p_.ratio;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "rangeDb")) out = p_.rangeDb;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // ABI v34. An expander's detector is the whole pedal, so keying it is the
    // whole feature: late in a chain it can follow the PICK instead of the
    // hiss the drive in front of it is making.
    bool supportsSidechain() const override { return true; }
    void processSidechained(float* const* ch, int nch, int n,
                            const float* const* key, int keyCh) override
    { mail_.applyPending(core_); core_.process(ch, nch, n, key, keyCh); }
    const char* typeId() const override { return "expander"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::ExpanderCore   core_;
    dcdsp::ExpanderParams p_{}; ParamMailbox<dcdsp::ExpanderParams> mail_;
};

// ---------------------------------------------------------------------------
// optocomp — optical compressor, program-dependent release (dcdsp_optocomp.h)
// ---------------------------------------------------------------------------
class OptoCompModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "peakReduction")) p_.peakReduction = v;
        else if (!std::strcmp(id, "gainDb")) p_.gainDb = v;
        // NAMED discrete (valueLabels on the row): the core runs an int, so the
        // adapter lrounds here and getParam hands the INT back — a UI must never
        // park this knob at 0.5 and show a mode the DSP is not running.
        else if (!std::strcmp(id, "limit")) p_.limit = quantise(v, 0);
        else if (!std::strcmp(id, "emphasis")) p_.emphasis = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "peakReduction")) out = p_.peakReduction;
        else if (!std::strcmp(id, "gainDb")) out = p_.gainDb;
        else if (!std::strcmp(id, "limit")) out = (float) p_.limit;
        else if (!std::strcmp(id, "emphasis")) out = p_.emphasis;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "optocomp"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::OptoCompCore   core_;
    dcdsp::OptoCompParams p_{}; ParamMailbox<dcdsp::OptoCompParams> mail_;
};

// ---------------------------------------------------------------------------
// ducker — sidechain ducker (dcdsp_ducker.h)
// ---------------------------------------------------------------------------
class DuckerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = v;
        else if (!std::strcmp(id, "duckDb")) p_.duckDb = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "holdMs")) p_.holdMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "duckDb")) out = p_.duckDb;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "holdMs")) out = p_.holdMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // ABI v34, and the reason this module exists: key it from the SONG MIX and
    // the rig rides under the backing track. With no key it falls back to its
    // own input, which is the engine's contract — never to silence.
    bool supportsSidechain() const override { return true; }
    void processSidechained(float* const* ch, int nch, int n,
                            const float* const* key, int keyCh) override
    { mail_.applyPending(core_); core_.process(ch, nch, n, key, keyCh); }
    const char* typeId() const override { return "ducker"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::DuckerCore   core_;
    dcdsp::DuckerParams p_{}; ParamMailbox<dcdsp::DuckerParams> mail_;
};

std::unique_ptr<DspModule> makeLimiter()   { return std::make_unique<LimiterModule>(); }
std::unique_ptr<DspModule> makeTransient() { return std::make_unique<TransientModule>(); }
std::unique_ptr<DspModule> makeMultiband() { return std::make_unique<MultibandModule>(); }
std::unique_ptr<DspModule> makeExpander()  { return std::make_unique<ExpanderModule>(); }
std::unique_ptr<DspModule> makeOptoComp()  { return std::make_unique<OptoCompModule>(); }
std::unique_ptr<DspModule> makeDucker()    { return std::make_unique<DuckerModule>(); }

} // namespace

void appendDynamicsGear(std::vector<Entry>& out)
{
    out.push_back({ { "limiter", "Pedal Limiter",
        { { "ceilingDb", "Ceiling (dB)", -20.0f, 0.0f, -1.0f },
          { "releaseMs", "Release (ms)", 5.0f, 500.0f, 80.0f },
          // Reported through latencySamples(), so the chain's PDC budgets for
          // it and a split region stays aligned.
          { "lookaheadMs", "Look-ahead (ms)", 0.0f, 5.0f, 1.0f },
          // INPUT gain: applied before the detector and the delay, which is
          // what keeps the ceiling a guarantee rather than a tendency.
          { "gainDb", "Gain (dB)", -12.0f, 12.0f, 0.0f } } },
        &makeLimiter, kCategoryDynamics });

    out.push_back({ { "transient", "Transient Shaper",
        { { "attackDb", "Attack (dB)", -12.0f, 12.0f, 0.0f },
          { "sustainDb", "Sustain (dB)", -12.0f, 12.0f, 0.0f },
          { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makeTransient, kCategoryDynamics });

    out.push_back({ { "multiband", "Multiband Compressor",
        { { "lowXoverHz", "Low Split (Hz)", 60.0f, 500.0f, 150.0f },
          { "highXoverHz", "High Split (Hz)", 1000.0f, 8000.0f, 2500.0f },
          { "lowThresholdDb", "Low Threshold (dB)", -60.0f, 0.0f, -20.0f },
          { "lowRatio", "Low Ratio", 1.0f, 20.0f, 3.0f },
          { "lowGainDb", "Low Gain (dB)", -12.0f, 12.0f, 0.0f },
          { "midThresholdDb", "Mid Threshold (dB)", -60.0f, 0.0f, -20.0f },
          { "midRatio", "Mid Ratio", 1.0f, 20.0f, 3.0f },
          { "midGainDb", "Mid Gain (dB)", -12.0f, 12.0f, 0.0f },
          { "highThresholdDb", "High Threshold (dB)", -60.0f, 0.0f, -20.0f },
          { "highRatio", "High Ratio", 1.0f, 20.0f, 3.0f },
          { "highGainDb", "High Gain (dB)", -12.0f, 12.0f, 0.0f },
          { "attackMs", "Attack (ms)", 0.1f, 100.0f, 10.0f },
          { "releaseMs", "Release (ms)", 5.0f, 1000.0f, 120.0f } } },
        &makeMultiband, kCategoryDynamics });

    out.push_back({ { "expander", "Expander",
        { { "thresholdDb", "Threshold (dB)", -80.0f, 0.0f, -40.0f },
          { "ratio", "Ratio", 1.0f, 10.0f, 2.0f },
          { "attackMs", "Attack (ms)", 0.1f, 100.0f, 10.0f },
          { "releaseMs", "Release (ms)", 5.0f, 1000.0f, 120.0f },
          { "rangeDb", "Range (dB)", 0.0f, 60.0f, 30.0f } } },
        &makeExpander, kCategoryDynamics, kFlagSidechain });

    out.push_back({ { "optocomp", "Optical Compressor",
        { { "peakReduction", "Peak Reduction", 0.0f, 1.0f, 0.4f },
          { "gainDb", "Gain (dB)", 0.0f, 24.0f, 0.0f },
          // NAMED discrete: two modes, not a 0..1 knob.
          { "limit", "Mode", 0.0f, 1.0f, 0.0f, { "Compress", "Limit" } },
          { "emphasis", "Emphasis", 0.0f, 1.0f, 0.3f } } },
        &makeOptoComp, kCategoryDynamics });

    out.push_back({ { "ducker", "Ducker",
        { { "thresholdDb", "Threshold (dB)", -60.0f, 0.0f, -30.0f },
          { "duckDb", "Duck (dB)", 0.0f, 40.0f, 12.0f },
          { "attackMs", "Attack (ms)", 1.0f, 200.0f, 10.0f },
          { "holdMs", "Hold (ms)", 0.0f, 500.0f, 50.0f },
          { "releaseMs", "Release (ms)", 10.0f, 2000.0f, 300.0f } } },
        &makeDucker, kCategoryDynamics, kFlagSidechain });
}

} // namespace dcrig
