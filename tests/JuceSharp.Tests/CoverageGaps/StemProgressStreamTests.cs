using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The progress-STREAM contract: <see cref="CoalescingProgressStream"/> and the
/// <see cref="IStemSeparator.SeparateWithProgressAsync"/> surface every backend
/// exposes on top of it.
///
/// <para>
/// Every test here is deterministic by construction — a gate the producer and
/// the consumer hand to each other — because the thing under test is a race
/// and a wall-clock assertion on a race flakes the moment the suite runs in
/// parallel. Nothing here sleeps.
/// </para>
/// </summary>
public sealed class StemProgressStreamTests
{
    // ---------------------------------------------- CoalescingProgressStream

    [Fact]
    public async Task Every_published_sample_arrives_in_order()
    {
        var stream = CoalescingProgressStream.Create<int>((report, _) =>
        {
            for (var i = 0; i < 10; i++) report(i);
            return Task.CompletedTask;
        }, ct: TestContext.Current.CancellationToken);

        var seen = new List<int>();
        await foreach (var i in stream.WithCancellation(TestContext.Current.CancellationToken))
            seen.Add(i);

        Assert.Equal(Enumerable.Range(0, 10), seen);
    }

    /// <summary>
    /// The work must not start until somebody enumerates — otherwise merely
    /// BUILDING a stream (to hand it to a view-model, say) would fire off a
    /// multi-gigabyte inference run.
    /// </summary>
    [Fact]
    public async Task Building_the_stream_does_not_start_the_work()
    {
        var started = 0;
        var stream = CoalescingProgressStream.Create<int>((report, _) =>
        {
            Interlocked.Increment(ref started);
            report(1);
            return Task.CompletedTask;
        }, ct: TestContext.Current.CancellationToken);

        Assert.Equal(0, Volatile.Read(ref started));

        await foreach (var _ in stream.WithCancellation(TestContext.Current.CancellationToken)) { }

        Assert.Equal(1, Volatile.Read(ref started));
    }

    /// <summary>
    /// A consumer that stops reading must not stall the producer, and the
    /// samples it loses must be the OLDEST ones — a UI wants the newest
    /// fraction, not a backlog of stale ones.
    ///
    /// <para>
    /// Deterministic without a clock: the consumer takes sample 0, then holds
    /// the <c>await foreach</c> BODY open (so no read is pending) while the
    /// producer bursts 99 more samples plus a terminal one into a queue of
    /// four, and only resumes once the producer signals it is done. The queue
    /// can then hold exactly the last four writes.
    /// </para>
    /// </summary>
    [Fact]
    public async Task A_stalled_consumer_loses_the_oldest_samples_and_never_blocks_the_producer()
    {
        const int capacity = 4;
        const int burst = 100;

        var consumerStalled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var producerDone = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var stream = CoalescingProgressStream.Create<int>(async (report, _) =>
        {
            report(-1);                                   // the sample that opens the gate
            await consumerStalled.Task.ConfigureAwait(false);
            for (var i = 0; i < burst; i++) report(i);    // never blocks, whatever the consumer does
            producerDone.SetResult();
        }, capacity, TestContext.Current.CancellationToken);

        var seen = new List<int>();
        await foreach (var i in stream.WithCancellation(TestContext.Current.CancellationToken))
        {
            seen.Add(i);
            if (i == -1)
            {
                consumerStalled.SetResult();
                // Still INSIDE the foreach body: no read is pending, so every
                // one of the producer's writes lands in (or falls out of) the
                // queue rather than being handed straight to a waiting reader.
                await producerDone.Task;
            }
        }

        // The producer ran to completion regardless of the stall — that is the
        // "a slow UI never stalls inference" half.
        Assert.True(producerDone.Task.IsCompletedSuccessfully);
        // And the survivors are the NEWEST four, not the oldest four.
        Assert.Equal(new[] { -1, burst - 4, burst - 3, burst - 2, burst - 1 }, seen);
    }

