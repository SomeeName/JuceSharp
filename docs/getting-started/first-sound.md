# First Sound

The shortest true path from an empty project to audio coming out of a device.
Every step here is required; none of them can be skipped on a fresh machine.

## 0. There is no package on nuget.org

The repository packs (`dotnet pack` produces a `JuceSharp` nupkg in CI), but
nothing is pushed to a public feed. **`dotnet add package JuceSharp` cannot
resolve.** Clone the repository and use a `ProjectReference`:

```xml
<ItemGroup>
  <ProjectReference Include="..\JuceSharp\src\JuceSharp\JuceSharp.csproj" />
</ItemGroup>
```

There are six packable projects, and they are separate: `JuceSharp` (the
engine), `JuceSharp.AudioIo` (decode / WAV), `JuceSharp.Dsp` (analysis DSP),
`JuceSharp.StemSeparation`, `JuceSharp.StemSeparation.Onnx`,
`JuceSharp.Diagnostics`. `JuceSharp` references none of the others — add a
second `ProjectReference` for each one you use.

Only **win-x64** has ever been built or tested. The CMake project has
Linux/macOS branches; treat them as source-available, not supported.

## 1. Build the native DLL — this is step one, not troubleshooting

`juce_sharp.dll` is a **build artifact and is not tracked in git**, so a fresh
clone has no engine in it. A managed-only build compiles clean and throws
`DllNotFoundException` at the first call. Build it with CMake before you run
anything: [Building from Source](../native/building.md) (and
[Building the Native DLL](building-native.md) for the Windows toolchain
details) — the commands are there and are not repeated here.

CMake's `POST_BUILD` step deploys two files:

| File | Deployed to | After `dotnet publish -r win-x64` |
|---|---|---|
| `juce_sharp.dll` | `src/JuceSharp/runtimes/win-x64/native/` | the **root** of the publish folder, next to the exe |
| `juce_vst_host.exe` | the same folder | the same place (only needed for sandboxed VST3 hosting) |

`JuceSharp.csproj` copies the DLL from `runtimes/win-x64/native/` into your
output directory. A RID-specific publish flattens runtime assets, which is why
both artifacts end up beside the exe rather than under `runtimes/`.

## 2. Check the DLL and the ABI before anything else

The wrapper and the DLL are matched by **equality** on `DCA_ABI_VERSION`:
`JuceAudioEngine`'s constructor calls `dca_abi_version()` and throws
`JuceAbiMismatchException` (a subclass of `InvalidOperationException`, carrying
`ExpectedAbiVersion` and `NativeAbiVersion`) when the numbers differ. Because
the check is equality, a stale DLL fails *here* rather than at whichever call
would have hit a missing export.

**The fix is always to rebuild the DLL — never to widen the check.** The usual
cause is a managed update without a native rebuild, or an older copy of
`juce_sharp.dll` earlier on the search path.

```csharp
try
{
    // Throws DllNotFoundException when juce_sharp.dll is not on the search path,
    // and it is the cheapest call that proves the native side loaded at all.
    Console.WriteLine($"native: {JuceAudioEngine.NativeVersion}");
}
catch (DllNotFoundException)
{
    Console.Error.WriteLine("juce_sharp.dll not found next to the executable — build the native target.");
    return 3;
}
catch (JuceAbiMismatchException ex)
{
    Console.Error.WriteLine($"{ex.Message} (wrapper {ex.ExpectedAbiVersion}, native {ex.NativeAbiVersion})");
    return 3;
}
```

`NativeVersion` is a log string (`JuceSharp.Native 1.1.0 (ABI 48, JUCE 9.0.3)`
in shape). Do not parse it for compatibility — the constructor already decided.

## 3. Enumerate devices without opening one

`JuceDeviceCatalog.Enumerate()` returns every host JUCE recognises on this
platform together with its devices. It opens **no** device and constructs no
engine, so it is safe to call from a UI thread while another engine is running,
and it is fast even when an ASIO driver cannot reach its hardware.

```csharp
foreach (var host in JuceDeviceCatalog.Enumerate())
{
    Console.WriteLine($"[{JuceDeviceCatalog.DisplayName(host.TypeName)}] " +
                      $"{host.OutputDevices.Count} out / {host.InputDevices.Count} in");
    foreach (var d in host.OutputDevices)
        Console.WriteLine($"    {d.Name} — {d.MaxOutputChannels} ch @ {d.DefaultSampleRate} Hz");
}
```

A host with zero output devices is normal (an installed ASIO driver with
nothing plugged in). `JuceAudioHost.HasOutputs` says so directly.

## 4. Open the engine

