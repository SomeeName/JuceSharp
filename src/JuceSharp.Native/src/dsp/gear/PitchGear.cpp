// Pitch gear — the "pitch" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendPitchGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
//
// The four modules here share three building blocks that live in
// ../cores/dcdsp_detail_pitchgear.h: a two-tap crossfaded delay-line pitch
// shifter, a native autocorrelation pitch tracker (JuceSharp.Dsp's YIN/CQT are
// MANAGED and unreachable from the callback), an LPC formant whiten/re-dress
// pair and a PolyBLEP oscillator. See docs/guides/gear-pitch.md.
#include "../GearEntry.h"

#include "../cores/dcdsp_basssynth.h"
#include "../cores/dcdsp_detune.h"
#include "../cores/dcdsp_pitchcorrect.h"
#include "../cores/dcdsp_twelvestring.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace dcrig
{
namespace
{

// Every adapter here rejects a NON-FINITE value at the door and returns false --
// which is what the header comment above has always claimed and what the other
// four gear files have always done. The shipping path clamps twice before this
// (the ABI rejects non-finite, DspChain::setParam rejects NaN), but a direct
// DspModule::setParam caller -- the native tests are exactly that -- goes
// through neither, and p_ is also what getParam reads back, so a stored NaN is
// a NaN handed to the UI. The cores clamp NaN-safely via detail::clampf, so the
// SOUND was never at risk; the file's own comment was what was false.
inline bool finite(float v) { return std::isfinite(v); }


// ---------------------------------------------------------------------------
// detune — micro-pitch doubler (dcdsp_detune.h)
// ---------------------------------------------------------------------------
class DetuneModule : public DspModule
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
        if (! finite(v)) return false;
        if (!std::strcmp(id, "cents")) p_.cents = v;
        else if (!std::strcmp(id, "spreadMs")) p_.spreadMs = v;
        else if (!std::strcmp(id, "width")) p_.width = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "lowLatency")) { p_.lowLatency = v; publishLatency(); }
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "cents")) out = p_.cents;
        else if (!std::strcmp(id, "spreadMs")) out = p_.spreadMs;
        else if (!std::strcmp(id, "width")) out = p_.width;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "lowLatency")) out = p_.lowLatency;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The window is half the group delay, so the latency mode MOVES it. It is
    // derived on the MESSAGE thread from the same arithmetic the core runs,
    // because DspChain re-derives branch PDC on every accepted param write and
    // would otherwise read a number the audio thread has not applied yet.
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    const char* typeId() const override { return "detune"; }
    void* coreForTest() override { return &core_; }

private:
    void publishLatency()
    {
        const bool lowLat = std::isfinite(p_.lowLatency) && p_.lowLatency >= 0.5f;
        const double winMs = lowLat ? dcdsp::DetuneCore::kWindowShortMs
                                    : dcdsp::DetuneCore::kWindowLongMs;
        const double ms = dcdsp::DetuneCore::kPreDelayMs + 0.5 * winMs;
        int samples = (int) std::lround(sr_ * ms / 1000.0);
        if (samples < 1) samples = 1;
        latency_.store(samples, std::memory_order_relaxed);
    }

    dcdsp::DetuneCore   core_;
    dcdsp::DetuneParams p_{}; ParamMailbox<dcdsp::DetuneParams> mail_;
    double sr_ = 48000.0;
    std::atomic<int> latency_{ 1008 };
};

