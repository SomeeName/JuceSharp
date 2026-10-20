// AmpCab gear — the "amp / cab" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendAmpCabGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include <cmath>
#include <cstring>

#include "../cores/dcdsp_bassamp.h"
#include "../cores/dcdsp_acpre.h"
#include "../cores/dcdsp_poweramp.h"
#include "../cores/dcdsp_micpos.h"

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


// --- bassamp ---------------------------------------------------------------
class BassAmpModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "model")) p_.model = quantise(v, 0);
        else if (!std::strcmp(id, "gain")) p_.gain = v;
        else if (!std::strcmp(id, "bass")) p_.bass = v;
        else if (!std::strcmp(id, "lowMid")) p_.lowMid = v;
        else if (!std::strcmp(id, "hiMid")) p_.hiMid = v;
        else if (!std::strcmp(id, "treble")) p_.treble = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "compress")) p_.compress = v;
        else if (!std::strcmp(id, "master")) p_.master = v;
        else if (!std::strcmp(id, "mid")) p_.mid = v;
        else if (!std::strcmp(id, "midHz")) p_.midHz = v;
        else if (!std::strcmp(id, "midQ")) p_.midQ = v;
        else if (!std::strcmp(id, "lowMidOct")) p_.lowMidOct = v;
        else if (!std::strcmp(id, "hiMidOct")) p_.hiMidOct = v;
        else if (!std::strcmp(id, "lowShift")) p_.lowShift = quantise(v, 0);
        else if (!std::strcmp(id, "highShift")) p_.highShift = quantise(v, 0);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "model")) out = (float) p_.model;
        else if (!std::strcmp(id, "gain")) out = p_.gain;
        else if (!std::strcmp(id, "bass")) out = p_.bass;
        else if (!std::strcmp(id, "lowMid")) out = p_.lowMid;
        else if (!std::strcmp(id, "hiMid")) out = p_.hiMid;
        else if (!std::strcmp(id, "treble")) out = p_.treble;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "compress")) out = p_.compress;
        else if (!std::strcmp(id, "master")) out = p_.master;
        else if (!std::strcmp(id, "mid")) out = p_.mid;
        else if (!std::strcmp(id, "midHz")) out = p_.midHz;
        else if (!std::strcmp(id, "midQ")) out = p_.midQ;
        else if (!std::strcmp(id, "lowMidOct")) out = p_.lowMidOct;
        else if (!std::strcmp(id, "hiMidOct")) out = p_.hiMidOct;
        else if (!std::strcmp(id, "lowShift")) out = (float) p_.lowShift;
        else if (!std::strcmp(id, "highShift")) out = (float) p_.highShift;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "bassamp"; }
private:
    dcdsp::BassAmpCore core_; dcdsp::BassAmpParams p_{}; ParamMailbox<dcdsp::BassAmpParams> mail_;
};

// --- acpre -----------------------------------------------------------------
class AcPreModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "notchHz")) p_.notchHz = v;
        else if (!std::strcmp(id, "notchDepthDb")) p_.notchDepthDb = v;
        else if (!std::strcmp(id, "notchQ")) p_.notchQ = v;
        else if (!std::strcmp(id, "phase")) p_.phase = quantise(v, 0);
        else if (!std::strcmp(id, "bass")) p_.bass = v;
        else if (!std::strcmp(id, "mid")) p_.mid = v;
        else if (!std::strcmp(id, "treble")) p_.treble = v;
        else if (!std::strcmp(id, "presence")) p_.presence = v;
        else if (!std::strcmp(id, "piezoFix")) p_.piezoFix = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "notchHz")) out = p_.notchHz;
        else if (!std::strcmp(id, "notchDepthDb")) out = p_.notchDepthDb;
        else if (!std::strcmp(id, "notchQ")) out = p_.notchQ;
        else if (!std::strcmp(id, "phase")) out = (float) p_.phase;
        else if (!std::strcmp(id, "bass")) out = p_.bass;
        else if (!std::strcmp(id, "mid")) out = p_.mid;
        else if (!std::strcmp(id, "treble")) out = p_.treble;
        else if (!std::strcmp(id, "presence")) out = p_.presence;
        else if (!std::strcmp(id, "piezoFix")) out = p_.piezoFix;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "acpre"; }
