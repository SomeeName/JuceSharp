// JuceSharp.Examples.ToneRig — the in-house reorderable DSP tone rig from a console.
//
// The rig is a seat's guitar-INPUT chain, reached with engine.GetDspChain(seat).
// Everything here goes through the JuceSharp wrapper — JuceDspModuleCatalog for
// the palette, JuceDspChain for the rig, JuceDspNode for one pedal. This example
// used to ship its own 235-line hand-written C-ABI binding; that file is gone.
//
// What it shows (0.9 / ABI v34-35):
//   * a whole preset built inside ONE edit scope, so the board changes as a
//     single crossfade instead of once per pedal;
//   * a parked insert (added at zero wet, configured, THEN faded in);
//   * a compressor keyed off the seat's dry input (sidechain), gated on the
//     catalogue's own capability flag;
//   * per-node meters, chain latency, and the engine's node-type/bypass getters;
//   * branch latency compensation across a parallel region, on and off;
//   * an automated offline render written out with WavWriter.
//
// Run `--help` for the arguments. `--offline` and `--catalog` need no sound card.

using System.Diagnostics;
using System.Globalization;
using JuceSharp;
using JuceSharp.AudioIo;
using JuceSharp.Dsp;

const int Seat = 0; // seat 0 == the pre-v28 solo chain; seats 1..3 are the co-op players

Console.OutputEncoding = System.Text.Encoding.UTF8;

var opts = Options.Parse(args);
if (opts is null) { PrintHelp(Console.Error); return 2; }
if (opts.Help) { PrintHelp(Console.Out); return 0; }

try
{
    Console.WriteLine($"JuceSharp native {JuceAudioEngine.NativeVersion}");
}
catch (DllNotFoundException)
{
    Console.Error.WriteLine("error: juce_sharp.dll was not found next to this executable.");
    return 3;
}

// The palette is STATIC: read once from native and cached for the process, no engine and no device.
// A pedalboard UI can therefore be built before anything is opened.
IReadOnlyList<JuceDspModuleInfo> catalogue;
try
{
    catalogue = JuceDspModuleCatalog.Modules;
}
catch (JuceAbiMismatchException ex)
{
    Console.Error.WriteLine("error: the native engine's ABI version does not match this build of JuceSharp.");
    Console.Error.WriteLine($"       {ex.Message}");
    Console.Error.WriteLine("       Rebuild juce_sharp.dll (see JuceSharp/AGENTS.md § Native rebuild) and republish.");
    return 3;
}
Console.WriteLine($"DSP module catalogue: {catalogue.Count} module types (static, no engine needed)");
if (opts.Catalog) { PrintCatalogue(catalogue); return 0; }

if (opts.OfflineIn is not null)
{
    if (!File.Exists(opts.OfflineIn))
    {
        Console.Error.WriteLine($"error: input file not found: {opts.OfflineIn}");
        return 2;
    }
    return RunOffline(opts.OfflineIn, opts.OfflineOut!);
}
return RunLive(opts);

// ------------------------------------------------------------------ live input

