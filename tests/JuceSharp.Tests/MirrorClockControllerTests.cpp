// Standalone unit tests for dotcharter::MirrorClockController — the pure
// clock/drift logic behind the streaming-mirror async sample-rate converter
// (src/JuceSharp.Native/src/MirrorClockController.h). ZERO JUCE / framework
// deps, so it compiles + runs with a single cl.exe invocation, matching the
// project's "standalone MSVC harness" convention for the dcdsp cores.
//
// Build + run (from a VS x64 dev prompt / after vcvars64.bat):
//   cl /EHsc /std:c++17 /nologo MirrorClockControllerTests.cpp && MirrorClockControllerTests.exe
//
// The header is the SINGLE source of truth — MirrorOutput.cpp includes the same
// file, so there is no second copy of the math to drift out of sync.

#include "../../src/JuceSharp.Native/src/MirrorClockController.h"

#include <cmath>
#include <cstdio>
#include <string>

using dotcharter::MirrorClockController;
using dotcharter::MirrorClockDecision;

static int g_failures = 0;
static int g_checks   = 0;

static void check(bool cond, const std::string& msg)
{
    ++g_checks;
    if (!cond)
    {
        ++g_failures;
        std::printf("  FAIL: %s\n", msg.c_str());
    }
}

static void checkNear(double a, double b, double tol, const std::string& msg)
{
    ++g_checks;
    if (std::fabs(a - b) > tol)
    {
        ++g_failures;
        std::printf("  FAIL: %s (got %.9f, expected %.9f, tol %.9f)\n", msg.c_str(), a, b, tol);
    }
}

// --------------------------------------------------------------------------
static void testNominalRatio()
{
    std::printf("[nominal ratio]\n");
    MirrorClockController c;
    c.configure(48000.0, 48000.0, 7200);
    checkNear(c.nominalRatio(), 1.0, 1e-12, "matched rates -> ratio 1.0");

    c.configure(48000.0, 44100.0, 7200);
    checkNear(c.nominalRatio(), 48000.0 / 44100.0, 1e-12, "48k engine / 44.1k device -> ~1.0884");

    c.configure(44100.0, 48000.0, 6615);
    checkNear(c.nominalRatio(), 44100.0 / 48000.0, 1e-12, "44.1k engine / 48k device -> ~0.91875");
}

// --------------------------------------------------------------------------
static void testPreRollPriming()
{
    std::printf("[pre-roll priming]\n");
    const int target = MirrorClockController::targetFramesForRate(48000.0); // ~7200
    check(target > 6000 && target < 8000, "target ~150ms @48k is ~7200 frames");

    MirrorClockController c;
    c.configure(48000.0, 48000.0, target);

    // Below target: never primed, always silence, never consumes.
    for (int fill = 0; fill < target; fill += target / 4)
    {
        auto d = c.step(fill, 480);
        check(!d.produce, "not producing while filling");
        check(!d.primed,  "not primed while filling");
        check(d.inputFramesNeeded == 0, "consumes nothing while filling");
    }
    check(!c.primed(), "still un-primed just below target");

    // Reaching target primes and immediately produces (target >> needed).
    auto d = c.step(target, 480);
    check(d.primed, "primes exactly at target fill");
    check(d.produce, "produces once primed with a full buffer");
    check(c.primed(), "controller reports primed");
}

// --------------------------------------------------------------------------
static void testRatioAtTargetAndDirection()
{
    std::printf("[ratio at target + nudge direction]\n");
    const double engine = 48000.0, device = 44100.0;
    const int target = 7200;
    MirrorClockController c;
    c.configure(engine, device, target);
    c.step(target, 480); // prime at target

    const double nominal = engine / device;

    // At target fill -> effectively no trim.
    auto atTarget = c.step(target, 480);
    checkNear(atTarget.effectiveRatio, nominal, 1e-9, "ratio == nominal at target fill");

    // Over-full -> ratio nudges UP (consume more engine frames -> drain back).
    auto over = c.step(target + target / 2, 480);
    check(over.effectiveRatio > nominal, "over-full nudges ratio UP (drains toward target)");

    // Under-full (but still enough to produce) -> ratio nudges DOWN (drain
    // slower -> refill toward target).
    auto under = c.step(target / 8, 480); // 900 frames >> needed(~526) so it still produces
    check(under.produce, "still produces when fill >= needed");
    check(under.effectiveRatio < nominal, "under-full nudges ratio DOWN (refills toward target)");
}

