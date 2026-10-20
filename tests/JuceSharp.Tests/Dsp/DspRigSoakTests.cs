using System.Runtime.InteropServices;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// DSP-rig concurrency coverage via the offline tooling export
/// <c>dca_engine_dsp_process_buffer</c> (2026-07-16 audit test-gap list:
/// "DspChain barrier under concurrent reorder/remove + process" and
/// "setParam while process runs" had no coverage at all).
///
/// The functional test proves the ParamMailbox actually APPLIES published
/// params on the processing thread (audit #4's fix would be silently inert
/// if applyPending never ran). The soak hammers process against concurrent
/// param sweeps, reorders, bypass flips and remove/re-add cycles and asserts
/// the output stays finite — the exact failure mode of torn coefficients is
/// a NaN/blow-up transient. Default duration is ~2 s; set
/// JUCESHARP_SOAK_SECONDS to lengthen it (same env-gating convention as the
/// GND_* eval switches).
/// </summary>
[Collection("NativeEngine")]
public sealed class DspRigSoakTests : IDisposable
{
    private const string Lib = "juce_sharp";
    private const int Sr = 48000;

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_process_buffer", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspProcessBuffer(nint engine, float[] left, float[] right, int numFrames, int sampleRate);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_add_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspAddNode(nint engine, [MarshalAs(UnmanagedType.LPUTF8Str)] string typeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_remove_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspRemoveNode(nint engine, int nodeId);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_move_node", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspMoveNode(nint engine, int nodeId, int newIndex);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_bypass", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetBypass(nint engine, int nodeId, int bypassed);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_set_param", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspSetParam(nint engine, int nodeId,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string paramId, float value);

    [DllImport(Lib, EntryPoint = "dca_engine_dsp_clear", CallingConvention = CallingConvention.Cdecl)]
    private static extern int DspClear(nint engine);

    private readonly JuceAudioEngine? _engine;

    public DspRigSoakTests()
    {
        if (!NativeProbe.IsAvailable) return;
        _engine = new JuceAudioEngine();
        // Stop the device: the offline path must be the ONLY processor so the
        // functional assertions are deterministic (a live input callback
        // would stir the same module state).
        _engine.Stop();
        DspClear(_engine.NativeHandle);
    }

    public void Dispose()
    {
        if (_engine is not null)
        {
            DspClear(_engine.NativeHandle);
            _engine.Dispose();
        }
    }

    private static float[] Sine(double freqHz, int samples, float amplitude = 0.25f)
    {
        var buf = new float[samples];
        for (var i = 0; i < samples; i++)
            buf[i] = amplitude * (float)Math.Sin(2.0 * Math.PI * freqHz * i / Sr);
        return buf;
    }

    private static void AssertAllFinite(float[] l, float[] r)
    {
        for (var i = 0; i < l.Length; i++)
        {
            Assert.True(float.IsFinite(l[i]), $"non-finite L sample at {i}");
            Assert.True(float.IsFinite(r[i]), $"non-finite R sample at {i}");
        }
    }

    [NativeAvailableFact]
    public void EmptyRig_IsPassthrough()
    {
        var h = _engine!.NativeHandle;
        var l = Sine(440, 4096);
        var r = (float[])l.Clone();
        var reference = (float[])l.Clone();
        Assert.Equal(0, DspProcessBuffer(h, l, r, l.Length, Sr));
        Assert.Equal(reference, l);
        Assert.Equal(reference, r);
    }

    [NativeAvailableFact]
    public void ParamMailbox_AppliesPublishedParams_OnTheProcessingThread()
    {
        // eq with only outputTrimDb moved is a flat +6 dB — if the mailbox
        // published but never applied, the output would stay at unity gain.
        var h = _engine!.NativeHandle;
        var eq = DspAddNode(h, "eq");
        Assert.True(eq >= 0);
        Assert.Equal(0, DspSetParam(h, eq, "outputTrimDb", 6.0f));

        var l = Sine(440, Sr / 2);
        var r = (float[])l.Clone();
        var reference = (float[])l.Clone();
        Assert.Equal(0, DspProcessBuffer(h, l, r, l.Length, Sr));
        AssertAllFinite(l, r);

        double rmsIn = 0, rmsOut = 0;
        // Skip the first 2048 samples: the trim lands at the first block
        // boundary and the eq's shelves have (tiny) transients.
        for (var i = 2048; i < l.Length; i++)
        {
            rmsIn += reference[i] * reference[i];
            rmsOut += l[i] * l[i];
        }
        var gain = Math.Sqrt(rmsOut / rmsIn);
        Assert.InRange(gain, 1.85, 2.15); // +6 dB = 1.995x
    }