static int RunLive(Options opts)
{
    // A rig with nothing to process is not an error — say so and leave.
    var inputs = JuceAudioEngine.EnumerateInputDevices();
    if (inputs.Count == 0 && !opts.NullDevice)
    {
        Console.Error.WriteLine("error: no audio INPUT device on this machine — the rig has nothing to process.");
        Console.Error.WriteLine("       Re-run with --offline <in> <out>, or --null for a silent headless engine.");
        return 4;
    }

    if (opts.NullDevice) Environment.SetEnvironmentVariable("DCA_ENABLE_NULL_DEVICE", "1");

    var open = JuceEngineFactory.OpenWithFallback(BuildConfig(opts));
    foreach (var a in open.Attempts) Console.WriteLine("  " + a);
    if (open.Engine is null)
    {
        Console.Error.WriteLine("error: no audio device could be opened (input + output).");
        Console.Error.WriteLine("       Re-run with --null to use the engine's silent Null device.");
        return 4;
    }

    using var engine = open.Engine;
    Console.WriteLine($"Engine: {engine.SampleRate} Hz, {engine.BufferFrames} frames, out='{open.ConfigUsed.OutputDeviceId}' in='{open.ConfigUsed.InputDeviceId}'");
    engine.ResetXrunStats();
    engine.SetInputGain(1f);
    engine.SetMonitorGain(0.8f); // the rig sits on the input path; the monitor is how you hear it

    var chain = engine.GetDspChain(Seat);
    var nodes = BuildPreset(chain);
    var delay = InsertParkedDelay(chain, beforeIndex: nodes.Count - 1);
    KeyTheCompressorOffTheDryInput(nodes);
    PrintChain(chain);

    // v30 meters are OFF until asked for; while off the callback is byte-for-byte the pre-v30 path.
    chain.MeteringEnabled = true;
    Console.WriteLine($"\nPlay something — polling per-node meters for {opts.Seconds} s (peak dBFS in → out per node):");
    var meters = new JuceDspNodeMeter[chain.NodeCount];
    var sw = Stopwatch.StartNew();
    var toggled = false;
    while (sw.Elapsed.TotalSeconds < opts.Seconds)
    {
        Thread.Sleep(250);
        var inPeak = engine.GetAndResetInputPeak();
        chain.TryReadMeters(meters, out var count); // ONE P/Invoke for the whole frame, in chain order
        var line = $"  t={sw.Elapsed.TotalSeconds,4:F1}s in={Db(inPeak),6:F1}";
        for (var i = 0; i < Math.Min(count, meters.Length); i++)
            line += $" | {Db(meters[i].InputPeak),6:F1}→{Db(meters[i].OutputPeak),6:F1}";
        line += $" | latency {chain.LatencySamples} smp";
        Console.WriteLine(line);

        // Half-way: bypass the delay again (10 ms fade, trails let the echoes decay), un-bypass 1 s later.
        if (!toggled && sw.Elapsed.TotalSeconds >= opts.Seconds / 2.0)
        {
            toggled = true;
            delay.Bypass = true;
            Console.WriteLine("  -> delay bypassed (trails=On, so the tail decays instead of cutting)");
            Task.Delay(1000).ContinueWith(_ => { try { delay.Bypass = false; } catch (ObjectDisposedException) { /* engine gone */ } });
        }
    }

    chain.MeteringEnabled = false; // also zeroes the meters
    PrintXruns(engine);

    chain.Clear();
    engine.Stop();       // stop the device first; the using-dispose then drains children + destroys the engine
    Console.WriteLine("Rig cleared, engine stopped and disposed.");
    return 0;
}

// ------------------------------------------------------------------ offline render

