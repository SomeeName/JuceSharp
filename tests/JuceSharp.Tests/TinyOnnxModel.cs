using System.Text;

namespace JuceSharp.Tests;

/// <summary>
/// Hand-encodes a tiny ONNX graph with the SAME IO layout as MrCitron's
/// waveform-only htdemucs exports — one input <c>input [batch, 2, time]</c>, one
/// output <c>output [batch, stems, ch, time]</c>, every dimension but the
/// input's channel count symbolic — so the real driver and the real ONNX
/// Runtime can be exercised in the default gate without a 300 MB download.
/// <para>
/// The graph is <c>output = Unsqueeze(input, 1) * w</c> with
/// <c>w : [1, stems, 2, 1]</c>: stem <c>s</c>, channel <c>c</c> is the input
/// channel <c>c</c> times <c>w[s, c]</c>. A weight per (stem, channel) is the
/// point — reading the output as <c>[stems, ch]</c> transposed, swapping L/R or
/// shifting a chunk all produce a sample that is not <c>w[s, c] * x</c>.
/// </para>
/// <para>
/// Written as raw protobuf (ModelProto field numbers from onnx.proto) because
/// nothing in this solution can build an ONNX graph, and a checked-in binary
/// would be an asset nobody can read.
/// </para>
/// </summary>
internal static class TinyOnnxModel
{
    public static byte[] WaveformOnlyHtdemucs(float[,] weights)
    {
        var stems = weights.GetLength(0);
        if (weights.GetLength(1) != 2) throw new ArgumentException("weights must be [stems, 2].", nameof(weights));

        var w = new byte[stems * 2 * sizeof(float)];
        for (var s = 0; s < stems; s++)
            for (var c = 0; c < 2; c++)
                BitConverter.TryWriteBytes(w.AsSpan((s * 2 + c) * sizeof(float)), weights[s, c]);

        var graph = Concat(
            Len(1, Node("Unsqueeze", ["input", "axes"], "u")),
            Len(1, Node("Mul", ["u", "w"], "output")),
            Str(2, "tiny_waveform_only_htdemucs"),
            Len(5, Tensor("axes", [1], dataType: 7, BitConverter.GetBytes(1L))),
            Len(5, Tensor("w", [1, stems, 2, 1], dataType: 1, w)),
            Len(11, ValueInfo("input", Dim("batch"), Dim(2), Dim("time"))),
            Len(12, ValueInfo("output", Dim("batch"), Dim("stems"), Dim("ch"), Dim("time"))));

        return Concat(
            Varint(1, 8),                                   // ir_version
            Len(7, graph),
            Len(8, Varint(2, 14)));                         // opset_import { version: 14 }
    }

    /// <summary>
    /// A tiny model with BS-RoFormer SW's split-complex IO: <c>spec_real</c> /
    /// <c>spec_imag</c> <c>[1, 2, F, T]</c> in, <c>out_spec_real</c> / <c>out_spec_imag</c>
    /// <c>[1, stems, 2, F, T]</c> out, every dimension static. Stem <c>s</c>, channel
    /// <c>c</c> is the input spectrogram of channel <c>c</c> times <c>weights[s, c]</c>, so
    /// after the driver's STFT and iSTFT a stem is exactly <c>weights[s, c]</c> times the mix
    /// - wherever the chunking, the slots or the binding go wrong, it is not.
    /// <paramref name="metadata"/> lands in <c>metadata_props</c>, AFTER the graph as the
    /// exporters write it.
    /// </summary>
    public static byte[] SplitComplexBsRoformer(float[,] weights, int bins, int frames,
        IReadOnlyDictionary<string, string> metadata)
    {
        var stems = weights.GetLength(0);
        if (weights.GetLength(1) != 2) throw new ArgumentException("weights must be [stems, 2].", nameof(weights));
        var w = new byte[stems * 2 * sizeof(float)];
        for (var s = 0; s < stems; s++)
            for (var c = 0; c < 2; c++)
                BitConverter.TryWriteBytes(w.AsSpan((s * 2 + c) * sizeof(float)), weights[s, c]);

        var graph = Concat(
            Len(1, Node("Unsqueeze", ["spec_real", "axes"], "ur")),
            Len(1, Node("Mul", ["ur", "w"], "out_spec_real", "mul_re")),
            Len(1, Node("Unsqueeze", ["spec_imag", "axes"], "ui", "unsqueeze_im")),
            Len(1, Node("Mul", ["ui", "w"], "out_spec_imag", "mul_im")),
            Str(2, "tiny_split_complex_bs_roformer"),
            Len(5, Tensor("axes", [1], dataType: 7, BitConverter.GetBytes(1L))),
            Len(5, Tensor("w", [1, stems, 2, 1, 1], dataType: 1, w)),
            Len(11, ValueInfo("spec_real", Dim(1), Dim(2), Dim(bins), Dim(frames))),
            Len(11, ValueInfo("spec_imag", Dim(1), Dim(2), Dim(bins), Dim(frames))),
            Len(12, ValueInfo("out_spec_real", Dim(1), Dim(stems), Dim(2), Dim(bins), Dim(frames))),
            Len(12, ValueInfo("out_spec_imag", Dim(1), Dim(stems), Dim(2), Dim(bins), Dim(frames))));

        return Concat(
        [
            Varint(1, 8),
            Len(7, graph),
            Len(8, Varint(2, 14)),
            .. metadata.Select(kv => Len(14, Concat(Str(1, kv.Key), Str(2, kv.Value)))),
        ]);
    }

    private static byte[] Node(string op, string[] inputs, string output, string name) =>
        Concat([.. inputs.Select(i => Str(1, i)), Str(2, output), Str(3, name), Str(4, op)]);

    private static byte[] Node(string op, string[] inputs, string output) =>
        Concat([.. inputs.Select(i => Str(1, i)), Str(2, output), Str(3, op + "_0"), Str(4, op)]);

    private static byte[] Tensor(string name, long[] dims, int dataType, byte[] raw) =>
        Concat([.. dims.Select(d => Varint(1, (ulong)d)), Varint(2, (ulong)dataType), Str(8, name), Len(9, raw)]);

    private static byte[] ValueInfo(string name, params byte[][] dims)
    {
        var shape = Concat([.. dims.Select(d => Len(1, d))]);
        var tensorType = Concat(Varint(1, 1), Len(2, shape));  // elem_type FLOAT, shape
        return Concat(Str(1, name), Len(2, Len(1, tensorType)));
    }

    private static byte[] Dim(long value) => Varint(1, (ulong)value);

    private static byte[] Dim(string param) => Str(2, param);

    private static byte[] Str(int field, string value) => Len(field, Encoding.UTF8.GetBytes(value));

    private static byte[] Len(int field, byte[] payload) =>
        Concat(RawVarint((ulong)((field << 3) | 2)), RawVarint((ulong)payload.Length), payload);

    private static byte[] Varint(int field, ulong value) => Concat(RawVarint((ulong)(field << 3)), RawVarint(value));

    private static byte[] RawVarint(ulong value)
    {
        var bytes = new List<byte>(10);
        do
        {
            var b = (byte)(value & 0x7F);
            value >>= 7;
            bytes.Add(value != 0 ? (byte)(b | 0x80) : b);
        }
        while (value != 0);
        return [.. bytes];
    }

    private static byte[] Concat(params byte[][] parts) => [.. parts.SelectMany(p => p)];
}
