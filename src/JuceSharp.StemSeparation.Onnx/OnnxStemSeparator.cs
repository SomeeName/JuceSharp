using System.Runtime.CompilerServices;
using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// One progress sample from
/// <see cref="OnnxStemSeparator.SeparateWithProgressAsync"/>.
/// </summary>
/// <param name="Phase">Coarse phase: <c>loading</c>, <c>inference</c>,
///   <c>stem</c>, <c>done</c>.</param>
/// <param name="Fraction">0..1 overall progress, or -1 for a sub-event that
///   carries no fractional position (a produced stem).</param>
/// <param name="Message">Human-readable status.</param>
/// <param name="Stem">Stem name when the sample announces one.</param>
/// <param name="Backend">Always <see cref="OnnxStemSeparator.BackendId"/>.</param>
/// <param name="Result">
/// Non-null on EXACTLY ONE sample per run — the terminal <c>done</c> item —
/// and it carries every produced buffer. The progress channel is bounded and
/// drops the OLDEST queued sample when a consumer falls behind, so interim
/// samples are expendable by design; the terminal item is the newest write and
/// therefore never dropped, which is what makes the coalescing lossless.
/// </param>
public readonly record struct OnnxStemProgress(
    string Phase,
    double Fraction,
    string Message,
    string? Stem,
    string Backend,
    IReadOnlyList<OnnxStemBuffer>? Result);

/// <summary>
/// High-level, application-agnostic ONNX stem-separation entry point.
/// Owns an <see cref="InferenceSession"/> and the per-kind inference
/// driver (<see cref="HtdemucsOnnxInference"/>, <see cref="BsRoformerInference"/>,
/// <see cref="MdxNetInference"/>) and exposes a single
/// <see cref="SeparateAsync"/> call returning named stem buffers — no
/// file I/O, no model catalog, no logger framework, no application config.
/// </summary>
/// <remarks>
/// <para>
/// This is the API that downstream projects (DotCharter, other DAWs,
/// command-line tools) should consume. The thin DotCharter-specific
/// adapter <c>DotCharter.Services.Audio.StemSeparation.Onnx.OnnxStemSeparationService</c>
/// only handles model-management concerns (catalog lookup, GPU
/// auto-promotion, on-demand downloads, WAV I/O) and forwards the actual
/// separation work to this class.
/// </para>
/// <para>
/// The instance is single-use: build, call <see cref="SeparateAsync"/>,
/// dispose. The session and its associated GPU allocations are released
/// when <see cref="Dispose"/> is called.
/// </para>
/// </remarks>
public sealed class OnnxStemSeparator : IDisposable
{
    /// <summary>One inference pass: (interleaved stereo, sample rate, progress, ct) → stems.</summary>
    internal delegate IReadOnlyList<OnnxStemBuffer> InferenceRunner(
        float[] interleavedStereo, int sampleRate, IProgress<double>? progress, CancellationToken ct);

    private readonly InferenceSession? _session;
    private readonly OnnxStemModelKind _kind;
    private readonly OnnxSessionFactory.SessionBundle? _bundle;
    private readonly string? _fallbackReason;
    private readonly Action<string>? _log;
    private readonly InferenceRunner _run;
    private readonly Action _releaseNative;
    private readonly InferenceGuard _guard = new();

    /// <summary>The htdemucs engine, built once. Its per-chunk scratch is ~216 MB;
    /// a fresh one per SeparateAsync dropped that on the floor every call.
    /// _guard serialises separations, so one instance is never used concurrently.</summary>
    private HtdemucsOnnxInference? _htdemucs;
    private readonly int _drainTimeoutMs;

    /// <summary>
    /// Description of the actual execution provider that was bound at
    /// construction (e.g. <c>"CUDA (NVIDIA)"</c>, <c>"DirectML (any DX12 GPU)"</c>,
    /// <c>"CPU"</c>). Useful for status text.
    /// </summary>
    public string BackendDescription => _bundle == null ? "test" : $"{_bundle.Active} ({_bundle.Description})";

    /// <summary>
    /// The execution provider actually attached after the requested EP /
    /// fallback chain was resolved. May differ from the preferred EP when
    /// the user requests an unavailable backend.
    /// </summary>
    public OnnxExecutionProvider ActiveExecutionProvider => _bundle?.Active ?? OnnxExecutionProvider.Cpu;

