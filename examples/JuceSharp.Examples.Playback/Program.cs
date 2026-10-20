// JuceSharp.Examples.Playback — engine + file playback from a console.
//
// What this shows, in order: the device catalogue, opening an engine with the
// device-walk fallback, the master limiter (OFF at engine start since ABI v35 —
// the host opts in), output latency and what the limiter contributes to it, a
// source with an A-B loop region, Rubber Band tempo/pitch, and the callback's
// own dropout counters.
//
// Run `--help` for the arguments. `--null` needs no sound card at all.

using System.Diagnostics;
using System.Globalization;
using JuceSharp;

Console.OutputEncoding = System.Text.Encoding.UTF8;

var opts = Options.Parse(args);
if (opts is null) { PrintHelp(Console.Error); return 2; }
if (opts.Help) { PrintHelp(Console.Out); return 0; }

if (opts.File is null)
{
    Console.Error.WriteLine("error: no audio file given.\n");
    PrintHelp(Console.Error);
    return 2;
}
if (!File.Exists(opts.File))
{
    Console.Error.WriteLine($"error: audio file not found: {opts.File}");
    Console.Error.WriteLine("       Any format JUCE's basic format manager reads: WAV / FLAC / OGG / MP3 / AIFF.");
    return 2;
}

// The Null device type is registered by the engine only when this variable is set, and it is read
// once at engine start — so setting it from here, before the first engine call, is enough.
if (opts.NullDevice) Environment.SetEnvironmentVariable("DCA_ENABLE_NULL_DEVICE", "1");

try
{
    Console.WriteLine($"JuceSharp native {JuceAudioEngine.NativeVersion}");
}
catch (DllNotFoundException)
{
    Console.Error.WriteLine("error: juce_sharp.dll was not found next to this executable.");
    Console.Error.WriteLine("       Publish the example (it copies runtimes/win-x64/native/) or build the native target.");
    return 3;
}

// 1. Device catalogue: every host JUCE knows on this platform, without opening anything (ABI v13).
var hosts = JuceDeviceCatalog.Enumerate();
foreach (var host in hosts)
{
    Console.WriteLine($"[{JuceDeviceCatalog.DisplayName(host.TypeName)}] {host.OutputDevices.Count} out / {host.InputDevices.Count} in");
    foreach (var d in host.OutputDevices)
        Console.WriteLine($"    out: {d.Name}  ({d.MaxOutputChannels} ch, {d.DefaultSampleRate} Hz){(d.IsDefault ? "  [default]" : "")}");
}

// 2. Open the engine. OpenWithFallback walks the host's other devices when the requested one fails
//    (an unplugged ASIO interface is JUCE's favourite "first device").
JuceEngineFactory.OpenResult open;
try
{
    open = JuceEngineFactory.OpenWithFallback(BuildConfig(opts, hosts));
}
catch (JuceAbiMismatchException ex)
{
    // Managed and native must match EXACTLY. The DLL is a build artifact and is not tracked, so the
    // usual cause is a package update without a native rebuild, or a stale copy on the search path.
    Console.Error.WriteLine("error: the native engine's ABI version does not match this build of JuceSharp.");
    Console.Error.WriteLine($"       {ex.Message}");
    Console.Error.WriteLine("       Rebuild juce_sharp.dll (see JuceSharp/AGENTS.md § Native rebuild) and republish.");
    return 3;
}

foreach (var a in open.Attempts) Console.WriteLine("  " + a);
if (open.Engine is null)
{
    Console.Error.WriteLine("error: no audio output device could be opened.");
    Console.Error.WriteLine("       Re-run with --null to use the engine's silent Null device (headless, nothing audible).");
    return 4;
}

// Held through the INTERFACE for the rest of the run. Everything below is on IJuceAudioEngine, which
// is what lets a host unit-test the code that drives the engine with no DLL and no sound card.
// The `using` is on the concrete instance because only it is IDisposable's owner.
using var owned = open.Engine;
IJuceAudioEngine engine = owned;

engine.ResetXrunStats(); // make the counters describe THIS run, not the device open
Console.WriteLine($"Engine: {engine.SampleRate} Hz, {engine.BufferFrames} frames, out='{open.ConfigUsed.OutputDeviceId}'");

