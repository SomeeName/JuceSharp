using Microsoft.ML.OnnxRuntime;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// The split-complex BS-RoFormer call through an <see cref="OrtIoBinding"/>: per SLOT, two
/// input planes and two output planes, arrays allocated ONCE and pinned as
/// <see cref="OrtValue"/>s, so a chunk allocates nothing and ONNX Runtime copies the results
/// straight into memory the driver owns.
/// <para>
/// <b>Why.</b> <c>InferenceSession.Run</c> hands back freshly allocated output tensors: for
/// SW at the 13.35 s training chunk that is two 57 MB buffers per chunk, touched page by
/// page on the first write, and they must be disposed before the next call. Two slots let
/// the driver prepare chunk <c>k + 1</c>'s input and read chunk <c>k - 1</c>'s output while
/// the model runs chunk <c>k</c> (<see cref="ChunkPipeline"/>).
/// </para>
/// </summary>
internal sealed class SplitComplexIoBinding : IDisposable
{
    private readonly InferenceSession _session;
    private readonly OrtIoBinding _binding;
    private readonly string[] _inputNames;
    private readonly string[] _outputNames;
    private readonly OrtValue[][] _inputs;
    private readonly OrtValue[][] _outputs;

    /// <param name="session">The loaded split-complex graph; not owned.</param>
    /// <param name="layout">Its tensor names and stem count.</param>
    /// <param name="bins">Frequency bins (<c>F</c>).</param>
    /// <param name="frames">Frames per chunk (<c>T</c>).</param>
    /// <param name="slots">Input/output slots; two for <see cref="ChunkPipeline"/>.</param>
    public SplitComplexIoBinding(InferenceSession session, BsRoformerIoBinding layout, int bins, int frames, int slots = 2)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentNullException.ThrowIfNull(layout);
        if (layout.Layout != BsRoformerIoLayout.SplitComplex)
            throw new ArgumentException("Only the split-complex layout binds two planes.", nameof(layout));
        _session = session;
        _inputNames = [.. layout.InputNames];
        _outputNames = [.. layout.OutputNames];
        long[] inShape = [1, 2, bins, frames];
        long[] outShape = [1, layout.Stems, 2, bins, frames];

        InputRe = new float[slots][];
        InputIm = new float[slots][];
        OutputRe = new float[slots][];
        OutputIm = new float[slots][];
        _inputs = new OrtValue[slots][];
        _outputs = new OrtValue[slots][];
        _binding = session.CreateIoBinding();
        try
        {
            for (var s = 0; s < slots; s++)
            {
                InputRe[s] = new float[2 * bins * frames];
                InputIm[s] = new float[2 * bins * frames];
                _inputs[s] =
                [
                    OrtValue.CreateTensorValueFromMemory(InputRe[s], inShape),
                    OrtValue.CreateTensorValueFromMemory(InputIm[s], inShape),
                ];
                OutputRe[s] = new float[layout.Stems * 2 * bins * frames];
                OutputIm[s] = new float[layout.Stems * 2 * bins * frames];
                _outputs[s] =
                [
                    OrtValue.CreateTensorValueFromMemory(OutputRe[s], outShape),
                    OrtValue.CreateTensorValueFromMemory(OutputIm[s], outShape),
                ];
            }
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    /// <summary>Per slot, the real input plane <c>[1, 2, F, T]</c>; fill it before <see cref="Run"/>.</summary>
    public float[][] InputRe { get; }

    /// <summary>Per slot, the imaginary input plane.</summary>
    public float[][] InputIm { get; }

    /// <summary>Per slot, the real output planes <c>[1, stems, 2, F, T]</c>.</summary>
    public float[][] OutputRe { get; }

    /// <summary>Per slot, the imaginary output planes.</summary>
    public float[][] OutputIm { get; }

    /// <summary>Runs the model on <paramref name="slot"/>'s input into its outputs.
    /// Cancellation interrupts the run (<see cref="SpectralOnnxRunner.RequestTerminate"/>)
    /// and throws <see cref="OperationCanceledException"/>.</summary>
    public void Run(int slot, CancellationToken ct)
    {
        // Bound per run, AFTER the caller filled the planes: binding a CPU input to a session
        // on a GPU provider copies it to the device AT BIND TIME, so a binding made once up
        // front feeds the model the planes as they were then (zeros) on every chunk.
        _binding.BindInput(_inputNames[0], _inputs[slot][0]);
        _binding.BindInput(_inputNames[1], _inputs[slot][1]);
        _binding.BindOutput(_outputNames[0], _outputs[slot][0]);
        _binding.BindOutput(_outputNames[1], _outputs[slot][1]);
        using var runOptions = new RunOptions();
        using var registration = ct.Register(SpectralOnnxRunner.RequestTerminate, runOptions);
        try
        {
            _session.RunWithBinding(runOptions, _binding);
        }
        catch (OnnxRuntimeException) when (ct.IsCancellationRequested)
        {
            // A terminated run may report itself as a failure; the caller asked for a cancel.
        }
        ct.ThrowIfCancellationRequested();
    }

    /// <summary>Releases the binding and the pinned tensors (the arrays stay the GC's).</summary>
    public void Dispose()
    {
        _binding?.Dispose();
        foreach (var set in new[] { _inputs, _outputs })
        {
            if (set == null) continue;
            foreach (var slot in set)
            {
                if (slot == null) continue;
                foreach (var value in slot) value?.Dispose();
            }
        }
    }
}
