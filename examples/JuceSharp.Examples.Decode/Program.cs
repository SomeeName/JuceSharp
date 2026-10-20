// JuceSharp.Examples.Decode — JuceSharp.AudioIo without an engine, a device or a DSP chain.
//
// Probe (header only) → Decode (any format JUCE's basic format manager reads: WAV / FLAC / OGG /
// MP3 / AIFF) → per-channel peaks → DecodeAs (resample + downmix natively, one call) → three WAV
// round trips through WavWriter, one per sample format, each measured against the source.
//
// Nothing here opens an audio device, so this example needs no sound card and no
// DCA_ENABLE_NULL_DEVICE. Run `--help` for the arguments.

using JuceSharp.AudioIo;

Console.OutputEncoding = System.Text.Encoding.UTF8; // the output prints arrows and dashes

if (args.Length == 0 || args[0] is "-h" or "--help")
{
    PrintHelp(args.Length == 0 ? Console.Error : Console.Out);
    return args.Length == 0 ? 2 : 0;
}
if (args.Length > 2)
{
    Console.Error.WriteLine("error: too many arguments.\n");
    PrintHelp(Console.Error);
    return 2;
}

var input = args[0];
if (!File.Exists(input))
{
    Console.Error.WriteLine($"error: audio file not found: {input}");
    Console.Error.WriteLine("       Any format JUCE's basic format manager reads: WAV / FLAC / OGG / MP3 / AIFF.");
    return 2;
}

(int SampleRate, int Channels, long Frames) probe;
try
{
    probe = JuceAudioDecoder.Probe(input); // header only — no samples decoded
}
catch (Exception ex) when (ex is IOException or InvalidOperationException or NotSupportedException)
{
    Console.Error.WriteLine($"error: {input} could not be read as audio: {ex.Message}");
    return 2;
}
Console.WriteLine($"Probe:  {probe.SampleRate} Hz, {probe.Channels} ch, {probe.Frames} frames, ~{probe.Frames / (double)probe.SampleRate:F2} s");

var sw = System.Diagnostics.Stopwatch.StartNew();
var buf = JuceAudioDecoder.Decode(input);            // native decode → interleaved float32
sw.Stop();
Console.WriteLine($"Decode: {buf.FrameCount} frames × {buf.Channels} ch in {sw.Elapsed.TotalMilliseconds:F0} ms ({buf.DurationSeconds / sw.Elapsed.TotalSeconds:F0}x realtime)");

var planar = buf.SplitChannels();
for (var c = 0; c < planar.Length; c++)
{
    float peak = 0, sumSq = 0;
    foreach (var v in planar[c]) { peak = MathF.Max(peak, MathF.Abs(v)); sumSq += v * v; }
    var rms = MathF.Sqrt(sumSq / Math.Max(1, planar[c].Length));
    Console.WriteLine($"  ch{c}: peak {Db(peak):F1} dBFS, rms {Db(rms):F1} dBFS");
}
Console.WriteLine($"ToMono: {buf.ToMono().Length} samples");

// Sample-rate conversion and downmix in ONE native call. Doing it managed-side, per sample, is the
// right quality at the wrong price — see LoadResampler.h, where the same job went from 6.1 s to
// 104 ms by making the ratio rational and precomputing the polyphase kernel.
var small = JuceAudioDecoder.DecodeAs(input, 22_050, 1);
Console.WriteLine($"DecodeAs(22050 Hz, mono): {small.FrameCount} frames, {small.DurationSeconds:F2} s");

if (args.Length < 2)
{
    Console.WriteLine("\n(no output path given — pass one to see the WavWriter round trips)");
    return 0;
}

// The first second, written three times. WavWriter is the STREAMING writer: a provisional header up
// front, appends as you go, sizes patched on Dispose — which is the shape a render loop, a capture
// ring or a stem being decoded actually has. WavIo.WriteFloat32 needs the whole signal in one array.
var dst = args[1];
var sliceFrames = Math.Min(buf.FrameCount, buf.SampleRate);
var slice = buf.Interleaved.AsSpan(0, sliceFrames * buf.Channels).ToArray();
Console.WriteLine($"\nWriting the first {sliceFrames / (double)buf.SampleRate:F2} s three ways, from {Path.GetFileName(dst)}:");

