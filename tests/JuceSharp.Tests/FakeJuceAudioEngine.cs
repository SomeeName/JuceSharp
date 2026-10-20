// Generated once from IJuceAudioEngine's member list and then hand-finished:
// the handful of members the example consumer actually drives do something, the
// rest record the call and return default. That is the whole point of the
// interface — a host can build one of these in a test project that never loads
// juce_sharp.dll. See docs/guides/testing-without-native.md.
//
// Nullable annotations are OFF for the fake: mirroring every string? / out
// string? of a 75-member interface adds nothing to what it demonstrates.
#nullable disable
using JuceSharp;
using JuceSharp.Dsp;

namespace JuceSharp.Tests;

/// <summary>A hand-written test double over <see cref="IJuceAudioEngine"/>.
/// Records the name of every call it receives.</summary>
public sealed class FakeJuceAudioEngine : IJuceAudioEngine
{
    private readonly List<string> _calls = new();

    /// <summary>Every member name this fake was asked for, in order.</summary>
    public IReadOnlyList<string> Calls => _calls;

    /// <summary>What <see cref="OpenFile"/> hands back. Set by the test.</summary>
    public FakeJuceAudioSource NextSource { get; set; } = new();

    public bool Disposed { get; private set; }

    /// <summary>The path the consumer asked <see cref="OpenFile"/> for.</summary>
    public string OpenedPath { get; private set; }

    private void Record(string member) => _calls.Add(member);

