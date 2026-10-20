// JuceSharp.Examples.AnalyzeChords — musical analysis with JuceSharp.Dsp (pure managed).
//
// Decode (resampled to mono) → JuceBeatDetector (BPM, tracked beats, TEMPO MAP and DOWNBEATS, 0.9)
// → JuceOnsetDetector through its allocation-free SPAN overload → JuceChromagram + JuceChordDetector
// over one-second hops → JuceTunerStream over the whole file, which is the streaming shape a live
// tuner actually has.
//
// Nothing here opens an audio device, so this example runs headless with no setup.
// Run `--help` for the arguments.

using JuceSharp.AudioIo;
using JuceSharp.Dsp;

Console.OutputEncoding = System.Text.Encoding.UTF8;

if (args.Length == 0 || args[0] is "-h" or "--help")
{
    PrintHelp(args.Length == 0 ? Console.Error : Console.Out);
    return args.Length == 0 ? 2 : 0;
}
if (args.Length > 1)
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

Console.WriteLine($"Decoding {input} ...");
AudioBuffer buf;
try
{
    buf = JuceAudioDecoder.DecodeAs(input, 22_050, 1);
}
catch (Exception ex) when (ex is IOException or InvalidOperationException or NotSupportedException)
{
    Console.Error.WriteLine($"error: {input} could not be decoded: {ex.Message}");
    return 2;
}
var mono = buf.Interleaved;
var sr = buf.SampleRate;
Console.WriteLine($"  → {sr} Hz mono, {buf.DurationSeconds:F2} s");

// ------------------------------------------------------------------ beats, tempo map, downbeats

const int beatFft = 1024;
if (mono.Length < beatFft * 4)
{
    Console.Error.WriteLine($"error: too short for beat detection — need at least four {beatFft}-sample windows.");
    return 2;
}

var beats = new JuceBeatDetector(sr) { MinBpm = 60, MaxBpm = 200 }.Analyze(mono);

// Confidence exists because this detector used to fabricate a whole plausible analysis for SILENCE
// with no way to tell. Treat a low value as "this audio may not have a beat" — never as an error.
Console.WriteLine($"Beats: {beats.Bpm:F1} BPM global, confidence {beats.Confidence:F2}, {beats.Beats.Count} tracked beats");
if (beats.Confidence < 0.1f)
    Console.WriteLine("  (low confidence — the onset envelope was barely periodic; read everything below with that in mind)");
Console.WriteLine($"  first beats: {string.Join(", ", beats.Beats.Take(6).Select(b => b.TotalSeconds.ToString("F2")))} s");

// The tempo MAP is the 0.9 addition, and it is the reason Beats is no longer a fixed grid: a global
// period stepped from the first half-beat cannot represent a song that speeds up, and every beat
// past a tempo change drifted. A steady song yields exactly one segment.
Console.WriteLine($"  tempo map: {beats.TempoMap.Count} segment(s)");
foreach (var seg in beats.TempoMap.Take(8))
    Console.WriteLine($"    from {seg.Time.TotalSeconds,7:F2} s  {seg.Bpm:F1} BPM");
if (beats.TempoMap.Count > 8) Console.WriteLine($"    … {beats.TempoMap.Count - 8} more");

Console.WriteLine(beats.BeatsPerBar > 0
    ? $"  meter: {beats.BeatsPerBar}/4, {beats.Downbeats.Count} downbeats; first: {string.Join(", ", beats.Downbeats.Take(6).Select(b => b.TotalSeconds.ToString("F2")))} s"
    : "  meter: not judged (too few beats, or the fixed-grid mode, which has no evidence about bars)");

// ------------------------------------------------------------------ onsets, through the span API

// The streaming detector takes blocks of ANY size. Push(span) allocates a list per call; the span
// overload writes into a buffer the caller owns, which is the one to use on a live thread.
// RequiredOnsetCapacity sizes that buffer for the worst case a block of this length can produce —
// guess it and the detector counts what it had to drop instead of growing.
const int block = 4096;
var onsets = new JuceOnsetDetector(sr);
Span<JuceOnset> onsetBuffer = stackalloc JuceOnset[JuceOnsetDetector.RequiredOnsetCapacity(block)];
var found = new List<JuceOnset>();
for (var i = 0; i < mono.Length; i += block)
{
    onsets.Push(mono.AsSpan(i, Math.Min(block, mono.Length - i)), onsetBuffer, out var n);
    for (var k = 0; k < n; k++) found.Add(onsetBuffer[k]);
}
Console.WriteLine($"Onsets: {found.Count} (spectral flux, {JuceOnsetDetector.FftSize}-pt FFT / {JuceOnsetDetector.HopSize} hop, "
                + $"{JuceOnsetDetector.LatencySamples} samples of reporting latency, {onsets.DroppedOnsets} dropped)");