static int RunOffline(string inPath, string outPath)
{
    AudioBuffer src;
    try
    {
        src = JuceAudioDecoder.Decode(inPath);
    }
    catch (Exception ex) when (ex is IOException or InvalidOperationException)
    {
        Console.Error.WriteLine($"error: could not decode {inPath}: {ex.Message}");
        return 2;
    }
    Console.WriteLine($"Decoded {Path.GetFileName(inPath)}: {src.SampleRate} Hz, {src.Channels} ch, {src.DurationSeconds:F2} s");
    if (src.Channels is not (1 or 2)) { Console.Error.WriteLine("error: the rig renders mono or stereo only."); return 2; }

    // JuceDspChain.ProcessOffline refuses an OPEN device — deliberately: its chunk loop takes the
    // chain's process barrier once per 512 frames, which would contend a live callback thousands of
    // times. The engine's own "Null" device type (registered only under DCA_ENABLE_NULL_DEVICE=1)
    // gives us an engine without touching a sound card.
    Environment.SetEnvironmentVariable("DCA_ENABLE_NULL_DEVICE", "1");
    JuceAudioEngine engine;
    try
    {
        engine = new JuceAudioEngine(new JuceEngineConfig { OutputTypeName = "Null", SampleRate = src.SampleRate, BufferFrames = 512, EnableConsoleLog = false });
        Console.WriteLine("Engine opened on the Null device (no hardware).");
    }
    catch (JuceAudioException ex)
    {
        Console.WriteLine($"Null device unavailable ({ex.Message}); opening the default device just to have an engine.");
        try { engine = new JuceAudioEngine(new JuceEngineConfig { EnableConsoleLog = false }); }
        catch (JuceAudioException ex2) { Console.Error.WriteLine($"error: no engine could be created: {ex2.Message}"); return 4; }
    }

    using (engine)
    {
        engine.Stop(); // OFFLINE MEANS OFFLINE: the offline path must be the only processor

        var chain = engine.GetDspChain(Seat);
        var nodes = BuildPreset(chain);
        var delay = InsertParkedDelay(chain, beforeIndex: nodes.Count - 1);
        KeyTheCompressorOffTheDryInput(nodes);
        PrintChain(chain);

        // --- a static render ------------------------------------------------
        var rendered = new float[src.Interleaved.Length];
        var sw = Stopwatch.StartNew();
        chain.ProcessOffline(src.Interleaved, src.Channels, src.SampleRate, rendered); // chunked natively
        sw.Stop();
        Console.WriteLine($"\nStatic render: {src.DurationSeconds:F2} s in {sw.Elapsed.TotalMilliseconds:F0} ms ({src.DurationSeconds / sw.Elapsed.TotalSeconds:F1}x realtime); chain latency {chain.LatencySamples} samples @ {src.SampleRate} Hz");
        WriteWav(outPath, rendered, src.SampleRate, src.Channels, WavSampleFormat.Pcm24);
        var outPeak = Db(Peak(rendered));
        Console.WriteLine($"Wrote {outPath} (24-bit PCM; peak in {Db(Peak(src.Interleaved)):F1} dBFS → out {outPeak:F1} dBFS)");
        if (outPeak > 0)
            // The rig has gain and nothing here bounds it: 24-bit PCM clamps at full scale, so this
            // file clips. On the LIVE path that is what engine.SetMasterLimiter is for — it is OFF
            // at engine start (ABI v35) and the host opts in. There is no limiter on ProcessOffline:
            // an offline render is the caller's to normalise.
            Console.WriteLine("  NOTE: the rendered peak is above 0 dBFS, so the PCM file clips. Trim the amp/cab output, or "
                            + "normalise — the offline path has no master limiter.");

        // --- the same take, automated ---------------------------------------
        // A static render is the one thing a re-amp is never asked to do. The automation overload
        // chunks the render at each event boundary, so an event at frame F is heard BY sample F, and
        // every event goes through the SAME setParam mailbox the live callback's setters use — which
        // is what stops an automated render and a live performance of the same gesture diverging.
        var autoPath = Path.ChangeExtension(outPath, null) + ".auto.wav";
        var automated = (float[])src.Interleaved.Clone(); // the automation overload renders IN PLACE
        var quarter = src.FrameCount / 4;
        var events = new[]
        {
            // The list MUST be sorted by non-decreasing frame: an unsorted list is rejected rather
            // than silently reordered, because a caller who thinks it is automating a sweep and is
            // actually automating a scramble gets a render that is plausible and wrong.
            JuceDspAutomationEvent.Create(0,             nodes[^1].Id, "mix",    0.05f),
            JuceDspAutomationEvent.Create(quarter,       nodes[^1].Id, "mix",    0.45f),  // reverb swells
            JuceDspAutomationEvent.Create(2 * quarter,   delay.Id,     "bypass", 1f),     // footswitch OFF
            JuceDspAutomationEvent.Create(3 * quarter,   delay.Id,     "bypass", 0f),     // and back ON
        };
        try
        {
            sw.Restart();
            chain.ProcessOffline(automated, src.Channels, src.SampleRate, events);
            sw.Stop();
            WriteWav(autoPath, automated, src.SampleRate, src.Channels, WavSampleFormat.Float32);
            Console.WriteLine($"Automated render: {events.Length} events in {sw.Elapsed.TotalMilliseconds:F0} ms → {autoPath} (float32)");
            Console.WriteLine("  \"bypass\" is a RESERVED param id routed to the chain's own bypass — no module declares one.");
        }
        catch (JuceAudioException ex)
        {
            // A rejected event names its INDEX and leaves the buffer untouched, so a half-processed
            // render can never be mistaken for a good one.
            Console.Error.WriteLine($"automated render rejected: {ex.Message}");
        }

        // --- branch latency compensation ------------------------------------
        ShowBranchCompensation(chain, src.SampleRate);

        chain.Clear();
    }
    return 0;
}

