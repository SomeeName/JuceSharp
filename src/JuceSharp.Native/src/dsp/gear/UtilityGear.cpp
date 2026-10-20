// Utility gear — the "utility" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendUtilityGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"

#include "../cores/dcdsp_looper.h"
#include "../cores/dcdsp_midside.h"
#include "../cores/dcdsp_volume.h"

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

// ---- volume ---------------------------------------------------------------
class VolumeModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "position")) p_.position = v;
        else if (!std::strcmp(id, "curve")) p_.curve = (float) std::lround(v);
        else if (!std::strcmp(id, "minDb")) p_.minDb = v;
        else if (!std::strcmp(id, "maxDb")) p_.maxDb = v;
        else if (!std::strcmp(id, "smoothMs")) p_.smoothMs = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "position")) out = p_.position;
        else if (!std::strcmp(id, "curve")) out = p_.curve;
        else if (!std::strcmp(id, "minDb")) out = p_.minDb;
        else if (!std::strcmp(id, "maxDb")) out = p_.maxDb;
        else if (!std::strcmp(id, "smoothMs")) out = p_.smoothMs;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "volume"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::VolumeCore core_; dcdsp::VolumeParams p_{}; ParamMailbox<dcdsp::VolumeParams> mail_;
};

// ---- midside --------------------------------------------------------------
class MidSideModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "midDb")) p_.midDb = v;
        else if (!std::strcmp(id, "sideDb")) p_.sideDb = v;
        else if (!std::strcmp(id, "width")) p_.width = v;
        else if (!std::strcmp(id, "midPhase")) p_.midPhase = (float) std::lround(v);
        else if (!std::strcmp(id, "sidePhase")) p_.sidePhase = (float) std::lround(v);
        else if (!std::strcmp(id, "monoBelowHz")) p_.monoBelowHz = v;
        else if (!std::strcmp(id, "swap")) p_.swap = (float) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "midDb")) out = p_.midDb;
        else if (!std::strcmp(id, "sideDb")) out = p_.sideDb;
        else if (!std::strcmp(id, "width")) out = p_.width;
        else if (!std::strcmp(id, "midPhase")) out = p_.midPhase;
        else if (!std::strcmp(id, "sidePhase")) out = p_.sidePhase;
        else if (!std::strcmp(id, "monoBelowHz")) out = p_.monoBelowHz;
        else if (!std::strcmp(id, "swap")) out = p_.swap;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "midside"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::MidSideCore core_; dcdsp::MidSideParams p_{}; ParamMailbox<dcdsp::MidSideParams> mail_;
};

// ---- looper ---------------------------------------------------------------
//
// `undo` is a TRIGGER, and the only param in this file whose getParam does not
// read back what setParam was given. Writing >= 0.5 performs one undo and the
// param reads 0 again immediately; the value that crosses the mailbox is a
// monotonically bumped SEQUENCE, because a 1 the message thread reset to 0 on
// its own copy would never be seen to FALL by the audio thread, so a second
// press would not be an edge. A UI renders it as a momentary button, never as a
// latch.
class LooperModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (! finite(v)) return false;
        if (!std::strcmp(id, "state")) p_.state = (float) std::lround(v);
        else if (!std::strcmp(id, "maxLoopS")) p_.maxLoopS = (float) std::lround(v);
        else if (!std::strcmp(id, "loopLevel")) p_.loopLevel = v;
        else if (!std::strcmp(id, "overdubDecay")) p_.overdubDecay = v;
        else if (!std::strcmp(id, "reverse")) p_.reverse = (float) std::lround(v);
        else if (!std::strcmp(id, "halfSpeed")) p_.halfSpeed = (float) std::lround(v);
        else if (!std::strcmp(id, "undo")) { if (v >= 0.5f) p_.undoSeq += 1.0f; }
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        // The transport is the ONE param here the ENGINE can change on its own:
        // a record pass that fills `maxLoopS` closes itself to Play on the audio
        // thread with no param write behind it. Reporting `p_.state` -- the last
        // value WRITTEN -- left a footswitch UI showing "Recording" forever for
        // a loop that was already playing, with no getter anywhere in the ABI
        // that could say otherwise. So: the player's own pending request until
        // the core has APPLIED it, and the live transport after that.
        if (!std::strcmp(id, "state"))
            out = (core_.lastSeenRequest() == (int) std::lround(p_.state))
                ? (float) core_.transportState()
                : p_.state;
        else if (!std::strcmp(id, "maxLoopS")) out = p_.maxLoopS;
        else if (!std::strcmp(id, "loopLevel")) out = p_.loopLevel;
        else if (!std::strcmp(id, "overdubDecay")) out = p_.overdubDecay;
        else if (!std::strcmp(id, "reverse")) out = p_.reverse;
        else if (!std::strcmp(id, "halfSpeed")) out = p_.halfSpeed;
        else if (!std::strcmp(id, "undo")) out = 0.0f;   // a trigger, never a latch
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "looper"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::LooperCore core_; dcdsp::LooperParams p_{}; ParamMailbox<dcdsp::LooperParams> mail_;
};

