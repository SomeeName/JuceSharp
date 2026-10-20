// ReSharper disable InconsistentNaming -- private static probe cache uses _camelCase consistent with project style.
using System.Runtime.CompilerServices;
using System.Text;
using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// One progress sample from <see cref="JuceStemSeparation.SeparateWithProgressAsync"/>.
/// </summary>
/// <param name="Phase">Coarse phase: <c>loading</c>, <c>inference</c>, <c>done</c>.</param>
/// <param name="Fraction">0..1 overall progress.</param>
/// <param name="Message">The native stage label, verbatim.</param>
/// <param name="Backend">Backend identifier — always
///   <see cref="JuceStemSeparation.BackendId"/>, so a host that merges the
///   streams of several backends can tell them apart.</param>
/// <remarks>
/// Deliberately carries no result object: <c>dca_stems_separate_file</c>
/// WRITES FILES and returns a status, so there is nothing for a terminal item
/// to hand back beyond "the output directory is now populated". The two
/// backends that do produce a value (the python sidecar's
/// <c>SeparationResult</c>, the ONNX one's stem buffers) put it on their own
/// terminal item; this one only has to say <c>done</c>.
/// </remarks>
public readonly record struct JuceStemProgress(string Phase, double Fraction, string Message, string Backend);

/// <summary>
/// Managed entry point for the native AI stem separation exposed by
/// <c>juce_sharp.dll</c>. Wraps the <c>dca_stems_*</c> C ABI (added in v5).
/// </summary>
/// <remarks>
/// The native side compiles in support only when the CMake build was
/// configured with <c>-DDOTCHARTER_STEMS_DEMUCS_CPP=ON</c>. When the option
/// is OFF the wrapper still loads, but <see cref="IsSupported"/> returns
/// <c>false</c> so the host can transparently fall back to an external
/// implementation (e.g. the Python demucs CLI).
///
/// The wrapper also tolerates older <c>juce_sharp.dll</c> builds that pre-date
/// ABI v5: an <see cref="EntryPointNotFoundException"/> on first use is
/// caught and the feature is reported as unsupported.
/// </remarks>
public static class JuceStemSeparation
{
    /// <summary>Identifier stamped on every <see cref="JuceStemProgress"/> this
    /// façade emits. Matches the DotCharter-side backend id for the native
    /// path.</summary>
    public const string BackendId = "demucs.cpp";

    // Lazy<bool> publishes the probed value with release semantics so the
    // first concurrent caller is guaranteed to observe a fully initialised
    // result instead of a torn nullable.
    private static readonly Lazy<bool> _supported =
        new(ProbeIsSupported, LazyThreadSafetyMode.ExecutionAndPublication);

    private static bool ProbeIsSupported()
    {
        try { return NativeMethods.StemsIsSupported() != 0; }
        catch (EntryPointNotFoundException) { return false; }
        catch (DllNotFoundException)        { return false; }
    }

    /// <summary>True if the native DLL was built with the demucs.cpp backend.</summary>
    public static bool IsSupported => _supported.Value;

    /// <summary>Human-readable description of the active backend.</summary>
    public static string DescribeBackend()
    {
        if (!IsSupported)
            return "Native stem separation unavailable (rebuild juce_sharp with -DDOTCHARTER_STEMS_DEMUCS_CPP=ON).";
        try
        {
            // The sizing call's status was DISCARDED, so any failure fell through
            // to the hard-coded "demucs.cpp" below — a build with a different
            // backend, or a DLL that could not answer, described itself as the
            // one thing it might not be (W4-C5).
            if (NativeMethods.StemsDescribeBackend(null, 0, out var size) != NativeMethods.DcaStatus.Ok)
                return "Native stem separation backend could not be identified.";
            if (size <= 1) return "demucs.cpp";
            var buf = new byte[size];
            if (NativeMethods.StemsDescribeBackend(buf, size, out _) != NativeMethods.DcaStatus.Ok)
                return "Native stem separation backend could not be identified.";
            // Trim trailing NUL.
            var n = Array.IndexOf<byte>(buf, 0);
            if (n < 0) n = buf.Length;
            return Encoding.UTF8.GetString(buf, 0, n);
        }
        catch (EntryPointNotFoundException) { return "Native stem separation unavailable (older juce_sharp.dll)."; }
    }

