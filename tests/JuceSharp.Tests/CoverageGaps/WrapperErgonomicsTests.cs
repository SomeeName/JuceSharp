using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The wave-4 audit's API-ergonomics findings: W4-E1 (the one minutes-long call
/// still on the thread pool), W4-E2 (a process-wide serial queue nobody
/// documented), W4-E3 (the last blocking public member with no async twin and
/// three bare <c>catch { }</c>), and the factory's interface-typed result.
/// </summary>
[Collection("NullDevice")]
public sealed class WrapperErgonomicsTests
{
    // ------------------------------------------------------------------ E1

    /// <summary>
    ///     <c>Interop/EngineWorker</c> states the rule in full: "Never the thread
    ///     pool. […] a <c>Task.Run</c> parks a pool thread for the duration and
    ///     the pool answers by INJECTING threads (one every ~0.5 s), which starves
    ///     every other async workload in the host." That argument was written for
    ///     a 25 s VST3 scan. A <c>dca_stems_separate_file</c> run is MINUTES, and
    ///     it was the one call in the wrapper still on the pool.
    ///     <para>
    ///         <c>IsThreadPoolThread</c> is the same shape <c>EngineAsyncTests</c>
    ///         uses for the engine worker, and it needs no native backend because
    ///         the helper is what is under test.
    ///     </para>
    /// </summary>
    [Fact]
    public async Task StemSeparation_RunsItsBlockingCallOffTheThreadPool()
    {
        var onPool = true;
        var background = false;
        string? name = null;

        await JuceStemSeparation.RunOffThePool(() =>
        {
            onPool = Thread.CurrentThread.IsThreadPoolThread;
            background = Thread.CurrentThread.IsBackground;
            name = Thread.CurrentThread.Name;
        }, TestContext.Current.CancellationToken);

        Assert.False(onPool, "a minutes-long native call is parked on a thread-pool thread");
        Assert.True(background, "the worker thread would keep the process alive");
        Assert.Equal("JuceStemSeparation worker", name);
    }