// ---------------------------------------------------------------------------
// pitchcorrect — scale-snapping pitch correction (dcdsp_pitchcorrect.h)
// ---------------------------------------------------------------------------
class PitchCorrectModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        sr_ = sr > 0.0 ? sr : 48000.0;
        // The splice-alignment search span is PART of the figure, exactly as
        // dcdsp_whammy.h's own search span is part of its latency_: the aligned
        // tap moves by up to +/- kAlignMs around the nominal delay.
        const double ms = dcdsp::PitchCorrectCore::kPreDelayMs
                        + dcdsp::PitchCorrectCore::kAlignMs
                        + 0.5 * dcdsp::PitchCorrectCore::kWindowMs;
        int samples = (int) std::lround(sr_ * ms / 1000.0);
        if (samples < 1) samples = 1;
        latency_.store(samples, std::memory_order_relaxed);
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "scale")) p_.scale = v;
        else if (!std::strcmp(id, "key")) p_.key = v;
        else if (!std::strcmp(id, "speedMs")) p_.speedMs = v;
        else if (!std::strcmp(id, "amount")) p_.amount = v;
        else if (!std::strcmp(id, "humanize")) p_.humanize = v;
        else if (!std::strcmp(id, "formant")) p_.formant = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "scale")) out = p_.scale;
        else if (!std::strcmp(id, "key")) out = p_.key;
        else if (!std::strcmp(id, "speedMs")) out = p_.speedMs;
        else if (!std::strcmp(id, "amount")) out = p_.amount;
        else if (!std::strcmp(id, "humanize")) out = p_.humanize;
        else if (!std::strcmp(id, "formant")) out = p_.formant;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The SHIFTER's group delay only. The tracker's own analysis lag delays no
    // audio — it just means a correction lands a few tens of milliseconds into
    // a new note — so folding it in here would make PDC align every other
    // branch against a delay that is not in the signal.
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    const char* typeId() const override { return "pitchcorrect"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::PitchCorrectCore   core_;
    dcdsp::PitchCorrectParams p_{}; ParamMailbox<dcdsp::PitchCorrectParams> mail_;
    double sr_ = 48000.0;
    std::atomic<int> latency_{ 1008 };
};

// ---------------------------------------------------------------------------
// twelvestring — 12-string course simulator (dcdsp_twelvestring.h)
// ---------------------------------------------------------------------------
class TwelveStringModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "octaveLevel")) p_.octaveLevel = v;
        else if (!std::strcmp(id, "unisonLevel")) p_.unisonLevel = v;
        else if (!std::strcmp(id, "detuneCents")) p_.detuneCents = v;
        else if (!std::strcmp(id, "delayMs")) p_.delayMs = v;
        else if (!std::strcmp(id, "brightness")) p_.brightness = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "octaveLevel")) out = p_.octaveLevel;
        else if (!std::strcmp(id, "unisonLevel")) out = p_.unisonLevel;
        else if (!std::strcmp(id, "detuneCents")) out = p_.detuneCents;
        else if (!std::strcmp(id, "delayMs")) out = p_.delayMs;
        else if (!std::strcmp(id, "brightness")) out = p_.brightness;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "twelvestring"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::TwelveStringCore   core_;
    dcdsp::TwelveStringParams p_{}; ParamMailbox<dcdsp::TwelveStringParams> mail_;
};

// ---------------------------------------------------------------------------
// basssynth — tracking synth voice (dcdsp_basssynth.h)
// ---------------------------------------------------------------------------
class BassSynthModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "waveform")) p_.waveform = v;
        else if (!std::strcmp(id, "octave")) p_.octave = v;
        else if (!std::strcmp(id, "cutoffHz")) p_.cutoffHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "envAmount")) p_.envAmount = v;
        else if (!std::strcmp(id, "decayMs")) p_.decayMs = v;
        else if (!std::strcmp(id, "glideMs")) p_.glideMs = v;
        else if (!std::strcmp(id, "dryMix")) p_.dryMix = v;
        else if (!std::strcmp(id, "synthMix")) p_.synthMix = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "waveform")) out = p_.waveform;
        else if (!std::strcmp(id, "octave")) out = p_.octave;
        else if (!std::strcmp(id, "cutoffHz")) out = p_.cutoffHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "envAmount")) out = p_.envAmount;
        else if (!std::strcmp(id, "decayMs")) out = p_.decayMs;
        else if (!std::strcmp(id, "glideMs")) out = p_.glideMs;
        else if (!std::strcmp(id, "dryMix")) out = p_.dryMix;
        else if (!std::strcmp(id, "synthMix")) out = p_.synthMix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override
    { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "basssynth"; }
    void* coreForTest() override { return &core_; }