    /// <summary>
    /// Runs the native model end-to-end. Blocks until inference finishes.
    /// </summary>
    /// <param name="inputAudioPath">Any audio file the JUCE format manager can decode.</param>
    /// <param name="outputDirectory">Will contain <c>drums.wav</c>, <c>bass.wav</c>, ... on success.</param>
    /// <param name="modelPath">Path to a <c>ggml-model-htdemucs-*-f16.bin</c> weights file.</param>
    /// <param name="numThreads">0 or negative = hardware concurrency.</param>
    /// <param name="onProgress">Optional progress callback (fraction 0..1, stage label).</param>
    public static void Separate(
        string inputAudioPath,
        string outputDirectory,
        string modelPath,
        int numThreads = 0,
        Action<float, string>? onProgress = null)
    {
        // Every other path argument on this ABI leads with this — OpenFile,
        // LoadPlugin, AddNode, GetParam, SetString — and these three reached the
        // marshaller raw, so a null modelPath surfaced as a generic
        // JuceAudioException(InvalidArg, "dca_stems_separate_file") naming
        // neither the argument nor the problem (W4-C5). Ahead of the capability check:
        // an argument is wrong on every build, and a caller branching on
        // NotSupportedException must not be told "no backend" about a typo.
        ArgumentException.ThrowIfNullOrWhiteSpace(inputAudioPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(modelPath);

        // NotSupportedException, not InvalidOperationException: "this build has no
        // demucs backend" is a CAPABILITY answer a caller can branch on to fall
        // through to the ONNX or python backend, and it used to be the same
        // exception type as "the native call failed" below — so a composite
        // separation service could not tell "try the next backend" from "this
        // backend broke". (JuceStemSeparationService already probes IsSupported,
        // which is why nothing has been observed to catch either one.)
        if (!IsSupported)
            throw new NotSupportedException("Native stem separation is not available in this build.");

        // ONE PATH TO SeparateCore. This used to manufacture a dead token of its
        // own, which made two overloads that both reached the core — and made it
        // possible, which is what happened (W5-E1), for an async entry point to
        // resolve to THIS one by passing five arguments and silently drop its
        // token: the whole ABI v36 cancel feature was unreachable for a release
        // while ten tests over the token-taking half passed. Forwarding means
        // the uncancellable behaviour is expressed as `default`, at one site.
        // `AbiV36Tests.BothAsyncSeparationPaths_HandTheirTokenToTheCancellableOverload`
        // lints that no dead token is constructed anywhere in this file.
        Separate(inputAudioPath, outputDirectory, modelPath, numThreads, onProgress, default);
    }

    /// <summary>
    ///     Runs the native separation, honouring <paramref name="cancellationToken" />
    ///     INSIDE the native call (ABI v36).
    /// </summary>
    /// <remarks>
    ///     <para>
    ///         Before v36 the native progress callback returned <c>void</c>, so there
    ///         was no channel to answer on: cancelling could only abandon the wrapper
    ///         task while the separation ran to completion on its own thread and wrote
    ///         stems nobody wanted. This routes the token through
    ///         <c>dca_stems_separate_file2</c>, whose callback returns 0 to stop.
    ///     </para>
    ///     <para>
    ///         A cancel is checked between the coarse phases and between the per-target
    ///         WAV writes. Inference itself is demucs.cpp's own threaded call and takes
    ///         no callback, so a cancel arriving during it lands when inference returns
    ///         — a real bound on latency, not an instant stop. Stems already written are
    ///         LEFT on disk; deleting a partial set is the caller's decision.
    ///     </para>
    /// </remarks>
    /// <param name="inputAudioPath">Any decodable audio file.</param>
    /// <param name="outputDirectory">Created if missing; one WAV per source is written into it.</param>
    /// <param name="modelPath">A <c>ggml-model-htdemucs-*.bin</c> weights file.</param>
    /// <param name="numThreads">0 or negative = hardware concurrency.</param>
    /// <param name="onProgress">Optional progress callback (fraction 0..1, stage label).</param>
    /// <param name="cancellationToken">Cancels the native run at its next checked point.</param>
    /// <exception cref="OperationCanceledException">The token was signalled.</exception>
    /// <exception cref="NotSupportedException">This build has no native backend.</exception>
    /// <exception cref="JuceAudioException">The native call failed.</exception>
    public static void Separate(
        string inputAudioPath,
        string outputDirectory,
        string modelPath,
        int numThreads,
        Action<float, string>? onProgress,
        CancellationToken cancellationToken)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(inputAudioPath);
        ArgumentException.ThrowIfNullOrWhiteSpace(outputDirectory);
        ArgumentException.ThrowIfNullOrWhiteSpace(modelPath);
        if (!IsSupported)
            throw new NotSupportedException("Native stem separation is not available in this build.");

        SeparateCore(inputAudioPath, outputDirectory, modelPath, numThreads,
                     onProgress, cancellationToken);
    }