// 3. The master limiter. It is OFF at engine start (ABI v35): the stage sits on the LIVE MONITOR
//    path, so a default-ON ceiling would cost every player 1 ms of latency on their own playing.
//    A host that wants a bounded output — a streaming mirror into OBS, say, whose float path would
//    otherwise carry samples past ±1.0 into an encoder that wraps — opts in.
var (wasOn, _, _) = engine.GetMasterLimiter();
Console.WriteLine($"Master limiter at engine start: {(wasOn ? "on" : "off")} (off is correct — the host opts in)");
if (opts.Limiter)
{
    engine.SetMasterLimiter(true, ceilingDb: -1.0f, releaseMs: 80f);
    // Read it back rather than echoing what we asked for: the engine CLAMPS, so this is the only
    // honest answer about what is actually running.
    var (on, ceiling, release) = engine.GetMasterLimiter();
    Console.WriteLine($"  -> enabled={on} ceiling={ceiling:F2} dBFS release={release:F0} ms (post-clamp, read back from the engine)");
}

// Output latency INCLUDES the limiter's 1 ms look-ahead while it is on, and the limiter reports its
// own share as 0 while it is off — switched off it is not in the path at all. Anything that aligns
// audio to a clock (a click scheduler, a chart playhead) must re-read this after toggling it.
Console.WriteLine($"Output latency: {engine.OutputLatencySamples} samples total, of which {engine.MasterLimiterLatencySamples} are the limiter's look-ahead");

// 4. Play, and poke the transport on the way.
using var src = engine.OpenFile(opts.File);
Console.WriteLine($"Opened {Path.GetFileName(opts.File)}: length {src.Length:mm\\:ss\\.fff}, playing {opts.Seconds} s");
src.Volume = 0.9f;

// An A-B loop. There is no sample-accurate native loop: JuceLoopRegion polls AudiblePosition on a
// timer and seeks back, so the overshoot is bounded by PollIntervalMs and not by a sample count.
// Good enough for practice/charting, which is what it exists for.
var loopEnd = TimeSpan.FromSeconds(Math.Min(src.Length.TotalSeconds, 6));
var loopStart = TimeSpan.FromSeconds(Math.Max(0, loopEnd.TotalSeconds - 4));
if (loopEnd > loopStart)
{
    src.Loop = new JuceLoopRegion(loopStart, loopEnd, pollIntervalMs: 5);
    Console.WriteLine($"Loop region {loopStart:mm\\:ss\\.ff} → {loopEnd:mm\\:ss\\.ff} (polling, ±5 ms)");
}

src.Play();

var sw = Stopwatch.StartNew();
var tempoAt = opts.Seconds / 3.0;
var pitchAt = 2 * opts.Seconds / 3.0;
bool tempoDone = false, pitchDone = false;
while (sw.Elapsed.TotalSeconds < opts.Seconds)
{
    Thread.Sleep(500);
    // Position is the transport's read head; AudiblePosition compensates the stretcher's read-ahead
    // when tempo != 1, so it is the one to draw a playhead from.
    var reduction = engine.MasterLimiterReductionDb; // POSITIVE dB of gain reduction; 0 when idle or off
    Console.WriteLine(
        $"  t={sw.Elapsed.TotalSeconds,4:F1}s  position={src.Position:mm\\:ss\\.ff}  audible={src.AudiblePosition:mm\\:ss\\.ff}" +
        $"  tempo={src.TempoRatio:F2}x  pitch={src.PitchSemitones.ToString("+0.0;-0.0", CultureInfo.InvariantCulture)} st" +
        $"  limiterGR={reduction:F1} dB");

    if (!tempoDone && sw.Elapsed.TotalSeconds >= tempoAt)
    {
        tempoDone = true;
        src.TempoRatio = 0.8f;
        Console.WriteLine("  -> TempoRatio = 0.80 (Rubber Band time-stretch, pitch unchanged)");
    }
    if (!pitchDone && sw.Elapsed.TotalSeconds >= pitchAt)
    {
        pitchDone = true;
        src.Loop = null; // detaches and disposes the previous region
        src.PitchSemitones = -2f;
        src.Seek(TimeSpan.FromSeconds(src.Length.TotalSeconds / 2));
        Console.WriteLine("  -> loop off, PitchSemitones = -2, Seek to the middle of the file");
    }
}
src.Stop();

