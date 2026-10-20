#include "DspModuleFactory.h"
#include "GearEntry.h"
#include "IrMailbox.h"
#include "IrWavLoader.h"
#include "RoutingModules.h" // splitter / merger (ABI v23 split/parallel chains)

#include <cmath>
#include <cstring>
#include <string>

// Module implementations. Each real effect wraps a pure `dcdsp` core (authored
// + numerically self-tested separately) in the thin DspModule interface,
// mapping stable string param ids onto the core's params struct. The catalogue
// metadata below MUST mirror each module's param ids/ranges — the managed UI
// trusts it. Cores are folded in one #include + adapter + catalogue entry each
// as they land.
//
// PARAM HANDOFF (2026-07-16 audit #4): setParam updates the adapter's own p_
// (so getParam keeps its clamp/lround read-back semantics) and then PUBLISHES
// the struct via mail_ (a ParamMailbox — see DspModule.h); process() applies
// it on the audio thread before the core runs. Never call core_.setParams
// directly from setParam — that recomputes multi-word coefficient sets while
// process() may be reading them.
#include "cores/dcdsp_gate.h"
#include "cores/dcdsp_compressor.h"
#include "cores/dcdsp_eq.h"
#include "cores/dcdsp_tremolo.h"
#include "cores/dcdsp_chorus.h"
#include "cores/dcdsp_flanger.h"
#include "cores/dcdsp_delay.h"
#include "cores/dcdsp_reverb.h"
#include "cores/dcdsp_overdrive.h"
#include "cores/dcdsp_distortion.h"
#include "cores/dcdsp_amp.h"
#include "cores/dcdsp_dualcab.h"
#include "cores/dcdsp_fuzz.h"
#include "cores/dcdsp_phaser.h"
#include "cores/dcdsp_wah.h"
#include "cores/dcdsp_autowah.h"
#include "cores/dcdsp_octaver.h"
#include "cores/dcdsp_ringmod.h"
#include "cores/dcdsp_bitcrusher.h"
#include "cores/dcdsp_tapedelay.h"
#include "cores/dcdsp_platereverb.h"
#include "cores/dcdsp_rotary.h"
#include "cores/dcdsp_formant.h"
#include "cores/dcdsp_graphiceq.h"
#include "cores/dcdsp_harmonizer.h"
#include "cores/dcdsp_vibrato.h"
#include "cores/dcdsp_sustainer.h"
#include "cores/dcdsp_springreverb.h"
#include "cores/dcdsp_shimmer.h"
#include "cores/dcdsp_univibe.h"
#include "cores/dcdsp_harmtremolo.h"
#include "cores/dcdsp_envfilter.h"
#include "cores/dcdsp_doubler.h"
#include "cores/dcdsp_acousticsim.h"
#include "cores/dcdsp_noisereducer.h"
#include "cores/dcdsp_klon.h"
#include "cores/dcdsp_bigmuff.h"
#include "cores/dcdsp_miku.h"
#include "cores/dcdsp_mikustomp.h"
#include "cores/dcdsp_boost.h"
#include "cores/dcdsp_dimension.h"
// 0.18 expressive/performance wave. Twelve new pure cores (the shared
// `cores/dcdsp_detail_pitch.h` period tracker is a DETAIL header — no module,
// no catalogue entry).
#include "cores/dcdsp_swell.h"
#include "cores/dcdsp_whammy.h"
#include "cores/dcdsp_reversedelay.h"
#include "cores/dcdsp_slapback.h"
#include "cores/dcdsp_widener.h"
#include "cores/dcdsp_freeze.h"
#include "cores/dcdsp_subharmonic.h"
#include "cores/dcdsp_filterseq.h"
#include "cores/dcdsp_feedbacker.h"
#include "cores/dcdsp_resonator.h"
#include "cores/dcdsp_tapestop.h"
#include "cores/dcdsp_glitch.h"

// Neural amp capture playback ("namamp", 0.16 §4.4). Its NeuralAudio
// dependency stays scoped to NamAmpModule.cpp; this TU only sees the
// factory hook + catalogue metadata.
#if DOTCHARTER_DSP_NEURAL
  #include "NamAmpModule.h"
#endif

namespace dcrig
{
namespace
{

// The rate an IrMailbox payload carries when there is no IR (its own Payload
// default). Spelled once so the three "clear the slot" call sites do not read
// as four independent rate DECISIONS, which none of them is — publish()
// short-circuits on the null pointer before the rate is looked at.
constexpr double kIrMailboxDefaultRate = 48000.0;

// ---- Built-in clean boost (no external core; proves the framework) ---------
class CleanGainModule : public DspModule
{
public:
    void prepare(double, int, int) override { primed_ = false; }
    void reset() override {}
    bool setParam(const char* id, float v) override
    {
        if (std::strcmp(id, "gainDb") == 0) { gainDb_.store(v, std::memory_order_relaxed); return true; }
        return false;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (std::strcmp(id, "gainDb") == 0) { out = gainDb_.load(std::memory_order_relaxed); return true; }
        return false;
    }
    void process(float* const* ch, int nch, int n) override
    {
        // Ramped across the block from the gain the PREVIOUS block ended on —
        // the same rule as AudioEngine::rampOrScale for the engine's own
        // faders. One constant per block was a zipper on every slider tick
        // and a full-amplitude step on a preset switch.
        const float target = std::pow(10.0f, gainDb_.load(std::memory_order_relaxed) / 20.0f);
        if (! primed_) { lastLin_ = target; primed_ = true; }
        const float start = lastLin_;
        if (n <= 0) return;
        if (start == target)
        {
            for (int c = 0; c < nch; ++c)
                if (ch[c] != nullptr)
                    for (int i = 0; i < n; ++i) ch[c][i] *= target;
        }
        else
        {
            const float inc = (target - start) / (float) n;
            for (int c = 0; c < nch; ++c)
            {
                if (ch[c] == nullptr) continue;
                float g = start;
                // MULTIPLY, THEN ADVANCE. Advancing first scaled sample 0 by
                // start+inc, i.e. one whole increment of discontinuity against
                // the previous block's last sample on every ramping block —
                // which is the zipper this ramp exists to remove, one step
                // smaller. juce::AudioBuffer::applyGainRamp, the reference this
                // code's own comment invokes above, applies `start` to sample 0.
                for (int i = 0; i < n; ++i) { ch[c][i] *= g; g += inc; }
            }
        }
        lastLin_ = target;
    }
    const char* typeId() const override { return "gain"; }

private:
    // Single scalar: a plain atomic is the whole handoff (no coefficients to tear).
    std::atomic<float> gainDb_{ 0.0f };
    float lastLin_ = 1.0f;   // audio-thread-only: where the last block ended
    bool  primed_  = false;
};

// ---- Adapters wrapping pure dcdsp cores ------------------------------------

class GateModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = v;
        else if (!std::strcmp(id, "ratio")) p_.ratio = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "holdMs")) p_.holdMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "autoFloor")) p_.autoFloor = v >= 0.5f ? 1 : 0;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "ratio")) out = p_.ratio;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "holdMs")) out = p_.holdMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "autoFloor")) out = (float) p_.autoFloor;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // ABI v34. The gate's envelope follower is the whole pedal, so keying it
    // from somewhere else is the entire feature: a gate late in the chain can
    // follow the PICK instead of the drive's noise floor ahead of it.
    bool supportsSidechain() const override { return true; }
    void processSidechained(float* const* ch, int nch, int n,
                            const float* const* key, int keyCh) override
    { mail_.applyPending(core_); core_.process(ch, nch, n, key, keyCh); }
    const char* typeId() const override { return "gate"; }
private:
    dcdsp::NoiseGateCore core_;
    dcdsp::GateParams    p_{}; ParamMailbox<dcdsp::GateParams> mail_;
};

class CompressorModule : public DspModule
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
        else if (!std::strcmp(id, "makeupDb")) p_.makeupDb = v;
        else if (!std::strcmp(id, "kneeDb")) p_.kneeDb = v;
        else if (!std::strcmp(id, "detector")) p_.detector = v;
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
        else if (!std::strcmp(id, "makeupDb")) out = p_.makeupDb;
        else if (!std::strcmp(id, "kneeDb")) out = p_.kneeDb;
        else if (!std::strcmp(id, "detector")) out = p_.detector;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // ABI v34. The core's "sidechain" was always an internal 100 Hz high-pass
    // on its OWN input — a detector shape, not a sidechain. With a key it is
    // the real thing: duck the rig under the song, or compress off the dry
    // pick rather than off whatever the drive ahead of it produced.
    bool supportsSidechain() const override { return true; }
    void processSidechained(float* const* ch, int nch, int n,
                            const float* const* key, int keyCh) override
    { mail_.applyPending(core_); core_.process(ch, nch, n, key, keyCh); }
    const char* typeId() const override { return "compressor"; }
private:
    dcdsp::CompressorCore   core_;
    dcdsp::CompressorParams p_{}; ParamMailbox<dcdsp::CompressorParams> mail_;
};

class EqModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "lowFreq")) p_.lowFreq = v;
        else if (!std::strcmp(id, "lowGainDb")) p_.lowGainDb = v;
        else if (!std::strcmp(id, "midFreq")) p_.midFreq = v;
        else if (!std::strcmp(id, "midQ")) p_.midQ = v;
        else if (!std::strcmp(id, "midGainDb")) p_.midGainDb = v;
        else if (!std::strcmp(id, "highFreq")) p_.highFreq = v;
        else if (!std::strcmp(id, "highGainDb")) p_.highGainDb = v;
        else if (!std::strcmp(id, "outputTrimDb")) p_.outputTrimDb = v;
        else return false;
        mail_.publish(p_);
        return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "lowFreq")) out = p_.lowFreq;
        else if (!std::strcmp(id, "lowGainDb")) out = p_.lowGainDb;
        else if (!std::strcmp(id, "midFreq")) out = p_.midFreq;
        else if (!std::strcmp(id, "midQ")) out = p_.midQ;
        else if (!std::strcmp(id, "midGainDb")) out = p_.midGainDb;
        else if (!std::strcmp(id, "highFreq")) out = p_.highFreq;
        else if (!std::strcmp(id, "highGainDb")) out = p_.highGainDb;
        else if (!std::strcmp(id, "outputTrimDb")) out = p_.outputTrimDb;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "eq"; }
private:
    dcdsp::EqCore   core_;
    dcdsp::EqParams p_{}; ParamMailbox<dcdsp::EqParams> mail_;
};

class TremoloModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "shape")) p_.shape = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "shape")) out = (float) p_.shape;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "tremolo"; }
private:
    dcdsp::TremoloCore core_; dcdsp::TremoloParams p_{}; ParamMailbox<dcdsp::TremoloParams> mail_;
};

class ChorusModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depthMs")) p_.depthMs = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "voices")) p_.voices = (int) std::lround(v);
        // 0.18 BBD pass — every one of these is a NO-OP at its default, so the
        // pre-0.18 chorus sound is bit-identical (wetHpfHz == 20 means BYPASSED,
        // not "a 20 Hz filter"; see dcdsp_chorus.h).
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "spread")) p_.spread = v;
        else if (!std::strcmp(id, "lfoShape")) p_.lfoShape = (int) std::lround(v);
        else if (!std::strcmp(id, "wetHpfHz")) p_.wetHpfHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depthMs")) out = p_.depthMs;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "voices")) out = (float) p_.voices;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "spread")) out = p_.spread;
        else if (!std::strcmp(id, "lfoShape")) out = (float) p_.lfoShape;
        else if (!std::strcmp(id, "wetHpfHz")) out = p_.wetHpfHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "chorus"; }
private:
    dcdsp::ChorusCore core_; dcdsp::ChorusParams p_{}; ParamMailbox<dcdsp::ChorusParams> mail_;
};

class FlangerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depthMs")) p_.depthMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depthMs")) out = p_.depthMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "flanger"; }
private:
    dcdsp::FlangerCore core_; dcdsp::FlangerParams p_{}; ParamMailbox<dcdsp::FlangerParams> mail_;
};

class DelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "pingPong")) p_.pingPong = (v != 0.0f);
        else if (!std::strcmp(id, "toneHz")) p_.toneHz = v;
        // These four isfinite guards are NOT dead, and the wave-4 audit's claim
        // that they are (W4-D55) is true only of the SHIPPING path: the ABI
        // rejects every non-finite float and DspChain::setParam rejects NaN
        // again, but a direct DspModule::setParam caller passes through neither,
        // and tests/dsp/delay_sync_trails.cpp is exactly that caller — it
        // asserts setParam("division", NaN) returns false ("invalid division
        // rejected before rounding"). Removing them is a module-contract change,
        // not a clean-up. They stay.
        else if (!std::strcmp(id, "sync")) { if (!std::isfinite(v)) return false; p_.sync = v >= 0.5f; }
        else if (!std::strcmp(id, "bpm")) { if (!std::isfinite(v)) return false; p_.bpm = std::clamp(v, 20.0f, 400.0f); }
        else if (!std::strcmp(id, "division")) { if (!std::isfinite(v)) return false; p_.division = (int)std::lround(std::clamp(v, 0.0f, 8.0f)); }
        else if (!std::strcmp(id, "trails")) { if (!std::isfinite(v)) return false; p_.trails = v >= 0.5f; }
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "pingPong")) out = p_.pingPong ? 1.0f : 0.0f;
        else if (!std::strcmp(id, "toneHz")) out = p_.toneHz;
        else if (!std::strcmp(id, "sync")) out = p_.sync ? 1.0f : 0.0f;
        else if (!std::strcmp(id, "bpm")) out = p_.bpm;
        else if (!std::strcmp(id, "division")) out = (float)p_.division;
        else if (!std::strcmp(id, "trails")) out = p_.trails ? 1.0f : 0.0f;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    bool ownsBypassProcessing() const override { return true; }
    void requestBypassReset() override { resetPending_.store(true, std::memory_order_release); }
    void processBypass(float* const* ch, int nch, int n, float start, float target, float step) override {
        if (mail_.consume(pendingParams_)) hasPendingParams_ = true;
        // Generic hard-bypass never consumes coefficient edits while parked.
        // Keep only the latest snapshot until it runs, preserving initial snap
        // and time-glide behavior when old presets are edited while bypassed.
        if (start == 0.0f && target == 0.0f && !core_.needsBypassProcessing()
            && (!hasPendingParams_ || !pendingParams_.trails)) {
            core_.processBypass(ch, nch, n, start, target, step);
            return;
        }
        const bool nextTrails = hasPendingParams_ ? pendingParams_.trails : audioTrails_;
        // Reset precedes coefficient application, exactly like the generic
        // parked path. Live trails are never reset when input resumes.
        if (resetPending_.exchange(false, std::memory_order_acquire) && !nextTrails) core_.reset();
        if (hasPendingParams_) {
            audioTrails_ = pendingParams_.trails;
            core_.setParams(pendingParams_);
            hasPendingParams_ = false;
        }
        core_.processBypass(ch, nch, n, start, target, step);
    }
    const char* typeId() const override { return "delay"; }
private:
    std::atomic<bool> resetPending_{false};
    bool audioTrails_ = false, hasPendingParams_ = false;
    dcdsp::DelayParams pendingParams_{};
    dcdsp::DelayCore core_; dcdsp::DelayParams p_{}; ParamMailbox<dcdsp::DelayParams> mail_;
};

class ReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "roomSize")) p_.roomSize = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "width")) p_.width = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "roomSize")) out = p_.roomSize;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "width")) out = p_.width;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "reverb"; }
private:
    dcdsp::ReverbCore core_; dcdsp::ReverbParams p_{}; ParamMailbox<dcdsp::ReverbParams> mail_;
};

class OverdriveModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override {
        core_.prepare(sr, mb, nch);
        dryLatency_ = core_.dryCompensationLatencySamples();
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else if (!std::strcmp(id, "model")) p_.model = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else if (!std::strcmp(id, "model")) out = (float) p_.model;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The chain queries metadata under its owner mutex, independently of the
    // callback. p_ and dryLatency_ are owner-written; core_.blend_ is not.
    //
    // Whether a model carries the dry-compensation delay is the MODEL TABLE's
    // decision (kModels[m].blend < 1), so ask the core's owner-safe predicate
    // rather than re-deriving it from the index: `model >= 3` was true only
    // because row 3 happened to be the sole blend below 1.0, and a reordered
    // or extended table would have made the PDC export and the merger's
    // branch alignment lie with nothing failing (W4-D53).
    int latencySamples() const override { return dcdsp::OverdriveCore::modelBlendsDry(p_.model) ? dryLatency_ : 0; }
    const char* typeId() const override { return "overdrive"; }
private:
    int dryLatency_ = 0;
    dcdsp::OverdriveCore core_; dcdsp::OverdriveParams p_{}; ParamMailbox<dcdsp::OverdriveParams> mail_;
};

class DistortionModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "gain")) p_.gain = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else if (!std::strcmp(id, "tightHz")) p_.tightHz = v;
        else if (!std::strcmp(id, "model")) p_.model = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "gain")) out = p_.gain;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else if (!std::strcmp(id, "tightHz")) out = p_.tightHz;
        else if (!std::strcmp(id, "model")) out = (float) p_.model;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "distortion"; }
private:
    dcdsp::DistortionCore core_; dcdsp::DistortionParams p_{}; ParamMailbox<dcdsp::DistortionParams> mail_;
};

class AmpModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "gain")) p_.gain = v;
        else if (!std::strcmp(id, "bass")) p_.bass = v;
        else if (!std::strcmp(id, "mid")) p_.mid = v;
        else if (!std::strcmp(id, "treble")) p_.treble = v;
        else if (!std::strcmp(id, "presence")) p_.presence = v;
        else if (!std::strcmp(id, "master")) p_.master = v;
        else if (!std::strcmp(id, "voicing")) p_.voicing = (int) std::lround(v);
        else if (!std::strcmp(id, "bright")) p_.bright = (int) std::lround(v);
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "gain")) out = p_.gain;
        else if (!std::strcmp(id, "bass")) out = p_.bass;
        else if (!std::strcmp(id, "mid")) out = p_.mid;
        else if (!std::strcmp(id, "treble")) out = p_.treble;
        else if (!std::strcmp(id, "presence")) out = p_.presence;
        else if (!std::strcmp(id, "master")) out = p_.master;
        else if (!std::strcmp(id, "voicing")) out = (float) p_.voicing;
        else if (!std::strcmp(id, "bright")) out = (float) p_.bright;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "amp"; }
private:
    dcdsp::AmpCore core_; dcdsp::AmpParams p_{}; ParamMailbox<dcdsp::AmpParams> mail_;
};

class CabModule : public DspModule
{
public:
    // Owner thread (DspChain::prepare holds writeMutex_ AND processMutex_, so
    // no callback is in flight). Both rate conversions this module can ever
    // need happen here or in setParam/setStringParam — never on the callback.
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        engineRate_ = sr > 0.0 ? sr : 48000.0;
        irMail_.setEngineRate(engineRate_);
        irMailB_.setEngineRate(engineRate_);
        publishModel();
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "model")) { p_.model = (int) std::lround(v); publishModel(); }
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "lowCutHz")) p_.lowCutHz = v;
        else if (!std::strcmp(id, "highCutHz")) p_.highCutHz = v;
        else if (!std::strcmp(id, "blend")) p_.blend = v;
        else if (!std::strcmp(id, "gainADb")) p_.gainADb = v;
        else if (!std::strcmp(id, "gainBDb")) p_.gainBDb = v;
        else if (!std::strcmp(id, "invertA")) p_.invertA = v >= .5f;
        else if (!std::strcmp(id, "invertB")) p_.invertB = v >= .5f;
        else if (!std::strcmp(id, "delayAMs")) p_.delayAMs = v;
        else if (!std::strcmp(id, "delayBMs")) p_.delayBMs = v;
        else if (!std::strcmp(id, "outputDb")) p_.outputDb = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "model")) out = (float) p_.model;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "lowCutHz")) out = p_.lowCutHz;
        else if (!std::strcmp(id, "highCutHz")) out = p_.highCutHz;
        else if (!std::strcmp(id, "blend")) out = p_.blend;
        else if (!std::strcmp(id, "gainADb")) out = p_.gainADb;
        else if (!std::strcmp(id, "gainBDb")) out = p_.gainBDb;
        else if (!std::strcmp(id, "invertA")) out = p_.invertA ? 1.f : 0.f;
        else if (!std::strcmp(id, "invertB")) out = p_.invertB ? 1.f : 0.f;
        else if (!std::strcmp(id, "delayAMs")) out = p_.delayAMs;
        else if (!std::strcmp(id, "delayBMs")) out = p_.delayBMs;
        else if (!std::strcmp(id, "outputDb")) out = p_.outputDb;
        else return false;
        return true;
    }
    // User IR loading (ABI v22): "irPath" (A) / "irPathB" point at WAV IRs.
    // Each independently retains its previous kernel on failure and adopts the
    // latest published snapshot at the next block. "irPath" points at a WAV impulse response
    // (mono / first channel, capped at 4096 taps); "" reverts to the bundled
    // bank for A; an empty B is absent and leaves A at full strength. The
    // parse happens here on the message thread — never
    // on the audio path. On a failed load the previous IR stays active and
    // false is reported through the ABI.
    bool setStringParam(const char* id, const char* v) override
    {
        const bool slotB = std::strcmp(id, "irPathB") == 0;
        if (!slotB && std::strcmp(id, "irPath") != 0) return false;
        IrMailbox& mailbox = slotB ? irMailB_ : irMail_;
        std::string& storedPath = slotB ? irPathB_ : irPath_;
        const std::string path = (v != nullptr) ? v : "";
        // The rate is unread on the clearing path (publish short-circuits on a
        // null pointer); it is spelled as the mailbox default, not as a rate
        // decision.
        if (path.empty()) { mailbox.publish(nullptr, 0, kIrMailboxDefaultRate); storedPath.clear(); return true; }
        irwav::IrData ir;
        // IrMailbox::kMaxTaps, not a literal: publish() silently truncates with
        // std::min(count, kMaxTaps), so raising one without the other drops the
        // excess taps with no error anywhere.
        if (!irwav::loadIrWav(path, IrMailbox::kMaxTaps, ir)) return false;
        mailbox.publish(ir.samples.data(), (int) ir.samples.size(), ir.sampleRate);
        storedPath = path;
        return true;
    }
    bool getStringParam(const char* id, std::string& out) const override
    {
        if (std::strcmp(id, "irPathB") == 0) out = irPathB_;
        else if (std::strcmp(id, "irPath") == 0) out = irPath_;
        else return false;
        return true;
    }
    // Adoption order is deliberate: the model kernel first, then the user IRs
    // (a user IR overrides the bank), then the ordinary params. Every one of
    // these is now a bounded copy — the Lanczos conversion happens in the
    // mailbox, on the thread that published.
    void process(float* const* ch, int nch, int n) override
    {
        modelMail_.applyPending(core_.slotA());
        irMail_.applyPending(core_.slotA());
        irMailB_.applyPending(core_.slotB());
        mail_.applyPending(core_);
        core_.process(ch, nch, n);
    }
    const char* typeId() const override { return "cab"; }
    void* coreForTest() override { return &core_; }
private:
    /// Owner thread. Slot B is deliberately NOT given a bundled kernel: it is
    /// active only when a real custom IR is loaded into it, so converting the
    /// bank for it was pure waste (DualCabCore::setParams forwarded `model` to
    /// both slots, so B re-resampled on every model switch even unused).
    void publishModel()
    {
        const int m = p_.model < 0 ? 0 : (p_.model >= dcdsp::cabir::kNumModels
                                          ? dcdsp::cabir::kNumModels - 1 : p_.model);
        modelMail_.publish(m, dcdsp::cabir::kIr48k[m], dcdsp::cabir::kIrLen,
                           dcdsp::cabir::kIrRate, engineRate_);
    }

    dcdsp::DualCabCore core_; dcdsp::DualCabParams p_{}; ParamMailbox<dcdsp::DualCabParams> mail_;
    IrMailbox irMail_, irMailB_;
    ModelIrMailbox modelMail_;
    double engineRate_ = 48000.0;
    std::string irPath_, irPathB_;
};

class FuzzModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "fuzz")) p_.fuzz = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "fuzz")) out = p_.fuzz;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "fuzz"; }
private:
    dcdsp::FuzzCore core_; dcdsp::FuzzParams p_{}; ParamMailbox<dcdsp::FuzzParams> mail_;
};

class PhaserModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "stages")) p_.stages = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "stages")) out = (float) p_.stages;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "phaser"; }
private:
    dcdsp::PhaserCore core_; dcdsp::PhaserParams p_{}; ParamMailbox<dcdsp::PhaserParams> mail_;
};

class WahModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mode")) p_.mode = (int) std::lround(v);
        else if (!std::strcmp(id, "position")) p_.position = v;
        else if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "rangeLoHz")) p_.rangeLoHz = v;
        else if (!std::strcmp(id, "rangeHiHz")) p_.rangeHiHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "position")) out = p_.position;
        else if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "rangeLoHz")) out = p_.rangeLoHz;
        else if (!std::strcmp(id, "rangeHiHz")) out = p_.rangeHiHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "wah"; }
private:
    dcdsp::WahCore core_; dcdsp::WahParams p_{}; ParamMailbox<dcdsp::WahParams> mail_;
};

class AutoWahModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else if (!std::strcmp(id, "rangeLo")) p_.rangeLoHz = v;
        else if (!std::strcmp(id, "rangeHi")) p_.rangeHiHz = v;
        else if (!std::strcmp(id, "direction")) p_.direction = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "rangeLo")) out = p_.rangeLoHz;
        else if (!std::strcmp(id, "rangeHi")) out = p_.rangeHiHz;
        else if (!std::strcmp(id, "direction")) out = (float) p_.direction;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "autowah"; }
private:
    dcdsp::AutoWahCore core_; dcdsp::AutoWahParams p_{}; ParamMailbox<dcdsp::AutoWahParams> mail_;
};

class OctaverModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sub1")) p_.sub1 = v;
        else if (!std::strcmp(id, "sub2")) p_.sub2 = v;
        else if (!std::strcmp(id, "dry")) p_.dry = v;
        else if (!std::strcmp(id, "toneHz")) p_.toneHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sub1")) out = p_.sub1;
        else if (!std::strcmp(id, "sub2")) out = p_.sub2;
        else if (!std::strcmp(id, "dry")) out = p_.dry;
        else if (!std::strcmp(id, "toneHz")) out = p_.toneHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "octaver"; }
private:
    dcdsp::OctaverCore core_; dcdsp::OctaverParams p_{}; ParamMailbox<dcdsp::OctaverParams> mail_;
};

class RingModModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "freqHz")) p_.freqHz = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "waveform")) p_.waveform = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "freqHz")) out = p_.freqHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "waveform")) out = (float) p_.waveform;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "ringmod"; }
private:
    dcdsp::RingModCore core_; dcdsp::RingModParams p_{}; ParamMailbox<dcdsp::RingModParams> mail_;
};

class BitCrusherModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "bits")) p_.bits = (int) std::lround(v);
        else if (!std::strcmp(id, "downsample")) p_.downsample = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "inputDb")) p_.inputDb = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "intensity")) p_.intensity = v;
        else if (!std::strcmp(id, "rectify")) p_.rectify = v;
        else if (!std::strcmp(id, "filter")) p_.filter = v;
        else if (!std::strcmp(id, "outputDb")) p_.outputDb = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "bits")) out = (float) p_.bits;
        else if (!std::strcmp(id, "downsample")) out = (float) p_.downsample;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "inputDb")) out = p_.inputDb;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "intensity")) out = p_.intensity;
        else if (!std::strcmp(id, "rectify")) out = p_.rectify;
        else if (!std::strcmp(id, "filter")) out = p_.filter;
        else if (!std::strcmp(id, "outputDb")) out = p_.outputDb;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "bitcrusher"; }
private:
    dcdsp::BitCrusherCore core_; dcdsp::BitCrusherParams p_{}; ParamMailbox<dcdsp::BitCrusherParams> mail_;
};

class TapeDelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "wowFlutter")) p_.wowFlutter = v;
        else if (!std::strcmp(id, "toneHz")) p_.toneHz = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "wowFlutter")) out = p_.wowFlutter;
        else if (!std::strcmp(id, "toneHz")) out = p_.toneHz;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "tapedelay"; }
private:
    dcdsp::TapeDelayCore core_; dcdsp::TapeDelayParams p_{}; ParamMailbox<dcdsp::TapeDelayParams> mail_;
};

class PlateReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "predelayMs")) p_.predelayMs = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "width")) p_.width = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "predelayMs")) out = p_.predelayMs;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "width")) out = p_.width;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "platereverb"; }
private:
    dcdsp::PlateReverbCore core_; dcdsp::PlateReverbParams p_{}; ParamMailbox<dcdsp::PlateReverbParams> mail_;
};

class RotaryModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "speed")) p_.speed = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "speed")) out = p_.speed;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "rotary"; }
private:
    dcdsp::RotaryCore core_; dcdsp::RotaryParams p_{}; ParamMailbox<dcdsp::RotaryParams> mail_;
};

class FormantModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "vowel")) p_.vowel = v;
        else if (!std::strcmp(id, "amount")) p_.amount = v;
        else if (!std::strcmp(id, "brightness")) p_.brightness = v;
        else if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "vowel")) out = p_.vowel;
        else if (!std::strcmp(id, "amount")) out = p_.amount;
        else if (!std::strcmp(id, "brightness")) out = p_.brightness;
        else if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "formant"; }
private:
    dcdsp::FormantCore core_; dcdsp::FormantParams p_{}; ParamMailbox<dcdsp::FormantParams> mail_;
};

class GraphicEqModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        const int b = bandIndex(id);
        if (b >= 0) p_.bands[b] = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        const int b = bandIndex(id);
        if (b >= 0) out = p_.bands[b];
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "graphiceq"; }
private:
    // Map the 10 scalar band ids onto bands[0..9] (ISO octave centres).
    static int bandIndex(const char* id)
    {
        if (!std::strcmp(id, "band31"))  return 0;
        if (!std::strcmp(id, "band63"))  return 1;
        if (!std::strcmp(id, "band125")) return 2;
        if (!std::strcmp(id, "band250")) return 3;
        if (!std::strcmp(id, "band500")) return 4;
        if (!std::strcmp(id, "band1k"))  return 5;
        if (!std::strcmp(id, "band2k"))  return 6;
        if (!std::strcmp(id, "band4k"))  return 7;
        if (!std::strcmp(id, "band8k"))  return 8;
        if (!std::strcmp(id, "band16k")) return 9;
        return -1;
    }
    dcdsp::GraphicEqCore core_; dcdsp::GraphicEqParams p_{}; ParamMailbox<dcdsp::GraphicEqParams> mail_;
};

class HarmonizerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "semitones")) p_.semitones = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "lowLatency")) p_.lowLatency = (v != 0.0f);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "semitones")) out = p_.semitones;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "lowLatency")) out = p_.lowLatency ? 1.0f : 0.0f;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // WSOLA pipeline delay. The core has always reported it; the adapter used
    // to swallow it, so the chain-latency export (v30) would have called the
    // one genuinely late module in a rig zero-latency.
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "harmonizer"; }
private:
    dcdsp::HarmonizerCore core_; dcdsp::HarmonizerParams p_{}; ParamMailbox<dcdsp::HarmonizerParams> mail_;
};

class VibratoModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depthMs")) p_.depthMs = v;
        else if (!std::strcmp(id, "waveform")) p_.waveform = (int) std::lround(v);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depthMs")) out = p_.depthMs;
        else if (!std::strcmp(id, "waveform")) out = (float) p_.waveform;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The nominal (centre) modulated tap — see the core.
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "vibrato"; }
private:
    dcdsp::VibratoCore core_; dcdsp::VibratoParams p_{}; ParamMailbox<dcdsp::VibratoParams> mail_;
};

class SustainerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sustain")) p_.sustain = v;
        else if (!std::strcmp(id, "attack")) p_.attack = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sustain")) out = p_.sustain;
        else if (!std::strcmp(id, "attack")) out = p_.attack;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "sustainer"; }
private:
    dcdsp::SustainerCore core_; dcdsp::SustainerParams p_{}; ParamMailbox<dcdsp::SustainerParams> mail_;
};

class SpringReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "drip")) p_.drip = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "drip")) out = p_.drip;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "springreverb"; }
private:
    dcdsp::SpringReverbCore core_; dcdsp::SpringReverbParams p_{}; ParamMailbox<dcdsp::SpringReverbParams> mail_;
};

class ShimmerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "shimmer")) p_.shimmer = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "shimmer")) out = p_.shimmer;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "shimmer"; }
private:
    dcdsp::ShimmerCore core_; dcdsp::ShimmerParams p_{}; ParamMailbox<dcdsp::ShimmerParams> mail_;
};

class UniVibeModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "mode")) p_.mode = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "mode")) out = p_.mode;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "univibe"; }
private:
    dcdsp::UniVibeCore core_; dcdsp::UniVibeParams p_{}; ParamMailbox<dcdsp::UniVibeParams> mail_;
};

class HarmTremoloModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "crossoverHz")) p_.crossoverHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "crossoverHz")) out = p_.crossoverHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "harmtremolo"; }
private:
    dcdsp::HarmTremoloCore core_; dcdsp::HarmTremoloParams p_{}; ParamMailbox<dcdsp::HarmTremoloParams> mail_;
};

class EnvFilterModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "rangeLoHz")) p_.rangeLoHz = v;
        else if (!std::strcmp(id, "rangeHiHz")) p_.rangeHiHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "direction")) p_.direction = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "rangeLoHz")) out = p_.rangeLoHz;
        else if (!std::strcmp(id, "rangeHiHz")) out = p_.rangeHiHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "direction")) out = p_.direction;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "envfilter"; }
private:
    dcdsp::EnvFilterCore core_; dcdsp::EnvFilterParams p_{}; ParamMailbox<dcdsp::EnvFilterParams> mail_;
};

class DoublerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "detuneCents")) p_.detuneCents = v;
        else if (!std::strcmp(id, "spread")) p_.spread = v;
        else if (!std::strcmp(id, "delayMs")) p_.delayMs = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "detuneCents")) out = p_.detuneCents;
        else if (!std::strcmp(id, "spread")) out = p_.spread;
        else if (!std::strcmp(id, "delayMs")) out = p_.delayMs;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "doubler"; }
private:
    dcdsp::DoublerCore core_; dcdsp::DoublerParams p_{}; ParamMailbox<dcdsp::DoublerParams> mail_;
};

class AcousticSimModule : public DspModule
{
public:
    // Owner thread: tell the mailbox what rate to convert user IRs to, so the
    // callback's adoption stays a copy (see dcrig::IrMailbox). The three
    // bundled body IRs are already converted in AcousticSimCore::prepare().
    void prepare(double sr, int mb, int nch) override
    {
        core_.prepare(sr, mb, nch);
        irMail_.setEngineRate(sr > 0.0 ? sr : 48000.0);
    }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "body")) p_.body = v;
        else if (!std::strcmp(id, "brightness")) p_.brightness = v;
        else if (!std::strcmp(id, "lowCutHz")) p_.lowCutHz = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "body")) out = p_.body;
        else if (!std::strcmp(id, "brightness")) out = p_.brightness;
        else if (!std::strcmp(id, "lowCutHz")) out = p_.lowCutHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    // User body-IR loading (ABI v22) — same contract as the cab's "irPath".
    bool setStringParam(const char* id, const char* v) override
    {
        if (std::strcmp(id, "irPath") != 0) return false;
        const std::string path = (v != nullptr) ? v : "";
        if (path.empty()) { irMail_.publish(nullptr, 0, kIrMailboxDefaultRate); irPath_.clear(); return true; }
        irwav::IrData ir;
        // IrMailbox::kMaxTaps, not a literal: publish() silently truncates with
        // std::min(count, kMaxTaps), so raising one without the other drops the
        // excess taps with no error anywhere.
        if (!irwav::loadIrWav(path, IrMailbox::kMaxTaps, ir)) return false;
        irMail_.publish(ir.samples.data(), (int) ir.samples.size(), ir.sampleRate);
        irPath_ = path;
        return true;
    }
    bool getStringParam(const char* id, std::string& out) const override
    {
        if (std::strcmp(id, "irPath") != 0) return false;
        out = irPath_;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { irMail_.applyPending(core_); mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "acousticsim"; }
private:
    dcdsp::AcousticSimCore core_; dcdsp::AcousticSimParams p_{}; ParamMailbox<dcdsp::AcousticSimParams> mail_;
    IrMailbox irMail_;
    std::string irPath_;
};

class NoiseReducerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "amount")) p_.amount = v;
        else if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = v;
        else if (!std::strcmp(id, "humFreq")) p_.humFreq = v;
        else if (!std::strcmp(id, "humDepth")) p_.humDepth = v;
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "amount")) out = p_.amount;
        else if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "humFreq")) out = p_.humFreq;
        else if (!std::strcmp(id, "humDepth")) out = p_.humDepth;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "noisereducer"; }
private:
    dcdsp::NoiseReducerCore core_; dcdsp::NoiseReducerParams p_{}; ParamMailbox<dcdsp::NoiseReducerParams> mail_;
};

class KlonModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "gain")) p_.gain = v;
        else if (!std::strcmp(id, "treble")) p_.treble = v;
        else if (!std::strcmp(id, "output")) p_.output = v;
        else if (!std::strcmp(id, "blend")) p_.blend = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "gain")) out = p_.gain;
        else if (!std::strcmp(id, "treble")) out = p_.treble;
        else if (!std::strcmp(id, "output")) out = p_.output;
        else if (!std::strcmp(id, "blend")) out = p_.blend;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The klon delays its dry path to align with the oversampled wet path.
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "klon"; }
private:
    dcdsp::KlonCore core_; dcdsp::KlonParams p_{}; ParamMailbox<dcdsp::KlonParams> mail_;
};

class BigMuffModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sustain")) p_.sustain = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "volume")) p_.volume = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sustain")) out = p_.sustain;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "volume")) out = p_.volume;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "bigmuff"; }
private:
    dcdsp::BigMuffCore core_; dcdsp::BigMuffParams p_{}; ParamMailbox<dcdsp::BigMuffParams> mail_;
};

class MikuModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "character")) p_.character = v;
        else if (!std::strcmp(id, "octave")) p_.octave = v;
        else if (!std::strcmp(id, "pattern")) p_.pattern = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "character")) out = p_.character;
        else if (!std::strcmp(id, "octave")) out = p_.octave;
        else if (!std::strcmp(id, "pattern")) out = p_.pattern;
        // Poll-only diagnostics (deliberately NOT in the catalogue):
        else if (!std::strcmp(id, "trackedHz")) out = core_.trackedHz();
        else if (!std::strcmp(id, "syllable")) out = (float) core_.syllableIndex();
        else return false;
        return true;
    }
    // Custom lyric table (ABI v22): free-text syllables split on whitespace /
    // comma / hyphen / '|' (e.g. "ha-ru ka" or "mi ku mi ku"); "" reverts to
    // the preset pattern selected by the "pattern" knob.
    bool setStringParam(const char* id, const char* v) override
    {
        if (std::strcmp(id, "phrases") != 0) return false;
        const std::string text = (v != nullptr) ? v : "";
        dcdsp::MikuCore::Syllable syl[dcdsp::MikuCore::kMaxSyllables];
        const int n = dcdsp::MikuCore::parsePhrases(text.c_str(), syl,
                                                    dcdsp::MikuCore::kMaxSyllables);
        core_.setPhrase(syl, n);
        phrases_ = text;
        return true;
    }
    bool getStringParam(const char* id, std::string& out) const override
    {
        if (std::strcmp(id, "phrases") != 0) return false;
        out = phrases_;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "miku"; }
private:
    dcdsp::MikuCore core_; dcdsp::MikuParams p_{}; ParamMailbox<dcdsp::MikuParams> mail_;
    std::string phrases_;
};

// The KORG Miku Stomp recreation. A SECOND, separate module — `miku` above is
// our own vocal stomp and stays exactly as it is. See dcdsp_mikustomp.h for
// what the hardware does, and for the honest list of where we differ from it
// (the pedal concatenates recorded voicebank samples; we synthesise the same
// structure with EpR, because we cannot ship Crypton's voicebank).
class MikuStompModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "character")) p_.character = v;
        else if (!std::strcmp(id, "breath")) p_.breath = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "portamentoMs")) p_.portamentoMs = v;
        else if (!std::strcmp(id, "vibrato")) p_.vibrato = v;
        else if (!std::strcmp(id, "vibratoRateHz")) p_.vibratoRateHz = v;
        else if (!std::strcmp(id, "octave")) p_.octave = v;
        else if (!std::strcmp(id, "pattern")) p_.pattern = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "character")) out = p_.character;
        else if (!std::strcmp(id, "breath")) out = p_.breath;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "portamentoMs")) out = p_.portamentoMs;
        else if (!std::strcmp(id, "vibrato")) out = p_.vibrato;
        else if (!std::strcmp(id, "vibratoRateHz")) out = p_.vibratoRateHz;
        else if (!std::strcmp(id, "octave")) out = p_.octave;
        else if (!std::strcmp(id, "pattern")) out = p_.pattern;
        // Poll-only diagnostics (deliberately NOT in the catalogue):
        else if (!std::strcmp(id, "trackedHz")) out = core_.trackedHz();
        else if (!std::strcmp(id, "syllable")) out = (float) core_.syllableIndex();
        else return false;
        return true;
    }
    // Three user lyric slots (ABI v22), mirroring the hardware's three Phrase
    // slots in its iOS Lyric Editor. Japanese romaji moras, free-text, split on
    // whitespace / comma / hyphen / '|' (e.g. "ha tsu ne mi ku", "nya-nya").
    // A slot is only sung when `pattern` selects it (6/7/8); "" reverts it to
    // "Ahh". The "phrases" id is deliberately the SAME as the v1 miku module's,
    // so an existing lyric string works in either.
    bool setStringParam(const char* id, const char* v) override
    {
        const int slot = phraseSlot(id);
        if (slot < 0) return false;
        const std::string text = (v != nullptr) ? v : "";
        dcdsp::MikuStompCore::Syllable syl[dcdsp::MikuStompCore::kMaxSyllables];
        const int n = dcdsp::MikuStompCore::parsePhrases(text.c_str(), syl,
                                                         dcdsp::MikuStompCore::kMaxSyllables);
        core_.setPhrase(slot, syl, n);
        phrases_[(size_t) slot] = text;
        return true;
    }
    bool getStringParam(const char* id, std::string& out) const override
    {
        const int slot = phraseSlot(id);
        if (slot < 0) return false;
        out = phrases_[(size_t) slot];
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "mikustomp"; }
private:
    static int phraseSlot(const char* id)
    {
        if (id == nullptr) return -1;
        if (!std::strcmp(id, "phrases")) return 0;   // Phrase 1 (v1-compatible id)
        if (!std::strcmp(id, "phrase2")) return 1;
        if (!std::strcmp(id, "phrase3")) return 2;
        return -1;
    }
    dcdsp::MikuStompCore core_; dcdsp::MikuStompParams p_{}; ParamMailbox<dcdsp::MikuStompParams> mail_;
    std::string phrases_[dcdsp::MikuStompCore::kNumSlots];
};

class BoostModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "gainDb")) p_.gainDb = v;
        else if (!std::strcmp(id, "tilt")) p_.tilt = v;
        else if (!std::strcmp(id, "character")) p_.character = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "gainDb")) out = p_.gainDb;
        else if (!std::strcmp(id, "tilt")) out = p_.tilt;
        else if (!std::strcmp(id, "character")) out = p_.character;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "boost"; }
private:
    dcdsp::BoostCore core_; dcdsp::BoostParams p_{}; ParamMailbox<dcdsp::BoostParams> mail_;
};

class DimensionModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mode")) p_.mode = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "dimension"; }
private:
    dcdsp::DimensionCore core_; dcdsp::DimensionParams p_{}; ParamMailbox<dcdsp::DimensionParams> mail_;
};

// ---- 0.18 expressive / performance wave ------------------------------------

class SwellModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mode")) p_.mode = (int) std::lround(v);
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "sensitivity")) p_.sensitivity = v;
        else if (!std::strcmp(id, "depth")) p_.depth = v;
        else if (!std::strcmp(id, "position")) p_.position = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "sensitivity")) out = p_.sensitivity;
        else if (!std::strcmp(id, "depth")) out = p_.depth;
        else if (!std::strcmp(id, "position")) out = p_.position;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "swell"; }
private:
    dcdsp::SwellCore core_; dcdsp::SwellParams p_{}; ParamMailbox<dcdsp::SwellParams> mail_;
};

class WhammyModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "position")) p_.position = v;
        else if (!std::strcmp(id, "heelSemis")) p_.heelSemis = v;
        else if (!std::strcmp(id, "toeSemis")) p_.toeSemis = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "glideMs")) p_.glideMs = v;
        else if (!std::strcmp(id, "formant")) p_.formant = v;
        else if (!std::strcmp(id, "lowLatency")) p_.lowLatency = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "position")) out = p_.position;
        else if (!std::strcmp(id, "heelSemis")) out = p_.heelSemis;
        else if (!std::strcmp(id, "toeSemis")) out = p_.toeSemis;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "glideMs")) out = p_.glideMs;
        else if (!std::strcmp(id, "formant")) out = p_.formant;
        else if (!std::strcmp(id, "lowLatency")) out = p_.lowLatency;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // WSOLA pipeline delay (shorter in the lowLatency mode).
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "whammy"; }
private:
    dcdsp::WhammyCore core_; dcdsp::WhammyParams p_{}; ParamMailbox<dcdsp::WhammyParams> mail_;
};

class ReverseDelayModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "feedback")) p_.feedback = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "toneHz")) p_.toneHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "feedback")) out = p_.feedback;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "toneHz")) out = p_.toneHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "reversedelay"; }
private:
    dcdsp::ReverseDelayCore core_; dcdsp::ReverseDelayParams p_{}; ParamMailbox<dcdsp::ReverseDelayParams> mail_;
};

class SlapbackModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "timeMs")) p_.timeMs = v;
        else if (!std::strcmp(id, "repeats")) p_.repeats = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else if (!std::strcmp(id, "tone")) p_.tone = v;
        else if (!std::strcmp(id, "drive")) p_.drive = v;
        else if (!std::strcmp(id, "wobble")) p_.wobble = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "timeMs")) out = p_.timeMs;
        else if (!std::strcmp(id, "repeats")) out = (float) p_.repeats;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "tone")) out = p_.tone;
        else if (!std::strcmp(id, "drive")) out = p_.drive;
        else if (!std::strcmp(id, "wobble")) out = p_.wobble;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "slapback"; }
private:
    dcdsp::SlapbackCore core_; dcdsp::SlapbackParams p_{}; ParamMailbox<dcdsp::SlapbackParams> mail_;
};

class WidenerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "width")) p_.width = v;
        else if (!std::strcmp(id, "haas")) p_.haas = v;
        else if (!std::strcmp(id, "delayMs")) p_.delayMs = v;
        else if (!std::strcmp(id, "bassMonoHz")) p_.bassMonoHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "width")) out = p_.width;
        else if (!std::strcmp(id, "haas")) out = p_.haas;
        else if (!std::strcmp(id, "delayMs")) out = p_.delayMs;
        else if (!std::strcmp(id, "bassMonoHz")) out = p_.bassMonoHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "widener"; }
private:
    dcdsp::WidenerCore core_; dcdsp::WidenerParams p_{}; ParamMailbox<dcdsp::WidenerParams> mail_;
};

class FreezeModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "capture")) p_.capture = v;
        else if (!std::strcmp(id, "lengthMs")) p_.lengthMs = v;
        else if (!std::strcmp(id, "level")) p_.level = v;
        else if (!std::strcmp(id, "fadeMs")) p_.fadeMs = v;
        else if (!std::strcmp(id, "toneHz")) p_.toneHz = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "capture")) out = p_.capture;
        else if (!std::strcmp(id, "lengthMs")) out = p_.lengthMs;
        else if (!std::strcmp(id, "level")) out = p_.level;
        else if (!std::strcmp(id, "fadeMs")) out = p_.fadeMs;
        else if (!std::strcmp(id, "toneHz")) out = p_.toneHz;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "freeze"; }
private:
    dcdsp::FreezeCore core_; dcdsp::FreezeParams p_{}; ParamMailbox<dcdsp::FreezeParams> mail_;
};

class SubharmonicModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "sub1")) p_.sub1 = v;
        else if (!std::strcmp(id, "sub2")) p_.sub2 = v;
        else if (!std::strcmp(id, "waveform")) p_.waveform = (int) std::lround(v);
        else if (!std::strcmp(id, "cutoffHz")) p_.cutoffHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "envAmount")) p_.envAmount = v;
        else if (!std::strcmp(id, "attackMs")) p_.attackMs = v;
        else if (!std::strcmp(id, "dry")) p_.dry = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "sub1")) out = p_.sub1;
        else if (!std::strcmp(id, "sub2")) out = p_.sub2;
        else if (!std::strcmp(id, "waveform")) out = (float) p_.waveform;
        else if (!std::strcmp(id, "cutoffHz")) out = p_.cutoffHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "envAmount")) out = p_.envAmount;
        else if (!std::strcmp(id, "attackMs")) out = p_.attackMs;
        else if (!std::strcmp(id, "dry")) out = p_.dry;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "subharmonic"; }
private:
    dcdsp::SubharmonicCore core_; dcdsp::SubharmonicParams p_{}; ParamMailbox<dcdsp::SubharmonicParams> mail_;
};

class FilterSeqModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "rateHz")) p_.rateHz = v;
        else if (!std::strcmp(id, "steps")) p_.steps = (int) std::lround(v);
        else if (!std::strcmp(id, "mode")) p_.mode = (int) std::lround(v);
        else if (!std::strcmp(id, "trigger")) p_.trigger = (int) std::lround(v);
        else if (!std::strcmp(id, "rangeLoHz")) p_.rangeLoHz = v;
        else if (!std::strcmp(id, "rangeHiHz")) p_.rangeHiHz = v;
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "glide")) p_.glide = v;
        else if (!std::strcmp(id, "filterType")) p_.filterType = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "rateHz")) out = p_.rateHz;
        else if (!std::strcmp(id, "steps")) out = (float) p_.steps;
        else if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "trigger")) out = (float) p_.trigger;
        else if (!std::strcmp(id, "rangeLoHz")) out = p_.rangeLoHz;
        else if (!std::strcmp(id, "rangeHiHz")) out = p_.rangeHiHz;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "glide")) out = p_.glide;
        else if (!std::strcmp(id, "filterType")) out = (float) p_.filterType;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "filterseq"; }
