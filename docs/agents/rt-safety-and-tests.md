# Real-time safety and the test gates

Deep reference for `JuceSharp/AGENTS.md`. How this repo proves the audio thread
never allocates and never takes a new lock, plus the test conventions that came
out of the stem-separator flake hunt and the null-device work.

## Validation

`dotnet test --project tests/JuceSharp.Tests/JuceSharp.Tests.csproj`. The DSP soak
defaults to ~2 s and honours `JUCESHARP_SOAK_SECONDS` to run longer.

**`JUCESHARP_REQUIRE_NATIVE=1` turns "the native DLL did not load" from a SKIP
into a FAILURE. Set it in CI and set it locally once you have built the native
side.** `NativeAvailableFactAttribute` / `NativeAvailableTheoryAttribute` decide
by calling `JuceAudioEngine.NativeVersion` inside a try — and they guard roughly
a QUARTER of this suite (**265 of 987 test methods, re-measured 2026-09-11** at
`a1184bc`, including every rig, DSP and engine integration test — recount with
`grep -rho '\[[A-Za-z]*\(Fact\|Theory\)' tests/JuceSharp.Tests --include=*.cs |
sort | uniq -c`, where the `NativeAvailable*` rows are the guarded ones; the
plain `[Fact]`/`[Theory]` count alone is NOT the suite size). Nothing asserted the probe
was ever true and the probe ended in a bare `catch { return false; }`, so a
`juce_sharp.dll` the CLR could not load — wrong bitness, missing VC++ runtime, an
ABI-mismatched build — read as a GREEN run with a pile of skips. With the
variable set the skip is withheld, so the guarded tests run and fail on the
missing entry point instead of vanishing, and `NativeProbe.FailureDescription`
names the exception rather than swallowing it. `Wave2NativeProbeTests` holds all
three halves: the env gate, a check that the probe is TRUE whenever
`juce_sharp.dll` is on disk (so an absent build is distinguishable from a load
failure), and the non-vacuity check that a cause is recorded at all.
`.github/workflows/build.yml` sets it on the managed test step, beside
`DCA_ENABLE_NULL_DEVICE=1`.

**Everything that touches `StemSeparator`'s process-global gate or
`ChildProcessRegistry`'s process-global live set belongs to the
`[Collection("StemSeparatorConcurrency")]` (`DisableParallelization = true`) —
eight classes carry the attribute today, and the count only ever grows, so grep
it rather than trusting this number.** `StemSeparatorTests` and
`PythonEnvManagerStreamingTests` used to sit outside it and run in PARALLEL
with the two that were inside, so `ResetGlobalGateForTests()` disposed the very
`SemaphoreSlim` an in-flight separation was about to `Release()`
(`ObjectDisposedException` out of a `finally`) and `KillAll()` terminated the
other collection's python children mid-test. That cross-fire — not the
environment — was most of what "the StemSeparator tests are flaky" meant.

**A finalizer that throws takes the whole suite with it, at a point that moves.**
`JuceAudioPlugin`'s constructor rejects a zero handle by throwing BEFORE it
assigns `_owner`, and its finalizer then dereferenced that null. The CLR does not
catch a finalizer exception — it fails the process fast — so
`JuceAudioPluginTests.Constructor_RejectsZeroHandle` PASSED and the corpse killed
the test host whenever the GC next got round to it: 392 tests into one run, 251
into the next, which is precisely the shape of "a flaky test" and was not one.
Every handle wrapper's `Dispose` therefore reaches its owner as `_owner?.` and
never `_owner.`, and the throwing ctor path calls `GC.SuppressFinalize(this)`
before it throws. `FinalizerSafetyTests` discovers every finalizable type in the
assembly by reflection and runs each finalizer against a
`RuntimeHelpers.GetUninitializedObject` instance — the exact state a constructor
that threw leaves behind — so a new wrapper is covered the day it is written.

**An UNEXPLAINED sighting of the same SHAPE, recorded here so the next person
starts with a lead instead of a mystery.** On 2026-09-12 one full
`dotnet test` run died at **1,994 of 2,058 cases with 0 test failures** and
process exit code **-1073740771 = 0xC000041D
(`STATUS_FATAL_USER_CALLBACK_EXCEPTION`)** — an exception escaping a callback
the OS or the runtime invoked, which is the finalizer/reverse-P-Invoke family,
not an assertion. It was the FIRST run after the serialized collections landed,
so the ordering it ran in was new; it has not recurred in six full runs since
(the four that day plus two the same evening), and no single class reproduces
it. Nothing is known beyond that. If you see it again: the case count is not
the interesting number (it moved 392 → 251 last time this family bit), the
signature is. Start from `FinalizerSafetyTests`' list of finalizable types and
from the `[UnmanagedFunctionPointer]` delegates
`NativeCallbackSafetyTests` discovers — a delegate with a code path that can
throw is the other half of this shape, and the stem progress callback was
exactly that until it was fixed.

**A research harness is opt-in or it is part of the gate; a hard-coded home
directory makes it both at once.** `IdmtDataset.ResolveRoot` used to fall back to
one developer's absolute IDMT checkout path after `GND_IDMT_ROOT`, so on that one
machine — the owner's, the only one whose gate anybody watches — both CQT eval
harnesses ran on every `dotnet test`, despite three doc comments promising they
are opt-in and "never run in the gate". At the default `GND_EVAL_CAP` of 60
tracks that is ~10 minutes ending in either a plain assertion failure or a silent
test-host death. `GND_IDMT_ROOT` is now the ONLY source, pinned by
`IdmtDatasetTests` through an injected env reader.

Two more conventions that came out of the same pass:
- **A missing dependency must `Assert.Skip`, never `return`.** A bare `return`
  reports PASSED, so a machine with no python silently reported ~18 green tests
  that executed nothing. (`StemSeparationIntegrationTests` kept two bare
  `return`s until v0.44.)
- **Assert on OVERLAP, never on elapsed time.** The concurrency tests now have
  each python child drop a liveness marker, count its live peers, and remove the
  marker before exiting, so the gate's behaviour is read off a COUNT
  (`ObservedConcurrency`). The `elapsed < 4.2 s` bound they replaced broke on a
  loaded box while the gate worked perfectly.
  **Four more wall-clock assertions were retired the same way on 2026-09-12, and
  each needed a DIFFERENT structural answer — there is no single substitution:**
  - *"the child was KILLED, not waited out"*
    (`StemSeparatorTests.SeparateTrackAsync_cancellation_kills_long_running_child`,
    `PythonEnvManagerStreamingTests.RunStreaming_times_out_and_kills_child`;
    both were `sw.Elapsed < 30 s` / `< 15 s`, neither of which says anything
    about a kill). The script now writes a marker file at its start and a second
    one AFTER its 60 s sleep returns. Present ⇒ waited out; absent ⇒ killed; the
    start marker is what stops "absent" from being satisfied by a child that
    never ran. The elapsed time is logged.
  - *"the pacing thread holds its rate"*
    (`NullDeviceTests.GuardOn_CallbackCadence_TracksTheConfiguredRate`,
    `InputCaptureRingTests` ×2, which were `InRange(rate, 0.7x..1.3x)` and
    `InRange(rate, 0.5x..2.0x)` after a `Thread.Sleep`). **The lower bound is
    the flaky half and the upper bound is the load-proof half**: a device
    callback scheduled on an absolute `steady_clock` timeline can only ever be
    LATE, so a rate below the configured one is a busy box and a rate above it
    is a broken clock pair. The lower bound is replaced by the invariant that
    holds at any speed — the clock and the capture ring move in WHOLE BLOCKS,
    because both are published once per callback.
  - *"two entry points agree"* on a LIVE ring
    (`SeatZeroAliasEquivalenceTests`, all three capture tests; the stream one
    failed a full gate with `Expected: 256, Actual: 224`). Two consecutive reads
    of a self-paced ring are not comparable at all. `engine.Stop()` freezes it
    first, and the streaming pair then proves something STRONGER than the
    original equality: drain through the seat-0 export and the `_for_player(0)`
    twin must report zero at the same absolute position — one cursor, not two.

### The runner configuration, and the collection map it makes meaningful

**`tests/JuceSharp.Tests/xunit.runner.json` is the suite's runner
configuration, and the csproj's `<Content … CopyToOutputDirectory>` item is
half of it.** Nothing in the xunit.v3 build targets copies that file next to
the assembly, and a config the runner never finds is indistinguishable from no
config at all — which is how this suite ran its whole life: no runner json, no
assembly-level xunit attributes, no timeouts, so the effective settings were the
xunit.v3 defaults and **42 classes that open a real device, a child process or
an ONNX session sat in implicit per-class collections and ran concurrently with
everything**. `TestCollectionLintTests.The_runner_configuration_ships_next_to_the_assembly`
asserts the copy at run time from `AppContext.BaseDirectory`. Verified 2026-09-12
by setting `longRunningTestSeconds: 1` in the OUTPUT copy and watching the
`[Long Running Test]` diagnostic appear — the file is auto-discovered and
honoured, it is not decoration. **`parallelizeTestCollections` stays `true`**:
turning it off serialises the whole ~2,050-case gate to fix a few dozen classes.

**`DisableParallelization = true` is the load-bearing flag and it does NOT mean
what its name suggests.** xunit's own doc: *"determines whether tests in this
collection runs in parallel with any other tests"* — it is exclusion against the
WHOLE run. A collection without it is serialized against ITSELF and against
nothing else, which is a guarantee almost nobody wants. `"NullDevice"` was
defined that way, so the promise written at the top of `NullDeviceTests.cs` —
that the process env guard can never leak into the guard-off assertion — was
untrue for a release. Every sanctioned collection carries the flag now and
`TestCollectionLintTests.Every_sanctioned_collection_disables_parallelization`
fails if one stops.

The five sanctioned collections, and what puts a class in one:

| Collection | Defined in | A class belongs here when it… |
|---|---|---|
| `NativeEngine` | `TestCollections.cs` | creates a native engine (`new JuceAudioEngine(`, `NullDevice.CreateEngine(`, `JuceEngineFactory`), spawns the `juce_vst_host.exe` child, loads the real ONNX runtime, parks more than one thread on a bounded wait, or reads a process-wide list the engine mutates (`Process.GetCurrentProcess().Modules`) |
| `NullDevice` | `NullDeviceTests.cs` | SETS `DCA_ENABLE_NULL_DEVICE` (i.e. constructs a `NullDeviceScope`) |
| `AllocationProbes` | `TestCollections.cs` | asserts on `GC.GetAllocatedBytesForCurrentThread()` |
| `StemSeparatorConcurrency` | `StemSeparatorConcurrencyTests.cs` | drives `StemSeparator`'s global gate or `ChildProcessRegistry`'s live set |
| `StemDownloads` | `CoverageGaps/Wave2StemFixTests.cs` | runs a real transfer, or writes `VerifiedDownloader.RequestTimeout` |

`CoverageGaps/TestCollectionLintTests` sweeps the source for each marker set and
fails on a class that carries one and no sanctioned attribute. **A bare
`new JuceAudioEngine()` opens the host's REAL default output device** — only
`NullDevice.CreateEngine()` asks for the null type — and xunit builds one class
instance per test, so an engine in a constructor is a full device open and close
per fact. ASIO is exclusive; that is what "the engine-creating tests time out"
meant.

