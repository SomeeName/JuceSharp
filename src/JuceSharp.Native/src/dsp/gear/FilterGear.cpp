// Filter gear — the "filter" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendFilterGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
//
// One row here is NOT in the filter drawer: `tilt` is a tone control, so it
// carries kCategoryEq. The drawer a module lives in is a UI fact, not a source
// tree fact, and a Tilt EQ filed under "filter" is a tone control a user would
// never find.
#include "../GearEntry.h"

#include "../cores/dcdsp_comb.h"
#include "../cores/dcdsp_svf.h"
#include "../cores/dcdsp_talkbox.h"
#include "../cores/dcdsp_tilt.h"
#include "../cores/dcdsp_vocoder.h"

#include <cmath>
#include <cstring>

namespace dcrig
{
namespace
{

// A NAMED/ORDINAL discrete param reaches an int core field, so the adapter
// lrounds it here and getParam hands the INT back. A NaN must never reach
// std::lround (the result is unspecified), so it is refused first — the chain
// rejects a NaN before this anyway, but a core is not allowed to depend on a
// caller it cannot see.
int quantise(float v, int fallback)
{
    return std::isfinite(v) ? (int) std::lround(v) : fallback;
}

// ---------------------------------------------------------------------------
// svf — multimode state-variable filter, LFO/envelope swept (dcdsp_svf.h)
// ---------------------------------------------------------------------------
class SvfModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mode")) p_.mode = quantise(v, 0);
        else if (!std::strcmp(id, "cutoffHz")) p_.cutoffHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "source")) p_.source = quantise(v, 0);
        else if (!std::strcmp(id, "lfoRateHz")) p_.lfoRateHz = v;
        else if (!std::strcmp(id, "lfoDepth")) p_.lfoDepth = v;
        else if (!std::strcmp(id, "envSens")) p_.envSens = v;
        else if (!std::strcmp(id, "envAttackMs")) p_.envAttackMs = v;
        else if (!std::strcmp(id, "envReleaseMs")) p_.envReleaseMs = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "cutoffHz")) out = p_.cutoffHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "source")) out = (float) p_.source;
        else if (!std::strcmp(id, "lfoRateHz")) out = p_.lfoRateHz;
        else if (!std::strcmp(id, "lfoDepth")) out = p_.lfoDepth;
        else if (!std::strcmp(id, "envSens")) out = p_.envSens;
        else if (!std::strcmp(id, "envAttackMs")) out = p_.envAttackMs;
        else if (!std::strcmp(id, "envReleaseMs")) out = p_.envReleaseMs;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "svf"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::SvfCore   core_;
    dcdsp::SvfParams p_{}; ParamMailbox<dcdsp::SvfParams> mail_;
};

// ---------------------------------------------------------------------------
// comb — tuned delay, feedforward or resonant (dcdsp_comb.h)
// ---------------------------------------------------------------------------
class CombModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "freqHz")) p_.freqHz = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "polarity")) p_.polarity = quantise(v, 1);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "freqHz")) out = p_.freqHz;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "polarity")) out = (float) p_.polarity;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "comb"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::CombCore   core_;
    dcdsp::CombParams p_{}; ParamMailbox<dcdsp::CombParams> mail_;
};

// ---------------------------------------------------------------------------
// vocoder — channel vocoder, sidechain-keyed carrier (dcdsp_vocoder.h)
// ---------------------------------------------------------------------------
class VocoderModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "bands")) p_.bands = quantise(v, 16);
        else if (!std::strcmp(id, "carrier")) p_.carrier = quantise(v, 1);
        else if (!std::strcmp(id, "pitchHz")) p_.pitchHz = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "formantShift")) p_.formantShift = v;
        else if (!std::strcmp(id, "emphasis")) p_.emphasis = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "bands")) out = (float) p_.bands;
        else if (!std::strcmp(id, "carrier")) out = (float) p_.carrier;
        else if (!std::strcmp(id, "pitchHz")) out = p_.pitchHz;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "formantShift")) out = p_.formantShift;
        else if (!std::strcmp(id, "emphasis")) out = p_.emphasis;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // ABI v34, and the reason this module exists at all: the KEY is the
    // CARRIER. Key it from a synth stem or the song mix and the guitar talks.
    // With no key the carrier falls back to the module's own input — the
    // engine's contract — which makes it a near-wire, never silence.
    bool supportsSidechain() const override { return true; }
    void processSidechained(float* const* ch, int nch, int n,
                            const float* const* key, int keyCh) override
    { mail_.applyPending(core_); core_.process(ch, nch, n, key, keyCh); }
    const char* typeId() const override { return "vocoder"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::VocoderCore   core_;
    dcdsp::VocoderParams p_{}; ParamMailbox<dcdsp::VocoderParams> mail_;
};

// ---------------------------------------------------------------------------
// talkbox — three sliding formants + driver saturation (dcdsp_talkbox.h)
// ---------------------------------------------------------------------------
class TalkBoxModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "vowel")) p_.vowel = v;
        else if (!std::strcmp(id, "sweep")) p_.sweep = quantise(v, 0);
        else if (!std::strcmp(id, "lfoRateHz")) p_.lfoRateHz = v;
        else if (!std::strcmp(id, "envSens")) p_.envSens = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "vowel")) out = p_.vowel;
        else if (!std::strcmp(id, "sweep")) out = (float) p_.sweep;
        else if (!std::strcmp(id, "lfoRateHz")) out = p_.lfoRateHz;
        else if (!std::strcmp(id, "envSens")) out = p_.envSens;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "talkbox"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::TalkBoxCore   core_;
    dcdsp::TalkBoxParams p_{}; ParamMailbox<dcdsp::TalkBoxParams> mail_;
};

