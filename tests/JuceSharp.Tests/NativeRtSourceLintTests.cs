using System;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Source lints over native code that the RT gate cannot reach.
/// <para>
/// <c>juce_sharp_rtcheck</c> compiles <c>JUCESHARP_CORE_SOURCES</c> a second
/// time, but <c>DOTCHARTER_DSP_NEURAL</c> never reaches the test target — the
/// string "namamp" is present in <c>juce_sharp.dll</c> and absent from
/// <c>juce_sharp_rtcheck.exe</c>. So <c>NamAmpModule.cpp</c> is NOT in the
/// allocation gate, and the one thing that can still be pinned from outside it
/// is that the module uses the mechanism the gate DOES cover
/// (<c>dcdsp::RetirementQueue</c>, pinned by
/// <c>RtRetiredSnapshotsDieOnTheOwnerThread</c>) rather than freeing a retired
/// model itself.
/// </para>
/// </summary>
public sealed class NativeRtSourceLintTests
{
    private const string NamAmp = "src/JuceSharp.Native/src/dsp/NamAmpModule.cpp";
    private const string EngineCpp = "src/JuceSharp.Native/src/AudioEngine.cpp";
    private const string EngineH = "src/JuceSharp.Native/src/AudioEngine.h";

    /// <summary>
    /// <c>midiFifo_</c> is a <c>juce::AbstractFifo</c>: single producer, single
    /// consumer, and <c>reset()</c> rewrites BOTH index atomics, so it is only
    /// legal when nothing else is touching the fifo. Both callers had two
    /// racers — the audio thread's <c>collectBridgeMidi</c> (which runs
    /// whenever no in-process instrument is attached, i.e. exactly around an
    /// attach) and <c>sendMidiShort</c> on the JUCE MIDI input thread.
    /// <c>discardQueuedMidi()</c> is a consumer-side drain under
    /// <c>remoteBridgeMutex_</c>, which is legal against both.
    /// <para>
    /// This is a source lint because the race itself is not reproducible on
    /// demand: the audio-thread consumer only runs with the sandbox bridge
    /// ENABLED, and a torn AbstractFifo loses or duplicates events rather than
    /// corrupting memory, so there is nothing deterministic to assert. What the
    /// gate CAN prove is that the discard still discards
    /// (<c>RtInstrumentAttachDiscardsStaleMidi</c>); what this proves is that
    /// nobody quietly puts <c>reset()</c> back.
    /// </para>
    /// </summary>
    [Fact]
    public void MidiFifo_IsNeverResetWhileAConsumerCanBeRunning()
    {
        var cpp = StripComments(File.ReadAllText(FindRepoFile(EngineCpp)));
        var h = StripComments(File.ReadAllText(FindRepoFile(EngineH)));

        Assert.DoesNotContain("midiFifo_.reset(", cpp, StringComparison.Ordinal);
        Assert.DoesNotContain("midiFifo_.reset(", h, StringComparison.Ordinal);

        // ...and the replacement is actually wired in at both call sites.
        var calls = Regex.Matches(cpp, @"\bdiscardQueuedMidi\(\)").Count;
        Assert.True(calls == 2,
            $"expected discardQueuedMidi() at exactly the two instrument slot changes in "
            + $"{EngineCpp} (attach and detach); found {calls}. If a third caller is "
            + "legitimate, widen this — do not delete it.");

        // The barrier is the point: a drain that does not exclude the audio
        // thread's consumer is the bug with extra steps.
        var body = Regex.Match(h, @"void discardQueuedMidi\(\) noexcept\s*\{(.|\n)*?\n    \}");
        Assert.True(body.Success, $"discardQueuedMidi() not found in {EngineH}");
        Assert.Contains("remoteBridgeMutex_", body.Value, StringComparison.Ordinal);
        Assert.Contains("prepareToRead", body.Value, StringComparison.Ordinal);
        Assert.Contains("finishedRead", body.Value, StringComparison.Ordinal);
    }