    /// <summary>
    /// Non-vacuity for the test above: run the identical scenario with a queue
    /// big enough for the burst and NOTHING is dropped. If the coalescing were
    /// silently disabled, that test would look exactly like this one, so this
    /// is what tells the two apart.
    /// </summary>
    [Fact]
    public async Task The_same_scenario_with_a_big_enough_queue_drops_nothing()
    {
        const int burst = 100;

        var consumerStalled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var producerDone = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var stream = CoalescingProgressStream.Create<int>(async (report, _) =>
        {
            report(-1);
            await consumerStalled.Task.ConfigureAwait(false);
            for (var i = 0; i < burst; i++) report(i);
            producerDone.SetResult();
        }, burst + 1, TestContext.Current.CancellationToken);

        var seen = new List<int>();
        await foreach (var i in stream.WithCancellation(TestContext.Current.CancellationToken))
        {
            seen.Add(i);
            if (i == -1) { consumerStalled.SetResult(); await producerDone.Task; }
        }

        Assert.Equal(burst + 1, seen.Count);
        Assert.Equal(Enumerable.Range(0, burst), seen.Skip(1));
    }

    /// <summary>
    /// The terminal item is the whole reason coalescing is safe: it is the
    /// newest write, and drop-OLDEST can never evict the newest. Burst far past
    /// the queue depth and it still arrives, still carrying its payload.
    /// </summary>
    [Fact]
    public async Task The_terminal_item_survives_a_queue_that_overflowed_many_times()
    {
        var result = new SeparationResult("in.wav", "out", "htdemucs", "cpu",
            [new StemFile("vocals", "out/vocals.wav")]);

        var consumerStalled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var producerDone = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var stream = CoalescingProgressStream.Create<StemSeparationProgress>(async (report, _) =>
        {
            report(new StemSeparationProgress("inference", 0, "start", null));
            await consumerStalled.Task.ConfigureAwait(false);
            for (var i = 0; i < 5_000; i++)
                report(new StemSeparationProgress("inference", i / 5000.0, "tick", null));
            report(new StemSeparationProgress("done", 1.0, "Separation complete.", null, "fake", result));
            producerDone.SetResult();
        }, capacity: 2, ct: TestContext.Current.CancellationToken);

        StemSeparationProgress last = default;
        await foreach (var p in stream.WithCancellation(TestContext.Current.CancellationToken))
        {
            last = p;
            if (p.Message == "start") { consumerStalled.SetResult(); await producerDone.Task; }
        }

        Assert.Equal("done", last.Phase);
        Assert.Same(result, last.Result);
    }

    /// <summary>
    /// A backend failure must surface with its OWN type. Completing the channel
    /// with the exception would hand the consumer a
    /// <c>ChannelClosedException</c> and bury the real one in
    /// <see cref="Exception.InnerException"/> — and a host that catches
    /// <see cref="StemSeparationVramException"/> to offer "retry on CPU" would
    /// never fire.
    /// </summary>
    [Fact]
    public async Task A_backend_fault_surfaces_with_its_original_exception_type()
    {
        var stream = CoalescingProgressStream.Create<int>((report, _) =>
        {
            report(1);
            throw new StemSeparationVramException("htdemucs", "cuda", "CUDA out of memory");
        }, ct: TestContext.Current.CancellationToken);

        var seen = new List<int>();
        var ex = await Assert.ThrowsAsync<StemSeparationVramException>(async () =>
        {
            await foreach (var i in stream.WithCancellation(TestContext.Current.CancellationToken))
                seen.Add(i);
        });

        // The samples published BEFORE the fault are still delivered: the
        // reader drains, then re-throws.
        Assert.Equal([1], seen);
        Assert.Contains("CUDA out of memory", ex.Message, StringComparison.Ordinal);
    }

    /// <summary>A fault raised before anything was published must still fault
    /// the enumeration rather than look like an empty stream.</summary>
    [Fact]
    public async Task A_fault_before_the_first_sample_is_not_an_empty_stream()
    {
        var stream = CoalescingProgressStream.Create<int>(
            (_, _) => throw new InvalidDataException("bad model"),
            ct: TestContext.Current.CancellationToken);

        await Assert.ThrowsAsync<InvalidDataException>(async () =>
        {
            await foreach (var _ in stream.WithCancellation(TestContext.Current.CancellationToken)) { }
        });
    }

