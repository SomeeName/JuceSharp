# Quick Start

> ℹ️ **Two things first.**
> 1. Build the native DLL — nothing on this page runs without it, and the
>    failure is a `DllNotFoundException` (or a `JuceAbiMismatchException` from a
>    stale one). See [Installation](installation.md).
> 2. These samples let the engine pick the output device. That is fine for a
>    first run, but an empty `OutputDeviceId` is also the usual explanation for
>    "it opened and there is no sound" — read
>    [Devices](../guides/devices.md) before you conclude the audio is broken.

## Playing a file

```csharp
using JuceSharp;

using var engine = new JuceAudioEngine(new JuceEngineConfig
{
    PreferredDriver = AudioDriver.Wasapi,
    SampleRate      = 48_000,
    BufferFrames    = 256,
});
engine.Start();

using var song = engine.OpenFile(@"C:\Music\track.wav");
song.Volume     = 1.0f;
song.TempoRatio = 0.9f;       // 10 % slower (Rubber Band time-stretch)
song.PitchSemitones = -2f;    // 2 semitones down
song.Play();

Console.WriteLine($"Length: {song.Length}");
await Task.Delay(song.Length);
```

## Hosting a VST3 effect on live input

```csharp
using var engine = new JuceAudioEngine();
engine.Start();

using var amp = engine.LoadPlugin(@"C:\Program Files\Common Files\VST3\Helix Native.vst3");
amp.AttachToInput();
amp.ShowEditor();   // pops up the plugin's own UI on a JUCE-managed window
```

`amp` is a [`JuceAudioPlugin`](../api/juce-audio-plugin.md) — disposing it
detaches from the graph and tears down the editor on the correct thread.

## Sending MIDI to an instrument plugin

```csharp
using var engine = new JuceAudioEngine();
engine.Start();

using var guitar = engine.LoadPlugin(@"C:\VST3\Ample Guitar SC.vst3");
guitar.AttachToInstrument();

engine.SendMidi(0x90, 60, 100);   // note-on  C4 vel=100
await Task.Delay(500);
engine.SendMidi(0x80, 60,   0);   // note-off C4
```

## Listening to engine logs

By default JuceSharp writes `[native] …` lines to the console if no
`ILogger` is supplied. Pipe to your own logging stack with:

```csharp
using Microsoft.Extensions.Logging;

using var loggerFactory = LoggerFactory.Create(b => b.AddConsole());
var log = loggerFactory.CreateLogger<JuceAudioEngine>();

using var engine = new JuceAudioEngine(config: null, logger: log);
```

See [Logging & Diagnostics](../guides/logging.md) for the full sink list.
