#include "dsp/NamAmpModule.h"
#include "dsp/Retirement.h"
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dcrig { extern void (*namLoadTestHook)(int); extern int namNonFiniteTestBlocks;
                  void namReportRetirementOverflowForTest(int pending, bool overflowed, bool& wasOver); }
namespace {
std::mutex gate;
std::condition_variable changed;
int prepared = 0, completed = 0, allowed = 0;
void hook(int stage)
{
    std::unique_lock<std::mutex> lock(gate);
    if (stage == 1)
    {
        const int attempt = ++prepared;
        changed.notify_all();
        changed.wait(lock, [attempt] { return allowed >= attempt; });
    }
    else { ++completed; changed.notify_all(); }
}
void waitFor(int& counter, int value)
{
    std::unique_lock<std::mutex> lock(gate);
    if (!changed.wait_for(lock, std::chrono::seconds(60), [&] { return counter >= value; }))
    { std::fprintf(stderr, "NAM worker did not reach expected stage\n"); std::exit(2); }
}
void release(int attempt)
{
    std::lock_guard<std::mutex> lock(gate); allowed = attempt; changed.notify_all();
}
bool loaded(dcrig::DspModule& module)
{ float value = -1; return module.getParam("loaded", value) && value == 1.f; }

constexpr int kBlock = 256;

/// One block of 220 Hz through the module; returns the peak magnitude out.
float runBlock(dcrig::DspModule& module, double& phase)
{
    std::vector<float> buf((std::size_t) kBlock, 0.f);
    for (int i = 0; i < kBlock; ++i)
    {
        buf[(std::size_t) i] = 0.25f * (float) std::sin(phase);
        phase += 2.0 * 3.14159265358979323846 * 220.0 / 44100.0;
    }
    float* ch[1] = { buf.data() };
    module.process(ch, 1, kBlock);
    float peak = 0.f;
    for (float v : buf) { if (!std::isfinite(v)) return -1.f; peak = std::max(peak, std::fabs(v)); }
    return peak;
}

/// W4-D34. The neural stage is UNTRUSTED by this engine's own definition, and
/// AmpFace::processPost is five recursive Biquads. A non-finite sample out of
/// NeuralAudio latched all five permanently — and AmpFace::reset() is only
/// reached from NamAmpModule::reset(), which by this repo's rule has exactly one
/// caller — so the amp went silent for the session with nothing saying why. The
/// module's INPUT was already scrubbed and the chain scrubs after the module;
/// the gap was precisely between the model and the face.
///
/// Not vacuous: without the scrub the recovery peak is 0.0 for ever (the final
/// clamp turns the latched NaN into silence), and this returns 1.
int neuralNaNCannotLatchTheAmpFace(dcrig::DspModule& module)
{
    // AWAY FROM FLAT FIRST. At its default tone settings the face's shelves and
    // peak are designed at 0 dB, i.e. as identity biquads with no feedback path
    // at all — a NaN passes through one sample and is gone, and the probe reads
    // green with or without the scrub. The whole hazard is the RECURSION, so the
    // filters have to be recursive for this to test anything.
    for (const char* id : { "bass", "mid", "treble", "presence", "resonance" })
        module.setParam(id, 1.0f);

    double phase = 0.0;
    for (int i = 0; i < 8; ++i) runBlock(module, phase);     // settle
    const float before = runBlock(module, phase);
    if (!(before > 1e-4f))
    { std::fprintf(stderr, "the amp was already silent before the probe (%f)\n", before); return 1; }

    dcrig::namNonFiniteTestBlocks = 1;                        // one bad sample
    const float during = runBlock(module, phase);
    if (during < 0.f)
    { std::fprintf(stderr, "a non-finite sample reached the module's OUTPUT\n"); return 1; }

    float after = 0.f;
    for (int i = 0; i < 8; ++i) after = std::max(after, runBlock(module, phase));
    if (!(after > 1e-4f))
    {
        std::fprintf(stderr, "one non-finite sample out of the capture silenced the amp face "
                             "for good (peak %f after %f before)\n", after, before);
        return 1;
    }
    return 0;
}

/// W4-D37. sweep() only claims items whose use_count() == 1; anything it cannot
/// claim used to be destroyed by ordinary member destruction of ~RetirementQueue,
/// dropping a reference that may not be the last one — which re-creates the exact
/// "the callback runs a NeuralAudio destructor" failure this class exists to
/// eliminate. The quoted guarantee ("the chain's remove barrier means no audio
/// block still references this module") is a safety claim about another class
/// with no assertion behind it. A leak beats a free on the audio thread.
///
/// Not vacuous: without the leak the destructor below runs inside the scope and
/// `died` is true while the reader still holds its reference.
int aQueueDestroyedUnderAReaderLeaksRatherThanFrees()
{
    struct Heavy
    {
        bool* died = nullptr;
        std::vector<float> payload;
        ~Heavy() { if (died != nullptr) *died = true; }
    };

    bool died = false;
    std::shared_ptr<Heavy> readersCopy;
    const unsigned long leaked0 = dcdsp::RetirementQueue<Heavy>::leakedForTest();
    {
        dcdsp::RetirementQueue<Heavy> queue;
        auto held = std::make_shared<Heavy>();
        held->died = &died;
        held->payload.assign(64 * 1024, 0.f);
        readersCopy = held;                 // "the callback copied the snapshot"
        queue.retire(std::move(held));
        if (queue.sweep() != 1)
        { std::fprintf(stderr, "sweep claimed an object the reader still holds\n"); return 1; }
    }                                       // ~RetirementQueue, reader still holding
    if (died)
    { std::fprintf(stderr, "~RetirementQueue dropped the LAST reference to an object a reader "
                           "still held — the reader's release would have run the destructor\n"); return 1; }
    if (dcdsp::RetirementQueue<Heavy>::leakedForTest() != leaked0 + 1)
    { std::fprintf(stderr, "the leak was not counted, so nobody can ever see it\n"); return 1; }

    readersCopy.reset();                    // the reader lets go; the leak stays a leak
    if (died)
    { std::fprintf(stderr, "the leaked object was freed after all\n"); return 1; }
    return 0;
}

/// W4-D38 part 1: what the QUEUE actually does, which is not what it says.
///
/// RetirementQueue raises `overflow_` past kSoftCap so a caller can SEE that the
/// unbounded queue is filling — the one thing its own comment says was missing
/// ("unbounded with no way to SEE it is the part that was wrong"). Nothing ever
/// called takeOverflowWarning(), so that was still true.
///
/// Its doc says "edge-triggered, so a caller can log once per excursion instead
/// of once per retire". It is NOT: `retire()` sets the flag on EVERY retire
/// while size > kSoftCap, so taking it per retire logs per retire. This case was
/// written expecting the doc and caught it, and it now asserts the behaviour in
/// the direction that is TRUE so the next reader is not misled.
int theQueueRaisesItsOverflowFlagPerRetireNotPerExcursion()
{
    dcdsp::RetirementQueue<int> queue;
    const int cap = dcdsp::RetirementQueue<int>::kSoftCap;

    for (int i = 0; i < cap; ++i)
    {
        queue.retire(std::make_shared<int>(i));
        if (queue.takeOverflowWarning())
        { std::fprintf(stderr, "the queue flagged overflow at or below its soft cap\n"); return 1; }
    }

    queue.retire(std::make_shared<int>(cap));
    if (! queue.takeOverflowWarning())
    { std::fprintf(stderr, "no overflow flag one past the soft cap - the one thing that makes "
                           "an unbounded queue visible never fires\n"); return 1; }

    queue.retire(std::make_shared<int>(cap + 1));
    if (! queue.takeOverflowWarning())
    { std::fprintf(stderr, "the queue no longer re-raises its overflow flag per retire. That is "
                           "an improvement and matches its own doc - move the latch out of "
                           "NamAmpModule's reportRetirementOverflowIfAny and delete this case\n");
      return 1; }
    return 0;
}

/// W4-D38 part 2: the MODULE's latch. One message per excursion, and a new
/// excursion speaks again.
///
/// The de-duplication lives in namamp rather than in the queue because the queue
/// is not this module's to change — and the layering is better this way: the
/// queue reports a STATE, the logger decides how often to speak about it.
int theOverflowWarningIsReportedOncePerExcursion()
{
    const int cap = dcdsp::RetirementQueue<int>::kSoftCap;
    const unsigned long before = dcrig::namOverflowWarningsForTest();
    bool wasOver = false;

    for (int i = 1; i <= cap; ++i)
        dcrig::namReportRetirementOverflowForTest(i, false, wasOver);
    if (dcrig::namOverflowWarningsForTest() != before)
    { std::fprintf(stderr, "a warning was reported at or below the soft cap\n"); return 1; }

    // One excursion: the queue flags every retire, the module speaks ONCE.
    for (int i = cap + 1; i <= cap + 12; ++i)
        dcrig::namReportRetirementOverflowForTest(i, true, wasOver);
    if (dcrig::namOverflowWarningsForTest() != before + 1)
    { std::fprintf(stderr, "twelve retires inside ONE excursion produced more than one warning - "
                           "a user arrowing through a model list would get a line per "
                           "selection\n"); return 1; }

    // The sweeper drains back to the cap: the latch re-arms, quietly.
    dcrig::namReportRetirementOverflowForTest(cap, false, wasOver);
    if (dcrig::namOverflowWarningsForTest() != before + 1)
    { std::fprintf(stderr, "draining back to the cap emitted a warning of its own\n"); return 1; }

    // ...and a SECOND excursion speaks again. Without this the "once" above
    // would be satisfied by a latch that never re-arms and warns exactly once
    // for the life of the process.
    dcrig::namReportRetirementOverflowForTest(cap + 1, true, wasOver);
    if (dcrig::namOverflowWarningsForTest() != before + 2)
    { std::fprintf(stderr, "a SECOND excursion past the soft cap did not warn - the latch never "
                           "re-arms\n"); return 1; }
    return 0;
}
}
int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    dcrig::namLoadTestHook = hook;
    auto module = dcrig::createNamAmpModule();
    module->prepare(44100, 256, 1); // include post-loader resampler preparation
    module->setStringParam("modelPath", argv[1]);
    waitFor(prepared, 1);
    module->setStringParam("modelPath", "");
    release(1); waitFor(completed, 1);
    if (loaded(*module)) { std::fprintf(stderr, "cleared capture was republished\n"); return 1; }

    module->setStringParam("modelPath", argv[1]);
    waitFor(prepared, 2);
    module->setStringParam("modelPath", argv[1]); // supersedes after expensive preparation
    release(2); waitFor(completed, 2); waitFor(prepared, 3);
    if (loaded(*module)) { release(3); std::fprintf(stderr, "superseded capture was published\n"); return 1; }
    release(3); waitFor(completed, 3);
    if (!loaded(*module)) { std::fprintf(stderr, "latest capture never published\n"); return 1; }

    // The loader hook is done with; the remaining probes drive the audio path.
    dcrig::namLoadTestHook = nullptr;
    if (neuralNaNCannotLatchTheAmpFace(*module) != 0) return 1;
    module.reset();

    if (aQueueDestroyedUnderAReaderLeaksRatherThanFrees() != 0) return 1;
    if (theQueueRaisesItsOverflowFlagPerRetireNotPerExcursion() != 0) return 1;
    if (theOverflowWarningIsReportedOncePerExcursion() != 0) return 1;

    std::puts("PASS: NAM clear and superseding requests cannot publish stale prepared captures");
    std::puts("PASS: a non-finite sample out of the capture cannot latch the amp face");
    std::puts("PASS: a retirement queue destroyed under a reader leaks instead of freeing");
    std::puts("PASS: the queue re-raises its overflow flag per retire (its doc says otherwise)");
    std::puts("PASS: namamp reports the soft-cap warning once per excursion, and re-arms");
    return 0;
}