private:
    dcdsp::AcPreCore core_; dcdsp::AcPreParams p_{}; ParamMailbox<dcdsp::AcPreParams> mail_;
};

// --- poweramp --------------------------------------------------------------
class PowerAmpModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "tubes")) p_.tubes = quantise(v, 2);
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "sag")) p_.sag = v;
        else if (!std::strcmp(id, "presence")) p_.presence = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "tubes")) out = (float) p_.tubes;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "sag")) out = p_.sag;
        else if (!std::strcmp(id, "presence")) out = p_.presence;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "poweramp"; }
private:
    dcdsp::PowerAmpCore core_; dcdsp::PowerAmpParams p_{}; ParamMailbox<dcdsp::PowerAmpParams> mail_;
};

// --- micpos ----------------------------------------------------------------
class MicPosModule : public DspModule
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
        if (!std::strcmp(id, "mic")) p_.mic = quantise(v, 0);
        else if (!std::strcmp(id, "position")) p_.position = v;
        else if (!std::strcmp(id, "distanceCm")) p_.distanceCm = v;
        else if (!std::strcmp(id, "room")) p_.room = v;
        else if (!std::strcmp(id, "blend2")) p_.blend2 = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else if (!std::strcmp(id, "angleDeg")) p_.angleDeg = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mic")) out = (float) p_.mic;
        else if (!std::strcmp(id, "position")) out = p_.position;
        else if (!std::strcmp(id, "distanceCm")) out = p_.distanceCm;
        else if (!std::strcmp(id, "room")) out = p_.room;
        else if (!std::strcmp(id, "blend2")) out = p_.blend2;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else if (!std::strcmp(id, "angleDeg")) out = p_.angleDeg;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // Owner-thread read of a PREPARE-time geometry: both capsules are referred
    // to the 30 cm reference plane, so this is constant and does NOT move when
    // the distance knob does.
    int latencySamples() const override { return latency_; }
    const char* typeId() const override { return "micpos"; }
private:
    int latency_ = 0;
    dcdsp::MicPosCore core_; dcdsp::MicPosParams p_{}; ParamMailbox<dcdsp::MicPosParams> mail_;
};

std::unique_ptr<DspModule> makeBassAmp()  { return std::make_unique<BassAmpModule>(); }
std::unique_ptr<DspModule> makeAcPre()    { return std::make_unique<AcPreModule>(); }
std::unique_ptr<DspModule> makePowerAmp() { return std::make_unique<PowerAmpModule>(); }
std::unique_ptr<DspModule> makeMicPos()   { return std::make_unique<MicPosModule>(); }

} // namespace