    /// <summary>
    /// Every publish of <c>active_</c> hands the old snapshot to the retirement
    /// queue. The audio thread copies <c>active_</c> for the duration of one
    /// <c>process()</c>, so whoever drops the last reference runs the
    /// destructor — and a whole NeuralAudio model destructor on the callback is
    /// the bug this replaced. The previous code spun on <c>use_count() &gt; 1</c>
    /// for 500 ms and then reset anyway, which handed the free to the audio
    /// thread in exactly the case the bound existed for.
    /// </summary>
    [Fact]
    public void NamAmpModule_RetiresEveryPublishedModel_AndFreesNoneItself()
    {
        // Comments are stripped first, the way this repo's other reachability
        // lints do: a name that survives only in prose ("this replaced
        // drainRelease") is exactly as inert as a `see cref`, and would
        // otherwise fail the very check the prose describes.
        var text = StripComments(File.ReadAllText(FindRepoFile(NamAmp)));
        var lines = text.Replace("\r\n", "\n").Split('\n');

        var publishes = Enumerable.Range(0, lines.Length)
            .Where(i => lines[i].Contains("atomic_exchange(&active_", StringComparison.Ordinal))
            .ToArray();

        Assert.True(publishes.Length >= 2,
            $"expected at least two publishes of active_ in {NamAmp} (the load path and the "
            + $"clear path); found {publishes.Length}. This lint is scanning the wrong file "
            + "or the module was restructured — repoint it, do not delete it.");

        foreach (var i in publishes)
        {
            var window = string.Join("\n", lines.Skip(i).Take(10));
            Assert.True(Regex.IsMatch(window, @"\bretire(d_\.retire)?\("),
                $"{NamAmp}:{i + 1} swaps active_ without retiring the old snapshot within "
                + "the next few lines. The retired model must go to dcdsp::RetirementQueue; "
                + "dropping it here can make the audio thread the last owner.");
        }

        Assert.DoesNotContain("drainRelease", text, StringComparison.Ordinal);
        Assert.Contains("RetirementQueue<LoadedModel> retired_", text, StringComparison.Ordinal);
    }

    /// <summary>
    ///     P2-14 (2026-08-29): between a device-rate / block-size <c>prepare()</c>
    ///     and the worker's re-publish, <c>process()</c> refuses the stale
    ///     snapshot and passes DRY while <c>loaded</c> read 1 and
    ///     <c>rateMismatch</c> read 0 — a working amp that was silently a wire.
    ///     namamp is not in the RT gate binary and no test box carries a
    ///     capture, so the flag is pinned at the source: set in <c>prepare</c>
    ///     when a snapshot is live, cleared on publish and on clear, and folded
    ///     into the <c>rateMismatch</c> poll the UI already honours.
    /// </summary>
    [Fact]
    public void NamAmpModule_ReportsARenegotiationInFlight_ThroughRateMismatch()
    {
        var text = StripComments(File.ReadAllText(FindRepoFile(NamAmp)));

        var prepare = text.IndexOf("void prepare(double sampleRate, int maxBlockSize, int numChannels) override", StringComparison.Ordinal);
        Assert.True(prepare >= 0, "NamAmpModule.prepare moved; repoint this lint");
        var prepareBody = text[prepare..text.IndexOf("void reset() override", prepare, StringComparison.Ordinal)];
        Assert.Contains("reloading_.store(true", prepareBody);
        Assert.Contains("requestLoad(path)", prepareBody);

        var publish = text.IndexOf("loaded_.store(true, std::memory_order_release);", StringComparison.Ordinal);
        Assert.True(publish >= 0);
        Assert.Contains("reloading_.store(false", text[publish..(publish + 400)]);

        var clear = text.IndexOf("loaded_.store(false, std::memory_order_release);", StringComparison.Ordinal);
        Assert.True(clear >= 0);
        Assert.Contains("reloading_.store(false", text[clear..(clear + 400)]);

        var getter = text.IndexOf("if (!std::strcmp(id, \"rateMismatch\"))", StringComparison.Ordinal);
        Assert.True(getter >= 0);
        Assert.Contains("reloading_.load(", text[getter..(getter + 300)]);
    }

    /// <summary>Proves the lint above is reading real code and not an empty or
    /// wholly-commented file: the module's audio-thread snapshot load must
    /// survive the comment strip.</summary>
    [Fact]
    public void TheNamAmpLint_IsNotScanningAnEmptySet()
    {
        var text = StripComments(File.ReadAllText(FindRepoFile(NamAmp)));
        Assert.Contains("atomic_load_explicit(&active_", text, StringComparison.Ordinal);
        Assert.Contains("void process(", text, StringComparison.Ordinal);
    }

    /// <summary>Blanks out <c>//</c> and <c>/* */</c> comments, keeping line
    /// structure so reported line numbers still match the file.</summary>
    private static string StripComments(string source)
    {
        var noBlocks = Regex.Replace(
            source,
            "/\\*.*?\\*/",
            m => Regex.Replace(m.Value, "[^\\r\\n]", " "),
            RegexOptions.Singleline);
        return Regex.Replace(noBlocks, "//[^\\r\\n]*", string.Empty);
    }

