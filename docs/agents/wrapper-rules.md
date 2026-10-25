# Wrapper, lifetime, licensing and packaging rules (moved verbatim out of AGENTS.md)

## Rules (mirror the DotCharter root guide)

- All real-time audio lives in the native engine; the managed wrapper stays a
  thin shell (marshalling + lifetime). No managed per-sample processing on the
  hot path.
- **Incompatible ABI change ⇒ bump `DCA_ABI_VERSION` and rebuild the DLL** before
  running managed code. Every shared-memory bridge offset is `static_assert`ed on
  both sides — change the layout ⇒ bump `kBridgeVersion` and both mirrors.
- Handle-owning wrappers use `NativeHandleOwner` (acquire/release around every
  P/Invoke, drain-on-dispose) so a poll racing a dispose surfaces as a caught
  `ObjectDisposedException`, never a native crash. **Its field is nullable and
  every use goes through the wrapper's own `Acquire`/`TryAcquire`/`Release`
  helpers**, because the finalizer sees a constructor that threw.
  **That claim was FALSE on two counts until 0.61 and both fixes must stay**
  (`NativeHandleDrainTests`, `EngineChildDrainTests`):
  - **The drain must never free under a live call.** It waited 2 s on a
    `SpinWait` and then returned the handle *anyway*, so every caller destroyed
    it mid-call — and the calls that routinely exceed 2 s are the ones a user
    reaches (`TryShowAsioControlPanel` is a MODAL vendor dialog, `Reconfigure`
    reopens the driver, a cold VST3 scan is 25 s+). It now waits on the last
    `Release` and, on expiry, LOGS at Error and **leaks** the handle. A leak
    beats a use-after-free.
  - **The drain is PER-HANDLE, so the engine has to drain its CHILDREN.** The
    engine's counter says nothing about a `dca_source_*` call in flight.
    **FIVE types own a `NativeHandleOwner`** — `JuceAudioEngine`,
    `JuceAudioPlugin`, `JuceAudioSource`, and since the wave-3 fix wave
    `JuceMidiInput` and `JuceMidiOutput` too. Both MIDI wrappers held a raw
    `nint _handle` closed with one `Interlocked.Exchange` and had **no drain at
    all**; the OUTPUT one was the urgent half, because it makes live native
    calls (`Send`, `SendAt`, the counters) while `dca_midi_output_close` is a
    bare `delete` that joins the dispatcher thread and destroys the
    `juce::MidiOutput` — so a sequencer flushing note-offs while the UI tore the
    engine down, which is the ORDINARY shutdown path, was a native
    use-after-free rather than a caught `ObjectDisposedException`. Leaving the
    INPUT one is what let the output one be written that way, so both were fixed
    together; pinned by
    `WrapperLifetimeAuditTests.EveryHandleOwningWrapper_DrainsThroughNativeHandleOwner`.
    `JuceDspChain` and
    `JuceDspNode` own **no handle at all**: they are views on the engine and
    every call goes through `_engine.Acquire()` / `TryAcquire()`, so the
    engine's own drain already covers them.
    **The mechanism the 0.9 wave actually introduced, and that no document
    named until now, is `Interop/NativeDestruction` + `INativeChild`.** It is
    the COMPLETION half, deliberately distinct from claiming a handle: an
    internal `NativeDestruction` token per child (`_state`: unclaimed →
    destroying → destroyed, or LEAKED when the native destroy failed), `Run`
    executing the destroy exactly once under a gate, and `Wait` blocking until
    it is complete. The engine keeps the TOKEN even after the child wrapper has
    been collected, so teardown can wait on a destruction whose wrapper no
    longer exists — which a `WeakReference` alone cannot do. A destroy that
    fails parks the callback owner in a static bag rather than freeing it,
    because a leaked native object may still invoke its registered delegate.
    All FOUR of `JuceAudioSource`, `JuceAudioPlugin`, `JuceMidiInput` and
    `JuceMidiOutput` implement `INativeChild`.
    `JuceAudioEngine` keeps a
    WEAK registry of every source/plugin/MIDI input **and MIDI output** it
    hands out (`_midiOutputs`, tracked since the wave-3 drain fix — this line
    omitted it until 2026-09-12; a
    `JuceLoopRegion` is reached through its source) and `Dispose` claims the
    engine handle → stops → disposes every child → *then*
    `dca_engine_destroy`. Claiming the engine handle first is what closes the
    registry: every factory `Acquire`s before it tracks. **The registry is for
    teardown ORDER and roots nothing** — a MIDI input is ALSO held strongly in
    `_midiInputs` until it or the engine is disposed (2026-08-29), because the
    app wires the callback and keeps no handle, and a weak-only registry let
    the first gen-2 collection close the device silently
    (`JuceMidiInputRootingTests`). A `JuceLoopRegion` publishes its poll timer
    STOPPED and arms it only after checking it was not disposed meanwhile
    (`JuceLoopRegionAttachRaceTests`).
  The native side states the same contract at `dotcharter_audio.cpp`'s
  `dca_engine_destroy` (contract D-06), and it was rewritten in the same wave to
  match — it now says the wrapper drains and disposes every child handle before
  the destroy and LEAKS rather than frees a handle whose drain times out. **Do
  not re-open this as a TODO**: this bullet pointed an agent at
  `dotcharter_audio.cpp:~198` as work still to do for a release after it was
  done, which is the kind of note that makes the next reader redo it.