**A GC allocation delta is a READING, not a fact.**
`GC.GetAllocatedBytesForCurrentThread()` subtracts the unconsumed remainder of
this thread's allocation context, and a collection RETIRES that context — so two
reads straddling a GC differ by up to the ~8 KB quantum *with nothing allocated
between them*, and these assertions have zero tolerance. **The collection alone
is not enough and that was measured, not assumed**: with `AllocationProbes`
serialized, `StreamingSpanPushTests.GuitarAnalyzerSpanPush_AllocatesNothingWhenNoEventsAreEmitted`
still reported 360 bytes over a region whose subject allocates none (2026-09-12,
third full run of the day) — the runtime's own threads keep allocating after the
rest of the suite has stopped.

So every zero-tolerance window goes through **`AllocationProbe.Measure`**
(`tests/JuceSharp.Tests/AllocationProbe.cs`): it settles the heap so the window
opens on a FRESH quantum, and re-reads once if the answer is non-zero. **A real
per-call allocation is deterministic and survives every retry**, and these
regions allocate tens of KB the moment their subject regresses, so the retry
cannot hide one. The caller still warms its own path up first — the first-ever
execution drags tier-0 JIT and first-touch cctors into the window and widens it
by orders of magnitude. `Wave2WavAuditFixTests.WritePlanar_DoesNotAllocate_NotEvenOnTheFirstCall`
keeps its own copy of that shape because its subject is the FIRST call on a
FRESH writer, so its warm-up has to run on a separate throwaway one.
**`GC.TryStartNoGCRegion` is deliberately not used**: it throws when the budget
cannot be granted, which on a loaded box is the same flake wearing a different
exception. No wall clock anywhere in any of it.

**No per-test `Timeout` was added, and that is a decision, not an omission.**
xunit only supports `Timeout` under `parallelAlgorithm: conservative` or in a
parallelization-disabled collection, so the child-process classes now qualify —
but they are already bounded from the inside (`VstHostChildLifecycleTests`'
`ExitTimeoutMs = 90_000`, documented there as "DEADLOCK DETECTOR, NOT A BUDGET";
every `Wait`/`WaitForExit` in the child-process classes takes an argument). A
second bound on top would ABANDON the test thread on expiry and leak the child
process past its `finally`, which is worse than the bound that exists. Add one
only for a wait that is genuinely unbounded, and never as a budget.

