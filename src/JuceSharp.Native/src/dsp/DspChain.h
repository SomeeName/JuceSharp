#pragma once

#include "DspModule.h"
#include "DspModuleFactory.h" // moduleCatalogue(), for the central param clamp
#include "IdleSleep.h"
#include "RoutingModules.h"
#include "Scrub.h"
#include "../RtDiag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Reorderable in-process effect chain for the guitar input path (0.16 Phase 0).
//
// RT-safety follows the SAME pattern the engine already uses for its input VST
// chain (AudioEngine `inputChain_`): the audio thread reads an immutable
// ordering snapshot via an atomic shared_ptr; the message thread mutates the
// owned-module map and publishes a fresh snapshot atomically, then takes a
// wait-one-block barrier so any callback that loaded the OLD snapshot has
// finished before a removed module is destroyed.
//
// Module STATE (filter memory) lives in the owned map and is stable across
// reorders/bypass — only the ordering snapshot (a vector of raw pointers +
// bypass flags) is rebuilt and swapped, so reordering a delay never clears its
// buffer.
//
// PARAMS get the same discipline as structure (2026-07-16 audit #4): setParam
// runs on the message thread under writeMutex_, but the adapter it reaches
// only records the value and PUBLISHES the params struct through a seqlock'd
// ParamMailbox (DspModule.h); the audio thread applies it at the top of the
// module's process(). setParam therefore never recomputes coefficients that a
// concurrent process() is reading, and neither side ever blocks on the other —
// a mid-publish block simply keeps the previous coefficients one block longer.
// (writeMutex_ is never taken on the audio thread; processMutex_ is never held
// by a writer for longer than the barrier acquire-release.)
//
// Split / parallel chains (ABI v23): a non-bypassed `splitter` node opens a
// parallel region of 2-3 branches; the next non-bypassed `merger` closes it.
// Between them every node processes ONE branch — chosen by its branch tag
// (setNodeBranch; 0 = branch 1) — over a COPY of the splitter's input, and the
// merger mixes the branches back with per-branch level/pan (see
// RoutingModules.h). The branch scratch is allocated in prepare(); process()
// stays allocation- and lock-free beyond the existing barrier mutex. Edge
// semantics (mirrored in dotcharter_audio.h): bypassed splitter => the region
// degrades to the plain linear chain; unmatched splitter => implicit unity
// average merge at chain end; a merger with no open region and a nested
// splitter are wires; branch tags clamp to the enabled branch count. Branch
// latency differences are COMPENSATED since ABI v34: the merger carries one
// delay line per branch, set to (max - branch) and re-derived on every publish,
// every accepted param/string write and every prepare. It is ON by default and
// dca_engine_dsp_set_branch_pdc_for_player turns it off (the comb is a sound
// some players want, and off is the pre-v34 path byte for byte). See
// RoutingModules.h:29-30 for the ring geometry and the 8,192-sample clamp.
//
// EDIT SCOPES (ABI v34). Every mutator above publishes a fresh ordering
// snapshot and takes the one-block barrier, which is right for ONE footswitch
// and wrong for a PRESET: loading a twelve-pedal board was twelve barriers plus
// thirty param publishes, each individually visible to the callback, so the
// user heard the new rig assemble itself pedal by pedal. `beginEdit()` opens a
// SHADOW: structure changes (add / remove / move / retag / bypass / clear) and
// param writes accumulate against `idOrder_`/`modules_` with publishing
// SUPPRESSED, and `commitEdit()` publishes the result. Reads inside the scope
// (node ids, counts, params, latency) report the SHADOW, which is what a
// caller that just built the preset expects to read back.
//
// Nodes added inside a scope are PARKED at wet 0 whatever their bypass flag, so
// they all fade in together at commit; nodes removed inside a scope are held
// alive (`editRemoved_`) rather than destroyed, so the commit can fade them out
// together before it retires them. A commit is therefore at most TWO
// publications regardless of preset size — one transitional (the old chain
// fading out while the new nodes fade in, in ONE 10 ms window) and one final —
// and exactly ONE when the scope removed nothing that was audible. `abortEdit`
// restores the pre-edit STRUCTURE (membership, order, bypass, branch tags) and
// destroys the nodes the scope added; parameter VALUES are not journalled and
// are not rolled back.
//
// Bypass / reorder / remove are CROSSFADED, not spliced (2026-08-28). Every
// structural change used to take effect between two blocks as a hard cut, for
// all ~60 cores at once: the footswitch clicked, and a core with an envelope
// follower (SwellCore) came back from bypass with a follower frozen at the
// level it saw when it left, so the first pick after un-bypass misfired. The
// chain owns the fix so no core has to: each regular node carries a wet/dry
// weight (NodeFade, audio-thread-owned, same grow-only pool discipline as the
// meters) that ramps linearly over kFadeSeconds toward 1 (in the path) or 0
// (bypassed). While it ramps, the module KEEPS PROCESSING — that is what keeps
// its followers warm — and the output is dry + (wet - dry) * w per sample; at
// exactly 0 generic modules are skipped. Explicit bypass-processing modules
// (delay trails) own their input/tail mix and keep advancing at zero weight.
// Remove waits for the fade-out before retiring the module; move and
// retag fade the node out at the old slot and back in at the new one. Routing
// nodes (splitter/merger) still switch instantly: their bypass changes the
// topology, and there is no single buffer to fade between.
namespace dcrig
{

// The ramp length. 10 ms is the usual buffered-bypass figure: long enough that
// a full-scale step spreads below the click threshold, short enough that a
// footswitch still feels instant.
constexpr float kFadeSeconds = 0.010f;

// Per-node wet weight. Written ONLY by the audio thread inside process(); the
// message thread reads it (relaxed) to know when a fade-out has landed.
struct NodeFade
{
    std::atomic<float> wet{ 1.0f };
    // Set the first time the module actually runs. A node that never ran has
    // nothing to fade OUT of: bypassing it snaps to 0, so a preset loaded with
    // bypassed pedals — or a test that adds, bypasses and renders — is
    // bit-exact pass-through from the first sample instead of 10 ms of a
    // module nobody has heard fading away.
    std::atomic<bool>  ran{ false };
};

// ---------------------------------------------------------------------------
// SPILLOVER (ABI v37). When a preset switch REMOVES a time-based module — a
// catalogue entry whose category is `delay` or `reverb` — and spillover is on
// for the seat, the module is not destroyed: its input is ramped to silence
// IN PLACE over kFadeSeconds (so neither the dry path nor the tail steps), and
// the same publication that takes it out of the chain re-publishes it as a
// post-chain TAIL. A tail is processed with silent input after every other
// node and summed into the chain output (pre master) until it has rung for
// kSpillMaxSeconds or its output has stayed under kSpillQuietLevel for
// kSpillQuietSeconds; the audio thread then marks it Done and the next
// publication on the message thread retires it (after the barrier — never a
// free on the callback). At most kMaxSpillTails ring at once per seat: a new
// one past that asks the OLDEST to fade out over kFadeSeconds.
//
// OFF at engine start, like every other rig feature the host opts into; off,
// the chain is the pre-v37 path byte for byte.
// ---------------------------------------------------------------------------
constexpr float kSpillMaxSeconds   = 8.0f;
constexpr float kSpillQuietSeconds = 0.2f;
constexpr float kSpillQuietLevel   = 3.1622777e-5f; // -90 dBFS
constexpr int   kMaxSpillTails     = 4;

// One tail's life. `state` and `fastFade` cross threads; the rest is written by
// the message thread ONLY before the snapshot that names the tail is published
// (armSpillLocked), and by the audio thread only after.
struct SpillTail
{
    static constexpr int kIdle = 0, kRinging = 1, kDone = 2;
    std::atomic<int>  state{ kIdle };      // audio thread moves Ringing -> Done
    std::atomic<bool> fastFade{ false };   // message -> audio: ramp out, then Done
    float             gain = 1.0f;         // audio thread
    long long         rendered = 0;        // audio thread, samples
    long long         quiet = 0;           // audio thread, consecutive quiet samples
    unsigned long     armSeq = 0;          // message thread: arm order (oldest first)
};

// ---------------------------------------------------------------------------
// NodeMeter — per-node input/output level, written by the audio thread and
// POLLED from the message thread (ABI v30). There is no callback, no queue and
// no allocation: the callback folds each block into a decaying envelope and the
// UI reads whatever is there whenever it asks. That is the only shape that
// respects this repo's rule that nothing pumps audio in managed code.
//
// The audio thread is the SOLE writer (DspChain::process runs under
// processMutex_), so the envelopes live in the published atomics themselves and
// every access is relaxed — the values are a picture, and a torn read costs one
// wrong pixel in one frame of a 30 Hz meter.
//
// `ms` holds the MEAN SQUARE, not the RMS: the sqrt is done once per poll on
// the message thread rather than twice per node per block on the audio thread.
//
// The `!(v > eps)` tests are doing two jobs. They park a decaying envelope at
// exactly 0 instead of letting it trail off into subnormals (the offline
// tooling path has no FTZ), and — because every comparison against NaN is
// false — they also mean a NaN block level resets the meter instead of latching
// it forever, which is the one way a meter could outlive the sample that made
// it.
// ---------------------------------------------------------------------------
struct NodeMeter
{
    std::atomic<float> inPeak{ 0.0f };
    std::atomic<float> inMs{ 0.0f };
    std::atomic<float> outPeak{ 0.0f };
    std::atomic<float> outMs{ 0.0f };

    static void step(std::atomic<float>& peak, std::atomic<float>& ms,
                     float blockPeak, float blockMs, float kPeak, float kRms) noexcept
    {
        float p = peak.load(std::memory_order_relaxed) * (1.0f - kPeak);
        if (blockPeak > p) p = blockPeak;
        if (!(p > 1.0e-9f)) p = 0.0f;
        peak.store(p, std::memory_order_relaxed);

        float m = ms.load(std::memory_order_relaxed);
        m += (blockMs - m) * kRms;
        if (!(m > 1.0e-18f)) m = 0.0f;
        ms.store(m, std::memory_order_relaxed);
    }

    void stepIn(float p, float m, float kP, float kR) noexcept  { step(inPeak, inMs, p, m, kP, kR); }
    void stepOut(float p, float m, float kP, float kR) noexcept { step(outPeak, outMs, p, m, kP, kR); }
    void decay(float kP, float kR) noexcept
    {
        step(inPeak, inMs, 0.0f, 0.0f, kP, kR);
        step(outPeak, outMs, 0.0f, 0.0f, kP, kR);
    }
    void clear() noexcept
    {
        inPeak.store(0.0f, std::memory_order_relaxed);
        inMs.store(0.0f, std::memory_order_relaxed);
        outPeak.store(0.0f, std::memory_order_relaxed);
        outMs.store(0.0f, std::memory_order_relaxed);
    }
};

// Meter ballistics. Peak falls 1/e per kMeterPeakTau seconds (~34 dB/s, fast
// enough to read a palm mute, slow enough to be visible at 30 fps); the RMS
// integrates over kMeterRmsTau, the usual ~300 ms VU window.
constexpr float kMeterPeakTau = 0.25f;
constexpr float kMeterRmsTau  = 0.30f;

// Floats written per node by readMeters(): inPeak, inRms, outPeak, outRms.
constexpr int kMeterFloatsPerNode = 4;

class DspChain
{
public:
    /// Sidechain key source for one node (ABI v34). Mirrored in
    /// dotcharter_audio.h and in the managed JuceDspSidechainSource.
    enum Sidechain
    {
        kSidechainNone      = 0, ///< key off the module's own input (default)
        kSidechainSeatInput = 1, ///< the seat's DRY input, i.e. this chain's input block
        kSidechainSongMix   = 2  ///< the song / master mix, handed in by the engine
    };

    struct NodeRef
    {
        DspModule*      module;
        bool            bypassed;
        int             branch;   // 0 = main/branch 1; 1..3 = explicit branch
        SplitterModule* splitter; // non-null when the module is a splitter
        MergerModule*   merger;   // non-null when the module is a merger
        NodeMeter*      meter;    // v30; never null, owned by meterPool_
        NodeFade*       fade;     // wet/dry ramp; never null, owned by fadePool_
        int             sidechain; // v34; one of Sidechain
        // v37 spillover. `spilling`: the node is being handed over to a tail
        // IN PLACE — its INPUT ramps to silence while its output keeps
        // ringing. `spill` non-null: the node is a POST-CHAIN tail, skipped by
        // the chain walk and processed on silence after it (owned by tailPool_).
        bool            spilling = false;
        SpillTail*      spill    = nullptr;
    };

    using Order = std::vector<NodeRef>;

private:
    /// The three per-node facts the audio thread reads out of a published
    /// snapshot, and the message thread's owning record. BOTH are declared up
    /// here, ahead of every member function, because publishListLocked names
    /// NodeFlags and abortEditLocked names Slot in a SIGNATURE — the class is
    /// complete inside a member function BODY but not in a declaration.
    struct NodeFlags { bool bypassed; int branch; int sidechain; };

    struct Slot
    {
        std::unique_ptr<DspModule> module;
        bool                       bypassed = false;
        int                        branch = 0; // v23 branch tag (0 = main)
        int                        sidechain = kSidechainNone; // v34 key source
        int                        meter = -1; // index into meterPool_ (v30)
        int                        fade = -1;  // index into fadePool_
        // Bumped by every setBypass. A move/retag samples it BEFORE its ~10 ms
        // unlocked fade wait and compares it after, so a footswitch that landed
        // inside that window is not silently clobbered by the restore — see
        // restoreLiveIfUntouchedLocked.
        unsigned long              bypassSeq = 0;
        // v37: a remove decided this node spills (see SpillTail); published as
        // NodeRef::spilling until the remove moves it into spills_.
        bool                       spilling = false;
    };

    /// A removed node that is ringing out as a post-chain tail (v37).
    struct SpillSlot
    {
        Slot slot;
        int  tail = -1; // index into tailPool_
    };

    /// Clears `committing_` and wakes every waiter on ANY exit from commitEdit,
    /// exception included. `committing_ = false` on the success path only left
    /// the flag set FOR EVER if one of the commit's allocations threw, after
    /// which every beginEdit blocked permanently on a predicate that could never
    /// become true — an unbounded hang on the UI thread, which this file's own
    /// v35 note calls worse than losing the edit. Declared here for the same
    /// reason NodeFlags and Slot are: a member function BODY sees the complete
    /// class, so this may sit anywhere in it.
    struct CommitGuard
    {
        DspChain* self = nullptr; // null until the commit actually claimed the scope
        ~CommitGuard()
        {
            if (self == nullptr) return;
            {
                std::lock_guard<std::mutex> lock(self->writeMutex_);
                self->committing_ = false;
            }
            self->editCv_.notify_all();
        }
    };

    /// How long beginEdit and ~DspChain wait for an in-flight commit before
    /// forcing the valve. Generously past the commit's own bound (one fade
    /// window plus three blocks), because the point is to bound a WEDGE, not to
    /// race a healthy commit.
    static constexpr int kCommitWaitMs = 500;

public:
    DspChain() { std::atomic_store(&order_, std::make_shared<const Order>()); }

    /// An open edit scope owns modules that are still in the published order
    /// (see editRemoved_), so tearing the chain down under one — which is what
    /// dca_engine_destroy does to a caller that never committed — has to end
    /// the transaction first rather than leak the parked slots.
    ///
    /// A commit in flight on ANOTHER thread is waited out first (bounded). The
    /// commit drops writeMutex_ for its fade window with `committing_` set, and
    /// abortEdit is REFUSED in that state — so the destructor used to return a
    /// silent false, end no transaction at all, and let the running commit go on
    /// holding NodeFade*s into a fadePool_ that was being destroyed under it.
    ~DspChain()
    {
        {
            std::unique_lock<std::mutex> lock(writeMutex_);
            editCv_.wait_for(lock, std::chrono::milliseconds(kCommitWaitMs),
                             [this] { return ! committing_; });
        }
        abortEdit();
    }

    // ---- Message thread (owns modules_, order_ writer side) ----------------

