// Reverb gear — the "reverb" drawer of the pedalboard palette.
//
// One core header under ../cores/ + one adapter class + one Entry row per
// module, appended in appendReverbGear(). Rules that apply to every module here are the
// ones the 57 originals follow (docs/agents/dsp-tone-rig.md): every
// user-facing gain/mix/level ramps (SmoothedValue or a block ramp from the
// previous block's value), discrete params declare valueLabels or a step,
// prepare() re-derives everything the rate owns and clears no user content,
// nothing on process() allocates or locks, every clamp rejects NaN, and the
// adapter's float params reach the core through ParamMailbox.
#include "../GearEntry.h"
#include "../IrWavLoader.h"
#include "../cores/dcdsp_convreverb.h"
#include "../cores/dcdsp_gatedreverb.h"
#include "../cores/dcdsp_hallreverb.h"
#include "../cores/dcdsp_reversereverb.h"

#include <cmath>
#include <cstring>
#include <string>

namespace dcrig
{
namespace
{

/// NaN-safe clamp for the ADAPTER's own copy of a parameter. DspChain rejects a
/// NaN before it ever reaches setParam, but `p_` is also what getParam reads
/// back, and an adapter that stores a value the core then clamps reports a
/// number the DSP is not running.
inline float clampNaN(float v, float lo, float hi) noexcept
{
    if (!(v >= lo)) return lo;   // NaN lands here, not on the fall-through
    if (v > hi)     return hi;
    return v;
}

// ---------------------------------------------------------------------------
// hallreverb — FDN room / hall
// ---------------------------------------------------------------------------
class HallReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        // Rejected at the DOOR, before the clamps: clampNaN sends a NaN to the
        // LOW end of the range, which is a silent setting change, and p_ is what
        // getParam reports. Every sibling gear file does this.
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "space")) p_.space = (int) std::lround(clampNaN(v, 0.f, 3.f));
        else if (!std::strcmp(id, "decayS")) p_.decayS = clampNaN(v, 0.2f, 20.f);
        else if (!std::strcmp(id, "predelayMs")) p_.predelayMs = clampNaN(v, 0.f, 200.f);
        else if (!std::strcmp(id, "dampingKHz")) p_.dampingKHz = clampNaN(v, 1.f, 16.f);
        else if (!std::strcmp(id, "lowCutHz")) p_.lowCutHz = clampNaN(v, 20.f, 500.f);
        else if (!std::strcmp(id, "erLevel")) p_.erLevel = clampNaN(v, 0.f, 1.f);
        else if (!std::strcmp(id, "diffusion")) p_.diffusion = clampNaN(v, 0.f, 1.f);
        else if (!std::strcmp(id, "width")) p_.width = clampNaN(v, 0.f, 1.f);
        else if (!std::strcmp(id, "mix")) p_.mix = clampNaN(v, 0.f, 1.f);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "space")) out = (float) p_.space;
        else if (!std::strcmp(id, "decayS")) out = p_.decayS;
        else if (!std::strcmp(id, "predelayMs")) out = p_.predelayMs;
        else if (!std::strcmp(id, "dampingKHz")) out = p_.dampingKHz;
        else if (!std::strcmp(id, "lowCutHz")) out = p_.lowCutHz;
        else if (!std::strcmp(id, "erLevel")) out = p_.erLevel;
        else if (!std::strcmp(id, "diffusion")) out = p_.diffusion;
        else if (!std::strcmp(id, "width")) out = p_.width;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "hallreverb"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::HallReverbCore core_;
    dcdsp::HallReverbParams p_{};
    ParamMailbox<dcdsp::HallReverbParams> mail_;
};