    /// <summary>
    /// Why the requested execution provider is not the one running, or <c>null</c>
    /// when it is. Set when the provider would not attach (no DirectML or CUDA in the
    /// onnxruntime.dll, a bad device id) and when it attached but could not build a
    /// session for this model (out of GPU memory, an unsupported operator), in which
    /// case the separator rebuilt the session on the CPU. A caller shows this so a
    /// GPU run that became a CPU run never looks like a GPU run.
    /// </summary>
    public string? FallbackReason => _fallbackReason;

    /// <summary>
    /// Loads <paramref name="modelPath"/> into an <see cref="InferenceSession"/>
    /// configured with the best matching execution provider, returning a
    /// separator ready to <see cref="SeparateAsync"/>.
    /// </summary>
    /// <param name="modelPath">Path to the .onnx file on disk.</param>
    /// <param name="kind">Which family of model this is — selects which
    ///   inference driver to invoke.</param>
    /// <param name="preferredExecutionProvider">EP preference; the factory
    ///   falls back to CPU if the requested EP is unavailable.</param>
    /// <param name="log">Optional logger callback. All progress messages
    ///   are routed through this delegate.</param>
    /// <param name="intraOpNumThreads">Cap on the CPU execution provider's
    ///   intra-op thread pool (0 = ONNX Runtime default / all cores). Callers
    ///   pass a reduced value to keep separation from saturating every core.</param>
    /// <param name="deviceId">Which GPU: the DXGI adapter index for DirectML
    ///   (<see cref="OnnxGpuAdapters.Enumerate"/>), the CUDA ordinal for CUDA.
    ///   Ignored on the CPU.</param>
    public OnnxStemSeparator(
        string modelPath,
        OnnxStemModelKind kind,
        OnnxExecutionProvider preferredExecutionProvider = OnnxExecutionProvider.Auto,
        Action<string>? log = null,
        int intraOpNumThreads = 0,
        int deviceId = 0)
    {
        if (string.IsNullOrWhiteSpace(modelPath)) throw new ArgumentException("modelPath is required.", nameof(modelPath));
        if (!File.Exists(modelPath)) throw new FileNotFoundException("ONNX model file not found.", modelPath);

        _kind = kind;
        _log = log;
        var factory = new OnnxSessionFactory(log);
        // DirectML graph fusion stays off unless the model declares it verified with it
        // (OnnxSessionFactory.DirectMlGraphFusionKey); read from the file, not a session.
        var graphFusion = OnnxSessionFactory.AllowsDirectMlGraphFusion(OnnxModelMetadata.Read(modelPath));
        // The bundle owns a native SessionOptions. A failed session build
        // (corrupt download, missing cuDNN) means this ctor never completes, so
        // Dispose never runs and that handle leaks — and model-load failure is a
        // routine, user-triggerable path. OpenWithCpuFallback disposes every
        // bundle it does not hand back.
        (_session, _bundle, _fallbackReason) = OpenWithCpuFallback(
            factory.Create(preferredExecutionProvider, intraOpNumThreads, deviceId, graphFusion),
            () => factory.Create(OnnxExecutionProvider.Cpu, intraOpNumThreads),
            options => new InferenceSession(modelPath, options),
            Path.GetFileName(modelPath),
            log);
        _run = RunForKind;
        var session = _session;
        var bundle = _bundle;
        _releaseNative = () =>
        {
            session.Dispose();
            bundle.Options.Dispose();
        };
        _drainTimeoutMs = InferenceGuard.DefaultDrainTimeoutMs;
        _log?.Invoke($"OnnxStemSeparator bound: kind={kind}, model='{Path.GetFileName(modelPath)}', " +
            $"backend={_bundle.Active} ({_bundle.Description}).");
    }

    /// <summary>
    /// Test seam: a separator over an injected inference runner instead of a
    /// real <see cref="InferenceSession"/>, so the dispose-vs-inference
    /// ordering can be exercised without a model file. Production code always
    /// uses the public constructor.
    /// </summary>
    internal OnnxStemSeparator(InferenceRunner runner, Action releaseNative,
        int drainTimeoutMs = InferenceGuard.DefaultDrainTimeoutMs, Action<string>? log = null)
    {
        _run = runner ?? throw new ArgumentNullException(nameof(runner));
        _releaseNative = releaseNative ?? throw new ArgumentNullException(nameof(releaseNative));
        _drainTimeoutMs = drainTimeoutMs;
        _kind = OnnxStemModelKind.Htdemucs;
        _log = log;
    }

