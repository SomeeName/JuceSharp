#pragma once

#include <atomic>
#include <array>
#include <cstring>
#include <type_traits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// In-house DSP tone-rig framework (0.16 "In-house tone rig", Phase 0).
//
// A DspModule is a lightweight processing node — deliberately NOT a
// juce::AudioProcessor subclass: the effect algorithms are pure `dcdsp` cores
// (prepare / setParams / process) with no framework coupling, and this thin
// interface just wraps one so the chain can own, order and process them. That
// keeps the hot path a plain virtual `process` over a std::vector, which is
// exactly what the design brief calls for (a runtime-ordered vector, not an
// AudioProcessorGraph). VST3 hosting stays the separate out-of-process path.
//
// Real-time contract: `process` never allocates, locks or throws; all
// allocation happens in `prepare` (message thread). `setParam` is cheap and
// called from the message thread; a module MUST NOT recompute state its
// `process` reads directly from `setParam` — publish through a ParamMailbox
// (below) or dedicated atomics so the audio thread applies the change at a
// block boundary.
namespace dcrig
{

// ---------------------------------------------------------------------------
// ParamMailbox — the one sanctioned way for an adapter's float params to reach
// its core (2026-07-16 audit #4).
//
// Adapters used to call `core_.setParams(p_)` synchronously from the message
// thread while the audio thread could be inside `core_.process(...)`. For a
// single float that is a benign one-block-late store, but setParams recomputes
// MULTI-WORD coefficient sets in place (biquad banks, filter cascades), so the
// audio thread could read a mix of old and new coefficients mid-recompute — a
// genuine data race whose worst case (a transient blow-up in a resonant
// section) was only bounded by DspChain's between-stage NaN scrub.
//
// The mailbox extends the chain's own snapshot discipline to params: the
// message thread PUBLISHES the whole params struct through a seqlock; the
// audio thread APPLIES it at the top of process() — so core_.setParams and
// core_.process always run on the same thread and can never interleave. The
// audio thread never blocks: if it observes a write in flight (odd sequence,
// or the sequence moved under the copy) it simply keeps the previous
// coefficients for one more block and retries on the next. The message thread
// never blocks either, so there is no lock to priority-invert.
//
// Requirements on ParamsT: trivially copyable (every dcdsp params struct is a
// plain aggregate). Requirements on setParams: allocation-free (they are — the
// cores size everything in prepare()); it now runs on the audio thread.
// Writers are serialized by DspChain's writeMutex_, so a single-slot mailbox
// suffices; intermediate values of a fast sweep are deliberately coalesced.
//
// getParam is NOT affected: adapters keep updating their own `p_` copy
// synchronously in setParam, so reads (including adapter-side clamping /
// lround quantization) behave exactly as before.
// ---------------------------------------------------------------------------
template <typename ParamsT>
class ParamMailbox
{
    static_assert(std::is_trivially_copyable_v<ParamsT>);
    static_assert(std::atomic<uint32_t>::is_always_lock_free);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static constexpr size_t kWords = (sizeof(ParamsT) + sizeof(uint32_t) - 1) / sizeof(uint32_t);
public:
    // All snapshot operations participate in one sequentially consistent order.
    // A plain payload behind a sequence counter is still a C++ data race; atomic
    // words make even a rejected overlapping copy well-defined. No retry loop.
    void publish(const ParamsT& p) noexcept
    {
        std::array<uint32_t, kWords> words{};
        std::memcpy(words.data(), &p, sizeof(p));
        seq_.fetch_add(1);
        for (size_t i = 0; i < kWords; ++i) pending_[i].store(words[i]);
        seq_.fetch_add(1);
    }

    // Only the consumer calls this. Intermediate edits may be coalesced.
    bool consume(ParamsT& value) noexcept
    {
        const uint64_t s = seq_.load();
        if (s == applied_ || (s & 1u) != 0) return false;
        std::array<uint32_t, kWords> words{};
        for (size_t i = 0; i < kWords; ++i) words[i] = pending_[i].load();
        if (seq_.load() != s) return false;
        std::memcpy(&value, words.data(), sizeof(value));
        applied_ = s;
        return true;
    }