// ---------------------------------------------------------------------------
// convreverb — uniform-partitioned FFT convolution
// ---------------------------------------------------------------------------
class ConvReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        // Rejected at the DOOR, before the clamps: clampNaN sends a NaN to the
        // LOW end of the range, which is a silent setting change, and p_ is what
        // getParam reports. Every sibling gear file does this.
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "predelayMs")) p_.predelayMs = clampNaN(v, 0.f, 200.f);
        else if (!std::strcmp(id, "lengthPct"))
        {
            p_.lengthPct = clampNaN(v, 10.f, 100.f);
            // The ONE adapter in the wave that reaches into its core from
            // setParam, and it is the same entry point `irFile` uses one line
            // down: a truncation re-partitions and re-transforms the whole IR,
            // which no mailbox can hand the callback. The rule it looks like it
            // breaks — "never call core_.setParams from setParam" — is about
            // recomputing coefficients the callback is READING; this touches no
            // audio-thread state at all (ConvReverbCore owns a second FFT and a
            // second pad buffer for exactly this path) and hands the result
            // over as one atomically published snapshot. `setLengthPct` is a
            // no-op on an unchanged value and the rate conversion feeding it is
            // cached, so a slider drag costs one partition+transform a tick.
            core_.setLengthPct(p_.lengthPct);
            mail_.publish(p_);
            return true;
        }
        else if (!std::strcmp(id, "lowCutHz")) p_.lowCutHz = clampNaN(v, 20.f, 500.f);
        else if (!std::strcmp(id, "highCutKHz")) p_.highCutKHz = clampNaN(v, 2.f, 20.f);
        else if (!std::strcmp(id, "mix")) p_.mix = clampNaN(v, 0.f, 1.f);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "predelayMs")) out = p_.predelayMs;
        else if (!std::strcmp(id, "lengthPct")) out = p_.lengthPct;
        else if (!std::strcmp(id, "lowCutHz")) out = p_.lowCutHz;
        else if (!std::strcmp(id, "highCutKHz")) out = p_.highCutKHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }

    /// ABI v22 string param, the cab's `irPath` contract one size up: the WAV
    /// is parsed and rate-converted on THIS thread and handed over as an
    /// immutable snapshot. A file that cannot be read is REFUSED and the
    /// previous IR keeps playing — a reverb that goes silent because a path
    /// moved is worse than one that ignores the request.
    bool setStringParam(const char* id, const char* v) override
    {
        if (std::strcmp(id, "irFile") != 0) return false;
        const std::string path = v != nullptr ? v : "";
        if (path.empty()) { core_.clearIr(); irPath_.clear(); return true; }
        irwav::IrData ir;
        if (!irwav::loadIrWav(path, dcdsp::ConvReverbCore::kMaxIrTaps, ir)) return false;
        if (!core_.setIr(ir.samples.data(), (int) ir.samples.size(), ir.sampleRate)) return false;
        irPath_ = path;
        return true;
    }
    bool getStringParam(const char* id, std::string& out) const override
    {
        if (std::strcmp(id, "irFile") != 0) return false;
        out = irPath_;
        return true;
    }

    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    int latencySamples() const override { return core_.latencySamples(); }
    const char* typeId() const override { return "convreverb"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::ConvReverbCore core_;
    dcdsp::ConvReverbParams p_{};
    ParamMailbox<dcdsp::ConvReverbParams> mail_;
    std::string irPath_;
};

// ---------------------------------------------------------------------------
// gatedreverb — the 80s gate
// ---------------------------------------------------------------------------
class GatedReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        // Rejected at the DOOR, before the clamps: clampNaN sends a NaN to the
        // LOW end of the range, which is a silent setting change, and p_ is what
        // getParam reports. Every sibling gear file does this.
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "decayS")) p_.decayS = clampNaN(v, 0.5f, 10.f);
        else if (!std::strcmp(id, "gateMs")) p_.gateMs = clampNaN(v, 20.f, 1000.f);
        else if (!std::strcmp(id, "thresholdDb")) p_.thresholdDb = clampNaN(v, -60.f, 0.f);
        else if (!std::strcmp(id, "releaseMs")) p_.releaseMs = clampNaN(v, 5.f, 200.f);
        else if (!std::strcmp(id, "predelayMs")) p_.predelayMs = clampNaN(v, 0.f, 100.f);
        else if (!std::strcmp(id, "dampingKHz")) p_.dampingKHz = clampNaN(v, 1.f, 16.f);
        else if (!std::strcmp(id, "mix")) p_.mix = clampNaN(v, 0.f, 1.f);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "decayS")) out = p_.decayS;
        else if (!std::strcmp(id, "gateMs")) out = p_.gateMs;
        else if (!std::strcmp(id, "thresholdDb")) out = p_.thresholdDb;
        else if (!std::strcmp(id, "releaseMs")) out = p_.releaseMs;
        else if (!std::strcmp(id, "predelayMs")) out = p_.predelayMs;
        else if (!std::strcmp(id, "dampingKHz")) out = p_.dampingKHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "gatedreverb"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::GatedReverbCore core_;
    dcdsp::GatedReverbParams p_{};
    ParamMailbox<dcdsp::GatedReverbParams> mail_;
};