    /// <summary>
    /// Builds the session on <paramref name="first"/>'s provider and, when that is a GPU
    /// provider that attached but cannot build THIS model (out of video memory, an
    /// operator it lacks), rebuilds it on the CPU instead of failing the run. The reason
    /// is returned so the caller never shows a CPU run as a GPU one. A CPU build that
    /// fails throws — the CPU shares a failure such as a corrupt file. Every bundle not
    /// handed back is disposed.
    /// </summary>
    internal static (TSession Session, OnnxSessionFactory.SessionBundle Bundle, string? FallbackReason)
        OpenWithCpuFallback<TSession>(
            OnnxSessionFactory.SessionBundle first,
            Func<OnnxSessionFactory.SessionBundle> cpuBundle,
            Func<SessionOptions, TSession> open,
            string modelName,
            Action<string>? log)
    {
        try
        {
            return (open(first.Options), first, first.FallbackReason);
        }
        catch (Exception ex) when (first.Active != OnnxExecutionProvider.Cpu)
        {
            first.Options.Dispose();
            var reason = $"{first.Active}: {ex.Message}";
            log?.Invoke($"ONNX session: {first.Active} could not load '{modelName}' ({ex.Message}). " +
                "Rebuilding the session on the CPU.");
            var cpu = cpuBundle();
            try
            {
                return (open(cpu.Options), cpu, reason);
            }
            catch
            {
                cpu.Options.Dispose();
                throw;
            }
        }
        catch
        {
            first.Options.Dispose();
            throw;
        }
    }

    private IReadOnlyList<OnnxStemBuffer> RunForKind(
        float[] interleavedStereo, int sampleRate, IProgress<double>? progress, CancellationToken ct) => _kind switch
    {
        OnnxStemModelKind.Htdemucs => SeparateHtdemucs(interleavedStereo, sampleRate, progress, ct),
        OnnxStemModelKind.BsRoformer => SeparateBsRoformer(interleavedStereo, sampleRate, progress, ct),
        OnnxStemModelKind.MdxNet => SeparateMdxNet(interleavedStereo, sampleRate, progress, ct),
        _ => throw new NotSupportedException($"Unsupported model kind {_kind}."),
    };

    /// <summary>True once <see cref="Dispose"/> has been called. Exposed for
    /// tests that pin the drain ordering.</summary>
    internal bool IsDisposeRequested => _guard.IsDisposeRequested;

    /// <summary>
    /// Runs stem separation on <paramref name="interleavedStereo"/> sampled
    /// at <paramref name="sampleRate"/> Hz. The returned stems are
    /// resampled back to the input rate so the caller can write them
    /// directly alongside the source audio.
    /// </summary>
    /// <param name="interleavedStereo">Interleaved stereo PCM (LR LR LR …).
    ///   Must have an even length. Mono input must be expanded to stereo
    ///   by the caller before invoking this method.</param>
    /// <param name="sampleRate">Input sample rate, e.g. 44100 or 48000.</param>
    /// <param name="progress">Optional 0..1 progress reporter.</param>
    /// <param name="ct">Cancellation token.</param>
    public Task<IReadOnlyList<OnnxStemBuffer>> SeparateAsync(
        float[] interleavedStereo,
        int sampleRate,
        IProgress<double>? progress = null,
        CancellationToken ct = default)
        => SeparateCoreAsync(interleavedStereo, sampleRate, progress, ct);

    /// <summary>Backend identifier stamped on every
    /// <see cref="OnnxStemProgress"/> this separator publishes.</summary>
    public const string BackendId = "onnx";