**Null device for device-dependent LOGIC tests.** `src/NullAudioIODevice.h` is a
"Null" `juce::AudioIODeviceType`: accepts any config, paces the callback from
its own thread (blockSize/sampleRate) with silence input, discards output. It is
registered ONLY when the engine was created with `DCA_ENGINE_CREATE_NULL_DEVICE`
(`dca_engine_create2`, ABI v41; managed `JuceEngineConfig.EnableNullDevice`) or
the env var `DCA_ENABLE_NULL_DEVICE=1` is set at engine start / mirror open
(`nulldev::registerIfEnabled`) — never in normal device lists. The two sources
are OR-ed; neither switches the other off. **Product code uses the FLAG only**:
the env var is process-wide, so a host that set it around one engine's
construction leaked the type into a live engine another thread built meanwhile
(DotCharter #85). A test that takes the flag sets no process state, but it still
builds an engine, so `TestCollectionLintTests` still wants it in a native
collection; `AbiV41Tests` pins the flag. Env-var test conventions
(`NullDeviceTests.cs`):
- Every test that SETS the guard lives in the xunit
  `[Collection("NullDevice", DisableParallelization = true)]` (the var is
  process-wide; the collection serializes setters against the guard-off
  assertion **and against every other collection**) and uses `NullDeviceScope` +
  `NullDevice.CreateEngine`. **Both halves of that were missing until
  2026-09-12** and the claim written in the file was false in both directions:
  `CoverageGaps/WrapperErgonomicsTests` constructed a `NullDeviceScope` from
  outside the collection — the only one of the suite's construction sites that
  did — so it set the guard while `GuardOff_NullType_NeverAppearsInDeviceLists`
  asserted the type is absent, and `NullDeviceTests`' own `Dispose` cleared the
  guard while that class's `NullDevice.CreateEngine()` was mid-constructor,
  which unregisters the type and throws. Failed under load, passed in
  isolation. `TestCollectionLintTests.Every_class_that_sets_a_process_environment_variable_is_serialized`
  and its engine sweep keep it closed.
- Capture-ring / mirror-switch / bridge round-trip stress runs on the null
  device; each family keeps ONE real-device smoke test (`*RealDeviceSmokeTests`)
  that `Assert.Skip`s on hosts without devices.
- Two registration gotchas live in the header comment: standard types must
  exist BEFORE `addAudioDeviceType` (an empty manager never creates them
  afterwards), and the env var is read via `SystemStats::getEnvironmentVariable`
  because the CRT's `getenv` cannot see variables set at runtime by managed code.

Two ABI tooling exports exist for deterministic in-gate DSP testing (and double
as offline bounce hooks): `dca_droptune_process_offline` (mono CSVD with
per-segment semitones) and `dca_engine_dsp_process_buffer` (stereo buffer
through the live DspChain, safe against concurrent mutations). Prefer them over
inventing device fixtures.

RT scratch rule (2026-07-16 audit #5): every audio-thread buffer is pre-sized
with 2x block headroom at prepare/attach time and the hot paths **skip the
block** instead of growing — never add a `setSize`/`resize` on the callback (or
on the sandbox child's worker, which the parent blocks on).

## Native RT gate (`juce_sharp_rtcheck`)

The rule above is no longer defended only by comments. `src/JuceSharp.Native/tests/rt/`
builds a second executable from the SAME sources as the DLL and drives the real
device callback with an allocation hook armed:

```powershell
cd src/JuceSharp.Native
cmake --build build --config Release --target juce_sharp_rtcheck
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

- **THE ORDER OF THE RUN LIST IS LOAD-BEARING, AND IT IS NOW ENFORCED.**
  `scenarioBridgeWaitBounded` launches the out-of-process VST host child and
  TIMES its handshake, so anything registered AHEAD of it shifts that timing and
  phase C of `RtBridgeWaitIsBounded` fails on a binary nobody changed — which
  reads as a flaky gate rather than as an ordering mistake. Five comments in
  `rt_main.cpp` asked the next author to APPEND at the end of the list and not
  one of them could stop anybody. The prefix is pinned now: `run` counts every
  registration (filtered or not) and **`RtRunListOrderIsPinned`** fails if the
  bridge scenario no longer registers at index
  `kRegistrationsBeforeTheBridgeChild` (41 on 2026-09-12), naming the cause and
  telling you to append instead. **Append and you never touch that number**; it
  moves only if you deliberately move the bridge scenario itself. The count is
  `run()` registrations, so it excludes `scenarioEveryCatalogueModule`, which
  takes a parameter, cannot match `run`'s `void(*)()` and is invoked directly.
  It is the one meta-assertion that still runs under `--only`, because it reads
  registrations rather than an accumulated result set.
- **`_CrtSetAllocHook` is the hook, and the debug CRT (`/MDd`) is therefore the
  gating configuration.** `juce::HeapBlock` and `AudioBuffer::setSize` allocate
  through `malloc`, not `operator new` — an `operator new` replacement (present
  as the portable fallback) would miss the entire audit-#5 class of bug. If the
  gate reports "no malloc-level hook", it is not testing what you think.
- **No ABI export exists for any of it.** The gate reaches the engine by
  compiling `AudioEngine.cpp` et al. directly, and drives blocks through the
  null device's manual-pump mode (`DCA_ENABLE_NULL_DEVICE=1` **plus**
  `DCA_NULL_DEVICE_MANUAL=1`), where `start()` skips the pacing thread and
  `nulldev::pump(n)` runs the callback synchronously on the calling thread. Do
  not add a test-only export; an export is permanent, a recompile is not.
  (It CAN drive a real SHIPPING export, though — `dotcharter_audio.cpp` is in
  `JUCESHARP_CORE_SOURCES`, so the exe carries every `dca_*` entry point and can
  open a genuine `dca_engine_t` on the PACED null device. That is what makes an
  export's own argument handling testable here without a device and without a
  test hook; see `RtXrunStatsAbiHonoursStructBytes` below.)
- **`src/RtDiag.h` compiles away.** `DCA_RT_LOCK_GUARD(m, "site")` is a plain
  `std::lock_guard` unless `DCA_RTCHECK=1` (rtcheck target only); with it, the
  guard times its own acquire. Every audio-thread mutex uses it.
- **`tests/rt/rt_lock_sites.txt` is the lock-site allow-list.** Adding a mutex
  to a callback path fails the gate with a diff — the one class of regression no
  allocation hook can catch. Removing a site means editing that file in the same
  change.
- **The gate cannot see a lock taken INSIDE the standard library, and there is
  one: `std::atomic_*(shared_ptr*)`.** On the MSVC STL those free functions are
  not lock-free (`atomic_is_lock_free` returns false); each one constructs a
  `_Shared_ptr_spin_lock`, a **process-wide** CRT spin lock. The audio thread
  takes it three times per block — `AudioEngine::processInputChain`
  (`inputChain_`), `DspChain::process` (`order_`) and `NamAmpModule::process`
  (`active_`) — against message-thread holders in `DspChain::publishLocked`
  (every add/remove/move/bypass edit), `attach`/`detachInputPlugin`, and the
  namamp worker's model swap. The sections are a pointer copy plus a refcount
  bump, so it has never moved the callback-duration numbers, but **a fourth
  such site on the RT path would pass the gate silently.** Don't add one
  without raising it. (This is why `namamp`'s poll-only `loaded` param is a
  separate `atomic<bool>` and not a read of `active_`: the UI polled it, which
  put a UI-rate contender on a lock the callback needs.)
- **A `shared_ptr` snapshot the callback copies must be RETIRED by its
  publisher, after the barrier.** `DspChain::process` copies `order_` for the
  block; `publishLocked` used to `atomic_store` the replacement and let the old
  snapshot's refcount fall wherever it fell — which, whenever a block was in
  flight, was on the audio thread: the callback's copy became the last owner
  and freed the vector. `RtStructureStormCrossfadesWithoutAllocating` (2026-08-28)
  is the first scenario that edits STRUCTURE while armed — bypass, move, retag,
  add+remove from a storm thread — and it reported 435 callback frees on its
  first run. `publishLocked` now exchanges, barriers, then resets the old
  pointer on the message thread. The same shape applies to any snapshot the
  callback copies (`inputChain_`, namamp's `active_`): the publisher owns the
  last reference, always.
- **A publisher that CANNOT take the callback's mutex still may not give up.**
  `DspChain` retires its order snapshot behind a wait-one-block barrier;
  `NamAmpModule` has no such mutex and used to spin on `use_count() > 1` for
  500 ms and then `reset()` ANYWAY. That is not a fallback — past the bound the
  callback's copy is the last owner, so a slow block was handed a `free()` plus
  a whole NeuralAudio model destructor, precisely when it could least afford
  one; and clearing a capture blocked the MESSAGE thread for half a second
  doing it. `src/dsp/Retirement.h`'s `dcdsp::RetirementQueue` is the shared
  mechanism now: park the retired object, sweep it from the worker once
  `use_count() == 1`, destroy it nowhere else. That count test is sound in this
  ONE direction only — the object has already been swapped out of the atomic
  slot, so no reader can take a new reference and the count can only fall.
  Pinned by `RtRetiredSnapshotsDieOnTheOwnerThread`.
  **namamp itself is NOT in this binary**: `DOTCHARTER_DSP_NEURAL` never
  reaches the test target, so `NamAmpModule.cpp` is not compiled into
  `juce_sharp_rtcheck` (the string "namamp" is in `juce_sharp.dll` and not in
  `juce_sharp_rtcheck.exe` — check it that way). The CMake comment at
  `CMakeLists.txt:457-464` already explains this correctly — the append is inert
  because `tests/CMakeLists.txt` removes the entry again for the debug CRT — so
  do not "fix" it back. That is a real hole in the gate. Until
  it is closed, the module's use of the queue is held by the managed source lint
  `NativeRtSourceLintTests.NamAmpModule_RetiresEveryPublishedModel_AndFreesNoneItself`,
  which strips comments first and fails if any `atomic_exchange(&active_` is not
  followed by a retire.
- **Every untrusted stage is followed by a scrub (2026-08-29).**
  `dcdsp::scrubNonFinite` (`src/dsp/Scrub.h`) is the one implementation, shared
  by `DspChain` and `AudioEngine`. A NaN entering a `juce::dsp::IIR` biquad is
  PERMANENT — it feeds its own output back, and the v29 EQ only calls `reset()`
  on an OFF→ON transition (deliberately: resetting a running band on every knob
  tick clicks). The chain had scrubbed its input and every stage boundary since
  it existed; the engine stages downstream of it had not, and two of them take
  samples from code this project does not compile — `processInputChain` (the
  user's VST3s) and `RemoteVstBridge::process` (a memcpy out of a child
  process's mapping). One NaN block therefore latched that seat's input EQ
  silent for the session, and the peak meter reads the same buffer, so the UI
  showed a dead input with nothing saying why. Three passes now: after the input
  chain + bridge (before `applyEqBank`), at the top of `processMasterBus`
  (the three fixed filters and the song EQ, whose contributors are the decoders
  and Rubber Band), and after the instrument mixes (before `mirror_.pushBlock`,
  i.e. before the device and before OBS/Discord).
  `RtNaNFromAnInputPluginCannotLatchTheEq` drives phases A–D with a stub input
  plugin and a stub VSTi that each poison exactly ONE block. Verified
  non-vacuous by removing each pass: without the input scrub the seat's min
  block peak goes 0.260875 → 0.000000 and never recovers (the latch, exactly);
  without the pre-mirror scrub phase D reports the instrument's NaN block
  reaching the device output. **The `processMasterBus` pass is the one with no
  scenario**: nothing upstream of it is reachable from the gate without
  shipping a 32-bit-float WAV fixture full of NaN through a real transport, and
  it is defensive insurance over a latch that is otherwise unrecoverable.
  A detector note worth keeping: `Fixture::lastOutputPeak()` CANNOT see this —
  `std::max(x, NaN)` returns `x`, so a fully poisoned block reads back as a peak
  of exactly 0. `Fixture::lastOutputHasNonFinite()` scans the block.
- **Four smaller audio-thread items, 2026-08-29, none of them separately
  testable.** `DspChain::setMetering(false)` now takes the one-block `barrier()`
  before clearing the meter pool: `metering_` is read ONCE at the top of
  `process()`, so a callback that loaded `true` a microsecond earlier goes on
  writing meters *after* the clear and leaves exactly the frozen lit meter the
  clear exists to prevent. `RemoteVstBridge` reads
  `QueryPerformanceFrequency` once in its constructor into `qpcFreq_` — it was
  calling it on the audio thread on every waiting block, for a value that is
  fixed for the life of the process. And the mono-mix path in the input callback
  null-checks `in[captureCh]` and `in[c]`: JUCE hands a null pointer for a
  channel the driver did not provide, every sibling read in that callback guards
  it, and these two did not. All three are invisible-until-they-are-not and are
  covered only by the gate staying green (0 allocations, 0 frees, the lock-site
  set unchanged).
- **The gate cannot see a lock that is MISSING, and one was (2026-08-29).**
  `rt_lock_sites.txt` fails on a lock the callback newly TAKES; nothing fails on
  a lock a message-thread writer forgot. `DspChain::prepare` took only
  `writeMutex_` while `process()` takes only `processMutex_`, so a device
  change, a NAM 48 kHz request, a watchdog recovery or `dspProcessOffline`'s
  lazy re-prepare freed `branchData_`/`dryData_` and re-prepared every module
  under a running block. `prepare()` now takes both, `writeMutex_` ->
  `processMutex_`, the order `publishLocked`/`remove`/`clear` already use.
  `RtRigPrepareCannotRaceTheCallback` drives a bare `DspChain` rather than the
  engine, because neither shipping entry point can be made to overlap on demand
  any more (the offline export refuses a live device; JUCE serialises its own
  device restart), and its detector is the geometry: a wide 2 x 1024 scratch
  alternating with a narrow 1 x 64 one while the render thread keeps asking for
  1024 frames through a real split region. Verified non-vacuous by removing the
  second lock — three consecutive runs aborted (exit 3, 3, `0xC0000409`) against
  a guarded build that reports 1,021 blocks / 159 prepares and passes.
- **`--only <substring>` runs one scenario, for exactly that A/B.** A full gate
  run is ~1 min idle and ten times that with another agent building, which is
  what made "break it on purpose, watch it fail, restore" expensive enough to
  skip. Under a filter the two meta-assertions (`RtLockSiteSetIsUnchanged`,
  `RtBaselineGate`) are SKIPPED — both compare a set accumulated across every
  scenario, so a partial run would report every unexercised lock site as
  removed — and a filter that matches nothing exits 2 rather than 0. It is a
  debugging aid; ctest still runs the whole file.
- **A race / contention probe GATHERS its evidence; it never assumes a fast
  machine.** Every probe whose vacuity floor counts events from another thread
  (bridge waits, rebuilds, storm writes, toggles, prepares, sweeps, seqlock
  reads) runs through `gatherEvidence(round, enough, minRounds)` in
  `rt_main.cpp`: at least its old nominal window, then on until the floor is
  met, bounded by a 60 s deadlock-detector cap (`evidenceCap()`). It fails only
  when the cap (or a nearly full mark recorder) ends it first, and the message
  then says so (`Gathered::why()`). The floors and the bounds the probes prove
  are unchanged. Why: on GitHub's 2-vCPU runner `RtBridgeWaitIsBounded` phase A
  saw 58 bridge waits in a fixed 256 blocks against a floor of 129 (#55).
  Two shapes matter: a floor that is a SAMPLE SIZE (phase A's 129 waits for its
  p99) accumulates across rounds, but a floor that is a DENSITY (phase C's
  "more than 100 waits in 600 blocks", which poll-only's every-256-blocks retry
  cannot reach) repeats whole windows and never sums them — summing would let
  the retries alone pass given time. `JUCESHARP_RT_EVIDENCE_CAP_SECONDS`
  shortens the cap for a mutation run; nothing in CI sets it. A NEW
  multi-threaded probe uses the helper, never a fixed window or a private
  deadline. Not covered by it: the wall-clock bounds themselves (phase A's p99,
  phase B's max). On a box oversubscribed enough to preempt the measuring
  thread mid-wait they can still fail (seen 2026-10-04 with spinner processes
  pinned to the same 2 cores; the CI runner itself measured p99 18.8 us).
- **`tests/rt/rt_baseline.txt` is a curated, wildcard-free allow-list** of the
  real-time violations that are architectural rather than fixable. It contains
  exactly one line (the sandbox round-trip wait). Heap allocations on the audio
  thread are never baselined.
- Warm-up is 64 unarmed blocks before every measured window; the warm-up
  allocation count is printed as an informational number, and a change in it
  means something moved into or out of `prepare`.
- **A scenario that only counts allocations is half a test.** An allocation gate
  passes perfectly against code that does nothing at all, so every scenario that
  adds a FEATURE has to assert the feature happened too.
  `RtNodeMeteringIsAllocationFreeAndNotVacuous` (ABI v30) is the worked example:
  it drives a real 0.25 sine through a real 12-node rig with the hook armed and
  fails on zero allocations, on meters that never moved
  ("every meter read zero while a 0.25 sine was running through the rig"), on an
  RMS above its own peak, on a bypassed node counting toward chain latency, and
  on meters that stay lit after metering is switched off. The off-state and
  on-state assertions contradict each other unless the code really runs, which
  is what makes it structurally non-vacuous — and it was verified by stubbing
  the meter write, watching it go red, and restoring it.
- **`RtBridgeWaitIsBounded` flakes intermittently, and NOT because of your
  change.** Its phase A wants the stub sandbox child to answer every block;
  sometimes it answers none and the scenario fails with "phase A saw only 0
  bridge waits — the probe is vacuous". Measured 2026-08-08 over eight runs of
  the SAME binary (nothing recompiled between them): three failures, five
  passes, interleaved. **It does not correlate cleanly with free RAM** — one
  failure happened at 12.4 GB free, and three consecutive passes followed it
  immediately. So the first move is to re-run it, not to bisect: if the same
  binary passes, that is the answer. Phase B (the wedged-child bound) has never
  been seen to flake; it is only the healthy-child probe that goes vacuous,
  which points at the child's startup handshake rather than at the RT path this
  scenario exists to measure. Worth fixing properly one day — a flaky gate
  teaches agents to ignore it.
- **`juce_vst_host.exe` writes `hello` BEFORE it starts `pipeReaderLoop`
  (JuceSharp #28).** The control pipe is a SYNCHRONOUS handle and Windows
  serialises synchronous I/O on one handle; `hello` used to go out after the
  reader had parked in its blocking `ReadFile`, so it waited for a read only
  the parent could complete, and a parent that waits for `hello` before it
  speaks (`VstHostChildLifecycleTests`) deadlocked in 4 launches of 70. Pinned
  by `VstHostChildLifecycleTests.TheChildSaysHelloBeforeItStartsReadingThePipe`.
  The same serialisation still holds back any LATER unsolicited child write (an
  `event` line) until the parent's next command — the product's
  `VstSandboxHost` keeps the line moving with its commands and pings.
- **PHASE C flakes too, and re-running does NOT clear it (2026-08-23).** The
  paragraph above says the first move is to re-run; for phase C that is not
  enough. It failed THREE consecutive runs with "only 2 waits over 600 blocks"
  on a binary that had just passed 16/16 minutes earlier, with 26 GB free and
  no stray `juce_vst_host.exe`, so neither documented cause applied. What
  settled it was the A/B the root guide prescribes: `git show HEAD~1:<path>`
  the changed sources into place, rebuild, run — the UNMODIFIED tree failed
  identically. **So a persistent phase-C failure is not evidence that your
  change broke it, and "it passed for me earlier" is not evidence that it
  did.** Do the A/B before spending an afternoon on the callback path.
- **Poll-only is NOT a one-way door, and phase C of `RtBridgeWaitIsBounded`
  exists to keep it that way (2026-08-21).** When the sandbox child misses three
  consecutive deadlines the bridge stops waiting for it, so a dead child costs
  the audio thread nothing. The check it then runs tests the response sequence
  NANOSECONDS after posting the request — which no real cross-process child can
  have answered — and `consecutiveTimeouts_` was only ever reset ON SUCCESS. So
  poll-only latched permanently and the bridge went **silent for the rest of the
  session**. It latches trivially: the child holds `graphMutex_` across
  `prepareToPlay`, so LOADING an instrument stalls several blocks by
  construction, and at 128 frames / 48 kHz the budget is 2.67 ms. Owner report
  2026-08-21: the VSTi loads, MIDI still reaches it (the request is written and
  the event set regardless of the wait), and no audio ever comes back. A merely
  slow child keeps its heartbeat alive, so the managed health timer never
  restarts it either. `RemoteVstBridge` now re-tries the real synchronous wait
  every `kRetrySyncEveryBlocks` (256, ~0.7 s at 128/48k) so a child that has
  finished loading is found healthy again.
  **The stub child needs `setAnswerDelayUs` for that phase to mean anything.**
  With a zero-delay stub the immediate poll succeeds by accident and phase C
  passed at 580/600 waits against the BROKEN code. At a realistic 300 µs it
  reads 0/600 broken and 374/600 fixed.

### The scenario roster — all 47 (76 by 2026-09-27: re-count)

**2026-10-20, ABI v48 appended `RtWemSourceIsPreloadedNeverStreamed`: 95 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp`.** **2026-10-18, ABI v47 appended `RtExternalProcessInsertExchangesWithoutAllocatingAndFallsBackDry`: 94 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp`; a full run printed 97 summary lines, all PASS.** **2026-10-13, JuceSharp #29 appended `RtCallbackLoadIsMeasuredWithoutAllocating`: 92 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp` (91 before it).** **2026-10-12, JuceSharp #31 appended `RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart`: 88 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp` (87 before it).** **2026-10-12, JuceSharp #32 appended the three `RtSpeedCrossingUnityIsSeamless*`: 87 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp` (84 before them).** **2026-10-07, #411 appended `RtPreloadedSourceHoldsSixteenBitPcm`: 82 lines match `run("Rt` / `wanted("Rt` in `rt_main.cpp` (81 before it).** **2026-09-28, the JUCE 9 port appended one (`RtMidiOutputRetriesARefusedSendAsJuce8Did`): a
full run prints 78 summary lines (75 registered scenarios, `RtRunListOrderIsPinned` and the
two meta-assertions).** **Re-counted 2026-09-27 after ABI v37 appended two: 74 lines match
`run("Rt` / `wanted("Rt` in `rt_main.cpp`, plus the two meta-scenarios.** The
paragraph below is the 2026-09-11 count and is kept as the history it is.

This file described 21 scenarios and the gate had 43 (measured 2026-09-11 in
`src/JuceSharp.Native/tests/rt/rt_main.cpp`). It is **47** as of the wave-3 fix
wave: **44 registered through `run("Rt…")`, plus `RtEveryCatalogueModuleIsAllocationFree`,
which is registered on its own through `wanted("Rt…")` because it takes a
per-module duration argument (`rt_main.cpp:5113-5114`), plus the two
meta-scenarios below** (`RtLockSiteSetIsUnchanged`, `RtBaselineGate`). That
number was already wrong when it was written — it missed the engine wave's own
two scenarios — which is the point of the sentence below. **Re-count rather than trusting this
number**, and count BOTH registration forms:
`grep -c 'run("Rt\|wanted("Rt' rt_main.cpp` (45 today) plus the two meta. A recount that
greps `run("Rt` alone is short by one and yields 46. A reader who follows the router
here must be able to see the whole gate; the sections around this one go deep on
a subset, and the rest were invisible.

**Allocation baselines** — the plain "this path costs the callback nothing" set:

| Scenario | One line |
|---|---|
| `RtIdleCallbackAllocatesNothing` | The floor: an open device with nothing playing. Anything here is a regression in the engine itself. |
| `RtFullRigAllocatesNothing` | A loaded rig with a real split region (two nodes on branch 2, so `DspChain::process`'s split path is exercised, not merely present). |
| `RtEveryCatalogueModuleIsAllocationFree` | Sweeps the WHOLE catalogue — every module type constructed, prepared and pumped. Not run under `--only`. |
| `RtConcurrentParamStormAllocatesNothing` | Param writes from a second thread at a rate no UI reaches, against a live callback. |
| `RtFilterSweepAllocatesNothing` | The master-bus filters swept while audio runs. |
| `RtMidiFloodAllocatesNothing` | The MIDI ring under a flood, including the packed drain. |
| `RtBlockSizeRenegotiationSkipsNotAllocates` | A block larger than the prepared headroom must SKIP the block, never `setSize` its way out. |
| `RtDropTuneOfflineIsAllocationFree` | The CSVD drop tuner on the offline tooling path, which inherits none of the callback's guarantees. |
| `RtChartVoiceRendersEveryArticulationWithoutAllocating` | The chart voice (guitar and bass, `src/dsp/voice/`) on a dense chart using every articulation, with a seek and a stop, driven on the calling thread like the drop tuner. Fails as vacuous if the armed window is silent, if a string never sounded, or if a stop leaves strings sounding. See `chart-voice.md`. |
| `RtChartVoiceZoneMissFallsBackWithoutBlocking` | #417, through the ENGINE: a chart published WITHOUT its sample zones (`setChartZonePreloadForTesting(false)`) plays its first note from the nearest decoded recording in the block that asks for the zone; the chart reaper decodes it; nothing allocated or freed on the callback. Fails as vacuous if the zones were decoded before publishing or the note never started. |
| `RtChartVoiceNewChartReleasesTheOldZones` | #417: another song's upload swaps in a voice on FRESH lazy packs (cross-faded), the reaper frees the old one, the held bytes fall to the new chart's; an edit only adds its zones. Nothing allocated or freed on the callback across the switch. |
| `RtStemGroupChurnDoesNotStallTheCallback` | Stem-group tempo/pitch churn must not stall the pump. |

**Structure, edit scopes and the v34 rig** (the wave's own additions):

| Scenario | One line |
|---|---|
| `RtStructureStormCrossfadesWithoutAllocating` | Bypass / move / retag / add+remove from a storm thread. The first scenario to edit STRUCTURE while armed; it found 435 callback frees on its first run. |
| `RtPresetCommitPublishesOncePerPresetNotOncePerNode` | An edit scope must cost ONE publication (nothing audible removed) or TWO, independent of preset size — counted against the unscoped baseline while the callback runs. Also asserts the outcome: right-sized chain, abort restores the start state, and that a NESTED begin **aborts the stale scope and bumps `staleScopeAborts_`** rather than being refused — see below. |
| `RtBranchPdcFollowsALatencyChangeWithoutAllocating` | A whammy's `lowLatency` flip inside a live split region (WSOLA geometry 1,152 → 1,920 samples) must move the merger's taps without sizing anything on the callback. Non-vacuity: branch delays non-zero with PDC on, zero with it off. |
| `RtBranchPdcFollowsThePublishedOrderNotTheShadow` | **Added 2026-09-11.** Branch PDC is derived from `publishedOrder_`, never `idOrder_`. Deriving it from the shadow re-aligned a region that is still playing the PRE-edit chain — and `setParam` had no `editing_` check, so the FIRST knob write inside a preset load did it. The observable is `chainLatency()`: the alignment must not move while a scope is open, and must move when it commits. |
| `RtSidechainKeyingIsAllocationFreeAndAdvertisedHonestly` | A compressor keyed off the dry input and a gate off the song mix, pumped armed — plus a catalogue sweep asserting every `dca_dsp_module_type_flags` row agrees with a constructed module and that the setter REJECTS a module that does not advertise the capability. |
| `RtNaNInTheSongMixSidechainKeyCannotLatchADetector` | **Added 2026-09-11.** The v34 song-mix key is the engine's OUTPUT buffer — untrusted, six `juce::dsp::IIR` biquads upstream — so `DspChain` copies it into `prepare()`-sized scratch and scrubs that. The per-stage scrub cleans the BUFFER and never a module's state, so one NaN latched `NoiseGateCore`'s recursive detector and `CompressorCore`'s RMS accumulator for the session. What is asserted is the CHAIN's half: the key handed to a module is finite. |
| `RtStagedInsertionNeverExposesDefaultWet` | v32 parked insertion: a node added bypassed must never be heard at its default wet before it is configured. |
| `RtNodeMeteringIsAllocationFreeAndNotVacuous` | The worked example of a non-vacuous allocation scenario — see the bullet above. |
| `RtNaNParamIsRejectedAndDoesNotKillTheModule` | Every core's clamp is NaN-transparent (`v < lo` and `v > hi` are both false), so the export must REPORT the rejection AND the module must still be making sound afterwards. |
| `RtPrepareKeepsUserStateAndReDerivesFromTheRate` | Both halves of the `prepare()` mistake: state it must NOT touch (user content) and state it must re-derive (everything the rate owns). Invisible until a device change. |
| `RtRigPrepareCannotRaceTheCallback` | `DspChain::prepare` holds `processMutex_` as well as `writeMutex_`. The detector is the GEOMETRY — see the section above. |
| `RtRetiredSnapshotsDieOnTheOwnerThread` | `dcdsp::RetirementQueue` directly, because namamp is not compiled into this binary. |
| `RtMikuPhraseSwapIsCoherentUnderEdits` | The per-slot seqlock over the Miku phrase tables, driven from a second thread at a rate no callback reaches (33,727 torn reads the moment the packing is removed). |
| `RtPitchCoreGeometrySurvivesALowLatencyFlip` | A buffer sized from a runtime MODE must be ALLOCATED for the worst case: the sizes the audio path masks with must stay inside what was allocated, and the flip must not allocate. |
| `RtCabKernelSwitchDoesNotBurstOneCallback` | The cab's IR kernel rebuild, averaged over many switches. It allocates nothing (so the allocation gate never saw it) and short-circuits at 48 kHz (so the owner's machine never saw it) — on a 44.1 kHz device every model knob tick paid it. Drives the shipping ADAPTER, because the fix lives in the wiring. |
| `RtGlitchCaptureDoesNotBurstOneCallback` | `GlitchCore` directly, footswitch flipped every 4 blocks so ~50 captures can be AVERAGED: a trigger block must cost what its neighbours cost, not a 386 KB memcpy more. |

**Gains, levels and non-finite input:**

| Scenario | One line |
|---|---|
| `RtGainSlidersRampInsteadOfStepping` | Master / monitor / input trim — the artefact, not the implementation. See the section below. |
| `RtSourcePanRampsInsteadOfStepping` | The same detector and the same 30 Hz stimulus for `PanningAudioSource`, which applied two equal-power gains as one constant per block and CUT to unity in a single sample on a drag back to centre. |
| `RtNonFiniteGainCannotLatchTheMasterBus` | Reaches PAST the ABI's `dcabi::finite` refusal and hands `setMasterGain` a NaN directly. The detector is RECOVERY: one wrong block is a click, a bus that never comes back is the bug. |
| `RtNaNFromAnInputPluginCannotLatchTheEq` | Three phases — wire, poisoned block finite, every later block audible again — structurally impossible to pass against the broken code. |
| `RtEqTrimRampsAndSparseInputIsSilent` | Exact trim ramps, JUCE null input pointers, EQ route re-entry. |
| `RtEqSnapshotRetainsReaderOwnership` | Holds the callback slot over a publisher burst; verified non-vacuous both ways (one revert exits `0xC0000005`). |

**Device, transport and host integration:**

| Scenario | One line |
|---|---|
| `RtPreloadedSourceAvoidsTheReadAheadLock` | `BufferingAudioSource` takes `callbackLock` across its decode and the callback takes the same lock — inside JUCE, so NEITHER RT gate can see it. Pins both directions: preload what fits, still PLAY what does not. |
| `RtPreloadedSourceHoldsSixteenBitPcm` | #411: the preload is 16-bit PCM (`Int16PreloadSource.h`), half the float32 it replaced. Pins 2 bytes a sample, a BIT-EXACT round trip for a 16-bit file at the device rate, overs stored with a coarser step instead of clipped, and an audible, allocation-free callback. |
| `RtPreloadedSourceResamplesAtLoadWithoutShiftingTime` | The conversion happens once at load AND its group delay is compensated — uncompensated, the whole song sits ~2.3 ms late against the chart. |
| `RtLoadResamplerUsesThePolyphasePathAndMatchesTheReference` | The offline half: the fast path was TAKEN (a silent fallback fails), the output matches ANALYTIC truth, and it agrees with the windowed-sinc reference. Timings are NOTES, never assertions. |
| `RtPreloadedSourceDownsamplesAtLoadWithoutAliasing` | JuceSharp #33: a 48 kHz file on a 44.1 kHz device is converted AT LOAD by the polyphase kernel at 147:160 — 15 kHz within 0.1 dB of a native 44.1 kHz file, a 21 kHz tone's fold-back (17.1 kHz) and a 23 kHz tone's alias (21.1 kHz) below −80 dB, burst and offline impulse at exactly the reference sample. |
| `RtStreamingSourceResamplesWithThePolyphaseKernel` | JuceSharp #33: a source that missed the preload budget goes through `PolyphaseStreamSource` in both directions — same level/fold-back bounds, zero callback allocations, zero time shift from the start and after a seek, and the stage matches the offline conversion sample for sample across odd block sizes. |
| `RtInstrumentChannelCountFollowsADeviceRebuild` | `reprepareAttachedPlugins` called `prepareToPlay` and stopped: an instrument that came back on a WIDER layout was handed a narrower buffer for the rest of the session. |
| `RtInstrumentAttachDiscardsStaleMidi` | `midiFifo_` is DRAINED, never `reset()`. The race is not reproducible on demand; what is testable is that the discard still discards. |
| `RtStretchQualityIsPerEngineNotPerProcess` | `dca_engine_set_stretch_quality` took a handle and ignored it, so one engine loading stems silently re-voiced another engine's stretchers. No device needed. |
| `RtSoloSeatOnAMultiInputDeviceFollowsItsCaptureChannel` | Four DC inputs at four distinct levels: which number comes out says which channel the rig was filled from, read against the capture ring in the same breath. |
| `RtBridgeWaitIsBounded` | The sandbox bridge's bounded wait. **Flakes intermittently and not because of your change** — see the bullet above, and note the stub needs `setAnswerDelayUs`. |
| `RtXrunCounterIsRealAndQuiet` | The dropout counters over the manual-pump device: real when there is a dropout, quiet when there is not. |
| `RtXrunStatsAbiHonoursStructBytes` | The EXPORT, not the engine method — the `cbSize` contract, which a unit test of `fillXrunStats` can never reach. |

**`DspChain::beginEdit` on a seat with an open scope ABORTS the stale scope and
logs a Warning — it does NOT return false (changed 2026-09-11; no ABI version
change — the behaviour is ABI-neutral, and `DCA_ABI_VERSION` is 35 today).** Refusing the new edit is the
correct answer for a transaction and the wrong one here: `editing_` was cleared
by no other managed call and the rigs are value members of `AudioEngine`, so a
caller who dropped a `JuceDspEditScope` left the seat editing until the ENGINE
was destroyed — every later add / remove / move / bypass piling up in the shadow,
returning success, and never reaching the audio callback again. Refusing the
begin preserved that dead scope forever. It now aborts it, increments
`staleScopeAborts_`, and the log line names the cause; the managed side closes
the same hole from the other end with a finalizer on `JuceDspEditScope`. A
nested begin is still never a nesting LEVEL.

**The v34 engine wave and the v35 fixes** — none of these was in this roster
until now:

| Scenario | One line |
|---|---|
| `RtMasterLimiterHoldsTheCeilingWithoutAllocating` | The master-bus ceiling end to end on a +12 dBFS monitor bus: OFF passes it through, ON holds it, the applied gain moves per SAMPLE, the meter falls back to exactly 0, and the BYPASS EDGE (the callback-side `std::fill` over the delay line) is pumped under the armed hook. |
| `RtMasterLimiterSkipIsCountedAndReportsNoLatency` | The skip branch, driven against `MasterLimiter` directly — a wider channel layout than `prepare()` sized. It must SKIP (never grow), COUNT the skip, and then report ZERO look-ahead, because the block reached the driver and the mirror unbounded. |
| `RtSparseOutputMaskIsPackedAndTheMasterBusKeepsLAndRApart` | JuceSharp #31. The null device opens outputs 3-4 of 8 and the engine's callback records (`DCA_RTCHECK` seam) what it is handed: 2 pointers, none null — JUCE PACKS the enabled outputs, as its WASAPI/ASIO/DirectSound devices do. Every per-channel master-bus stage (HP, LP, BP, song EQ, limiter) reports that one width (`masterBusStageWidths()`); with the HP + a bell band engaged a sine on L only leaves R EXACTLY 0 and a sine on both comes out equal with no block-boundary step; a 4-pointer block on the 2-wide bus is skipped and counted, not folded onto one state; a rebuild to 4 outputs re-derives every width and keeps the HP. Failed 5 checks against the pre-#31 code (limiter 8 wide, the wide block shared bank 1); with the sections mutated to share state 0 the L-only leak, the L != R and the boundary step (0.078 vs 0.0085 inside blocks) all fire. |
| `RtScheduledMidiLandsOnItsSampleWithoutAllocating` | Six phases: four offsets inside one block, an event for the NEXT block not firing early, a late event at sample 0 AND counted, a far-future event parked rather than clamped, the 512-slot parked set SATURATING and counting the drops, and a 32-block storm with the sentinel armed. |
| `RtConcurrentMidiProducersLoseNoEvents` | Two message-thread producers against the SPSC `midiFifo_` while the callback drains it. Delivered + dropped must equal sent — a second producer writes the same slot twice and double-advances the index — and the CONSUMER must stay lock-free. |

**Idle sleep (2026-09-24)** — `dsp-tone-rig.md` § Idle sleep has the design:

| Scenario | One line |
|---|---|
| `RtSpilloverTailRingsAfterRemovalWithoutAllocating` | ABI v37 spillover end to end on the manual-pump device: OFF (the engine default) a removed feedback delay leaves silence; ON it is still ringing on the output with the input silent, the post-chain tail pass allocates and frees NOTHING over 32 armed blocks, the tail finishes and is RETIRED by a message-thread publish (`dspSpillSlotCountForTest` back to 0), a fifth tail makes the oldest fade (4 ringing), and switching spillover off fades every tail out. Proved non-vacuous by making `spillEligibleLocked` answer false (4 assertions fail). |
| `RtInputInjectionReplacesTheSeatInputWithoutAllocating` | ABI v37 re-amp: a loaded take REPLACES seat 0's input (a paused take mutes the live stimulus; playing, the capture ring is exactly the take), the injection render allocates and frees nothing over 256 armed blocks, a looping 1000-frame take is at `played % 1000`, a replaced take is freed on the control thread and a one-shot take stops and rewinds at its end, and clearing it gives the live input back. Proved non-vacuous by making `renderInputInjection` always answer "live input" (5 assertions fail). |
| `RtChartSourceFeedsTheSeatRigNotTheCaptureRing` | ABI v38 chart source on the manual-pump device, the device input left SILENT: twelve notes (a slide, a bend, vibrato + tremolo) follow a playing song clock into seat 0's rig and reach the output; the seat's capture ring, drained through the streaming cursor after EVERY block, stays exactly 0 (so detection, scoring and the tuner never hear it); rendering, a seek inside a held note (chased) and a table swap allocate and free nothing over 400 armed blocks; disabling silences it; the in-process instrument routed to seat 0 is NOT on the master (monitor 0 = silence) and IS on the seat's monitor, and route -1 puts it back; a destroyed clock source unbinds itself; a device with NO input still runs a generator seat. Proved non-vacuous by writing the chain buffer into the capture ring after the generators and by letting the routed instrument also reach the master (3 assertions fail). The scheduler's clock mapping is its own ctest, `chart_scheduler` (`tests/synth/`), proved by four mutations (no chase, no seek generation, naive per-block window, no boundary epsilon). |
| `RtMidiOutputRetriesARefusedSendAsJuce8Did` | JUCE 9 port (2026-09-28), not an audio-thread scenario — `MidiOutputDevice.h` is compiled into this binary and nowhere else testable. JUCE 9's Win32 `juce::MidiOutput` dropped 8.x's `MIDIERR_NOTREADY` (50 x 1 ms) and `MMSYSERR_ALLOCATED` (4 x 100 ms) retries; `sendWithRetry` / `openWithRetry` put them back. Pins both bounds, that an accepted-first-time send never pauses, that a send refused 50 times reports a refusal (the dispatcher counts it `failed`), and the two ways the dispatcher cuts retries short: a single try while the driver is already refusing (an unplugged interface costs 50 ms once, not per message) and none once it is asked to exit. |
| `RtRigIdleSleepSkipsSilenceAndWakesOnThePick` | An owner-like in-house rig (gate -> overdrive -> amp -> cab -> eq -> gain) through pluck -> silence -> pluck with the sentinel ARMED: asleep after the 5 s hold and not before, exact zeros while asleep, the waking block processed and everything after it `==` a never-slept engine, a param write / monitor move wakes it and restarts the hold, a tremolo keeps it awake, an offline render of a sleeping seat equals a never-slept one's. |

**`Fixture` switches idle sleep OFF** (`AudioEngine::setRigIdleSleepForTest`;
pass `rigIdleSleep = true` to opt in). Most scenarios pump silence into a rig
for longer than the hold; with sleep on they would stop exercising the very
modules they exist to check, and pass for the wrong reason.

Counted 2026-09-24 in the rig-idle worktree: **72 `run(`/`wanted(` registrations**
(`Grep` for `run\("Rt|wanted\("Rt`), and 75 lines in the summary with
`RtRunListOrderIsPinned` and the two meta-assertions. The roster heading above
is older than that; re-count before quoting either.

**`RtReverbGearIsAllocationFreeUnderAnIrSwapAndASpaceSwitch`'s convreverb race
renders until 100 rebuilds have overlapped it** (at least 1,500 blocks), via
`gatherEvidence` (next section). Its publisher paces itself with a 200 us
`sleep_for` whose real length depends on the timer resolution earlier scenarios
leave behind, so the fixed 1,500-block window it had collected 34-38 rebuilds
in some full runs and 126-128 on a loaded box, against a floor of 100.

**Meta-assertions** — accumulated across the whole run, so they are SKIPPED
under `--only` and are not part of the `run(...)` list:

| Scenario | One line |
|---|---|
| `RtLockSiteSetIsUnchanged` | The set of `DCA_RT_LOCK_GUARD` sites the callback actually took, against `tests/rt/rt_lock_sites.txt`. |
| `RtBaselineGate` | Every real-time violation seen, against the curated wildcard-free `tests/rt/rt_baseline.txt` (one line today). |

### A stretcher that has run stays on until a seek (JuceSharp #32, 2026-10-12)

`RubberBandAudioSource` (song and stem-group stretcher) has a unity fast path that
skips Rubber Band. Leaving it for the fast path mid-song SKIPPED the stretcher's
read-ahead (the inner is read that far ahead of the listener), coming back REPLAYED
whatever it held from last time, and a seek never reached it at all. Rules:

- **`stretching_` latches.** Once the stretcher has consumed the inner it renders
  every block, at ratio 1.0 too, until a reset. Only a CLEAN stretcher takes the
  fast path. Rewinding the inner instead is a seek on the audio thread
  (`AudioTransportSource` / `ResamplingAudioSource` / `BufferingAudioSource` locks);
  draining it with a final `process()` flushes zero padding, a dropout.
- **A reset happens on the CONTROL thread, under `stretcherMutex_`, in one step with
  the move** (`repositionInner`, from `TransportSource::seekSeconds` / `stop`,
  `setBypass`, `AudioEngine::resetStemGroupStretcher`). Never on the audio thread:
  **R3's `reset()` allocates** (`BinClassifier::reset`) — the gate caught it.
- **A clean start is padded**: `getPreferredStartPad()` zeros in, `getStartDelay()`
  output dropped. Cold, R3 opened with ~27 ms of silence and a fade.
- **R2 (Faster) is prepared at time ratio 4.0** (the 0.25x speed floor), because it
  grows its output ring on the first slower ratio — on the callback, 2 x 320 KB.
  A host going below 0.25x would allocate again.
- Known, Rubber Band's own: R3 entering ratio 1.0 phase-resets band by band from
  16 kHz down (`Guide::updateForUnity`), a ~10 ms dip when the sweep crosses a
  pure tone (~0.2 s after the edge at 430 Hz); R2 dips to a third on a chirp in
  steady state.

| Scenario | One line |
|---|---|
| `RtSpeedCrossingUnityIsSeamlessFiner` / `…Faster` / `…StemGroup` | A 16 s chirp (frequency = song time) through 100 -> 80 -> 100 -> 80 %, a seek while slowed, back to 100 %, a seek at 100 %. Each speed edge must move the song by what the speeds account for (+-30 ms; the old code: +68..81 ms skip, -81..-83 ms replay), a seek must play the target (old: 4.6 s-old audio), no dropout after an edge (old R3 cold start: rms 0), read-ahead 0 after the last seek, zero callback allocations (old R2: 4). The positions are FFT-peak readings fitted over +-0.1 s; zero crossings are thrown by one phase step. |

### The file->device rate conversion happens ONCE, offline, at load

`juce::ResamplingAudioSource` -- what `AudioTransportSource` uses when it is
given a `sourceSampleRateToCorrectFor` -- is LINEAR interpolation plus a single
2nd-order Butterworth (`juce_ResamplingAudioSource.cpp`: `*dst++ = src[pos] +
alpha * (src[next] - src[pos])`, then `createLowPass(ratio)`). A 44.1 kHz
backing track on a 48 kHz interface is the ordinary case -- backing tracks are
44.1 kHz, guitar interfaces run 48 kHz -- and measured end to end on the null
device it was losing

    1 kHz -0.01 dB | 5 kHz -0.37 | 10 kHz -1.46 | 15 kHz -3.14 | 19 kHz -5.56

against the same tone in a 48 kHz file. Every 44.1 kHz backing track was dulled
by 3 dB at 15 kHz, for the life of the project.

Because the whole file is already decoded into RAM on the LOAD thread,
`tryPreloadIntoMemory` converts it there, once, and the transport is told the
buffer is already at the device rate -- so its own resampler runs at ratio 1.0,
where `alpha` is exactly 0 and it is a bit-exact passthrough.

### ...and it is a RATIONAL POLYPHASE FIR, not a per-sample windowed sinc

The first version of that conversion used a 100-crossing
`juce::WindowedSincInterpolator` and measured **-0.088 dB** at 15 kHz -- the
right quality at the wrong price. Owner's log, v0.48.5: **every
`source_open_file` preload of a 183 s stereo 44.1 kHz file took ~6.4 s**, and a
6-stem project loads them sequentially, so ~39 s of a load was this one
function. JUCE's kernel is 201 taps per output sample, each tap two lookup-table
loads plus a linear interpolation between them, inside a loop carrying a running
sinc position, a sign and a wrapping circular index: it cannot vectorise, and at
a FIXED rate ratio it recomputes the same 201 coefficients for every output
sample even though only L distinct coefficient sets exist in the whole file.

`src/LoadResampler.h` exploits exactly that. 44.1 -> 48 kHz is **147:160
EXACTLY**, so the prototype (one 100-crossing windowed sinc at 160x the input
rate, Kaiser beta 8.6, cutoff 0.99 x the input Nyquist -- the same 0.99 JUCE's
own table zeroes at) is built once per load and split into 160 phases of 201
taps. Each output sample is then ONE forward dot product over contiguous memory,
4-wide SSE (the DLL stays on the SSE2 baseline for distribution, so no `/arch`
bump), and because every output sample is independent the work is chunked across
up to 4 threads -- on the LOAD thread, which may allocate and may spawn threads.
**Nothing in that header is ever reached from the callback.**

Measured on the owner's case, a 183 s stereo 44.1 kHz buffer:

    windowed sinc  6,129 ms | polyphase  104 ms (4 threads)  ->  x59
                            | polyphase  340 ms (1 thread)   ->  x18

and the quality went UP, not sideways: 15 kHz is now **-0.000319 dB** against a
native 48 kHz file (measured end to end on the null device), and a resampled
1 kHz / 15 kHz sine is within **2e-6** of the ANALYTIC sine at 48 kHz.

Three things are load-bearing:

- **THE GROUP DELAY IS ZERO BY CONSTRUCTION on this path.** `kTapsPerPhase` is
  even, so the prototype's centre lands on a whole multiple of L, i.e. a whole
  number of INPUT samples, and output sample `n` reads input time exactly
  `n*M/L`. There is nothing to trim. The fallback still trims by hand, and its
  trim is an INTEGER (`llround(100 * 48000 / 44100)` = 109), which is why it
  carries up to half an output sample of residual shift that this path does not
  -- the reference comparison in the gate is bounded by exactly that figure.
- **THE FALLBACK IS SILENT.** A non-integer rate, or a ratio needing more than
  `kMaxPhases` (1024) phases, goes back to `resampleBufferWindowedSinc`, which
  still sounds right and costs 6.4 s. Nothing audible distinguishes the two, so
  the gate asserts the report says `polyphase`, not just that the output is good.
- **BOTH DIRECTIONS (JuceSharp #33).** Until #33 a downsample was not converted
  offline at all (both guards refused it, W4-N16) and played through JUCE's
  linear converter: −2.89 dB at 15 kHz for a 48 kHz file on a 44.1 kHz device,
  and it ALIASED — a 21 kHz tone's image folded back to 17.1 kHz at −10.9 dB, a
  23 kHz tone aliased to 21.1 kHz at −19.1 dB (measured on the null device with
  the new scenario against the old code). Nothing about the polyphase
  derivation assumed M < L; only the cutoff did. The cutoff is now
  `kCutoffScale × min(1, L/M)` × the input Nyquist (`cutoffFor`), and the
  window's half-width `halfTapsFor(L, M) = ceil(100 × M/L)` input samples, so a
  downsample keeps ~100 zero crossings at the OUTPUT rate (48→44.1: 109, i.e.
  219 taps per phase). An upsample's kernel is bit-identical to before. The
  phase walk carries `m / l` whole input samples per output. Measured: 15 kHz
  +0.0003 dB, fold-back −102 dB, 23 kHz alias −106 dB, burst offset 0.
  `dca_convert_audio_for_device` stays upsample-only (`loadWillUpsample`): it
  writes nothing for a file at or above the device rate, by contract.
- **STREAMING SOURCES GET THE SAME KERNEL (`src/PolyphaseStreamSource.h`).** A
  source that misses the preload budget used to reach `ResamplingAudioSource`
  in EITHER direction (−3.45 dB at 15 kHz streaming 44.1→48). `TransportSource::
  tryWireStreamingResampler` now builds the `BufferingAudioSource` itself (the
  transport's own parameters: 32768 frames, the engine read-ahead thread, two
  channels), puts the stage on top and tells the transport the DEVICE rate, so
  its resampler is a ratio-1.0 passthrough. The stage is RT-safe by
  construction: history sized once at `create()` (load thread), output made in
  ≤`kChunkOut` (512) frame chunks so no block size can outgrow it, no lock of
  its own, the kernel a `shared_ptr<const>` read-only on the callback.
  `setNextReadPosition` / `prepareToPlay` re-anchor it (`resync`) and arrive
  under `stretcherMutex_`, never mid-pull; `getNextReadPosition` is an atomic
  because position polls do not take that lock. Preload priority was the
  alternative and was rejected: the budget is a size budget shared by every
  source, so prioritising one only moves the miss to another and cannot help
  a song longer than the budget.
- **THE CONVERTER IS LOGGED.** `ResampleReport::converter` (+ `fromRate`,
  `toRate`, L:M) names the path every source ended up on, including
  `transport-linear` for a ratio with no rational form, in `logSourceReadPath`.
- **EACH PHASE IS DC-NORMALISED.** Without it every phase has a slightly
  different DC gain, which is an amplitude modulation at the phase-cycle rate
  rather than a harmless overall level error.

Three things that are load-bearing:

- **The windowed-sinc fallback is UPSAMPLE ONLY.** Its JUCE kernel cuts at the
  input Nyquist and cannot follow the output one; a downsample the rational
  path refuses stays on the transport's converter.
- **The group delay is compensated, and that matters more than the top end.** A
  windowed sinc is centred; `getBaseLatency()` is 100 INPUT samples, which is
  109 output samples at 44.1->48. Not compensating would put the whole song
  2.3 ms LATE against the chart. `playbackSampleRate_`, not `fileSampleRate_`,
  is what `setSource` is given -- passing the file's rate after converting would
  resample a second time and play the song at the wrong speed.
- **The budget reserves what is HELD**, i.e. the post-conversion size (+8.8% at
  44.1->48), not the decoded size.

If the DEVICE rate changes after a load, the buffer is at the OLD device rate
and JUCE converts the rest of the way as it always did -- correct, one extra
hop, and only until the next load.

`RtPreloadedSourceResamplesAtLoadWithoutShiftingTime` measures both halves END
TO END through the engine and the null device. Verified non-vacuous three times:
disabling the conversion returns -3.138 dB at 15 kHz and reports the buffer
still held at 44,100 Hz; zeroing the latency compensation puts the burst at
output sample 24109 instead of 24000, exactly the 109 predicted; and narrowing
the polyphase cutoff to 0.60 returns -81.9 dB.

`RtLoadResamplerUsesThePolyphasePathAndMatchesTheReference` is the OFFLINE half
-- no device, no callback -- and it is where the structural speed claim lives.
It asserts the ratio reduction (160:147, 2:1, and the two cases it must REFUSE),
that `ResampleReport::polyphase` is true for 44.1 -> 48, that the output is
within 2e-6 of the analytic sine at 1 kHz and 15 kHz, that a 1.000 s burst lands
at output sample 48000 EXACTLY, that it agrees with the windowed-sinc reference
to within the reference's own half-sample trim rounding, and that the 183 s case
ran on more than one thread. **The timings it prints are notes, never
assertions** -- a wall-clock threshold in the gate flakes the moment two agents
build at once. It runs LAST, with `RtRigPrepareCannotRaceTheCallback` and the
other order-sensitive tail, because several seconds of pure CPU ahead of
`scenarioBridgeWaitBounded` would shift the sandbox child's handshake. Verified
non-vacuous four ways: forcing the fallback (11 checks fire), shifting the
prototype centre by one input sample (analytic, reference and burst fire),
narrowing the cutoff to 0.60 (the 15 kHz analytic check fires), and pinning the
thread count to 1.

### A PRELOADED source holds the decoded buffer and nothing else

Once `wireReaderToTransport` has its `Int16PreloadSource`, the format reader
and the bytes it decoded from are unreachable -- `readerSource_` is null on that
path and `fileSampleRate_` was cached before the decode -- so both are freed.
For `openMemory` (the PSARC pipeline, which hands over decoded 16-bit WAV) the
blob is about the size of the 16-bit preload built from it: measured on the
gate's own 4 s fixture, 750 KB released against 750 KB kept (1500 KB kept while
the preload was float32). For `openFile` it is an open file handle plus Opus /
Vorbis / FLAC decoder state, per stem, for a whole session.

**The preload is 16-bit PCM, not float32 (#411).** `Int16PreloadSource` holds
the decoded, rate-converted song as int16 per channel with ONE scale per source
-- the smallest step that fits the buffer's own peaks, never below 1/32768 (the
step JUCE's 16-bit readers decode with) -- and converts back in
`getNextAudioBlock`, a multiply-copy loop with the same block semantics as the
`juce::MemoryAudioSource` it replaced (channel mapping, end-of-data clearing,
position arithmetic). A 16-bit file at the device rate round-trips BIT-EXACT;
a rate-converted or deeper source is rounded to the nearest step (<= -96 dBFS);
overs get a coarser step instead of a clip. `tryPreloadIntoMemory` still
decodes and resamples in float -- quantising after the conversion avoids a
second rounding -- so a load briefly needs ~3x what the source then holds; the
budget is charged what is HELD (`Int16PreloadSource::bytesFor`). Measured on
a 240 s stereo 44.1 kHz WAV at a 48 kHz device: 87.9 MB -> 43.9 MB per source.
`RtPreloadedSourceHoldsSixteenBitPcm` pins it; verified non-vacuous by
breaking each of its four checks (4-byte samples, a 1/32767 base step, no
headroom for overs, a `std::vector` in the callback) -- each one failed.

**A re-open of one `TransportSource` was a use-after-free, and separately leaked
its preload reservation.** `openFile`/`openMemory` rebuilt `panner_` while
`mixer_` still held the raw pointer `addSource` gave it (`attached_` was already
true, so `addSource` was skipped and never re-run), and `openMemory` overwrote
`memoryBytes_` out from under the previous reader's `MemoryInputStream`.
`preloadedBytes_` was likewise overwritten with no release, so every re-open
leaked a source's worth of the engine-wide budget until the engine died -- and
an exhausted budget puts every later source back on the JUCE read-ahead lock,
i.e. back on the 503 ms callback stall preloading exists to remove.
`releaseWiring()` is now the single teardown, shared with the destructor, and
runs before either open touches `reader_`/`memoryBytes_`. Nothing in the ABI
re-opens a source today (`dca_source_open_*` always makes a fresh one) -- it was
a live trap, not a live bug.

`RtPreloadedSourceAvoidsTheReadAheadLock` covers all of it. The budget half is
sized deliberately: it squeezes the engine budget down to HALF a source before
re-opening, because a leak of 1.5 MB against the 256 MB floor is otherwise
invisible, and at that margin a re-open that releases first passes while one
that leaks fails on the first iteration. Verified non-vacuous by reverting each
half: without the frees, two assertions go red naming the retained reader and
768,104 retained bytes; without `releaseWiring`, the gate does not merely fail,
it **exits 0xC0000005** on the play+pump after the re-open -- the dangling
panner, caught live.

### NOTHING escapes the driver callback (W4-N1, 2026-09-11)

An exception out of the audio callback unwinds through JUCE's ASIO/WASAPI frame
into the driver's, with no catch anywhere: undefined behaviour, and in practice
`std::terminate`. It is not theoretical — `NullAudioIODevice::renderOneBlock`
is `noexcept`, so the RT gate itself dies on one. **A process kill, not a
dropped block.**

Everything this engine runs on the callback is its own code and cannot throw,
**except the third-party plugins, of which there are three call sites.** The
engine has always stated the rule and applied it at two of them
(`renderInstrumentBus`, and `reprepareAttachedPlugins` for `prepareToPlay`).
`processInputChain` — the path every amp-sim user is on — called `processBlock`
bare for the life of the feature, and `audioDeviceIOCallbackWithContext` had no
backstop either.

Both halves are now there and both must stay:

- **Per-stage, and it BREAKS rather than continues.** A chain whose stage N
  threw has left the buffer in whatever state that stage reached; running stage
  N+1 over half-processed audio is a second fault on top of the first.
- **A catch-all around the whole callback body**, which is what makes "nothing
  escapes" a property of the CLASS rather than of a list of call sites somebody
  has to keep complete. The override delegates to a private `renderBlock()` so
  the `try` is one frame and not a 480-line reindent. It must stay a bare
  `catch (...)` with an EMPTY body — logging, allocating or rethrowing on the
  audio thread are each worse than the block of silence it produces. Measured:
  it fires **zero** times across the whole gate, so it is not masking anything.

`RtAThrowingInputPluginCannotKillTheCallback` attaches two input stubs and makes
the first throw on one block. With both guards removed the gate **process dies**
(exit 3, the remaining output lost to the stdout buffer — an `abort()` planted
in the backstop is how the throw was confirmed to be what reaches it). There is
nothing to assert about a `std::terminate`, which is what makes it non-vacuous.
`NativeCallbackGuardLintTests` covers what a scenario cannot: it sweeps EVERY
`->processBlock(` call site in `AudioEngine.{h,cpp}` for an enclosing
`try`/`catch`, so a fourth site added tomorrow fails the gate instead of being
invisible until a user's plugin misbehaves.

The same lint carries the **device-pointer rule** — `getCurrentAudioDevice()`
may appear only at the four sanctioned sites (`start()`'s open check,
`showAsioControlPanel`, and the two post-swap checks inside `reconfigure`'s and
`set_device_type`'s `callSync` bodies), everything else takes
`deviceSnapshot()`. `dca_engine_show_asio_panel` was neither: it dereferenced
the pointer from the CALLER's thread and held it across a MODAL vendor dialog,
the longest window any reader of that pointer has, while a concurrent
reconfigure is free to delete it (W4-N5). It is marshalled through
`JuceHost::callSync` now — which serialises it against both mutators — and
`guard()`ed, **with the catch INSIDE the callSync body**, because callSync runs
the body on the message thread and a `guard()` outside would never see the
throw. That is the shape `dca_engine_start` already used.

### A user gain is RAMPED across the block, never applied as a constant

`AudioEngine::rampOrScale` / `rampOrScaleAdd` (AudioEngine.h) are the only way
the master volume, the per-seat monitor level and the input trim reach the
signal. **They are not the only RAMPING SITE in the engine, and this section
said they were until 2026-09-11.** A per-SOURCE level ramps where the source
lives: `PanningAudioSource::getNextAudioBlock`
(`src/JuceSharp.Native/src/PanningAudioSource.h`) ramps its two equal-power pan
gains with `juce::AudioBuffer::applyGainRamp` from the previous block's applied
value, falling back to a flat `applyGain` only when the gain did not move, and
priming `appliedL_`/`appliedR_` on the first block so the source does not fade
up from silence. Its own comment records that it "was the last user-facing level
that still stepped". It also used to `return` early for `|pan| < 1e-4`, which
turned a drag back to centre into a one-sample CUT from the panned gains to
unity; the shortcut survives without the discontinuity. Pinned by
`RtSourcePanRampsInsteadOfStepping`, which uses the same detector and the same
30 Hz stimulus as the master-bus scenario below.
**The centre gains are NOT exactly 1.0f, and this section said they were until
2026-09-11** (W4-N20). `RtSourcePanCentreIsUnity` computes the shipping
expression and reports **0x3F7FFFFF for both channels — one ulp BELOW unity** —
so the `!= 1.0f` skip that claim justified never fired at centre at all, and a
centred source paid a full `applyGain` pass per block. Benign, but it means the
shortcut did not exist. `kUnityEps = 1e-6` (0.0000087 dB) restores it; the RAMP
comparisons stay exact. The scenario asserts the measured fact in the direction
it is true, so a toolchain change that DID make it exact surfaces rather than
quietly making the epsilon pointless.
**A new user-facing level belongs in whichever of the two mechanisms owns its
signal — do not route a source gain through `rampOrScale`.** Each of those is a slider: it publishes a new value per UI tick, and
one constant multiply per block steps the waveform at every block boundary --
the classic zipper, worst exactly when the drag is fastest. Muting the monitor
was worse than a zipper: the old guard (`if (pmon > 0.0f)`) skipped the mix
outright, which is a full-amplitude cut in one sample.

**The INSTRUMENT gain was the one nobody swept** (W4-N3, 2026-09-11).
`dca_engine_set_instrument_gain` is a user-facing fader like the three above and
it carried BOTH forbidden shapes, on BOTH legs — the in-process VSTi
(`AudioEngine.cpp`, `renderInstrumentBus`) and the sandbox child
(`AudioEngine.h`, which shares the member): a per-block constant
multiply-and-add with an `if (g <= 0.0f) return;` in front of it, i.e. the
identical mute cliff the monitor mix 600 lines above documents having FIXED.
`grep instrumentGain tests/rt/rt_main.cpp` returned nothing. It goes through
`rampOrScaleAdd` now, against a new audio-thread-only `instrumentGainCurrent_`
reseeded in `prepareMasterBus` beside `masterGainCurrent_`, and the early-out is
`g <= 0 && prev <= 0` so the block that ARRIVES at zero still ramps through it.
`RtGainSlidersRampInsteadOfStepping` gained a third phase for it, driven by a
new `StubToneInstrument` rendering a continuous 30 Hz sine from its own running
phase — **not the 187.5 Hz the monitor phase uses**, and that is load-bearing:
at 187.5 Hz the signal's own slew is 0.0061 against a 0.0125 gain step, which
sails under the 3x bound, and the probe was measured PASSING with the fix
reverted until the frequency was changed. At 30 Hz, reverted: drag jump 0.012514
against a 0.000982 slew (12.7x) and mute-block jump 0.119577 (122x), both red.

The `from == to` arm is the pre-existing vectorised multiply (and nothing at
all at unity), so a user who is not touching a slider pays what they always
paid. Reseeded in `prepareMasterBus`, so a device change starts AT the
published gains instead of fading up to them.

`RtGainSlidersRampInsteadOfStepping` measures the artefact rather than the
implementation: the largest sample-to-sample jump across block boundaries
during a 20-block drag must stay under 3x the same signal's own slew. Two
details are what make it mean anything, and both were found by watching it pass
against broken code:

- **A 30 Hz song sine.** Its own slew (`A*2*pi*f/fs`) is 0.0012, so a 0.05 gain
  step stands out as 0.015 instead of hiding inside the signal's slope.
- **The monitor stimulus is 187.5 Hz -- one whole cycle per 256-frame block --
  with a `pi/2` PHASE OFFSET.** Whole-cycle keeps the endlessly replayed
  stimulus block continuous (at 220 Hz the input itself jumps 0.22 every block
  and the probe is meaningless); the offset then puts the boundary on the PEAK.
  Without it the boundary sits on a zero crossing, where a hard cut to silence
  is a jump of 0.006 -- and the probe passed the cut it exists to catch.

Verified non-vacuous by forcing both helpers down the constant arm: master drag
0.001165 -> 0.014978, monitor mute 0.004980 -> 0.249925, both red. Restored.

#### The same rule INSIDE a core, and how it is measured there

Thirty user-facing mixes and output levels across twenty-four `src/dsp/cores/*.h`
headers were one constant per block until 2026-08-29 (`octaver`'s three levels,
`subharmonic`'s three, `feedbacker` mix, `freeze` level, `reverb` mix+width,
`platereverb` mix+width, `springreverb`, `shimmer`, `harmonizer`, `whammy`,
`tapedelay`, `wah`, `autowah`, `cab`, `envfilter`, `filterseq`, `formant`
amount, `acousticsim`, `resonator`, `glitch`, `miku`, `mikustomp`, `tapestop`
mix, `harmtremolo` depth). Each now walks a `SmoothedValue` over 10 ms.

Two things that are load-bearing:

- **`SmoothedValue::next()` composes with a SAMPLE-outer loop only.** In a
  channel-outer core the inner sample loop would walk the same smoother once per
  channel: the ramp runs numChannels times too fast, and each channel is handed a
  different gain over the same block, which tilts the image while the knob moves.
  `blockStart(n, step)` + `advanceBlock(n)` hand every channel ONE line and
  advance once, after the loop (`cab`, `acousticsim`, `springreverb`, `shimmer`,
  `octaver`, `resonator` use it). A ramp with fewer than `n` steps left is
  stretched to land at the block end rather than overshooting -- at most one
  block longer than configured, never a step. `dcdsp_eq.h` and `dcdsp_graphiceq.h`
  hand-rolled the same line before it was shared and keep their own version, with
  a comment saying why; do not "fix" those into `SmoothedValue`.
- **A mix cannot be measured the way a gain is.** `DspGainZipperTests.Cases`
  needs the parameter to raise the RMS by >2.5x; a dry/wet mix on a steady tone
  often does not change the RMS at all. `MixCases` therefore renders the same
  stimulus THREE times per case -- parameter never moved, parameter moved from
  the start, parameter moved at the seam -- and asserts the seam run is still
  within 50 % of the OLD render 128 samples after the change and within 15 % of
  the NEW one 1,200 samples later. Every parameter in the table feeds only the
  output blend, never the core's internal state, which is exactly what makes the
  "moved from the start" render a faithful stand-in for the old hard switch. Both
  spans are asserted large first, so a case that turns out not to change anything
  fails as vacuous instead of passing (two did on the first run: `tapestop` with
  the brake released reads its wet at the same base delay as its dry, and
  `freeze` snapshots a 40 ms pre-roll out of a ring that starts silent -- hence
  `PreRoll = 5` blocks and the per-case `arm` column).

Verified non-vacuous by reverting `dcdsp_octaver.h` to its pre-change form and
rebuilding: exactly its three cases went red, at 100 % / 76 % / 64 % of the way
to the new setting (the two sub levels come in under 100 % because the tone
low-pass and DC blocker smear the step, which is why the bound is 50 % and not
90 %). Restored.

**Feedback coefficients were deliberately left alone** -- `springreverb` decay,
`tapedelay`/`reversedelay` feedback, `shimmer` decay, `feedbacker` amount,
`resonator` decay. A loop gain is not a per-sample multiply on the output: a step
changes how fast a tail decays, which is audible over a tail but is never a
sample-to-sample discontinuity, and ramping one changes the stability argument
each of those cores documents.

### A capture is drained, not memcpy'd in one callback

`RtGlitchCaptureDoesNotBurstOneCallback` drives `GlitchCore` directly (no engine,
no device, so the numbers are pure CPU): 64-frame stereo blocks at the 1000 ms
maximum slice, the footswitch flipped every four blocks so ~60 captures land
across 480 blocks and the two populations can be AVERAGED — a single timing is
noise on a shared box, a mean over sixty is not. It asserts the capture blocks
cost under 5x their neighbours, that at least twenty captures actually fired,
that the stutter's output energy is plausible against the dry (a core doing
nothing would post a perfect ratio), and that nothing allocated.

The hazard was invisible to every existing gate because it is neither an
allocation nor a lock, and because `RtEveryCatalogueModuleIsAllocationFree`
drives each module at its DEFAULTS — glitch's are Manual mode with the trigger
off, so the capture path never ran there at all. **A module whose expensive path
is behind a footswitch is not covered by the catalogue sweep.**

Measured: 759x before the incremental rewrite (1.386 ms in one callback), 1.13x
after. That IS the non-vacuity proof — `git show HEAD:...dcdsp_glitch.h` back
into place, rebuild, watch it fail, restore.

### The dropout / xrun counter is gated by TWO scenarios

The counter (`dca_engine_get_xrun_stats`, see `native-engine-abi.md`) has more
ways to pass while measuring nothing than most things here, so it gets one
scenario for the DETECTOR and one for the EXPORT.

**`RtXrunCounterIsRealAndQuiet`** — the manual-pump device is what makes this
testable at all: the callback runs on the test's own thread, so the "gap between
callbacks" the detector measures is simply the wall-clock time between two
`pump()` calls, and a real stall can be synthesised with a `sleep`. Three
phases, one per way of being vacuous:

- **Phase A (no false positives)** — a back-to-back pump loop must count ZERO
  late callbacks. A detector that fires on ordinary jitter is worse than none.
  Measured: 320 callbacks, 0 late, 0 deadline overruns, 0 near misses, budget
  5,333,333 ns (256 frames at 48 kHz), last callback ~3 µs. Also runs with the
  allocation sentinel armed, so the counters themselves are proven allocation
  free.
- **Phase B (not vacuous)** — a 40 ms gap between two callbacks, about seven
  missed periods, MUST be counted. Measured: 1 late callback, 1,696–1,895
  dropped frames, `last_xrun_render_frame` set.
- **Phase C (the warm-up guard, not luck)** — reopen the device at a new block
  size across a deliberate 30 ms stall and demand ZERO. This is the device
  change / sample-rate change / watchdog recovery case, and without
  `resetXrunSession()` every one of them would report a phantom dropout.

Verified non-vacuous the way this repo requires: setting `kXrunWarmupBlocks` to
a number the run can never reach makes phase B fail with *"a 40 ms gap between
two callbacks was not counted as a dropout — the detector is vacuous"*, then it
was restored.

**`RtXrunStatsAbiHonoursStructBytes`** — opens a real `dca_engine_t` on the
PACED null device (no hardware, no stolen sound card) and drives the shipping
exports. It proves the `struct_bytes` cbSize contract in the only way that
matters: a short `struct_bytes` must fill the declared prefix and leave a 0xAB
guard band past the declared end **completely untouched** — the contract is a
compatibility mechanism, and the failure mode if it is wrong is a buffer
overrun in the caller. Also covers the null-handle, null-out and
undersized-`struct_bytes` rejections, that the null device honestly reports
`device_xruns == -1`, and that `dca_engine_reset_xrun_stats` actually resets.

- **A sub-millisecond `sleep_for` in a test stub is a coin flip, and the coin
  is someone else's process.** Windows rounds a short sleep up to the timer
  resolution: 1 ms while ANY process holds `timeBeginPeriod(1)` (Rider, a
  browser, the app itself with ASIO open), ~15.6 ms on a quiet box.
  `RtBridgeWaitIsBounded` phase C gave its stub child a 300 µs answer delay
  via `sleep_for`; on a quiet box that became ~15 ms, longer than the 10.7 ms
  block budget, so BOTH of the bridge's `kRetrySyncEveryBlocks` retries timed
  out and the scenario reported "only 2 waits over 600 blocks" — five runs on
  2026-08-29, deterministic, while the same binary passed whenever the owner's
  IDE or the app was running. The stub spins on `QueryPerformanceCounter` now
  (374 waits, twice). A test that models a real child's latency must not
  borrow the scheduler's granularity to do it.

## Tone snapshot and teardown regressions

`RtEqSnapshotRetainsReaderOwnership` holds the callback slot over a publisher
burst. `RtEqTrimRampsAndSparseInputIsSilent` checks exact trim ramps, JUCE null
input pointers, and EQ route re-entry. `RtStagedInsertionNeverExposesDefaultWet`
checks the dry configuration window and ordinary offline insertion compatibility.

`juce_sharp_nam_lifetime` is a separate release-CRT executable linked to the real
NeuralAudio loader; it pauses after capture/resampler preparation to interleave
clear and superseding requests before publication. Its synchronization hooks are
compiled only into that test target. This does not close the NAM callback
allocation-sentinel gap: the main RT gate still excludes the neural module.

`juce_sharp_rig_idle_nam` (2026-09-24) is the other release-CRT executable: it
recompiles the WHOLE engine in the shipping configuration (NeuralAudio, the
AVX2 flag included) and drives the manual-pump null device, so the one module
the RT gate cannot load — `namamp` — is covered for idle sleep: slept vs
never-slept vs a later pick on a real capture (ctest runs it on NeuralAudio's
BossLSTM; pass the owner's captures by hand). It asserts sound, not
allocation: the sentinel gap above is unchanged. `--bench` times the callback.

`NativeDestructionTests` separates claiming a child handle from completion of
its native destruction. The engine retains completion tokens, joins concurrent
explicit child disposal, and leaks its native parent when a child drain times
out or destruction fails. Finalization only destroys the parent if every child
already completed; it never blocks the finalizer queue waiting for another
finalizer. A collected wrapper cannot erase its incomplete token.

Failed plugin/MIDI destructors retain their wrapper deliberately: leaked native
objects may still own their callback function pointers. Successful destruction
adds no permanent root. The engine log trampoline is already process-static;
engine leak paths leave its registration intact and free no callback GCHandle.