private:
    dcdsp::BassSynthCore   core_;
    dcdsp::BassSynthParams p_{}; ParamMailbox<dcdsp::BassSynthParams> mail_;
};

std::unique_ptr<DspModule> makeDetune()       { return std::make_unique<DetuneModule>(); }
std::unique_ptr<DspModule> makePitchCorrect() { return std::make_unique<PitchCorrectModule>(); }
std::unique_ptr<DspModule> makeTwelveString() { return std::make_unique<TwelveStringModule>(); }
std::unique_ptr<DspModule> makeBassSynth()    { return std::make_unique<BassSynthModule>(); }

} // namespace

void appendPitchGear(std::vector<Entry>& out)
{
    out.push_back({ { "detune", "Detune / Micro-Pitch",
        { { "cents", "Detune (cents)", 0.0f, 50.0f, 12.0f },
          { "spreadMs", "Spread (ms)", 0.0f, 30.0f, 8.0f },
          { "width", "Width", 0.0f, 1.0f, 1.0f },
          { "mix", "Mix", 0.0f, 1.0f, 0.5f },
          // NAMED discrete: the short window halves the group delay, which is
          // what a player monitoring themselves hears, at the cost of a little
          // low-end smear. Reported through latencySamples().
          { "lowLatency", "Latency", 0.0f, 1.0f, 0.0f, { "Normal", "Low" } } } },
        &makeDetune, kCategoryPitch });

    out.push_back({ { "pitchcorrect", "Pitch Correction",
        { { "scale", "Scale", 0.0f, 4.0f, 0.0f,
            { "Chromatic", "Major", "Minor", "Pentatonic", "Blues" } },
          { "key", "Key", 0.0f, 11.0f, 0.0f,
            { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" } },
          { "speedMs", "Speed (ms)", 0.0f, 500.0f, 50.0f },
          { "amount", "Amount", 0.0f, 1.0f, 1.0f },
          { "humanize", "Humanize", 0.0f, 1.0f, 0.0f },
          { "formant", "Formant", 0.0f, 1.0f, 0.0f, { "Off", "Preserve" } },
          { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
        &makePitchCorrect, kCategoryPitch });

    out.push_back({ { "twelvestring", "12-String Simulator",
        { { "octaveLevel", "Octave Courses", 0.0f, 1.0f, 0.6f },
          { "unisonLevel", "Unison Courses", 0.0f, 1.0f, 0.6f },
          { "detuneCents", "Unison Detune (cents)", 0.0f, 25.0f, 8.0f },
          { "delayMs", "Course Offset (ms)", 0.0f, 20.0f, 6.0f },
          { "brightness", "Brightness", 0.0f, 1.0f, 0.5f },
          { "mix", "Course Level", 0.0f, 1.0f, 0.6f } } },
        &makeTwelveString, kCategoryPitch });

    out.push_back({ { "basssynth", "Bass Synth Tracker",
        { { "waveform", "Waveform", 0.0f, 3.0f, 0.0f,
            { "Square", "Saw", "Sub-Sine", "PWM" } },
          { "octave", "Octave", 0.0f, 2.0f, 1.0f, { "-2", "-1", "0" } },
          { "cutoffHz", "Cutoff (Hz)", 40.0f, 8000.0f, 800.0f },
          { "resonance", "Resonance", 0.0f, 1.0f, 0.4f },
          { "envAmount", "Env -> Cutoff", 0.0f, 1.0f, 0.6f },
          { "decayMs", "Decay (ms)", 10.0f, 2000.0f, 300.0f },
          { "glideMs", "Glide (ms)", 0.0f, 300.0f, 20.0f },
          { "dryMix", "Dry", 0.0f, 1.0f, 0.3f },
          { "synthMix", "Synth", 0.0f, 1.0f, 0.7f } } },
        &makeBassSynth, kCategoryPitch });
}

} // namespace dcrig
