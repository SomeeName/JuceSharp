using JuceSharp.Dsp;

namespace JuceSharp;

/// <summary>
///     The contract of <see cref="JuceAudioEngine" />; implemented by
///     <see cref="JuceAudioEngine" />.
///     <para>
///         It exists so a consumer can UNIT-TEST the code that drives the engine.
///         Every wrapper type in this package is <c>sealed</c> with an
///         <c>internal</c> constructor — deliberately, because the only legitimate
///         way to get one is from a live native handle — which left a host with no
///         way to exercise its own transport logic, device-change handling or rig
///         wiring without <c>juce_sharp.dll</c> and a sound card. Fake this
///         interface instead; see <c>docs/guides/testing-without-native.md</c>.
///     </para>
///     <para>
///         <b>Instance members only.</b> The statics on
///         <see cref="JuceAudioEngine" /> (<c>NativeVersion</c>,
///         <c>EnumerateOutputDevices</c>, <c>GenerateWaveform</c>,
///         <c>RenderToWav</c>, <c>MaxPlayers</c>, …) are not on an interface
///         because an interface cannot make them substitutable; wrap the ones you
///         need behind your own seam.
///     </para>
///     <para>
///         The four factory members answer in INTERFACE terms
///         (<see cref="IJuceAudioSource" />, <see cref="IJuceAudioPlugin" />,
///         <see cref="IJuceMidiInput" />, <see cref="IJuceDspChain" />), which is
///         what makes a fake able to hand back fakes. On the concrete class they
///         are EXPLICIT implementations over the public ones, so a caller holding a
///         <see cref="JuceAudioEngine" /> still gets the concrete types back and no
///         existing signature changed.
///     </para>
///     <para>
///         <b>Every member below is <c>&lt;inheritdoc&gt;</c> onto its concrete
///         counterpart on <see cref="JuceAudioEngine" />, deliberately: one source
///         of truth, so a contract sentence cannot drift from the implementation it
///         describes. Pinned by <c>Wave2DocsInterfaceDocTests</c>.</b>
///     </para>
/// </summary>
public interface IJuceAudioEngine : IDisposable
{
    /// <inheritdoc cref="JuceAudioEngine.NativeHandle" />
    nint NativeHandle { get; }

    /// <inheritdoc cref="JuceAudioEngine.SampleRate" />
    int SampleRate { get; }

    /// <inheritdoc cref="JuceAudioEngine.BufferFrames" />
    int BufferFrames { get; }

    /// <inheritdoc cref="JuceAudioEngine.OutputLatencySamples" />
    int OutputLatencySamples { get; }

    /// <inheritdoc cref="JuceAudioEngine.StemGroupTempo" />
    float StemGroupTempo { get; set; }

    /// <inheritdoc cref="JuceAudioEngine.StemGroupPitchSemitones" />
    float StemGroupPitchSemitones { get; set; }

    /// <inheritdoc cref="JuceAudioEngine.Start" />
    void Start();

    /// <inheritdoc cref="JuceAudioEngine.Stop" />
    void Stop();

    /// <inheritdoc cref="JuceAudioEngine.Reconfigure(int, int, string, string)" />
    void Reconfigure(int sampleRate, int bufferFrames, string? outputDeviceId, string? inputDeviceId);

    /// <inheritdoc cref="JuceAudioEngine.SetDeviceType(string, string, string, uint, uint)" />
    void SetDeviceType(string? typeName,
                       string? outputDeviceId = null,
                       string? inputDeviceId = null,
                       uint outputChannelMask = 0,
                       uint inputChannelMask = 0);

    /// <inheritdoc cref="JuceAudioEngine.SetMonitorGain(float)" />
    void SetMonitorGain(float gain);

    /// <inheritdoc cref="JuceAudioEngine.SetInputGain(float)" />
    void SetInputGain(float gain);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShift(float)" />
    void SetInputPitchShift(float semitones);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShift(int, float)" />
    void SetInputPitchShift(int player, float semitones);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShiftMode(int)" />
    void SetInputPitchShiftMode(int mode);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShiftMode(JuceDropTuneMode)" />
    void SetInputPitchShiftMode(JuceDropTuneMode mode);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShiftMode(int, JuceDropTuneMode)" />
    void SetInputPitchShiftMode(int player, JuceDropTuneMode mode);

