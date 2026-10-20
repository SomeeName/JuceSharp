using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using JuceSharp.AudioIo;
using JuceSharp.Dsp;

namespace JuceSharp.Examples.AvaloniaDemo.ViewModels;

/// <summary>
/// Tab-based showcase that drives the entire managed JuceSharp surface:
/// engine + driver/device selection (WASAPI/ASIO/etc.), transport with
/// seek + A/B loop, source volume / pan / tempo / pitch / mute / stretch,
/// master / monitor / input gain with live peak, biquad HP/LP/BP, live
/// FFT spectrum + chromatic tuner against the decoded file at the
/// playhead, offline BPM/beat + chord detection + spectrogram, and VST3
/// plugin hosting (load, attach to input or instrument, editor window,
/// bypass, parameter list, simple MIDI keyboard).
/// </summary>
public sealed partial class MainWindowViewModel : ObservableObject, IDisposable
{
    private readonly DispatcherTimer _positionTimer;
    private readonly DispatcherTimer _liveTimer;
    private readonly DispatcherTimer _meterTimer;

    private JuceAudioEngine? _engine;
    private JuceAudioSource? _source;
    private JuceAudioPlugin? _plugin;

    private AudioBuffer? _decoded;
    private float[]? _decodedMono;
    private CancellationTokenSource? _analysisCts;
    private string? _loadedPath;

    private JuceFft? _liveFft;
    private float[] _liveWindow = Array.Empty<float>();
    private float[] _livePeak   = Array.Empty<float>();

    private JuceChromaticTuner? _tuner;
    private IReadOnlyList<JuceAudioHost> _hostCache = Array.Empty<JuceAudioHost>();

    private bool _initialised;

    public MainWindowViewModel()
    {
        OutputDevices  = new ObservableCollection<DeviceItem>();
        InputDevices   = new ObservableCollection<DeviceItem>();
        InputChannels  = new ObservableCollection<InputChannelItem>();
        ChannelModes   = new ObservableCollection<ChannelModeItem>(BuildChannelModes());
        Vst3Plugins    = new ObservableCollection<Vst3Item>();
        Drivers        = new ObservableCollection<DriverItem>(EnumerateDrivers());

        OutputChannelMode = ChannelModes.First(c => c.Mask == ChannelMask.Stereo);
        InputChannelMode  = ChannelModes.First(c => c.Mask == 0); // auto/default
        ChordTimeline  = new ObservableCollection<ChordRow>();
        BeatTimeline   = new ObservableCollection<BeatRow>();
        Log            = new ObservableCollection<string>();
        PluginParams   = new ObservableCollection<PluginParam>();

        SelectedDriver = Drivers.First();

        _positionTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(33) };
        _positionTimer.Tick += (_, _) => RefreshPosition();

        _liveTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _liveTimer.Tick += (_, _) => RefreshLiveDsp();

