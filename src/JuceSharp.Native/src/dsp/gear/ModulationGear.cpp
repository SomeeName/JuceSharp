// Modulation gear — the "modulation" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendModulationGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include "../cores/dcdsp_tzflanger.h"
#include "../cores/dcdsp_barberpole.h"
#include "../cores/dcdsp_autopan.h"
#include "../cores/dcdsp_ensemble.h"
#include "../cores/dcdsp_freqshift.h"

#include <cmath>
#include <cstring>

namespace dcrig
{
namespace
{

// ---------------------------------------------------------------------------
// tzflanger — thru-zero flanger. The only module in this file that reports
// LATENCY, because its reference path is a FIXED delay.
// ---------------------------------------------------------------------------
class TzFlangerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { sr_ = sr > 0.0 ? sr : 48000.0; core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::isfinite(v)) return false;   // a NaN is rejected, never stored
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depthMs")) p_.depthMs = v;
        else if (!std::strcmp(id, "manualMs")) p_.manualMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "shape")) p_.shape = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depthMs")) out = p_.depthMs;
        else if (!std::strcmp(id, "manualMs")) out = p_.manualMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "shape")) out = (float) p_.shape;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // Answered from the MESSAGE thread's copy of the params and the rate
    // prepare() was given — the core's own copy is written by the audio thread
    // through the mailbox, so reading it here would race the PDC re-derivation
    // that a setParam triggers.
    int latencySamples() const override { return dcdsp::TzFlangerCore::latencySamplesFor(p_.manualMs, sr_); }
    const char* typeId() const override { return "tzflanger"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::TzFlangerCore core_; dcdsp::TzFlangerParams p_{}; ParamMailbox<dcdsp::TzFlangerParams> mail_;
    double sr_ = 48000.0;
};

// ---------------------------------------------------------------------------
// barberpole — endless-rise (Shepard) phaser.
// ---------------------------------------------------------------------------
class BarberPoleModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "stages")) p_.stages = (int) std::lround(v);
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "direction")) p_.direction = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "stages")) out = (float) p_.stages;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "direction")) out = (float) p_.direction;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "barberpole"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::BarberPoleCore core_; dcdsp::BarberPoleParams p_{}; ParamMailbox<dcdsp::BarberPoleParams> mail_;
};

// ---------------------------------------------------------------------------
// autopan — stereo auto-panner / rotary placement.
// ---------------------------------------------------------------------------
class AutoPanModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "shape")) p_.shape = (int) std::lround(v);
        else if (!std::strcmp(id, "phaseDeg")) p_.phaseDeg = v;
        else if (!std::strcmp(id, "law")) p_.law = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "shape")) out = (float) p_.shape;
        else if (!std::strcmp(id, "phaseDeg")) out = p_.phaseDeg;
        else if (!std::strcmp(id, "law")) out = (float) p_.law;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "autopan"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::AutoPanCore core_; dcdsp::AutoPanParams p_{}; ParamMailbox<dcdsp::AutoPanParams> mail_;
};

// ---------------------------------------------------------------------------
// ensemble — Solina/Eminent string ensemble.
// ---------------------------------------------------------------------------
class EnsembleModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "fastRateHz")) p_.fastRateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "voices")) p_.voices = (int) std::lround(v);
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "fastRateHz")) out = p_.fastRateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "voices")) out = (float) p_.voices;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "ensemble"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::EnsembleCore core_; dcdsp::EnsembleParams p_{}; ParamMailbox<dcdsp::EnsembleParams> mail_;
};

// ---------------------------------------------------------------------------
// freqshift — Bode single-sideband frequency shifter.
// ---------------------------------------------------------------------------
class FreqShiftModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "shiftHz")) p_.shiftHz = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "direction")) p_.direction = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "shiftHz")) out = p_.shiftHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "direction")) out = (float) p_.direction;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "freqshift"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::FreqShiftCore core_; dcdsp::FreqShiftParams p_{}; ParamMailbox<dcdsp::FreqShiftParams> mail_;
};