    /// <inheritdoc cref="JuceAudioEngine.SetInputPitchShiftMode(int, int)" />
    void SetInputPitchShiftMode(int player, int mode);

    /// <inheritdoc cref="JuceAudioEngine.SetMasterGain(float)" />
    void SetMasterGain(float gain);

    /// <inheritdoc cref="JuceAudioEngine.SetHighPass(float, float)" />
    void SetHighPass(float freqHz, float q);

    /// <inheritdoc cref="JuceAudioEngine.SetLowPass(float, float)" />
    void SetLowPass(float freqHz, float q);

    /// <inheritdoc cref="JuceAudioEngine.SetBandPass(float, float)" />
    void SetBandPass(float freqHz, float q);

    /// <inheritdoc cref="JuceAudioEngine.SetEqBand(int, int, float, float, float, bool)" />
    void SetEqBand(int index, int type, float freqHz, float q, float gainDb, bool enabled);

    /// <inheritdoc cref="JuceAudioEngine.SetEqBand(int, JuceEqBandType, float, float, float, bool)" />
    void SetEqBand(int index, JuceEqBandType type, float freqHz, float q, float gainDb, bool enabled);

    /// <inheritdoc cref="JuceAudioEngine.SetEqEnabled(bool)" />
    void SetEqEnabled(bool enabled);

    /// <inheritdoc cref="JuceAudioEngine.SetEqSolo(int)" />
    void SetEqSolo(int bandIndex);

    /// <inheritdoc cref="JuceAudioEngine.SetEqRoute(int)" />
    void SetEqRoute(int route);

    /// <inheritdoc cref="JuceAudioEngine.SetEqRoute(JuceEqRoute)" />
    void SetEqRoute(JuceEqRoute route);

    /// <inheritdoc cref="JuceAudioEngine.SetEqTrim(float)" />
    void SetEqTrim(float trimDb);

    /// <inheritdoc cref="JuceAudioEngine.GetEqActiveCount" />
    int GetEqActiveCount();

    /// <inheritdoc cref="JuceAudioEngine.ReadEqSpectrum(float[])" />
    int ReadEqSpectrum(float[] destDb);

    /// <inheritdoc cref="JuceAudioEngine.SetMirrorDevice(string)" />
    bool SetMirrorDevice(string? deviceId);

    /// <inheritdoc cref="JuceAudioEngine.SetMirrorOutput(string)" />
    /// <remarks>Carries the same <see cref="ObsoleteAttribute" /> as the concrete
    /// member it mirrors: the class implements this one IMPLICITLY, so without the
    /// attribute here a caller programming against the interface got no warning at
    /// all while the identical call on <see cref="JuceAudioEngine" /> did.</remarks>
    [Obsolete("Use SetMirrorDevice(string?) — this is the same call under a second name.")]
    bool SetMirrorOutput(string? deviceId);

    /// <inheritdoc cref="JuceAudioEngine.PlayClick(int)" />
    void PlayClick(int clickType);

    /// <inheritdoc cref="JuceAudioEngine.PlayClick(JuceClickType)" />
    void PlayClick(JuceClickType clickType);

    /// <inheritdoc cref="JuceAudioEngine.ScheduleClick(int, long)" />
    void ScheduleClick(int clickType, long renderFrame);

    /// <inheritdoc cref="JuceAudioEngine.ScheduleClick(JuceClickType, long)" />
    void ScheduleClick(JuceClickType clickType, long renderFrame);

    /// <inheritdoc cref="JuceAudioEngine.ClearScheduledClicks" />
    void ClearScheduledClicks();

    /// <inheritdoc cref="JuceAudioEngine.TryGetRenderClock(out long, out long)" />
    bool TryGetRenderClock(out long renderFrames, out long hostTicks);

    /// <inheritdoc cref="JuceAudioEngine.TryGetXrunStats(out XrunStats)" />
    bool TryGetXrunStats(out XrunStats stats);

