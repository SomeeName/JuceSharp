# Testing without the native DLL

Every handle-owning type in this package is `sealed` with an `internal`
constructor. That is deliberate — the only legitimate way to get one is from a
live native handle, and nothing should be able to fabricate one — but it left a
consumer with no way to unit-test **its own** transport logic, device-change
handling or pedalboard code without `juce_sharp.dll` and a sound card.

Since **0.9** each of those types implements an interface covering its public
instance surface:

| Concrete | Contract |
|---|---|
| `JuceAudioEngine` | `IJuceAudioEngine` |
| `JuceAudioSource` | `IJuceAudioSource` |
| `JuceAudioPlugin` | `IJuceAudioPlugin` |
| `JuceMidiInput` | `IJuceMidiInput` |
| `JuceMidiOutput` | `IJuceMidiOutput` (ABI v35 — it was unfakeable before) |
| `JuceDspChain` | `IJuceDspChain` |
| `JuceDspNode` | `IJuceDspNode` |

Nothing else changed: the interfaces are additive, and the factory members
(`OpenFile`, `LoadPlugin`, `GetDspChain`, `OpenMidiInput`, `OpenMidiOutput`, and the async
equivalents) answer in **interface** terms on the interface only — as explicit
implementations — so code holding a `JuceAudioEngine` still gets a
`JuceAudioSource` back exactly as before. That indirection is what lets a fake
engine hand back fake children.

## Two levels of test, and they answer different questions

| | Fakes over the interfaces | The **Null device** |
|---|---|---|
| Needs `juce_sharp.dll` | no | yes |
| Needs a sound card | no | no |
| Answers | "does MY code drive the engine correctly?" | "does the ENGINE do what I asked?" |
| Speed | microseconds | a real engine open |

Use fakes for logic. Use the Null device for anything where the answer depends on
the native side actually running.

## Faking the engine

Write the double by hand; it is one file, and a hand-written double is easier to
read in a failure than a mocking framework's expectation script.

```csharp
public sealed class FakeEngine : IJuceAudioEngine
{
    public List<string> Calls { get; } = new();
    public FakeSource NextSource { get; set; } = new();

    public IJuceAudioSource OpenFile(string path)
    {
        Calls.Add(nameof(OpenFile));
        return NextSource;
    }

    public void Start() => Calls.Add(nameof(Start));

    // …the rest: record the call and return default.
}
```

…and the consumer under test names no concrete type:

```csharp
public sealed class PracticeLoopPlayer(IJuceAudioEngine engine) : IDisposable
{
    private IJuceAudioSource? _source;

    public void Start(string path, TimeSpan from)
    {
        engine.Start();
        _source = engine.OpenFile(path);
        _source.Seek(from);
        _source.Play();
    }

    public void Dispose() => _source?.Dispose();
}
```

```csharp
[Fact]
public void ItSeeksBeforeItPlays()
{
    var engine = new FakeEngine();
    using var player = new PracticeLoopPlayer(engine);

    player.Start("song.ogg", TimeSpan.FromSeconds(30));

    Assert.Equal(TimeSpan.FromSeconds(30), engine.NextSource.Position);
    Assert.True(engine.NextSource.Playing);
}
```

This repository's own worked example is
`tests/JuceSharp.Tests/InterfaceSeamTests.cs`
(`AConsumerRunsOnAFakeEngine_WithoutLoadingTheNativeLibrary`), with
`FakeJuceAudioEngine` / `FakeJuceAudioSource` beside it. It asserts the thing
that actually matters: **no native module arrived in the process while the test
ran.** It does that as a before/after delta on `Process.Modules`, not as an
absolute — another test in the same host may already have loaded the DLL, and
what is being claimed is that *this* test did not need it.

### Fake the async members synchronously

`ReconfigureAsync` and friends exist because the real calls take seconds. A fake
has nothing to wait for, so return `Task.CompletedTask` / `Task.FromResult(…)`
and keep your test free of a worker thread, a device and a clock.

### The interfaces cannot silently rot

`InterfaceSeamTests` walks every pair by reflection in both directions, so a
member added to a concrete type and forgotten on its interface **fails the
build's test run** rather than quietly un-covering the seam. If you add a member
to a wrapper type in this repo, add it to the interface in the same change.

## When you need the real engine: the Null device

Some questions are about the native side, and a fake cannot answer them. The
native `Null` audio device type accepts any requested configuration and paces the
audio callback from its own thread with silent input — a real engine, real
callbacks, no hardware.

It is guarded by a process environment variable read at engine init:

```csharp
Environment.SetEnvironmentVariable("DCA_ENABLE_NULL_DEVICE", "1");

using var engine = new JuceAudioEngine(new JuceEngineConfig
{
    OutputTypeName = "Null",
    SampleRate     = 48000,
    BufferFrames   = 480,     // 10 ms, comfortably above timer granularity
    InputChannels  = 0,
});
```

Two rules that are learned the hard way:

* **The variable is process-wide**, so every test that sets it must be
  serialized against every test that asserts it is *off* — in this repo that is
  the `NullDevice` xunit collection, with the variable set in the class
  constructor and cleared on dispose.
* **With the guard off the null type must be unreachable.** That is the invariant
  that lets it ship in the release DLL, and it is asserted
  (`NullDeviceTests.GuardOff_NullType_NeverAppearsInDeviceLists`).

The offline rig render (`JuceDspChain.ProcessOffline`) needs no device at all,
and in fact refuses to run while one is open — for a tone test, call
`engine.Stop()` first or use a device-less engine.

## See also

* [JuceAudioEngine](../api/juce-audio-engine.md) — the async members and the
  interface list
* [JuceDspChain](../api/juce-dsp-chain.md) — the rig surface, including the
  offline render
* [Error Handling Patterns](error-handling.md)
