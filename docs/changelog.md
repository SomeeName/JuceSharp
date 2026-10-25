# Changelog

The version lives in `Directory.Build.props` at the repository root (one
`<Version>` for every packable project) and a second time in
`src/JuceSharp.Native/CMakeLists.txt`, which feeds `dca_version_string()`;
`PackagingLintTests` asserts the two agree. Every entry names the ABI version
it requires, because the managed wrapper demands exact equality and refuses a
stale `juce_sharp.dll`.

## 1.3.0 (2026-10-25)

**ABI v49.**

- **Device channel names** — `dca_engine_get_channel_names`
  (`JuceAudioEngine.GetChannelNames`, also on `IJuceAudioEngine`) returns the
  input or output channel names the OPEN device reports, entry i naming
  channel i. A device that is not the open one on that side gets no names.
- **A mono output hears both channels.** A device with one active output now
  runs a two-wide master bus and writes (L + R) / 2 to its channel,
  allocation-free; before, anything panned right never reached a mono out.

## 1.2.0 (2026-10-22)

**ABI v48.**

- **Static MSVC runtime** — `juce_sharp.dll` and `juce_vst_host.exe` link the
  C/C++ runtime statically (`JUCESHARP_STATIC_CRT`, default ON), so a release
  zip loads on a Windows without the VC++ 2015-2022 x64 redistributable. Both
  files import only Windows system DLLs.
- **Untrusted inputs bounded.** A NAM capture declaring an absurd sample rate
  dry-bypasses (status `rateMismatch`) instead of terminating the host; `.wem`
  input is capped at 256 MiB, read once, and a `.wem` song is never streamed
  (it is refused when it does not fit the preload budget); a plugin state over
  2 GiB is refused by `dca_plugin_save_state`; stem names from a model yaml must
  be `[A-Za-z0-9_-]+` and stay inside `--out`.
- **`dca_engine_chart_source_set_notes_for_player2`** — the chart note upload
  with the byte length of the notes buffer; `JuceAudioEngine.SetChartNotes`
  uses it.
- **Python sidecar supply chain pinned end to end** (`JuceSharp.StemSeparation`).
  pip is bootstrapped from a SHA-256-pinned wheel instead of `get-pip.py`
  (`GetPipUrl` / `GetPipSha256` are replaced by `PipWheelUrl` /
  `PipWheelSha256`, both required, https only). Packages install from a
  generated hash lock with `--require-hashes --no-deps --only-binary :all:`;
  `EnsurePackagesAsync` refuses specs outside `PythonPackagePins` and any
  non-Windows platform. Demucs weights are fetched by `StemSeparator` from a
  pinned HuggingFace revision with a full SHA-256 per file
  (`StemSeparationOptions.DemucsWeightsDirectory`); unpinned model names are
  refused and the script never lets demucs download. The interpreter is never
  taken from PATH, and the bootstrap gate is a lock file in the root instead of
  a machine-wide named semaphore. **Breaking:** no system-Python fallback on
  Linux/macOS.
- **Safe checkpoint loading** (`JuceSharp.StemSeparation`). torch is pinned to
  >= 2.6 and checkpoints load with `weights_only=True` first; the full pickle
  fallback (`CkptToOnnxConverter.AllowUnsafePickleFallback`) runs only when the
  caller opts in. `.safetensors` never unpickles.

## 1.1.0 (2026-10-20)

**ABI v47** (unchanged).

- **CI on GitHub Actions** — the build and test workflow runs on the public
  repository (SHA-pinned actions, read-only token, a virtual audio device for
  the device tests); the private-submodule token is optional.
- **Sponsor button** — `FUNDING.yml` (Ko-fi).

## 1.0.0 (2026-10-20): initial public release

**ABI v47.**

- **Native engine** — a JUCE 9 audio engine behind a versioned C ABI
  (`dotcharter_audio.h`): WASAPI / DirectSound / ASIO device I/O, device
  catalogue, host fallback and hot reconfiguration, xrun and health counters.
- **Playback** — file decoding and transport with Rubber Band time-stretch and
  pitch-shift, loop regions, a mixer, offline render to WAV.
- **Live input** — per-seat input routing and monitoring for several players,
  drop tune, EQ bank, meters, mirror output and an external-process insert.
- **Tone rig** — an in-house, reorderable, allocation-free DSP rig (drive, amp
  and cab, filter, modulation, pitch, delay, reverb, dynamics, utility) with
  edit scopes, sidechain and delay compensation; NAM model playback.
- **Plug-in hosting** — VST3 on the input bus and as a MIDI instrument,
  in-process or sandboxed in a child process.
- **MIDI** — input and output devices, MIDI instruments.
- **Analysis** — FFT, tuner, onset/pitch and beat detection, chromagram and
  chord detection, spectrogram and peaks (`JuceSharp.Dsp`).
- **Stem separation** — ONNX (HTDemucs, MDX-Net, BS-RoFormer; DirectML or CPU)
  and demucs.cpp backends with verified model downloads.
- **Safety nets** — a real-time safety gate for the audio callback and a
  managed test suite that runs without the native DLL.