// ---------------------------------------------------------------------------
// tilt — one-knob shelving pair about a pivot (dcdsp_tilt.h)
// ---------------------------------------------------------------------------
class TiltModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "tilt")) p_.tilt = v;
        else if (!std::strcmp(id, "pivotHz")) p_.pivotHz = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "tilt")) out = p_.tilt;
        else if (!std::strcmp(id, "pivotHz")) out = p_.pivotHz;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "tilt"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::TiltCore   core_;
    dcdsp::TiltParams p_{}; ParamMailbox<dcdsp::TiltParams> mail_;
};

std::unique_ptr<DspModule> makeSvf()     { return std::make_unique<SvfModule>(); }
std::unique_ptr<DspModule> makeComb()    { return std::make_unique<CombModule>(); }
std::unique_ptr<DspModule> makeVocoder() { return std::make_unique<VocoderModule>(); }
std::unique_ptr<DspModule> makeTalkBox() { return std::make_unique<TalkBoxModule>(); }
std::unique_ptr<DspModule> makeTilt()    { return std::make_unique<TiltModule>(); }

} // namespace

void appendFilterGear(std::vector<Entry>& out)
{
    out.push_back({ { "svf", "Multimode Filter",
        // NAMED discrete: four taps of ONE filter, crossfaded on a switch.
        { { "mode", "Mode", 0.0f, 3.0f, 0.0f, { "LowPass", "BandPass", "HighPass", "Notch" } },
          // The VALUE is Hz. A UI is expected to give the knob a logarithmic
          // taper, but nothing about that taper reaches the DSP.
          { "cutoffHz", "Cutoff (Hz)", 20.0f, 20000.0f, 1000.0f },
          { "resonance", "Resonance", 0.0f, 1.0f, 0.3f },
          // Saturation INSIDE the resonance path, which is what makes a high-Q
          // setting sing instead of screaming.
          { "drive", "Drive", 0.0f, 1.0f, 0.0f },
          { "source", "Sweep", 0.0f, 2.0f, 0.0f, { "Manual", "LFO", "Envelope" } },
          { "lfoRateHz", "LFO Rate (Hz)", 0.05f, 20.0f, 1.0f },
          // Octaves of sweep: 1.0 is the full +/- 4.
          { "lfoDepth", "LFO Depth", 0.0f, 1.0f, 0.5f },
          { "envSens", "Env Sensitivity", 0.0f, 1.0f, 0.5f },
          { "envAttackMs", "Env Attack (ms)", 1.0f, 200.0f, 10.0f },
          { "envReleaseMs", "Env Release (ms)", 10.0f, 2000.0f, 200.0f },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makeSvf, kCategoryFilter });

    out.push_back({ { "comb", "Comb Filter",
        { { "freqHz", "Frequency (Hz)", 20.0f, 5000.0f, 220.0f },
          // Negative feedback is an INVERTED comb, not a quieter one.
          { "feedback", "Feedback", -0.98f, 0.98f, 0.7f },
          { "damping", "Damping", 0.0f, 1.0f, 0.3f },
          // NAMED discrete. Defaults to Feedback: with the default feedback of
          // 0.7 a feedforward comb would be the quieter, duller of the two.
          { "polarity", "Type", 0.0f, 1.0f, 1.0f, { "Feedforward", "Feedback" } },
          { "mix", "Mix", 0.0f, 1.0f, 0.5f } } },
        &makeComb, kCategoryFilter });

    out.push_back({ { "vocoder", "Vocoder",
        // ORDINAL discrete (step 4): 8, 12, 16, 20, 24 bands.
        { { "bands", "Bands", 8.0f, 24.0f, 16.0f, {}, 4.0f },
          // NAMED discrete. Sidechain makes the KEY the carrier and this
          // module's own input the modulator.
          { "carrier", "Carrier", 0.0f, 3.0f, 1.0f, { "Sidechain", "Saw", "Pulse", "Noise" } },
          // The internal oscillators only, and FIXED: nothing tracks pitch.
          { "pitchHz", "Pitch (Hz)", 30.0f, 500.0f, 110.0f },
          { "attackMs", "Attack (ms)", 1.0f, 100.0f, 5.0f },
          { "releaseMs", "Release (ms)", 5.0f, 500.0f, 40.0f },
          { "formantShift", "Formant (semitones)", -12.0f, 12.0f, 0.0f },
          { "emphasis", "Emphasis", 0.0f, 1.0f, 0.5f },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makeVocoder, kCategoryFilter, kFlagSidechain });

    out.push_back({ { "talkbox", "Talk Box",
        { { "vowel", "Vowel", 0.0f, 1.0f, 0.5f },
          { "sweep", "Sweep", 0.0f, 2.0f, 0.0f, { "Manual", "Envelope", "LFO" } },
          { "lfoRateHz", "LFO Rate (Hz)", 0.05f, 10.0f, 0.5f },
          { "envSens", "Env Sensitivity", 0.0f, 1.0f, 0.5f },
          { "resonance", "Resonance", 0.0f, 1.0f, 0.6f },
          { "drive", "Drive", 0.0f, 1.0f, 0.3f },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makeTalkBox, kCategoryFilter });

    // A tone control, not a filter — hence kCategoryEq.
    out.push_back({ { "tilt", "Tilt EQ",
        { { "tilt", "Tilt (dB)", -12.0f, 12.0f, 0.0f },
          { "pivotHz", "Pivot (Hz)", 200.0f, 5000.0f, 1000.0f },
          { "level", "Level (dB)", -12.0f, 12.0f, 0.0f } } },
        &makeTilt, kCategoryEq });
}

} // namespace dcrig