    public void Dispose()
    {
        Record(nameof(Dispose));
        Disposed = true;
    }
    public nint NativeHandle { get => default; }
    public int SampleRate { get; set; } = 48000;
    public int BufferFrames { get => default; }
    public int OutputLatencySamples { get => default; }
    public float StemGroupTempo { get => default; set { } }
    public float StemGroupPitchSemitones { get => default; set { } }
    public void Start() => Record("Start");
    public void Stop() => Record("Stop");
    public void Reconfigure(int sampleRate, int bufferFrames, string outputDeviceId, string inputDeviceId) => Record("Reconfigure");
    public void SetDeviceType(string typeName, string outputDeviceId, string inputDeviceId, uint outputChannelMask, uint inputChannelMask) => Record("SetDeviceType");
    public void SetMonitorGain(float gain) => Record("SetMonitorGain");
    public void SetInputGain(float gain) => Record("SetInputGain");
    public void SetInputPitchShift(float semitones) => Record("SetInputPitchShift");
    public void SetInputPitchShift(int player, float semitones) => Record("SetInputPitchShift");
    public void SetInputPitchShiftMode(int mode) => Record("SetInputPitchShiftMode");
    public void SetInputPitchShiftMode(JuceSharp.JuceDropTuneMode mode) => Record("SetInputPitchShiftMode");
    public void SetInputPitchShiftMode(int player, JuceSharp.JuceDropTuneMode mode) => Record("SetInputPitchShiftMode");
    public void SetInputPitchShiftMode(int player, int mode) => Record("SetInputPitchShiftMode");
    public void SetMasterGain(float gain) => Record("SetMasterGain");
    public void SetHighPass(float freqHz, float q) => Record("SetHighPass");
    public void SetLowPass(float freqHz, float q) => Record("SetLowPass");
    public void SetBandPass(float freqHz, float q) => Record("SetBandPass");
    public void SetEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled) => Record("SetEqBand");
    public void SetEqBand(int index, JuceSharp.JuceEqBandType type, float freqHz, float q, float gainDb, bool enabled) => Record("SetEqBand");
    public void SetEqEnabled(bool enabled) => Record("SetEqEnabled");
    public void SetEqSolo(int bandIndex) => Record("SetEqSolo");
    public void SetEqRoute(int route) => Record("SetEqRoute");
    public void SetEqRoute(JuceSharp.JuceEqRoute route) => Record("SetEqRoute");
    public void SetEqTrim(float trimDb) => Record("SetEqTrim");
    public int GetEqActiveCount() { Record("GetEqActiveCount"); return default; }
    public int ReadEqSpectrum(float[] destDb) { Record("ReadEqSpectrum"); return default; }
    public bool SetMirrorDevice(string deviceId) { Record("SetMirrorDevice"); return default; }
    public bool SetMirrorOutput(string deviceId) { Record("SetMirrorOutput"); return default; }
    public void PlayClick(int clickType) => Record("PlayClick");
    public void PlayClick(JuceSharp.JuceClickType clickType) => Record("PlayClick");
    public void ScheduleClick(int clickType, long renderFrame) => Record("ScheduleClick");
    public void ScheduleClick(JuceSharp.JuceClickType clickType, long renderFrame) => Record("ScheduleClick");
    public void ClearScheduledClicks() => Record("ClearScheduledClicks");
    public bool TryGetRenderClock(out long renderFrames, out long hostTicks) { Record("TryGetRenderClock"); renderFrames = default; hostTicks = default; return default; }
    public bool TryGetXrunStats(out JuceSharp.XrunStats stats) { Record("TryGetXrunStats"); stats = default; return default; }
    public bool ResetXrunStats() { Record("ResetXrunStats"); return default; }
    public bool TryGetCallbackLoad(out JuceSharp.CallbackLoad load) { Record("TryGetCallbackLoad"); load = default; return default; }
    public JuceSharp.ExternalProcessInsertOpenResult OpenExternalProcessInsert(int player, string mappingName, string magic = null) { Record("OpenExternalProcessInsert"); return default; }
    public bool CloseExternalProcessInsert(int player) { Record("CloseExternalProcessInsert"); return default; }
    public bool TryGetExternalProcessInsertStats(int player, out JuceSharp.ExternalProcessInsertStats stats) { Record("TryGetExternalProcessInsertStats"); stats = default; return default; }
    public float GetAndResetInputPeak() { Record("GetAndResetInputPeak"); return default; }
    public void SetInputCaptureChannel(int channel) => Record("SetInputCaptureChannel");
    public int ReadInputCapture(System.Span<float> dest) { Record("ReadInputCapture"); return default; }
    public int ReadInputCapture(int player, System.Span<float> dest) { Record("ReadInputCapture"); return default; }
    public int ReadInputCaptureStream(System.Span<float> dest) { Record("ReadInputCaptureStream"); return default; }
    public int ReadInputCaptureStream(System.Span<float> dest, out ulong streamPosition) { Record("ReadInputCaptureStream"); streamPosition = default; return default; }
    public int ReadInputCaptureStream(int player, System.Span<float> dest) { Record("ReadInputCaptureStream"); return default; }
    public int ReadInputCaptureStream(int player, System.Span<float> dest, out ulong streamPosition) { Record("ReadInputCaptureStream"); streamPosition = default; return default; }
    public int ReadInputCaptureFrom(int player, ulong from, System.Span<float> dest, out ulong firstPosition) { Record("ReadInputCaptureFrom"); firstPosition = default; return default; }
    public void SetInputInjection(int player, System.ReadOnlySpan<float> mono, bool loop) { Record("SetInputInjection"); }
    public void ClearInputInjection(int player) { Record("ClearInputInjection"); }
    public bool SetInputInjectionPlaying(int player, bool playing) { Record("SetInputInjectionPlaying"); return default; }
    public bool SetInputInjectionLoop(int player, bool loop) { Record("SetInputInjectionLoop"); return default; }
    public bool SeekInputInjection(int player, long frame) { Record("SeekInputInjection"); return default; }
    public bool TryGetInputInjectionStatus(int player, out JuceSharp.JuceInputInjectionStatus status) { Record("TryGetInputInjectionStatus"); status = default; return default; }
    public void SetSongClockSource(JuceSharp.IJuceAudioSource source) => Record("SetSongClockSource");
    public void SetChartNotes(int player, System.ReadOnlySpan<JuceSharp.JuceChartNote> notes, System.ReadOnlySpan<JuceSharp.JuceChartBendPoint> bends) => Record("SetChartNotes");
    public void SetChartSourceEnabled(int player, bool enabled) => Record("SetChartSourceEnabled");
    public bool TrySetChartVoice(int player, JuceSharp.JuceChartVoice voice) { Record("TrySetChartVoice"); return default; }
    public void SetChartSourceParams(int player, float gainLinear, double offsetSeconds, JuceSharp.JuceChartSourceMode mode) => Record("SetChartSourceParams");
    public bool TryGetChartSourceStatus(int player, out JuceSharp.JuceChartSourceStatus status) { Record("TryGetChartSourceStatus"); status = default; return default; }
    public bool TrySetChartInstrument(int player, JuceSharp.JuceChartInstrument instrument) { Record("TrySetChartInstrument"); return default; }
    public JuceSharp.JuceChartInstrument? GetChartInstrument(int player, bool bass) { Record("GetChartInstrument"); return default; }
    public void SetInstrumentRoute(int player) => Record("SetInstrumentRoute");
    public int GetInstrumentRoute() { Record("GetInstrumentRoute"); return default; }
    public bool TryGetInputCaptureSync(out ulong capturedSamples, out long hostTicks) { Record("TryGetInputCaptureSync"); capturedSamples = default; hostTicks = default; return default; }
    public bool TryGetInputCaptureSync(int player, out ulong capturedSamples, out long hostTicks) { Record("TryGetInputCaptureSync"); capturedSamples = default; hostTicks = default; return default; }
    public void SetPlayerCount(int count) => Record("SetPlayerCount");
    public int GetPlayerCount() { Record("GetPlayerCount"); return default; }
    public void SetPlayerInputChannel(int player, int channel) => Record("SetPlayerInputChannel");
    public int GetPlayerInputChannel(int player) { Record("GetPlayerInputChannel"); return default; }
    public void SetPlayerMonitorGain(int player, float linearGain) => Record("SetPlayerMonitorGain");
    public float GetAndResetPlayerInputPeak(int player) { Record("GetAndResetPlayerInputPeak"); return default; }
    public bool TryShowAsioControlPanel(nint parentWindow, out string error) { Record("TryShowAsioControlPanel"); error = default; return default; }
    public void SetStretchQuality(bool faster) => Record("SetStretchQuality");
    public bool SetRemoteBridge(string shmName, string requestEventName, string responseEventName, bool enable) { Record("SetRemoteBridge"); return default; }
    public bool TryGetRemoteBridgeStats(out uint processed, out uint timeouts, out uint heartbeat) { Record("TryGetRemoteBridgeStats"); processed = default; timeouts = default; heartbeat = default; return default; }
    public void SendMidi(byte status, byte data1, byte data2) => Record("SendMidi");
    // Recorded UNDER ITS OWN NAME. Both overloads recorded "SendMidi", so no
    // test could assert that host code took the SAMPLE-ACCURATE path — which is
    // the entire point of the v34 addition.
    public void SendMidi(byte status, byte data1, byte data2, long renderFrame)
    {
        Record("SendMidiAt");
        ScheduledMidiFrames.Add(renderFrame);
    }

    /// <summary>Every <c>renderFrame</c> handed to the sample-accurate
    /// <c>SendMidi</c> overload, in order.</summary>
    public List<long> ScheduledMidiFrames { get; } = new();

    public (long Scheduled, long Late, long Dropped) GetMidiTimingStats() { Record("GetMidiTimingStats"); return default; }
    public bool TryGetMidiTimingStats(out MidiTimingStats stats) { Record("TryGetMidiTimingStats"); stats = default; return false; }
    public bool ResetMidiTimingStats() { Record("ResetMidiTimingStats"); return default; }

    /// <summary>What <see cref="OpenMidiOutput" /> hands back. Set by the test.</summary>
    public FakeJuceMidiOutput NextMidiOutput { get; set; } = new();

    public IJuceMidiOutput OpenMidiOutput(int deviceIndex)
    {
        Record("OpenMidiOutput");
        NextMidiOutput.DeviceIndex = deviceIndex;
        return NextMidiOutput;
    }
    public void SetMasterLimiter(bool enabled, float ceilingDb = -0.3f, float releaseMs = 80f) => Record("SetMasterLimiter");
    public (bool Enabled, float CeilingDb, float ReleaseMs) GetMasterLimiter() { Record("GetMasterLimiter"); return default; }
    public float MasterLimiterReductionDb { get { Record("MasterLimiterReductionDb"); return default; } }
    public int MasterLimiterLatencySamples { get { Record("MasterLimiterLatencySamples"); return default; } }
    public void SetInstrumentGain(float gain) => Record("SetInstrumentGain");
    public System.Collections.Generic.IReadOnlyList<JuceSharp.JuceAudioDevice> EnumerateOutputDevicesLive() { Record("EnumerateOutputDevicesLive"); return default; }
    public System.Collections.Generic.IReadOnlyList<JuceSharp.JuceAudioDevice> EnumerateInputDevicesLive() { Record("EnumerateInputDevicesLive"); return default; }
    public JuceSharp.Dsp.IJuceDspChain GetDspChain(int player) { Record("GetDspChain"); return default; }
    public JuceSharp.IJuceAudioSource OpenFile(string path)
    {
        Record("OpenFile");
        OpenedPath = path;
        return NextSource;
    }

    public JuceSharp.IJuceAudioSource OpenMemory(System.ReadOnlySpan<byte> bytes, string formatHint) { Record("OpenMemory"); return default; }
    public JuceSharp.IJuceAudioPlugin LoadPlugin(string path) { Record("LoadPlugin"); return default; }
    public JuceSharp.IJuceAudioPlugin LoadPlugin(string path, int componentIndex) { Record("LoadPlugin"); return default; }
    public JuceSharp.IJuceMidiInput OpenMidiInput(int deviceIndex, System.Action<JuceSharp.JuceMidiMessage> handler) { Record("OpenMidiInput"); return default; }
    public JuceSharp.IJuceMidiInput OpenMidiInput(int deviceIndex, JuceSharp.JuceMidiSpanHandler handler) { Record("OpenMidiInput"); return default; }

    // The async half. A fake completes SYNCHRONOUSLY on purpose: the point of
    // the seam is that a consumer's logic can be tested without a worker thread,
    // a device or a clock.
    public Task ReconfigureAsync(int sampleRate, int bufferFrames, string outputDeviceId, string inputDeviceId,
                                 CancellationToken cancellationToken = default)
    {
        Record(nameof(ReconfigureAsync));
        return Task.CompletedTask;
    }

    public Task SetDeviceTypeAsync(string typeName, string outputDeviceId = null, string inputDeviceId = null,
                                   uint outputChannelMask = 0, uint inputChannelMask = 0,
                                   CancellationToken cancellationToken = default)
    {
        Record(nameof(SetDeviceTypeAsync));
        return Task.CompletedTask;
    }

    public Task<JuceAsioPanelResult> ShowAsioControlPanelAsync(nint parentWindow,
                                                               CancellationToken cancellationToken = default)
    {
        Record(nameof(ShowAsioControlPanelAsync));
        return Task.FromResult(new JuceAsioPanelResult(false, "fake engine"));
    }

    public Task<IJuceAudioSource> OpenFileAsync(string path, CancellationToken cancellationToken = default)
    {
        Record(nameof(OpenFileAsync));
        OpenedPath = path;
        return Task.FromResult<IJuceAudioSource>(NextSource);
    }

    public Task<IJuceAudioPlugin> LoadPluginAsync(string path, CancellationToken cancellationToken = default)
    {
        Record(nameof(LoadPluginAsync));
        return Task.FromResult<IJuceAudioPlugin>(null);
    }

    public Task<IJuceAudioPlugin> LoadPluginAsync(string path, int componentIndex,
                                                  CancellationToken cancellationToken = default)
    {
        Record(nameof(LoadPluginAsync));
        return Task.FromResult<IJuceAudioPlugin>(null);
    }
}
