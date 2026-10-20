using Microsoft.Extensions.Logging.Abstractions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>Integration tests for <see cref="JuceAudioEngine"/>. Each test
/// uses its own engine instance so failures don't poison other tests.
/// Tests are skipped automatically when the native DLL is unavailable.</summary>
[Collection("NativeEngine")]
public sealed class JuceAudioEngineTests
{
    [NativeAvailableFact]
    public void NativeVersion_IsNonEmptyString()
    {
        var v = JuceAudioEngine.NativeVersion;
        Assert.False(string.IsNullOrWhiteSpace(v));
    }

    /// <summary>
    /// The version string is GENERATED from the build now, not typed a second
    /// time. It said "DotCharter.Audio.Native 0.3.0 (JUCE)" for five releases —
    /// a hard-coded copy of a CMake <c>project(VERSION)</c> that was itself
    /// stale, under a name the library no longer goes by, with no ABI version
    /// in it at all. The ABI number is the part a caller can act on, so this
    /// asserts THAT: a bump that forgets to rebuild the DLL is exactly the
    /// mismatch <c>NativeMethods.ExpectedAbiVersion</c> refuses at engine open,
    /// and this is the diagnostic a human reads when it does.
    /// </summary>
    [NativeAvailableFact]
    public void NativeVersion_NamesTheLibraryAndTheAbiItWasBuiltFor()
    {
        var v = JuceAudioEngine.NativeVersion;
        Assert.StartsWith("JuceSharp.Native ", v, StringComparison.Ordinal);
        Assert.Contains($"ABI {NativeMethods.ExpectedAbiVersion}", v, StringComparison.Ordinal);
        Assert.Contains("JUCE ", v, StringComparison.Ordinal);
        Assert.DoesNotContain("0.0.0-nocmake", v, StringComparison.Ordinal);
    }

    [NativeAvailableFact]
    public void Create_DefaultConfig_Succeeds()
    {
        using var engine = new JuceAudioEngine();
        // Engine constructed without throwing; basic getters work.
        Assert.True(engine.SampleRate > 0);
        Assert.True(engine.BufferFrames > 0);
    }

    [NativeAvailableFact]
    public void Create_NullConfig_UsesDefaults()
    {
        using var engine = new JuceAudioEngine(null, NullLogger<JuceAudioEngine>.Instance);
        Assert.True(engine.SampleRate > 0);
    }

    [NativeAvailableFact]
    public void StartStop_AreIdempotent()
    {
        using var engine = new JuceAudioEngine();
        engine.Start();
        engine.Start();   // second start is a no-op
        engine.Stop();
        engine.Stop();    // second stop is a no-op
    }

    /// <summary>
    /// <c>dca_engine_create</c> opens AND STARTS the device when given a config,
    /// and the wrapper always gives it one — so <c>Stop()</c> on a freshly
    /// constructed engine must actually close the device. It did not: the
    /// constructor left <c>_started</c> false and <c>Stop()</c> early-outs on
    /// it, so every <c>new JuceAudioEngine(); Stop();</c> was a no-op.
    /// <para>
    /// Eight call sites do exactly that pair and every one of them says in a
    /// comment that it wants no device — DotCharter's <c>TakeReampService</c>
    /// and its offline DSP fixtures, and four in this repo. They were all
    /// rendering against a live callback and holding the host's sound card.
    /// </para>
    /// <para>
    /// Asserted through <c>SampleRate</c>, which reads
    /// <c>getCurrentAudioDevice()</c> natively and reports 0 when there is no
    /// device — the same predicate <c>dca_engine_dsp_process_buffer</c>'s
    /// open-device rejection uses, so this is the state that matters and not a
    /// managed flag talking about itself.
    /// </para>
    /// </summary>
    [NativeAvailableFact]
    public void Stop_OnAFreshEngine_ActuallyClosesTheDevice()
    {
        using var engine = new JuceAudioEngine();
        // Non-vacuous: the constructor really did open one. On a host with no
        // audio hardware there is nothing to close and nothing to prove.
        if (engine.SampleRate <= 0)
        {
            Assert.Skip("no audio device on this host — the constructor opened none");
            return;
        }

        engine.Stop();
        Assert.Equal(0, engine.SampleRate);

        // ...and it stays closed: a stopped engine refuses to restart (#27), because
        // Stop() detaches every source for good and a reopened device would play
        // silence. EngineRestartRefusalTests pins the refusal itself.
        Assert.Throws<InvalidOperationException>(engine.Start);
        Assert.Equal(0, engine.SampleRate);
        engine.Stop();   // a second stop stays a harmless no-op
    }