private:
    dcdsp::FilterSeqCore core_; dcdsp::FilterSeqParams p_{}; ParamMailbox<dcdsp::FilterSeqParams> mail_;
};

class FeedbackerModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "amount")) p_.amount = v;
        else if (!std::strcmp(id, "riseMs")) p_.riseMs = v;
        else if (!std::strcmp(id, "harmonic")) p_.harmonic = (int) std::lround(v);
        else if (!std::strcmp(id, "resonance")) p_.resonance = v;
        else if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "amount")) out = p_.amount;
        else if (!std::strcmp(id, "riseMs")) out = p_.riseMs;
        else if (!std::strcmp(id, "harmonic")) out = (float) p_.harmonic;
        else if (!std::strcmp(id, "resonance")) out = p_.resonance;
        else if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "feedbacker"; }
private:
    dcdsp::FeedbackerCore core_; dcdsp::FeedbackerParams p_{}; ParamMailbox<dcdsp::FeedbackerParams> mail_;
};

class ResonatorModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "key")) p_.key = (int) std::lround(v);
        else if (!std::strcmp(id, "chord")) p_.chord = (int) std::lround(v);
        else if (!std::strcmp(id, "octave")) p_.octave = (int) std::lround(v);
        else if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "damping")) p_.damping = v;
        else if (!std::strcmp(id, "spread")) p_.spread = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "key")) out = (float) p_.key;
        else if (!std::strcmp(id, "chord")) out = (float) p_.chord;
        else if (!std::strcmp(id, "octave")) out = (float) p_.octave;
        else if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "damping")) out = p_.damping;
        else if (!std::strcmp(id, "spread")) out = p_.spread;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "resonator"; }
private:
    dcdsp::ResonatorCore core_; dcdsp::ResonatorParams p_{}; ParamMailbox<dcdsp::ResonatorParams> mail_;
};

class TapeStopModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "trigger")) p_.trigger = (int) std::lround(v);
        else if (!std::strcmp(id, "stopMs")) p_.stopMs = v;
        else if (!std::strcmp(id, "startMs")) p_.startMs = v;
        else if (!std::strcmp(id, "curve")) p_.curve = v;
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "trigger")) out = (float) p_.trigger;
        else if (!std::strcmp(id, "stopMs")) out = p_.stopMs;
        else if (!std::strcmp(id, "startMs")) out = p_.startMs;
        else if (!std::strcmp(id, "curve")) out = p_.curve;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    // The read head parks 2 samples in so the Lagrange window is always full.
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "tapestop"; }
private:
    dcdsp::TapeStopCore core_; dcdsp::TapeStopParams p_{}; ParamMailbox<dcdsp::TapeStopParams> mail_;
};

class GlitchModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        if (!std::strcmp(id, "mode")) p_.mode = (int) std::lround(v);
        else if (!std::strcmp(id, "trigger")) p_.trigger = (int) std::lround(v);
        else if (!std::strcmp(id, "sliceMs")) p_.sliceMs = v;
        else if (!std::strcmp(id, "repeats")) p_.repeats = (int) std::lround(v);
        else if (!std::strcmp(id, "chance")) p_.chance = v;
        else if (!std::strcmp(id, "decay")) p_.decay = v;
        else if (!std::strcmp(id, "reverse")) p_.reverse = (int) std::lround(v);
        else if (!std::strcmp(id, "mix")) p_.mix = v;
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "mode")) out = (float) p_.mode;
        else if (!std::strcmp(id, "trigger")) out = (float) p_.trigger;
        else if (!std::strcmp(id, "sliceMs")) out = p_.sliceMs;
        else if (!std::strcmp(id, "repeats")) out = (float) p_.repeats;
        else if (!std::strcmp(id, "chance")) out = p_.chance;
        else if (!std::strcmp(id, "decay")) out = p_.decay;
        else if (!std::strcmp(id, "reverse")) out = (float) p_.reverse;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "glitch"; }
private:
    dcdsp::GlitchCore core_; dcdsp::GlitchParams p_{}; ParamMailbox<dcdsp::GlitchParams> mail_;
};

// The category constants, the Entry row shape and kFlagSidechain moved to
// GearEntry.h (2026-09-11) so the per-category gear files can append rows.

std::unique_ptr<DspModule> makeGain() { return std::make_unique<CleanGainModule>(); }
std::unique_ptr<DspModule> makeGate() { return std::make_unique<GateModule>(); }
std::unique_ptr<DspModule> makeCompressor() { return std::make_unique<CompressorModule>(); }
std::unique_ptr<DspModule> makeEq() { return std::make_unique<EqModule>(); }
std::unique_ptr<DspModule> makeTremolo() { return std::make_unique<TremoloModule>(); }
std::unique_ptr<DspModule> makeChorus() { return std::make_unique<ChorusModule>(); }
std::unique_ptr<DspModule> makeFlanger() { return std::make_unique<FlangerModule>(); }
std::unique_ptr<DspModule> makeDelay() { return std::make_unique<DelayModule>(); }
std::unique_ptr<DspModule> makeReverb() { return std::make_unique<ReverbModule>(); }
std::unique_ptr<DspModule> makeOverdrive() { return std::make_unique<OverdriveModule>(); }
std::unique_ptr<DspModule> makeDistortion() { return std::make_unique<DistortionModule>(); }
std::unique_ptr<DspModule> makeAmp() { return std::make_unique<AmpModule>(); }
std::unique_ptr<DspModule> makeCab() { return std::make_unique<CabModule>(); }
std::unique_ptr<DspModule> makeFuzz() { return std::make_unique<FuzzModule>(); }
std::unique_ptr<DspModule> makePhaser() { return std::make_unique<PhaserModule>(); }
std::unique_ptr<DspModule> makeWah() { return std::make_unique<WahModule>(); }
std::unique_ptr<DspModule> makeAutoWah() { return std::make_unique<AutoWahModule>(); }
std::unique_ptr<DspModule> makeOctaver() { return std::make_unique<OctaverModule>(); }
std::unique_ptr<DspModule> makeRingMod() { return std::make_unique<RingModModule>(); }
std::unique_ptr<DspModule> makeBitCrusher() { return std::make_unique<BitCrusherModule>(); }
std::unique_ptr<DspModule> makeTapeDelay() { return std::make_unique<TapeDelayModule>(); }
std::unique_ptr<DspModule> makePlateReverb() { return std::make_unique<PlateReverbModule>(); }
std::unique_ptr<DspModule> makeRotary() { return std::make_unique<RotaryModule>(); }
std::unique_ptr<DspModule> makeFormant() { return std::make_unique<FormantModule>(); }
std::unique_ptr<DspModule> makeGraphicEq() { return std::make_unique<GraphicEqModule>(); }
std::unique_ptr<DspModule> makeHarmonizer() { return std::make_unique<HarmonizerModule>(); }
std::unique_ptr<DspModule> makeVibrato() { return std::make_unique<VibratoModule>(); }
std::unique_ptr<DspModule> makeSustainer() { return std::make_unique<SustainerModule>(); }
std::unique_ptr<DspModule> makeSpringReverb() { return std::make_unique<SpringReverbModule>(); }
std::unique_ptr<DspModule> makeShimmer() { return std::make_unique<ShimmerModule>(); }
std::unique_ptr<DspModule> makeUniVibe() { return std::make_unique<UniVibeModule>(); }
std::unique_ptr<DspModule> makeHarmTremolo() { return std::make_unique<HarmTremoloModule>(); }
std::unique_ptr<DspModule> makeEnvFilter() { return std::make_unique<EnvFilterModule>(); }
std::unique_ptr<DspModule> makeDoubler() { return std::make_unique<DoublerModule>(); }
std::unique_ptr<DspModule> makeAcousticSim() { return std::make_unique<AcousticSimModule>(); }
std::unique_ptr<DspModule> makeNoiseReducer() { return std::make_unique<NoiseReducerModule>(); }
std::unique_ptr<DspModule> makeKlon() { return std::make_unique<KlonModule>(); }
std::unique_ptr<DspModule> makeBigMuff() { return std::make_unique<BigMuffModule>(); }
std::unique_ptr<DspModule> makeMiku() { return std::make_unique<MikuModule>(); }
std::unique_ptr<DspModule> makeMikuStomp() { return std::make_unique<MikuStompModule>(); }
std::unique_ptr<DspModule> makeBoost() { return std::make_unique<BoostModule>(); }
std::unique_ptr<DspModule> makeDimension() { return std::make_unique<DimensionModule>(); }
std::unique_ptr<DspModule> makeSplitter() { return std::make_unique<SplitterModule>(); }
std::unique_ptr<DspModule> makeMerger() { return std::make_unique<MergerModule>(); }
std::unique_ptr<DspModule> makeSwell() { return std::make_unique<SwellModule>(); }
std::unique_ptr<DspModule> makeWhammy() { return std::make_unique<WhammyModule>(); }
std::unique_ptr<DspModule> makeReverseDelay() { return std::make_unique<ReverseDelayModule>(); }
std::unique_ptr<DspModule> makeSlapback() { return std::make_unique<SlapbackModule>(); }
std::unique_ptr<DspModule> makeWidener() { return std::make_unique<WidenerModule>(); }
std::unique_ptr<DspModule> makeFreeze() { return std::make_unique<FreezeModule>(); }
std::unique_ptr<DspModule> makeSubharmonic() { return std::make_unique<SubharmonicModule>(); }
std::unique_ptr<DspModule> makeFilterSeq() { return std::make_unique<FilterSeqModule>(); }
std::unique_ptr<DspModule> makeFeedbacker() { return std::make_unique<FeedbackerModule>(); }
std::unique_ptr<DspModule> makeResonator() { return std::make_unique<ResonatorModule>(); }
std::unique_ptr<DspModule> makeTapeStop() { return std::make_unique<TapeStopModule>(); }
std::unique_ptr<DspModule> makeGlitch() { return std::make_unique<GlitchModule>(); }