foreach (var o in found.Take(6))
    Console.WriteLine($"  {o.SamplePosition / (double)sr,7:F3} s  strength {o.Strength:F2}");
if (found.Count > 6) Console.WriteLine($"  … {found.Count - 6} more");

// ------------------------------------------------------------------ chords

const int fft = 4096;
var chroma = new JuceChromagram(sr, fft);
var detector = new JuceChordDetector { MinConfidence = 0.55f };
Console.WriteLine($"Chords (1 s hops, {fft}-sample windows, confidence ≥ {detector.MinConfidence:F2}):");
for (var start = 0; start + fft <= mono.Length; start += sr)
{
    var cv = chroma.Compute(mono.AsSpan(start, fft));
    var reading = detector.Detect(cv);
    var label = reading is { } r ? $"{r.Name,-6} {r.Quality,-10} ({r.Confidence:F2})" : "(no chord)";
    Console.WriteLine($"  {start / (double)sr,6:F1}s  {label}");
}

// ------------------------------------------------------------------ tuner, streamed

// JuceTunerStream (0.9) is JuceChromaticTuner with the hysteresis a tuner UI needs: a reading has to
// hold for NoteChangeFrames before the displayed note moves, so a passing overtone cannot make the
// display flicker, while a jump of more than ImmediateChangeCents is taken at once because that is a
// new note and not noise. Pushing a decoded FILE through it is the same code path a live input
// takes — which is exactly why it is worth doing here.
var tunerStream = new JuceTunerStream(sr, windowSize: 2048) { NoteChangeFrames = 3, StableFrames = 3 };
var readings = new List<(double Time, JuceTunerFrame Frame)>();
string? lastNote = null;
for (var i = 0; i < mono.Length; i += block)
{
    if (!tunerStream.Push(mono.AsSpan(i, Math.Min(block, mono.Length - i)))) continue;
    if (tunerStream.Latest is not { } frame) continue;
    var note = $"{frame.NoteName}{frame.Octave}";
    if (note == lastNote) continue;             // only log the transitions, not every frame
    lastNote = note;
    // The frame carries no timestamp; StreamPosition is the detector's own sample cursor since the
    // last Reset, which is the clock a live caller aligns against too.
    readings.Add((tunerStream.StreamPosition / (double)sr, frame));
}
Console.WriteLine($"Tuner: {tunerStream.FramesEmitted} frames analysed, {readings.Count} note changes "
                + $"(monophonic — on a chord YIN reports the strongest periodicity, usually the bass note):");
foreach (var (time, f) in readings.Take(10))
    Console.WriteLine($"  {time,6:F2}s  {f.NoteName}{f.Octave,-2} {f.FrequencyHz,7:F1} Hz  {f.CentsOff,6:+0.0;-0.0} cents  confidence {f.Confidence:F2}");
if (readings.Count > 10) Console.WriteLine($"  … {readings.Count - 10} more");
return 0;

static void PrintHelp(TextWriter w)
{
    w.WriteLine("JuceSharp.Examples.AnalyzeChords — beats, tempo map, onsets, chords and pitch from a file.");
    w.WriteLine();
    w.WriteLine("Usage:");
    w.WriteLine("  AnalyzeChords <audio file>");
    w.WriteLine();
    w.WriteLine("Arguments:");
    w.WriteLine("  <audio file>   WAV / FLAC / OGG / MP3 / AIFF. Needs at least ~0.2 s of audio.");
    w.WriteLine();
    w.WriteLine("No audio device is opened, so this example runs headless with no extra setup");
    w.WriteLine("(DCA_ENABLE_NULL_DEVICE is not needed here — only the engine-backed examples want it).");
    w.WriteLine();
    w.WriteLine("Exit codes: 0 ok, 2 bad arguments / unreadable or too-short file.");
}