// --------------------------------------------------------------------------
static void testTrimClamp()
{
    std::printf("[trim clamp]\n");
    const double engine = 48000.0, device = 48000.0;
    const int target = 7200;
    MirrorClockController c;
    c.configure(engine, device, target);
    c.step(target, 480); // prime

    const double nominal = engine / device;
    const double maxTrim = MirrorClockController::kMaxTrim;

    // Sweep a wide fill range; |eff/nominal - 1| must never exceed the clamp.
    for (int fill = 500; fill < 40000; fill += 250)
    {
        auto d = c.step(fill, 480);
        const double frac = std::fabs(d.effectiveRatio / nominal - 1.0);
        check(frac <= maxTrim + 1e-9, "trim magnitude within +/-kMaxTrim across the fill range");
    }

    // Extreme over-full saturates at exactly +kMaxTrim.
    auto huge = c.step(target * 3, 480); // normErr = 2 -> clamped to 1
    checkNear(huge.effectiveRatio, nominal * (1.0 + maxTrim), 1e-9, "extreme over-full saturates at +kMaxTrim");
}

// --------------------------------------------------------------------------
static void testInputFrameCount()
{
    std::printf("[input-frame count]\n");
    const double engine = 48000.0, device = 44100.0;
    const int target = 7200;
    MirrorClockController c;
    c.configure(engine, device, target);
    c.step(target, 480); // prime

    const int outFrames = 512;
    auto d = c.step(target, outFrames); // ~no trim
    const int expected = static_cast<int>(std::ceil(outFrames * d.effectiveRatio)) + MirrorClockController::kInterpGuard;
    check(d.inputFramesNeeded == expected, "needed == ceil(outFrames*ratio) + guard");
    check(d.inputFramesNeeded > outFrames, "needs more engine frames than output frames when engine > device");
}

// --------------------------------------------------------------------------
// End-to-end control-law simulation: model the FIFO fill under a producer that
// adds engine frames at the (possibly drifting) real device pace, and a
// consumer that removes exactly what the interpolator uses (~outFrames*eff).
// Asserts convergence toward target and that the buffer never hits either wall.
static double simulateFill(double engine, double device, int target,
                           double startFillFrac, double driftPpm, int blocks,
                           int outFrames, double fifoFrames,
                           double* minFillOut, double* maxFillOut)
{
    MirrorClockController c;
    c.configure(engine, device, target);

    const double nominal = engine / device;
    double fill = target * startFillFrac;
    // Prime first (feed the controller a full buffer until it primes).
    {
        auto d = c.step(static_cast<int>(fill), outFrames);
        (void) d;
    }

    double minFill = fill, maxFill = fill;
    const double producePerBlock = outFrames * nominal * (1.0 + driftPpm * 1e-6);

    for (int i = 0; i < blocks; ++i)
    {
        auto d = c.step(static_cast<int>(fill), outFrames);
        double consumed = 0.0;
        if (d.produce)
            consumed = outFrames * d.effectiveRatio; // interpolator eats ~outFrames*ratio input frames
        fill += producePerBlock - consumed;
        if (fill < 0.0) fill = 0.0;
        if (fill > fifoFrames) fill = fifoFrames; // producer would drop newest on overrun
        if (fill < minFill) minFill = fill;
        if (fill > maxFill) maxFill = fill;
    }
    if (minFillOut) *minFillOut = minFill;
    if (maxFillOut) *maxFillOut = maxFill;
    return fill;
}

static void testConvergence()
{
    std::printf("[convergence]\n");
    const int target = 7200;
    const double fifo = 65536.0;

    // Start over-full, matched rates, no drift -> converge DOWN to target.
    double mn = 0, mx = 0;
    double f1 = simulateFill(48000.0, 48000.0, target, 1.30, 0.0, 20000, 480, fifo, &mn, &mx);
    check(std::fabs(f1 - target) < target * 0.05, "over-full converges to within 5% of target");
    check(mn > 0.0 && mx < fifo, "over-full run never hits an empty/overflow wall");

    // Start under-full -> converge UP to target.
    double f2 = simulateFill(48000.0, 48000.0, target, 0.30, 0.0, 20000, 480, fifo, &mn, &mx);
    check(std::fabs(f2 - target) < target * 0.05, "under-full converges to within 5% of target");
    check(mn > 0.0 && mx < fifo, "under-full run never hits an empty/overflow wall");

    // Mismatched rates (48k engine / 44.1k device) start at target, no drift:
    // the nominal ratio does the bulk conversion, so it stays parked.
    double f3 = simulateFill(48000.0, 44100.0, target, 1.00, 0.0, 20000, 512, fifo, &mn, &mx);
    check(std::fabs(f3 - target) < target * 0.05, "rate-mismatch stays parked near target");
    check(mn > 0.0 && mx < fifo, "rate-mismatch run stays off both walls");
}

static void testDriftHolds()
{
    std::printf("[crystal-drift steady state]\n");
    const int target = 7200;
    const double fifo = 65536.0;

    // Device crystal runs +200 ppm fast (produces faster than nominal). The
    // proportional trim should settle the fill at a bounded offset and never
    // walk into the wall over a long run (~7 minutes @ 480/48k).
    double mn = 0, mx = 0;
    double f = simulateFill(48000.0, 48000.0, target, 1.00, +200.0, 40000, 480, fifo, &mn, &mx);
    check(mn > target * 0.5, "fill stays well above empty under +200ppm drift");
    check(mx < fifo * 0.95,  "fill stays well below overflow under +200ppm drift");
    check(f > 0.0 && f < fifo, "final fill bounded under +200ppm drift");

    // -200 ppm (device slow) — mirror direction.
    double f2 = simulateFill(48000.0, 48000.0, target, 1.00, -200.0, 40000, 480, fifo, &mn, &mx);
    check(mn > target * 0.3, "fill stays above empty under -200ppm drift");
    check(mx < fifo, "fill stays below overflow under -200ppm drift");
    check(f2 > 0.0 && f2 < fifo, "final fill bounded under -200ppm drift");
}