const std::vector<Entry>& entries()
{
    static const std::vector<Entry> e = [] {
        std::vector<Entry> v = {
        { { "gain", "Clean Boost",
            { { "gainDb", "Gain (dB)", -24.0f, 24.0f, 0.0f } } }, &makeGain, kCategoryUtility },
        { { "gate", "Noise Gate",
            { { "thresholdDb", "Threshold (dB)", -80.0f, 0.0f, -55.0f },
              { "ratio", "Ratio", 1.0f, 50.0f, 10.0f },
              { "attackMs", "Attack (ms)", 0.1f, 50.0f, 1.0f },
              { "holdMs", "Hold (ms)", 0.0f, 500.0f, 40.0f },
              { "releaseMs", "Release (ms)", 5.0f, 1000.0f, 120.0f },
              // dcdsp_gate.h: On = the threshold follows the input's measured noise
              // floor (DotCharter #155) and Threshold is its ceiling.
              { "autoFloor", "Auto Threshold", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } }, &makeGate, kCategoryDynamics, kFlagSidechain },
        { { "compressor", "Compressor",
            { { "thresholdDb", "Threshold (dB)", -60.0f, 0.0f, -18.0f },
              { "ratio", "Ratio", 1.0f, 20.0f, 4.0f },
              { "attackMs", "Attack (ms)", 0.1f, 100.0f, 10.0f },
              { "releaseMs", "Release (ms)", 5.0f, 1000.0f, 120.0f },
              { "makeupDb", "Makeup (dB)", 0.0f, 24.0f, 0.0f },
              { "kneeDb", "Knee (dB)", 0.0f, 24.0f, 6.0f },
              // NOT an enum — a continuous BLEND, and it stays a knob.
              // dcdsp_compressor.h: `level = (1-det)*peak + det*rms`. Every
              // value between 0 and 1 is a real, different detector.
              { "detector", "Detector (Peak-RMS)", 0.0f, 1.0f, 1.0f } } }, &makeCompressor, kCategoryDynamics, kFlagSidechain },
        { { "eq", "Parametric EQ",
            // Ranges = dcdsp_eq.h's clamp (widened 2026-10 for a four-band studio
            // parametric, DotCharter #68). The managed mirror is DcRigTargetCatalog.
            { { "lowFreq", "Low Freq", 20.0f, 500.0f, 100.0f },
              { "lowGainDb", "Low Gain (dB)", -18.0f, 18.0f, 0.0f },
              { "midFreq", "Mid Freq", 100.0f, 8000.0f, 1000.0f },
              { "midQ", "Mid Q", 0.3f, 8.0f, 1.0f },
              { "midGainDb", "Mid Gain (dB)", -18.0f, 18.0f, 0.0f },
              { "highFreq", "High Freq", 2000.0f, 20000.0f, 6000.0f },
              { "highGainDb", "High Gain (dB)", -18.0f, 18.0f, 0.0f },
              { "outputTrimDb", "Output (dB)", -24.0f, 24.0f, 0.0f } } }, &makeEq, kCategoryEq },
        { { "overdrive", "Overdrive",
            // 0.16: `model` param (0..3) picks a soundalike pedal voicing;
            // model 0 is bit-identical to the original overdrive.
            { { "model", "Pedal Model", 0.0f, 3.0f, 0.0f,
                { "Generic", "Screamer", "Blues Breaker", "Transparent" } },
              { "drive", "Drive", 0.0f, 1.0f, 0.5f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "level", "Level", 0.0f, 1.0f, 0.7f } } }, &makeOverdrive, kCategoryDrive },
        { { "distortion", "Distortion",
            // 0.16: `model` param (0..3) picks a soundalike pedal voicing;
            // model 0 is bit-identical to the original distortion.
            { { "model", "Pedal Model", 0.0f, 3.0f, 0.0f,
                { "Generic", "Gnash", "Metal", "Plus" } },
              { "gain", "Gain", 0.0f, 1.0f, 0.6f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "level", "Level", 0.0f, 1.0f, 0.6f },
              { "tightHz", "Tight (Hz)", 20.0f, 300.0f, 90.0f } } }, &makeDistortion, kCategoryDrive },
        // DISPLAY NAME ONLY (0.36). `namamp` is the amp the rig leads with now,
        // so this one is presented as the legacy option — but the type id "amp"
        // is a PERSISTED identifier: it is written into every DcRigPreset, all
        // 83 factory tone presets (70 of which use this module), every saved
        // user tone's JSON and DcRigTargetCatalog's managed mirror. Renaming the
        // id would silently un-resolve every one of them. Never touch it.
        { { "amp", "Legacy Amp",
            { { "gain", "Gain", 0.0f, 1.0f, 0.5f },
              { "bass", "Bass", 0.0f, 1.0f, 0.5f },
              { "mid", "Mid", 0.0f, 1.0f, 0.5f },
              { "treble", "Treble", 0.0f, 1.0f, 0.5f },
              { "presence", "Presence", 0.0f, 1.0f, 0.3f },
              { "master", "Master", 0.0f, 1.0f, 0.6f },
              // 0.16 RANGE CHANGE (was 0..2, then 0..6): 24-model gear roster.
              // Indices 0-6 keep their original coefficients (and thus sound),
              // so persisted RsGearOverrides stay valid; 7-23 are new models.
              // v24 value labels name each model for the picker.
              // 2026-10 (DotCharter #70): 24-26 are CIRCUIT voicings — a family
              // row through its documented passive tone stack (dcdsp_amp.h,
              // dcdsp_tonestack.h). 0-23 do not move.
              { "voicing", "Amp Model", 0.0f, 26.0f, 1.0f,
                { "Blackface Twin 85", "Plexi 50", "Five-Oh-One Lead",
                  "AC-Thirty", "Recto Dual", "Jazz Solid 120", "Brown Variac",
                  "Tweed Champ 5W", "Tweed Twin 40", "Blackface Deluxe 22",
                  "Plexi 100", "JMP-Eight-Hundred", "Two-Oh-Three Crunch",
                  "AC-Fifteen", "Recto Triple", "Modern Djent 8-String",
                  "PowerBall Lead", "Overdrive Special", "Class-A Boutique 30",
                  "Bass Three-Sixty", "Bass Flip-Top", "SVT Bass 300W",
                  "Acoustic Pre", "Studio Direct",
                  "Tweed Bass 45", "Plexi 50 Circuit", "Plexi 100 Circuit" } },
              // 2026-10 (#70): the amp-panel switches Rocksmith's amps carry.
              // Both default to the pre-existing sound (and skip their code).
              { "bright", "Bright", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.0f } } }, &makeAmp, kCategoryAmp },
        { { "cab", "Cabinet",
            // 0.16 RANGE CHANGE (was 0..3): 12-model cab roster. Indices 0-3
            // keep their exact IR (sound), so persisted `model` ints stay
            // valid; 4-11 are new synthesized IRs. v24 labels name each model.
            // 2026-10 (DotCharter #74): 12-25 APPENDED — IRs built from
            // measured driver responses (tools/cab_irs/gen_cab_irs.py) for the
            // speaker layouts Rocksmith's cabinets use and 0-11 lacked.
            { { "model", "Cab Model", 0.0f, 25.0f, 0.0f,
                { "1x12 Combo", "2x12 Combo", "4x12 Scooped", "Vintage 4x12",
                  "1x8 Tweed", "2x12 AC Open", "2x12 Boutique Closed",
                  "4x12 Greenback", "4x12 V30 Modern", "4x10 Bass",
                  "8x10 Bass Fridge", "1x15 Vintage Bass",
                  "4x10 Tweed Open", "1x10 Combo", "2x15 Guitar", "PA 15 + Horn",
                  "PA Column", "Hi-Fi Tower", "2x10 Bass", "3x10 Bass",
                  "6x10 Bass", "1x12 Bass", "2x12 Bass", "2x15 Bass",
                  "4x15 Bass", "1x15 + 1x10 Bass" } },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f },
              // 0.35: 60, not 80 — at 80 Hz this Butterworth took another
              // 2.8 dB at the low E on top of the IR's own rolloff (dcdsp_cab.h).
              { "lowCutHz", "Low Cut (Hz)", 40.0f, 200.0f, 60.0f },
              { "highCutHz", "High Cut (Hz)", 3000.0f, 12000.0f, 8000.0f },
              { "blend", "IR A/B Blend", 0.f, 1.f, 0.f },
              { "gainADb", "IR A Gain (dB)", -24.f, 24.f, 0.f },
              { "gainBDb", "IR B Gain (dB)", -24.f, 24.f, 0.f },
              { "invertA", "IR A Polarity", 0.f, 1.f, 0.f, { "Normal", "Inverted" } },
              { "invertB", "IR B Polarity", 0.f, 1.f, 0.f, { "Normal", "Inverted" } },
              { "delayAMs", "IR A Alignment (ms)", 0.f, 20.f, 0.f },
              { "delayBMs", "IR B Alignment (ms)", 0.f, 20.f, 0.f },
              { "outputDb", "Output (dB)", -24.f, 24.f, 0.f } },
            { { "irPath", "IR A File", "*.wav" }, { "irPathB", "IR B File", "*.wav" } } }, &makeCab, kCategoryCab },
        { { "tremolo", "Tremolo",
            { { "rateHz", "Rate (Hz)", 0.1f, 12.0f, 5.0f },
              { "depth", "Depth", 0.0f, 1.0f, 0.5f },
              // dcdsp_tremolo.h: `int shape` — 0 = sine, 1 = triangle, 2 = square.
              { "shape", "Shape", 0.0f, 2.0f, 0.0f,
                { "Sine", "Triangle", "Square" } } } }, &makeTremolo, kCategoryModulation },
        // 0.18 BBD pass: four params appended, EVERY ONE a no-op at its default
        // (feedback 0, spread 0, lfoShape 0 = Sine, wetHpfHz 20 = BYPASSED), so
        // the module is bit-identical to 0.16 until they are touched.
        { { "chorus", "Chorus",
            { { "rateHz", "Rate (Hz)", 0.1f, 5.0f, 0.8f },
              { "depthMs", "Depth (ms)", 1.0f, 15.0f, 6.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f },
              // dcdsp_chorus.h: `int voices` [1..3] — an ordinal COUNT, so it
              // snaps (no "2.4 voices"), not a named choice.
              { "voices", "Voices", 1.0f, 3.0f, 2.0f, {}, 1.0f },
              { "feedback", "Feedback (BBD)", 0.0f, 0.9f, 0.0f },
              { "spread", "Stereo Spread", 0.0f, 1.0f, 0.0f },
              { "lfoShape", "LFO Shape", 0.0f, 2.0f, 0.0f,
                { "Sine", "Triangle", "Hyper-Triangle" } },
              { "wetHpfHz", "Wet HPF (Hz)", 20.0f, 400.0f, 20.0f } } }, &makeChorus, kCategoryModulation },
        { { "flanger", "Flanger",
            { { "rateHz", "Rate (Hz)", 0.05f, 3.0f, 0.3f },
              { "depthMs", "Depth (ms)", 0.5f, 5.0f, 2.0f },
              { "feedback", "Feedback", -0.95f, 0.95f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeFlanger, kCategoryModulation },
        { { "delay", "Delay",
            { { "timeMs", "Time (ms)", 10.0f, 2000.0f, 300.0f },
              { "feedback", "Feedback", 0.0f, 0.95f, 0.4f },
              { "mix", "Mix", 0.0f, 1.0f, 0.35f },
              // dcdsp_delay.h: `bool pingPong` — the feedback crosses channels.
              { "pingPong", "Ping-Pong", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "toneHz", "Tone (Hz)", 500.0f, 12000.0f, 4000.0f },
              { "sync", "Tempo Sync", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "bpm", "Tempo (BPM)", 20.0f, 400.0f, 120.0f },
              { "division", "Note Division", 0.0f, 8.0f, 2.0f, { "1/1", "1/2", "1/4", "1/4 Dotted", "1/4 Triplet", "1/8", "1/8 Dotted", "1/8 Triplet", "1/16" } },
              { "trails", "Bypass Trails", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } }, &makeDelay, kCategoryDelay },
        { { "reverb", "Reverb",
            { { "roomSize", "Room Size", 0.0f, 1.0f, 0.5f },
              { "damping", "Damping", 0.0f, 1.0f, 0.5f },
              { "width", "Width", 0.0f, 1.0f, 1.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.3f } } }, &makeReverb, kCategoryReverb },
        { { "fuzz", "Fuzz",
            { { "fuzz", "Fuzz", 0.0f, 1.0f, 0.6f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "level", "Level", 0.0f, 1.0f, 0.7f } } }, &makeFuzz, kCategoryDrive },
        { { "phaser", "Phaser",
            { { "rateHz", "Rate (Hz)", 0.05f, 8.0f, 0.5f },
              { "depth", "Depth", 0.0f, 1.0f, 0.7f },
              { "feedback", "Feedback", -0.95f, 0.95f, 0.3f },
              // dcdsp_phaser.h: `int stages` [2..12], FORCED EVEN by the core
              // (`s &= ~1`). Step 2 — a knob that stopped on 7 would display a
              // stage count the cascade is not running.
              { "stages", "Stages", 2.0f, 12.0f, 6.0f, {}, 2.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makePhaser, kCategoryModulation },
        { { "wah", "Wah / Auto-Wah",
            // dcdsp_wah.h: `int mode` — 0 = auto (envelope-driven), 1 = manual
            // (the treadle position). NOT a blend: `auto_ = (mode != 1)`.
            { { "mode", "Mode", 0.0f, 1.0f, 0.0f, { "Auto", "Manual" } },
              { "position", "Position", 0.0f, 1.0f, 0.5f },
              { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
              { "rangeLoHz", "Range Lo (Hz)", 200.0f, 800.0f, 400.0f },
              { "rangeHiHz", "Range Hi (Hz)", 1200.0f, 3000.0f, 2000.0f },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeWah, kCategoryFilter },
        { { "autowah", "Auto-Wah (Envelope)",
            { { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
              { "attackMs", "Attack (ms)", 1.0f, 100.0f, 10.0f },
              { "releaseMs", "Release (ms)", 20.0f, 500.0f, 100.0f },
              { "rangeLo", "Range Lo (Hz)", 100.0f, 800.0f, 300.0f },
              { "rangeHi", "Range Hi (Hz)", 800.0f, 4000.0f, 2200.0f },
              // dcdsp_autowah.h: `int direction` — 0 = up (louder → higher),
              // 1 = down (the reverse quack). A SWITCH, not a blend (cf. the
              // envfilter's `direction`, which really is a blend and stays a knob).
              { "direction", "Direction", 0.0f, 1.0f, 0.0f, { "Up", "Down" } },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeAutoWah, kCategoryFilter },
        { { "octaver", "Octaver",
            { { "sub1", "Sub 1", 0.0f, 1.0f, 0.6f },
              { "sub2", "Sub 2", 0.0f, 1.0f, 0.0f },
              { "dry", "Dry", 0.0f, 1.0f, 1.0f },
              { "toneHz", "Tone (Hz)", 500.0f, 5000.0f, 2000.0f } } }, &makeOctaver, kCategoryPitch },
        { { "ringmod", "Ring Modulator",
            { { "freqHz", "Freq (Hz)", 20.0f, 3000.0f, 500.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f },
              // dcdsp_ringmod.h: `int waveform` — 0 = sine, 1 = triangle carrier.
              { "waveform", "Waveform", 0.0f, 1.0f, 0.0f,
                { "Sine", "Triangle" } } } }, &makeRingMod, kCategoryModulation },
        { { "bitcrusher", "Bit Crusher",
            // dcdsp_bitcrusher.h: both are integer COUNTS — `int bits` (amplitude
            // resolution) and `int downsample` (sample-and-hold factor).
            { { "bits", "Bits", 1.0f, 16.0f, 16.0f, {}, 1.0f },
              { "downsample", "Downsample", 1.0f, 64.0f, 1.0f, {}, 1.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f },
              // The crushed path's level / drive / lo-fi stages (dcdsp_bitcrusher.h,
              // DotCharter #157); at these defaults the path is the plain crusher above.
              { "inputDb", "Input (dB)", -24.0f, 48.0f, 0.0f },
              { "drive", "Drive", 0.0f, 100.0f, 0.0f },
              { "intensity", "Intensity", 0.0f, 100.0f, 0.0f },
              { "rectify", "Rectify", 0.0f, 100.0f, 0.0f },
              { "filter", "Filter", 0.0f, 100.0f, 0.0f },
              { "outputDb", "Output (dB)", -48.0f, 24.0f, 0.0f } } }, &makeBitCrusher, kCategoryDrive },
        { { "tapedelay", "Tape Delay",
            { { "timeMs", "Time (ms)", 20.0f, 2000.0f, 350.0f },
              { "feedback", "Feedback", 0.0f, 0.95f, 0.35f },
              { "mix", "Mix", 0.0f, 1.0f, 0.35f },
              { "wowFlutter", "Wow/Flutter", 0.0f, 1.0f, 0.25f },
              { "toneHz", "Tone (Hz)", 800.0f, 8000.0f, 4000.0f },
              { "drive", "Drive", 0.0f, 1.0f, 0.25f } } }, &makeTapeDelay, kCategoryDelay },
        { { "platereverb", "Plate Reverb",
            { { "decay", "Decay", 0.0f, 1.0f, 0.5f },
              { "damping", "Damping", 0.0f, 1.0f, 0.3f },
              { "predelayMs", "Predelay (ms)", 0.0f, 120.0f, 0.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.3f },
              { "width", "Width", 0.0f, 1.0f, 1.0f } } }, &makePlateReverb, kCategoryReverb },
        { { "rotary", "Rotary Speaker",
            { { "speed", "Speed", 0.0f, 1.0f, 0.0f },
              { "depth", "Depth", 0.0f, 1.0f, 1.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeRotary, kCategoryModulation },
        { { "formant", "Formant (Vocal)",
            { { "vowel", "Vowel", 0.0f, 1.0f, 0.0f },
              { "amount", "Amount", 0.0f, 1.0f, 0.5f },
              { "brightness", "Brightness", 0.0f, 1.0f, 0.5f },
              { "rateHz", "Rate (Hz)", 0.0f, 8.0f, 0.0f } } }, &makeFormant, kCategoryFilter },
        { { "graphiceq", "Graphic EQ",
            { { "band31", "31 Hz", -12.0f, 12.0f, 0.0f },
              { "band63", "63 Hz", -12.0f, 12.0f, 0.0f },
              { "band125", "125 Hz", -12.0f, 12.0f, 0.0f },
              { "band250", "250 Hz", -12.0f, 12.0f, 0.0f },
              { "band500", "500 Hz", -12.0f, 12.0f, 0.0f },
              { "band1k", "1 kHz", -12.0f, 12.0f, 0.0f },
              { "band2k", "2 kHz", -12.0f, 12.0f, 0.0f },
              { "band4k", "4 kHz", -12.0f, 12.0f, 0.0f },
              { "band8k", "8 kHz", -12.0f, 12.0f, 0.0f },
              { "band16k", "16 kHz", -12.0f, 12.0f, 0.0f },
              { "level", "Level (dB)", -12.0f, 12.0f, 0.0f } } }, &makeGraphicEq, kCategoryEq },
        { { "harmonizer", "Pitch Harmonizer",
            { { "semitones", "Semitones", -12.0f, 12.0f, 0.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f },
              // dcdsp_harmonizer.h: read as a BOOL (`lowLatency >= 0.5f`) —
              // it picks a window/search geometry, there is no half-way.
              { "lowLatency", "Low Latency", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
          &makeHarmonizer, kCategoryPitch },
        { { "vibrato", "Vibrato",
            { { "rateHz", "Rate (Hz)", 0.1f, 10.0f, 5.0f },
              { "depthMs", "Depth (ms)", 0.5f, 12.0f, 3.0f },
              // dcdsp_vibrato.h: `int waveform` — 0 = sine, 1 = triangle.
              { "waveform", "Waveform", 0.0f, 1.0f, 0.0f,
                { "Sine", "Triangle" } } } }, &makeVibrato, kCategoryModulation },
        { { "sustainer", "Sustainer",
            { { "sustain", "Sustain", 0.0f, 1.0f, 0.5f },
              { "attack", "Attack", 0.0f, 1.0f, 0.3f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "level", "Level", 0.0f, 1.0f, 0.5f } } }, &makeSustainer, kCategoryDynamics },
        { { "springreverb", "Spring Reverb",
            { { "decay", "Decay", 0.0f, 1.0f, 0.5f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "drip", "Drip", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.3f } } }, &makeSpringReverb, kCategoryReverb },
        { { "shimmer", "Shimmer Reverb",
            { { "decay", "Decay", 0.0f, 1.0f, 0.7f },
              { "shimmer", "Shimmer", 0.0f, 1.0f, 0.5f },
              { "damping", "Damping", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.35f } } }, &makeShimmer, kCategoryReverb },
        { { "univibe", "Uni-Vibe",
            { { "rateHz", "Rate (Hz)", 0.1f, 8.0f, 2.0f },
              { "depth", "Depth", 0.0f, 1.0f, 0.7f },
              // NOT an enum — a continuous MORPH, and it stays a knob.
              // dcdsp_univibe.h smooths it and computes
              // `wetAmt = mix + mode*(1 - mix)`: 0 = chorus (wet+dry), 1 =
              // vibrato (100% wet), and everything between is a real voicing.
              { "mode", "Voicing (Chorus-Vibrato)", 0.0f, 1.0f, 0.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeUniVibe, kCategoryModulation },
        { { "harmtremolo", "Harmonic Tremolo",
            { { "rateHz", "Rate (Hz)", 0.1f, 12.0f, 4.0f },
              { "depth", "Depth", 0.0f, 1.0f, 0.6f },
              { "crossoverHz", "Crossover (Hz)", 200.0f, 2000.0f, 800.0f } } }, &makeHarmTremolo, kCategoryModulation },
        { { "envfilter", "Envelope Filter",
            { { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
              { "rangeLoHz", "Range Lo (Hz)", 100.0f, 800.0f, 200.0f },
              { "rangeHiHz", "Range Hi (Hz)", 800.0f, 4000.0f, 2500.0f },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.7f },
              // NOT an enum — a continuous BLEND, and it stays a knob.
              // dcdsp_envfilter.h: `cutoff = cutUp + dir*(cutDown - cutUp)`, so
              // 0.5 sweeps HALFWAY between the up and the down sweep. (The
              // autowah's same-named param IS an int switch — they differ.)
              { "direction", "Direction (Up-Down)", 0.0f, 1.0f, 0.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeEnvFilter, kCategoryFilter },
        { { "doubler", "Detune Doubler",
            { { "detuneCents", "Detune (cents)", 0.0f, 30.0f, 12.0f },
              { "spread", "Spread", 0.0f, 1.0f, 0.7f },
              { "delayMs", "Delay (ms)", 2.0f, 40.0f, 12.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeDoubler, kCategoryModulation },
        { { "acousticsim", "Acoustic Sim",
            { { "body", "Body", 0.0f, 1.0f, 0.5f },
              { "brightness", "Brightness", 0.0f, 1.0f, 0.5f },
              { "lowCutHz", "Low Cut (Hz)", 40.0f, 200.0f, 90.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } },
            { { "irPath", "Body IR File", "*.wav" } } }, &makeAcousticSim, kCategoryCab },
        { { "noisereducer", "Noise Reducer",
            { { "amount", "Amount", 0.0f, 1.0f, 0.5f },
              { "thresholdDb", "Threshold (dB)", -80.0f, -20.0f, -55.0f },
              { "humFreq", "Hum Freq (Hz)", 50.0f, 60.0f, 50.0f },
              { "humDepth", "Hum Depth", 0.0f, 1.0f, 0.0f },
              { "releaseMs", "Release (ms)", 20.0f, 500.0f, 120.0f } } }, &makeNoiseReducer, kCategoryDynamics },
        { { "boost", "Boost (Clean/Treble)",
            { { "gainDb", "Gain (dB)", 0.0f, 24.0f, 6.0f },
              { "tilt", "Tilt (Dark-Bright)", -1.0f, 1.0f, 0.0f },
              { "character", "Character (Clean-Treble)", 0.0f, 1.0f, 0.0f } } }, &makeBoost, kCategoryDrive },
        { { "dimension", "Dimension Chorus",
            // dcdsp_dimension.h: `int mode` [0..3] IS the hardware's four
            // buttons (a rate/depth table, subtle → wide) — the pedal has no
            // in-between, so the labels are the button numbers, nothing invented.
            { { "mode", "Mode", 0.0f, 3.0f, 1.0f,
                { "1", "2", "3", "4" } },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeDimension, kCategoryModulation },
        { { "klon", "Transparent Boost",
            { { "gain", "Gain", 0.0f, 1.0f, 0.5f },
              { "treble", "Treble", 0.0f, 1.0f, 0.5f },
              { "output", "Output", 0.0f, 1.0f, 0.6f },
              { "blend", "Blend", 0.0f, 1.0f, 0.6f } } }, &makeKlon, kCategoryDrive },
        { { "bigmuff", "Big Muff Fuzz",
            { { "sustain", "Sustain", 0.0f, 1.0f, 0.7f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "volume", "Volume", 0.0f, 1.0f, 0.6f } } }, &makeBigMuff, kCategoryDrive },
        { { "miku", "Miku Vocal Synth",
            { { "mix", "Mix", 0.0f, 1.0f, 0.6f },
              { "character", "Character", 0.0f, 1.0f, 0.5f },
              // dcdsp_miku.h: whole octaves, ROUNDED (-1 .. +2 relative to the
              // tracked guitar pitch; +1 is the Miku register, hence the default).
              { "octave", "Octave", -1.0f, 2.0f, 1.0f,
                { "-1 Oct", "Unison", "+1 Oct", "+2 Oct" } },
              // dcdsp_miku.h: preset phrase pattern 0..7 (rounded), in THIS order.
              { "pattern", "Pattern", 0.0f, 7.0f, 0.0f,
                { "Looh", "Lahh", "Ahh", "Pahh", "Nyan", "Scat",
                  "Random 1", "Random 2" } } },
            { { "phrases", "Phrases", "" } } }, &makeMiku, kCategorySynth },
        // The KORG Miku Stomp recreation — a SEPARATE module from `miku` above,
        // which stays as it is. Defaults mirror the hardware: wet-only (mix 1),
        // pattern 0 = "Ahh". `pattern` uses the v24 value-label catalogue so the
        // UI shows the pedal's own eleven lyric-pattern names.
        { { "mikustomp", "Miku Stomp (Vocaloid)",
            { { "mix", "Mix", 0.0f, 1.0f, 1.0f },
              { "character", "Character (Dark-Kawaii)", 0.0f, 1.0f, 0.5f },
              { "breath", "Breathiness", 0.0f, 1.0f, 0.35f },
              { "attackMs", "Attack (ms)", 1.0f, 200.0f, 12.0f },
              { "portamentoMs", "Portamento (ms)", 0.0f, 300.0f, 50.0f },
              { "vibrato", "Vibrato Depth", 0.0f, 1.0f, 0.45f },
              { "vibratoRateHz", "Vibrato Rate (Hz)", 3.0f, 8.0f, 5.8f },
              // dcdsp_mikustomp.h: whole octaves, ROUNDED — same table as `miku`.
              { "octave", "Octave", -1.0f, 2.0f, 1.0f,
                { "-1 Oct", "Unison", "+1 Oct", "+2 Oct" } },
              { "pattern", "Lyric Pattern", 0.0f, 10.0f, 0.0f,
                { "Ahh", "Lahh", "Looh", "Pahh", "Nyan", "Scat",
                  "Phrase 1", "Phrase 2", "Phrase 3", "Random 1", "Random 2" } } },
            { { "phrases", "Phrase 1 Lyrics", "" },
              { "phrase2", "Phrase 2 Lyrics", "" },
              { "phrase3", "Phrase 3 Lyrics", "" } } }, &makeMikuStomp, kCategorySynth },
        // ---- 0.18 expressive / performance wave -----------------------------
        // Ranges + defaults mirror each core header's params struct VERBATIM
        // (the managed DcRigTargetCatalog hand-mirror is gated against this
        // table).
        //
        // EVERY discrete param carries v24 value labels — including the
        // footswitch/latch toggles (freeze `capture`, tapestop/glitch `trigger`,
        // whammy `formant`/`lowLatency`). They used to be withheld because the
        // gear rail treated a module's FIRST labelled param as its "browse
        // models" param, so labelling a footswitch would have let the rail drop
        // in a pedal already stomped. That was fixed at the cause
        // (GearCardViewModel.ModelParamOf now resolves the module's declared
        // MODEL param — amp voicing / cab-drive model — not "the first labelled
        // param"), so a latch can be honestly labelled and still never be a model.
        // Discrete + NAMED ⇒ valueLabels (a picker). Discrete + ORDINAL ⇒ `step`
        // (a snapping knob). Continuous ⇒ neither.
        { { "swell", "Volume / Swell",
            { { "mode", "Mode", 0.0f, 1.0f, 0.0f, { "Auto Swell", "Volume" } },
              { "attackMs", "Swell Time (ms)", 5.0f, 2000.0f, 350.0f },
              { "sensitivity", "Sensitivity", 0.0f, 1.0f, 0.5f },
              { "depth", "Depth", 0.0f, 1.0f, 1.0f },
              { "position", "Position", 0.0f, 1.0f, 1.0f } } }, &makeSwell, kCategoryDynamics },
        { { "whammy", "Whammy (Pitch Treadle)",
            { { "position", "Position", 0.0f, 1.0f, 0.0f },
              { "heelSemis", "Heel (semitones)", -24.0f, 24.0f, 0.0f },
              { "toeSemis", "Toe (semitones)", -24.0f, 24.0f, 12.0f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f },
              { "glideMs", "Glide (ms)", 0.0f, 200.0f, 20.0f },
              // dcdsp_whammy.h: both are 0/1 switches — `formant` = 1 engages the
              // LPC whiten/re-dress path, `lowLatency` = 1 picks a shorter
              // window+search. Neither has a meaningful in-between.
              { "formant", "Formant Preserve", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "lowLatency", "Low Latency", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } },
          &makeWhammy, kCategoryPitch },
        { { "reversedelay", "Reverse Delay",
            { { "timeMs", "Segment (ms)", 100.0f, 2000.0f, 500.0f },
              // Hard cap 0.7: the two power-complementary heads can sum to
              // sqrt(2), so 0.7*sqrt(2) = 0.99 is the stability bound.
              { "feedback", "Feedback", 0.0f, 0.7f, 0.3f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f },
              { "toneHz", "Tone (Hz)", 500.0f, 12000.0f, 4000.0f } } }, &makeReverseDelay, kCategoryDelay },
        { { "slapback", "Slapback Echo",
            { { "timeMs", "Slap (ms)", 40.0f, 300.0f, 110.0f },
              // dcdsp_slapback.h: "`repeats` is an exact COUNT (1..3), not a
              // decay coefficient. One repeat means one repeat." So it snaps.
              { "repeats", "Repeats", 1.0f, 3.0f, 1.0f, {}, 1.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.35f },
              { "tone", "Tone", 0.0f, 1.0f, 0.5f },
              { "drive", "Drive", 0.0f, 1.0f, 0.25f },
              { "wobble", "Wobble", 0.0f, 1.0f, 0.2f } } }, &makeSlapback, kCategoryDelay },
        { { "widener", "Stereo Widener",
            { { "width", "Width", 0.0f, 2.0f, 1.4f },
              { "haas", "Haas", 0.0f, 1.0f, 0.35f },
              { "delayMs", "Haas Time (ms)", 1.0f, 40.0f, 18.0f },
              { "bassMonoHz", "Bass Mono (Hz)", 0.0f, 500.0f, 120.0f } } }, &makeWidener, kCategoryUtility },
        { { "freeze", "Freeze (Infinite Sustain)",
            // `capture` is a LATCH (rising edge captures, 0 releases) — a preset
            // must never ship it at 1; capture is a performance gesture. It is
            // labelled (a two-state picker, never a knob), which is safe only
            // because the gear rail no longer treats "first labelled param" as a
            // model — see the header comment on this block.
            { { "capture", "Capture", 0.0f, 1.0f, 0.0f,
                { "Released", "Held" } },
              { "lengthMs", "Length (ms)", 120.0f, 2000.0f, 500.0f },
              { "level", "Level", 0.0f, 1.0f, 0.7f },
              { "fadeMs", "Fade (ms)", 5.0f, 2000.0f, 80.0f },
              { "toneHz", "Tone (Hz)", 500.0f, 12000.0f, 6000.0f } } }, &makeFreeze, kCategorySynth },
        { { "subharmonic", "Subharmonic Synth",
            { { "sub1", "Sub -1 Oct", 0.0f, 1.0f, 0.7f },
              { "sub2", "Sub -2 Oct", 0.0f, 1.0f, 0.0f },
              { "waveform", "Waveform", 0.0f, 2.0f, 0.0f,
                { "Square", "Saw", "Sine" } },
              { "cutoffHz", "Cutoff (Hz)", 100.0f, 4000.0f, 900.0f },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.25f },
              { "envAmount", "Env Amount", 0.0f, 1.0f, 0.4f },
              { "attackMs", "Attack (ms)", 0.0f, 300.0f, 5.0f },
              { "dry", "Dry", 0.0f, 1.0f, 1.0f } } }, &makeSubharmonic, kCategoryPitch },
        { { "filterseq", "Filter Sequencer",
            { { "rateHz", "Rate (steps/s)", 0.1f, 16.0f, 4.0f },
              // dcdsp_filterseq.h: `int steps` — the sequence LENGTH [2..16].
              { "steps", "Steps", 2.0f, 16.0f, 8.0f, {}, 1.0f },
              { "mode", "Pattern", 0.0f, 3.0f, 0.0f,
                { "Up", "Down", "Up-Down", "Random (S&H)" } },
              { "trigger", "Step Source", 0.0f, 1.0f, 0.0f, { "Clock", "Pick" } },
              { "rangeLoHz", "Range Lo (Hz)", 100.0f, 1000.0f, 200.0f },
              { "rangeHiHz", "Range Hi (Hz)", 500.0f, 6000.0f, 3000.0f },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.6f },
              { "glide", "Glide", 0.0f, 1.0f, 0.15f },
              { "filterType", "Filter", 0.0f, 2.0f, 0.0f,
                { "Low-pass", "Band-pass", "High-pass" } },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeFilterSeq, kCategoryFilter },
        { { "feedbacker", "Feedbacker",
            { { "amount", "Amount", 0.0f, 1.0f, 0.5f },
              { "riseMs", "Bloom (ms)", 10.0f, 3000.0f, 400.0f },
              { "harmonic", "Harmonic", 0.0f, 3.0f, 1.0f,
                { "Fundamental", "Octave Up", "Fifth Up", "Two Octaves Up" } },
              { "resonance", "Resonance", 0.0f, 1.0f, 0.7f },
              { "thresholdDb", "Threshold (dB)", -60.0f, -10.0f, -40.0f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeFeedbacker, kCategorySynth },
        { { "resonator", "Resonator",
            { { "key", "Key", 0.0f, 11.0f, 0.0f,
                { "C", "C#", "D", "D#", "E", "F",
                  "F#", "G", "G#", "A", "A#", "B" } },
              { "chord", "Chord", 0.0f, 4.0f, 0.0f,
                { "Power", "Major", "Minor", "Sus4", "Octaves" } },
              // dcdsp_resonator.h: "Register of the root: MIDI note =
              // 24 + key + 12*octave (2 -> C3)". MIDI 24 is C1, so value i is
              // register i+1 — the labels name the register, not the index.
              { "octave", "Octave", 0.0f, 4.0f, 2.0f,
                { "Oct 1", "Oct 2", "Oct 3", "Oct 4", "Oct 5" } },
              { "decay", "Decay", 0.0f, 1.0f, 0.7f },
              { "damping", "Damping", 0.0f, 1.0f, 0.5f },
              { "spread", "Spread", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 0.5f } } }, &makeResonator, kCategorySynth },
        { { "tapestop", "Tape Stop",
            // dcdsp_tapestop.h: `int trigger` — "0 = Run (tape at speed),
            // 1 = Stop (brake engaged). The pedal's footswitch."
            { { "trigger", "Brake", 0.0f, 1.0f, 0.0f, { "Run", "Stop" } },
              { "stopMs", "Stop (ms)", 50.0f, 3000.0f, 600.0f },
              { "startMs", "Start (ms)", 50.0f, 3000.0f, 400.0f },
              { "curve", "Curve", 0.0f, 1.0f, 0.5f },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeTapeStop, kCategoryPitch },
        { { "glitch", "Glitch (Beat Repeat)",
            { { "mode", "Mode", 0.0f, 1.0f, 0.0f, { "Manual", "Auto" } },
              // dcdsp_glitch.h: `int trigger` — "Manual only: 0 = Off, 1 = On
              // (stutter while held)" — the footswitch.
              { "trigger", "Trigger", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "sliceMs", "Slice (ms)", 20.0f, 1000.0f, 250.0f },
              // dcdsp_glitch.h: `int repeats` — "Auto only: how many times a
              // captured slice repeats. [1, 16]".
              { "repeats", "Repeats", 1.0f, 16.0f, 4.0f, {}, 1.0f },
              { "chance", "Chance", 0.0f, 1.0f, 0.35f },
              { "decay", "Decay", 0.0f, 1.0f, 0.0f },
              { "reverse", "Direction", 0.0f, 1.0f, 0.0f, { "Forward", "Reverse" } },
              { "mix", "Mix", 0.0f, 1.0f, 1.0f } } }, &makeGlitch, kCategoryUtility },
        // Split/parallel chain routing (ABI v23). The chain executes the
        // [splitter .. merger] span as parallel branches; these nodes are
        // wires by themselves. Semantics documented in dotcharter_audio.h.
        { { "splitter", "Splitter",
            // RoutingModules.h: an integer branch COUNT (2..3) — it snaps.
            { { "enabledBranches", "Branches", 2.0f, 3.0f, 2.0f, {}, 1.0f } } },
          &makeSplitter, kCategoryRouting },
        { { "merger", "Merger",
            { { "level1", "Level 1", 0.0f, 2.0f, 1.0f },
              { "pan1", "Pan 1", -1.0f, 1.0f, 0.0f },
              { "level2", "Level 2", 0.0f, 2.0f, 1.0f },
              { "pan2", "Pan 2", -1.0f, 1.0f, 0.0f },
              { "level3", "Level 3", 0.0f, 2.0f, 1.0f },
              { "pan3", "Pan 3", -1.0f, 1.0f, 0.0f },
              // RoutingModules.h: "mode 0 (average) scales the sum by
              // 1/branchCount so an untouched splitter->merger pair is a
              // bit-exact passthrough; mode 1 is a raw sum." Two states.
              { "mode", "Mode", 0.0f, 1.0f, 0.0f,
                { "Average", "Sum" } },
              { "mute1", "Mute 1", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "solo1", "Solo 1", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "mute2", "Mute 2", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "solo2", "Solo 2", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "mute3", "Mute 3", 0.0f, 1.0f, 0.0f, { "Off", "On" } },
              { "solo3", "Solo 3", 0.0f, 1.0f, 0.0f, { "Off", "On" } } } }, &makeMerger, kCategoryRouting },
#if DOTCHARTER_DSP_NEURAL
        { namAmpTypeInfo(), &createNamAmpModule, kCategoryAmp },
#endif
        };
        // Gear added after the 57-row table lives in src/dsp/gear/, one file
        // per palette category (GearEntry.h).
        appendDynamicsGear(v);
        appendDriveGear(v);
        appendAmpCabGear(v);
        appendFilterGear(v);
        appendModulationGear(v);
        appendPitchGear(v);
        appendDelayGear(v);
        appendReverbGear(v);
        appendUtilityGear(v);
        appendRsAmpGear(v);
        return v;
    }();
    return e;
}

} // namespace

const std::vector<ModuleTypeInfo>& moduleCatalogue()
{
    static const std::vector<ModuleTypeInfo> cat = [] {
        std::vector<ModuleTypeInfo> c;
        for (const auto& e : entries())
        {
            c.push_back(e.info);
            // ABI v33. The table owns the category; ModuleTypeInfo carries it so
            // dca_dsp_module_type_category has one source of truth to read.
            c.back().category = e.category != nullptr ? e.category : "";
        }
        return c;
    }();
    return cat;
}

uint32_t moduleTypeFlags(int index)
{
    const auto& e = entries();
    if (index < 0 || (size_t) index >= e.size()) return 0;
    return e[(size_t) index].flags;
}

std::unique_ptr<DspModule> createModule(const char* typeId)
{
    if (typeId == nullptr) return nullptr;
    for (const auto& e : entries())
        if (e.info.id == typeId) return e.make();
    return nullptr;
}

} // namespace dcrig