Pick a device by **`OutputTypeName` + `OutputDeviceId`** — the `TypeName` from
the host and the `Id` from the device row. Leaving `OutputDeviceId` empty makes
JUCE take the host's *first* device, which is frequently an offline ASIO
interface, so an empty id is how you get either a hard open failure or a
successful open into silence.

`JuceEngineFactory.OpenWithFallback` handles that: if the requested device
fails, it walks the rest of the **same host** and returns the config that
actually worked plus the trail of attempts.

> `new JuceAudioEngine(config)` **opens and starts the device in the
> constructor** — the engine is running from the moment it returns. `Start()`
> is only for a restart after `Stop()`.

```csharp
var open = JuceEngineFactory.OpenWithFallback(new JuceEngineConfig
{
    OutputTypeName = "Windows Audio",   // or host.TypeName from the catalogue
    OutputDeviceId = deviceId,          // JuceAudioDevice.Id — do not leave empty
    SampleRate     = 48_000,
    BufferFrames   = 480,
});

if (open.Engine is null)
{
    foreach (var attempt in open.Attempts) Console.Error.WriteLine(attempt);
    return 4;   // nothing on this host could be opened
}

using var engine = open.Engine;
Console.WriteLine($"{engine.SampleRate} Hz, {engine.BufferFrames} frames, " +
                  $"{engine.OutputLatencySamples} samples of output latency");
```

The **master limiter is OFF at engine start** (ABI v35) and the host opts in
with `SetMasterLimiter`. See
[Real-time constraints](../guides/realtime-constraints.md#the-master-limiter-is-off-and-costs-1-ms-when-on).

### The ASIO control panel

`engine.TryShowAsioControlPanel(parentWindow, out var error)` opens the vendor's
own panel (buffer size, clock source). It is a **modal** dialog with no upper
bound on how long it stays up, so from a UI thread use the async twin,
`ShowAsioControlPanelAsync`, which runs it on the engine's own worker thread.
**Close the panel before disposing the engine** — nothing can cancel a modal
vendor dialog, and a dispose that lands while it is open blocks and then leaks
the native engine rather than freeing it under the live call.

### No sound card: the Null device

The engine registers a silent, timer-paced `"Null"` `AudioIODeviceType` **only**
when `DCA_ENABLE_NULL_DEVICE=1`. The variable is read once at engine start, so
setting it in-process before the first engine call is enough. This is how CI and
headless tests reach every code path except the driver.

```csharp
Environment.SetEnvironmentVariable("DCA_ENABLE_NULL_DEVICE", "1");
var headless = new JuceEngineConfig { OutputTypeName = "Null", SampleRate = 48_000, BufferFrames = 480 };
```

Nothing is audible and `LateCallbacks` is routinely non-zero on it — it is a
fake device, not a driver. See
[Testing without the native DLL](../guides/testing-without-native.md).

## 5. Play a file

```csharp
using var song = engine.OpenFile(@"C:\Music\track.wav");   // WAV / FLAC / OGG / MP3 / AIFF
song.Volume = 0.9f;
song.Play();

while (song.AudiblePosition < song.Length)
    Thread.Sleep(100);

song.Stop();
```

`OpenFile` decodes the whole file and, off a 44.1 kHz source on a 48 kHz device,
resamples it — seconds of work for a long file. From a UI thread use
`OpenFileAsync`, which runs on the engine's dedicated worker thread.

`Position` is the transport's read head; **`AudiblePosition`** compensates the
stretcher's read-ahead when `TempoRatio != 1`, so it is the one to drive a
playhead from. Neither is an event — there is no position callback in the
wrapper; you poll. See [Threading](../guides/threading.md#polling-not-events).

## 6. Dispose in the right order

Stop the device, dispose the source, then the engine. The two `using`s above
already do it (C# disposes in reverse declaration order), and the engine's own
`Dispose` is a backstop: it claims its handle first, stops, then disposes every
source / plugin / MIDI input it handed out, and only then destroys the engine.

You therefore do **not** have to dispose children before the engine. A poll
racing a dispose surfaces as a caught `ObjectDisposedException`, never a native
crash, because `Dispose` waits for in-flight P/Invokes to return — and if one
outlives the drain deadline the handle is **leaked and logged at Error** rather
than freed under a live call.

## Where to go next

* [Threading](../guides/threading.md) — which call is safe from which thread.
* [Failure modes & runtime health](../guides/failure-modes.md) — what throws,
  what only logs, and how to tell a healthy engine from a struggling one.
* [Real-time constraints](../guides/realtime-constraints.md) — what your code
  must honour to keep the callback clean.
* [Devices, Hosts & Fallback](../guides/devices.md) for device selection in
  depth, and [Audio Playback](../guides/playback.md) for the transport.
