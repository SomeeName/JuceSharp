using JuceSharp.StemSeparation.Onnx;
using JuceSharp.StemSeparation.Processing;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// The progress-stream surface on the two backends that are NOT
/// <see cref="IStemSeparator"/>: the in-process ONNX one and the native
/// demucs.cpp façade. Both sit on <see cref="CoalescingProgressStream"/>, so
/// what is proved here is the per-backend WIRING — the phases, the terminal
/// payload, the backend id, and that a fault keeps its type through the
/// backend's own code path.
/// <para>
/// No model file and no <c>juce_sharp.dll</c> is required: the ONNX tests go
/// through the internal runner-injecting constructor (the same seam the
/// dispose-race tests use), and the native test asserts the capability answer.
/// </para>
/// </summary>
public sealed class BackendProgressStreamTests
{
    private static readonly float[] Stereo = new float[8];

    private static IReadOnlyList<OnnxStemBuffer> TwoStems(int rate) =>
    [
        new OnnxStemBuffer("vocals", new float[8], rate),
        new OnnxStemBuffer("other", new float[8], rate),
    ];

    [Fact]
    public async Task The_onnx_stream_reports_load_inference_every_stem_and_a_terminal_result()
    {
        using var separator = new OnnxStemSeparator(
            runner: (_, rate, progress, _) =>
            {
                progress?.Report(0.25);
                progress?.Report(0.75);
                return TwoStems(rate);
            },
            releaseNative: () => { });

        var seen = new List<OnnxStemProgress>();
        await foreach (var p in separator.SeparateWithProgressAsync(
                           Stereo, 44100, TestContext.Current.CancellationToken))
            seen.Add(p);

        Assert.Equal(["loading", "inference", "inference", "stem", "stem", "done"],
            seen.Select(p => p.Phase));
        Assert.All(seen, p => Assert.Equal(OnnxStemSeparator.BackendId, p.Backend));
        Assert.Equal([0.25, 0.75], seen.Where(p => p.Phase == "inference").Select(p => p.Fraction));
        Assert.Equal(["vocals", "other"], seen.Where(p => p.Phase == "stem").Select(p => p.Stem));

        // Exactly one sample carries the buffers, and it is the last.
        var terminal = Assert.Single(seen, p => p.Result is not null);
        Assert.Same(seen[^1].Result, terminal.Result);
        Assert.Equal(["vocals", "other"], terminal.Result!.Select(b => b.Name));
    }

    /// <summary>
    /// A driver blowing up mid-inference must reach the consumer as the
    /// driver's own exception — this is the branch a host uses to fall back
    /// from GPU to CPU.
    /// </summary>
    [Fact]
    public async Task An_onnx_driver_fault_keeps_its_type_through_the_stream()
    {
        using var separator = new OnnxStemSeparator(
            runner: (_, _, _, _) => throw new OnnxRuntimeTestException("DML allocation failed"),
            releaseNative: () => { });

        var ex = await Assert.ThrowsAsync<OnnxRuntimeTestException>(async () =>
        {
            await foreach (var _ in separator.SeparateWithProgressAsync(
                               Stereo, 44100, TestContext.Current.CancellationToken)) { }
        });
        Assert.Equal("DML allocation failed", ex.Message);
    }