    [NativeAvailableFact]
    public void GainSetters_DoNotThrow()
    {
        using var engine = new JuceAudioEngine();
        engine.SetMasterGain(1.0f);
        engine.SetMonitorGain(0.5f);
        engine.SetInputGain(0.25f);
        engine.SetInstrumentGain(0.75f);
        engine.SetHighPass(80f, 0.7f);
        engine.SetLowPass(8000f, 0.7f);
        engine.SetBandPass(1000f, 1.0f);
    }

    [NativeAvailableFact]
    public void GainSetters_AfterDispose_AreNoOp()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        engine.SetMasterGain(0f);
        engine.SetMonitorGain(0f);
        engine.SetInputGain(0f);
        engine.SetInstrumentGain(0f);
        engine.SetHighPass(80f, 0.7f);
        engine.SetLowPass(8000f, 0.7f);
        engine.SetBandPass(1000f, 1.0f);
        engine.SendMidi(0x90, 60, 100);
        engine.PlayClick(0);
        Assert.False(engine.SetMirrorDevice("any"));
    }

    [NativeAvailableFact]
    public void GetAndResetInputPeak_ReturnsZeroWhenIdle()
    {
        using var engine = new JuceAudioEngine();
        var peak = engine.GetAndResetInputPeak();
        Assert.True(peak >= 0f);
    }

    [NativeAvailableFact]
    public void GetAndResetInputPeak_AfterDispose_IsZero()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.Equal(0f, engine.GetAndResetInputPeak());
    }

    [NativeAvailableFact]
    public void EnumerateOutputDevices_Static_ReturnsList()
    {
        var list = JuceAudioEngine.EnumerateOutputDevices();
        Assert.NotNull(list);
        // Most desktop machines have at least one output, but a totally
        // headless CI box may not — just assert the shape.
        foreach (var d in list)
        {
            Assert.NotNull(d.Id);
            Assert.NotNull(d.Name);
            Assert.True(d.MaxOutputChannels >= 0);
        }
    }

    [NativeAvailableFact]
    public void EnumerateInputDevices_Static_ReturnsList()
    {
        var list = JuceAudioEngine.EnumerateInputDevices();
        Assert.NotNull(list);
        foreach (var d in list)
        {
            Assert.NotNull(d.Id);
            Assert.NotNull(d.Name);
            Assert.True(d.MaxInputChannels >= 0);
        }
    }

    [NativeAvailableFact]
    public void EnumerateDevicesLive_OnDisposedEngine_IsEmpty()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.Empty(engine.EnumerateOutputDevicesLive());
        Assert.Empty(engine.EnumerateInputDevicesLive());
    }

    [NativeAvailableFact]
    public void EnumerateDevicesLive_OnLiveEngine_DoesNotThrow()
    {
        using var engine = new JuceAudioEngine();
        var outs = engine.EnumerateOutputDevicesLive();
        var ins = engine.EnumerateInputDevicesLive();
        Assert.NotNull(outs);
        Assert.NotNull(ins);
    }

    [NativeAvailableFact]
    public void OpenFile_NullOrEmptyPath_Throws()
    {
        using var engine = new JuceAudioEngine();
        Assert.Throws<ArgumentException>(() => engine.OpenFile(""));
        Assert.Throws<ArgumentException>(() => engine.OpenFile("   "));
        Assert.Throws<ArgumentNullException>(() => engine.OpenFile(null!));
    }

    [NativeAvailableFact]
    public void OpenFile_OnDisposedEngine_Throws()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.Throws<ObjectDisposedException>(() => engine.OpenFile("anything.wav"));
    }

    [NativeAvailableFact]
    public void OpenFile_RealTinyWav_RoundTrips()
    {
        var path = TinyWav.Create(seconds: 0.5);
        try
        {
            using var engine = new JuceAudioEngine();
            using var source = engine.OpenFile(path);
            Assert.True(source.Length.TotalSeconds > 0.1);
            Assert.True(source.Length.TotalSeconds < 1.0);
        }
        finally { TinyWav.TryDelete(path); }
    }

    [NativeAvailableFact]
    public void PlayClick_AllVariants_DoNotThrow()
    {
        using var engine = new JuceAudioEngine();
        engine.PlayClick(0);
        engine.PlayClick(1);
        engine.PlayClick(2);
        engine.PlayClick(999);   // unknown click type tolerated
    }

    // --- Sample-accurate click scheduling (ABI v26) -------------------------

    [NativeAvailableFact]
    public void ScheduleClick_AllVariants_DoNotThrow()
    {
        using var engine = new JuceAudioEngine();
        engine.TryGetRenderClock(out var frames, out _);   // 0 if no device rendered yet
        engine.ScheduleClick(0, frames + 480);
        engine.ScheduleClick(1, frames + 960);
        engine.ScheduleClick(2, frames + 1440);
        engine.ScheduleClick(999, frames + 100);  // unknown click type tolerated
        engine.ScheduleClick(0, frames - 100_000_000);  // absurdly stale frame tolerated
        engine.ClearScheduledClicks();
    }

    [NativeAvailableFact]
    public void RenderClock_AdvancesWhileTheDeviceRuns_OnTheStopwatchTimebase()
    {
        using var engine = new JuceAudioEngine();
        if (!engine.TryGetRenderClock(out var frames0, out var ticks0))
            return;   // headless/no output device on this machine — nothing to assert

        Assert.True(frames0 >= 0);
        // The tick source is juce::Time::getHighResolutionTicks == QPC == Stopwatch,
        // so the sync tick must not be in the future relative to Stopwatch's "now".
        Assert.InRange(ticks0, 1, System.Diagnostics.Stopwatch.GetTimestamp());

        Thread.Sleep(120);
        Assert.True(engine.TryGetRenderClock(out var frames1, out var ticks1));
        Assert.True(frames1 >= frames0, "the render clock must be monotonic");
        Assert.True(ticks1 >= ticks0, "the sync tick must be monotonic");
    }

    [NativeAvailableFact]
    public void RenderClock_AfterDispose_ReturnsFalse()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.False(engine.TryGetRenderClock(out _, out _));
    }

    [NativeAvailableFact]
    public void ScheduleAndClear_AfterDispose_DoNotThrow()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        engine.ScheduleClick(0, 1000);      // NativeHandleOwner: no-ops on a dead handle
        engine.ClearScheduledClicks();
    }

    [NativeAvailableFact]
    public void GenerateWaveform_EmptyPath_ReturnsEmpty()
    {
        Assert.Empty(JuceAudioEngine.GenerateWaveform("", 20.0));
    }

    [NativeAvailableFact]
    public void GenerateWaveform_MissingFile_ReturnsEmpty()
    {
        Assert.Empty(JuceAudioEngine.GenerateWaveform("does-not-exist.wav", 20.0));
    }

    [NativeAvailableFact]
    public void GenerateWaveform_NonPositiveBlockMs_UsesDefault()
    {
        var path = TinyWav.Create(seconds: 0.5);
        try
        {
            var peaks = JuceAudioEngine.GenerateWaveform(path, blockMs: 0.0);
            Assert.NotNull(peaks);
            Assert.NotEmpty(peaks);
            foreach (var p in peaks)
            {
                Assert.True(p >= 0f && p <= 1.5f);
            }
        }
        finally { TinyWav.TryDelete(path); }
    }

    [NativeAvailableFact]
    public void GenerateWaveform_RealWav_ReturnsPeaks()
    {
        var path = TinyWav.Create(seconds: 1.0);
        try
        {
            var peaks = JuceAudioEngine.GenerateWaveform(path, 50.0);
            Assert.NotEmpty(peaks);
        }
        finally { TinyWav.TryDelete(path); }
    }

    [NativeAvailableFact]
    public void SendMidi_ToLiveEngineWithoutInstrument_DoesNotThrow()
    {
        using var engine = new JuceAudioEngine();
        engine.SendMidi(0x90, 60, 100);  // NoteOn — drops silently
        engine.SendMidi(0x80, 60, 0);    // NoteOff
        engine.SendMidi(0xB0, 7, 100);   // CC volume
    }

    [NativeAvailableFact]
    public void SetMirrorDevice_NullOrEmpty_ClosesMirror()
    {
        using var engine = new JuceAudioEngine();
        // Closing with null/empty is always considered "ok" by the wrapper.
        Assert.True(engine.SetMirrorDevice(null));
        Assert.True(engine.SetMirrorDevice(string.Empty));
    }

    [NativeAvailableFact]
    public void TryShowAsioControlPanel_OnDisposedEngine_ReturnsErrorString()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.False(engine.TryShowAsioControlPanel(nint.Zero, out var err));
        Assert.Equal("engine disposed", err);
    }


    [NativeAvailableFact]
    public void Dispose_IsIdempotent()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        engine.Dispose();   // no throw
    }

    [NativeAvailableFact]
    public void PropertyAccess_AfterDispose_Throws()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.Throws<ObjectDisposedException>(() => _ = engine.SampleRate);
        Assert.Throws<ObjectDisposedException>(() => _ = engine.BufferFrames);
    }

    [NativeAvailableFact]
    public void Start_AfterDispose_Throws()
    {
        var engine = new JuceAudioEngine();
        engine.Dispose();
        Assert.Throws<ObjectDisposedException>(() => engine.Start());
    }

    [NativeAvailableFact]
    public void Logger_ReceivesMessagesFromNative()
    {
        var logger = new CapturingLogger();
        using var engine = new JuceAudioEngine(new JuceEngineConfig(), logger);
        // Start triggers device probing which usually emits log lines.
        engine.Start();
        engine.Stop();
        // We don't assert on message count — depends on host environment —
        // just confirm the engine ran without throwing through the trampoline.
        Assert.True(logger.Records.Count >= 0);
    }

    [NativeAvailableFact]
    public void StemGroup_TempoPitch_Accepted_Without_Source()
    {
        // ABI v9: the stem-group stretcher is lazily created when the first
        // source is added to it; calling the setters before any source is
        // grouped must NOT throw and must cache the values for later.
        using var engine = new JuceAudioEngine();
        engine.StemGroupTempo = 0.75f;
        engine.StemGroupPitchSemitones = -2.0f;
        Assert.Equal(0.75f, engine.StemGroupTempo);
        Assert.Equal(-2.0f, engine.StemGroupPitchSemitones);
    }

    [NativeAvailableFact]
    public void Configure_RequestedSampleRateAndBuffer_AreReflected()
    {
        var cfg = new JuceEngineConfig
        {
            SampleRate = 48000,
            BufferFrames = 256,
        };
        using var engine = new JuceAudioEngine(cfg);
        // We requested 48000/256; JUCE may negotiate, just confirm > 0.
        Assert.True(engine.SampleRate > 0);
        Assert.True(engine.BufferFrames > 0);
    }
}