// ------------------------------------------------------------------ the rig

/// <summary>
/// gate → compressor → overdrive → amp → cab → reverb, built inside ONE edit scope.
/// Without the scope each add publishes its own ordering snapshot and its own 10 ms crossfade, so a
/// six-pedal preset change is six audible steps through half-built boards. Inside a scope the writes
/// accumulate against a shadow chain, reads report the shadow, and Commit() publishes the lot as one
/// change — at most two publications whatever the preset size.
/// </summary>
static IReadOnlyList<JuceDspNode> BuildPreset(JuceDspChain chain)
{
    // Dispose-aborts is the whole reason for the `using`: a throw half-way through leaves the rig
    // exactly as it was, instead of stuck in an edit nobody will ever close.
    using var edit = chain.BeginEdit();

    chain.Clear();   // the old board keeps playing until the commit

    var gate = chain.AddNode("gate");
    gate.SetParam("thresholdDb", -50f);

    var comp = chain.AddNode("compressor");
    comp.SetParam("thresholdDb", -20f);
    comp.SetParam("ratio", 4f);

    var od = chain.AddNode("overdrive");
    od.SetParam("model", 1f);   // NAMED param: 1 == "Screamer" (see --catalog)
    od.SetParam("drive", 0.6f);

    var amp = chain.AddNode("amp");
    amp.SetParam("voicing", 1f); // "Plexi 50"
    amp.SetParam("gain", 0.65f);

    var cab = chain.AddNode("cab");
    cab.SetParam("model", 3f);   // "Vintage 4x12"

    var reverb = chain.AddNode("reverb");
    reverb.SetParam("mix", 0.2f);

    Console.WriteLine($"Preset built inside one edit scope (IsEditing={chain.IsEditing}, {chain.NodeCount} nodes in the shadow).");
    edit.Commit();   // one change, one fade window
    Console.WriteLine($"Committed (IsEditing={chain.IsEditing}).");

    return [gate, comp, od, amp, cab, reverb];
}

/// <summary>ABI v32 parked insertion: the node exists at ZERO wet, is configured and moved into
/// place while inaudible, and only then fades in — no half-configured delay ever reaches the output.</summary>
static JuceDspNode InsertParkedDelay(JuceDspChain chain, int beforeIndex)
{
    var delay = chain.AddNodeBypassed("delay");
    delay.SetParam("sync", 1f);      // tempo sync ON (timeMs is ignored while on)
    delay.SetParam("bpm", 120f);     // the node's own tempo — nothing implicit
    delay.SetParam("division", 5f);  // "1/8" (labels: 1/1, 1/2, 1/4, 1/4 D, 1/4 T, 1/8, ...)
    delay.SetParam("trails", 1f);    // bypass lets the echoes decay instead of cutting
    delay.SetParam("feedback", 0.35f);
    delay.SetParam("mix", 0.3f);
    delay.MoveTo(beforeIndex);       // slot it before the reverb, still parked
    delay.Bypass = false;            // 10 ms fade-in with every param already in place
    Console.WriteLine($"Inserted a parked delay (node {delay.Id}): sync=On bpm=120 division=1/8 trails=On, then un-bypassed.");
    return delay;
}