    /// <summary>
    /// <see cref="SeparateAsync"/> as a PULL stream. Samples arrive in order,
    /// one per inference-progress tick plus one per produced stem, and the
    /// terminal item carries every <see cref="OnnxStemBuffer"/> in
    /// <see cref="OnnxStemProgress.Result"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <b>Peer of <see cref="SeparateAsync"/>, not a wrapper around it.</b>
    /// Both sit on <see cref="SeparateCoreAsync"/>; the
    /// <see cref="IProgress{T}"/> handed to the driver here IS the channel
    /// writer (an <see cref="ImmediateProgress{T}"/>, which reports on the
    /// calling thread), so there is no <see cref="Progress{T}"/> post and no
    /// second buffering layer between the driver's per-chunk tick and the
    /// consumer.
    /// </para>
    /// <para>
    /// Cancellation (<see cref="OperationCanceledException"/> out of
    /// <c>MoveNextAsync</c>, and abandoning the enumeration cancels the run),
    /// coalescing, and "a fault keeps its original type" are
    /// <see cref="CoalescingProgressStream"/>'s contract. A
    /// <see cref="Dispose"/> racing an enumeration still surfaces as
    /// <see cref="ObjectDisposedException"/> through the same
    /// <see cref="InferenceGuard"/> the push API uses.
    /// </para>
    /// </remarks>
    public async IAsyncEnumerable<OnnxStemProgress> SeparateWithProgressAsync(
        float[] interleavedStereo,
        int sampleRate,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        var stream = CoalescingProgressStream.Create<OnnxStemProgress>(
            async (report, token) =>
            {
                report(new OnnxStemProgress("loading", 0.0,
                    $"Running {_kind} inference on {BackendDescription}.", null, BackendId, null));

                var sink = new ImmediateProgress<double>(f =>
                    report(new OnnxStemProgress("inference", f, "Separating…", null, BackendId, null)));

                var stems = await SeparateCoreAsync(interleavedStereo, sampleRate, sink, token)
                    .ConfigureAwait(false);

                foreach (var stem in stems)
                    report(new OnnxStemProgress("stem", -1, $"Produced stem '{stem.Name}'.",
                        stem.Name, BackendId, null));

                report(new OnnxStemProgress("done", 1.0, "Separation complete.", null, BackendId, stems));
            },
            ct: ct);

        await foreach (var item in stream.ConfigureAwait(false))
            yield return item;
    }

    private Task<IReadOnlyList<OnnxStemBuffer>> SeparateCoreAsync(
        float[] interleavedStereo,
        int sampleRate,
        IProgress<double>? progress,
        CancellationToken ct)
    {
        if (interleavedStereo == null) throw new ArgumentNullException(nameof(interleavedStereo));
        if (interleavedStereo.Length % 2 != 0)
            throw new ArgumentException("Input must be interleaved stereo (even-length array).", nameof(interleavedStereo));
        if (sampleRate <= 0) throw new ArgumentOutOfRangeException(nameof(sampleRate));
        if (_guard.IsDisposeRequested) throw new ObjectDisposedException(nameof(OnnxStemSeparator));

        return Task.Run(() =>
        {
            ct.ThrowIfCancellationRequested();
            // The pin happens on the POOL thread, not the caller's: a Dispose
            // that lands between the check above and this line is observed
            // here (Enter throws), and a Dispose that lands after it waits for
            // Exit before it frees the session. See InferenceGuard.
            _guard.Enter(nameof(OnnxStemSeparator));
            try
            {
                return _run(interleavedStereo, sampleRate, progress, ct);
            }
            finally
            {
                _guard.Exit();
            }
        }, ct);
    }

    private IReadOnlyList<OnnxStemBuffer> SeparateHtdemucs(
        float[] interleavedStereo, int sampleRate, IProgress<double>? progress, CancellationToken ct)
    {
        // ONE engine per separator, not one per call. Constructing it is cheap,
        // but its chunk scratch is ~216 MB and every SeparateAsync used to build
        // a fresh set and drop the last one on the floor. _guard serialises
        // separations, so a single instance is never used concurrently.
        var engine = _htdemucs ??= new HtdemucsOnnxInference(_session!, _log);
        var stems = engine.Predict(interleavedStereo, sampleRate, progress, ct);
        // The SAME post-resample every driver uses. It used to be a second copy
        // that had lost FitToFrames (W4-S11), so an htdemucs stem could come
        // back one frame longer than the mix while SeparateAsync's contract says
        // a caller may write them straight alongside the source audio.
        var pairs = new (string Name, float[] Stereo)[stems.Count];
        for (var i = 0; i < stems.Count; i++) pairs[i] = (stems[i].Name, stems[i].InterleavedStereo);
        return ResampleStemsBack(pairs, HtdemucsOnnxInference.SampleRate, sampleRate,
                                 interleavedStereo.Length / 2, "htdemucs", _log);
    }