    private static void SeparateCore(
        string inputAudioPath,
        string outputDirectory,
        string modelPath,
        int numThreads,
        Action<float, string>? onProgress,
        CancellationToken cancellationToken)
    {
        // MODEL MISSING IS ITS OWN ANSWER (W4-N19 follow-up). Native returns
        // DCA_ERR_NOT_INITIALIZED for "the weights are not on disk" AND for "the
        // backend is not compiled in" — it has no third code — so the ABI cannot
        // tell a host which one it is looking at. The wrapper can, and the
        // difference is actionable: one is a DOWNLOAD (DemucsGgmlModelCatalog
        // exists to perform it), the other is a rebuild. Checked after
        // IsSupported so the capability answer still wins.
        if (!File.Exists(modelPath))
            throw new FileNotFoundException(
                "The demucs model weights were not found. This is a download, not a bug: "
                + "resolve the file with DemucsGgmlModelCatalog.EnsureAsync before separating.",
                modelPath);

        var cb = WrapCancellableProgress(onProgress, cancellationToken);

        var st = NativeMethods.StemsSeparateFile2(
            inputAudioPath, outputDirectory, modelPath, numThreads, cb, nint.Zero);
        // Keep the delegate alive for the duration of the call (defensive —
        // we already block until the native call returns, but the JIT may be
        // aggressive about deciding `cb` is dead before then).
        GC.KeepAlive(cb);

        // A CANCEL IS NOT A FAILURE. Surfacing it as JuceAudioException would
        // read to every consumer as a reason to fall through to the next
        // backend, which is the repo's stated rule for stem separation.
        if (st == NativeMethods.DcaStatus.Cancelled)
            throw new OperationCanceledException(cancellationToken);

        // The real status, typed, so a caller can branch on Decode (a file this
        // build cannot read) versus Internal (inference blew up) instead of
        // regex-ing it back out of a message string.
        if (st != NativeMethods.DcaStatus.Ok)
            throw new JuceAudioException((JuceAudioStatus)(int)st, "dca_stems_separate_file2");
    }

    /// <summary>
    ///     Marshals the host's progress action into the v36 cancellable callback
    ///     shape: returns non-zero to continue, 0 to stop.
    /// </summary>
    /// <remarks>
    ///     A handler that THROWS does not cancel. It is a broken observer, and
    ///     tearing down a multi-minute separation because somebody's progress bar
    ///     faulted is the wrong blast radius — it is logged and the run continues,
    ///     exactly as the non-cancellable callback behaves. Only the token stops it.
    /// </remarks>
    /// <param name="onProgress">The host's handler, or null.</param>
    /// <param name="cancellationToken">Polled on every report.</param>
    /// <returns>A delegate to hand to <c>dca_stems_separate_file2</c>. Never null:
    /// the token has to be polled even when the host wants no progress.</returns>
    internal static NativeMethods.DcaStemsProgressCallback2 WrapCancellableProgress(
        Action<float, string>? onProgress, CancellationToken cancellationToken)
    {
        return (_, frac, stage) =>
        {
            if (cancellationToken.IsCancellationRequested) return 0;
            if (onProgress is not null)
            {
                try { onProgress(frac, stage); }
                catch (Exception ex)
                {
                    JuceLog.Emit(JuceLogLevel.Warn,
                        $"Stem separation progress handler threw: {ex.GetType().Name}: {ex.Message}");
                }
            }
            return 1;
        };
    }