    /// <summary>
    /// NO FUNCTION-LOCAL STATIC ON THE CALLBACK PATH.
    /// <para>
    /// A magic static is not free: the compiler wraps it in a thread-safe
    /// initialisation guard, so every call carries an atomic load of that
    /// guard, and the FIRST call takes a lock and runs the initialiser — which
    /// for <c>recordCallbackTiming</c>'s old <c>static const double perTick</c>
    /// meant a <c>QueryPerformanceFrequency</c> SYSCALL inside the first audio
    /// callback of the process. The allocation gate cannot see either half, and
    /// <c>rt_lock_sites.txt</c> cannot see a lock taken inside the compiler's
    /// own guard.
    /// </para>
    /// <para>
    /// The scope is the callback path in AudioEngine.h — the EngineCallback
    /// class and the helpers it drives — which is where every audio-thread
    /// function in this engine lives.
    /// </para>
    /// </summary>
    [Fact]
    public void NoFunctionLocalStatic_LivesOnTheAudioCallbackPath()
    {
        var h = StripComments(File.ReadAllText(FindRepoFile(EngineH)));

        // `static constexpr` is a compile-time constant with no guard and no
        // initialiser — it is the idiom this file uses for block sizes and
        // limits, and it is not what this lint is about.
        var offenders = Regex.Matches(h, @"^\s*static\s+(?!constexpr)(?:const\s+)?[A-Za-z_][\w:<>]*\s+\w+\s*(=|\{)",
                                      RegexOptions.Multiline)
            .Select(m => m.Value.Trim())
            .ToList();

        Assert.True(offenders.Count == 0,
            "AudioEngine.h carries function-local static(s) on the audio callback path — each one "
            + "is a guard check per call and a lock plus an initialiser on the first: "
            + string.Join(" | ", offenders));

        // Non-vacuity: the member that replaced the old magic static is really
        // there, so this cannot be passing because the file moved.
        Assert.Contains("perTick_", h, StringComparison.Ordinal);
    }

    /// <summary>
    /// <c>DspChain::prepare</c> takes the sizing block AND the device block,
    /// and only the second is any use for TIMING — the scratch carries 2x
    /// headroom, so the sizing figure is >= 2048 frames (~43 ms a block) and a
    /// fade wait derived from it is ~85 ms per remove / move / retag. The chain
    /// half is pinned natively (<c>tone_regressions</c>'s
    /// <c>timingBlockForTest</c>); this pins the WIRING, which nothing else
    /// would notice going missing.
    /// </summary>
    [Fact]
    public void EveryRigPrepare_PassesTheDeviceBlockForFadeTiming()
    {
        var cpp = StripComments(File.ReadAllText(FindRepoFile(EngineCpp)));
        var call = Regex.Match(cpp, @"rig\.prepare\([^)]*\)");
        Assert.True(call.Success, "no rig.prepare(...) call in AudioEngine.cpp");
        Assert.Contains("deviceBlock_", call.Value, StringComparison.Ordinal);
    }

    /// <summary>
    /// The Rubber Band engine quality is PER ENGINE.
    /// <c>dca_engine_set_stretch_quality</c> takes a handle and used to ignore
    /// it, writing a process-global <c>g_engineQuality</c> that every
    /// <c>RubberBandAudioSource</c> read at prepareToPlay — so in a host with
    /// two engines (the app's playback engine and the device-less one the take
    /// re-amp drives) one of them loading stems re-voiced the OTHER's
    /// stretchers from R3 to R2, silently and for good.
    /// <para>
    /// The behaviour is pinned natively by
    /// <c>RtStretchQualityIsPerEngineNotPerProcess</c>; this pins that the
    /// global cannot come back.
    /// </para>
    /// </summary>
    [Fact]
    public void StretchQuality_IsNotAProcessGlobal()
    {
        var cpp = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/RubberBandAudioSource.cpp")));
        var h = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/RubberBandAudioSource.h")));
        var abi = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/dotcharter_audio.cpp")));

        Assert.DoesNotContain("g_engineQuality", cpp, StringComparison.Ordinal);
        Assert.DoesNotContain("setGlobalEngineFaster", cpp, StringComparison.Ordinal);
        Assert.DoesNotContain("setGlobalEngineFaster", h, StringComparison.Ordinal);
        Assert.DoesNotContain("setGlobalEngineFaster", abi, StringComparison.Ordinal);

        // Non-vacuity: the export really resolves its handle now, so the
        // DoesNotContain assertions are not passing over a file that moved.
        Assert.Contains("setStretchFaster", abi, StringComparison.Ordinal);
        Assert.Contains("engineQuality_", cpp, StringComparison.Ordinal);
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException(
            $"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