    [Fact]
    public async Task A_cancelled_onnx_stream_is_a_cancellation()
    {
        using var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) => TwoStems(rate),
            releaseNative: () => { });

        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
        {
            await foreach (var _ in separator.SeparateWithProgressAsync(Stereo, 44100, cts.Token)) { }
        });
    }

    /// <summary>
    /// The stream honours the SAME <see cref="InferenceGuard"/> as the push
    /// API: a disposed separator is an <see cref="ObjectDisposedException"/>,
    /// not a silently empty stream.
    /// </summary>
    [Fact]
    public async Task A_disposed_onnx_separator_faults_the_stream()
    {
        var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) => TwoStems(rate),
            releaseNative: () => { });
        separator.Dispose();

        await Assert.ThrowsAsync<ObjectDisposedException>(async () =>
        {
            await foreach (var _ in separator.SeparateWithProgressAsync(
                               Stereo, 44100, TestContext.Current.CancellationToken)) { }
        });
    }

    /// <summary>
    /// Argument validation is a FAULT ON THE STREAM, not a throw from the
    /// factory call — a caller that stores the stream and enumerates it later
    /// must still see the error.
    /// </summary>
    [Fact]
    public async Task A_bad_onnx_input_faults_the_stream_rather_than_the_call()
    {
        using var separator = new OnnxStemSeparator(
            runner: (_, rate, _, _) => TwoStems(rate),
            releaseNative: () => { });

        var stream = separator.SeparateWithProgressAsync(
            new float[3], 44100, TestContext.Current.CancellationToken);   // odd length = not stereo

        await Assert.ThrowsAsync<ArgumentException>(async () =>
        {
            await foreach (var _ in stream.WithCancellation(TestContext.Current.CancellationToken)) { }
        });
    }

    /// <summary>
    /// The native façade's capability answer must survive the stream: an
    /// engine built without demucs.cpp reports
    /// <see cref="NotSupportedException"/> — the type a composite service
    /// branches on to try the next backend — rather than a generic failure or
    /// an empty stream.
    /// </summary>
    [Fact]
    public async Task The_native_stream_reports_an_unsupported_build_as_a_capability_answer()
    {
        if (JuceStemSeparation.IsSupported)
            Assert.Skip("This build has the demucs.cpp backend; the unsupported branch is unreachable here.");

        await Assert.ThrowsAsync<NotSupportedException>(async () =>
        {
            await foreach (var _ in JuceStemSeparation.SeparateWithProgressAsync(
                               "song.wav", "out", "model.bin",
                               cancellationToken: TestContext.Current.CancellationToken)) { }
        });
    }

    [Fact]
    public async Task A_pre_cancelled_native_stream_never_calls_into_native()
    {
        using var cts = new CancellationTokenSource();
        await cts.CancelAsync();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(async () =>
        {
            await foreach (var _ in JuceStemSeparation.SeparateWithProgressAsync(
                               "song.wav", "out", "model.bin", cancellationToken: cts.Token)) { }
        });
    }
}

/// <summary>A driver-specific exception type, so "the ORIGINAL type survives"
/// is asserted against a type nothing in the plumbing knows about.</summary>
public sealed class OnnxRuntimeTestException(string message) : Exception(message);

/// <summary>
/// W4-C2. <see cref="CoalescingProgressStream.DefaultCapacity" />'s doc said the
/// depth existed so a stalled consumer "does not lose the phase transitions that
/// happened during the stall, only the fractions". It cannot:
/// <see cref="System.Threading.Channels.BoundedChannelFullMode.DropOldest" />
/// drops strictly by AGE and knows nothing about an item's content. The test
/// below documents the REAL behaviour, which is the only honest thing to pin —
/// the fix was the sentence, and this is what makes it stay fixed.
/// </summary>
public sealed class ProgressStreamDropOldestTests
{
    private readonly record struct Sample(string Phase, float Fraction);

    /// <summary>
    ///     A reader that does not start reading until the producer has finished
    ///     is the stall, expressed without a clock: the writes all happen first,
    ///     so the drops are decided by capacity alone.
    ///     <para>
    ///         40 samples into a capacity-4 stream with the phase change at index
    ///         3 — the change is gone, and the terminal item is not.
    ///     </para>
    /// </summary>
    [Fact]
    public async Task DropOldest_EvictsAPhaseTransitionAsReadilyAsAFraction()
    {
        var produced = new TaskCompletionSource();

        var stream = CoalescingProgressStream.Create<Sample>((report, _) =>
        {
            for (var i = 0; i < 40; i++)
                report(new Sample(i < 3 ? "loading" : "inference", i / 39f));
            produced.SetResult();
            return Task.CompletedTask;
        }, capacity: 4, ct: TestContext.Current.CancellationToken);

        var seen = new List<Sample>();
        await foreach (var s in stream) seen.Add(s);

        await produced.Task;

        Assert.Equal(4, seen.Count);
        // The oldest four survivors are the NEWEST four written, so nothing from
        // the "loading" phase — nor the first "inference" sample, which is the
        // transition itself — is in the stream.
        Assert.DoesNotContain(seen, s => s.Phase == "loading");
        Assert.Equal(39 / 39f, seen[^1].Fraction);   // the terminal item DOES survive
    }

    /// <summary>…and with a capacity that covers the burst nothing is dropped at
    /// all, so the test above is measuring the mode and not a bug in the
    /// stream.</summary>
    [Fact]
    public async Task WithEnoughDepth_NothingIsDropped()
    {
        var stream = CoalescingProgressStream.Create<Sample>((report, _) =>
        {
            for (var i = 0; i < 40; i++)
                report(new Sample(i < 3 ? "loading" : "inference", i / 39f));
            return Task.CompletedTask;
        }, capacity: 64, ct: TestContext.Current.CancellationToken);

        var seen = new List<Sample>();
        await foreach (var s in stream) seen.Add(s);

        Assert.Equal(40, seen.Count);
        Assert.Contains(seen, s => s.Phase == "loading");
    }
}