    /// <inheritdoc cref="JuceAudioEngine.ResetXrunStats" />
    bool ResetXrunStats();

    /// <inheritdoc cref="JuceAudioEngine.TryGetCallbackLoad(out CallbackLoad)" />
    bool TryGetCallbackLoad(out CallbackLoad load);

    /// <inheritdoc cref="JuceAudioEngine.OpenExternalProcessInsert(int, string, string?)" />
    ExternalProcessInsertOpenResult OpenExternalProcessInsert(int player, string mappingName, string? magic = null);

    /// <inheritdoc cref="JuceAudioEngine.CloseExternalProcessInsert(int)" />
    bool CloseExternalProcessInsert(int player);

    /// <inheritdoc cref="JuceAudioEngine.TryGetExternalProcessInsertStats(int, out ExternalProcessInsertStats)" />
    bool TryGetExternalProcessInsertStats(int player, out ExternalProcessInsertStats stats);

    /// <inheritdoc cref="JuceAudioEngine.GetAndResetInputPeak" />
    float GetAndResetInputPeak();

    /// <inheritdoc cref="JuceAudioEngine.SetInputCaptureChannel(int)" />
    void SetInputCaptureChannel(int channel);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCapture(Span{float})" />
    int ReadInputCapture(Span<float> dest);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCapture(int, Span{float})" />
    int ReadInputCapture(int player, Span<float> dest);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCaptureStream(Span{float})" />
    int ReadInputCaptureStream(Span<float> dest);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCaptureStream(Span{float}, out ulong)" />
    int ReadInputCaptureStream(Span<float> dest, out ulong streamPosition);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCaptureStream(int, Span{float})" />
    int ReadInputCaptureStream(int player, Span<float> dest);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCaptureStream(int, Span{float}, out ulong)" />
    int ReadInputCaptureStream(int player, Span<float> dest, out ulong streamPosition);

    /// <inheritdoc cref="JuceAudioEngine.TryGetInputCaptureSync(out ulong, out long)" />
    bool TryGetInputCaptureSync(out ulong capturedSamples, out long hostTicks);

    /// <inheritdoc cref="JuceAudioEngine.TryGetInputCaptureSync(int, out ulong, out long)" />
    bool TryGetInputCaptureSync(int player, out ulong capturedSamples, out long hostTicks);

    /// <inheritdoc cref="JuceAudioEngine.ReadInputCaptureFrom(int, ulong, Span{float}, out ulong)" />
    int ReadInputCaptureFrom(int player, ulong from, Span<float> dest, out ulong firstPosition);

    /// <inheritdoc cref="JuceAudioEngine.SetInputInjection(int, ReadOnlySpan{float}, bool)" />
    void SetInputInjection(int player, ReadOnlySpan<float> mono, bool loop);

    /// <inheritdoc cref="JuceAudioEngine.ClearInputInjection(int)" />
    void ClearInputInjection(int player);

    /// <inheritdoc cref="JuceAudioEngine.SetInputInjectionPlaying(int, bool)" />
    bool SetInputInjectionPlaying(int player, bool playing);

    /// <inheritdoc cref="JuceAudioEngine.SetInputInjectionLoop(int, bool)" />
    bool SetInputInjectionLoop(int player, bool loop);

    /// <inheritdoc cref="JuceAudioEngine.SeekInputInjection(int, long)" />
    bool SeekInputInjection(int player, long frame);

    /// <inheritdoc cref="JuceAudioEngine.TryGetInputInjectionStatus(int, out JuceInputInjectionStatus)" />
    bool TryGetInputInjectionStatus(int player, out JuceInputInjectionStatus status);

    /// <summary>
    ///     Binds the song clock the chart sources follow (ABI v38); see
    ///     <see cref="JuceAudioEngine.SetSongClockSource(JuceAudioSource?)" />. An
    ///     implementation must reject a source it did not open.
    /// </summary>
    void SetSongClockSource(IJuceAudioSource? source);