- **Every managed delegate handed to native needs a catch-all body.** A
  reverse-P/Invoke that throws unwinds through C++ frames: a process kill, not
  a catchable error. Three of the four callbacks always had it; the stem
  progress callback did not, and it is invoked from a demucs WORKER thread.
  `NativeCallbackSafetyTests.EveryDelegateHandedToNative_HasACatchAllBody`
  discovers the `[UnmanagedFunctionPointer]` types from `NativeMethods.cs` and
  lints both shapes — a variable assigned a lambda or method group, and a
  factory method whose return type is one of them.
- **Every string crossing this ABI is UTF-8: `[MarshalAs(UnmanagedType.LPUTF8Str)]`,
  never `LPStr`.** `LPStr` is the system codepage (audit #3: localized device names
  read back as mojibake and re-encoded to a byte sequence native could not match,
  so device selection silently fell through to the fallback walk). ASCII is
  identical in both, which is why one `LPStr` survived six ABI versions;
  `NativeMethodsTests.EveryStringParameter_IsMarshalledAsUtf8` reads the
  DECLARATIONS now. The two by-value structs' size and field offsets are pinned to
  the header the same way.
  **And a fixed out buffer truncates at a CODE POINT, never a byte** —
  `dcabi::utf8TruncatedLength` (`src/Utf8Truncate.h`) is the one implementation
  behind both `copyTruncated` and `copyStdTruncated`. A torn sequence decodes to
  U+FFFD, so a short buffer CORRUPTS the tail rather than clipping it; and
  `dca_device_info` writes the same name into `id[128]` and `name[256]`, so a
  byte-wise cut tore the id of any device whose 127th byte landed mid-sequence
  while leaving the name whole — the two then disagree and selection by id falls
  through, which is the `LPStr` mojibake failure from the other end. Pinned by
  `Utf8TruncationTests`, which drives every capacity through a real export and
  decodes with `throwOnInvalidBytes`.
- **A new PUBLIC member ships with a real XML doc comment — the compiler
  enforces it.** `CS1591` is no longer suppressed in `Directory.Build.props`
  (0.9; removing that one line surfaced 252 undocumented members across 42
  files), so an undocumented public type or member is a diagnostic at the site
  that introduced it — **an ERROR since 2026-09-11**, because
  `Directory.Build.props` now sets `TreatWarningsAsErrors` for the whole tree
  (the solution measured 0 warnings at the time, tests and examples included).
  **Never put the `NoWarn` back to make a build green** —
  `PublicApiDocsLintTests` fails if it returns, and also covers the two things
  the warning cannot see: a member missing from the assembly's SHIPPED `.xml`,
  and a summary that is a placeholder restatement of the signature
  ("Gets or sets the Volume.", "The Volume.", empty). Write what it does, its
  units, its threading contract, what it throws and the `dca_*` export it maps
  to. `tests/` and `examples/` set `GenerateDocumentationFile=false` in their own
  `Directory.Build.props`, so the rule reaches the six packable projects only.
