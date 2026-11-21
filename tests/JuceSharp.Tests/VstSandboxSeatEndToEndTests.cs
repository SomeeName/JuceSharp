using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.ExceptionServices;
using System.Text;
using System.Text.Json;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// DotCharter #646 / #696 end to end: real audio through the REAL sandbox
/// children (<c>juce_vst_host.exe</c>, one per seat since ABI v50) and a real
/// VST3, on two seats. The stub and fake
/// children of <see cref="RemoteBridgeRoundTripTests"/> and the RT gate prove
/// the engine side only; this one runs the child's own request reader, seat
/// routing, seat mask and plugin hosting.
/// <para>
/// The plugin is <c>DcTestHalfGain.vst3</c> (output = input x 0.5), built ONLY
/// by the native test configuration (target <c>dc_test_halfgain_plugin</c>,
/// tests/CMakeLists.txt) into <c>src/JuceSharp.Native/build/test-plugins/</c>.
/// A contributor with the prebuilt native pair has no such plugin, so the test
/// SKIPS saying so. The engine runs on the null device (no sound card); each
/// seat plays its own looped take, seat 0 at 0.1 and seat 1 at 0.4, and what
/// each seat is HEARD at is its post-chain peak.
/// </para>
/// <para>
/// Each seat's bridge is armed with its own names and served by its own
/// child. Checked in order: nothing loaded, both dry; a slot in seat 1's child
/// halves seat 1 only; the same after an engine rebuild over the surviving
/// children; unloaded, both dry; a slot in each child halves both; a bypass in
/// seat 0's child leaves seat 1 halved; a second slot on seat 1 and a reorder
/// keep seat 1 at a quarter and seat 0 at a half; killing seat 1's child leaves
/// seat 1 dry and seat 0 still processed by its own child, and the replay into a
/// fresh seat-1 child (the slot loaded again with its seat, which is what the
/// host's replay sends) restores seat 1 halved.
/// </para>
/// Waits are deadlock detectors, never budgets.
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
[Collection("NativeEngine")]
public sealed class VstSandboxSeatEndToEndTests
{
    private const int DeadlockMs = 90_000;
    private const float Seat0Amplitude = 0.1f;
    private const float Seat1Amplitude = 0.4f;

    [NativeAvailableFact]
    public void ASandboxedSlotProcessesOnlyItsOwnSeat_ThroughTheRealChild()
    {
        var exe = Path.Combine(AppContext.BaseDirectory, "juce_vst_host.exe");
        if (!File.Exists(exe)) Assert.Skip($"no juce_vst_host.exe next to the test assembly ({exe})");
        var plugin = FindTestPlugin();
        if (plugin is null)
        {
            Assert.Skip($"the test-only DcTestHalfGain.vst3 was not built: looked for {TestPluginRelativePath} "
                        + $"under {AppContext.BaseDirectory} and every folder above it. Build the native target "
                        + "dc_test_halfgain_plugin (the full native build and the RT gate's juce_sharp_rtcheck "
                        + "build both produce it); the prebuilt native pair does not include it");
        }

        var suffix = Guid.NewGuid().ToString("N");
        var names0 = new BridgeNames("dcvb_e2e0_shm_" + suffix, "dcvb_e2e0_req_" + suffix, "dcvb_e2e0_resp_" + suffix);
        var names1 = new BridgeNames("dcvb_e2e1_shm_" + suffix, "dcvb_e2e1_req_" + suffix, "dcvb_e2e1_resp_" + suffix);
        JuceAudioEngine? engine = null;
        SandboxChild? child0 = null;
        SandboxChild? child1 = null;
        try
        {
            engine = ArmTwoSeats(names0, names1);
            child0 = SandboxChild.Start(exe, names0, "dcvb_e2e0_pipe_" + suffix);
            child1 = SandboxChild.Start(exe, names1, "dcvb_e2e1_pipe_" + suffix);

            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude, "nothing loaded");

            child1.Load("a", plugin, seat: 1);
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude * 0.5f, "slot a on seat 1");

