using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Regression tests for the JuceSharp audit fixes. Each test references the
/// audit ID it guards. Pure-managed tests run anywhere; tests touching the
/// native DLL use <see cref="NativeAvailableFactAttribute"/> so CI hosts
/// without the compiled binary still execute the suite.
/// </summary>
public class AuditRegressionTests
{
    // --- p3-48 ABI marshalling invariants ------------------------------------

    [NativeAvailableFact]
    public void p3_48_NativeAbiMatchesManagedExpectation()
    {
        Assert.Equal(NativeMethods.ExpectedAbiVersion, NativeMethods.AbiVersion());
    }

    // --- p2-18 dca_render_to_wav argument validation -------------------------

    [NativeAvailableFact]
    public void p2_18_RenderToWav_RejectsNaNTempo()
    {
        var src = TinyWav.Create(seconds: 0.05);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "naN-tempo.wav");
        try
        {
            var ex = Assert.Throws<JuceAudioException>(
                () => JuceAudioEngine.RenderToWav(src, dst, tempo: float.NaN));
            Assert.Equal(JuceAudioStatus.InvalidArg, ex.Status);
        }
        finally { TinyWav.TryDelete(src); }
    }

    [NativeAvailableFact]
    public void p2_18_RenderToWav_RejectsInfiniteSemitones()
    {
        var src = TinyWav.Create(seconds: 0.05);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "inf-semi.wav");
        try
        {
            var ex = Assert.Throws<JuceAudioException>(
                () => JuceAudioEngine.RenderToWav(src, dst, semitones: float.PositiveInfinity));
            Assert.Equal(JuceAudioStatus.InvalidArg, ex.Status);
        }
        finally { TinyWav.TryDelete(src); }
    }

    [NativeAvailableFact]
    public void p2_18_RenderToWav_RejectsOutOfRangeSampleRate()
    {
        var src = TinyWav.Create(seconds: 0.05);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "huge-sr.wav");
        try
        {
            var ex = Assert.Throws<JuceAudioException>(
                () => JuceAudioEngine.RenderToWav(src, dst, sampleRate: 5_000_000));
            Assert.Equal(JuceAudioStatus.InvalidArg, ex.Status);

            var ex2 = Assert.Throws<JuceAudioException>(
                () => JuceAudioEngine.RenderToWav(src, dst, sampleRate: -1));
            Assert.Equal(JuceAudioStatus.InvalidArg, ex2.Status);
        }
        finally { TinyWav.TryDelete(src); }
    }

    [NativeAvailableFact]
    public void p2_18_RenderToWav_AcceptsDefaultZeroSampleRate()
    {
        // sampleRate==0 means "use source default" and must still succeed.
        var src = TinyWav.Create(seconds: 0.05);
        var dst = Path.Combine(Path.GetDirectoryName(src)!, "default-sr.wav");
        try
        {
            JuceAudioEngine.RenderToWav(src, dst, tempo: 1.0f, semitones: 0.0f, sampleRate: 0);
            Assert.True(File.Exists(dst));
            Assert.True(new FileInfo(dst).Length > 44);
        }
        finally
        {
            TinyWav.TryDelete(src);
            try { File.Delete(dst); } catch { /* best-effort */ }
        }
    }

    // --- p0-5 editor lifetime token ------------------------------------------
    //
    // We can't open a real VST3 editor in CI without an actual plugin, but we
    // can verify that the dca_plugin_show_editor / close paths against an
    // invalid handle return a clean error rather than crash.

    [NativeAvailableFact]
    public void p0_5_PluginEditorShow_AgainstInvalidHandleIsSafe()
    {
        var status = NativeMethods.PluginShowEditor(nint.Zero, nint.Zero);
        // Either InvalidArg or NotInitialized is acceptable — both indicate
        // the guard rejected the bogus handle without dereferencing it.
        Assert.True(status is NativeMethods.DcaStatus.InvalidArg
                            or NativeMethods.DcaStatus.NotInitialized
                            or NativeMethods.DcaStatus.Internal,
            $"Expected guarded error, got {status}.");
    }

    // --- audit #9: a Source that outlives its Engine ---------------------------
    //
    // Before the engine-side source registry, this exact ordering was an
    // unguarded use-after-free: ~TransportSource called engine_.removeSource
    // on the already-deleted engine. The engine now orphans still-registered
    // sources at destroy time; the shell handle stays valid until
    // dca_source_destroy frees it.

    [NativeAvailableFact]
    public void audit9_SourceOutlivingEngine_IsOrphanedNotUaf()
    {
        var wav = TinyWav.Create(seconds: 0.05);
        try
        {
            var cfg = default(NativeMethods.DcaEngineConfig);
            cfg.OutputChannels = 2;
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineCreate(in cfg, out var engine));
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.SourceOpenFile(engine, wav, out var source));
            Assert.NotEqual(nint.Zero, source);

            // Destroy the ENGINE first — the historically fatal order.
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDestroy(engine));

            // The orphaned shell must answer every call safely — and, since
            // 0.48, HONESTLY. It used to report Ok + 0.0 seconds here, which
            // reads to a playhead as "the song is at the start" rather than
            // "there is no song"; every other dca_source_* export on an orphan
            // (play, stop, seek, set_volume, get_audible_position) has always
            // said InvalidArg. `pos` is still zeroed, so a caller that ignores
            // the status is no worse off than it was.
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.SourceGetPosition(source, out var pos));
            Assert.Equal(0.0, pos);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.SourceGetLength(source, out var len));
            Assert.Equal(0.0, len);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, NativeMethods.SourcePlay(source));

            // ...and free cleanly afterwards.
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.SourceDestroy(source));
        }
        finally { TinyWav.TryDelete(wav); }
    }

    [NativeAvailableFact]
    public void audit9_NormalOrder_SourceThenEngine_StillWorks()
    {
        var wav = TinyWav.Create(seconds: 0.05);
        try
        {
            var cfg = default(NativeMethods.DcaEngineConfig);
            cfg.OutputChannels = 2;
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.EngineCreate(in cfg, out var engine));
            Assert.Equal(NativeMethods.DcaStatus.Ok,
                NativeMethods.SourceOpenFile(engine, wav, out var source));

            // Registry bookkeeping must not disturb the documented order.
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.SourceDestroy(source));
            Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineDestroy(engine));
        }
        finally { TinyWav.TryDelete(wav); }
    }

    // --- drop-tune quality mode: the ABI guard outlived the two-engine era ----

    [NativeAvailableFact]
    public void dropTune_AllThreeQualityModes_AreAcceptedAtTheAbiBoundary()
    {
        var cfg = default(NativeMethods.DcaEngineConfig);
        cfg.OutputChannels = 2;
        Assert.Equal(NativeMethods.DcaStatus.Ok, NativeMethods.EngineCreate(in cfg, out var engine));

        try
        {
            // 0 Tight / 1 Balanced / 2 Smooth — three real modes since ABI v25,
            // with 10 / 20 / 35 ms splice-lag floors. The guard still validated
            // {0, 1} from the era when mode picked between two (now deleted)
            // shifter engines, so every "Smooth" push was rejected here and the
            // 35 ms path was unreachable. It failed SILENTLY because
            // JuceAudioEngine.SetInputPitchShiftMode routes the status through
            // WarnIfFailed — a shipped, selectable setting that did nothing.
            for (var mode = 0; mode <= 2; mode++)
            {
                Assert.Equal(NativeMethods.DcaStatus.Ok,
                    NativeMethods.EngineSetInputPitchShiftMode(engine, mode));
            }

            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetInputPitchShiftMode(engine, -1));
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                NativeMethods.EngineSetInputPitchShiftMode(engine, 3));
        }
        finally { NativeMethods.EngineDestroy(engine); }
    }

    // --- W5-E7 a throwing input plugin is LATCHED, and the count reaches managed ---

    /// <summary>
    /// The appended <c>input_plugin_faults</c> counter survives the whole
    /// round trip: native struct → <c>DcaXrunStats</c> → <c>XrunStats</c>.
    /// <para>
    /// The BEHAVIOUR — that a plugin which throws is skipped on the next block
    /// rather than re-entered — is pinned in the RT gate's
    /// <c>RtAThrowingInputPluginCannotKillTheCallback</c> storm phase, which is
    /// the only place a third-party plugin can be made to fault deterministically
    /// over 200 real callbacks. What is worth pinning HERE is the mapping, which
    /// is the half that silently drops a field: the projection is hand-written
    /// per property, so a new one that is not listed in <c>From</c> reads 0 for
    /// ever while every native test still passes.
    /// </para>
    /// </summary>
    [Fact]
    public void W5_E7_InputPluginFaults_SurvivesTheProjection()
    {
        var stats = XrunStats.From(new NativeMethods.DcaXrunStats { InputPluginFaults = 3 });
        Assert.Equal(3, stats.InputPluginFaults);

        // Zero is the ordinary answer and must not be confused with "absent":
        // there is no sentinel here, unlike DeviceXruns' -1.
        Assert.Equal(0, XrunStats.From(default).InputPluginFaults);
    }

    // --- W5-E9 the ASIO panel's blast radius is DOCUMENTED, not silent --------

    /// <summary>
    /// <c>dca_engine_show_asio_panel</c> is the one export that can occupy the
    /// JuceHost message thread indefinitely — a modal vendor dialog — and every
    /// other <c>callSync</c> export, <c>dca_engine_destroy</c> included, queues
    /// behind it. It is not automatable against a real driver and none of the
    /// alternatives is better (see the export's own comment), so the thing that
    /// CAN rot is the note that says so.
    /// <para>
    /// A source lint rather than a behavioural test, stated plainly: without the
    /// note, the next reader meets this as a mystery hang at shutdown and
    /// "fixes" it by unmarshalling the call, which is W4-N5 all over again.
    /// </para>
    /// </summary>
    [Fact]
    public void W5_E9_TheAsioPanelsBlockingIsDocumentedAtTheExport()
    {
        var abi = File.ReadAllText(FindRepoFile("src/JuceSharp.Native/src/dotcharter_audio.cpp"));
        var at = abi.IndexOf("dca_status DCA_CALL dca_engine_show_asio_panel", StringComparison.Ordinal);
        Assert.True(at > 0, "dca_engine_show_asio_panel is gone — repoint this lint.");

        // The comment block immediately above the definition, with the `//`
        // markers and line breaks folded away — the claim is a SENTENCE and it
        // wraps, so a raw substring match would pass or fail on where the
        // author happened to break the line.
        var comment = Regex.Replace(
            abi[Math.Max(0, at - 4000)..at].Replace("//", " ", StringComparison.Ordinal),
            @"\s+", " ");
        Assert.Contains("MESSAGE THREAD INDEFINITELY", comment, StringComparison.Ordinal);
        Assert.Contains("dca_engine_destroy", comment, StringComparison.Ordinal);
        Assert.Contains("callAsync", comment, StringComparison.Ordinal);

        // …and the guide carries the same consequence, because that is where an
        // agent looks before opening the file.
        var guide = File.ReadAllText(FindRepoFile("docs/agents/native-engine-abi.md"));
        Assert.Contains("dca_engine_show_asio_panel", guide, StringComparison.Ordinal);
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }

        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }
}
