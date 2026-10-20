// JuceSharp.Examples.Spectrogram — offline time/frequency analysis (JuceSharp.Dsp, pure managed).
//
// Decode → JuceSpectrogramBuilder (the Hann-windowed STFT over JuceFft) → a PGM image you can open
// in anything → optional CSV. Plus one frame computed with JuceFft directly, to show what the
// builder is doing, and JuceWaveformPeaks for a waveform view.
//
// The image is a PGM on purpose: it is eight lines of code and no dependency, where a PNG would mean
// pulling an imaging package into an example whose subject is the DSP.
//
// Nothing here opens an audio device, so this example runs headless with no setup.
// Run `--help` for the arguments.

using JuceSharp.AudioIo;
using JuceSharp.Dsp;

Console.OutputEncoding = System.Text.Encoding.UTF8;

var opts = Options.Parse(args);
if (opts is null) { PrintHelp(Console.Error); return 2; }
if (opts.Help) { PrintHelp(Console.Out); return 0; }
if (opts.Input is null) { Console.Error.WriteLine("error: no audio file given.\n"); PrintHelp(Console.Error); return 2; }
if (!File.Exists(opts.Input))
{
    Console.Error.WriteLine($"error: audio file not found: {opts.Input}");
    Console.Error.WriteLine("       Any format JUCE's basic format manager reads: WAV / FLAC / OGG / MP3 / AIFF.");
    return 2;
}

AudioBuffer buf;
try
{
    // Mono at 22.05 kHz is plenty for analysis and quarters the work; DecodeAs resamples and
    // downmixes natively in one call.
    buf = JuceAudioDecoder.DecodeAs(opts.Input, 22_050, 1);
}
catch (Exception ex) when (ex is IOException or InvalidOperationException or NotSupportedException)
{
    Console.Error.WriteLine($"error: {opts.Input} could not be decoded: {ex.Message}");
    return 2;
}
var mono = buf.Interleaved;
Console.WriteLine($"Decoded {Path.GetFileName(opts.Input)}: {buf.DurationSeconds:F2} s mono @ {buf.SampleRate} Hz");

if (mono.Length < opts.FftSize * 2)
{
    Console.Error.WriteLine($"error: too short — need at least two {opts.FftSize}-sample windows, got {mono.Length} samples.");
    return 2;
}

// ------------------------------------------------------------------ the STFT

var builder = new JuceSpectrogramBuilder(fftSize: opts.FftSize, hopSize: opts.FftSize / 2);
var sw = System.Diagnostics.Stopwatch.StartNew();
var sg = builder.Build(mono, buf.SampleRate, decibelScale: true);
sw.Stop();
Console.WriteLine($"STFT: {sg.FrameCount} frames × {sg.BinCount} bins (dB), {sg.BinHz(1):F1} Hz/bin, {sg.FrameTimeSeconds(1) * 1000:F1} ms/frame, in {sw.Elapsed.TotalMilliseconds:F0} ms");

// The same thing one frame at a time, so it is clear what the builder is: JuceFft.Magnitude over a
// windowed slice. JuceFft is the in-house real FFT the whole Dsp assembly runs on — the 0.9 wave
// replaced MathNet with it and halved the scratch while dropping the per-chunk garbage entirely.
var fft = new JuceFft(opts.FftSize);
var frame0 = new float[opts.FftSize];
// Symmetric Hann, computed here because JuceSharp.Dsp.Windows is internal to the assembly. Window
// first: an unwindowed slice leaks its discontinuity across every bin.
for (var i = 0; i < opts.FftSize; i++)
    frame0[i] = mono[i] * (float)(0.5 - 0.5 * Math.Cos(2 * Math.PI * i / (opts.FftSize - 1)));
var mag = fft.Magnitude(frame0);
Console.WriteLine($"  JuceFft.Magnitude on frame 0 directly: {mag.Length} bins, DC {mag[0]:E2}, bin 1 {mag[1]:E2} (linear, not dB)");

// Loudest bin of the loudest frame — a cheap "what is the dominant frequency" readout.
var bestFrame = 0; var bestBin = 0; var best = float.MinValue;
for (var f = 0; f < sg.FrameCount; f++)
    for (var b = 1; b < sg.BinCount; b++)
        if (sg.Frames[f][b] > best) { best = sg.Frames[f][b]; bestFrame = f; bestBin = b; }
Console.WriteLine($"  peak {best:F1} dB at {sg.FrameTimeSeconds(bestFrame):F2} s, {sg.BinHz(bestBin):F0} Hz");

var peaks = JuceWaveformPeaks.Build(mono, buf.Channels, buf.SampleRate, bucketMilliseconds: 50);
Console.WriteLine($"Waveform peaks: {peaks.Length} buckets (50 ms each), first bucket min/max {peaks[0].Min:F2}/{peaks[0].Max:F2}");