- **These packages are NOT MIT, and five of the six say so.** The MIT grant
  covers this wrapper's C# source. `juce_sharp.dll` statically links JUCE 9
  (**AGPLv3** or commercial), Rubber Band v4 (**GPLv2** or commercial), the
  VST 3 SDK and ASIO SDK headers inside JUCE, and NeuralAudio/NAM
  Core/RTNeural/Eigen — so every package that carries or transitively pulls that
  DLL declares `PackageLicenseFile` and ships the repository `LICENSE` (the MIT
  text, then a LICENCE OF THE NATIVE BINARIES statement — every build of
  `juce_sharp.dll` / `juce_vst_host.exe` is AGPLv3 with the GPL terms of Rubber
  Band and ASIO — naming each component) and `THIRD-PARTY-NOTICES.md` (versions,
  copyright lines, licence texts). `ThirdPartyNoticeCoverageTests` fails when a
  `FetchContent_Declare` has no notice naming its repository and pinned tag, or
  when LICENSE's MIT text stops coming first. **`JuceSharp.Dsp` is the ONLY
  one that may keep `PackageLicenseExpression=MIT`**, and only because it has no
  ProjectReference, no PackageReference and no P/Invoke; a package may declare an
  expression or a file, never both (NU5035). Adding a native dependency to a
  package means moving it off the expression. `PackagingLintTests` pins the form,
  the packed LICENSE, that the file still names each component, and that exactly
  one project opts out.
- **Packaging, analyzer and CI settings live in ONE place each and are linted.**
  `Directory.Build.props` owns `<Version>`, the package metadata, and the
  analyzer level (`AnalysisLevel=latest`, `AnalysisMode=Default` plus
  Documentation/SingleFile at `All` — the highest combination measured clean;
  Minimum is DIRTIER than Default, so re-measure rather than assume).
  `Directory.Build.targets` packs the LICENSE. `global.json` pins the SDK with
  `rollForward`, and `.github/workflows/build.yml` installs THAT one
  (`global-json-file:`), runs on pinned runner images, and scopes the default
  token with a top-level `permissions: contents: read`. It runs on push and
  pull_request only on the PUBLIC repo: every job carries
  `if: github.event_name == 'workflow_dispatch' || !github.event.repository.private`,
  so on the private dev repo those events give a run of skipped jobs (0 paid
  minutes) and only the "Run workflow" button runs it. A hosted runner has no
  audio endpoint, so the windows job installs the VB-CABLE virtual device
  before the managed tests (the default-device engine tests need one), and the
  test project sets `DeterministicSourcePaths=false` so the `[CallerFilePath]`
  source lints still find the repo under `ContinuousIntegrationBuild`.
  Every action is pinned by full commit SHA, every checkout sets
  `persist-credentials: false`, the concurrency group includes the event, and
  JUCE / Rubber Band / NeuralAudio are fetched by commit (`*_COMMIT`, the
  commit their `*_TAG` named).
  `CiWorkflowLintTests`
  parses the YAML — never grep CI YAML, a substring matches the comment that
  explains why a block is absent.
- **A shipped package is UNSIGNED, so an `InternalsVisibleTo` naming an assembly
  outside this repo is a coupling anyone can claim, never a protection.** Grant
  one only when a named assembly uses an internal member TODAY. Three of the
  four such grants were dead in 0.9; the one that survives is
  `JuceSharp.StemSeparation.Onnx -> DotCharter.Tests`
  (`HtdemucsChunkScratchTests` constructs the internal `HtdemucsChunkScratch`),
  and `PackagingLintTests.Internals_are_granted_to_the_consumer_repo_exactly_once`
  LISTS the set so a new one is a deliberate edit.
- **Submodule workflow:** commit changes here first (this repo), then bump the
  submodule pointer in the parent DotCharter repo. Don't edit the wrapper to
  paper over a native bug — fix it native-side and rebuild.