    [Fact]
    public async Task Cancelling_the_enumeration_surfaces_a_cancellation_and_not_a_failure()
    {
        using var cts = new CancellationTokenSource();
        var running = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var stream = CoalescingProgressStream.Create<int>(async (report, token) =>
        {
            report(0);
            running.SetResult();
            await Task.Delay(Timeout.Infinite, token).ConfigureAwait(false);
        }, ct: cts.Token);

        var ex = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
        {
            await foreach (var _ in stream)
                await cts.CancelAsync();
        });

        Assert.IsNotType<StemSeparationException>(ex);
    }

    /// <summary>
    /// Walking away from the stream has to stop the backend, or a user who
    /// closes the dialog leaves a multi-gigabyte inference running.
    /// </summary>
    [Fact]
    public async Task Abandoning_the_enumeration_cancels_the_work()
    {
        var cancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        var stream = CoalescingProgressStream.Create<int>(async (report, token) =>
        {
            report(0);
            try { await Task.Delay(Timeout.Infinite, token).ConfigureAwait(false); }
            catch (OperationCanceledException) { cancelled.SetResult(); throw; }
        }, ct: TestContext.Current.CancellationToken);

        await foreach (var _ in stream.WithCancellation(TestContext.Current.CancellationToken))
            break;      // disposes the enumerator

        await cancelled.Task;   // would hang forever if the break did not cancel
        Assert.True(cancelled.Task.IsCompletedSuccessfully);
    }

    // ------------------------------------ IStemSeparator default implementation

    /// <summary>
    /// A third-party backend that implements nothing but
    /// <see cref="IStemSeparator.SeparateTrackAsync"/> still gets the stream,
    /// with the samples in order, the backend id stamped, and the result on the
    /// terminal item.
    /// </summary>
    [Fact]
    public async Task The_interface_default_bridges_a_push_only_backend()
    {
        IStemSeparator sep = new PushOnlySeparator();

        var seen = new List<StemSeparationProgress>();
        await foreach (var p in sep.SeparateWithProgressAsync(
                           "song.wav", "out", new StemSeparationOptions { Model = "htdemucs_6s" },
                           TestContext.Current.CancellationToken))
            seen.Add(p);

        Assert.Equal(["inference", "stem", "done"], seen.Select(p => p.Phase));
        Assert.All(seen, p => Assert.Equal(nameof(PushOnlySeparator), p.Backend));

        // Exactly one sample carries the result, and it is the last one.
        Assert.Single(seen, p => p.Result is not null);
        Assert.Equal("htdemucs_6s", seen[^1].Result!.Model);
        Assert.Equal("vocals", Assert.Single(seen[^1].Result!.Stems).Name);
    }

    [Fact]
    public async Task The_interface_default_keeps_a_backend_fault_typed()
    {
        IStemSeparator sep = new PushOnlySeparator(
            fail: () => new StemSeparationVramException("htdemucs", "cuda", "out of VRAM"));

        await Assert.ThrowsAsync<StemSeparationVramException>(async () =>
        {
            await foreach (var _ in sep.SeparateWithProgressAsync(
                               "song.wav", "out", ct: TestContext.Current.CancellationToken)) { }
        });
    }

    [Fact]
    public async Task The_interface_default_reports_a_cancel_as_a_cancel()
    {
        IStemSeparator sep = new PushOnlySeparator();
        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        var ex = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
        {
            await foreach (var _ in sep.SeparateWithProgressAsync("song.wav", "out", ct: cts.Token)) { }
        });
        Assert.IsNotType<StemSeparationException>(ex);
    }

    /// <summary>A backend that reports nothing still produces the terminal
    /// item, so a consumer can always get at the result.</summary>
    [Fact]
    public async Task A_silent_backend_still_produces_the_terminal_item()
    {
        IStemSeparator sep = new PushOnlySeparator(silent: true);

        var seen = new List<StemSeparationProgress>();
        await foreach (var p in sep.SeparateWithProgressAsync(
                           "song.wav", "out", ct: TestContext.Current.CancellationToken))
            seen.Add(p);

        var only = Assert.Single(seen);
        Assert.Equal("done", only.Phase);
        Assert.NotNull(only.Result);
    }

    /// <summary>
    /// A push-only <see cref="IStemSeparator"/>: it implements the ORIGINAL
    /// three members and nothing else, which is the exact shape the default
    /// interface method exists to serve.
    /// </summary>
    private sealed class PushOnlySeparator(Func<Exception>? fail = null, bool silent = false) : IStemSeparator
    {
        public bool IsReady() => true;

        public PythonEnvStatus Probe() => new(true, true, true, "fake-python", "3.11.9");

        public Task<SeparationResult> SeparateTrackAsync(
            string inputPath, string outputDir, StemSeparationOptions? options = null,
            IProgress<StemSeparationProgress>? progress = null, CancellationToken ct = default)
        {
            ct.ThrowIfCancellationRequested();
            if (fail is not null) throw fail();
            var opts = options ?? StemSeparationOptions.Default;
            if (!silent)
            {
                progress?.Report(new StemSeparationProgress("inference", 0.5, "half", null));
                progress?.Report(new StemSeparationProgress("stem", -1, "wrote vocals", "vocals"));
            }
            return Task.FromResult(new SeparationResult(inputPath, outputDir, opts.Model, opts.Device,
                [new StemFile("vocals", Path.Combine(outputDir, "vocals.wav"))]));
        }
    }
}