/// <summary>
/// ABI v34 sidechain. Only the DETECTOR moves: keyed off the seat's DRY input, the compressor
/// follows the pick rather than whatever the drive ahead of it produced.
/// The capability must be read off the catalogue, because setting a key on a module that would
/// ignore it THROWS — deliberately, so a source a module quietly dropped can never read as a live
/// feature that does nothing.
/// </summary>
static void KeyTheCompressorOffTheDryInput(IReadOnlyList<JuceDspNode> nodes)
{
    var comp = nodes.FirstOrDefault(n => n.TypeId == "compressor");
    if (comp is null) return;
    var info = JuceDspModuleCatalog.Find("compressor");
    if (info is null || !info.SupportsSidechain)
    {
        Console.WriteLine("Compressor does not advertise a sidechain on this engine — leaving it self-keyed.");
        return;
    }
    comp.Sidechain = JuceDspSidechainSource.SeatInput;
    Console.WriteLine($"Compressor keyed off {comp.Sidechain} (the seat's dry input). On the offline path the "
                    + "SongMix key would be unavailable and the module falls back to its OWN input — never to silence, "
                    + "which on a gate would mean closed.");
}

/// <summary>
/// Summing branches of different latency is a comb filter, not a blend. Since ABI v34 the merger
/// delays each branch by (latest − its own), so a parallel region runs at the MAXIMUM branch latency
/// rather than smearing. Turning it off restores the pre-v34 sum — some players want that comb.
/// </summary>
static void ShowBranchCompensation(JuceDspChain chain, int sampleRate)
{
    JuceDspNode amp, harm;
    using (var edit = chain.BeginEdit())
    {
        chain.Clear();
        chain.AddNode("splitter").SetParam("enabledBranches", 2f);
        amp = chain.AddNode("amp");
        amp.Branch = 0;                         // branch 1: no latency
        harm = chain.AddNode("harmonizer");     // branch 2: the pitch family IS late
        harm.Branch = 1;
        harm.SetParam("semitones", 7f);
        chain.AddNode("merger");
        edit.Commit();
    }

    // The CHAIN figure is the MAXIMUM over the enabled branches either way — they run in parallel, so
    // the region is as late as its latest branch, and this number never was their sum. What
    // BranchLatencyCompensation changes is what the MERGER does with the difference: on (the
    // default) it delays each branch by (latest − its own), off it sums them as they arrive, which
    // is a comb filter rather than a blend. That comb was the accepted v23 limit; some players want
    // it, so it stays reachable.
    var chainLatency = chain.LatencySamples;
    Console.WriteLine($"Parallel region (amp ∥ harmonizer +7): amp {amp.LatencySamples} smp, harmonizer {harm.LatencySamples} smp, "
                    + $"chain {chainLatency} smp = {chainLatency / (double)sampleRate * 1000:F1} ms @ {sampleRate} Hz (the MAX, not the sum).");
    chain.BranchLatencyCompensation = false;
    Console.WriteLine($"  BranchLatencyCompensation=false → chain still reports {chain.LatencySamples} smp, but the merger no "
                    + "longer aligns the branches: the region combs instead of blending.");
    chain.BranchLatencyCompensation = true;
}

static void PrintChain(JuceDspChain chain)
{
    var nodes = chain.Nodes;
    Console.WriteLine($"Chain (seat {chain.Player}, {nodes.Count} nodes, {chain.LatencySamples} samples latency):");
    foreach (var node in nodes)
    {
        // ABI v33: the TYPE and the BYPASS flag come from the ENGINE, not from a managed memory of
        // what this process created — so this would read the same for a rig some other binding built
        // against the same engine. Bypass is the message-thread INTENT, never the crossfade position.
        var info = JuceDspModuleCatalog.Find(node.TypeId);
        var name = info?.DisplayName ?? node.TypeId;
        var category = info?.Category is { Length: > 0 } c ? c : "?";
        var summary = node.TypeId switch
        {
            "delay"      => $"sync={node.GetParam("sync"):F0} bpm={node.GetParam("bpm"):F0} division={node.GetParam("division"):F0} trails={node.GetParam("trails"):F0}",
            "overdrive"  => $"model={node.GetParam("model"):F0} drive={node.GetParam("drive"):F2}",
            "amp"        => $"voicing={node.GetParam("voicing"):F0} gain={node.GetParam("gain"):F2}",
            "cab"        => $"model={node.GetParam("model"):F0}",
            "gate"       => $"thresholdDb={node.GetParam("thresholdDb"):F0}",
            "compressor" => $"thresholdDb={node.GetParam("thresholdDb"):F0} ratio={node.GetParam("ratio"):F0} key={node.Sidechain}",
            "reverb"     => $"mix={node.GetParam("mix"):F2}",
            _ => string.Empty,
        };
        var state = node.Bypass ? "bypassed" : "active  ";
        Console.WriteLine($"  [{node.Id,7}] {name,-12} {category,-10} {state}  latency {node.LatencySamples,3} smp  {summary}");
    }
}

