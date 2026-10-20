# Table of contents

* [Welcome](README.md)
* [Features & Roadmap](features.md)

## Getting Started

* [Installation](getting-started/installation.md)
* [First Sound](getting-started/first-sound.md)
* [Quick Start](getting-started/quick-start.md)
* [Building the Native DLL](getting-started/building-native.md)
* [Examples](guides/examples.md)

## Guides

* [Audio Playback](guides/playback.md)
* [Devices, Hosts & Fallback](guides/devices.md)
* [Live Input & Monitoring](guides/input.md)
* [Live Seats (Multiple Players)](guides/seats.md)
* [The DSP Tone Rig](guides/tone-rig.md)
  * [Dynamics gear](guides/gear-dynamics.md)
  * [Drive gear](guides/gear-drive.md)
  * [Amp & cab gear](guides/gear-ampcab.md)
  * [Filter gear](guides/gear-filter.md)
  * [Modulation gear](guides/gear-modulation.md)
  * [Pitch gear](guides/gear-pitch.md)
  * [Delay gear](guides/gear-delay.md)
  * [Reverb gear](guides/gear-reverb.md)
  * [Utility & stereo gear](guides/gear-utility.md)
* [Drop Tune](guides/drop-tune.md)
* [Master EQ Bank](guides/eq.md)
* [Clicks & the Render Clock](guides/clicks.md)
* [Hosting VST3 Plugins](guides/vst3-hosting.md)
* [MIDI Instruments](guides/midi-instruments.md)
* [Mirror Output (OBS / Discord)](guides/mirror-output.md)
* [Waveform Generation](guides/waveform.md)
* [Offline Processing](guides/offline.md)
* [Audio I/O](guides/audioio.md)
* [Audio Analysis](guides/dsp-analysis.md)
* [Engine Health & Dropouts](guides/health.md)
* [Logging & Diagnostics](guides/logging.md)
* [Error Handling Patterns](guides/error-handling.md)
* [Failure Modes & Runtime Health](guides/failure-modes.md)
* [Threading](guides/threading.md)
* [Real-time Constraints for Consumers](guides/realtime-constraints.md)
* [Testing without the native DLL](guides/testing-without-native.md)

## Stem Separation

* [Choosing a Backend](guides/stem-backends.md)
* [Python Sidecar Guide](StemSeparation.md)
* [JuceSharp.StemSeparation API](api/stem-separation.md)
* [JuceSharp.StemSeparation.Onnx API](api/stem-separation-onnx.md)
* [JuceStemSeparation (native)](api/juce-stem-separation.md)

## API Reference

* [JuceAudioEngine](api/juce-audio-engine.md)
* [JuceAudioSource](api/juce-audio-source.md)
* [JuceWemDecoder (Wwise .wem)](api/juce-wem-decoder.md)
* [JuceAudioPlugin](api/juce-audio-plugin.md)
* [JuceLoopRegion](api/juce-loop-region.md)
* [JuceMixer](api/juce-mixer.md)
* [JuceEngineConfig](api/juce-engine-config.md)
* [JuceAudioDevice](api/juce-audio-device.md)
* [JuceDeviceCatalog & JuceAudioHost](api/juce-device-catalog.md)
* [JuceEngineFactory](api/juce-engine-factory.md)
* [JuceVst3Scanner](api/juce-vst3-scanner.md)
* [JuceMidiInput](api/juce-midi-input.md)
* [JuceMidiOutput](api/juce-midi-output.md)
* [JuceParameterInfo](api/juce-parameter-info.md)
* [ChannelMask](api/channel-mask.md)
* [XrunStats](api/xrun-stats.md)
* [JuceDspChain (tone rig)](api/juce-dsp-chain.md)
* [JuceDspMeters](api/juce-dsp-meters.md)
* [WavWriter](api/wav-writer.md)
* [JuceAudioException](api/juce-audio-exception.md)
* [JuceLog](api/juce-log.md)

## DSP Toolkit

* [Overview](dsp.md)
* [JuceFft](dsp/juce-fft.md)
* [JuceChromaticTuner](dsp/juce-tuner.md)
* [JuceTunerStream](dsp/juce-tuner-stream.md)
* [JuceBeatDetector](dsp/juce-beat-detector.md)
* [JuceChromagram](dsp/juce-chromagram.md)
* [JuceChordDetector](dsp/juce-chord-detector.md)
* [Onset, Pitch, Notes & Guitar Events](dsp/onset-and-pitch.md)
* [Spectrogram & Waveform Peaks](dsp/spectrogram-and-peaks.md)
* [Detection v8](dsp/detection-v8.md)

## Native Layer

* [Architecture](native/architecture.md)
* [C ABI Reference](native/c-abi.md)
* [Threading Model](native/threading.md)
* [Building from Source](native/building.md)

## Project

* [Contributing](contributing.md)
* [ABI & Versioning Policy](abi-policy.md)
* [Changelog](changelog.md)