    template <typename CoreT>
    void applyPending(CoreT& core)
    {
        ParamsT local{};
        if (consume(local)) core.setParams(local);
    }

private:
    std::array<std::atomic<uint32_t>, kWords> pending_{};
    std::atomic<uint64_t> seq_{0};
    uint64_t applied_ = 0; // consumer-owned
};

// Static metadata describing one tweakable parameter of a module type. The
// managed UI builds generic sliders from these, so no per-module UI is needed.
struct ParamInfo
{
    std::string id;          // stable machine id, e.g. "drive"
    std::string displayName; // human label, e.g. "Drive"
    float       minValue;
    float       maxValue;
    float       defaultValue;
    // Optional per-VALUE labels for enum-like params (ABI v24): index i names
    // the integer value (minValue + i). Empty for continuous params. Used by
    // the pedalboard UI to render a named picker (amp voicing, cab model,
    // drive model) instead of a "3.0" slider. When present, size() == the
    // count of selectable values and covers minValue..maxValue inclusive.
    std::vector<std::string> valueLabels;
    // Quantization step (ABI v27). 0 = continuous (the default — a plain knob).
    // > 0 = the param only accepts multiples of `step` off `minValue`: the
    // adapter lrounds it into an int core field, so a UI that let the user park
    // it between two steps would DISPLAY a value the DSP is not running
    // ("6.5 stages", "2.4 voices"). The UI snaps the knob to the step and shows
    // the integer. ORDINAL counts only (chorus voices, phaser stages — which is
    // step 2, the core forces the allpass count EVEN, bitcrusher bits, …);
    // a NAMED discrete choice carries `valueLabels` and renders as a picker,
    // not a stepped knob.
    float step = 0.0f;
};

// Static metadata describing one string-valued parameter of a module type
// (ABI v22): file paths (NAM captures, user IR WAVs) and text tables (Miku
// phrase strings). `fileFilter` is a ';'-separated wildcard list for a file
// picker (e.g. "*.nam;*.aidax"); empty when the value is not a file path.
struct StringParamInfo
{
    std::string id;          // stable machine id, e.g. "modelPath"
    std::string displayName; // human label, e.g. "NAM Model"
    std::string fileFilter;  // picker wildcards; "" = free text
};

// Static metadata describing one available module type (a pedal / amp / cab).
struct ModuleTypeInfo
{
    std::string                  id;          // e.g. "overdrive"
    std::string                  displayName; // e.g. "Overdrive"
    std::vector<ParamInfo>       params;
    std::vector<StringParamInfo> stringParams; // v22; empty for most modules
    // ABI v33: which drawer of a pedalboard palette this module belongs in, out
    // of a short FIXED set (dynamics / drive / amp / cab / modulation / delay /
    // reverb / pitch / filter / eq / utility / routing / synth). It is the LAST
    // member so every existing brace-initialiser in DspModuleFactory.cpp keeps
    // its meaning; the factory's Entry table carries the value and
    // moduleCatalogue() folds it in. EVERY module has one — pinned by
    // tone_regressions' everyCatalogueEntryHasAKnownCategory().
    std::string                  category;
};

class DspModule
{
public:
    virtual ~DspModule() = default;

    // Message thread. Allocate/size everything here.
    virtual void prepare(double sampleRate, int maxBlockSize, int numChannels) = 0;

    // Message thread or audio thread (state clear only, no alloc).
    virtual void reset() = 0;

    // Set one parameter by id. Returns false if the id is unknown. Cheap.
    virtual bool setParam(const char* id, float value) = 0;

    // Read one parameter by id into out. Returns false if unknown.
    virtual bool getParam(const char* id, float& out) const = 0;