    /// `maxBlockSize` SIZES the scratch (it carries 2x headroom, so it is
    /// >= 2048 in practice). `deviceBlockSize` is what the callback actually
    /// renders, and it is used only for TIMING — how long one block takes, and
    /// therefore how long a structure edit may wait for a fade to land. Using
    /// the sizing figure for that made every remove / move / retag wait as if a
    /// block were 2,048 frames: ~85 ms on an engine with no device open at all,
    /// and 10 ms per step of a UI drag on one that has. Pass 0 to fall back to
    /// `maxBlockSize` (offline callers, where nothing is waiting anyway).
    /// How many 1 ms sleeps structure edits have spent waiting for a fade to
    /// land. Test hook: an edit on a chain nothing has ever processed must cost
    /// none.
    unsigned long fadeWaitSleepsForTest() const noexcept
    { return fadeWaitSleeps_.load(std::memory_order_relaxed); }

    /// The block size fade timing is derived from. Test hook: a wall-clock
    /// assertion on the wait itself would flake the moment two builds run at
    /// once, so the tests assert the INPUT to the bound instead.
    int timingBlockForTest() const noexcept { return deviceBlock_; }

    void prepare(double sampleRate, int maxBlockSize, int numChannels, int deviceBlockSize = 0)
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        // ...AND the audio thread's own mutex, across the WHOLE body. Every
        // line below is state process() reads while it runs: branchData_ and
        // dryData_ are REALLOCATED (the old blocks freed under memcpys that
        // hold pointers into them), numChannels_/maxBlock_ decide whether
        // process() may touch that scratch at all, and each module's prepare()
        // rewrites coefficients and delay-line memory the same block is
        // walking. writeMutex_ alone defended NONE of it: prepare() took only
        // writeMutex_ and process() takes only processMutex_, so the two ran
        // concurrently — prepareMasterBus on a device change / NAM 48 kHz
        // request / watchdog recovery against a live callback, and
        // dspProcessOffline re-preparing from a worker thread.
        //
        // Order is writeMutex_ -> processMutex_, the same order publishLocked,
        // remove() and clear() already take (each publishes under writeMutex_
        // then calls barrier(), which IS processMutex_); the audio thread never
        // takes writeMutex_, so it cannot invert. This holds the callback out
        // for the length of a rig prepare — a device-thread event with no
        // callback in flight on the normal path, and the price of correctness
        // on the paths where there is one.
        std::lock_guard<std::mutex> audio(processMutex_);
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        maxBlock_ = std::max(1, maxBlockSize);
        deviceBlock_ = deviceBlockSize > 0 ? deviceBlockSize : maxBlock_;
        numChannels_ = std::max(1, numChannels);
        prepared_ = true;
        // Meter ballistics are per-block coefficients derived from the rate, so
        // they have to follow a device change like every other coefficient; and
        // the levels themselves describe audio that is about to be discarded.
        rtInvSampleRate_.store((float) (1.0 / sampleRate_), std::memory_order_release);
        for (auto& m : meterPool_) if (m) m->clear();
        // Branch scratch for split/parallel regions (ABI v23): one buffer per
        // branch x channel. Allocated here ONLY — process() just reads the
        // prebuilt pointer tables. NOTE: prepare() runs before the device
        // starts (and the audio thread reads these vectors), same contract as
        // every module's own prepare().
        branchData_.assign((size_t) kDspMaxBranches * numChannels_ * maxBlock_, 0.0f);
        branchPtrs_.assign((size_t) kDspMaxBranches, {});
        for (int b = 0; b < kDspMaxBranches; ++b)
        {
            branchPtrs_[(size_t) b].resize((size_t) numChannels_);
            for (int c = 0; c < numChannels_; ++c)
                branchPtrs_[(size_t) b][(size_t) c] =
                    branchData_.data() + ((size_t) b * numChannels_ + c) * maxBlock_;
        }
        // One linear step per sample so the ramp is kFadeSeconds long at THIS
        // rate — a device change re-derives it like every other coefficient.
        rtFadeInc_.store((float) (1.0 / (kFadeSeconds * sampleRate_)), std::memory_order_release);
        // Dry copy for the crossfade: one buffer per channel, block-sized.
        // Allocated here ONLY, like the branch scratch.
        dryData_.assign((size_t) numChannels_ * maxBlock_, 0.0f);
        dryPtrs_.resize((size_t) numChannels_);
        for (int c = 0; c < numChannels_; ++c)
            dryPtrs_[(size_t) c] = dryData_.data() + (size_t) c * maxBlock_;
        // Sidechain key: the chain's INPUT block, kept for the whole pass so a
        // node late in the chain can key off the dry pick. Allocated HERE only,
        // like every other audio-thread buffer, and sized for the worst case
        // whether or not anything is currently keyed off it — a node can be
        // keyed while the device is running, and growing this on the callback
        // is exactly the audit-#5 bug.
        scDryData_.assign((size_t) numChannels_ * maxBlock_, 0.0f);
        scDryPtrs_.resize((size_t) numChannels_);
        for (int c = 0; c < numChannels_; ++c)
            scDryPtrs_[(size_t) c] = scDryData_.data() + (size_t) c * maxBlock_;
        scSongData_.assign((size_t) numChannels_ * maxBlock_, 0.0f);
        scSongPtrs_.resize((size_t) numChannels_);
        for (int c = 0; c < numChannels_; ++c)
            scSongPtrs_[(size_t) c] = scSongData_.data() + (size_t) c * maxBlock_;
        // Spillover tail scratch (v37): the tails run one at a time over silence
        // and are summed into the chain output, so ONE block-sized buffer per
        // channel serves them all. Allocated here only, like every other buffer
        // the callback touches.
        spillData_.assign((size_t) numChannels_ * maxBlock_, 0.0f);
        spillPtrs_.resize((size_t) numChannels_);
        for (int c = 0; c < numChannels_; ++c)
            spillPtrs_[(size_t) c] = spillData_.data() + (size_t) c * maxBlock_;
        // A tail is NOT re-prepared: it was ringing at the old rate, and what is
        // left of it is not worth re-deriving. It is ended here (no callback is
        // in flight — processMutex_ is held) and retired by the next publish.
        for (auto& s : spills_)
            if (SpillTail* t = tailAtLocked(s.tail)) t->state.store(SpillTail::kDone, std::memory_order_release);
        for (auto& kv : modules_) kv.second.module->prepare(sampleRate, maxBlock_, numChannels_);
        // A node removed inside an OPEN edit scope is still in the published
        // order and still being processed by the callback — it just is not in
        // modules_ any more. A device change mid-edit must re-derive its
        // coefficients too, or it runs the rest of the scope at the old rate.
        for (auto& pr : editRemoved_)
            if (pr.second.module) pr.second.module->prepare(sampleRate, maxBlock_, numChannels_);
        // Latency is a RATE-derived number for most of the modules that have
        // any (oversampler group delay, WSOLA windows), and prepare() is where
        // they re-derive it — so branch alignment has to be re-derived here as
        // well, or a device change leaves a region compensated for the old
        // rate. The merger's own rings were just reallocated by its prepare().
        // From the PUBLISHED order: a device change that lands mid-edit-scope
        // must re-align the chain that is still playing, not the shadow one
        // the caller has not committed yet.
        recomputeBranchPdcLocked(publishedOrder_);
        // Idle sleep: the hold is a number of SAMPLES, so it follows the rate,
        // and a re-prepared rig is a changed rig — wake a sleeping seat.
        publishIdleHoldLocked();
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }

    // Add a module (already constructed) at the end. Returns the new node id.
    int addNode(std::unique_ptr<DspModule> module, bool initiallyBypassed = false)
    {
        if (module == nullptr) return -1;
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int id = nextId_++;
        if (prepared_) module->prepare(sampleRate_, maxBlock_, numChannels_);
        auto& slot = modules_[id];
        slot.module = std::move(module);
        slot.bypassed = initiallyBypassed;
        slot.meter = acquireMeterLocked();
        // Legacy/offline callers retain immediate full-wet insertion. Live
        // preset construction opts into a parked node so no callback can hear
        // its defaults before parameters, content and position are configured.
        // INSIDE AN EDIT SCOPE every new node is parked regardless: it is not
        // in any published order yet, and parking is what lets the whole preset
        // fade in as one gesture at commit.
        slot.fade = acquireFadeLocked((initiallyBypassed || editing_) ? 0.0f : 1.0f);
        idOrder_.push_back(id);
        if (editing_) editAdded_.push_back(id);
        publishLocked();
        return id;
    }

    bool removeNode(int nodeId)
    {
        // Declared out here so a module destroyed by the shadow path below dies
        // AFTER writeMutex_ is released (a module destructor can be a
        // NeuralAudio teardown or a file-handle close).
        std::vector<Slot> doomed;
        // Inside an edit scope a remove is a SHADOW remove: nothing is
        // published, so the node keeps playing out of the live snapshot and
        // the commit fades it out with every other departing node at once.
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (editing_) return removeNodeEditingLocked(nodeId, doomed);
            // Spillover (v37): an audible delay/reverb leaving the chain rings
            // out instead of being cut. Marked BEFORE the fade below publishes,
            // so the one fade window is the INPUT ramp of the hand-over rather
            // than an output fade that would take the tail with it.
            auto sit = modules_.find(nodeId);
            if (sit != modules_.end() && spillEligibleLocked(sit->second))
                sit->second.spilling = true;
        }
        // Fade the node out of the path FIRST, so the splice below happens on
        // a node whose contribution is already zero. A node that is already
        // bypassed (weight 0) goes straight through.
        if (! fadeOutAndWait(nodeId)) return false;