// 5. What did the callback see? Three counters, three different faults — never add them up.
if (engine.TryGetXrunStats(out var x))
{
    Console.WriteLine($"Dropouts: callbacks={x.Callbacks} late={x.LateCallbacks} (audible) deadlineOverruns={x.DeadlineOverruns} (our fault) nearMisses={x.NearMisses} (load warning)");
    // DeviceXruns is null, not 0, on a driver that cannot report — surfacing that as zero would be a
    // claim of a clean run this engine cannot make.
    Console.WriteLine($"          driverXruns={x.DeviceXruns?.ToString(CultureInfo.InvariantCulture) ?? "n/a (this driver cannot report)"} maxCallback={x.MaxCallbackNs / 1e6:F2} ms of a {x.BlockBudgetNs / 1e6:F2} ms block, load={x.Load:P0}");
}
else
{
    Console.WriteLine("Dropout counters unavailable (DLL predates ABI v30).");
}

// 6. Teardown order: stop the device, dispose the SOURCE, then the engine. The two `using`s above
//    already do that (C# disposes in reverse declaration order: src, then owned), and the engine's
//    own Dispose drains in-flight calls and disposes any child it still tracks as a backstop.
engine.Stop();
Console.WriteLine("Engine stopped; disposing source then engine.");
return 0;

static JuceEngineConfig BuildConfig(Options opts, IReadOnlyList<JuceAudioHost> hosts)
{
    if (opts.NullDevice)
    {
        Console.WriteLine("Using the Null device (headless, silent).");
        return new JuceEngineConfig { OutputTypeName = "Null", SampleRate = 48_000, BufferFrames = 480 };
    }
    if (!string.IsNullOrEmpty(opts.Device))
    {
        foreach (var host in hosts)
        {
            var d = host.OutputDevices.FirstOrDefault(o => o.Name.Contains(opts.Device, StringComparison.OrdinalIgnoreCase));
            if (d is null) continue;
            Console.WriteLine($"Matched '{opts.Device}': {d.Name} on {JuceDeviceCatalog.DisplayName(host.TypeName)}");
            return new JuceEngineConfig { OutputTypeName = host.TypeName, OutputDeviceId = d.Id, InputTypeName = host.TypeName };
        }
        Console.WriteLine($"No output device matches '{opts.Device}' — using the default.");
    }
    return new JuceEngineConfig();
}

static void PrintHelp(TextWriter w)
{
    w.WriteLine("JuceSharp.Examples.Playback — play a file through the engine and watch the transport.");
    w.WriteLine();
    w.WriteLine("Usage:");
    w.WriteLine("  Playback <audio file> [seconds] [options]");
    w.WriteLine();
    w.WriteLine("Arguments:");
    w.WriteLine("  <audio file>          WAV / FLAC / OGG / MP3 / AIFF.");
    w.WriteLine("  [seconds]             How long to play. Default 8.");
    w.WriteLine();
    w.WriteLine("Options:");
    w.WriteLine("  --device <substring>  Use the first output device whose name contains this.");
    w.WriteLine("  --null                Use the engine's silent \"Null\" device: no sound card, nothing");
    w.WriteLine("                        audible, runs headless. Sets DCA_ENABLE_NULL_DEVICE=1, which is");
    w.WriteLine("                        what registers that device type; you can set it yourself instead.");
    w.WriteLine("  --limiter             Opt into the master-bus limiter. It is OFF at engine start.");
    w.WriteLine("  -h, --help            This text.");
    w.WriteLine();
    w.WriteLine("Exit codes: 0 ok, 2 bad arguments, 3 native DLL missing / ABI mismatch, 4 no device.");
}

internal sealed record Options(string? File, int Seconds, string? Device, bool NullDevice, bool Limiter, bool Help)
{
    /// <summary>Returns null when an argument is malformed; the caller prints the help and fails.</summary>
    public static Options? Parse(string[] args)
    {
        string? file = null, device = null;
        var seconds = 8;
        bool nullDevice = false, limiter = false, help = false;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-h" or "--help": help = true; break;
                case "--null": nullDevice = true; break;
                case "--limiter": limiter = true; break;
                // `--device null` is the spelling the docs use; keep it as an alias of `--null`.
                case "--device" when i + 1 < args.Length && string.Equals(args[i + 1], "null", StringComparison.OrdinalIgnoreCase): nullDevice = true; i++; break;
                case "--device" when i + 1 < args.Length: device = args[++i]; break;
                case "--device": return null; // flag given with nothing after it
                default:
                    if (args[i].StartsWith('-')) return null;
                    if (file is null) file = args[i];
                    else if (int.TryParse(args[i], NumberStyles.Integer, CultureInfo.InvariantCulture, out var s) && s > 0) seconds = s;
                    else return null;
                    break;
            }
        }
        return new Options(file, seconds, device, nullDevice, limiter, help);
    }
}