    /// <summary>
    /// Marshals the host's progress action into the native callback shape, with
    /// a catch-all around the host call.
    /// <para>
    /// <b>Never let a host exception escape into native.</b> This delegate is a
    /// reverse-P/Invoke, so an unhandled throw unwinds through C++ frames — a
    /// process kill, not a catchable error. It is invoked ON THE THREAD THAT
    /// CALLED <c>dca_stems_separate_file2</c>, i.e. the dedicated background
    /// thread <c>RunOffThePool</c> started (this said "a demucs WORKER thread"
    /// until W5-E10; the separator's checked points are all on the calling
    /// thread and demucs.cpp's threaded inference takes no progress hook). A
    /// progress-bar update touching an off-thread collection is exactly the
    /// shape that throws, and until 0.61 this was the one callback in the
    /// wrapper without the guard (<c>JuceMidiInput.OnMidi</c>,
    /// <c>JuceAudioPlugin</c>'s editor-closed thunk and
    /// <c>JuceAudioEngine.StaticOnNativeLog</c> all had it).
    /// </para>
    /// <para>A named method rather than an inline lambda so the behaviour is
    /// reachable from a test — see
    /// <c>NativeCallbackSafetyTests.AThrowingStemProgressHandler_IsSwallowedAndLogged</c>.</para>
    /// </summary>
    internal static NativeMethods.DcaStemsProgressCallback? WrapProgress(Action<float, string>? onProgress)
    {
        if (onProgress is null) return null;

        return (_, frac, stage) =>
        {
            try { onProgress(frac, stage); }
            catch (Exception ex)
            {
                JuceLog.Emit(JuceLogLevel.Warn,
                    $"Stem separation progress handler threw: {ex.GetType().Name}: {ex.Message}");
            }
        };
    }