std::unique_ptr<DspModule> makeTzFlanger()  { return std::make_unique<TzFlangerModule>(); }
std::unique_ptr<DspModule> makeBarberPole() { return std::make_unique<BarberPoleModule>(); }
std::unique_ptr<DspModule> makeAutoPan()    { return std::make_unique<AutoPanModule>(); }
std::unique_ptr<DspModule> makeEnsemble()   { return std::make_unique<EnsembleModule>(); }
std::unique_ptr<DspModule> makeFreqShift()  { return std::make_unique<FreqShiftModule>(); }

} // namespace

void appendModulationGear(std::vector<Entry>& out)
{
    out.push_back({ { "tzflanger", "Thru-Zero Flanger",
        { { "rateHz", "Rate (Hz)", 0.02f, 10.0f, 0.3f },
          { "depthMs", "Depth (ms)", 0.1f, 10.0f, 3.0f },
          // The FIXED reference delay, and therefore this module's reported
          // latency. Zero manual = an ordinary flanger with no through-zero.
          { "manualMs", "Manual (ms)", 0.0f, 10.0f, 3.0f },
          { "feedback", "Feedback", -0.95f, 0.95f, 0.3f },
          { "mix", "Mix", 0.0f, 1.0f, 0.5f },
          { "shape", "LFO Shape", 0.0f, 1.0f, 0.0f, { "Sine", "Triangle" } } } },
        &makeTzFlanger, kCategoryModulation });

    out.push_back({ { "barberpole", "Barber-Pole Phaser",
        { { "rateHz", "Rate (Hz)", 0.05f, 5.0f, 0.2f },
          // ORDINAL, not a picker: the cascade only makes notches in pairs, so
          // the core forces the count even and the knob steps by 2.
          { "stages", "Stages", 2.0f, 12.0f, 6.0f, {}, 2.0f },
          { "depth", "Depth", 0.0f, 1.0f, 0.8f },
          { "feedback", "Feedback", -0.9f, 0.9f, 0.3f },
          { "direction", "Direction", 0.0f, 1.0f, 0.0f, { "Up", "Down" } },
          { "mix", "Mix", 0.0f, 1.0f, 0.5f } } },
        &makeBarberPole, kCategoryModulation });

    out.push_back({ { "autopan", "Auto-Pan",
        { { "rateHz", "Rate (Hz)", 0.05f, 10.0f, 1.0f },
          { "depth", "Depth", 0.0f, 1.0f, 1.0f },
          { "shape", "LFO Shape", 0.0f, 2.0f, 0.0f, { "Sine", "Triangle", "Square" } },
          { "phaseDeg", "L/R Phase", 0.0f, 180.0f, 90.0f },
          { "law", "Pan Law", 0.0f, 1.0f, 0.0f, { "Equal Power", "Linear" } } } },
        &makeAutoPan, kCategoryModulation });

    out.push_back({ { "ensemble", "String Ensemble",
        { { "rateHz", "Slow Rate (Hz)", 0.1f, 2.0f, 0.6f },
          { "fastRateHz", "Fast Rate (Hz)", 3.0f, 8.0f, 5.5f },
          { "depth", "Depth", 0.0f, 1.0f, 0.6f },
          { "voices", "Voices", 2.0f, 3.0f, 3.0f, {}, 1.0f },
          { "tone", "Tone", 0.0f, 1.0f, 0.5f },
          { "mix", "Mix", 0.0f, 1.0f, 0.6f } } },
        &makeEnsemble, kCategoryModulation });

    out.push_back({ { "freqshift", "Frequency Shifter",
        { { "shiftHz", "Shift (Hz)", -1000.0f, 1000.0f, 5.0f },
          { "mix", "Mix", 0.0f, 1.0f, 0.5f },
          { "feedback", "Feedback", 0.0f, 0.9f, 0.0f },
          { "direction", "Direction", 0.0f, 2.0f, 0.0f, { "Up", "Down", "Both" } } } },
        &makeFreqShift, kCategoryModulation });
}

} // namespace dcrig