    /// <inheritdoc cref="JuceAudioEngine.SetChartNotes(int, ReadOnlySpan{JuceChartNote}, ReadOnlySpan{JuceChartBendPoint})" />
    void SetChartNotes(int player, ReadOnlySpan<JuceChartNote> notes, ReadOnlySpan<JuceChartBendPoint> bends);

    /// <inheritdoc cref="JuceAudioEngine.SetChartSourceEnabled(int, bool)" />
    void SetChartSourceEnabled(int player, bool enabled);

    /// <inheritdoc cref="JuceAudioEngine.TrySetChartVoice(int, JuceChartVoice)" />
    bool TrySetChartVoice(int player, JuceChartVoice voice);

    /// <inheritdoc cref="JuceAudioEngine.SetChartSourceParams(int, float, double, JuceChartSourceMode)" />
    void SetChartSourceParams(int player, float gainLinear, double offsetSeconds, JuceChartSourceMode mode);

    /// <inheritdoc cref="JuceAudioEngine.TryGetChartSourceStatus(int, out JuceChartSourceStatus)" />
    bool TryGetChartSourceStatus(int player, out JuceChartSourceStatus status);

    /// <inheritdoc cref="JuceAudioEngine.TrySetChartInstrument(int, JuceChartInstrument)" />
    bool TrySetChartInstrument(int player, JuceChartInstrument instrument);

    /// <inheritdoc cref="JuceAudioEngine.GetChartInstrument(int, bool)" />
    JuceChartInstrument? GetChartInstrument(int player, bool bass);

    /// <inheritdoc cref="JuceAudioEngine.SetInstrumentRoute(int)" />
    void SetInstrumentRoute(int player);

    /// <inheritdoc cref="JuceAudioEngine.GetInstrumentRoute" />
    int GetInstrumentRoute();

    /// <inheritdoc cref="JuceAudioEngine.SetPlayerCount(int)" />
    void SetPlayerCount(int count);

    /// <inheritdoc cref="JuceAudioEngine.GetPlayerCount" />
    int GetPlayerCount();

    /// <inheritdoc cref="JuceAudioEngine.SetPlayerInputChannel(int, int)" />
    void SetPlayerInputChannel(int player, int channel);

    /// <inheritdoc cref="JuceAudioEngine.GetPlayerInputChannel(int)" />
    int GetPlayerInputChannel(int player);

    /// <inheritdoc cref="JuceAudioEngine.SetPlayerMonitorGain(int, float)" />
    void SetPlayerMonitorGain(int player, float linearGain);

    /// <inheritdoc cref="JuceAudioEngine.GetAndResetPlayerInputPeak(int)" />
    float GetAndResetPlayerInputPeak(int player);

    /// <inheritdoc cref="JuceAudioEngine.TryShowAsioControlPanel(nint, out string)" />
    bool TryShowAsioControlPanel(nint parentWindow, out string? error);

    /// <inheritdoc cref="JuceAudioEngine.SetStretchQuality(bool)" />
    void SetStretchQuality(bool faster);

    /// <inheritdoc cref="JuceAudioEngine.SetRemoteBridge(string, string, string, bool)" />
    bool SetRemoteBridge(string? shmName, string? requestEventName, string? responseEventName, bool enable);

    /// <inheritdoc cref="JuceAudioEngine.TryGetRemoteBridgeStats(out uint, out uint, out uint)" />
    bool TryGetRemoteBridgeStats(out uint processed, out uint timeouts, out uint heartbeat);

    /// <inheritdoc cref="JuceAudioEngine.SendMidi(byte, byte, byte)" />
    void SendMidi(byte status, byte data1, byte data2);

    /// <inheritdoc cref="JuceAudioEngine.SetInstrumentGain(float)" />
    void SetInstrumentGain(float gain);

    /// <inheritdoc cref="JuceAudioEngine.EnumerateOutputDevicesLive" />
    IReadOnlyList<JuceAudioDevice> EnumerateOutputDevicesLive();

    /// <inheritdoc cref="JuceAudioEngine.EnumerateInputDevicesLive" />
    IReadOnlyList<JuceAudioDevice> EnumerateInputDevicesLive();

    // --- factories: interface in, interface out -------------------------

    /// <inheritdoc cref="JuceAudioEngine.GetDspChain(int)" />
    IJuceDspChain GetDspChain(int player = 0);