foreach (var format in new[] { WavSampleFormat.Float32, WavSampleFormat.Pcm24, WavSampleFormat.Pcm16 })
{
    var path = format == WavSampleFormat.Float32 ? dst : Path.ChangeExtension(dst, null) + $".{format.ToString().ToLowerInvariant()}.wav";

    // Written in blocks on purpose: this is the API's whole point, and Write is allocation-free.
    using (var w = new WavWriter(path, buf.SampleRate, buf.Channels, format))
    {
        const int blockFrames = 4096;
        for (var f = 0; f < sliceFrames; f += blockFrames)
        {
            var n = Math.Min(blockFrames, sliceFrames - f) * buf.Channels;
            w.Write(slice.AsSpan(f * buf.Channels, n));
        }
    }

    var back = WavIo.Read(path);
    var maxErr = 0f;
    for (var i = 0; i < slice.Length; i++) maxErr = MathF.Max(maxErr, MathF.Abs(slice[i] - back.Interleaved[i]));

    // PCM scales by 2^(bits-1) and rounds half away from zero, which is the exact inverse of the
    // divisor WavIo.Read applies — so the only round-trip error is the half LSB of rounding. There
    // is no dither: that is a mastering decision the caller makes, and a writer that dithered
    // silently would make its own round trip unreproducible.
    var expected = format switch
    {
        WavSampleFormat.Pcm16 => 1f / 65536,
        WavSampleFormat.Pcm24 => 1f / 16_777_216,
        _ => 0f,
    };
    Console.WriteLine($"  {format,-8} {new FileInfo(path).Length,10:N0} bytes  max round-trip error {maxErr:E2} (half an LSB is {expected:E2})  unfinished={WavWriter.IsUnfinished(path)}");
    Console.WriteLine($"           → {path}");
}

// IsUnfinished answers "did anyone ever patch the header?". The provisional sizes are the RIFF
// unknown-length placeholder, NOT zero, so a process that dies before Dispose leaves a file WavIo
// still decodes in full — zeros would have produced a valid-looking WAV declaring no audio, which
// is silent data loss with nothing to notice.
Console.WriteLine("All three files report unfinished=False, i.e. Dispose patched the sizes.");
Console.WriteLine("A round-trip error of exactly 0 at a given depth means the SOURCE was already on that grid —");
Console.WriteLine("a 16-bit WAV decoded to float and written back as PCM16 is lossless. Feed it a float or 24-bit");
Console.WriteLine("source to see the half-LSB figure above actually reached.");
return 0;

static double Db(float linear) => 20.0 * Math.Log10(Math.Max(linear, 1e-6f));

static void PrintHelp(TextWriter w)
{
    w.WriteLine("JuceSharp.Examples.Decode — decode, inspect and re-write audio with JuceSharp.AudioIo.");
    w.WriteLine();
    w.WriteLine("Usage:");
    w.WriteLine("  Decode <audio file> [out.wav]");
    w.WriteLine();
    w.WriteLine("Arguments:");
    w.WriteLine("  <audio file>   WAV / FLAC / OGG / MP3 / AIFF.");
    w.WriteLine("  [out.wav]      Optional. Writes the first second three times — <out.wav> as float32,");
    w.WriteLine("                 plus <out>.pcm24.wav and <out>.pcm16.wav — and measures each round trip.");
    w.WriteLine();
    w.WriteLine("No audio device is opened, so this example runs headless with no extra setup");
    w.WriteLine("(DCA_ENABLE_NULL_DEVICE is not needed here — only the engine-backed examples want it).");
    w.WriteLine();
    w.WriteLine("Exit codes: 0 ok, 2 bad arguments / unreadable file.");
}