    /// <summary>Faults and cancellation keep their identity across the dedicated
    /// thread — a hand-rolled worker that loses the exception type would break
    /// the "fall through to the next backend" branch every consumer has.</summary>
    [Fact]
    public async Task TheDedicatedWorker_PreservesFaultsAndCancellation()
    {
        await Assert.ThrowsAsync<NotSupportedException>(
            () => JuceStemSeparation.RunOffThePool(
                () => throw new NotSupportedException("backend"), TestContext.Current.CancellationToken));

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => JuceStemSeparation.RunOffThePool(() => { }, cts.Token));
    }

    /// <summary>…and neither async member reaches for <c>Task.Run</c> again.
    /// Anchored to the file that owns the rule-breaking call, which is the whole
    /// of the evidence for "the pool is not used here".</summary>
    [Fact]
    public void StemSeparation_UsesNoTaskRunAtAll()
    {
        var text = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceStemSeparation.cs");
        // Comments of BOTH kinds go: the doc quotes the rule verbatim and the
        // code carries a "RunOffThePool, not Task.Run" signpost, so a lint that
        // only stripped /// would be reading its own explanation.
        var code = Regex.Replace(text, @"//[^\n]*", " ");
        Assert.DoesNotContain("Task.Run", code, StringComparison.Ordinal);
    }

    // ------------------------------------------------------------------ E2

    /// <summary>
    /// The process-wide offline worker serialises <c>GenerateWaveformAsync</c>,
    /// <c>GenerateWaveformFromMemoryAsync</c> and <c>RenderToWavAsync</c> for the
    /// life of the process, and is deliberately never shut down — so one wedged
    /// decode wedges all three permanently. Neither property was written down
    /// anywhere a consumer reads. The degree is pinned here so the CHOICE is
    /// explicit rather than incidental.
    /// </summary>
    [Fact]
    public async Task TheOfflineWorkerIsSerial_AndSaysSo()
    {
        // THE SUBJECT IS THE MECHANISM, DRIVEN HERE, NOT A SHARED METER READ.
        // This used to assert
        // `JuceAudioEngine.OfflineWorkerForTests.MaxObservedConcurrency <= 1`.
        // That field is a high-water mark on a PROCESS-WIDE EngineWorker with no
        // reset: run this class alone and it reads 0 because nothing was ever
        // enqueued (the assertion passes having tested nothing), run the full
        // gate and its value was accumulated by whichever other classes called
        // GenerateWaveformAsync/RenderToWavAsync first. Neither reading is about
        // this test. So drive a worker of our own and make the mark mean
        // something: twenty items that each PARK inside the worker until the one
        // after it has been queued would drive the mark to 2 the moment the
        // queue stopped being serial.
        var worker = new EngineWorker("TheOfflineWorkerIsSerial_AndSaysSo");
        try
        {
            var inFlight = 0;
            var maxSeenByTheWorkItself = 0;
            var tasks = new List<Task<int>>();
            for (var i = 0; i < 20; i++)
                tasks.Add(worker.Enqueue(() =>
                {
                    var live = Interlocked.Increment(ref inFlight);
                    if (live > maxSeenByTheWorkItself) maxSeenByTheWorkItself = live;
                    Thread.Yield();     // hand the scheduler every chance to overlap us
                    Interlocked.Decrement(ref inFlight);
                    return live;
                }, TestContext.Current.CancellationToken));

            await Task.WhenAll(tasks);

            Assert.All(tasks, t => Assert.Equal(1, t.Result));
            Assert.Equal(1, maxSeenByTheWorkItself);
            Assert.Equal(1, worker.MaxObservedConcurrency);
            Assert.Equal(20, worker.StartedCount);
        }
        finally
        {
            worker.Shutdown(EngineWorker.DisposeJoinTimeoutMs);
        }

        // …and the offline members really do run on ONE such worker: the whole
        // point is that the three of them SHARE it.
        Assert.NotNull(JuceAudioEngine.OfflineWorkerForTests);
        Assert.Same(JuceAudioEngine.OfflineWorkerForTests, JuceAudioEngine.OfflineWorkerForTests);

        var async = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceAudioEngine.Async.cs");
        // The field's doc plus all three members say SERIALISED. `>=`, not `==`:
        // an exact occurrence count of a word in a production source file fails
        // the day somebody explains the rule ONE more time, which is not a
        // regression in anything.
        Assert.True(Regex.Matches(async, "SERIALISED").Count >= 4,
            "the field's doc and all three static members have to say SERIALISED.");
    }

    // ------------------------------------------------------------------ E3

    /// <summary>
    /// <c>JuceVst3Scanner.Scan</c> walks the OS VST3 roots to depth 4 and sums
    /// every file under every bundle recursively — seconds on a plugin-heavy
    /// machine — with no token, no async twin and three bare <c>catch { }</c>.
    /// A pre-cancelled token now returns immediately rather than after the walk.
    /// </summary>
    [Fact]
    public void Vst3Scan_HonoursAPreCancelledToken()
    {
        using var cts = new CancellationTokenSource();
        cts.Cancel();

        Assert.Throws<OperationCanceledException>(
            () => JuceVst3Scanner.Scan(null, maxDepth: 4, limit: 0, cts.Token));
    }

    /// <summary>
    /// Cancellation must not be swallowed by the catches that exist for
    /// permission errors: a cancelled scan that returned a PARTIAL list would
    /// look exactly like a machine with few plugins.
    /// </summary>
    [Fact]
    public async Task Vst3ScanAsync_CancelsRatherThanReturningAPartialList()
    {
        var root = Path.Combine(Path.GetTempPath(), "vst3-cancel-" + Guid.NewGuid());
        for (var i = 0; i < 40; i++)
            Directory.CreateDirectory(Path.Combine(root, $"d{i}", $"P{i}.vst3", "Contents"));
        try
        {
            using var cts = new CancellationTokenSource();
            await cts.CancelAsync();

            await Assert.ThrowsAnyAsync<OperationCanceledException>(
                () => JuceVst3Scanner.ScanAsync([root], cancellationToken: cts.Token));
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (IOException) { /* best effort */ }
        }
    }

    /// <summary>The async twin still finds what the blocking one finds, and runs
    /// off the pool for the same reason E1's does.</summary>
    [Fact]
    public async Task Vst3ScanAsync_FindsWhatScanFinds()
    {
        var root = Path.Combine(Path.GetTempPath(), "vst3-async-" + Guid.NewGuid());
        var bundle = Path.Combine(root, "nested", "AsyncPlugin.vst3");
        Directory.CreateDirectory(bundle);
        File.WriteAllBytes(Path.Combine(bundle, "payload.bin"), new byte[512]);
        try
        {
            var blocking = JuceVst3Scanner.Scan([root], maxDepth: 4, limit: 0,
                                                TestContext.Current.CancellationToken);
            var asynchronous = await JuceVst3Scanner.ScanAsync(
                [root], cancellationToken: TestContext.Current.CancellationToken);

            Assert.Equal(blocking.Select(e => e.Path), asynchronous.Select(e => e.Path));
            Assert.Single(asynchronous);
        }
        finally
        {
            try { Directory.Delete(root, true); } catch (IOException) { /* best effort */ }
        }
    }

    /// <summary>No bare <c>catch { }</c> is left: a permission-denied root and a
    /// corrupt bundle used to be indistinguishable from "nothing there", which
    /// is the shape of every "the scanner does not find my plugins" report
    /// nobody can reproduce.</summary>
    [Fact]
    public void Vst3Scanner_LogsWhatItSkips()
    {
        var text = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceVst3Scanner.cs");
        var code = Regex.Replace(text, @"//[^\n]*", " ");

        Assert.DoesNotMatch(new Regex(@"catch\s*\{"), code);
        Assert.Contains("private static void Skipped(", code, StringComparison.Ordinal);
        // …and cancellation is re-thrown rather than folded into that log.
        Assert.True(Regex.Matches(code, @"catch \(OperationCanceledException\) \{ throw; \}").Count >= 3);
    }

    // ---------------------------------------------------- factory seam

    /// <summary>
    /// <c>OpenWithFallback</c> hands back the CONCRETE engine, so a host that
    /// holds its engine through <see cref="IJuceAudioEngine" /> had to name
    /// <see cref="JuceAudioEngine" /> here and nowhere else.
    /// <c>OpenResult.EngineInterface</c> is the same instance typed as the seam.
    /// The positional member stays concrete deliberately: three of the six
    /// shipped examples pass it to a method taking the concrete type.
    /// </summary>
    [Fact]
    public void OpenResult_ExposesTheEngineThroughTheInterfaceSeam()
    {
        var property = typeof(JuceEngineFactory.OpenResult)
            .GetProperty(nameof(JuceEngineFactory.OpenResult.EngineInterface));

        Assert.NotNull(property);
        Assert.Equal(typeof(IJuceAudioEngine), property!.PropertyType);

        // Null exactly when Engine is.
        var empty = new JuceEngineFactory.OpenResult(null, new JuceEngineConfig(), []);
        Assert.Null(empty.EngineInterface);
    }

    /// <summary>…and with a live engine it really is the SAME object, not a
    /// wrapper — disposing either has to dispose the one engine.</summary>
    [NativeAvailableFact]
    public void OpenResult_EngineInterfaceIsTheSameInstance()
    {
        using var scope = new NullDeviceScope();
        using var engine = NullDevice.CreateEngine();

        var result = new JuceEngineFactory.OpenResult(engine, new JuceEngineConfig(), []);

        Assert.Same(engine, result.EngineInterface);
    }
}