        _meterTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(50) };
        _meterTimer.Tick += (_, _) => RefreshInputPeak();

        _initialised = true;

        AppendLog($"JuceSharp Avalonia demo started. Native v{SafeNativeVersion()}.");
        InitToneRig(); // catalogue + meter/dropout poll timer (MainWindowViewModel.ToneRig.cs)
        StartEngineCommand.Execute(null);
    }

    // ------------------------------------------------------------------ Observable state

    public ObservableCollection<DeviceItem>  OutputDevices  { get; }
    public ObservableCollection<DeviceItem>  InputDevices   { get; }
    public ObservableCollection<InputChannelItem> InputChannels { get; }
    public ObservableCollection<ChannelModeItem>  ChannelModes  { get; }
    public ObservableCollection<Vst3Item>    Vst3Plugins    { get; }
    public ObservableCollection<DriverItem>  Drivers        { get; }
    public ObservableCollection<ChordRow>    ChordTimeline  { get; }
    public ObservableCollection<BeatRow>     BeatTimeline   { get; }
    public ObservableCollection<string>      Log            { get; }
    public ObservableCollection<PluginParam> PluginParams   { get; }

    private readonly List<DeviceItem> _allOutputs = new();
    private readonly List<DeviceItem> _allInputs  = new();

    [ObservableProperty] private DriverItem? _selectedDriver;
    [ObservableProperty] private DeviceItem? _selectedOutput;
    [ObservableProperty] private DeviceItem? _selectedInput;
    [ObservableProperty] private InputChannelItem? _selectedInputChannel;
    [ObservableProperty] private ChannelModeItem? _outputChannelMode;
    [ObservableProperty] private ChannelModeItem? _inputChannelMode;
    [ObservableProperty] private bool _liveDspFromInput = true;
    [ObservableProperty] private int _sampleRate   = 48_000;
    [ObservableProperty] private int _bufferFrames = 512;
    [ObservableProperty] private string _engineStatus = "stopped";

    [ObservableProperty] private string _loadedFile = "(no file loaded)";
    [ObservableProperty] private double _lengthSeconds;
    [ObservableProperty] private double _positionSeconds;
    [ObservableProperty] private string _positionDisplay = "00:00";

    [ObservableProperty] private float _volume = 1.0f;
    [ObservableProperty] private float _pan;
    [ObservableProperty] private float _tempoRatio = 1.0f;
    [ObservableProperty] private float _pitchSemitones;
    [ObservableProperty] private bool  _mute;
    [ObservableProperty] private bool  _fasterStretch;

    [ObservableProperty] private double _loopStart;
    [ObservableProperty] private double _loopEnd;
    [ObservableProperty] private bool   _loopEnabled;

    [ObservableProperty] private float _masterGain  = 1.0f;
    [ObservableProperty] private float _monitorGain = 1.0f;
    [ObservableProperty] private float _inputGain   = 1.0f;
    [ObservableProperty] private float _inputPeak;

    [ObservableProperty] private float _hpFreq = 20f;
    [ObservableProperty] private float _hpQ    = 0.707f;
    [ObservableProperty] private float _lpFreq = 20_000f;
    [ObservableProperty] private float _lpQ    = 0.707f;
    [ObservableProperty] private float _bpFreq = 1_000f;
    [ObservableProperty] private float _bpQ    = 0.707f;

    [ObservableProperty] private float[]? _waveformMins;
    [ObservableProperty] private float[]? _waveformMaxs;
    [ObservableProperty] private float[][]? _spectrogram;
    [ObservableProperty] private double _spectrogramRatio;

    [ObservableProperty] private float[]? _liveSpectrum;
    [ObservableProperty] private float[]? _liveSpectrumPeak;
    [ObservableProperty] private string?  _tunerNote;
    [ObservableProperty] private double   _tunerCents;
    [ObservableProperty] private double   _tunerFreq;
    [ObservableProperty] private bool     _tunerHasReading;

    [ObservableProperty] private float _bpm;
    [ObservableProperty] private int   _beatCount;

    [ObservableProperty] private string  _pluginPath = string.Empty;
    [ObservableProperty] private string  _pluginStatus = "(no plugin loaded)";
    [ObservableProperty] private bool    _pluginAsInstrument;
    [ObservableProperty] private bool    _pluginBypass;

    [ObservableProperty] private bool _analysisRunning;
    [ObservableProperty] private string _statusLine = "Ready.";

    // ------------------------------------------------------------------ Engine

    [RelayCommand]
    private void StartEngine()
    {
        try
        {
            DisposeEngine();
            var cfg = BuildConfig(SelectedOutput?.Device.Id, SelectedInput?.Device.Id);
            var result = JuceEngineFactory.OpenWithFallback(cfg);
            foreach (var line in result.Attempts) AppendLog(line);
            _engine = result.Engine;
            if (_engine is null)
            {
                EngineStatus = "FAILED";
                AppendLog($"No openable device found for driver '{DriverLabel()}'. Try a different host.");
                return;
            }
            _engine.Start();
            EngineStatus = $"running @ {_engine.SampleRate} Hz / {_engine.BufferFrames} frames ({DriverLabel()}; out='{result.ConfigUsed.OutputDeviceId}')";
            AppendLog($"Engine started. {EngineStatus}");
            EnsureLiveDspGear(_engine.SampleRate);
            RebuildInputChannels();
            ApplySelectedInputChannel();
            RefreshDevicesFromEngine();
            _liveTimer.Start();
            _meterTimer.Start();
            OnEngineReplacedForRig(); // a new engine has an empty chain
        }
        catch (Exception ex)
        {
            AppendLog($"Engine start failed: {ex.Message}");
            EngineStatus = "FAILED";
        }
    }

    /// <summary>Fast in-place host swap when an engine is already running.
    /// Falls back to a fresh OpenWithFallback if the in-place swap fails.</summary>
    private void SwitchHostInPlace()
    {
        if (_engine is null) { StartEngine(); return; }
        try
        {
            _engine.SetDeviceType(SelectedDriver?.TypeName ?? string.Empty,
                outputDeviceId:    SelectedOutput?.Device.Id,
                inputDeviceId:     SelectedInput?.Device.Id,
                outputChannelMask: OutputChannelMode?.Mask ?? 0,
                inputChannelMask:  InputChannelMode?.Mask  ?? 0);
            EngineStatus = $"running @ {_engine.SampleRate} Hz / {_engine.BufferFrames} frames ({DriverLabel()})";
            EnsureLiveDspGear(_engine.SampleRate);
            RebuildInputChannels();
            ApplySelectedInputChannel();
            RefreshDevicesFromEngine();
            AppendLog($"Host swapped in-place to '{DriverLabel()}'.");
        }
        catch (Exception ex)
        {
            // Don't fall back to a fresh StartEngine here — the previous
            // engine is still running on its current host, and silently
            // tearing it down to retry the user's swap was the source of
            // the "engine created 3×" loop the user reported. Surface the
            // failure instead so the UI status reflects reality.
            AppendLog($"In-place host swap to '{DriverLabel()}' failed: {ex.Message}");
            EngineStatus = $"swap to '{DriverLabel()}' FAILED — still on previous host";
        }
    }

    private JuceEngineConfig BuildConfig(string? outputId, string? inputId) => new()
    {
        SampleRate        = SampleRate,
        BufferFrames      = BufferFrames,
        PreferredDriver   = SelectedDriver?.Driver  ?? AudioDriver.Default,
        OutputTypeName    = SelectedDriver?.TypeName ?? string.Empty,
        InputTypeName     = SelectedDriver?.TypeName ?? string.Empty,
        OutputDeviceId    = outputId ?? string.Empty,
        InputDeviceId     = inputId  ?? string.Empty,
        OutputChannelMask = OutputChannelMode?.Mask ?? 0,
        InputChannelMask  = InputChannelMode?.Mask  ?? 0,
    };

    [RelayCommand]
    private void RefreshDevices() => RefreshDevicesForDriver();

    private void RefreshDevicesForDriver()
    {
        try
        {
            // Library helper: probes the chosen host (spinning a short-lived
            // engine when needed for ASIO/CoreAudio/etc.) and returns its
            // devices. Cached in the catalog so repeated calls are cheap.
            var typeName = SelectedDriver?.TypeName ?? string.Empty;
            IReadOnlyList<JuceAudioDevice> outs = Array.Empty<JuceAudioDevice>();
            IReadOnlyList<JuceAudioDevice> ins  = Array.Empty<JuceAudioDevice>();
            if (string.IsNullOrEmpty(typeName))
            {
                outs = JuceAudioEngine.EnumerateOutputDevices();
                ins  = JuceAudioEngine.EnumerateInputDevices();
            }
            else
            {
                var host = _hostCache.FirstOrDefault(h =>
                    string.Equals(h.TypeName, typeName, StringComparison.OrdinalIgnoreCase));
                if (host is null)
                {
                    _hostCache = JuceDeviceCatalog.Enumerate();
                    host = _hostCache.FirstOrDefault(h =>
                        string.Equals(h.TypeName, typeName, StringComparison.OrdinalIgnoreCase));
                }
                if (host is not null) { outs = host.OutputDevices; ins = host.InputDevices; }
            }

            _allOutputs.Clear();
            _allInputs.Clear();
            foreach (var d in outs) _allOutputs.Add(new DeviceItem(d));
            foreach (var d in ins)  _allInputs .Add(new DeviceItem(d));
            FilterDeviceLists();
            AppendLog($"Probed '{DriverLabel()}': {OutputDevices.Count} output, {InputDevices.Count} input devices.");
        }
        catch (Exception ex)
        {
            AppendLog($"Device probe failed: {ex.Message}");
        }
    }

    private void RefreshDevicesFromEngine()
    {
        // After an in-place host swap, the device list might have changed
        // (e.g. ASIO -> WASAPI exposes different output devices). Re-run
        // the host-scoped catalog rather than EnumerateOutputDevicesLive()
        // — the latter returns devices from EVERY host on the system, which
        // would let the user accidentally pin a WASAPI device on an ASIO
        // engine and trigger "No such device" loops.
        RefreshDevicesForDriver();
    }

    private void FilterDeviceLists()
    {
        // Output combo only lists devices that actually expose output channels;
        // input combo only lists devices with input channels. ASIO drivers
        // typically expose both sides on the same device record, so this is
        // safe — and avoids showing recording-only devices as playback options.
        OutputDevices.Clear();
        foreach (var d in _allOutputs)
            if (d.Device.MaxOutputChannels > 0 || _allOutputs.Count == _allInputs.Count) OutputDevices.Add(d);

        InputDevices.Clear();
        foreach (var d in _allInputs)
            if (d.Device.MaxInputChannels > 0 || _allOutputs.Count == _allInputs.Count) InputDevices.Add(d);

        // Auto-pick a default ONLY when no engine is running. Once the engine
        // is alive, leave the selection as the user left it (or null) — never
        // silently re-pin the user to "OutputDevices.FirstOrDefault()" on
        // every refresh, because that first device may be offline (e.g. ASIO
        // Thunderbolt 0x54f) and would cause subsequent settings changes to
        // pass a known-bad device ID back into native.
        if (_engine is null)
        {
            if (SelectedOutput is null || !OutputDevices.Contains(SelectedOutput))
                SelectedOutput = OutputDevices.FirstOrDefault(x => x.Device.IsDefault) ?? OutputDevices.FirstOrDefault();
            if (SelectedInput is null || !InputDevices.Contains(SelectedInput))
                SelectedInput = InputDevices.FirstOrDefault(x => x.Device.IsDefault) ?? InputDevices.FirstOrDefault();
        }
        else
        {
            // Clear stale references if the prior selection isn't in the new host's list.
            if (SelectedOutput is not null && !OutputDevices.Contains(SelectedOutput)) SelectedOutput = null;
            if (SelectedInput  is not null && !InputDevices .Contains(SelectedInput )) SelectedInput  = null;
        }
    }

    private string DriverLabel()
        => string.IsNullOrEmpty(SelectedDriver?.TypeName) ? "auto" : SelectedDriver!.TypeName;

    partial void OnSelectedDriverChanged(DriverItem? value)
    {
        if (!_initialised) return;
        // Refresh devices for the new host first (native catalog ABI v13 —
        // scans device names without opening anything, completes in ms).
        RefreshDevicesForDriver();
        if (_engine is not null)
        {
            StatusLine = $"Switching to {DriverLabel()}…";
            // Clear the device pins so the native device-walk fallback can
            // skip offline ASIO devices (e.g. Thunderbolt 0x54f) and land on
            // a working one (USB ASIO / ReaRoute / ...).
            SelectedOutput = null;
            SelectedInput  = null;
            SwitchHostInPlace();
        }
        else
        {
            EngineStatus = "(driver changed — pick a device and press Apply or Start engine)";
        }
    }

    [RelayCommand]
    private void ApplyDeviceSettings()
    {
        // Prefer the fast in-place native swap (ABI v12) when an engine is
        // already running — same-host SR/buffer/device changes never need
        // to re-create the engine, and even cross-host swaps (ASIO -> WASAPI)
        // are handled in a single JUCE call. Falls back to a full open with
        // device-walk fallback if the in-place swap fails.
        if (_engine is not null) SwitchHostInPlace();
        else                     StartEngine();
    }

    [RelayCommand]
    private void ShowAsioPanel()
    {
        if (_engine is null) return;
        if (_engine.TryShowAsioControlPanel(IntPtr.Zero, out var err))
            AppendLog("ASIO control panel opened.");
        else
            AppendLog($"ASIO control panel unavailable: {err ?? "(no detail)"}");
    }

    // ------------------------------------------------------------------ File / transport

    /// <summary>
    /// Opens a file for playback and analysis. Async because BOTH halves take seconds: the engine's
    /// own open is a decode plus a resample (0.9 gives it an async twin that runs on the engine's
    /// dedicated worker thread, never the thread pool — parking a pool thread for seconds makes the
    /// pool inject threads while the rest of the host's async work starves), and the managed decode
    /// behind the waveform/analysis views is pure CPU, which is what Task.Run is actually for.
    /// Doing either on the UI thread is a frozen window for the length of the file.
    /// </summary>
    public async Task OpenFileAsync(string path)
    {
        if (_engine is null) StartEngine();
        if (_engine is null) return;
        try
        {
            _source?.Stop();
            _source?.Dispose();
            _source = await _engine.OpenFileAsync(path).ConfigureAwait(true);
            ApplySourceState();

            _loadedPath   = path;
            LoadedFile    = Path.GetFileName(path);
            LengthSeconds = _source.Length.TotalSeconds;
            LoopStart     = 0;
            LoopEnd       = LengthSeconds;

            _decoded     = await Task.Run(() => JuceAudioDecoder.Decode(path)).ConfigureAwait(true);
            _decodedMono = _decoded.ToMono();
            BuildWaveformPeaks();

            // Reset live DSP gear to match decoded sample-rate (only the
            // file-tuner uses this rate; live-input mode follows engine SR).
            EnsureLiveDspGear(_decoded.SampleRate);

            AppendLog($"Loaded '{path}' — {LengthSeconds:F2}s, {_decoded.SampleRate} Hz, {_decoded.Channels} ch.");
            _positionTimer.Start();
        }
        catch (Exception ex)
        {
            AppendLog($"Failed to open '{path}': {ex.Message}");
        }
    }

    [RelayCommand] private void Play()  { try { _source?.Play();  } catch (Exception e) { AppendLog($"Play: {e.Message}"); } }
    [RelayCommand] private void Pause() { try { _source?.Pause(); } catch (Exception e) { AppendLog($"Pause: {e.Message}"); } }
    [RelayCommand] private void Stop()  { try { _source?.Stop();  } catch (Exception e) { AppendLog($"Stop: {e.Message}"); } }

    [RelayCommand] private void PlayClick() { try { _engine?.PlayClick(0); } catch (Exception e) { AppendLog($"Click: {e.Message}"); } }

    public void SeekTo(double seconds)
    {
        try { _source?.Seek(TimeSpan.FromSeconds(Math.Max(0, seconds))); }
        catch (Exception e) { AppendLog($"Seek: {e.Message}"); }
    }

    // ------------------------------------------------------------------ Property change handlers

    partial void OnVolumeChanged(float value)         { if (_source is not null) try { _source.Volume = value; } catch { } }
    partial void OnPanChanged(float value)            { if (_source is not null) try { _source.Pan = value; } catch { } }
    partial void OnTempoRatioChanged(float value)     { if (_source is not null) try { _source.TempoRatio = value; } catch { } }
    partial void OnPitchSemitonesChanged(float value) { if (_source is not null) try { _source.PitchSemitones = value; } catch { } }
    partial void OnMuteChanged(bool value)            { if (_source is not null) try { _source.Mute = value; } catch { } }
    partial void OnFasterStretchChanged(bool value)   { try { _engine?.SetStretchQuality(value); } catch { } }

    partial void OnMasterGainChanged(float value)  { try { _engine?.SetMasterGain(value); }  catch { } }
    partial void OnMonitorGainChanged(float value) { try { _engine?.SetMonitorGain(value); } catch { } }
    partial void OnSelectedInputChanged(DeviceItem? value)
    {
        if (!_initialised) return;
        RebuildInputChannels();
    }

    partial void OnSelectedInputChannelChanged(InputChannelItem? value)
    {
        ApplySelectedInputChannel();
    }

    private void ApplySelectedInputChannel()
    {
        try { _engine?.SetInputCaptureChannel(SelectedInputChannel?.Channel ?? 0); }
        catch { }
    }

    private void RebuildInputChannels()
    {
        var prev = SelectedInputChannel?.Channel ?? 0;
        InputChannels.Clear();
        var ch = SelectedInput?.Device.MaxInputChannels ?? 2;
        if (ch <= 0) ch = 2;
        InputChannels.Add(new InputChannelItem(-1, "Mix (sum to mono)"));
        for (var i = 0; i < ch; i++)
            InputChannels.Add(new InputChannelItem(i, $"Channel {i + 1}"));
        SelectedInputChannel = InputChannels.FirstOrDefault(c => c.Channel == prev) ?? InputChannels[1];
    }

    private void EnsureLiveDspGear(int sampleRate)
    {
        if (sampleRate <= 0) sampleRate = 48_000;
        _liveFft    ??= new JuceFft(2048);
        if (_liveWindow.Length != 2048) _liveWindow = HannWindow(2048);
        if (_livePeak.Length   != 1024) _livePeak   = new float[1024];
        if (_tuner is null || _tuner.SampleRate != sampleRate)
        {
            _tuner = new JuceChromaticTuner(sampleRate);
        }
    }

    partial void OnInputGainChanged(float value)   { try { _engine?.SetInputGain(value); }   catch { } }

    partial void OnHpFreqChanged(float value) => ApplyHp();
    partial void OnHpQChanged(float value)    => ApplyHp();
    partial void OnLpFreqChanged(float value) => ApplyLp();
    partial void OnLpQChanged(float value)    => ApplyLp();
    partial void OnBpFreqChanged(float value) => ApplyBp();
    partial void OnBpQChanged(float value)    => ApplyBp();

    private void ApplyHp() { try { _engine?.SetHighPass(HpFreq, HpQ); } catch { } }
    private void ApplyLp() { try { _engine?.SetLowPass(LpFreq, LpQ);  } catch { } }
    private void ApplyBp() { try { _engine?.SetBandPass(BpFreq, BpQ); } catch { } }

    partial void OnLoopEnabledChanged(bool value) => ApplyLoop();
    partial void OnLoopStartChanged(double value) => ApplyLoop();
    partial void OnLoopEndChanged(double value)   => ApplyLoop();

    private void ApplyLoop()
    {
        if (_source is null) return;
        try
        {
            if (LoopEnabled && LoopEnd > LoopStart)
            {
                _source.Loop = new JuceLoopRegion(TimeSpan.FromSeconds(LoopStart),
                                                  TimeSpan.FromSeconds(LoopEnd));
            }
            else
            {
                _source.Loop = null;
            }
        }
        catch (Exception ex) { AppendLog($"Loop: {ex.Message}"); }
    }

    private void ApplySourceState()
    {
        if (_source is null) return;
        try
        {
            _source.Volume          = Volume;
            _source.Pan             = Pan;
            _source.TempoRatio      = TempoRatio;
            _source.PitchSemitones  = PitchSemitones;
            _source.Mute            = Mute;
        }
        catch { }
    }

    // ------------------------------------------------------------------ Timers

    private void RefreshPosition()
    {
        if (_source is null) return;
        try
        {
            var p = _source.AudiblePosition.TotalSeconds;
            PositionSeconds = p;
            PositionDisplay = TimeSpan.FromSeconds(p).ToString(@"mm\:ss\.ff");
            if (LengthSeconds > 0)
                SpectrogramRatio = Math.Clamp(p / LengthSeconds, 0, 1);
        }
        catch { }
    }

    private void RefreshInputPeak()
    {
        try { if (_engine is not null) InputPeak = _engine.GetAndResetInputPeak(); }
        catch { }
    }

    private void RefreshLiveDsp()
    {
        // Live-input mode is the default: the FFT spectrum + tuner are
        // computed from the DRY (pre-VST, pre-gain) input signal captured
        // by the engine, so the user sees what their guitar is actually
        // feeding into the chain — independent of file playback.
        if (LiveDspFromInput && _engine is not null)
        {
            try
            {
                EnsureLiveDspGear(_engine.SampleRate);
                if (_liveFft is null) return;
                var win = _liveWindow.Length;
                Span<float> input = stackalloc float[2048];
                if (win != 2048) input = new float[win];
                var got = _engine.ReadInputCapture(input);
                if (got <= 0) return;
                RenderSpectrumAndTuner(input, _engine.SampleRate);
            }
            catch { }
            return;
        }

        if (_decodedMono is null || _decoded is null || _source is null || _liveFft is null) return;
        try
        {
            var sr  = _decoded.SampleRate;
            var pos = (int)(_source.AudiblePosition.TotalSeconds * sr);
            var win = _liveWindow.Length;
            if (pos + win >= _decodedMono.Length || pos < 0) return;
            RenderSpectrumAndTuner(new ReadOnlySpan<float>(_decodedMono, pos, win), sr);
        }
        catch { }
    }

    private void RenderSpectrumAndTuner(ReadOnlySpan<float> mono, int sampleRate)
    {
        if (_liveFft is null) return;
        var win = _liveWindow.Length;
        if (mono.Length < win) return;

        var re = new float[win];
        var im = new float[win];
        for (var i = 0; i < win; i++)
        {
            re[i] = mono[i] * _liveWindow[i];
            im[i] = 0;
        }
        _liveFft.Forward(re, im);

        var bins = win / 2;
        var mags = new float[bins];
        var fmax = 1e-6f;
        for (var k = 0; k < bins; k++)
        {
            mags[k] = MathF.Sqrt(re[k] * re[k] + im[k] * im[k]);
            if (mags[k] > fmax) fmax = mags[k];
        }
        // log-scale collapsed to 192 bars
        var bars = 192;
        var output = new float[bars];
        var minHz  = 30f;
        var maxHz  = sampleRate / 2f;
        for (var b = 0; b < bars; b++)
        {
            var t  = b / (float)(bars - 1);
            var hz = minHz * MathF.Pow(maxHz / minHz, t);
            var k  = (int)Math.Clamp(hz / sampleRate * win, 0, bins - 1);
            var v  = mags[k] / fmax;
            output[b] = MathF.Pow(v, 0.6f);
            if (output[b] > _livePeak[b % _livePeak.Length])
                _livePeak[b % _livePeak.Length] = output[b];
            else
                _livePeak[b % _livePeak.Length] *= 0.92f;
        }
        LiveSpectrum     = output;
        LiveSpectrumPeak = (float[])_livePeak.AsSpan(0, bars).ToArray();

        // Tuner
        if (_tuner is not null)
        {
            var tunerWin = Math.Min(2048, mono.Length);
            var r = _tuner.Detect(mono.Slice(0, tunerWin));
            if (r is { } reading)
            {
                TunerHasReading = true;
                TunerNote       = $"{reading.NoteName}{reading.Octave}";
                TunerCents      = reading.CentsOff;
                TunerFreq       = reading.FrequencyHz;
            }
            else
            {
                TunerHasReading = false;
            }
        }
    }

    // ------------------------------------------------------------------ Offline analysis

    [RelayCommand]
    private async Task RunAnalysis()
    {
        if (_decoded is null || _decodedMono is null)
        {
            AppendLog("Load a file first.");
            return;
        }
        if (AnalysisRunning) return;
        AnalysisRunning = true;
        StatusLine = "Analysing…";
        _analysisCts?.Cancel();
        _analysisCts = new CancellationTokenSource();
        var token = _analysisCts.Token;

        var mono = _decodedMono;
        var sr   = _decoded.SampleRate;

        try
        {
            var spectrogramTask = Task.Run(() =>
            {
                var b = new JuceSpectrogramBuilder(2048, 512);
                return b.Build(mono, sr, decibelScale: true);
            }, token);

            var beatTask = Task.Run(() =>
            {
                var bd = new JuceBeatDetector(sr);
                return bd.Analyze(mono);
            }, token);

            var chordTask = Task.Run(() =>
            {
                var rows = new List<ChordRow>();
                var c    = new JuceChromagram(sr);
                var det  = new JuceChordDetector();
                var win  = 4096;
                var hop  = sr;
                for (int p = 0; p + win < mono.Length; p += hop)
                {
                    var cv = c.Compute(new ReadOnlySpan<float>(mono, p, win));
                    var r  = det.Detect(cv);
                    if (r is { } reading)
                        rows.Add(new ChordRow(TimeSpan.FromSeconds(p / (double)sr).ToString(@"mm\:ss"),
                            reading.Name, reading.Confidence));
                }
                return rows;
            }, token);

            await Task.WhenAll(spectrogramTask, beatTask, chordTask);

            var spec  = spectrogramTask.Result;
            var beats = beatTask.Result;
            var chord = chordTask.Result;

            Spectrogram = spec.Frames;
            Bpm         = beats.Bpm;
            BeatCount   = beats.Beats.Count;
            BeatTimeline.Clear();
            for (var i = 0; i < beats.Beats.Count; i++)
                BeatTimeline.Add(new BeatRow(i + 1, beats.Beats[i].ToString(@"mm\:ss\.ff")));
            ChordTimeline.Clear();
            foreach (var row in chord) ChordTimeline.Add(row);

            StatusLine = $"Analysis done — BPM={Bpm:F1}, beats={BeatCount}, chords={ChordTimeline.Count}.";
            AppendLog(StatusLine);
        }
        catch (OperationCanceledException) { StatusLine = "Analysis cancelled."; }
        catch (Exception ex) { AppendLog($"Analysis failed: {ex.Message}"); StatusLine = "Analysis failed."; }
        finally { AnalysisRunning = false; }
    }

    [RelayCommand]
    private async Task ExportSliceWavAsync()
    {
        if (_decoded is null) { AppendLog("Load a file first."); return; }
        try
        {
            await Task.Run(() =>
            {
                var sr     = _decoded.SampleRate;
                var ch     = _decoded.Channels;
                var startF = (int)(Math.Clamp(LoopStart, 0, LengthSeconds) * sr);
                var endF   = (int)(Math.Clamp(LoopEnd > LoopStart ? LoopEnd : LoopStart + 1, 0, LengthSeconds) * sr);
                var samples= (endF - startF) * ch;
                if (samples <= 0) return;
                var slice  = new float[samples];
                Array.Copy(_decoded.Interleaved, startF * ch, slice, 0, samples);
                var dst    = Path.Combine(Path.GetTempPath(), $"jucesharp_slice_{DateTime.UtcNow:HHmmss}.wav");
                WavIo.WriteFloat32(dst, new AudioBuffer(slice, sr, ch));
                AppendLog($"Slice exported → {dst}");
            });
        }
        catch (Exception ex) { AppendLog($"Slice export failed: {ex.Message}"); }
    }

    private void BuildWaveformPeaks()
    {
        if (_decoded is null) { WaveformMins = null; WaveformMaxs = null; return; }
        var peaks = JuceWaveformPeaks.Build(_decoded.Interleaved, _decoded.Channels, _decoded.SampleRate, 20);
        var mins  = new float[peaks.Length];
        var maxs  = new float[peaks.Length];
        for (var i = 0; i < peaks.Length; i++) { mins[i] = peaks[i].Min; maxs[i] = peaks[i].Max; }
        WaveformMins = mins;
        WaveformMaxs = maxs;
    }

    // ------------------------------------------------------------------ VST3 plugin

    /// <summary>
    /// Loads a VST3 and attaches it. Async because a COLD VST3 scan measures 25 s and more — the
    /// synchronous LoadPlugin holds the engine handle on the calling thread for all of it, which
    /// from a click handler is a window that stops repainting.
    /// </summary>
    public async Task LoadPluginFromPathAsync(string path)
    {
        if (_engine is null) { AppendLog("Engine is not running."); return; }
        try
        {
            UnloadPlugin();
            AppendLog($"Loading {Path.GetFileName(path)} — a cold VST3 scan can take half a minute.");
            _plugin = await _engine.LoadPluginAsync(path).ConfigureAwait(true);
            PluginPath   = path;
            PluginStatus = $"Loaded {Path.GetFileName(path)} — {_plugin.ParameterCount} params, latency {_plugin.LatencySamples} smp";
            AppendLog(PluginStatus);

            PluginParams.Clear();
            foreach (var p in _plugin.EnumerateParameters())
            {
                PluginParams.Add(new PluginParam(this, p.Index, p.Name, _plugin.GetParameter(p.Index)));
            }
            if (PluginAsInstrument) _plugin.AttachToInstrument(); else _plugin.AttachToInput();
        }
        catch (Exception ex) { AppendLog($"LoadPlugin failed: {ex.Message}"); }
    }

    [RelayCommand]
    private void UnloadPlugin()
    {
        try
        {
            _plugin?.Dispose();
            _plugin = null;
            PluginParams.Clear();
            PluginStatus = "(no plugin loaded)";
        }
        catch (Exception ex) { AppendLog($"UnloadPlugin: {ex.Message}"); }
    }

    [RelayCommand] private void ShowPluginEditor() { try { _plugin?.ShowEditor(); } catch (Exception e) { AppendLog($"ShowEditor: {e.Message}"); } }
    [RelayCommand] private void HidePluginEditor() { try { _plugin?.HideEditor(); } catch (Exception e) { AppendLog($"HideEditor: {e.Message}"); } }

    partial void OnPluginBypassChanged(bool value)
    {
        try { if (_plugin is not null) _plugin.Bypass = value; }
        catch (Exception e) { AppendLog($"Bypass: {e.Message}"); }
    }

    partial void OnPluginAsInstrumentChanged(bool value)
    {
        if (_plugin is null) return;
        try
        {
            if (value) { _plugin.DetachFromInput();      _plugin.AttachToInstrument(); }
            else       { _plugin.DetachFromInstrument(); _plugin.AttachToInput();      }
        }
        catch (Exception e) { AppendLog($"Attach: {e.Message}"); }
    }

    [RelayCommand]
    private void SendMidiNote(string noteParam)
    {
        // noteParam is a MIDI note number (e.g. "60"). Fires note-on + note-off on channel 1.
        if (!int.TryParse(noteParam, out var note)) return;
        try
        {
            _engine?.SendMidi(0x90, (byte)note, 96);
            Task.Delay(180).ContinueWith(_ => { try { _engine?.SendMidi(0x80, (byte)note, 0); } catch { } });
        }
        catch (Exception e) { AppendLog($"MIDI: {e.Message}"); }
    }

    internal void SetPluginParameter(int index, float value)
    {
        try { _plugin?.SetParameter(index, value); }
        catch (Exception e) { AppendLog($"SetParameter[{index}]: {e.Message}"); }
    }

    // ------------------------------------------------------------------ Util / housekeeping

    public void Dispose()
    {
        _positionTimer.Stop();
        _liveTimer.Stop();
        _meterTimer.Stop();
        DisposeToneRig();
        _analysisCts?.Cancel();
        UnloadPlugin();
        DisposeEngine();
    }

    private void DisposeEngine()
    {
        try { _source?.Dispose(); } catch { }
        _source = null;
        try { _engine?.Dispose(); } catch { }
        _engine = null;
        OnEngineReplacedForRig(); // the chain died with the engine
    }

    /// <summary>Log sink for the view's own fire-and-forget handlers; see MainWindow.Forget.</summary>
    public void AppendUserLog(string line) => AppendLog(line);

    private void AppendLog(string line)
    {
        var stamped = $"[{DateTime.Now:HH:mm:ss}] {line}";
        if (Dispatcher.UIThread.CheckAccess())
            DoAppend(stamped);
        else
            Dispatcher.UIThread.Post(() => DoAppend(stamped));

        void DoAppend(string s)
        {
            Log.Insert(0, s);
            while (Log.Count > 200) Log.RemoveAt(Log.Count - 1);
        }
    }

    private static IReadOnlyList<DriverItem> EnumerateDrivers()
    {
        // Built from the same canonical host list the JuceDeviceCatalog uses
        // so display names stay in sync with the library helper.
        var list = new List<DriverItem>
        {
            new("Auto (engine default)", AudioDriver.Default, ""),
        };
        foreach (var name in new[]
        {
            "Windows Audio",
            "Windows Audio (Exclusive Mode)",
            "Windows Audio (Low Latency Mode)",
            "DirectSound",
            "ASIO",
            "CoreAudio",
            "ALSA",
            "JACK",
        })
        {
            list.Add(new DriverItem(JuceDeviceCatalog.DisplayName(name), JuceDeviceCatalog.DriverFor(name), name));
        }
        return list;
    }

    private static IReadOnlyList<ChannelModeItem> BuildChannelModes()
    {
        return new List<ChannelModeItem>
        {
            new("Auto / Default",       0u),
            new("Mono (channel 1)",     ChannelMask.Mono),
            new("Stereo (1+2)",         ChannelMask.Stereo),
            new("Quad (1+2+3+4)",       ChannelMask.First(4)),
            new("Surround 5.1 (1..6)",  ChannelMask.First(6)),
            new("Surround 7.1 (1..8)",  ChannelMask.First(8)),
        };
    }

    [RelayCommand]
    private async Task ScanVst3()
    {
        try
        {
            StatusLine = "Scanning VST3 plugins…";
            var found = await Task.Run(() => JuceVst3Scanner.Scan(maxDepth: 4));
            Vst3Plugins.Clear();
            foreach (var e in found.OrderBy(e => e.Name, StringComparer.OrdinalIgnoreCase))
                Vst3Plugins.Add(new Vst3Item(e));
            StatusLine = $"VST3 scan: {Vst3Plugins.Count} plugin(s) found.";
            AppendLog(StatusLine);
        }
        catch (Exception ex)
        {
            AppendLog($"VST3 scan failed: {ex.Message}");
            StatusLine = "VST3 scan failed.";
        }
    }

    partial void OnOutputChannelModeChanged(ChannelModeItem? value) => ReapplyChannels();
    partial void OnInputChannelModeChanged (ChannelModeItem? value) => ReapplyChannels();

    private void ReapplyChannels()
    {
        if (!_initialised || _engine is null) return;
        // In-place channel layout change — much faster than tearing the
        // engine down for a Mono/Stereo toggle.
        SwitchHostInPlace();
    }

    private static float[] HannWindow(int n)
    {
        var w = new float[n];
        for (var i = 0; i < n; i++)
            w[i] = 0.5f * (1f - MathF.Cos(2f * MathF.PI * i / (n - 1)));
        return w;
    }

    private static string SafeNativeVersion()
    {
        try { return JuceAudioEngine.NativeVersion; } catch { return "(unavailable)"; }
    }

    // ------------------------------------------------------------------ Lightweight records

    public sealed record ChordRow(string Time, string Chord, float Confidence);
    public sealed record BeatRow(int Index, string Time);
    public sealed record DriverItem(string Display, AudioDriver Driver, string TypeName);
    public sealed record InputChannelItem(int Channel, string Display);
    public sealed record ChannelModeItem(string Display, uint Mask);

    public sealed class Vst3Item
    {
        public Vst3Item(JuceVst3Scanner.Vst3Entry entry) { Entry = entry; }
        public JuceVst3Scanner.Vst3Entry Entry { get; }
        public string Display => Entry.Display;
        public string Path    => Entry.Path;
    }

    public sealed class DeviceItem
    {
        public DeviceItem(JuceAudioDevice device) { Device = device; }
        public JuceAudioDevice Device { get; }
        public string Display => $"{Device.Name}" +
            (string.IsNullOrWhiteSpace(Device.TypeName) ? "" : $"  [{Device.TypeName}]") +
            (Device.IsDefault ? "  (default)" : "");
    }

    public sealed partial class PluginParam : ObservableObject
    {
        private readonly MainWindowViewModel _vm;
        public int Index { get; }
        public string Name { get; }

        [ObservableProperty] private float _value;

        public PluginParam(MainWindowViewModel vm, int index, string name, float value)
        {
            _vm    = vm;
            Index  = index;
            Name   = string.IsNullOrEmpty(name) ? $"param {index}" : name;
            _value = value;
        }

        partial void OnValueChanged(float value) => _vm.SetPluginParameter(Index, value);
    }
}