// --------------------------------------------------------------------------
static void testReprimeHysteresis()
{
    std::printf("[re-prime hysteresis]\n");
    const double engine = 48000.0, device = 48000.0;
    const int target = MirrorClockController::targetFramesForRate(engine); // 7200
    const int outFrames = 480;

    // Brief hiccup: prime, a handful of starved blocks, then recovery — must
    // stay primed the whole time (no full re-prime for a short gap).
    {
        MirrorClockController c;
        c.configure(engine, device, target);
        c.step(target, outFrames); // prime
        check(c.primed(), "primed after a full buffer");
        for (int i = 0; i < 5; ++i)
        {
            auto d = c.step(0, outFrames);
            check(!d.produce, "silence during brief underrun");
            check(c.primed(), "stays primed through a brief underrun");
        }
        auto good = c.step(target, outFrames);
        check(good.produce, "resumes producing immediately when data returns");
        check(c.primed(), "still primed after recovery");
    }

    // Sustained starvation: after kReprimeSeconds of silence the controller
    // drops back to un-primed so it rebuilds the pre-roll when audio returns.
    {
        MirrorClockController c;
        c.configure(engine, device, target);
        c.step(target, outFrames); // prime
        const int flipBlock = static_cast<int>(device * MirrorClockController::kReprimeSeconds) / outFrames; // 25
        for (int i = 1; i < flipBlock; ++i)
            c.step(0, outFrames);
        check(c.primed(), "still primed just before the re-prime threshold");
        c.step(0, outFrames); // the block that crosses the threshold
        check(!c.primed(), "un-primes after sustained starvation");

        // And it re-primes cleanly: silence until the buffer refills to target.
        auto half = c.step(target / 2, outFrames);
        check(!half.produce && !half.primed, "stays silent while re-filling below target");
        auto full = c.step(target, outFrames);
        check(full.primed && full.produce, "re-primes when the buffer is full again");
    }
}

// --------------------------------------------------------------------------
// W4-N18: a targetFrames of 0 must not DISABLE the controller for ever.
//
// `configure` substitutes a sane default for a non-positive engineRate and for
// a non-positive deviceRate, and then used to substitute 0 — the one value that
// makes `step` unable to prime at all, because priming needs
// `targetFrames_ > 0 && fill >= targetFrames_`. Every call after that returns
// produce == false whatever the FIFO holds, i.e. a mirror that is SILENT for
// the life of the stream with nothing anywhere reporting an error. Of the three
// possible answers to a bad argument (clamp, reject, disable), that is the
// worst, and it is one a host reaches by calling configure before it has
// computed the depth.
//
// Latent rather than live — every shipping caller passes
// targetFramesForRate(...) — which is exactly why nothing would have caught it.
static void testZeroTargetFallsBackToTheDefaultDepth()
{
    std::printf("[zero target falls back]\n");

    MirrorClockController c;
    c.configure(48000.0, 48000.0, 0);

    const int expected = MirrorClockController::targetFramesForRate(48000.0);
    check(c.targetFrames() == expected,
          "configure(.., 0) adopts the default pre-roll depth rather than 0");

    // Non-vacuity: the default is a real depth, so the priming assertions below
    // are the same ones testPreRollPriming makes and not "0 >= 0".
    check(c.targetFrames() > 0, "the substituted depth is positive");
    check(!c.step(c.targetFrames() / 2, 480).produce,
          "still silent below the substituted target — it did not just prime on anything");

    auto d = c.step(c.targetFrames(), 480);
    check(d.primed,  "primes at the substituted target");
    check(d.produce, "PRODUCES at the substituted target — a 0 argument no longer mutes the "
                     "mirror for the life of the stream");

    // ...and the rate it derives the depth from is the ENGINE rate, not a
    // hard-coded 48 kHz.
    MirrorClockController c96;
    c96.configure(96000.0, 48000.0, 0);
    check(c96.targetFrames() == MirrorClockController::targetFramesForRate(96000.0),
          "the substituted depth follows the engine rate");
}

int main()
{
    std::printf("MirrorClockController unit tests\n");
    testNominalRatio();
    testPreRollPriming();
    testZeroTargetFallsBackToTheDefaultDepth();
    testRatioAtTargetAndDirection();
    testTrimClamp();
    testInputFrameCount();
    testConvergence();
    testDriftHolds();
    testReprimeHysteresis();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
        std::printf("ALL TESTS PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