        std::unique_ptr<DspModule> retired;
        int retiredMeter = -1;
        int retiredFade = -1;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            // RE-CHECK `editing_`. It was false above, the lock was then DROPPED
            // for a fade wait of up to ~150 ms, and a beginEdit from another
            // thread can land in that window — the managed JuceDspEditScope's
            // finalizer runs on the GC thread and the UI thread removes nodes, so
            // this needs no deliberate threading to reach. With a scope open,
            // publishLocked() below EARLY-RETURNS, so the erase+destroy freed a
            // module whose raw pointer is still in the snapshot the audio
            // callback is reading; barrier() cannot help, because the snapshot
            // was never replaced. Use-after-free on the callback.
            //
            // Falling into the shadow path instead keeps the module alive in
            // editRemoved_ until the commit's barrier retires it. clear() has
            // always made this decision atomically under one lock, which is what
            // showed the correct shape; this one was check-then-act.
            if (editing_) return removeNodeEditingLocked(nodeId, doomed);
            auto it = modules_.find(nodeId);
            if (it == modules_.end()) return false;
            if (it->second.spilling)
            {
                // ONE publication moves it from its slot in the chain (where it
                // has been ringing over a silenced input) to the post-chain
                // tail list, so no block ever sees it in both places or in
                // neither. Nothing is destroyed here; the tail is retired by a
                // later publish once the audio thread has marked it Done.
                armSpillLocked(it->second);
                modules_.erase(it);
                eraseId(nodeId);
                publishLocked();
                return true;
            }
            retired = std::move(it->second.module); // keep alive past the barrier
            retiredMeter = it->second.meter;
            retiredFade = it->second.fade;
            modules_.erase(it);
            eraseId(nodeId);
            publishLocked();
        }
        // The old snapshot may still be in an audio callback that referenced
        // this module — barrier until every such block has finished, THEN the
        // unique_ptr goes out of scope and destroys it.
        barrier();
        // Past the barrier no callback can still be writing this node's meter,
        // so the slot is safe to hand to the next add. (The NodeMeter object
        // itself is never destroyed before the chain is — meterPool_ only ever
        // grows — so even a bug here could not dangle, only mix two nodes'
        // levels.)
        releaseMeter(retiredMeter);
        releaseFade(retiredFade);
        return true;
    }

    bool moveNode(int nodeId, int newIndex)
    {
        // Inside an edit scope a move is pure bookkeeping against the shadow:
        // nothing is published, so there is no slot to fade out OF.
        unsigned long seq = 0;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (editing_) return reorderLocked(nodeId, newIndex);
            auto it = modules_.find(nodeId);
            if (it == modules_.end()) return false;
            seq = it->second.bypassSeq;
        }
        // Out at the old slot, in at the new one — two fades, no splice. The
        // node's own bypass flag is restored afterwards, so a user-bypassed
        // pedal moves silently and stays bypassed.
        const bool wasLive = fadeOutAndWait(nodeId, /*onlyIfLive*/ true);
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto pos = std::find(idOrder_.begin(), idOrder_.end(), nodeId);
        if (pos == idOrder_.end()) return false;
        idOrder_.erase(pos);
        if (newIndex < 0) newIndex = 0;
        if (newIndex > (int) idOrder_.size()) newIndex = (int) idOrder_.size();
        idOrder_.insert(idOrder_.begin() + newIndex, nodeId);
        if (wasLive) restoreLiveIfUntouchedLocked(nodeId, seq);
        publishLocked();
        return true;
    }

    bool setBypass(int nodeId, bool bypassed)
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        if (! bypassed && it->second.bypassed) resetIfParkedLocked(it->second);
        it->second.bypassed = bypassed;
        // The USER expressed a bypass intent for this node. A move/retag that is
        // mid-fade samples this counter and will not clobber the flag we just
        // wrote (restoreLiveIfUntouchedLocked).
        ++it->second.bypassSeq;
        if (bypassed) snapIfNeverRanLocked(it->second);
        publishLocked();
        return true;
    }

    // Branch tag for split/parallel regions (ABI v23). 0 = main (branch 1
    // inside a split region); 1..kDspMaxBranches = explicit branch. The tag is
    // node state (survives reorders) and only takes effect between a splitter
    // and its merger. Out-of-range values are rejected.
    bool setNodeBranch(int nodeId, int branch)
    {
        if (branch < 0 || branch > kDspMaxBranches) return false;
        // Shadow retag inside an edit scope: no publication, so no fade.
        unsigned long seq = 0;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            auto it = modules_.find(nodeId);
            if (it == modules_.end()) return false;
            if (editing_)
            {
                it->second.branch = branch;
                return true;
            }
            seq = it->second.bypassSeq;
        }
        // A retag moves the node between branch buffers — the same splice as a
        // reorder, faded the same way.
        const bool wasLive = fadeOutAndWait(nodeId, /*onlyIfLive*/ true);
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        it->second.branch = branch;
        if (wasLive) restoreLiveIfUntouchedLocked(nodeId, seq);
        publishLocked();
        return true;
    }

    // ABI v33. The type id a node was created from — the chain has always known
    // it (DspModule::typeId()) and simply never exposed it, so a caller that did
    // not create the node had no way to look its parameters up.
    bool getNodeTypeId(int nodeId, std::string& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        const char* t = it->second.module->typeId();
        out = t != nullptr ? t : "";
        return true;
    }

    // ABI v33. The node's bypass FLAG — the message-thread intent, i.e. the
    // value setBypass last stored. It is deliberately NOT the fade position:
    // the crossfade is the audio thread's business and a getter that reported
    // it would flicker for 10 ms after every toggle.
    bool getBypass(int nodeId, bool& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        out = it->second.bypassed;
        return true;
    }

    // ---- Sidechain (ABI v34) -----------------------------------------------

    /// Key this node's detector from another signal. REJECTED (false) for an
    /// unknown node, an out-of-range source, or a module that does not declare
    /// `supportsSidechain()` — a source silently ignored by the module would
    /// read as a live feature that does nothing, which is the exact shape of
    /// this repo's dead-feature bugs.
    bool setNodeSidechain(int nodeId, int source)
    {
        if (source < kSidechainNone || source > kSidechainSongMix) return false;
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        if (source != kSidechainNone && ! it->second.module->supportsSidechain()) return false;
        it->second.sidechain = source;
        publishLocked();
        return true;
    }

    bool getNodeSidechain(int nodeId, int& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        out = it->second.sidechain;
        return true;
    }

    bool getNodeBranch(int nodeId, int& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        out = it->second.branch;
        return true;
    }

    bool setParam(int nodeId, const char* paramId, float value)
    {
        // NaN is REJECTED here, at the one place every param write funnels
        // through, and reported as a failure rather than swallowed.
        //
        // Every core clamps with `v < lo ? lo : (v > hi ? hi : v)`, and BOTH
        // comparisons are false for NaN — so a clamp returns NaN unchanged.
        // Inf is caught (`Inf > hi` is true); NaN is the single hole, and it is
        // permanent: it reaches a filter coefficient or a smoother target, and
        // from there the module's own state is NaN for ever. `DspChain::process`
        // scrubs the BUFFER, never a module's internal state, so the pedal goes
        // silent and stays silent until the node is removed and re-added.
        //
        // Four cores (filterseq, feedbacker, resonator, graphiceq) already had
        // their own `if (!(v == v))` guard, so the intent existed and had been
        // applied to 4 of 54. One gate is cheaper than 50 copies of it — and a
        // core that keeps its own guard is still correct, just redundant.
        if (!(value == value)) return false;

        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        // ...and the CATALOGUE's declared [min,max] is enforced HERE, once, for
        // all 57 module types. Neither the ABI (non-finite only) nor this
        // function (NaN only) used to clamp, and only 3 of 57 adapters did it
        // themselves — so `thresholdDb = +50` on a param declared [-80, 0] was
        // accepted, stored raw, and READ BACK as 50 while the core ran 0.
        // DspModule.h's own contract says the opposite, and this is the same
        // defect ABI v33 fixed for dca_source_set_pan ("so a cached managed
        // getter cannot disagree with the engine") left unfixed one layer in.
        // A param the catalogue does not declare passes through untouched.
        value = clampToCatalogue(it->second.module->typeId(), paramId, value);
        if (! it->second.module->setParam(paramId, value)) return false;
        // A param can move a module's LATENCY (whammy's lowLatency flips its
        // whole geometry, overdrive's model switches the oversampler in and
        // out, vibrato's depth is its delay). Branch alignment is derived from
        // those numbers, so it has to follow a param write and not only a
        // structure change — this is a cheap walk over the node list, on the
        // message thread, and it is the only thing keeping the compensation
        // honest between two publishes.
        //
        // Over the PUBLISHED order, never idOrder_. Inside an edit scope those
        // differ, and a knob turned during a preset load used to re-align the
        // audible region for a chain that had not been published yet: nodes the
        // scope removed were still playing and no longer counted, nodes it
        // added were counted and not audible. Walking what is published keeps
        // the live region aligned to itself, and the commit re-derives from the
        // new order the moment it publishes it.
        recomputeBranchPdcLocked(publishedOrder_);
        // ...and AGAIN on the callback, once the core has actually adopted the
        // value (W4-D54). The walk above reads latencySamples() while the new
        // params are still sitting in the ParamMailbox, so for every adapter
        // that delegates to its core it programs the mergers from the OLD
        // latency — and nothing came back afterwards, so the region stayed
        // misaligned until an unrelated publish. This flag is the "come back"
        // half; see recomputeBranchPdcRt.
        pdcDirty_.store(true, std::memory_order_release);
        // Idle sleep: a knob turn wakes a sleeping seat on its next block and
        // restarts the hold, so the new setting is heard (and measured) first —
        // and a release knob moves the hold itself.
        recomputeIdleHoldLocked();
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    bool getParam(int nodeId, const char* paramId, float& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        return it->second.module->getParam(paramId, out);
    }

    /// Does this node exist, and does it know this parameter id? A RESOLVE-ONLY
    /// probe, added for the automated offline render: it lets the whole event
    /// list be checked before a single sample is rendered, so a rejected event
    /// can no longer leave the rig half-swept (a wah parked at 0.37, a pedal
    /// bypassed) with no way for the caller to know or undo it.
    ///
    /// It is NOT a second lookup path: it walks `modules_` and asks the module
    /// the SETTER's question, through `DspModule::hasSettableParam`. It used to
    /// ask `getParam`, on the stated grounds that the two share one contract —
    /// and they do not: `namamp` answers `getParam` for three POLL-ONLY status
    /// ids ("loaded", "rateMismatch", "resampling") that its `setParam` rejects.
    /// So the pre-pass accepted an event naming one of them, the render started,
    /// and `applyAutomationEvent` then failed MID-RENDER — leaving exactly the
    /// arbitrary half-swept rig the pre-pass exists to prevent. A null/empty id,
    /// and the reserved `"bypass"` (which the CHAIN owns and no module declares),
    /// are the caller's to handle.
    bool hasParam(int nodeId, const char* paramId) const
    {
        if (paramId == nullptr || paramId[0] == '\0') return false;
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        return it->second.module->hasSettableParam(paramId);
    }

    /// Does this node exist at all? The `"bypass"` half of the same probe:
    /// bypass is owned by the chain, so there is no module id to resolve.
    bool hasNode(int nodeId) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        return it != modules_.end() && it->second.module != nullptr;
    }

    // String-valued params (ABI v22). Same threading contract as setParam:
    // message thread under writeMutex_; the module defers any heavy work
    // (file parse / model load) off-thread itself.
    bool setStringParam(int nodeId, const char* paramId, const char* utf8Value)
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        if (! it->second.module->setStringParam(paramId, utf8Value)) return false;
        // Same reason as setParam: a string param can move a latency (a NAM
        // capture at a different rate brings a resampler with it), and the same
        // published-order rule.
        recomputeBranchPdcLocked(publishedOrder_);
        // ...and the same callback-side re-derive (W4-D54). namamp's latency
        // moves when the WORKER publishes the loaded model, which is later
        // still than a mailbox adoption, so this one matters even more than the
        // setParam case.
        pdcDirty_.store(true, std::memory_order_release);
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
        return true;
    }

    bool getStringParam(int nodeId, const char* paramId, std::string& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        return it->second.module->getStringParam(paramId, out);
    }

    int nodeCount() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return (int) idOrder_.size();
    }

    // Whether prepare() has ever run (device start or offline tooling). Used
    // by the offline process path to lazily prepare a device-less rig.
    bool isPrepared() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return prepared_;
    }

    // Audio-thread-safe node count for the per-block early-out: a real atomic,
    // never a mutex — writeMutex_ is held across prepare() allocations on the
    // message thread and must never be contended from the audio callback
    // (priority inversion → dropout).
    int nodeCountRt() const noexcept
    {
        return rtNodeCount_.load(std::memory_order_acquire);
    }

    // ---- Idle sleep (dsp/IdleSleep.h), audio-thread reads ------------------
    // The chain does not sleep itself — process() always processes, which is
    // what keeps the offline renders exact. The DEVICE callback asks these
    // three questions and decides (AudioEngine::processDspRigLive).

    /// Bumped by every message-thread write that can change what the rig
    /// outputs: a publication (add / remove / move / bypass / branch /
    /// sidechain / clear / commit), a param or string param, prepare(), the
    /// branch-PDC switch and the meters. A sleeping seat wakes on the first
    /// block that sees it move.
    uint64_t rtEditGeneration() const noexcept
    {
        return rtEditGeneration_.load(std::memory_order_acquire);
    }

    /// How long, in samples at the prepared rate, the rig's output has to stay
    /// quiet before it may sleep: the base hold or the longest settle a node in
    /// the PUBLISHED order declares. -1 when a node's type is not on the
    /// allow-list (or the chain is empty) — the seat never sleeps.
    long long rtIdleHoldSamples() const noexcept
    {
        return rtIdleHoldSamples_.load(std::memory_order_acquire);
    }

    /// False while something makes sleeping unsafe whatever the level: a
    /// bypass / move / insert crossfade was still ramping in the last block
    /// (the ramp only advances inside process()), the per-node meters are on
    /// (a sleeping rig would freeze them), or a node is keyed from the SONG
    /// mix (the song can open a keyed stage with no guitar input at all).
    /// The chain's modules were advanced by someone other than the device
    /// callback (the offline renders): a seat asleep on the OLD state must not
    /// resume from a state it never saw being made. Any thread.
    void noteExternalProcess() noexcept
    {
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool rtMayRest() const noexcept
    {
        return ! rtFadedLastBlock_.load(std::memory_order_relaxed)
            && ! metering_.load(std::memory_order_relaxed)
            && ! rtSidechainSong_.load(std::memory_order_relaxed);
    }

    // Copies current node ids (chain order) into out. Returns the count.
    int getNodeIds(int* out, int capacity) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int n = (int) idOrder_.size();
        const int m = (n < capacity) ? n : capacity;
        for (int i = 0; i < m; ++i) out[i] = idOrder_[i];
        return n;
    }

    void clear()
    {
        std::vector<std::unique_ptr<DspModule>> retired;
        std::vector<int> retiredMeters;
        std::vector<int> retiredFades;
        std::vector<int> retiredTails;
        std::vector<Slot> doomed; // destroyed after writeMutex_ is released
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (editing_)
            {
                // Shadow clear: every node becomes a pending removal, so a
                // "clear then rebuild" preset load is still ONE commit.
                const std::vector<int> ids = idOrder_;
                for (int id : ids) removeNodeEditingLocked(id, doomed);
                return;
            }
            for (auto& kv : modules_)
            {
                retired.push_back(std::move(kv.second.module));
                retiredMeters.push_back(kv.second.meter);
                retiredFades.push_back(kv.second.fade);
            }
            // A clear is a RESET, not a preset switch: ringing tails end with
            // it (v37). Retired exactly like the nodes — after the barrier.
            for (auto& s : spills_)
            {
                retired.push_back(std::move(s.slot.module));
                retiredMeters.push_back(s.slot.meter);
                retiredFades.push_back(s.slot.fade);
                retiredTails.push_back(s.tail);
            }
            spills_.clear();
            modules_.clear();
            idOrder_.clear();
            publishLocked();
        }
        barrier();
        for (int m : retiredMeters) releaseMeter(m);
        for (int f : retiredFades) releaseFade(f);
        if (! retiredTails.empty())
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            for (int t : retiredTails) releaseTailLocked(t);
        }
    }

    // ---- Edit scopes (ABI v34) ---------------------------------------------

    /// Open an edit scope.
    ///
    /// A begin on a chain that ALREADY has one open ABORTS the stale scope,
    /// logs at Warning, and opens a fresh one — it is not a nesting level and
    /// not a reference count. This reverses the v34 shipping behaviour (it
    /// returned false) on purpose, and the reason is that returning false left
    /// the dead scope OPEN. `editing_` suppresses every publication, and the
    /// only thing that cleared it without a managed call was ~DspChain — which
    /// for a rig that is a value member of AudioEngine runs exactly once, at
    /// engine destruction. So a caller who dropped a scope (no `using`, an
    /// exception past the commit, a field that went out of scope) froze that
    /// seat's rig for the life of the engine: every later add/remove/move/
    /// bypass accumulated in the shadow, all of them returned success, and
    /// NOTHING was ever published to the callback again. Nobody polls
    /// `editing()`, so there was no tell at all.
    ///
    /// Aborting the stale scope discards its accumulated structure edits, which
    /// is a real cost — but it restores the pre-edit structure rather than
    /// inventing one, it is bounded to the leaked scope, and it is recoverable.
    /// A permanently dead rig is not. The managed `JuceDspEditScope` also has a
    /// finalizer that aborts, so this is the second net, not the first.
    ///
    /// A begin that arrives while another thread is INSIDE `commitEdit` WAITS
    /// for that commit rather than aborting it. `commitEdit` has to drop
    /// `writeMutex_` for up to the 10 ms crossfade (holding it there would block
    /// every other message-thread mutator for the fade), and it drops it with
    /// `editing_` still true — so the stale-scope valve above used to fire into
    /// the middle of it: the abort restored `preEditOrder_`, cleared `editing_`
    /// and republished, and the resumed commit then clobbered the state the NEW
    /// scope had just established (`editing_ = false`, `editAdded_.clear()`,
    /// `preEditOrder_.clear()`), leaving a scope that returned success, published
    /// immediately from then on, and could not be aborted meaningfully. No
    /// deliberate multithreading was needed to reach it: the managed
    /// `JuceDspEditScope` finalizer runs on the GC thread.
    ///
    /// Always returns true.
    bool beginEdit()
    {
        // Declared BEFORE the guard so the stale scope's modules are destroyed
        // after writeMutex_ is released — a module destructor can be a
        // NeuralAudio model teardown.
        std::vector<Slot> doomed;
        bool aborted = false;
        bool forced  = false;
        {
            std::unique_lock<std::mutex> lock(writeMutex_);
            // BOUNDED. An unconditional wait here is an unbounded hang on the UI
            // thread if `committing_` is ever left set — which it was, on any
            // throw out of commitEdit before CommitGuard existed. A commit has
            // its own fade budget of a few tens of ms, so past kCommitWaitMs it
            // is wedged and the right answer is to force the valve and say so,
            // not to block the caller for ever.
            if (! editCv_.wait_for(lock, std::chrono::milliseconds(kCommitWaitMs),
                                   [this] { return ! committing_; }))
            {
                committing_ = false;
                forced = true;
            }
            if (editing_)
            {
                staleScopeAborts_.fetch_add(1, std::memory_order_relaxed);
                doomed  = abortEditLocked();
                aborted = true;
            }
            editing_ = true;
            preEditOrder_ = idOrder_;
            preEditFlags_.clear();
            for (const auto& kv : modules_)
                preEditFlags_[kv.first] = NodeFlags{ kv.second.bypassed, kv.second.branch,
                                                     kv.second.sidechain };
            editAdded_.clear();
            editRemoved_.clear();
        }

        // OUTSIDE the lock, next to where `doomed` dies. logSink_ trampolines
        // into the managed log delegate — arbitrary user code, possibly file I/O,
        // possibly a UI dispatcher — and running that under the chain's write
        // mutex blocked every other message-thread mutator for its duration and
        // DEADLOCKED any handler that read back from the same chain ("log, then
        // dump the node list" is a plausible diagnostic). The surrounding code
        // already got the harder half of this right: `doomed` is declared before
        // the guard precisely so module destructors run outside it.
        if (forced && logSink_ != nullptr)
            logSink_(kLogWarn,
                     "DspChain::beginEdit waited out a commitEdit that never released its "
                     "scope and forced it open. A commit that leaves `committing_` set "
                     "blocks every later edit on this seat's rig.");
        if (aborted && logSink_ != nullptr)
            logSink_(kLogWarn,
                     "DspChain::beginEdit found an edit scope already open and aborted it. "
                     "A dropped JuceDspEditScope (no `using`?) freezes the seat's rig: every "
                     "edit accumulates in the shadow and nothing is published to the audio "
                     "callback. The stale scope's structure edits were discarded.");
        return true;
    }

    bool editing() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return editing_;
    }

    /// Publish everything the scope accumulated. Returns false if no scope is
    /// open.
    ///
    /// At most TWO publications, whatever the preset size:
    ///  * transitional — only when the scope removed a node that is still
    ///    AUDIBLE. It republishes the pre-edit order with those nodes marked
    ///    bypassed (so they fade OUT) and the scope's new nodes appended
    ///    (parked at 0, so they fade IN); both halves ramp in the SAME 10 ms
    ///    window, and the commit then waits once for the fade-outs to land.
    ///  * final — the shadow order itself. `publishListLocked` takes the
    ///    one-block barrier before it drops the previous snapshot, so the
    ///    departing modules are safe to destroy the moment it returns.
    ///
    /// While the commit has `writeMutex_` DROPPED for `waitForFades`, a
    /// `committing_` flag holds the scope: a concurrent `beginEdit` waits for it
    /// and a concurrent `abortEdit` is refused, so the commit always finishes on
    /// the scope that started it. A second concurrent `commitEdit` returns false.
    bool commitEdit()
    {
        std::vector<NodeFade*> waitFor;
        // Clears `committing_` and notifies on EVERY exit, exception included:
        // the allocations below can throw, and the success-path-only clear left
        // the flag set for ever, after which every beginEdit blocked on a
        // predicate that could never become true.
        CommitGuard guard;
        {
            std::unique_lock<std::mutex> lock(writeMutex_);
            if (! editing_ || committing_) return false;
            committing_ = true;
            guard.self  = this;
            for (auto& pr : editRemoved_)
            {
                NodeFade* f = fadeAtLocked(pr.second.fade);
                if (f == nullptr) continue;
                const bool audible = f->wet.load(std::memory_order_relaxed) > 0.0f;
                // Spillover (v37): an audible delay/reverb the preset drops
                // hands over to a tail — its fade in the transitional order is
                // the INPUT ramp, and the final publication below re-publishes
                // it post-chain instead of retiring it. `spilling` may already
                // be set by a removeNode whose scope opened mid-fade.
                if (audible && spillTypeEligibleLocked(pr.second)) pr.second.spilling = true;
                if (! audible) continue;
                pr.second.bypassed = true; // fade it out in the transitional order
                waitFor.push_back(f);
            }
            if (! waitFor.empty())
            {
                // Old chain (at its pre-edit positions) + the scope's new
                // nodes, so the two ramps overlap instead of queueing.
                //
                // FLAGS come from the beginEdit SNAPSHOT, not from the live
                // slots. setNodeBranch writes `branch` and setBypass writes
                // `bypassed` directly during a scope (only the PUBLICATION is
                // suppressed), so publishing the old ORDER off the live slots
                // put the NEW branch tags and bypass states on it — a topology
                // change inside the 10 ms window the scope had promised would
                // be invisible until commit. The one deliberate departure is
                // the nodes this commit is retiring: they were just marked
                // bypassed above so they fade OUT, and that has to survive.
                std::map<int, NodeFlags> transitionalFlags = preEditFlags_;
                for (const auto& pr : editRemoved_)
                {
                    auto fi = transitionalFlags.find(pr.first);
                    if (fi != transitionalFlags.end()) fi->second.bypassed = pr.second.bypassed;
                }
                // ...and the scope's new nodes go in at their SHADOW index, not
                // appended. Appending faded a new drive pedal in POST-cab for
                // the ramp and only then moved it to its real slot, where its
                // fade continued from wherever it had reached.
                std::vector<int> transitional = preEditOrder_;
                for (int id : idOrder_)
                {
                    if (modules_.find(id) == modules_.end()) continue;
                    if (std::find(editAdded_.begin(), editAdded_.end(), id) == editAdded_.end())
                        continue;
                    // Insert after the nearest preceding shadow neighbour that
                    // is already in the transitional list; at the front if the
                    // node opens the shadow chain.
                    size_t at = 0;
                    for (int prev : idOrder_)
                    {
                        if (prev == id) break;
                        auto pos = std::find(transitional.begin(), transitional.end(), prev);
                        if (pos != transitional.end())
                            at = (size_t) (pos - transitional.begin()) + 1;
                    }
                    if (at > transitional.size()) at = transitional.size();
                    transitional.insert(transitional.begin() + (std::ptrdiff_t) at, id);
                    // Added nodes are parked at 0 wet whatever their flag says,
                    // so they take their LIVE slot state (absent from the
                    // pre-edit snapshot by construction).
                }
                publishListLocked(transitional, &transitionalFlags);
            }
        }
        waitForFades(waitFor);

        std::vector<std::pair<int, Slot>> retired;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            retired.swap(editRemoved_);
            // Spilling nodes become tails in the SAME publication that drops
            // them from the chain (v37); armSpillLocked moves the slot out and
            // marks the source so the release loop below skips its pools.
            for (auto& pr : retired)
                if (pr.second.spilling && pr.second.module != nullptr) armSpillLocked(pr.second);
            editing_    = false;
            // `committing_` is cleared (and waiters woken) by `guard`, on this
            // path and on every other one. One owner, not two.
            editAdded_.clear();
            preEditOrder_.clear();
            preEditFlags_.clear();
            publishListLocked(idOrder_);
            // Past publishListLocked's barrier nothing can still reference the
            // retired modules, so their pool slots are safe to recycle here.
            for (auto& pr : retired)
            {
                releaseMeterLocked(pr.second.meter);
                releaseFadeLocked(pr.second.fade);
            }
        }
        retired.clear(); // destroys the departed modules, off the audio thread
        return true;     // ~guard clears committing_ and notifies
    }

    /// Throw the scope away. Restores the pre-edit STRUCTURE — membership,
    /// order, bypass flags and branch tags — and destroys the nodes the scope
    /// added (none of which was ever published, so none can be in a callback).
    /// Parameter VALUES are deliberately not journalled: a scope is a structure
    /// transaction, not an undo stack.
    ///
    /// REFUSED (false) while a `commitEdit` is in flight on another thread. The
    /// commit is past the point of no return — it has already published a
    /// transitional order and is waiting for fades — so tearing its scope out
    /// from under it would make it finish against state it does not own. The
    /// commit wins; an abort that loses simply did not happen.
    bool abortEdit()
    {
        std::vector<Slot> doomed;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            if (! editing_ || committing_) return false;
            doomed = abortEditLocked();
        }
        doomed.clear();
        return true;
    }

    /// The body of abortEdit, for a caller that already holds writeMutex_ —
    /// beginEdit's stale-scope valve. Returns the modules the scope added, for
    /// the caller to destroy OUTSIDE the lock (a module destructor can be a
    /// NeuralAudio model teardown or a file handle close, and holding the write
    /// mutex across one blocks every other message-thread mutator).
    /// Precondition: editing_ is true.
    std::vector<Slot> abortEditLocked()
    {
        std::vector<Slot> doomed;
        {
            for (int id : editAdded_)
            {
                auto it = modules_.find(id);
                if (it == modules_.end()) continue;
                releaseMeterLocked(it->second.meter);
                releaseFadeLocked(it->second.fade);
                doomed.push_back(std::move(it->second));
                modules_.erase(it);
            }
            for (auto& pr : editRemoved_)
            {
                // A remove that had decided to spill is undone with the scope.
                pr.second.spilling = false;
                modules_[pr.first] = std::move(pr.second);
            }
            editRemoved_.clear();
            editAdded_.clear();
            idOrder_ = preEditOrder_;
            for (auto& kv : modules_)
            {
                auto f = preEditFlags_.find(kv.first);
                if (f == preEditFlags_.end()) continue;
                kv.second.bypassed  = f->second.bypassed;
                kv.second.branch    = f->second.branch;
                kv.second.sidechain = f->second.sidechain;
            }
            preEditOrder_.clear();
            preEditFlags_.clear();
            editing_ = false;
            // The live snapshot never moved during the scope, so this
            // republishes an identical order — cheap, and it is the one place
            // that guarantees it, rather than an argument that it must be.
            publishLocked();
        }
        return doomed;
    }

    /// Ordering snapshots published since construction. Test hook: a preset
    /// swap inside a scope must cost a BOUNDED number of these, not one per
    /// node.
    unsigned long publishCountForTest() const noexcept
    { return publishCount_.load(std::memory_order_relaxed); }

    /// How many times beginEdit found a scope already open and aborted it.
    /// A non-zero count means somebody LEAKED an edit scope — the rig kept
    /// working, which is the point, but the leak is a bug in the caller.
    static unsigned long staleScopeAbortsForTest() noexcept
    { return staleScopeAborts_.load(std::memory_order_relaxed); }

    /// Where the stale-scope warning goes. dotcharter_audio.cpp installs a
    /// trampoline into emitLog at static-init time, the same way AudioEngine
    /// does; unset it is a no-op, so the native test executables are
    /// unaffected. `level` mirrors dca_log_level (0 trace … 4 error) and the
    /// ABI TU static_asserts that kLogWarn agrees.
    using LogSink = void (*)(int level, const char* message);
    static void setLogSink(LogSink sink) noexcept { logSink_ = sink; }
    static constexpr int kLogWarn = 3;

    // ---- Per-node meters + latency (ABI v30) -------------------------------

    // Metering is OFF until asked for: it costs two extra passes over the block
    // per node, and a rig the user is not LOOKING at should not pay for a
    // picture nobody is drawing. Off, process() is byte-for-byte the pre-v30
    // path.
    //
    // MESSAGE THREAD ONLY — this said "safe from any thread" and was not: the
    // true->false direction takes writeMutex_ and then barrier()'s
    // processMutex_, which the audio thread holds for the whole block, so
    // calling it FROM the callback self-deadlocks. (The `noexcept` is likewise a
    // claim about two mutex acquisitions; it holds only because neither is ever
    // taken recursively from this path.)
    void setMetering(bool on) noexcept
    {
        const bool was = metering_.exchange(on, std::memory_order_acq_rel);
        // A sleeping rig writes no meters, so opening the meter panel must wake
        // it (and rtMayRest keeps it awake while the panel is open).
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
        if (was && !on)
        {
            // Leaving the meters wherever they happened to stop would freeze a
            // lit rig on screen the moment the UI stopped asking.
            std::lock_guard<std::mutex> lock(writeMutex_);
            // ...and the clear has to WAIT OUT the block that is already in
            // flight. metering_ is read ONCE at the top of process(), so a
            // callback that loaded `true` a microsecond ago will go on writing
            // meters for the rest of its block — after this clear — and leave
            // exactly the frozen lit meter the clear exists to prevent, until
            // the UI asks again. One barrier acquire, on the message thread,
            // at the moment a user closes a meter panel.
            barrier();
            for (auto& m : meterPool_) if (m) m->clear();
        }
    }
    bool metering() const noexcept { return metering_.load(std::memory_order_acquire); }

    // Levels for ONE node (linear, 0..1+ — never dB; the caller decides the
    // scale). Returns false for an unknown node. Message thread.
    bool getNodeMeter(int nodeId, float& inPeak, float& inRms,
                      float& outPeak, float& outRms) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        const NodeMeter* m = meterAtLocked(it->second.meter);
        if (m == nullptr) return false;
        readMeter(*m, inPeak, inRms, outPeak, outRms);
        return true;
    }

    // Bulk read for a UI frame: kMeterFloatsPerNode floats per node, in CHAIN
    // ORDER, matching getNodeIds(). Returns the node COUNT (which may exceed
    // what fitted — same size-then-copy contract as getNodeIds). One call per
    // frame instead of one per pedal.
    int readMeters(float* out, int capacityFloats) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        const int n = (int) idOrder_.size();
        if (out == nullptr || capacityFloats <= 0) return n;
        const int fit = std::min(n, capacityFloats / kMeterFloatsPerNode);
        for (int i = 0; i < fit; ++i)
        {
            float* dst = out + (size_t) i * kMeterFloatsPerNode;
            dst[0] = dst[1] = dst[2] = dst[3] = 0.0f;
            auto it = modules_.find(idOrder_[(size_t) i]);
            if (it == modules_.end()) continue;
            if (const NodeMeter* m = meterAtLocked(it->second.meter))
                readMeter(*m, dst[0], dst[1], dst[2], dst[3]);
        }
        return n;
    }

    // Latency this node adds, in samples at the prepared rate. Returns false
    // for an unknown node. A BYPASSED node reports 0 — it is not in the signal
    // path, and a delay-compensating caller must not budget for it.
    bool getNodeLatency(int nodeId, int& out) const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        auto it = modules_.find(nodeId);
        if (it == modules_.end() || it->second.module == nullptr) return false;
        out = it->second.bypassed ? 0 : std::max(0, it->second.module->latencySamples());
        return true;
    }

    // ---- Branch delay compensation (ABI v34) -------------------------------

    /// Whether a parallel region's branches are delay-aligned at the merger.
    /// DEFAULT ON: summing branches of different latency is a comb filter, not
    /// a blend, and that was the v23 accepted limit rather than a choice. Off
    /// is still offered because the comb is a sound some players want (and
    /// because it is the pre-v34 behaviour, byte for byte).
    void setBranchPdc(bool on)
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (branchPdc_.load(std::memory_order_relaxed) == on) return;
        branchPdc_.store(on, std::memory_order_relaxed);
        recomputeBranchPdcLocked(publishedOrder_);
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }
    bool branchPdc() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return branchPdc_.load(std::memory_order_relaxed);
    }

    // ---- Spillover (ABI v37) ------------------------------------------------

    /// Whether a delay/reverb that a structure change REMOVES rings out as a
    /// post-chain tail (see SpillTail). OFF at engine start. Turning it off asks
    /// every ringing tail to fade out over kFadeSeconds rather than cutting it.
    /// Message thread.
    void setSpillover(bool on)
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (spillover_ == on) return;
        spillover_ = on;
        if (! on)
            for (auto& s : spills_)
                if (SpillTail* t = tailAtLocked(s.tail)) t->fastFade.store(true, std::memory_order_release);
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool spillover() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return spillover_;
    }

    /// Tails still RINGING (not yet marked Done by the audio thread). A Done
    /// tail that the next publish has not retired yet is not counted: it is
    /// silent and skipped. Message thread.
    int spillTailCount() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        int n = 0;
        for (const auto& s : spills_)
            if (const SpillTail* t = tailAtLocked(s.tail))
                if (t->state.load(std::memory_order_acquire) == SpillTail::kRinging) ++n;
        return n;
    }

    /// Retires every tail the audio thread has marked Done: one publication
    /// without them (whose barrier is what makes the free safe), then the
    /// modules die here, on the calling thread. Every publish does this anyway;
    /// this is for a chain nobody is editing. No-op when nothing is Done.
    /// Message thread.
    void reapFinishedSpills()
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (editing_) return; // the commit publishes (and reaps) soon enough
        bool any = false;
        for (const auto& s : spills_)
            if (const SpillTail* t = tailAtLocked(s.tail))
                if (t->state.load(std::memory_order_acquire) == SpillTail::kDone) { any = true; break; }
        if (any) publishListLocked(idOrder_);
    }

    /// Tails held by the chain (ringing or awaiting retirement). Test hook.
    int spillSlotCountForTest() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return (int) spills_.size();
    }

    // Total latency of the chain as it is currently ordered, in samples.
    //
    // Inside a split/parallel region this is the MAXIMUM over the enabled
    // branches, not the sum: the branches run in parallel, so the region is as
    // late as its latest branch. Since v34 that is also what the region
    // actually RUNS at — the merger delays every branch up to the max, so the
    // number describes an ALIGNED region rather than merely the latest of
    // several misaligned ones. With branch PDC switched off it is the old
    // figure and the old caveat: the branches disagree by up to this much.
    // Bypassed nodes contribute nothing. Message thread.
    //
    // From the PUBLISHED order, resolved through slotForPublishLocked — the same
    // rule recomputeBranchPdcLocked states in bold and every one of its five call
    // sites obeys. This walk did NOT until 2026-09-11: it walked `idOrder_`, the
    // SHADOW chain the moment an edit scope is open, so inside a scope a node the
    // scope had removed was still compensated for by the mergers and no longer
    // counted here, and a node it had added did the reverse. The two are now one
    // walk (walkRegionsLocked), which is the only way a divergence like that
    // stops happening.
    int chainLatencySamples() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return walkRegionsLocked(publishedOrder_, [](MergerModule*, const int*, int) {});
    }

    // The rate the chain's modules were last prepared for (0 = never prepared).
    double preparedSampleRate() const
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        return prepared_ ? sampleRate_ : 0.0;
    }

    // ---- Audio thread ------------------------------------------------------

    // Process the buffer in place through the current chain order, skipping
    // bypassed nodes. RT-safe (atomic snapshot load; no alloc/lock).
    /// `songMix` (ABI v34) is the engine's current output block — the song plus
    /// whatever has already been mixed into it — handed in READ-ONLY for the
    /// duration of this call so a node keyed to kSidechainSongMix can duck the
    /// rig under the backing track. Null (the offline and test paths, and any
    /// caller that predates v34) means that source is UNAVAILABLE, and a node
    /// asking for it falls back to its own input rather than to silence.
    void process(float* const* channels, int numChannels, int numFrames)
    {
        process(channels, numChannels, numFrames, nullptr, 0);
    }

    void process(float* const* channels, int numChannels, int numFrames,
                 const float* const* songMix, int songChannels)
    {
        // Lock FIRST, then load — the order is mandatory. remove/clear publish
        // the new snapshot and then take this same mutex as their barrier
        // before destroying the retired module; loading the snapshot before we
        // hold the mutex leaves a window where the barrier passes and the
        // module a stale snapshot references is destroyed mid-block
        // (use-after-free). Same order as AudioEngine::processInputChain.
        DCA_RT_LOCK_GUARD(processMutex_, "dspChain/processMutex");
        auto snap = std::atomic_load_explicit(&order_, std::memory_order_acquire);
        if (!snap) return;
        // Heartbeat for the message thread's fade-out wait: if this stops
        // advancing, nobody is going to finish the fade and the wait gives up.
        rtBlocks_.fetch_add(1, std::memory_order_relaxed);

        // ---- branch PDC: SAMPLE-AND-CLEAR AT THE TOP (W5-E2) ---------------
        //
        // The re-derive itself runs at the END of this block, after the modules
        // have adopted their mailboxes — see the comment there. But the FLAG
        // has to be consumed HERE, before the first module adopts, or the fix
        // has a one-callback hole: a setParam landing after a module's adopt
        // and before an end-of-block exchange is consumed by a block whose
        // modules still ran the OLD latency, the recompute programs the mergers
        // from that old value, the flag is cleared, and nothing comes back —
        // the region stays misaligned "until an unrelated publish", which is
        // the exact wording of the bug W4-D54 was written for. A UI drag at
        // 5-10 ms blocks lands in that window as a matter of course.
        //
        // Reading it at the top is what makes the release/acquire pair carry
        // the guarantee the code wants: a flag observed HERE means the mailbox
        // write happened-before, so every module adopts it in THIS block and
        // the walk below sees the new latencies. A write that lands after this
        // point leaves the flag set for the NEXT block, which is correct.
        const bool redoBranchPdc = pdcDirty_.exchange(false, std::memory_order_acq_rel);

        // Sanitize the chain INPUT so a non-finite sample can never enter the
        // first stateful stage.
        scrubNonFinite(channels, numChannels, numFrames);

        // Split/parallel state (ABI v23). Branch buffers are only usable when
        // the block fits the prepared scratch; otherwise splits degrade to the
        // plain linear chain (safe fallback, never out-of-bounds).
        const bool branchesReady = !branchPtrs_.empty()
                                   && numChannels <= numChannels_
                                   && numFrames <= maxBlock_;
        int activeBranches = 0; // 0 = no split region open

        // Sidechain key sources (v34). The seat's DRY input is this block as it
        // arrived, so it has to be copied BEFORE the first module runs — but
        // only when some node actually asked for it, which is a flag the
        // message thread publishes rather than a scan of the snapshot.
        const bool wantsDry = rtSidechainDry_.load(std::memory_order_relaxed)
                              && ! scDryPtrs_.empty()
                              && numChannels <= numChannels_
                              && numFrames <= maxBlock_;
        if (wantsDry)
            for (int c = 0; c < numChannels; ++c)
                if (channels[c] != nullptr)
                    std::memcpy(scDryPtrs_[(size_t) c], channels[c],
                                (size_t) numFrames * sizeof(float));

        // The song-mix key is the engine's OUTPUT block, handed in read-only.
        // It is untrusted — see scSongData_'s note — and the chain cannot scrub
        // it in place, so it is copied into prepared scratch and the COPY is
        // scrubbed. `songKeyCh` is clamped to what was prepared; a block that
        // does not fit leaves `songReady` false and the keyed node falls back
        // to its own input, exactly as an unavailable source does. Never to
        // silence: keyed from silence a gate sits CLOSED.
        const int songKeyCh = songChannels < numChannels_ ? songChannels : numChannels_;
        const bool songReady = rtSidechainSong_.load(std::memory_order_relaxed)
                               && songMix != nullptr
                               && songKeyCh > 0
                               && ! scSongPtrs_.empty()
                               && numFrames <= maxBlock_;
        if (songReady)
        {
            for (int c = 0; c < songKeyCh; ++c)
                if (songMix[c] != nullptr)
                    std::memcpy(scSongPtrs_[(size_t) c], songMix[c],
                                (size_t) numFrames * sizeof(float));
                else
                    std::memset(scSongPtrs_[(size_t) c], 0, (size_t) numFrames * sizeof(float));
            scrubNonFinite(scSongPtrs_.data(), songKeyCh, numFrames);
        }

        // Crossfade scratch is usable under the same block-fits-prepare rule;
        // otherwise a change snaps, exactly as before this existed.
        const bool  fadeReady = !dryPtrs_.empty()
                                && numChannels <= numChannels_
                                && numFrames <= maxBlock_;
        const float fadeInc   = rtFadeInc_.load(std::memory_order_relaxed);

        // Per-node metering (v30). Coefficients once per block, never per node:
        // a one-pole `k = dt/tau`, clamped — the first-order form of
        // 1-exp(-dt/tau), which keeps a transcendental off the callback for a
        // number that only has to look right on a 30 Hz meter.
        const bool  meter  = metering_.load(std::memory_order_relaxed);
        const float invSr  = rtInvSampleRate_.load(std::memory_order_relaxed);
        const float dt     = (float) numFrames * invSr;
        const float kPeak  = clamp01(dt / kMeterPeakTau);
        const float kRms   = clamp01(dt / kMeterRmsTau);
        // Carries the last measured buffer forward: in a LINEAR chain node k's
        // output is node k+1's input, so the input measure is free for every
        // node but the first.
        const float* measuredBuf = nullptr;
        float        measuredPeak = 0.0f, measuredMs = 0.0f;
        // Idle sleep: a crossfade only advances in here, so a seat must not go
        // to sleep with one mid-ramp (see rtMayRest).
        bool anyFading = false;

        for (const auto& n : *snap)
        {
            if (n.module == nullptr) continue;
            // A post-chain spill tail (v37) is not part of the walk; it runs
            // after it, below.
            if (n.spill != nullptr) continue;
            const bool routing = n.splitter != nullptr || n.merger != nullptr;
            if (n.bypassed && routing)
            {
                // A bypassed routing node switches instantly (topology, not
                // level); its meter must still fall to zero rather than freeze
                // at whatever it read when the user switched it off.
                if (meter && n.meter != nullptr) n.meter->decay(kPeak, kRms);
                continue;
            }

            if (n.splitter != nullptr)
            {
                // A splitter passes the signal through unchanged, so its two
                // meters are the same measurement — but they still have to be
                // WRITTEN, or the routing node is the one card on the board
                // whose lights never move.
                if (meter && n.meter != nullptr)
                {
                    const float* head = firstBuf(channels, numChannels);
                    float p = measuredPeak, ms = measuredMs;
                    if (head == nullptr || head != measuredBuf)
                        measureBlock(channels, numChannels, numFrames, p, ms);
                    n.meter->stepIn(p, ms, kPeak, kRms);
                    n.meter->stepOut(p, ms, kPeak, kRms);
                    measuredBuf = head; measuredPeak = p; measuredMs = ms;
                }
                // Open a region (nested splitters inside an open one are
                // wires — their process() is a no-op anyway).
                if (activeBranches == 0 && branchesReady)
                {
                    activeBranches = n.splitter->branchCountRt();
                    for (int b = 0; b < activeBranches; ++b)
                        for (int c = 0; c < numChannels; ++c)
                            if (channels[c] != nullptr)
                                std::memcpy(branchPtrs_[(size_t) b][(size_t) c], channels[c],
                                            (size_t) numFrames * sizeof(float));
                }
                continue;
            }

            if (n.merger != nullptr)
            {
                if (activeBranches > 0)
                {
                    // The merger's "input" is the branch set it is about to
                    // fold; metering the branches individually would need three
                    // more passes, so the honest cheap answer is the LOUDEST
                    // branch — which is what a clip on this node would come
                    // from.
                    if (meter && n.meter != nullptr)
                    {
                        float p = 0.0f, ms = 0.0f;
                        for (int b = 0; b < activeBranches; ++b)
                        {
                            float bp = 0.0f, bms = 0.0f;
                            measureBlock(branchPtrs_[(size_t) b].data(), numChannels, numFrames, bp, bms);
                            if (bp > p) p = bp;
                            if (bms > ms) ms = bms;
                        }
                        n.meter->stepIn(p, ms, kPeak, kRms);
                    }
                    n.merger->mergeRt(channels, branchPtrs_, activeBranches,
                                      numChannels, numFrames);
                    scrubNonFinite(channels, numChannels, numFrames);
                    activeBranches = 0;
                    measuredBuf = nullptr;
                    if (meter && n.meter != nullptr)
                    {
                        float p = 0.0f, ms = 0.0f;
                        measureBlock(channels, numChannels, numFrames, p, ms);
                        n.meter->stepOut(p, ms, kPeak, kRms);
                        measuredBuf = firstBuf(channels, numChannels);
                        measuredPeak = p; measuredMs = ms;
                    }
                }
                // No open region: the merger is a wire.
                continue;
            }

            // Regular node: its wet weight ramps toward 1 (in the path) or 0
            // (bypassed). Only opt-in tail processors run while parked at zero.
            const float wetTarget = n.bypassed ? 0.0f : 1.0f;
            float       wet       = n.fade != nullptr ? n.fade->wet.load(std::memory_order_relaxed) : wetTarget;
            bool        fading    = wet != wetTarget;
            if (fading && ! fadeReady)
            {
                // No scratch for this block shape: snap, as the chain did
                // before crossfades existed.
                wet = wetTarget;
                fading = false;
                if (n.fade != nullptr) n.fade->wet.store(wet, std::memory_order_relaxed);
            }
            const bool customBypass = n.module->ownsBypassProcessing();
            anyFading |= fading || n.spilling;
            if (! n.spilling && ! fading && n.bypassed && !customBypass)
            {
                if (meter && n.meter != nullptr) n.meter->decay(kPeak, kRms);
                continue;
            }

            // Inside a region it processes its tagged branch's buffer,
            // otherwise the main buffer.
            float* const* target = channels;
            if (activeBranches > 0)
            {
                int b = n.branch <= 0 ? 1 : n.branch; // 0 = branch 1
                if (b > activeBranches) b = activeBranches;
                target = branchPtrs_[(size_t) (b - 1)].data();
            }
            if (meter && n.meter != nullptr)
            {
                const float* head = firstBuf(target, numChannels);
                if (head != nullptr && head == measuredBuf)
                {
                    n.meter->stepIn(measuredPeak, measuredMs, kPeak, kRms);
                }
                else
                {
                    float p = 0.0f, ms = 0.0f;
                    measureBlock(target, numChannels, numFrames, p, ms);
                    n.meter->stepIn(p, ms, kPeak, kRms);
                }
            }
            if (n.spilling)
            {
                // v37 hand-over: the INPUT ramps to silence while the output
                // keeps ringing, and the dry path ramps back to unity — so a
                // removed delay/reverb leaves with neither its tail nor the
                // signal around it stepping. Same scrub + meter as any stage.
                spillHandOverRt(n, target, numChannels, numFrames, wet, fadeReady, fadeInc);
                scrubNonFinite(target, numChannels, numFrames);
                measuredBuf = nullptr;
                if (meter && n.meter != nullptr)
                {
                    float p = 0.0f, ms = 0.0f;
                    measureBlock(target, numChannels, numFrames, p, ms);
                    n.meter->stepOut(p, ms, kPeak, kRms);
                    measuredBuf = firstBuf(target, numChannels);
                    measuredPeak = p; measuredMs = ms;
                }
                continue;
            }
            if (fading && !customBypass)
                for (int c = 0; c < numChannels; ++c)
                    if (target[c] != nullptr)
                        std::memcpy(dryPtrs_[(size_t) c], target[c], (size_t) numFrames * sizeof(float));
            if (customBypass) {
                // `step` is 0 in steady state. It used to be -fadeInc whenever
                // `wetTarget <= wet`, i.e. a DOWNWARD ramp handed out alongside
                // an equal-or-higher target, for every block of a fully wet node
                // — harmless only because DelayCore::processBypass recomputes
                // `fading` itself, and wrong for any module that honours the
                // documented contract.
                const float step = wetTarget > wet ? fadeInc
                                                   : (wetTarget < wet ? -fadeInc : 0.0f);
                n.module->processBypass(target, numChannels, numFrames, wet, wetTarget, step);
                if (fading && n.fade != nullptr) {
                    // STEPPED, not clamp01(wet + step * numFrames). The two are
                    // the same in exact arithmetic and NOT in floating point:
                    // the accumulation rounds per step, the stored endpoint
                    // seeds the next block's ramp, and the difference compounds.
                    // Closing this form cost delay_sync_trails' bit-exactness
                    // against the legacy core at three rates. Measured, then put
                    // back — the O(numFrames) loop is the contract.
                    float end = wet;
                    for (int i = 0; i < numFrames; ++i) end = clamp01(end + step);
                    n.fade->wet.store(end, std::memory_order_relaxed);
                }
            } else if (n.sidechain != kSidechainNone) {
                // Keyed node (v34). The audio path is untouched — only the
                // module's DETECTOR reads something else. An unavailable
                // source falls back to `process`, i.e. to the module's own
                // input: never to silence, because "keyed from silence" on a
                // gate means closed and on a compressor means a feature that
                // looks live and does nothing.
                const float* const* key = nullptr;
                int keyCh = 0;
                if (n.sidechain == kSidechainSeatInput && wantsDry)
                {
                    key = scDryPtrs_.data();
                    keyCh = numChannels;
                }
                else if (n.sidechain == kSidechainSongMix && songReady)
                {
                    // The SCRUBBED copy, never the engine's buffer.
                    key = scSongPtrs_.data();
                    keyCh = songKeyCh;
                }
                if (key != nullptr) n.module->processSidechained(target, numChannels, numFrames, key, keyCh);
                else                n.module->process(target, numChannels, numFrames);
            } else n.module->process(target, numChannels, numFrames);
            if (n.fade != nullptr) n.fade->ran.store(true, std::memory_order_relaxed);
            if (fading && !customBypass)
            {
                // out = dry + (wet - dry) * w, w stepping linearly toward the
                // target; the same w per sample on every channel.
                const float step = wetTarget > wet ? fadeInc : -fadeInc;
                float wEnd = wet;
                for (int c = 0; c < numChannels; ++c)
                {
                    float* out = target[c];
                    if (out == nullptr) continue;
                    const float* dry = dryPtrs_[(size_t) c];
                    float w = wet;
                    for (int i = 0; i < numFrames; ++i)
                    {
                        w = clamp01(w + step);
                        out[i] = dry[i] + (out[i] - dry[i]) * w;
                    }
                    wEnd = w;
                }
                if (n.fade != nullptr) n.fade->wet.store(wEnd, std::memory_order_relaxed);
            }
            // ...and after every stage: a single NaN/Inf out of one module
            // would otherwise latch a downstream feedback/IIR core (delay,
            // reverb tank, graphic EQ) into PERMANENT NaN. This bounds the
            // whole chain finite regardless of any one module's edge
            // behaviour; the isfinite scan is negligible vs. the per-module
            // DSP. (Reviewed hazard for tapedelay/platereverb/graphiceq.)
            scrubNonFinite(target, numChannels, numFrames);
            // Measured AFTER the scrub, so what the meter shows is what the
            // next stage actually receives.
            measuredBuf = nullptr;
            if (meter && n.meter != nullptr)
            {
                float p = 0.0f, ms = 0.0f;
                measureBlock(target, numChannels, numFrames, p, ms);
                n.meter->stepOut(p, ms, kPeak, kRms);
                measuredBuf = firstBuf(target, numChannels);
                measuredPeak = p; measuredMs = ms;
            }
        }

        // Unmatched splitter: implicit unity average merge at chain end so a
        // half-built board still makes sound at sane levels.
        if (activeBranches > 0)
        {
            const float inv = 1.0f / (float) activeBranches;
            for (int c = 0; c < numChannels; ++c)
            {
                float* out = channels[c];
                if (out == nullptr) continue;
                for (int i = 0; i < numFrames; ++i)
                {
                    float s = 0.0f;
                    for (int b = 0; b < activeBranches; ++b)
                        s += branchPtrs_[(size_t) b][(size_t) c][i];
                    out[i] = s * inv;
                }
            }
            scrubNonFinite(channels, numChannels, numFrames);
        }

        // Spillover tails (v37), after the WHOLE chain: each runs over silence
        // and is summed into the chain output. A ringing tail keeps the seat
        // awake (it is audio the sleep logic must not cut).
        const bool tailsRinging = processSpillTailsRt(*snap, channels, numChannels, numFrames,
                                                      fadeInc, invSr);

        rtFadedLastBlock_.store(anyFading || tailsRinging, std::memory_order_relaxed);

        // ---- branch PDC, re-derived AFTER the modules adopted (W4-D54) -----
        //
        // setParam publishes into each module's ParamMailbox and the CORE only
        // adopts at the top of its next process(), but setParam also called
        // recomputeBranchPdcLocked immediately — so the mergers were programmed
        // from the latency the core had NOT yet replaced. Every adapter that
        // delegates latencySamples() to its core (whammy, vibrato, harmonizer,
        // klon) was affected; overdrive was not, because it answers from its own
        // `p_` copy, which is updated synchronously. And it was not "one block
        // early": nothing re-ran the recompute afterwards, so the region stayed
        // misaligned until an unrelated publish/prepare happened along.
        //
        // The message thread cannot fix this by waiting — the one-block barrier
        // proves no block is IN FLIGHT, not that one has RUN — so the re-derive
        // belongs here, where the adoption just happened. It is gated on a flag
        // the message thread sets, so an ordinary block does not pay for it and
        // the pre-v36 path is byte-for-byte unchanged.
        //
        // The flag was SAMPLED AND CLEARED at the top of this block (W5-E2);
        // consuming it here would leave a one-callback window in which a
        // setParam is swallowed by a block whose modules never adopted it.
        if (redoBranchPdc)
            recomputeBranchPdcRt(*snap);
    }

    /// Audio thread. The same max-minus-branch arithmetic as
    /// recomputeBranchPdcLocked, over the SNAPSHOT the callback already holds
    /// rather than over publishedOrder_ + the modules_ map — which is what makes
    /// it RT-safe: no lock, no map lookup, no allocation, just virtual calls and
    /// relaxed atomic stores into the mergers.
    ///
    /// Deliberately a second implementation of one rule, which this repo
    /// normally forbids. It is here because the two run on different threads
    /// with different things in hand, and the alternative — taking writeMutex_
    /// on the callback — is the one thing that is never allowed. They are pinned
    /// against each other by RtBranchPdcFollowsAParamOnTheBlockItChanges, which
    /// asserts the merger ends up at the value the message-thread walk would
    /// have produced.
    void recomputeBranchPdcRt(const Order& snap) noexcept
    {
        // Relaxed atomic load, never a plain read: writeMutex_ is not available
        // here and never will be (W5-E3).
        const bool pdc = branchPdc_.load(std::memory_order_relaxed);
        int branchAcc[kDspMaxBranches] = { 0 };
        int activeBranches = 0;

        for (const auto& n : snap)
        {
            DspModule* m = n.module;
            if (m == nullptr || n.spill != nullptr) continue; // a tail is outside every region

            if (n.bypassed)
            {
                if (n.merger != nullptr)
                    for (int b = 0; b < kDspMaxBranches; ++b) n.merger->setBranchDelay(b, 0);
                continue;
            }
            if (n.splitter != nullptr)
            {
                if (activeBranches == 0)
                {
                    activeBranches = n.splitter->branchCountRt();
                    for (int b = 0; b < kDspMaxBranches; ++b) branchAcc[b] = 0;
                }
                continue;   // a nested splitter is a wire (mirrors process())
            }
            if (n.merger != nullptr)
            {
                const int mx = activeBranches > 0 ? maxBranch(branchAcc, activeBranches) : 0;
                for (int b = 0; b < kDspMaxBranches; ++b)
                    n.merger->setBranchDelay(
                        b, (pdc && activeBranches > 0 && b < activeBranches) ? mx - branchAcc[b] : 0);
                activeBranches = 0;
                continue;
            }
            const int lat = std::max(0, m->latencySamples());
            if (activeBranches > 0)
            {
                int b = n.branch <= 0 ? 1 : n.branch;
                if (b > activeBranches) b = activeBranches;
                branchAcc[b - 1] += lat;
            }
        }
    }