    /// <inheritdoc cref="JuceAudioEngine.OpenFile(string)" />
    IJuceAudioSource OpenFile(string path);

    /// <inheritdoc cref="JuceAudioEngine.OpenMemory(ReadOnlySpan{byte}, string)" />
    IJuceAudioSource OpenMemory(ReadOnlySpan<byte> bytes, string? formatHint = null);

    /// <inheritdoc cref="JuceAudioEngine.LoadPlugin(string)" />
    IJuceAudioPlugin LoadPlugin(string path);

    /// <inheritdoc cref="JuceAudioEngine.LoadPlugin(string, int)" />
    IJuceAudioPlugin LoadPlugin(string path, int componentIndex);

    /// <inheritdoc cref="JuceAudioEngine.OpenMidiInput(int, Action{JuceMidiMessage})" />
    IJuceMidiInput OpenMidiInput(int deviceIndex, Action<JuceMidiMessage> handler);

    /// <inheritdoc cref="JuceAudioEngine.OpenMidiInput(int, JuceMidiSpanHandler)" />
    IJuceMidiInput OpenMidiInput(int deviceIndex, JuceMidiSpanHandler handler);

    // --- async: one dedicated worker thread per engine, never the pool -----
    // See JuceAudioEngine.Async.cs for the threading and cancellation contract.

    /// <inheritdoc cref="JuceAudioEngine.ReconfigureAsync(int, int, string, string, CancellationToken)" />
    Task ReconfigureAsync(int sampleRate, int bufferFrames,
                          string? outputDeviceId, string? inputDeviceId,
                          CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.SetDeviceTypeAsync(string, string, string, uint, uint, CancellationToken)" />
    Task SetDeviceTypeAsync(string? typeName,
                            string? outputDeviceId = null,
                            string? inputDeviceId = null,
                            uint outputChannelMask = 0,
                            uint inputChannelMask = 0,
                            CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.ShowAsioControlPanelAsync(nint, CancellationToken)" />
    Task<JuceAsioPanelResult> ShowAsioControlPanelAsync(nint parentWindow,
                                                       CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.OpenFileAsync(string, CancellationToken)" />
    Task<IJuceAudioSource> OpenFileAsync(string path, CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.LoadPluginAsync(string, CancellationToken)" />
    Task<IJuceAudioPlugin> LoadPluginAsync(string path, CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.LoadPluginAsync(string, int, CancellationToken)" />
    Task<IJuceAudioPlugin> LoadPluginAsync(string path, int componentIndex,
                                           CancellationToken cancellationToken = default);

    /// <inheritdoc cref="JuceAudioEngine.SetMasterLimiter" />
    void SetMasterLimiter(bool enabled, float ceilingDb = -0.3f, float releaseMs = 80f);

    /// <inheritdoc cref="JuceAudioEngine.GetMasterLimiter" />
    (bool Enabled, float CeilingDb, float ReleaseMs) GetMasterLimiter();

    /// <inheritdoc cref="JuceAudioEngine.MasterLimiterReductionDb" />
    float MasterLimiterReductionDb { get; }

    /// <inheritdoc cref="JuceAudioEngine.MasterLimiterLatencySamples" />
    int MasterLimiterLatencySamples { get; }

    /// <inheritdoc cref="JuceAudioEngine.SendMidi(byte, byte, byte, long)" />
    void SendMidi(byte status, byte data1, byte data2, long renderFrame);

    /// <inheritdoc cref="JuceAudioEngine.GetMidiTimingStats" />
    (long Scheduled, long Late, long Dropped) GetMidiTimingStats();

    /// <inheritdoc cref="JuceAudioEngine.TryGetMidiTimingStats" />
    bool TryGetMidiTimingStats(out MidiTimingStats stats);

    /// <inheritdoc cref="JuceAudioEngine.ResetMidiTimingStats" />
    bool ResetMidiTimingStats();

    /// <inheritdoc cref="JuceAudioEngine.OpenMidiOutput" />
    IJuceMidiOutput OpenMidiOutput(int deviceIndex);
}