    // Would `setParam(id, …)` accept this id? A RESOLVE-ONLY probe, used by the
    // automated offline render to validate a whole event list before it renders
    // a sample (DspChain::hasParam).
    //
    // Defaulted to `getParam` because for 56 of the 57 modules the two ids sets
    // are the same one — but it is NOT the same question, and the difference is
    // load-bearing: `namamp` answers `getParam` for three POLL-ONLY status ids
    // ("loaded", "rateMismatch", "resampling") that its `setParam` rejects, so a
    // probe built on `getParam` accepted an automation event the render then
    // failed on half-way through. A module whose two sets differ MUST override
    // this.
    virtual bool hasSettableParam(const char* id) const
    {
        float ignored = 0.0f;
        return getParam(id, ignored);
    }

    // Set one string-valued parameter (UTF-8) by id (ABI v22). Returns false
    // if the id is unknown or the value was rejected. Message thread; may
    // allocate, but any HEAVY work (file I/O, neural-model parse) must move
    // off-thread inside the module — process() never blocks on it.
    virtual bool setStringParam(const char* id, const char* utf8Value)
    {
        (void) id; (void) utf8Value; return false;
    }

    // Read one string-valued parameter by id into out (ABI v22). Returns
    // false if unknown. Message thread.
    virtual bool getStringParam(const char* id, std::string& out) const
    {
        (void) id; (void) out; return false;
    }

    // Audio thread. In-place processing of numChannels non-interleaved buffers.
    // RT-safe: no alloc / lock / throw.
    virtual void process(float* const* channels, int numChannels, int numFrames) = 0;

    // ---- Sidechain (ABI v34) ----------------------------------------------
    // Until v34 every module was single-buffer: the compressor's "sidechain"
    // was an internal 100 Hz high-pass on its OWN input, which is a detector
    // shape, not a sidechain. A module that opts in here can be KEYED from
    // another signal — the seat's dry input (so a pedal late in the chain
    // follows the pick rather than what the drive left of it) or the song mix
    // (so the rig ducks under the backing track).
    //
    // The chain hands the key to `processSidechained` and NOTHING ELSE
    // CHANGES: the audio path, the buffer, the crossfade and the scrub are the
    // same. `process` stays pure virtual and every existing core keeps its one
    // override — the default below forwards, so a module that never heard of
    // this is byte-for-byte what it was.
    //
    // `key` is READ-ONLY, may be shorter in channels than `numChannels`, and is
    // valid only for the duration of the call. It is never null here: the chain
    // calls `process` instead when the requested source is unavailable, which
    // is the important half of the contract — an unavailable key falls back to
    // the module's own input, never to silence, because "keyed from silence" on
    // a GATE means closed.
    virtual bool supportsSidechain() const { return false; }
    virtual void processSidechained(float* const* channels, int numChannels, int numFrames,
                                    const float* const* key, int keyChannels)
    {
        (void) key; (void) keyChannels;
        process(channels, numChannels, numFrames);
    }

    // Stable opt-in: custom bypass processing remains callback-owned even at zero
    // weight. The module must apply this chain ramp once, retaining unity dry.
    virtual bool ownsBypassProcessing() const { return false; }
    virtual void processBypass(float* const* channels, int numChannels, int numFrames,
                               float wetStart, float wetTarget, float wetStep)
    { (void) wetStart; (void) wetTarget; (void) wetStep; process(channels, numChannels, numFrames); }
    // Message thread: a draining module cannot be reset directly while parked.
    // Opt-in implementations publish a reset request for their next callback.
    virtual void requestBypassReset() {}

    // Latency this module adds, in samples (0 for most). Reported for PDC.
    virtual int latencySamples() const { return 0; }

    // The module type id this instance was created from.
    virtual const char* typeId() const = 0;

    /// Escape hatch for the NATIVE tests only: the adapter's underlying dcdsp
    /// core, so a test can measure the shipping wiring (mailboxes, adoption
    /// order) instead of a hand-built core. Never called from the engine, and
    /// nothing crosses the ABI — a test target recompiles these sources.
    virtual void* coreForTest() { return nullptr; }
};

} // namespace dcrig
