# Error Handling Patterns

JuceSharp follows three rules:

1. **Every native *error status* becomes a `JuceAudioException`** — never a
   silent return. This is about failures the engine *reports*: a
   `DllNotFoundException` is still what you get when `juce_sharp.dll` cannot be
   loaded at all, and it surfaces at the first member that actually calls into
   native — `new JuceAudioEngine(...)` usually, but `JuceDspModuleCatalog.Modules`
   documents it too, and it is deliberately reachable *before* any engine exists.
   (`JuceStemSeparation.IsSupported` is the exception that proves the rule: it
   catches both `DllNotFoundException` and `EntryPointNotFoundException` and
   answers `false`, because "can this build separate stems" is a capability
   question.) See [Installation](../getting-started/installation.md) for the
   two causes (missing DLL, missing VC++ runtime).
2. **Constructors validate eagerly** — bad inputs throw `ArgumentException`
   (managed validation) before any P/Invoke happens.
3. **`Dispose` is always safe** — calling it twice, from multiple
   threads, or after an exception is fine.

## Exception model

```csharp
try
{
    using var amp = engine.LoadPlugin(path);
}
catch (JuceAudioException ex) when (ex.Status == JuceAudioStatus.Plugin)
{
    log.Warn("Plugin load failed: {Msg} (op={Op})", ex.Message, ex.Operation);
}
catch (FileNotFoundException ex)
{
    // path didn't exist — caught before we entered native code
}
```

[`JuceAudioStatus`](../api/juce-audio-exception.md) values mirror the
native `DcaStatus` enum:

| Member             | Meaning                                            |
| ------------------ | -------------------------------------------------- |
| `Ok`               | success — you'll never see this on an exception    |
| `InvalidArg`       | a parameter was out of range                       |
| `NotInitialized`   | engine wasn't started                              |
| `Device`           | audio device couldn't be opened / disappeared      |
| `Decode`           | file couldn't be decoded                           |
| `Plugin`           | plugin couldn't be scanned / loaded / instantiated |
| `Cancelled`        | **not a failure** (ABI v36) — the caller's `CancellationToken` stopped a native stem separation. Surfaces as `OperationCanceledException`, never as a `JuceAudioException`, so a composite service does not read it as a reason to try the next backend |
| `Internal`         | unexpected C++ exception — see logs for details    |

## Validation errors

Construction-time validation always throws `ArgumentException`:

```csharp
new JuceEngineConfig
{
    SampleRate = -1,            // ArgumentOutOfRangeException
    BufferFrames = 999_999,     // ArgumentOutOfRangeException
};
```

You can call `JuceEngineConfig.Validate()` explicitly to fail fast in
config-load code.

## After-disposal calls

Every method on `JuceAudioEngine`, `JuceAudioSource` and
`JuceAudioPlugin` checks for disposal and throws
`ObjectDisposedException`:

```csharp
engine.Dispose();
engine.Start();   // → ObjectDisposedException("JuceAudioEngine")
```

## Native crashes ≠ exceptions

If a misbehaving plugin segfaults inside the audio thread, the .NET
process will crash — that's outside JuceSharp's control. The library
**does** install safety nets for the common shutdown-order bugs (see
[VST3 Hosting](vst3-hosting.md)), but bad code in third-party DLLs
ultimately runs at native privilege.

## The exceptions that are *not* `JuceAudioException`

1. **ABI mismatch** throws `JuceAbiMismatchException` (derives from
   `InvalidOperationException`) from
   `JuceAudioEngine`'s constructor, with the message
   `"JuceSharp.Native ABI mismatch: wrapper expects N, native is M. Rebuild
   the native DLL."` It is not a status code, so it cannot be a
   `JuceAudioException`.
2. **`RenderToWav` asked to stretch or shift** throws
   `NotSupportedException`. That offline path is a decode plus a WAV write; it
   has no Rubber Band transport. Only `tempo` of `0f`/`1f` and `semitones` of
   `0f` are accepted.
3. **A backend that is not compiled in** throws `NotSupportedException` too —
   `JuceStemSeparation.Separate` when `IsSupported` is `false`. It is
   deliberately a different type from the `JuceAudioException` a *failed* native
   call throws, so a composite service can tell "try the next backend" from
   "this backend broke".

## Stem separation has its own hierarchy — seven types

The Python sidecar (`JuceSharp.StemSeparation`) does not use
`JuceAudioException` at all. `StemSeparationException` is the base, with six
sealed subclasses, each of which a caller can branch on:

| Type | Means |
| --- | --- |
| `StemSeparationException` | the base; any sidecar failure that is not one of the below |
| `StemSeparationVramException` | out of GPU memory — the "retry on CPU, or lower `Overlap`" branch |
| `StemSeparationCudaDriverException` | the CUDA driver is too old / broken |
| `StemSeparationCudaUnavailableException` | torch found no CUDA at all |
| `StemSeparationMissingPackageException` | the Python environment is incomplete |
| `StemSeparationModelLoadException` | the model could not be loaded |
| `StemSeparationAudioFormatException` | the input audio could not be read |

> ⚠️ **A cancelled separation throws `OperationCanceledException`, NEVER a
> `StemSeparationException`.** If you chain backends, do not treat a user's
> Cancel as a reason to fall through to the next one. And note the asymmetry
> between backends: the sidecar's Cancel really does kill its child process,
> while `JuceStemSeparation`'s native call is not cancellable and runs to
> completion — see
> [Choosing a Stem-Separation Backend](stem-backends.md).

## Disposal

Every wrapper implements **`IDisposable` only** — there is no `DisposeAsync`
on `JuceAudioEngine`, `JuceAudioSource`, `JuceAudioPlugin`, `JuceMidiInput`,
`JuceLoopRegion` or `JuceMixer`.

`Dispose()` is a real barrier, not a flag flip: the handle owner waits on the
last in-flight P/Invoke before destroying the native handle, and if a call
outlives the drain deadline the handle is **leaked and logged at Error**
rather than freed under a live caller. `JuceAudioEngine.Dispose()` additionally
claims its own handle first (which closes its child registry), stops the
device, disposes every source / plugin / MIDI input it handed out, and only
then destroys the engine.

The practical consequence: you do **not** have to dispose children before the
engine, and a poll racing a dispose surfaces as `ObjectDisposedException` on
the next call rather than as a native crash.