// ---------------------------------------------------------------------------
// reversereverb — backwards capture windows
// ---------------------------------------------------------------------------
class ReverseReverbModule : public DspModule
{
public:
    void prepare(double sr, int mb, int nch) override { core_.prepare(sr, mb, nch); }
    void reset() override { core_.reset(); }
    bool setParam(const char* id, float v) override
    {
        // Rejected at the DOOR, before the clamps: clampNaN sends a NaN to the
        // LOW end of the range, which is a silent setting change, and p_ is what
        // getParam reports. Every sibling gear file does this.
        if (!std::isfinite(v)) return false;
        if (!std::strcmp(id, "decayS")) p_.decayS = clampNaN(v, 0.5f, 5.f);
        else if (!std::strcmp(id, "windowMs")) p_.windowMs = clampNaN(v, 100.f, 2000.f);
        else if (!std::strcmp(id, "predelayMs")) p_.predelayMs = clampNaN(v, 0.f, 200.f);
        else if (!std::strcmp(id, "dampingKHz")) p_.dampingKHz = clampNaN(v, 1.f, 16.f);
        else if (!std::strcmp(id, "mix")) p_.mix = clampNaN(v, 0.f, 1.f);
        else if (!std::strcmp(id, "swellShape")) p_.swellShape = clampNaN(v, 0.f, 1.f);
        else return false;
        mail_.publish(p_); return true;
    }
    bool getParam(const char* id, float& out) const override
    {
        if (!std::strcmp(id, "decayS")) out = p_.decayS;
        else if (!std::strcmp(id, "windowMs")) out = p_.windowMs;
        else if (!std::strcmp(id, "predelayMs")) out = p_.predelayMs;
        else if (!std::strcmp(id, "dampingKHz")) out = p_.dampingKHz;
        else if (!std::strcmp(id, "mix")) out = p_.mix;
        else if (!std::strcmp(id, "swellShape")) out = p_.swellShape;
        else return false;
        return true;
    }
    void process(float* const* ch, int nch, int n) override { mail_.applyPending(core_); core_.process(ch, nch, n); }
    const char* typeId() const override { return "reversereverb"; }
    void* coreForTest() override { return &core_; }
private:
    dcdsp::ReverseReverbCore core_;
    dcdsp::ReverseReverbParams p_{};
    ParamMailbox<dcdsp::ReverseReverbParams> mail_;
};

std::unique_ptr<DspModule> makeHallReverb()    { return std::make_unique<HallReverbModule>(); }
std::unique_ptr<DspModule> makeConvReverb()    { return std::make_unique<ConvReverbModule>(); }
std::unique_ptr<DspModule> makeGatedReverb()   { return std::make_unique<GatedReverbModule>(); }
std::unique_ptr<DspModule> makeReverseReverb() { return std::make_unique<ReverseReverbModule>(); }

} // namespace

void appendReverbGear(std::vector<Entry>& out)
{
    out.push_back(
        { { "hallreverb", "Hall / Room Reverb",
            { // `space` is a NAMED discrete choice: it re-derives the whole
              // delay-network geometry, so it renders as a picker, never as a
              // knob a user can park between two rooms.
              { "space", "Space", 0.f, 3.f, 2.f, { "Room", "Chamber", "Hall", "Cathedral" } },
              { "decayS", "Decay (s)", 0.2f, 20.f, 2.5f },
              { "predelayMs", "Pre-delay (ms)", 0.f, 200.f, 20.f },
              { "dampingKHz", "Damping (kHz)", 1.f, 16.f, 6.f },
              { "lowCutHz", "Low Cut (Hz)", 20.f, 500.f, 80.f },
              { "erLevel", "Early Reflections", 0.f, 1.f, 0.5f },
              { "diffusion", "Diffusion", 0.f, 1.f, 0.7f },
              { "width", "Width", 0.f, 1.f, 1.f },
              { "mix", "Mix", 0.f, 1.f, 0.3f } } },
          &makeHallReverb, kCategoryReverb });

    out.push_back(
        { { "convreverb", "Convolution Reverb",
            { { "predelayMs", "Pre-delay (ms)", 0.f, 200.f, 0.f },
              { "lengthPct", "IR Length (%)", 10.f, 100.f, 100.f },
              // 20 Hz and 20 kHz mean OFF, not "nearly transparent": at the
              // ends of these two ranges the filters are out of the path and
              // the module is a pure convolution.
              { "lowCutHz", "Low Cut (Hz)", 20.f, 500.f, 40.f },
              { "highCutKHz", "High Cut (kHz)", 2.f, 20.f, 16.f },
              { "mix", "Mix", 0.f, 1.f, 0.3f } },
            { { "irFile", "IR File", "*.wav" } } },
          &makeConvReverb, kCategoryReverb });

    out.push_back(
        { { "gatedreverb", "Gated Reverb",
            { { "decayS", "Decay (s)", 0.5f, 10.f, 3.f },
              { "gateMs", "Gate Hold (ms)", 20.f, 1000.f, 250.f },
              { "thresholdDb", "Threshold (dB)", -60.f, 0.f, -30.f },
              { "releaseMs", "Release (ms)", 5.f, 200.f, 20.f },
              { "predelayMs", "Pre-delay (ms)", 0.f, 100.f, 0.f },
              { "dampingKHz", "Damping (kHz)", 1.f, 16.f, 8.f },
              { "mix", "Mix", 0.f, 1.f, 0.5f } } },
          &makeGatedReverb, kCategoryReverb });

    out.push_back(
        { { "reversereverb", "Reverse Reverb",
            { { "decayS", "Decay (s)", 0.5f, 5.f, 2.f },
              { "windowMs", "Window (ms)", 100.f, 2000.f, 800.f },
              { "predelayMs", "Pre-delay (ms)", 0.f, 200.f, 0.f },
              { "dampingKHz", "Damping (kHz)", 1.f, 16.f, 8.f },
              { "mix", "Mix", 0.f, 1.f, 0.5f },
              { "swellShape", "Swell Shape", 0.f, 1.f, 0.5f } } },
          &makeReverseReverb, kCategoryReverb });
}

} // namespace dcrig