    /// <summary>
    /// The rate every BS-RoFormer and MDX-Net checkpoint we ship was trained
    /// at. Both drivers take "stereo at 44.1 kHz" on trust — neither reads a
    /// rate — so a 48 kHz session used to be fed 48 kHz samples as if they were
    /// 44.1 kHz: the STFT bands landed a semitone-and-a-half sharp of what the
    /// model learned, and the stems came back at the wrong pitch relationship
    /// to the mix. htdemucs always resampled; these two never did.
    /// </summary>
    internal const int SpectralModelSampleRate = 44_100;

    private IReadOnlyList<OnnxStemBuffer> SeparateBsRoformer(
        float[] interleavedStereo, int sampleRate, IProgress<double>? progress, CancellationToken ct)
    {
        var engine = new BsRoformerInference(_session!, _log);

        // Detect multi-stem (output channels > 4) vs single-stem (vocals only).
        var outDims = _session!.OutputMetadata[_session.OutputNames[0]].Dimensions;
        var outCh = outDims.Length >= 2 && outDims[1] > 0 ? outDims[1] : 4;

        return SeparateAtModelRate(interleavedStereo, sampleRate, SpectralModelSampleRate, "BS-RoFormer", _log,
            modelRateInput =>
            {
                // The split-complex export (BS-RoFormer SW) names its own stems, in ITS
                // order — bass first, not drums — so StemNamesForCount must not be
                // applied to it.
                if (engine.Binding.Layout == BsRoformerIoLayout.SplitComplex)
                    return engine.PredictNamedStems(modelRateInput, progress, ct);

                if (outCh <= 4)
                {
                    var vocals = engine.PredictVocals(modelRateInput, progress, ct);
                    return new[]
                    {
                        ("vocals", vocals),
                        ("other", Residual(modelRateInput, vocals)),
                    };
                }

                var stems = engine.PredictAllStems(modelRateInput, progress, ct);
                var names = StemNamesForCount(stems.Count);
                var list = new (string, float[])[stems.Count];
                for (var i = 0; i < stems.Count; i++) list[i] = (names[i], stems[i]);
                return list;
            });
    }

    private IReadOnlyList<OnnxStemBuffer> SeparateMdxNet(
        float[] interleavedStereo, int sampleRate, IProgress<double>? progress, CancellationToken ct)
    {
        var engine = new MdxNetInference(_session!);
        return SeparateAtModelRate(interleavedStereo, sampleRate, SpectralModelSampleRate, "MDX-Net", _log,
            modelRateInput =>
            {
                var other = engine.PredictInstrumental(modelRateInput, progress, ct);
                return new[]
                {
                    ("other", other),
                    ("vocals", Residual(modelRateInput, other)),
                };
            });
    }

    /// <summary>mix − stem, computed at the MODEL rate so both operands are
    /// the same signal — subtracting a 44.1 kHz stem from a 48 kHz mix is
    /// what a resample-free path would have done.</summary>
    private static float[] Residual(float[] mix, float[] stem)
    {
        var residual = new float[stem.Length];
        var n = Math.Min(mix.Length, stem.Length);
        for (var i = 0; i < n; i++) residual[i] = mix[i] - stem[i];
        return residual;
    }

    /// <summary>
    /// Runs <paramref name="predictAtModelRate"/> on the input converted to
    /// <paramref name="modelRate"/> and returns every stem converted BACK to
    /// <paramref name="sampleRate"/> at exactly the input's frame count, so
    /// the caller can write the stems directly alongside the source audio.
    /// At the model rate the input is handed over untouched and no copy is
    /// made. Internal so the rate contract is testable without a model.
    /// </summary>
    internal static IReadOnlyList<OnnxStemBuffer> SeparateAtModelRate(
        float[] interleavedStereo, int sampleRate, int modelRate, string label, Action<string>? log,
        Func<float[], IReadOnlyList<(string Name, float[] Stereo)>> predictAtModelRate)
    {
        if (sampleRate == modelRate)
        {
            var native = predictAtModelRate(interleavedStereo);
            var passthrough = new OnnxStemBuffer[native.Count];
            for (var i = 0; i < native.Count; i++)
                passthrough[i] = new OnnxStemBuffer(native[i].Name, native[i].Stereo, sampleRate);
            return passthrough;
        }

        log?.Invoke($"{label} ONNX: resampling input {sampleRate} → {modelRate} Hz (Lanczos-8).");
        var input = SampleRateConverter.Resample(interleavedStereo, 2, sampleRate, modelRate);
        var stems = predictAtModelRate(input);

        return ResampleStemsBack(stems, modelRate, sampleRate, interleavedStereo.Length / 2, label, log);
    }