// ------------------------------------------------------------------ plumbing

static void WriteWav(string path, float[] interleaved, int sampleRate, int channels, WavSampleFormat format)
{
    // WavWriter streams: it writes a provisional header, appends, and patches the sizes on Dispose.
    // A process that dies first leaves a file WavIo.Read still decodes in full, because the
    // placeholder sizes are the RIFF unknown-length marker and not zero.
    using var w = new WavWriter(path, sampleRate, channels, format);
    w.Write(interleaved);
}

static JuceEngineConfig BuildConfig(Options opts)
{
    if (opts.NullDevice)
    {
        Console.WriteLine("Using the Null device (headless, silent).");
        return new JuceEngineConfig { OutputTypeName = "Null", SampleRate = 48_000, BufferFrames = 480 };
    }
    if (string.IsNullOrEmpty(opts.Device)) return new JuceEngineConfig();
    foreach (var host in JuceDeviceCatalog.Enumerate())
    {
        var outDev = host.OutputDevices.FirstOrDefault(d => d.Name.Contains(opts.Device, StringComparison.OrdinalIgnoreCase));
        if (outDev is null) continue;
        var inDev = host.InputDevices.FirstOrDefault(d => d.Name.Contains(opts.Device, StringComparison.OrdinalIgnoreCase))
                    ?? host.InputDevices.FirstOrDefault();
        Console.WriteLine($"Matched '{opts.Device}': {outDev.Name} on {JuceDeviceCatalog.DisplayName(host.TypeName)}");
        return new JuceEngineConfig
        {
            OutputTypeName = host.TypeName, InputTypeName = host.TypeName,
            OutputDeviceId = outDev.Id, InputDeviceId = inDev?.Id ?? string.Empty,
        };
    }
    Console.WriteLine($"No device matches '{opts.Device}' — using the defaults.");
    return new JuceEngineConfig();
}

static void PrintXruns(JuceAudioEngine engine)
{
    if (!engine.TryGetXrunStats(out var x)) { Console.WriteLine("Dropout counters: unavailable on this DLL."); return; }
    Console.WriteLine($"Dropouts: callbacks={x.Callbacks} late={x.LateCallbacks} overruns={x.DeadlineOverruns} nearMisses={x.NearMisses} " +
                      $"driverXruns={(x.DeviceXruns?.ToString(CultureInfo.InvariantCulture) ?? "n/a (driver cannot report)")} " +
                      $"maxCallback={x.MaxCallbackNs / 1e6:F2} ms budget={x.BlockBudgetNs / 1e6:F2} ms");
}

