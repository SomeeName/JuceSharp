namespace JuceSharp.StemSeparation.Processing;

/// <summary>
/// Abstraction over an audio stem-separation backend. The default
/// implementation is <see cref="StemSeparator"/>, which drives the
/// bundled Python sidecar; alternative implementations (e.g. an
/// in-process ONNX backend, a fake for unit tests) can be plugged in via
/// DI.
/// </summary>
public interface IStemSeparator
{
    /// <summary>
    /// Returns <c>true</c> when the backend is fully initialised and
    /// ready to process a track without further bootstrap work (e.g. the
    /// embedded interpreter and required packages are installed).
    /// </summary>
    bool IsReady();

    /// <summary>Probe the Python environment used by the backend.</summary>
    Runtime.PythonEnvStatus Probe();

    /// <summary>
    /// Run separation on <paramref name="inputPath"/>, writing per-stem
    /// audio files into <paramref name="outputDir"/>. The returned
    /// <see cref="SeparationResult"/> lists the produced files.
    /// </summary>
    Task<SeparationResult> SeparateTrackAsync(
        string inputPath,
        string outputDir,
        StemSeparationOptions? options = null,
        IProgress<StemSeparationProgress>? progress = null,
        CancellationToken ct = default);

    /// <summary>
    /// <see cref="SeparateTrackAsync"/> as a PULL stream: the caller
    /// <c>await foreach</c>es the progress instead of being called back, and
    /// the LAST item carries the whole <see cref="SeparationResult"/> in
    /// <see cref="StemSeparationProgress.Result"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>Why a stream at all.</b> <see cref="IProgress{T}"/> pushes: the
    /// producer decides the rate and the consumer has no way to say it is
    /// behind, so every consumer grew its own throttle. A stream lets the
    /// consumer set the pace and the transport coalesce, which is the same
    /// answer in one place instead of N.
    /// </para>
    /// <para>
    /// <b>The shape, and why this one.</b> The terminal item carries the
    /// result rather than the method returning a
    /// <c>(stream, Task&lt;SeparationResult&gt;)</c> pair, because a UI binds
    /// ONE sequence: <c>await foreach</c> the samples, update the bar, and on
    /// the item whose <see cref="StemSeparationProgress.Result"/> is non-null
    /// commit the output — no second await to forget, no way to observe a
    /// result the stream has not finished reporting, and no partially-consumed
    /// stream left holding a live separation. It is also what keeps coalescing
    /// lossless: see <see cref="StemSeparationProgress.Result"/>.
    /// </para>
    /// <para>
    /// <b>Contract.</b> Samples arrive in order. Cancelling the token — or
    /// abandoning the enumeration — cancels the separation and surfaces
    /// <see cref="OperationCanceledException"/> from <c>MoveNextAsync</c>,
    /// never a <see cref="StemSeparationException"/>. A backend failure
    /// surfaces from <c>MoveNextAsync</c> with its ORIGINAL exception type
    /// (<see cref="StemSeparationVramException"/> stays a
    /// <see cref="StemSeparationVramException"/>), so a consumer's
    /// "retry on CPU" branch still fires. The sequence is single-shot.
    /// </para>
    /// <para>
    /// <b>This default implementation is a bridge, not the real thing.</b> It
    /// adapts whatever <see cref="SeparateTrackAsync"/> the implementation has,
    /// so a third-party backend gets the stream for free. Every backend in this
    /// repo overrides it and publishes into the channel directly.
    /// </para>
    /// </remarks>
    IAsyncEnumerable<StemSeparationProgress> SeparateWithProgressAsync(
        string inputPath,
        string outputDir,
        StemSeparationOptions? options = null,
        CancellationToken ct = default)
    {
        var backend = GetType().Name;
        return CoalescingProgressStream.Create<StemSeparationProgress>(
            async (report, token) =>
            {
                // ImmediateProgress, never Progress<T>: Progress<T> posts to the
                // captured SynchronizationContext, so on a UI host the samples
                // would reach the channel AFTER the terminal item.
                var bridge = new ImmediateProgress<StemSeparationProgress>(
                    p => report(p with { Backend = p.Backend ?? backend }));
                var result = await SeparateTrackAsync(inputPath, outputDir, options, bridge, token)
                    .ConfigureAwait(false);
                report(new StemSeparationProgress(
                    "done", 1.0, "Separation complete.", Stem: null, Backend: backend, Result: result));
            },
            ct: ct);
    }
}
