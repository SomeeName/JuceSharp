# Changelog

The version lives in `Directory.Build.props` at the repository root (one
`<Version>` for every packable project) and a second time in
`src/JuceSharp.Native/CMakeLists.txt`, which feeds `dca_version_string()`;
`PackagingLintTests` asserts the two agree. Every entry names the ABI version
it requires, because the managed wrapper demands exact equality and refuses a
stale `juce_sharp.dll`.

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