void appendAmpCabGear(std::vector<Entry>& out)
{
    out.push_back({ { "bassamp", "Bass Amp",
        { // A NAMED discrete choice: the head. It moves the four tone-stack
          // CENTRES as well as the gain curve and the clip character, which is
          // why the core crossfades two filter banks rather than retuning one.
          { "model", "Model", 0.0f, 3.0f, 0.0f,
            { "Flip-Top", "Tube 300", "Solid 800", "Modern" } },
          { "gain", "Gain", 0.0f, 1.0f, 0.4f },
          // The four bands. The two shelves are BOUNDED; nothing in this
          // module high-passes, because the low B is 31 Hz. +/-20 dB since
          // 2026-10 (#72), so a converted bass tone's deep cuts fit.
          { "bass", "Bass (dB)", -20.0f, 20.0f, 0.0f },
          { "lowMid", "Low Mid (dB)", -20.0f, 20.0f, 0.0f },
          { "hiMid", "High Mid (dB)", -20.0f, 20.0f, 0.0f },
          { "treble", "Treble (dB)", -20.0f, 20.0f, 0.0f },
          { "drive", "Drive", 0.0f, 1.0f, 0.2f },
          { "compress", "Compress", 0.0f, 1.0f, 0.3f },
          { "master", "Master", 0.0f, 1.0f, 0.6f },
          // 2026-10 (#72): the bass-head controls Rocksmith's heads carry
          // (dcdsp_bassamp.h header). Every default is the pre-existing sound.
          { "mid", "Mid (dB)", -20.0f, 20.0f, 0.0f },
          { "midHz", "Mid Freq (Hz)", 100.0f, 5000.0f, 800.0f },
          { "midQ", "Mid Q", 0.3f, 8.0f, 1.0f },
          { "lowMidOct", "Low Mid Freq (oct)", -4.0f, 4.0f, 0.0f },
          { "hiMidOct", "High Mid Freq (oct)", -4.0f, 4.0f, 0.0f },
          { "lowShift", "Ultra Lo", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
          { "highShift", "Ultra Hi", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
        &makeBassAmp, kCategoryAmp });

    out.push_back({ { "acpre", "Acoustic Preamp",
        { { "notchHz", "Notch (Hz)", 60.0f, 500.0f, 120.0f },
          { "notchDepthDb", "Notch Depth (dB)", 0.0f, 30.0f, 0.0f },
          { "notchQ", "Notch Q", 2.0f, 30.0f, 10.0f },
          { "phase", "Phase", 0.0f, 1.0f, 0.0f, { "Normal", "Inverted" } },
          { "bass", "Bass (dB)", -12.0f, 12.0f, 0.0f },
          { "mid", "Mid (dB)", -12.0f, 12.0f, 0.0f },
          { "treble", "Treble (dB)", -12.0f, 12.0f, 0.0f },
          { "presence", "Presence", 0.0f, 1.0f, 0.3f },
          { "piezoFix", "Piezo Fix", 0.0f, 1.0f, 0.0f },
          { "level", "Level", 0.0f, 1.0f, 0.7f } } },
        &makeAcPre, kCategoryAmp });

    out.push_back({ { "poweramp", "Power Amp & Sag",
        { { "tubes", "Tubes", 0.0f, 4.0f, 2.0f,
            { "EL84", "6V6", "EL34", "6L6", "KT88" } },
          { "drive", "Drive", 0.0f, 1.0f, 0.3f },
          { "sag", "Sag", 0.0f, 1.0f, 0.3f },
          { "presence", "Presence", 0.0f, 1.0f, 0.5f },
          { "resonance", "Resonance", 0.0f, 1.0f, 0.5f },
          { "damping", "Damping", 0.0f, 1.0f, 0.5f },
          { "level", "Level", 0.0f, 1.0f, 0.6f } } },
        &makePowerAmp, kCategoryAmp });

    out.push_back({ { "micpos", "Cab Mic Position",
        // 2026-10 (DotCharter #75): a fifth mic, the large-diaphragm TUBE
        // condenser, APPENDED so persisted 0-3 keep their mic; and the
        // capsule's ANGLE off the speaker axis, appended last with a default
        // of 0 (on axis = the pre-#75 sound, sample for sample).
        { { "mic", "Mic", 0.0f, 4.0f, 0.0f, { "57", "121", "421", "Condenser", "Tube" } },
          { "position", "Position (Cap-Edge)", 0.0f, 1.0f, 0.3f },
          { "distanceCm", "Distance (cm)", 0.0f, 30.0f, 5.0f },
          { "room", "Room", 0.0f, 1.0f, 0.0f },
          { "blend2", "Second Mic", 0.0f, 1.0f, 0.0f },
          { "level", "Level", 0.0f, 1.0f, 0.7f },
          { "angleDeg", "Angle (deg)", 0.0f, 60.0f, 0.0f } } },
        &makeMicPos, kCategoryCab });
}

} // namespace dcrig