std::unique_ptr<DspModule> makeVolume()  { return std::make_unique<VolumeModule>(); }
std::unique_ptr<DspModule> makeMidSide() { return std::make_unique<MidSideModule>(); }
std::unique_ptr<DspModule> makeLooper()  { return std::make_unique<LooperModule>(); }

} // namespace

void appendUtilityGear(std::vector<Entry>& out)
{
    out.push_back(
        { { "volume", "Volume / Expression Pedal",
            { { "position", "Position", 0.0f, 1.0f, 1.0f },
              // A NAMED choice of pot taper, not a number: the adapter lrounds
              // it, so a UI slider parked at 1.5 would show a curve the DSP is
              // not running.
              { "curve", "Curve", 0.0f, 3.0f, 0.0f,
                { "Linear", "Log", "Audio", "S-Curve" } },
              { "minDb", "Heel (dB)", -90.0f, 0.0f, -90.0f },
              { "maxDb", "Toe (dB)", -12.0f, 12.0f, 0.0f },
              { "smoothMs", "Smoothing (ms)", 1.0f, 200.0f, 20.0f } } },
          &makeVolume, kCategoryUtility });

    out.push_back(
        { { "midside", "Mid/Side Processor",
            { { "midDb", "Mid (dB)", -24.0f, 12.0f, 0.0f },
              { "sideDb", "Side (dB)", -24.0f, 12.0f, 0.0f },
              { "width", "Width", 0.0f, 2.0f, 1.0f },
              { "midPhase", "Mid Polarity", 0.0f, 1.0f, 0.0f, { "Normal", "Inverted" } },
              { "sidePhase", "Side Polarity", 0.0f, 1.0f, 0.0f, { "Normal", "Inverted" } },
              { "monoBelowHz", "Mono Below (Hz)", 0.0f, 500.0f, 0.0f },
              { "swap", "Swap L/R", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
          &makeMidSide, kCategoryUtility });

    out.push_back(
        { { "looper", "Looper",
            { // The footswitch. A NAMED transport, not a knob.
              { "state", "Transport", 0.0f, 3.0f, 0.0f,
                { "Stop", "Record", "Play", "Overdub" } },
              // An ordinal COUNT of seconds: the record-length cap, NOT a
              // geometry change — both rings are always allocated for the 60 s
              // worst case (dcdsp_looper.h, the memory note).
              { "maxLoopS", "Max Loop (s)", 1.0f, 60.0f, 30.0f, {}, 1.0f },
              { "loopLevel", "Loop Level", 0.0f, 1.0f, 1.0f },
              { "overdubDecay", "Overdub Decay", 0.0f, 1.0f, 1.0f },
              { "reverse", "Reverse", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "halfSpeed", "Half Speed", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              // A momentary TRIGGER: writing Undo performs it and the param
              // reads back 0. See LooperModule's comment.
              { "undo", "Undo", 0.0f, 1.0f, 0.0f, { "-", "Undo" } },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } },
          &makeLooper, kCategoryUtility });
}

} // namespace dcrig