    [NativeAvailableFact]
    public void Soak_ProcessWhileReorderBypassAndParamSweep_StaysFinite()
    {
        var h = _engine!.NativeHandle;

        // A split region between eq and the merger gives the soak branch
        // routing coverage too.
        var nodes = new Dictionary<string, int>
        {
            ["gate"] = DspAddNode(h, "gate"),
            ["compressor"] = DspAddNode(h, "compressor"),
            ["eq"] = DspAddNode(h, "eq"),
            ["splitter"] = DspAddNode(h, "splitter"),
            ["chorus"] = DspAddNode(h, "chorus"),
            ["delay"] = DspAddNode(h, "delay"),
            ["merger"] = DspAddNode(h, "merger"),
        };
        Assert.All(nodes.Values, id => Assert.True(id >= 0));
        NativeMethods.EngineDspSetNodeBranch(h, nodes["chorus"], 1);
        NativeMethods.EngineDspSetNodeBranch(h, nodes["delay"], 2);

        // Sweep table: every id verified against the adapters/catalogue.
        var sweeps = new (string Node, string Param, float Min, float Max)[]
        {
            ("gate", "thresholdDb", -70f, -20f),
            ("gate", "releaseMs", 20f, 400f),
            ("compressor", "thresholdDb", -40f, -5f),
            ("compressor", "ratio", 1f, 10f),
            ("compressor", "makeupDb", 0f, 12f),
            ("eq", "lowGainDb", -12f, 12f),
            ("eq", "midFreq", 200f, 4000f),
            ("eq", "midQ", 0.3f, 4f),
            ("eq", "midGainDb", -12f, 12f),
            ("eq", "highGainDb", -12f, 12f),
            ("eq", "outputTrimDb", -6f, 6f),
            ("chorus", "rateHz", 0.1f, 5f),
            ("chorus", "depthMs", 1f, 10f),
            ("chorus", "mix", 0f, 1f),
            ("delay", "timeMs", 30f, 800f),
            ("delay", "feedback", 0f, 0.7f),
            ("delay", "mix", 0f, 1f),
            ("delay", "toneHz", 500f, 12000f),
            ("merger", "level1", 0f, 2f),
            ("merger", "pan1", -1f, 1f),
            ("merger", "level2", 0f, 2f),
            ("merger", "mode", 0f, 1f),
        };

        var seconds = 2;
        if (int.TryParse(Environment.GetEnvironmentVariable("JUCESHARP_SOAK_SECONDS"), out var envSec) && envSec > 0)
            seconds = envSec;

        var stop = false;
        Exception? mutatorError = null;
        var rng = new Random(42);

        var mutator = new Thread(() =>
        {
            try
            {
                var iter = 0;
                while (!Volatile.Read(ref stop))
                {
                    var pick = rng.Next(10);
                    if (pick < 6)
                    {
                        var (node, param, min, max) = sweeps[rng.Next(sweeps.Length)];
                        DspSetParam(h, nodes[node], param, min + (float)rng.NextDouble() * (max - min));
                    }
                    else if (pick < 8)
                    {
                        // Reorder inside the pre-split span so the split
                        // region stays structurally valid.
                        var names = new[] { "gate", "compressor", "eq" };
                        DspMoveNode(h, nodes[names[rng.Next(names.Length)]], rng.Next(3));
                    }
                    else if (pick < 9)
                    {
                        var names = new[] { "compressor", "chorus", "delay" };
                        var n = names[rng.Next(names.Length)];
                        DspSetBypass(h, nodes[n], rng.Next(2));
                    }
                    else
                    {
                        // Remove + re-add: exercises the retire barrier while
                        // blocks are in flight.
                        var names = new[] { "compressor", "chorus" };
                        var n = names[rng.Next(names.Length)];
                        DspRemoveNode(h, nodes[n]);
                        var id = DspAddNode(h, n);
                        if (id < 0) throw new InvalidOperationException($"re-add of {n} failed");
                        nodes[n] = id;
                        if (n == "chorus") NativeMethods.EngineDspSetNodeBranch(h, id, 1);
                    }

                    if (++iter % 16 == 0) Thread.Sleep(0);
                }
            }
            catch (Exception ex) { mutatorError = ex; }
        });

        mutator.Start();
        try
        {
            var l = Sine(196, 4096);
            var template = (float[])l.Clone();
            var r = (float[])l.Clone();
            var deadline = Environment.TickCount64 + seconds * 1000;
            var blocks = 0;
            while (Environment.TickCount64 < deadline)
            {
                Array.Copy(template, l, l.Length);
                Array.Copy(template, r, r.Length);
                Assert.Equal(0, DspProcessBuffer(h, l, r, l.Length, Sr));
                AssertAllFinite(l, r);
                blocks++;
            }
            Assert.True(blocks > 10, $"soak only processed {blocks} buffers — engine wedged?");
        }
        finally
        {
            Volatile.Write(ref stop, true);
            mutator.Join();
        }
        Assert.Null(mutatorError);
    }
}