            // An engine rebuild: a fresh engine re-armed over the surviving children.
            engine.Dispose();
            engine = ArmTwoSeats(names0, names1);
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude * 0.5f, "after an engine rebuild");

            child1.Command("{\"op\":\"unload\",\"slot\":\"a\"}");
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude, "slot a unloaded");

            child0.Load("b", plugin, seat: 0);
            child1.Load("c", plugin, seat: 1);
            AssertSeatsSettleAt(engine, Seat0Amplitude * 0.5f, Seat1Amplitude * 0.5f, "b on seat 0, c on seat 1");

            child0.Command("{\"op\":\"bypass\",\"slot\":\"b\",\"value\":true}");
            AssertSeatsSettleAt(engine, Seat0Amplitude, Seat1Amplitude * 0.5f, "b bypassed");
            child0.Command("{\"op\":\"bypass\",\"slot\":\"b\",\"value\":false}");
            child1.Load("d", plugin, seat: 1);
            child1.Command("{\"op\":\"reorder\",\"slots\":[\"d\",\"c\"]}");
            AssertSeatsSettleAt(engine, Seat0Amplitude * 0.5f, Seat1Amplitude * 0.25f, "b back on; d, c on seat 1");
            child1.Command("{\"op\":\"unload\",\"slot\":\"d\"}");
            AssertSeatsSettleAt(engine, Seat0Amplitude * 0.5f, Seat1Amplitude * 0.5f, "only b on seat 0, c on seat 1");

            // Seat 1's child dies: seat 1's trips miss and pass through dry, and
            // seat 0 is still processed by its own child.
            child1.Kill();
            child1.Dispose();
            child1 = null;
            AssertSeatsSettleAt(engine, Seat0Amplitude * 0.5f, Seat1Amplitude, "seat 1's child killed");

            // The replay into a fresh seat-1 child: the slot again, with its seat.
            child1 = SandboxChild.Start(exe, names1, "dcvb_e2e1_pipe2_" + suffix);
            child1.Load("c", plugin, seat: 1);
            AssertSeatsSettleAt(engine, Seat0Amplitude * 0.5f, Seat1Amplitude * 0.5f, "replayed into a fresh seat-1 child");
        }
        finally
        {
            child1?.Dispose();
            child0?.Dispose();
            engine?.Dispose();
        }
    }

    private sealed record BridgeNames(string Shm, string Request, string Response);

    private static JuceAudioEngine ArmTwoSeats(BridgeNames names0, BridgeNames names1)
    {
        var engine = new JuceAudioEngine(new JuceEngineConfig
        {
            OutputTypeName = NullDevice.TypeName,
            EnableNullDevice = true,
            SampleRate = 48000,
            BufferFrames = 256,
        });
        engine.SetPlayerCount(2);
        foreach (var (seat, amplitude) in new[] { (0, Seat0Amplitude), (1, Seat1Amplitude) })
        {
            var take = new float[4800];
            for (var i = 0; i < take.Length; i++) take[i] = amplitude * MathF.Sin(2f * MathF.PI * 1000f * i / 48000f);
            engine.SetInputInjection(seat, take, loop: true);
            Assert.True(engine.SetInputInjectionPlaying(seat, true), $"seat {seat} has no take to play");
        }

        Assert.True(engine.SetRemoteBridge(0, names0.Shm, names0.Request, names0.Response, enable: true), "seat 0's bridge enable failed");
        Assert.True(engine.SetRemoteBridge(1, names1.Shm, names1.Request, names1.Response, enable: true), "seat 1's bridge enable failed");
        return engine;
    }

    // Peaks inside [0.9, 1.05] of the expected level on BOTH seats in one read
    // window. A missed trip leaves its block dry, so this waits for a clean
    // window instead of trusting the first one.
    private static void AssertSeatsSettleAt(JuceAudioEngine engine, float seat0, float seat1, string stage)
    {
        float p0 = 0f, p1 = 0f;
        _ = engine.GetAndResetPlayerInputPeak(0);
        _ = engine.GetAndResetPlayerInputPeak(1);
        var sw = Stopwatch.StartNew();
        var settled = false;
        while (!settled && sw.ElapsedMilliseconds < DeadlockMs)
        {
            Thread.Sleep(50);
            p0 = engine.GetAndResetPlayerInputPeak(0);
            p1 = engine.GetAndResetPlayerInputPeak(1);
            settled = p0 >= seat0 * 0.9f && p0 <= seat0 * 1.05f && p1 >= seat1 * 0.9f && p1 <= seat1 * 1.05f;
        }

        Assert.True(settled, $"{stage}: seat 0 heard at {p0} (want {seat0}), seat 1 at {p1} (want {seat1})");
    }

    private static readonly string TestPluginRelativePath =
        Path.Combine("src", "JuceSharp.Native", "build", "test-plugins", "DcTestHalfGain.vst3");

    private static string? FindTestPlugin()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, TestPluginRelativePath);
            if (Directory.Exists(candidate)) return candidate;
        }

        return null;
    }

    /// <summary>The real child over its control pipe, as the host drives it.</summary>
    private sealed class SandboxChild : IDisposable
    {
        private readonly Process _process;
        private readonly NamedPipeServerStream _pipe;
        private readonly StreamReader _reader;
        private readonly StreamWriter _writer;
        private readonly System.Collections.Concurrent.ConcurrentQueue<string> _log;
        private int _nextId = 1;

        // The pipe is connected: a reader or writer over an unconnected pipe throws.
        private SandboxChild(Process process, NamedPipeServerStream pipe,
                             System.Collections.Concurrent.ConcurrentQueue<string> log)
        {
            _process = process;
            _pipe = pipe;
            _log = log;
            _reader = new StreamReader(pipe, Encoding.UTF8, false, 4096, leaveOpen: true);
            _writer = new StreamWriter(pipe, new UTF8Encoding(false), 4096, leaveOpen: true) { AutoFlush = true };
        }

        public static SandboxChild Start(string exe, BridgeNames names, string pipeName)
        {
            var pipe = new NamedPipeServerStream(pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                                                 PipeOptions.Asynchronous);
            var psi = new ProcessStartInfo(exe)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardError = true,
            };
            foreach (var arg in new[]
                     {
                         "shm=" + names.Shm, "req=" + names.Request, "resp=" + names.Response,
                         "pipe=" + pipeName, "sr=48000", "block=256", "parent=" + Environment.ProcessId,
                     })
                psi.ArgumentList.Add(arg);

            var process = Process.Start(psi) ?? throw new InvalidOperationException("the child did not start");
            var log = new System.Collections.Concurrent.ConcurrentQueue<string>();
            process.ErrorDataReceived += (_, e) => { if (e.Data is not null) log.Enqueue(e.Data); };
            process.BeginErrorReadLine();
            SandboxChild? child = null;
            try
            {
                Assert.True(pipe.WaitForConnectionAsync().Wait(DeadlockMs),
                    "the child never connected to the control pipe. Child log: " + string.Join(" | ", log));
                child = new SandboxChild(process, pipe, log);
                var hello = child.ReadLine();
                Assert.Contains("\"hello\"", hello, StringComparison.Ordinal);
                return child;
            }
            catch
            {
                if (child is not null) child.Dispose();
                else
                {
                    try { if (!process.HasExited) process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
                    pipe.Dispose();
                    process.Dispose();
                }

                throw;
            }
        }

        public void Load(string slot, string path, int seat)
            => Command("{\"op\":\"load\",\"slot\":\"" + slot + "\",\"path\":" + JsonSerializer.Serialize(path)
                       + ",\"bypass\":false,\"seat\":" + seat + "}");

        /// <summary>Sends one command (an <c>id</c> is added) and asserts its reply is ok.</summary>
        public void Command(string json)
        {
            var id = _nextId++;
            _writer.Write("{\"id\":" + id + "," + json[1..] + "\n");
            while (true)
            {
                var line = ReadLine();
                using var doc = JsonDocument.Parse(line);
                var root = doc.RootElement;
                if (!root.TryGetProperty("id", out var replyId) || replyId.GetInt32() != id) continue;
                var ok = root.TryGetProperty("ok", out var okProp) && okProp.GetBoolean();
                Assert.True(ok, $"the child refused {json}: {line}. Child log: {string.Join(" | ", _log)}");
                return;
            }
        }

        public void Kill()
        {
            _process.Kill(entireProcessTree: true);
            Assert.True(_process.WaitForExit(DeadlockMs), "the killed child never exited");
        }

        public void Dispose()
        {
            try { if (!_process.HasExited) _process.Kill(entireProcessTree: true); } catch (InvalidOperationException) { }
            _reader.Dispose();
            _writer.Dispose();
            _pipe.Dispose();
            _process.Dispose();
        }

        // One line off the control pipe, on a thread of its own (see
        // VstHostChildLifecycleTests.ReadLine for why not the pool).
        private string ReadLine()
        {
            string? line = null;
            ExceptionDispatchInfo? failure = null;
            var thread = new Thread(() =>
            {
                try { line = _reader.ReadLine(); }
                catch (Exception ex) { failure = ExceptionDispatchInfo.Capture(ex); }
            })
            { IsBackground = true, Name = "vst-host-e2e-pipe" };
            thread.Start();
            Assert.True(thread.Join(DeadlockMs), "the child never answered on the control pipe");
            failure?.Throw();
            Assert.False(string.IsNullOrEmpty(line), "the child closed the control pipe. Child log: " + string.Join(" | ", _log));
            return line!;
        }
    }
}