// ------------------------------------------------------------------ the image

var pgmPath = opts.Image ?? Path.ChangeExtension(opts.Input, ".pgm");
WritePgm(pgmPath, sg, opts.DynamicRangeDb);
Console.WriteLine($"Wrote {pgmPath}: {sg.FrameCount}×{sg.BinCount} 8-bit PGM, {opts.DynamicRangeDb:F0} dB of range below the loudest bin, low frequencies at the BOTTOM.");

if (opts.Csv is not null)
{
    using var w = new StreamWriter(opts.Csv);
    w.Write("time_s");
    for (var b = 0; b < sg.BinCount; b++) w.Write($",{sg.BinHz(b):F0}Hz");
    w.WriteLine();
    for (var f = 0; f < sg.FrameCount; f++)
    {
        w.Write($"{sg.FrameTimeSeconds(f):F3}");
        foreach (var v in sg.Frames[f]) w.Write($",{v:F2}");
        w.WriteLine();
    }
    Console.WriteLine($"Wrote {opts.Csv}");
}
return 0;

/// <summary>
/// Binary PGM (P5): a two-line ASCII header then one byte per pixel. Column = frame, row = bin with
/// bin 0 at the bottom, which is the orientation every spectrogram is read in.
/// </summary>
static void WritePgm(string path, JuceSpectrogram sg, double dynamicRangeDb)
{
    var max = float.MinValue;
    foreach (var frame in sg.Frames)
        foreach (var v in frame)
            if (float.IsFinite(v) && v > max) max = v;
    var floor = max - dynamicRangeDb;

    using var s = File.Create(path);
    using var w = new BinaryWriter(s);
    w.Write(System.Text.Encoding.ASCII.GetBytes($"P5\n{sg.FrameCount} {sg.BinCount}\n255\n"));
    var row = new byte[sg.FrameCount];
    for (var b = sg.BinCount - 1; b >= 0; b--) // top row of the image is the HIGHEST bin
    {
        for (var f = 0; f < sg.FrameCount; f++)
        {
            var v = sg.Frames[f][b];
            // A non-finite bin is drawn as black rather than skipped: a gap in the image is a
            // visible fault, where a silently reused previous value is not.
            var t = float.IsFinite(v) ? (v - floor) / (float)dynamicRangeDb : 0f;
            row[f] = (byte)Math.Clamp((int)(t * 255f + 0.5f), 0, 255);
        }
        w.Write(row);
    }
}

static void PrintHelp(TextWriter w)
{
    w.WriteLine("JuceSharp.Examples.Spectrogram — STFT a file and write it out as an image.");
    w.WriteLine();
    w.WriteLine("Usage:");
    w.WriteLine("  Spectrogram <audio file> [options]");
    w.WriteLine();
    w.WriteLine("Options:");
    w.WriteLine("  --image <out.pgm>   Where to write the greyscale image. Default: <input>.pgm.");
    w.WriteLine("  --csv <out.csv>     Also dump every frame as CSV (large — one column per bin).");
    w.WriteLine("  --fft <N>           FFT size, a power of two, 256..8192. Default 1024. Hop is N/2.");
    w.WriteLine("  --range <dB>        Dynamic range below the loudest bin. Default 80.");
    w.WriteLine("  -h, --help          This text.");
    w.WriteLine();
    w.WriteLine("No audio device is opened, so this example runs headless with no extra setup");
    w.WriteLine("(DCA_ENABLE_NULL_DEVICE is not needed here — only the engine-backed examples want it).");
    w.WriteLine();
    w.WriteLine("Exit codes: 0 ok, 2 bad arguments / unreadable file.");
}

internal sealed record Options(string? Input, string? Image, string? Csv, int FftSize, double DynamicRangeDb, bool Help)
{
    /// <summary>Returns null when an argument is malformed; the caller prints the help and fails.</summary>
    public static Options? Parse(string[] args)
    {
        string? input = null, image = null, csv = null;
        var fftSize = 1024;
        var range = 80.0;
        var help = false;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-h" or "--help": help = true; break;
                case "--image" when i + 1 < args.Length: image = args[++i]; break;
                case "--csv" when i + 1 < args.Length: csv = args[++i]; break;
                case "--fft" when i + 1 < args.Length && int.TryParse(args[i + 1], out var n) && n is >= 256 and <= 8192 && (n & (n - 1)) == 0:
                    fftSize = n; i++; break;
                case "--range" when i + 1 < args.Length && double.TryParse(args[i + 1], System.Globalization.CultureInfo.InvariantCulture, out var r) && r > 0:
                    range = r; i++; break;
                default:
                    if (args[i].StartsWith('-') || input is not null) return null;
                    input = args[i];
                    break;
            }
        }
        return new Options(input, image, csv, fftSize, range, help);
    }
}