    /// <summary>
    /// Convert every stem from <paramref name="modelRate"/> back to
    /// <paramref name="sampleRate"/> and fit it to the mix's exact frame count.
    /// <para>ONE implementation for every driver, deliberately: this existed
    /// twice and the htdemucs copy had no <see cref="FitToFrames"/> call, which
    /// is exactly how the next fix lands in one of two copies. Internal so the
    /// length contract is testable without a model.</para>
    /// </summary>
    /// <param name="stems">Stems at <paramref name="modelRate"/>.</param>
    /// <param name="modelRate">Rate the stems are at.</param>
    /// <param name="sampleRate">Rate the caller wants back.</param>
    /// <param name="frames">The MIX's frame count; every stem is fitted to it.</param>
    /// <param name="label">Driver name, for the log line.</param>
    /// <param name="log">Optional log sink.</param>
    internal static IReadOnlyList<OnnxStemBuffer> ResampleStemsBack(
        IReadOnlyList<(string Name, float[] Stereo)> stems,
        int modelRate, int sampleRate, int frames, string label, Action<string>? log)
    {
        var results = new OnnxStemBuffer[stems.Count];
        if (modelRate == sampleRate)
        {
            for (var i = 0; i < stems.Count; i++)
                results[i] = new OnnxStemBuffer(stems[i].Name, FitToFrames(stems[i].Stereo, frames), sampleRate);
            return results;
        }

        var swResample = System.Diagnostics.Stopwatch.StartNew();
        // Stem post-resamples are embarrassingly parallel — one O(N·Lobes) pass
        // each. Doing them sequentially was the visible 90→92% stall in the UI
        // on 6-stem models (~50 s on a 3:30 song).
        Parallel.For(0, stems.Count, i =>
        {
            var back = SampleRateConverter.Resample(stems[i].Stereo, 2, modelRate, sampleRate);
            results[i] = new OnnxStemBuffer(stems[i].Name, FitToFrames(back, frames), sampleRate);
        });
        swResample.Stop();
        log?.Invoke($"{label} post-resample: {swResample.ElapsedMilliseconds}ms ({stems.Count} stems).");
        return results;
    }

    /// <summary>A round trip through two rounded frame counts can land one
    /// frame long or short; pad with silence or trim so every stem has the
    /// mix's exact length.</summary>
    private static float[] FitToFrames(float[] interleavedStereo, int frames)
    {
        var want = frames * 2;
        if (interleavedStereo.Length == want) return interleavedStereo;
        var fitted = new float[want];
        Array.Copy(interleavedStereo, fitted, Math.Min(want, interleavedStereo.Length));
        return fitted;
    }

    /// <summary>Names for a PACKED multi-stem BS-RoFormer, which carries none: htdemucs'
    /// order. A split-complex model names its own (<see cref="BsRoformerInference.StemNames"/>).</summary>
    private static string[] StemNamesForCount(int n) => n switch
    {
        2 => new[] { "vocals", "other" },
        4 => new[] { "drums", "bass", "other", "vocals" },
        5 => new[] { "drums", "bass", "other", "vocals", "guitar" },
        6 => new[] { "drums", "bass", "other", "vocals", "guitar", "piano" },
        _ => Enumerable.Range(0, n).Select(i => $"stem_{i + 1}").ToArray(),
    };

    /// <summary>
    /// Releases the session and its execution-provider allocations. BLOCKS
    /// until an in-flight <see cref="SeparateAsync"/> has left the session
    /// (cancel it first for a prompt return); if one is still running after
    /// the drain timeout the session is LEAKED rather than freed under it.
    /// </summary>
    public void Dispose()
    {
        switch (_guard.TakeForDispose(_drainTimeoutMs))
        {
            case InferenceGuard.DrainOutcome.AlreadyClaimed:
                return;
            case InferenceGuard.DrainOutcome.TimedOut:
                _log?.Invoke($"OnnxStemSeparator: an inference was still running after {_drainTimeoutMs} ms " +
                    "of drain-on-dispose. LEAKING the ONNX session instead of freeing it under a live Run.");
                return;
        }
        _htdemucs = null;   // release the chunk scratch with the session
        _releaseNative();
    }
}