static void PrintCatalogue(IReadOnlyList<JuceDspModuleInfo> catalogue)
{
    // ABI v33: the category is the ENGINE's, out of a short fixed set it owns — group a palette by
    // it rather than hand-mirroring a managed table that will drift the first time a module is added.
    foreach (var group in catalogue.GroupBy(m => m.Category).OrderBy(g => g.Key, StringComparer.Ordinal))
    {
        Console.WriteLine($"\n== {group.Key} ({group.Count()}) ==");
        foreach (var m in group)
        {
            var key = m.SupportsSidechain ? "  [accepts a sidechain key]" : string.Empty;
            Console.WriteLine($"{m.Id,-14} {m.DisplayName,-26}{key}");
            foreach (var p in m.Params)
            {
                // A discrete param is exactly one of two shapes and a UI must render them
                // differently: NAMED picks a named thing (render a picker), ORDINAL is a count
                // (render a knob that SNAPS — a continuous one reads "6.5 stages" while the core
                // runs 6).
                var kind = p.IsNamed ? $"named: {string.Join(" | ", p.Labels!)}" : p.IsOrdinal ? $"ordinal step {p.Step}" : "continuous";
                Console.WriteLine($"    {p.Id,-14} {p.Name,-24} [{p.Min:G4}..{p.Max:G4}] default {p.Default:G4}  ({kind})");
            }
            foreach (var s in m.StringParams)
                Console.WriteLine($"    {s.Id,-14} {s.Name,-24} (string{(s.FileFilter.Length > 0 ? $", file: {s.FileFilter}" : "")})");
        }
    }
}

static void PrintHelp(TextWriter w)
{
    w.WriteLine("JuceSharp.Examples.ToneRig — build and drive the engine's in-house tone rig.");
    w.WriteLine();
    w.WriteLine("Usage:");
    w.WriteLine("  ToneRig [--seconds N] [--device <substring>] [--null]   live input through the rig");
    w.WriteLine("  ToneRig --offline <in.wav|flac|ogg|mp3> <out.wav>       render a file, no sound card");
    w.WriteLine("  ToneRig --catalog                                       every module type, by category");
    w.WriteLine();
    w.WriteLine("Options:");
    w.WriteLine("  --seconds N           How long to poll the meters in live mode. Default 8.");
    w.WriteLine("  --device <substring>  Use the first device whose name contains this.");
    w.WriteLine("  --null                Use the engine's silent \"Null\" device: no sound card, runs");
    w.WriteLine("                        headless. Sets DCA_ENABLE_NULL_DEVICE=1, which is what");
    w.WriteLine("                        registers that device type; you can set it yourself instead.");
    w.WriteLine("                        --offline sets it on your behalf — an offline render needs an");
    w.WriteLine("                        engine but must not have a device open.");
    w.WriteLine("  -h, --help            This text.");
    w.WriteLine();
    w.WriteLine("--offline writes THREE things: <out.wav> (24-bit, static), <out>.auto.wav (float32,");
    w.WriteLine("automated) and a branch-latency read-back on stdout.");
    w.WriteLine();
    w.WriteLine("Exit codes: 0 ok, 2 bad arguments, 3 native DLL missing / ABI mismatch, 4 no device.");
}

static float Peak(float[] x) { var p = 0f; foreach (var v in x) p = MathF.Max(p, MathF.Abs(v)); return p; }
static double Db(float linear) => 20.0 * Math.Log10(Math.Max(linear, 1e-5f));

internal sealed record Options(int Seconds, string? Device, string? OfflineIn, string? OfflineOut, bool Catalog, bool NullDevice, bool Help)
{
    /// <summary>Returns null when an argument is malformed; the caller prints the help and fails.</summary>
    public static Options? Parse(string[] args)
    {
        int seconds = 8; string? device = null, offIn = null, offOut = null;
        bool catalog = false, nullDevice = false, help = false;
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "-h" or "--help": help = true; break;
                case "--seconds" when i + 1 < args.Length && int.TryParse(args[i + 1], NumberStyles.Integer, CultureInfo.InvariantCulture, out var s) && s > 0:
                    seconds = s; i++; break;
                // `--device null` is the spelling the docs use; keep it as an alias of `--null`.
                case "--device" when i + 1 < args.Length && string.Equals(args[i + 1], "null", StringComparison.OrdinalIgnoreCase): nullDevice = true; i++; break;
                case "--device" when i + 1 < args.Length: device = args[++i]; break;
                case "--offline" when i + 2 < args.Length: offIn = args[++i]; offOut = args[++i]; break;
                case "--catalog": catalog = true; break;
                case "--null": nullDevice = true; break;
                default: return null;
            }
        }
        return new Options(seconds, device, offIn, offOut, catalog, nullDevice, help);
    }
}