    /// <summary>
    /// Async wrapper around <see cref="Separate(string, string, string, int,
    /// Action{float, string}, CancellationToken)"/>. The native call is blocking
    /// but CANCELLABLE since ABI v36: the token is checked between the coarse
    /// phases and between the per-target WAV writes, so cancelling stops the
    /// work rather than merely stopping the wrapper task from observing it.
    /// Inference itself takes no callback, so a cancel arriving during it lands
    /// when inference returns. Stems already written are LEFT on disk.
    /// </summary>
    public static Task SeparateAsync(
        string inputAudioPath,
        string outputDirectory,
        string modelPath,
        int numThreads = 0,
        Action<float, string>? onProgress = null,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return RunOffThePool(() =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            // SIX arguments: the token-taking overload. Five would resolve to
            // the convenience overload and the cancel would never reach native.
            Separate(inputAudioPath, outputDirectory, modelPath, numThreads, onProgress, cancellationToken);
            cancellationToken.ThrowIfCancellationRequested();
        }, cancellationToken);
    }

    /// <summary>
    ///     Run one blocking separation on a DEDICATED background thread.
    ///     <para>
    ///         <b>Never the thread pool</b>, which is the rule
    ///         <c>Interop/EngineWorker</c> states in full and which this file broke
    ///         until W4-E1: "a <c>Task.Run</c> parks a pool thread for the duration
    ///         and the pool answers by INJECTING threads (one every ~0.5 s), which
    ///         starves every other async workload in the host". That argument was
    ///         written for a 25 s VST3 scan; a <c>dca_stems_separate_file</c> run
    ///         is MINUTES, an order of magnitude past the worst case the rule
    ///         exists for, and it was the one call in the wrapper still on the
    ///         pool.
    ///     </para>
    ///     <para>
    ///         A thread per call rather than <c>EngineWorker</c>'s single serial
    ///         queue, deliberately: the worker's serialisation exists because two
    ///         device reconfigurations must not overlap, and nothing about two
    ///         separations says that. Whether they SHOULD overlap is the caller's
    ///         choice, and the backends already have their own process-wide gate.
    ///     </para>
    ///     <para>
    ///         <c>RunContinuationsAsynchronously</c> so the caller's <c>await</c>
    ///         body never executes on this thread, which is about to exit.
    ///     </para>
    /// </summary>
    internal static Task RunOffThePool(Action work, CancellationToken cancellationToken)
    {
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled(cancellationToken);

        var tcs = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                work();
                tcs.TrySetResult();
            }
            catch (OperationCanceledException oce) { tcs.TrySetCanceled(oce.CancellationToken); }
            catch (Exception ex) { tcs.TrySetException(ex); }
        })
        {
            IsBackground = true,
            Name = "JuceStemSeparation worker",
        };
        thread.Start();
        return tcs.Task;
    }

    /// <summary>
    /// <see cref="SeparateAsync"/> as a PULL stream: every native progress
    /// sample arrives as a <see cref="JuceStemProgress"/> a consumer can
    /// <c>await foreach</c>, ending with a terminal <c>done</c> item.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>The native callback is the channel writer, directly.</b>
    /// <c>dca_stems_separate_file2</c> invokes its progress callback on the
    /// separation thread — never the consumer's — which is why this is a channel
    /// and not an event: <see cref="System.Threading.Channels.ChannelWriter{T}.TryWrite"/>
    /// on the bounded queue is thread-safe and never blocks, so a slow
    /// consumer cannot back-pressure inference, and a consumer that stalls
    /// pays in stale fractions rather than in wall time. See
    /// <see cref="CoalescingProgressStream"/> for the whole contract.
    /// </para>
    /// <para>
    /// <b>Faults keep their type.</b> An unsupported build still throws
    /// <see cref="NotSupportedException"/> and a native failure still throws
    /// <see cref="JuceAudioException"/> — out of <c>MoveNextAsync</c>, not
    /// wrapped in a channel exception, so the caller's
    /// "fall through to the next backend" branch still fires.
    /// </para>
    /// <para>
    /// <b>Cancellation reaches the native run</b> since ABI v36. Cancelling — or
    /// abandoning the enumeration — stops the separation at its next checked
    /// point (between the coarse phases, and between the per-target WAV writes)
    /// and the enumeration surfaces <see cref="OperationCanceledException"/>.
    /// Inference itself takes no callback, so a cancel arriving during it lands
    /// when inference returns. Stems already written are LEFT on disk.
    /// </para>
    /// </remarks>
    public static async IAsyncEnumerable<JuceStemProgress> SeparateWithProgressAsync(
        string inputAudioPath,
        string outputDirectory,
        string modelPath,
        int numThreads = 0,
        [EnumeratorCancellation] CancellationToken cancellationToken = default)
    {
        var stream = CoalescingProgressStream.Create<JuceStemProgress>(
            // RunOffThePool, not Task.Run: see its own doc. A progress stream
            // does not change how long the native call blocks.
            (report, token) => RunOffThePool(() =>
            {
                token.ThrowIfCancellationRequested();
                report(new JuceStemProgress("loading", 0.0, "Loading native model…", BackendId));
                // SIX arguments, and the LINKED token the stream hands us — so
                // abandoning the enumeration cancels the native run, not just
                // the observation of it.
                Separate(inputAudioPath, outputDirectory, modelPath, numThreads,
                    (fraction, stage) => report(new JuceStemProgress("inference", fraction, stage, BackendId)),
                    token);
                token.ThrowIfCancellationRequested();
                report(new JuceStemProgress("done", 1.0, "Separation complete.", BackendId));
            }, token),
            ct: cancellationToken);

        await foreach (var item in stream.ConfigureAwait(false))
            yield return item;
    }
}