private:
    // Replace any non-finite (NaN/Inf) sample with 0. Guards stateful cores
    // from latching permanent NaN off a single bad sample. Called between every
    // stage in process(); cheap relative to the modules' own math.
    // The implementation lives in dsp/Scrub.h because the ENGINE needs the same
    // pass after its own untrusted stages (the VST3 input chain and the sandbox
    // bridge) -- see that header.
    static void scrubNonFinite(float* const* ch, int nch, int n) noexcept
    {
        dcdsp::scrubNonFinite(ch, nch, n);
    }

    static float clamp01(float v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    static const float* firstBuf(float* const* ch, int nch) noexcept
    {
        for (int c = 0; c < nch; ++c) if (ch[c] != nullptr) return ch[c];
        return nullptr;
    }

    // Block peak and MEAN SQUARE over every channel. The sum accumulates in
    // double so a 2048-frame block cannot lose the quiet samples to the loud
    // ones; the peak is |x| so a non-finite sample (already scrubbed by the
    // caller in every real path) cannot produce a negative maximum.
    static void measureBlock(float* const* ch, int nch, int n,
                             float& peakOut, float& meanSqOut) noexcept
    {
        float  peak = 0.0f;
        double acc  = 0.0;
        int    cnt  = 0;
        for (int c = 0; c < nch; ++c)
        {
            const float* src = ch[c];
            if (src == nullptr) continue;
            for (int i = 0; i < n; ++i)
            {
                const float s = src[i];
                const float a = s < 0.0f ? -s : s;
                if (a > peak) peak = a;
                acc += (double) s * (double) s;
            }
            cnt += n;
        }
        peakOut   = peak;
        meanSqOut = cnt > 0 ? (float) (acc / (double) cnt) : 0.0f;
    }

    static void readMeter(const NodeMeter& m, float& inPeak, float& inRms,
                          float& outPeak, float& outRms) noexcept
    {
        inPeak  = m.inPeak.load(std::memory_order_relaxed);
        outPeak = m.outPeak.load(std::memory_order_relaxed);
        const float imsq = m.inMs.load(std::memory_order_relaxed);
        const float omsq = m.outMs.load(std::memory_order_relaxed);
        inRms  = imsq > 0.0f ? std::sqrt(imsq) : 0.0f;
        outRms = omsq > 0.0f ? std::sqrt(omsq) : 0.0f;
    }

    /// The catalogue's declared range for one (type, param), applied. Returns
    /// `v` unchanged for a type or an id the catalogue does not declare — poll-
    /// only ids and the routing modules' per-branch ids that no adapter exposes
    /// as a knob keep working exactly as before. Message thread; a linear scan
    /// over 57 entries on a knob tick, which is free at UI rates.
    static float clampToCatalogue(const char* typeId, const char* paramId, float v)
    {
        if (typeId == nullptr || paramId == nullptr) return v;
        for (const ModuleTypeInfo& t : moduleCatalogue())
        {
            if (t.id != typeId) continue;
            for (const ParamInfo& p : t.params)
            {
                if (p.id != paramId) continue;
                if (!(p.minValue <= p.maxValue)) return v; // malformed row: leave it alone
                return v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
            }
            return v;
        }
        return v;
    }

    static int maxBranch(const int* acc, int count) noexcept
    {
        int best = 0;
        for (int i = 0; i < count && i < kDspMaxBranches; ++i)
            if (acc[i] > best) best = acc[i];
        return best;
    }

    // ---- Spillover internals (v37) ------------------------------------------

    /// Audio thread. One block of a node being handed over to a tail IN PLACE:
    /// input = in * w, out = module(in * w) + in * (1 - w), w ramping from the
    /// node's fade weight toward 0 by `fadeInc` per sample. At w = 0 that is
    /// "the dry signal plus whatever the module still rings", so the dry path
    /// ends at unity and the tail never drops — exactly what the post-chain
    /// tail then continues. Without crossfade scratch (a block larger than
    /// prepared) the module is skipped for the block and the input passes.
    void spillHandOverRt(const NodeRef& n, float* const* target, int numChannels, int numFrames,
                         float wet, bool fadeReady, float fadeInc) noexcept
    {
        if (! fadeReady)
        {
            if (n.fade != nullptr) n.fade->wet.store(0.0f, std::memory_order_relaxed);
            return;
        }
        float wEnd = wet;
        for (int c = 0; c < numChannels; ++c)
        {
            float* x = target[c];
            if (x == nullptr) continue;
            float* dry = dryPtrs_[(size_t) c];
            std::memcpy(dry, x, (size_t) numFrames * sizeof(float));
            float w = wet;
            for (int i = 0; i < numFrames; ++i)
            {
                w = clamp01(w - fadeInc);
                x[i] = dry[i] * w;
            }
            wEnd = w;
        }
        n.module->process(target, numChannels, numFrames);
        for (int c = 0; c < numChannels; ++c)
        {
            float* x = target[c];
            if (x == nullptr) continue;
            const float* dry = dryPtrs_[(size_t) c];
            float w = wet;
            for (int i = 0; i < numFrames; ++i)
            {
                w = clamp01(w - fadeInc);
                x[i] += dry[i] * (1.0f - w);
            }
        }
        if (n.fade != nullptr)
        {
            n.fade->wet.store(wEnd, std::memory_order_relaxed);
            n.fade->ran.store(true, std::memory_order_relaxed);
        }
    }

    /// Audio thread. Runs every RINGING tail in the snapshot over silence and
    /// sums it into the chain output. Returns whether any tail rang this block.
    /// Bounded: at most the tails the message thread published (a small fixed
    /// number in practice — kMaxSpillTails plus any fading out), no allocation,
    /// no lock beyond the processMutex_ the caller already holds.
    bool processSpillTailsRt(const Order& snap, float* const* channels, int numChannels,
                             int numFrames, float fadeInc, float invSr) noexcept
    {
        const bool ready = ! spillPtrs_.empty()
                           && numChannels <= numChannels_
                           && numFrames <= maxBlock_;
        if (! ready) return false;
        const float     sr         = invSr > 0.0f ? 1.0f / invSr : 44100.0f;
        const long long maxSamples = (long long) (kSpillMaxSeconds * sr);
        const long long quietMax   = (long long) (kSpillQuietSeconds * sr);
        bool any = false;
        for (const auto& n : snap)
        {
            if (n.spill == nullptr || n.module == nullptr) continue;
            SpillTail& t = *n.spill;
            if (t.state.load(std::memory_order_acquire) != SpillTail::kRinging) continue;
            any = true;
            for (int c = 0; c < numChannels; ++c)
                std::memset(spillPtrs_[(size_t) c], 0, (size_t) numFrames * sizeof(float));
            n.module->process(spillPtrs_.data(), numChannels, numFrames);
            scrubNonFinite(spillPtrs_.data(), numChannels, numFrames);
            // Out of time, or asked to make room / stop: ramp out, then Done.
            const bool fading = t.fastFade.load(std::memory_order_acquire) || t.rendered >= maxSamples;
            float peak = 0.0f;
            float gEnd = t.gain;
            for (int c = 0; c < numChannels; ++c)
            {
                float* out = channels[c];
                if (out == nullptr) continue;
                const float* s = spillPtrs_[(size_t) c];
                float g = t.gain;
                for (int i = 0; i < numFrames; ++i)
                {
                    if (fading) g = g > fadeInc ? g - fadeInc : 0.0f;
                    const float v = s[i] * g;
                    out[i] += v;
                    const float a = v < 0.0f ? -v : v;
                    if (a > peak) peak = a;
                }
                gEnd = g;
            }
            t.gain      = gEnd;
            t.rendered += numFrames;
            t.quiet     = peak < kSpillQuietLevel ? t.quiet + numFrames : 0;
            if ((fading && t.gain <= 0.0f) || t.quiet >= quietMax)
                t.state.store(SpillTail::kDone, std::memory_order_release);
        }
        return any;
    }

    /// The tail-producing module types: every catalogue entry whose category is
    /// `delay` or `reverb` (GearEntry.h's fixed set) — the only two families
    /// whose output outlives their input by more than a few milliseconds.
    /// Message thread; a linear scan over the catalogue once per remove.
    static bool isSpillType(const char* typeId)
    {
        if (typeId == nullptr) return false;
        for (const ModuleTypeInfo& t : moduleCatalogue())
            if (t.id == typeId) return t.category == "delay" || t.category == "reverb";
        return false;
    }

    /// Spillover is on, the chain is prepared and the node is a tail type.
    /// writeMutex_ held.
    bool spillTypeEligibleLocked(const Slot& slot) const
    {
        return spillover_ && prepared_ && slot.module != nullptr && isSpillType(slot.module->typeId());
    }

    /// ...and it is AUDIBLE right now: in the path, has run, not faded out.
    /// A bypassed or never-run node has no tail to keep. writeMutex_ held.
    bool spillEligibleLocked(const Slot& slot) const
    {
        if (slot.bypassed || ! spillTypeEligibleLocked(slot)) return false;
        const NodeFade* f = fadeAtLocked(slot.fade);
        return f != nullptr && f->ran.load(std::memory_order_relaxed)
               && f->wet.load(std::memory_order_relaxed) > 0.0f;
    }

    /// Moves `slot` into spills_ as a RINGING tail (writeMutex_ held; the
    /// caller publishes). The source keeps no pool index, so a release loop
    /// over it is a no-op. Past kMaxSpillTails the OLDEST ringing tail is asked
    /// to fade out; a tail armed while spillover has just been switched off
    /// fades out at once.
    void armSpillLocked(Slot& slot)
    {
        int ringing = 0;
        for (auto& s : spills_)
            if (SpillTail* t = tailAtLocked(s.tail))
                if (t->state.load(std::memory_order_acquire) == SpillTail::kRinging
                    && ! t->fastFade.load(std::memory_order_relaxed)) ++ringing;
        while (ringing >= kMaxSpillTails)
        {
            SpillTail* oldest = nullptr;
            for (auto& s : spills_)
                if (SpillTail* t = tailAtLocked(s.tail))
                    if (t->state.load(std::memory_order_acquire) == SpillTail::kRinging
                        && ! t->fastFade.load(std::memory_order_relaxed)
                        && (oldest == nullptr || t->armSeq < oldest->armSeq)) oldest = t;
            if (oldest == nullptr) break;
            oldest->fastFade.store(true, std::memory_order_release);
            --ringing;
        }

        const int ti = acquireTailLocked();
        SpillTail* t = tailAtLocked(ti);
        t->gain     = 1.0f;
        t->rendered = 0;
        t->quiet    = 0;
        t->armSeq   = ++spillArmSeq_;
        t->fastFade.store(! spillover_, std::memory_order_relaxed);
        t->state.store(SpillTail::kRinging, std::memory_order_release);

        SpillSlot entry;
        entry.slot = std::move(slot);
        entry.slot.spilling = false; // a tail now, not a hand-over
        entry.tail = ti;
        spills_.push_back(std::move(entry));
        slot.meter = -1;
        slot.fade  = -1;
        slot.spilling = false;
    }

    int acquireTailLocked()
    {
        if (! freeTails_.empty())
        {
            const int idx = freeTails_.back();
            freeTails_.pop_back();
            return idx;
        }
        tailPool_.push_back(std::unique_ptr<SpillTail>(new SpillTail()));
        return (int) tailPool_.size() - 1;
    }

    void releaseTailLocked(int idx)
    {
        if (idx < 0 || (size_t) idx >= tailPool_.size()) return;
        tailPool_[(size_t) idx]->state.store(SpillTail::kIdle, std::memory_order_relaxed);
        freeTails_.push_back(idx);
    }

    SpillTail* tailAtLocked(int idx) const
    {
        if (idx < 0 || (size_t) idx >= tailPool_.size()) return nullptr;
        return tailPool_[(size_t) idx].get();
    }

    // ---- Fade pool (message thread; writeMutex_ held) ----------------------
    // Same shape as the meter pool, for the same reason: grow-only, so the raw
    // pointer baked into a stale snapshot can never dangle.
    int acquireFadeLocked(float initialWet)
    {
        int idx;
        if (!freeFades_.empty())
        {
            idx = freeFades_.back();
            freeFades_.pop_back();
        }
        else
        {
            fadePool_.push_back(std::unique_ptr<NodeFade>(new NodeFade()));
            idx = (int) fadePool_.size() - 1;
        }
        if (idx >= 0 && (size_t) idx < fadePool_.size() && fadePool_[(size_t) idx])
        {
            fadePool_[(size_t) idx]->wet.store(initialWet, std::memory_order_relaxed);
            fadePool_[(size_t) idx]->ran.store(false, std::memory_order_relaxed);
        }
        return idx;
    }

    void releaseFadeLocked(int idx)
    {
        if (idx < 0 || (size_t) idx >= fadePool_.size()) return;
        freeFades_.push_back(idx);
    }

    void releaseFade(int idx)
    {
        if (idx < 0) return;
        std::lock_guard<std::mutex> lock(writeMutex_);
        releaseFadeLocked(idx);
    }

    NodeFade* fadeAtLocked(int idx) const
    {
        if (idx < 0 || (size_t) idx >= fadePool_.size()) return nullptr;
        return fadePool_[(size_t) idx].get();
    }

    // Generic modules reset here; opt-in tail processors receive a callback-owned
    // reset request because they may still be advancing at zero wet weight.
    // A generic node whose fade has landed at 0 is skipped by process() -- `continue` -- so its delay
    // lines, reverb tanks, followers and LFO phases are FROZEN at the instant the
    // fade landed. Not decaying: frozen. Fading that back in replays audio from
    // before the bypass, which is neither thing a pedal does (true bypass kills
    // the tail, trails let it decay away), so an un-bypass that follows a
    // COMPLETED fade-out clears the module first.
    //
    // It is safe precisely BECAUSE the fade has landed. `wet` is stored at the
    // end of the block in which the module was last processed (see process()),
    // so once the message thread observes 0 the audio thread is already past it
    // and every later block skips it; the node stays marked bypassed in the live
    // snapshot until publishLocked() below.
    //
    // A quick toggle -- bypass and un-bypass inside the 10 ms ramp -- is not a
    // park and keeps its state, which is also the musical answer.
    //
    // MOVE and REMOVE do not come through here: fadeOutAndWait/fadeIn drive the
    // flag directly, so "reordering a delay never clears its buffer" (top of this
    // file) still holds.
    void resetIfParkedLocked(Slot& slot)
    {
        NodeFade* f = fadeAtLocked(slot.fade);
        if (f == nullptr) return;
        if (f->wet.load(std::memory_order_relaxed) != 0.0f) return;
        if (slot.module->ownsBypassProcessing()) slot.module->requestBypassReset();
        else slot.module->reset();
    }

    // Marks the node bypassed, publishes, and waits until the audio thread has
    // ramped its weight to 0 — or until it is clear nobody is processing (the
    // block heartbeat stalls for two blocks), so a chain with no device open
    // costs a few milliseconds here, not a hang. Returns false for an unknown
    // node. With onlyIfLive, a node that is already bypassed returns false
    // WITHOUT touching it (the caller uses the answer to restore it).
    //
    // Message thread only; never holds writeMutex_ while waiting. The wait is
    // bounded (~kFadeSeconds + three blocks) and sleeps in 1 ms steps.
    bool fadeOutAndWait(int nodeId, bool onlyIfLive = false)
    {
        NodeFade* fade = nullptr;
        double blockMs = 0.0;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            auto it = modules_.find(nodeId);
            if (it == modules_.end()) return false;
            // Already bypassed: for a move there is nothing to do (and the
            // caller must not "restore" it); for a remove, still wait for a
            // ramp that may be mid-flight from a footswitch a moment ago.
            const bool alreadyBypassed = it->second.bypassed;
            if (alreadyBypassed && onlyIfLive) return false;
            const bool routing = it->second.module != nullptr
                                 && (std::strcmp(it->second.module->typeId(), "splitter") == 0
                                     || std::strcmp(it->second.module->typeId(), "merger") == 0);
            if (routing) return true; // switches instantly; nothing to wait for
            if (! alreadyBypassed)
            {
                it->second.bypassed = true;
                snapIfNeverRanLocked(it->second);
                publishLocked();
            }
            fade = fadeAtLocked(it->second.fade);
            // The DEVICE block, not the sizing block — see prepare().
            blockMs = 1000.0 * deviceBlock_ / sampleRate_;
        }
        if (fade == nullptr) return true;

        // A node that has NEVER RUN is already parked at 0 by
        // snapIfNeverRanLocked above, so the loop below exits at once — the
        // expensive case is a node that HAS run and whose chain has since gone
        // quiet (an offline render that finished, a stopped transport), where
        // the wait runs to `stallMax`. That is why `stallMax` has to come off
        // the DEVICE block: derived from the sizing block it was ~85 ms per
        // remove / move / retag.

        using clock = std::chrono::steady_clock;
        const auto start    = clock::now();
        const auto budget   = std::chrono::milliseconds((int) (kFadeSeconds * 1000.0f + 3.0 * blockMs + 5.0));
        const auto stallMax = std::chrono::milliseconds(std::max(4, (int) (2.0 * blockMs)));
        unsigned lastBlocks = rtBlocks_.load(std::memory_order_relaxed);
        auto lastAdvance = start;
        while (fade->wet.load(std::memory_order_relaxed) > 0.0f)
        {
            const auto now = clock::now();
            if (now - start > budget) break;
            const unsigned b = rtBlocks_.load(std::memory_order_relaxed);
            if (b != lastBlocks) { lastBlocks = b; lastAdvance = now; }
            else if (now - lastAdvance > stallMax) break; // nobody is processing
            fadeWaitSleeps_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        // Whether the ramp landed or the wait gave up, the node leaves the
        // path at 0 from here on: a snap is the pre-crossfade behaviour. On the
        // heartbeat (`stallMax`) branch nothing was processing, so nothing hears
        // it. On the `budget` branch something IS processing — just slower than
        // real time — and this store then races the audio thread's end-of-block
        // `wet.store(wEnd)`. Either value is a legal weight and both threads use
        // relaxed atomics, so the outcome is a one-block-late fade-out rather
        // than a fault; it is a bounded audible splice on an already-overloaded
        // callback, not the "nobody could hear it" this comment used to claim.
        fade->wet.store(0.0f, std::memory_order_relaxed);
        return true;
    }

    // A node whose module never ran has contributed nothing, so there is
    // nothing to fade out of: park it at 0 right away (writeMutex_ held). The
    // store can race a callback that is about to run the module for the first
    // time; the worst case is that the callback's end-of-block store wins and
    // the node fades out normally from there — the pre-snap behaviour.
    void snapIfNeverRanLocked(const Slot& slot)
    {
        NodeFade* f = fadeAtLocked(slot.fade);
        if (f != nullptr && ! f->ran.load(std::memory_order_relaxed))
            f->wet.store(0.0f, std::memory_order_relaxed);
    }

    // Undoes fadeOutAndWait's temporary bypass (writeMutex_ held). The fade-in
    // then happens on the audio thread from the 0 the fade-out left behind.
    //
    // ...UNLESS the user moved the footswitch while the move/retag was fading.
    // This used to store `bypassed = false` unconditionally off a `wasLive`
    // sampled BEFORE a ~10 ms unlocked wait, so a setBypass(id, true) landing in
    // that window returned true, stored its flag, and was then silently un-done:
    // the UI read the pedal as bypassed and it was audible. `bypassSeq` is
    // bumped by every setBypass, so "did the user touch this meanwhile" is
    // answerable, and the user's intent wins.
    void restoreLiveIfUntouchedLocked(int nodeId, unsigned long seq)
    {
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return;
        if (it->second.bypassSeq != seq) return;
        it->second.bypassed = false;
    }

    // ---- Meter pool (message thread; writeMutex_ held) ---------------------
    // Indices, not owning pointers, so a Slot stays copyable and the audio
    // thread only ever sees the resolved raw pointer baked into the snapshot.
    // The pool only GROWS: a NodeMeter is never destroyed while the chain
    // lives, which is what makes a stale snapshot's meter pointer harmless.
    int acquireMeterLocked()
    {
        if (!freeMeters_.empty())
        {
            const int idx = freeMeters_.back();
            freeMeters_.pop_back();
            if (idx >= 0 && (size_t) idx < meterPool_.size() && meterPool_[(size_t) idx])
                meterPool_[(size_t) idx]->clear();
            return idx;
        }
        meterPool_.push_back(std::unique_ptr<NodeMeter>(new NodeMeter()));
        return (int) meterPool_.size() - 1;
    }

    void releaseMeterLocked(int idx)
    {
        if (idx < 0 || (size_t) idx >= meterPool_.size()) return;
        if (meterPool_[(size_t) idx]) meterPool_[(size_t) idx]->clear();
        freeMeters_.push_back(idx);
    }

    void releaseMeter(int idx)
    {
        if (idx < 0) return;
        std::lock_guard<std::mutex> lock(writeMutex_);
        releaseMeterLocked(idx);
    }

    const NodeMeter* meterAtLocked(int idx) const
    {
        if (idx < 0 || (size_t) idx >= meterPool_.size()) return nullptr;
        return meterPool_[(size_t) idx].get();
    }

    // ---- Edit scope internals (writeMutex_ held) ---------------------------

    /// A remove inside an edit scope. A node the SAME scope added was never
    /// published, so it is retired outright; anything older is parked in
    /// editRemoved_ so the commit can fade it out and retire it.
    ///
    /// `doomed` collects slots to DESTROY, and the caller must let it die
    /// outside writeMutex_ — the same rule beginEdit and abortEditLocked follow,
    /// which this one function used to do the opposite of by running a module
    /// destructor (a NeuralAudio teardown, a file-handle close) under the lock.
    bool removeNodeEditingLocked(int nodeId, std::vector<Slot>& doomed)
    {
        auto it = modules_.find(nodeId);
        if (it == modules_.end()) return false;
        auto added = std::find(editAdded_.begin(), editAdded_.end(), nodeId);
        // "Never published, so nothing to barrier against" is TRUE only while no
        // commit is in flight. commitEdit publishes editAdded_ nodes in its
        // TRANSITIONAL order and then drops writeMutex_ for waitForFades with
        // `editing_` still true — so a mutator landing in that window destroyed a
        // PUBLISHED module and recycled its NodeFade/NodeMeter slot into the free
        // lists, after which the next addNode was handed the very NodeFade* a
        // live NodeRef still pointed at. While `committing_`, park it instead:
        // the commit's final publishListLocked barriers before it retires
        // editRemoved_, which is exactly the guarantee this needs.
        if (added != editAdded_.end() && ! committing_)
        {
            editAdded_.erase(added);
            releaseMeterLocked(it->second.meter);
            releaseFadeLocked(it->second.fade);
            doomed.push_back(std::move(it->second));
            modules_.erase(it);
            eraseId(nodeId);
            return true;
        }
        if (added != editAdded_.end()) editAdded_.erase(added);
        editRemoved_.emplace_back(nodeId, std::move(it->second));
        modules_.erase(it);
        eraseId(nodeId);
        return true;
    }

    bool reorderLocked(int nodeId, int newIndex)
    {
        auto pos = std::find(idOrder_.begin(), idOrder_.end(), nodeId);
        if (pos == idOrder_.end()) return false;
        idOrder_.erase(pos);
        if (newIndex < 0) newIndex = 0;
        if (newIndex > (int) idOrder_.size()) newIndex = (int) idOrder_.size();
        idOrder_.insert(idOrder_.begin() + newIndex, nodeId);
        return true;
    }

    /// Wait for a SET of fade-outs at once — the commit's one fade window. Same
    /// bound and the same heartbeat give-up as fadeOutAndWait, so a chain
    /// nothing is processing costs milliseconds rather than hanging, and the
    /// wait does not grow with the number of departing nodes.
    void waitForFades(const std::vector<NodeFade*>& fades)
    {
        if (fades.empty()) return;
        double blockMs = 0.0;
        {
            std::lock_guard<std::mutex> lock(writeMutex_);
            blockMs = 1000.0 * deviceBlock_ / sampleRate_;
        }
        using clock = std::chrono::steady_clock;
        const auto start    = clock::now();
        const auto budget   = std::chrono::milliseconds((int) (kFadeSeconds * 1000.0f + 3.0 * blockMs + 5.0));
        const auto stallMax = std::chrono::milliseconds(std::max(4, (int) (2.0 * blockMs)));
        unsigned lastBlocks = rtBlocks_.load(std::memory_order_relaxed);
        auto lastAdvance = start;
        for (;;)
        {
            bool any = false;
            for (NodeFade* f : fades)
                if (f != nullptr && f->wet.load(std::memory_order_relaxed) > 0.0f) { any = true; break; }
            if (! any) break;
            const auto now = clock::now();
            if (now - start > budget) break;
            const unsigned b = rtBlocks_.load(std::memory_order_relaxed);
            if (b != lastBlocks) { lastBlocks = b; lastAdvance = now; }
            else if (now - lastAdvance > stallMax) break; // nobody is processing
            fadeWaitSleeps_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (NodeFade* f : fades)
            if (f != nullptr) f->wet.store(0.0f, std::memory_order_relaxed);
    }

    /// Re-derive every merger's per-branch compensation delays from the CURRENT
    /// shadow order (ABI v34). Message thread, writeMutex_ held.
    ///
    /// The walk is the same one chainLatencySamples() does — which is the
    /// point: the branch totals it computed were thrown away, and the merger
    /// summed branches that disagreed by exactly those numbers. Each branch is
    /// delayed by (max - its own total) so they all arrive at the merger
    /// aligned. Re-run on every publish (structure) AND on every accepted
    /// param write, because a param can change a module's latency (whammy's
    /// lowLatency, overdrive's model, vibrato's depth).
    ///
    /// A merger that closes no region is zeroed — it is a wire, and leaving a
    /// stale delay on it would make it one with a delay line.
    /// ALWAYS walk the PUBLISHED order, never the shadow. `idOrder_` is the
    /// shadow chain the moment an edit scope is open, and the merger delays
    /// this writes are read by the audio thread on the very next block — so
    /// deriving them from the shadow re-aligns a region that is still playing
    /// the PRE-edit chain, which is exactly the audibility a scope exists to
    /// prevent. Every caller passes `publishedOrder_`; publishListLocked
    /// assigns that first, so a publication aligns the order it just published
    /// and a param/prepare in between aligns the one still playing. Slots are
    /// resolved through slotForPublishLocked so a node the scope REMOVED —
    /// still in the published order, still contributing latency — still counts.
    void recomputeBranchPdcLocked(const std::vector<int>& ids)
    {
        const bool pdc = branchPdc_.load(std::memory_order_relaxed);
        // An UNMATCHED splitter auto-merges at chain end inside process()
        // itself, not through a MergerModule, so the total walkRegionsLocked
        // returns for it has nothing to compensate — documented, not forgotten.
        (void) walkRegionsLocked(ids, [pdc](MergerModule* mg, const int* acc, int active)
        {
            const int mx = active > 0 ? maxBranch(acc, active) : 0;
            for (int b = 0; b < kDspMaxBranches; ++b)
                mg->setBranchDelay(b, (pdc && active > 0 && b < active) ? mx - acc[b] : 0);
        });
    }

    /// The ONE region walk. Returns the chain's total latency in samples —
    /// serial nodes summed, a split region counted as the MAX over its enabled
    /// branches — and calls `sink(merger, branchAcc, activeBranches)` at every
    /// merger it passes, with `activeBranches == 0` for a merger that closes no
    /// region (a wire, or one that is bypassed: both must be ZEROED, or a stale
    /// alignment comes back when it is re-enabled).
    ///
    /// It exists because chainLatencySamples() and recomputeBranchPdcLocked were
    /// two hand-maintained copies of these ~35 lines that had already drifted
    /// apart on BOTH axes — which order they walked and how they resolved a slot.
    /// Callers pass `publishedOrder_`, never `idOrder_`: see
    /// recomputeBranchPdcLocked. writeMutex_ held.
    template <class Sink>
    int walkRegionsLocked(const std::vector<int>& ids, Sink&& sink) const
    {
        int total = 0;
        int branchAcc[kDspMaxBranches] = { 0, 0, 0 };
        int activeBranches = 0;
        for (int id : ids)
        {
            const Slot* slot = slotForPublishLocked(id);
            if (slot == nullptr || slot->module == nullptr) continue;
            DspModule* m = slot->module.get();
            const bool isSplit  = std::strcmp(m->typeId(), "splitter") == 0;
            const bool isMerger = std::strcmp(m->typeId(), "merger") == 0;
            if (slot->bypassed)
            {
                if (isMerger) sink(static_cast<MergerModule*>(m), branchAcc, 0);
                continue;
            }
            if (isSplit)
            {
                if (activeBranches == 0)
                {
                    activeBranches = static_cast<SplitterModule*>(m)->branchCountRt();
                    for (int b = 0; b < kDspMaxBranches; ++b) branchAcc[b] = 0;
                }
                continue; // a nested splitter is a wire (mirrors process())
            }
            if (isMerger)
            {
                sink(static_cast<MergerModule*>(m), branchAcc, activeBranches);
                if (activeBranches > 0)
                {
                    total += maxBranch(branchAcc, activeBranches);
                    activeBranches = 0;
                }
                continue;
            }
            const int lat = std::max(0, m->latencySamples());
            if (activeBranches > 0)
            {
                int b = slot->branch <= 0 ? 1 : slot->branch;
                if (b > activeBranches) b = activeBranches;
                branchAcc[b - 1] += lat;
            }
            else
            {
                total += lat;
            }
        }
        // Unmatched splitter: the chain auto-merges at the end, so the region is
        // still a region.
        if (activeBranches > 0) total += maxBranch(branchAcc, activeBranches);
        return total;
    }

    /// A slot by id, whether it is live or parked for retirement by an open
    /// edit scope. Only the transitional commit publication needs the second
    /// half; everything else resolves out of modules_.
    const Slot* slotForPublishLocked(int id) const
    {
        auto it = modules_.find(id);
        if (it != modules_.end()) return &it->second;
        for (const auto& pr : editRemoved_) if (pr.first == id) return &pr.second;
        return nullptr;
    }

    Slot* slotForPublishLocked(int id)
    {
        return const_cast<Slot*>(static_cast<const DspChain*>(this)->slotForPublishLocked(id));
    }

    void publishLocked()
    {
        // Suppressed for the length of an edit scope — that suppression IS the
        // feature: one publication for a whole preset instead of one per node.
        if (editing_) return;
        publishListLocked(idOrder_);
    }

    /// `flags` is an optional bypass/branch/sidechain OVERRIDE, keyed by node
    /// id, for ids it contains; anything absent from it reads its live slot.
    /// Only the commit's TRANSITIONAL publication passes one, and it passes the
    /// snapshot beginEdit took — see commitEdit for why publishing the old
    /// ORDER with the new FLAGS was a topology change the scope had promised
    /// was invisible until commit.
    void publishListLocked(const std::vector<int>& ids,
                           const std::map<int, NodeFlags>* flags = nullptr)
    {
        publishCount_.fetch_add(1, std::memory_order_relaxed);
        // What is published IS what the branch alignment is derived from, so
        // record it before deriving. Assigning first is load-bearing: a
        // setParam or a device change between this publication and the next
        // re-derives from `publishedOrder_`, and that has to be the order the
        // callback is actually running.
        publishedOrder_ = ids;
        recomputeBranchPdcLocked(publishedOrder_);
        auto order = std::make_shared<Order>();
        order->reserve(ids.size());
        // Does anything in THIS order want the dry-input key, or the song-mix
        // key? Decided once here, on the message thread, so the callback pays
        // for each copy only when a node is actually keyed off it — the same
        // "off until asked for" discipline the meters follow.
        bool wantsDry = false;
        bool wantsSong = false;
        for (int id : ids)
        {
            Slot* slot = slotForPublishLocked(id);
            if (slot != nullptr && slot->module != nullptr)
            {
                NodeFlags eff{ slot->bypassed, slot->branch, slot->sidechain };
                if (flags != nullptr)
                {
                    auto fi = flags->find(id);
                    if (fi != flags->end()) eff = fi->second;
                }
                DspModule* m = slot->module.get();
                // Identify routing nodes ONCE at publish time (message thread)
                // so the audio thread never string-compares. typeId() is the
                // factory's stable identity — only RoutingModules.h classes
                // carry these ids, so the static_cast is safe without RTTI.
                SplitterModule* sp = std::strcmp(m->typeId(), "splitter") == 0
                                         ? static_cast<SplitterModule*>(m) : nullptr;
                MergerModule* mg = std::strcmp(m->typeId(), "merger") == 0
                                       ? static_cast<MergerModule*>(m) : nullptr;
                NodeMeter* mt = nullptr;
                const int mi = slot->meter;
                if (mi >= 0 && (size_t) mi < meterPool_.size()) mt = meterPool_[(size_t) mi].get();
                NodeFade* fd = fadeAtLocked(slot->fade);
                NodeRef ref{ m, eff.bypassed, eff.branch, sp, mg, mt, fd, eff.sidechain };
                ref.spilling = slot->spilling;
                order->push_back(ref);
                if (! eff.bypassed)
                {
                    if (eff.sidechain == kSidechainSeatInput) wantsDry = true;
                    if (eff.sidechain == kSidechainSongMix)   wantsSong = true;
                }
            }
        }
        // Spillover tails (v37). The ones the audio thread has marked Done leave
        // with THIS publication and are destroyed after its barrier, below; the
        // rest ride along at the end of the order, flagged so the walk skips
        // them and the tail pass runs them.
        std::vector<SpillSlot> reaped;
        for (size_t i = spills_.size(); i-- > 0;)
        {
            const SpillTail* t = tailAtLocked(spills_[i].tail);
            if (t == nullptr || t->state.load(std::memory_order_acquire) == SpillTail::kDone)
            {
                reaped.push_back(std::move(spills_[i]));
                spills_.erase(spills_.begin() + (std::ptrdiff_t) i);
            }
        }
        for (auto& s : spills_)
        {
            if (s.slot.module == nullptr) continue;
            NodeRef ref{ s.slot.module.get(), true, 0, nullptr, nullptr, nullptr,
                         fadeAtLocked(s.slot.fade), kSidechainNone };
            ref.spill = tailAtLocked(s.tail);
            order->push_back(ref);
        }
        rtSidechainDry_.store(wantsDry, std::memory_order_release);
        rtSidechainSong_.store(wantsSong, std::memory_order_release);
        recomputeIdleHoldLocked();
        auto old = std::atomic_exchange_explicit(&order_, std::shared_ptr<const Order>(order),
                                                 std::memory_order_acq_rel);
        rtNodeCount_.store((int) order->size(), std::memory_order_release);
        rtEditGeneration_.fetch_add(1, std::memory_order_acq_rel);
        // The OLD snapshot must die HERE, on the message thread. A callback
        // that loaded it holds a shared_ptr copy for the whole block; if this
        // thread dropped its reference first, the callback's copy would be the
        // last owner and the vector would be freed on the audio thread —
        // measured at 435 frees over one 512-block structure storm by
        // juce_sharp_rtcheck (RtStructureStormCrossfadesWithoutAllocating),
        // and latent in every bypass/move since the chain existed. The
        // barrier waits for any in-flight block, after which nothing but
        // `old` can reference it. (writeMutex_ -> processMutex_ is the same
        // order remove/clear already take; the audio thread never takes
        // writeMutex_, so it cannot invert.)
        if (old)
        {
            barrier();
            old.reset();
        }
        // Past the barrier no block can still hold a reaped tail. Its pool
        // slots go back and its module dies here, on this thread — a delay or
        // reverb (the only spill types) owns plain buffers, no thread or model.
        for (auto& s : reaped)
        {
            releaseMeterLocked(s.slot.meter);
            releaseFadeLocked(s.slot.fade);
            releaseTailLocked(s.tail);
        }
    }

    /// writeMutex_ held. Idle sleep: the longest settle over EVERY node of the
    /// PUBLISHED order, bypassed ones included — a bypassed delay with trails
    /// still runs, and a type that is not on the allow-list vetoes the whole
    /// seat (-1). Re-run on every publication and every accepted param write,
    /// because a settle can follow a param (a gate's release).
    void recomputeIdleHoldLocked()
    {
        double settle = publishedOrder_.empty() ? -1.0 : 0.0;
        for (int id : publishedOrder_)
        {
            const Slot* slot = slotForPublishLocked(id);
            if (slot == nullptr || slot->module == nullptr) continue;
            const double sv = idleSettleSecondsFor(*slot->module);
            if (sv < 0.0) { settle = -1.0; break; }
            if (sv > settle) settle = sv;
        }
        idleMaxSettleSeconds_ = settle;
        publishIdleHoldLocked();
    }

    /// writeMutex_ held. The published order's settle, in samples at the
    /// prepared rate (IdleSleep.h's idleHoldSamples).
    void publishIdleHoldLocked()
    {
        rtIdleHoldSamples_.store(idleHoldSamples(idleMaxSettleSeconds_, sampleRate_),
                                 std::memory_order_release);
    }

    void eraseId(int nodeId)
    {
        auto pos = std::find(idOrder_.begin(), idOrder_.end(), nodeId);
        if (pos != idOrder_.end()) idOrder_.erase(pos);
    }

    // Wait until any audio block that may still hold the previous snapshot has
    // finished, so a just-removed module can be safely destroyed.
    void barrier()
    {
        std::lock_guard<std::mutex> b(processMutex_);
    }

    mutable std::mutex             writeMutex_;   // serializes message-thread mutations
    std::mutex                     processMutex_; // wait-one-block barrier vs. audio process
    std::map<int, Slot>            modules_;      // node id -> owned module + bypass
    std::vector<int>              idOrder_;        // chain order by node id
    // The id list the LIVE snapshot was built from. Equal to idOrder_ outside
    // an edit scope and to the pre-edit chain inside one, which is the whole
    // reason it exists: branch PDC writes straight into the mergers the audio
    // thread reads next block, so it has to be derived from what is PLAYING,
    // never from the shadow. (writeMutex_.)
    std::vector<int>              publishedOrder_;
    std::shared_ptr<const Order>  order_;         // atomic snapshot for the audio thread
    std::atomic<int>               rtNodeCount_{ 0 }; // lock-free count for the audio thread
    // Idle sleep (IdleSleep.h). The generation starts at 1 so a seat's first
    // block always reads as a change; the hold starts vetoed.
    std::atomic<uint64_t>          rtEditGeneration_{ 1 };
    std::atomic<long long>         rtIdleHoldSamples_{ -1 };
    std::atomic<bool>              rtFadedLastBlock_{ false }; // written by process()
    double                         idleMaxSettleSeconds_ = -1.0; // writeMutex_
    int                            nextId_ = 0;
    std::atomic<unsigned long>     publishCount_{ 0 }; // snapshots published (test hook)
    static inline std::atomic<unsigned long> staleScopeAborts_{ 0 };
    static inline LogSink                    logSink_ = nullptr;

    // ---- Edit scope state (writeMutex_) ------------------------------------
    // editRemoved_ holds slots that are OUT of the shadow chain but still in
    // the LIVE snapshot, so they must stay alive (and be re-prepared by a
    // device change) until the commit's barrier retires them.
    /// Branch delay compensation, ON by default (ABI v34).
    ///
    /// ATOMIC, because recomputeBranchPdcRt reads it from the AUDIO THREAD
    /// while setBranchPdc writes it on the message thread (W5-E3). It was a
    /// plain `bool` from v34 to 2026-09-12: a data race by the standard's
    /// definition, benign-looking on x86 but with an audible consequence — a
    /// stale read programs a whole parallel region's mergers to the wrong
    /// compensation. Every other cross-thread scalar in this class is an
    /// std::atomic for exactly this reason, because the callback may not take
    /// writeMutex_. Relaxed on both sides: it orders nothing, it IS the value.
    std::atomic<bool>                   branchPdc_ { true };
    /// Set by a param/string write, consumed by the audio thread AFTER the
    /// modules have applied their mailboxes (W4-D54). An atomic and not a plain
    /// bool: the message thread sets it and the callback clears it.
    std::atomic<bool>                   pdcDirty_ { false };
    bool                                editing_ = false;
    /// A commitEdit is in flight and has DROPPED writeMutex_ to wait for fades.
    /// beginEdit waits on editCv_ for it; abortEdit is refused. Without it a
    /// begin/abort landing in that window aborted the commit's scope and the
    /// resumed commit then clobbered the new one. (writeMutex_.)
    bool                                committing_ = false;
    std::condition_variable             editCv_;
    std::vector<int>                    preEditOrder_;
    std::map<int, NodeFlags>            preEditFlags_;
    std::vector<int>                    editAdded_;
    std::vector<std::pair<int, Slot>>   editRemoved_;

    // Per-node meters (v30). Grow-only pool + free list; the audio thread only
    // ever sees the raw pointers baked into the published snapshot.
    std::vector<std::unique_ptr<NodeMeter>> meterPool_;
    std::vector<int>                        freeMeters_;
    std::atomic<bool>                       metering_{ false };
    std::atomic<float>                      rtInvSampleRate_{ 1.0f / 44100.0f };

    // Per-node wet/dry ramps. Same pool discipline as the meters.
    std::vector<std::unique_ptr<NodeFade>> fadePool_;
    std::vector<int>                       freeFades_;
    std::atomic<float>                     rtFadeInc_{ 1.0f / (kFadeSeconds * 44100.0f) };
    std::atomic<unsigned>                  rtBlocks_{ 0 }; // process() heartbeat
    /// How many 1 ms sleeps structure edits have spent waiting for a fade.
    /// Test/diagnostic hook: an edit on a chain nothing is processing must cost
    /// ZERO of them.
    std::atomic<unsigned long>             fadeWaitSleeps_{ 0 };
    std::vector<float>                     dryData_;   // nch * maxBlock, sized in prepare()
    std::vector<float*>                    dryPtrs_;   // [channel] -> dry copy
    // Sidechain key: the chain's input block (v34). Sized in prepare();
    // rtSidechainDry_ says whether the published order needs it filled.
    std::vector<float>                     scDryData_;
    std::vector<float*>                    scDryPtrs_;
    std::atomic<bool>                      rtSidechainDry_{ false };

    // Song-mix key scratch (v34). The engine hands processDspRig a READ-ONLY
    // pointer into its own output block, and that block is UNTRUSTED: at the
    // point the seat loop runs it is the post-song-EQ buffer, i.e. the output
    // of six juce::dsp::IIR biquads that reset() only on an OFF->ON
    // transition, and processMasterBus scrubs only at its TOP. A NaN there used
    // to go straight into NoiseGateCore's recursive one-pole detector and
    // CompressorCore's sidechain HPF + RMS accumulator, which the chain's
    // per-stage scrub cannot clean because it cleans the BUFFER and never a
    // module's internal state — so the gate latched closed and the compressor
    // latched for the session, on one seat, with nothing saying why. We cannot
    // scrub the engine's buffer (it is const, shared across seats, and owned
    // elsewhere), so the key is COPIED here and the copy is scrubbed. Same
    // "off until asked for" discipline as the dry key: rtSidechainSong_.
    std::vector<float>                     scSongData_;
    std::vector<float*>                    scSongPtrs_;
    std::atomic<bool>                      rtSidechainSong_{ false };

    // Spillover (v37). spills_/tailPool_/freeTails_/spillover_/spillArmSeq_ are
    // writeMutex_ state; the audio thread only sees the SpillTail pointers baked
    // into the published order. The tail pool is grow-only like the fade pool,
    // so a stale snapshot's pointer never dangles.
    bool                                     spillover_ = false;
    unsigned long                            spillArmSeq_ = 0;
    std::vector<SpillSlot>                   spills_;
    std::vector<std::unique_ptr<SpillTail>>  tailPool_;
    std::vector<int>                         freeTails_;
    std::vector<float>                       spillData_; // nch * maxBlock, sized in prepare()
    std::vector<float*>                      spillPtrs_;

    // Split/parallel scratch (v23). Sized in prepare(); audio-thread read-only.
    std::vector<float>               branchData_; // kDspMaxBranches * nch * maxBlock
    std::vector<std::vector<float*>> branchPtrs_; // [branch][channel] -> buffer

    double sampleRate_ = 44100.0;
    int    maxBlock_ = 512;
    int    deviceBlock_ = 512;   // TIMING only — see prepare()
    int    numChannels_ = 2;
    bool   prepared_ = false;
};

} // namespace dcrig