/// <summary>
/// The same contract on the REAL <see cref="StemSeparator"/>, as far as it can
/// be driven without an embedded interpreter: the stream must fault with the
/// backend's own exception and must treat a cancel as a cancel. Both faults are
/// raised by the production code path, not by a fake.
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed class StemSeparatorProgressStreamTests
{
    /// <summary>
    /// Validation happens when the stream is ENUMERATED, not when it is built —
    /// and it surfaces as the original <see cref="FileNotFoundException"/>,
    /// which is what the diagnostics CLI's ENVIRONMENT-FAILED arm matches on.
    /// </summary>
    [Fact]
    public async Task A_missing_input_faults_the_stream_with_the_original_exception()
    {
        var sep = new StemSeparator();
        var missing = Path.Combine(Path.GetTempPath(), $"no-such-{Guid.NewGuid():N}.wav");

        // Building it must not throw — the fault belongs to MoveNextAsync.
        var stream = sep.SeparateWithProgressAsync(missing, Path.GetTempPath(),
            ct: TestContext.Current.CancellationToken);

        var ex = await Assert.ThrowsAsync<FileNotFoundException>(async () =>
        {
            await foreach (var _ in stream.WithCancellation(TestContext.Current.CancellationToken)) { }
        });
        Assert.Equal(missing, ex.FileName);
    }

    /// <summary>
    /// A pre-cancelled run is an <see cref="OperationCanceledException"/> out of
    /// the enumeration and NEVER a <see cref="StemSeparationException"/> — the
    /// standing cancel contract, now also through the pull surface.
    /// </summary>
    [Fact]
    public async Task A_cancelled_run_is_a_cancellation_and_not_a_separation_failure()
    {
        var dir = Path.Combine(Path.GetTempPath(), $"jsstream-{Guid.NewGuid():N}");
        Directory.CreateDirectory(dir);
        var input = Path.Combine(dir, "song.wav");
        await File.WriteAllBytesAsync(input, [0, 0, 0, 0], TestContext.Current.CancellationToken);
        // A script that EXISTS, so the run reaches the cancellation check
        // instead of stopping at "bundled separator script not found".
        var script = Path.Combine(dir, "separate_track.py");
        await File.WriteAllTextAsync(script, "# stub", TestContext.Current.CancellationToken);

        try
        {
            var sep = new StemSeparator(scriptPathOverride: script);
            using var cts = new CancellationTokenSource();
            await cts.CancelAsync();

            var ex = await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
            {
                await foreach (var _ in sep.SeparateWithProgressAsync(
                                   input, Path.Combine(dir, "out"), ct: cts.Token)) { }
            });
            Assert.IsNotType<StemSeparationException>(ex);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>The backend id is a real constant a host can compare against,
    /// and the terminal helper stamps it.</summary>
    [Fact]
    public void The_backend_id_is_stable()
    {
        Assert.Equal("python-sidecar", StemSeparator.BackendId);
        Assert.Equal("demucs.cpp", JuceStemSeparation.BackendId);
        Assert.Equal("onnx", JuceSharp.StemSeparation.Onnx.OnnxStemSeparator.BackendId);
    }
}
