# Stem separation, child processes and the decoder

Deep reference for `JuceSharp/AGENTS.md`. `JuceSharp.StemSeparation/` + `.Onnx/`
(the native demucs.cpp path and the ONNX one), `JuceSharp.AudioIo/` and
`JuceSharp.Diagnostics/`. The app-side policy (the never-regenerated `.dcstems`
cache, the resource policy, the cache key) is
`docs/agents/services/stem-separation.md` in the DotCharter repo.

- `JuceSharp.StemSeparation/` + `.Onnx/` — stem-separation backends (native
  demucs.cpp path + ONNX).
  - **The native demucs.cpp build is OFF by default**
    (`DOTCHARTER_STEMS_DEMUCS_CPP` in `src/JuceSharp.Native/CMakeLists.txt`) —
    with it off the `dca_stems_*` C ABI reports "not supported" and the managed
    side falls back to the Python `demucs` CLI service.
  - **`DEMUCS_CPP_TAG` is PINNED to a 40-hex commit SHA of sevagh/demucs.cpp
    (`f1206e9adeea103aef4a636b9e62297cf1f8e34e`, upstream `main` as of
    2026-08-29 via `git ls-remote`; the repo has no released tags).** It
    defaulted to the mutable ref `"main"` until then, so a stems build was not
    reproducible. CMake still `message(WARNING ...)`s on `main|master|HEAD`, and
    the DotCharter gate's `Infrastructure/DemucsCppPinLintTests` fails on any
    default that is not a 40-hex SHA or a `v*` tag. Bump the SHA deliberately;
    never put a branch name back.
  - **pip is invoked through `ProcessStartInfo.ArgumentList`, one element per
    argument** (`PythonEnvManager.BuildPipInstallArguments`; the pip-wheel
    bootstrap too, `BuildPipBootstrapArguments`). The old `"{pkg}"` string concatenation broke on an embedded
    quote and on a wheel path ending in `\`. Pinned by
    `PythonEnvManagerPipArgumentTests`.
  - **The ONNX runtime is the DirectML build (`Microsoft.ML.OnnxRuntime.DirectML`
    1.24.4, the last release), not CUDA.** Any DirectX 12 GPU, no CUDA/cuDNN
    download; `Auto` resolves DirectML → CUDA (absent here) → CPU
    (`OnnxSessionFactory.FallbackOrder`). Three rules ride on it:
    (1) **every DirectML session takes `OnnxSessionFactory.ApplyDirectMlOptions`**
    — `ep.dml.disable_graph_fusion=1` is a CORRECTNESS setting: with DirectML's
    graph fusion on, htdemucs returns stems 35-65x too loud on every adapter
    (`OnnxDirectMlTests.Htdemucs_OnDirectMl_MatchesTheCpu`, GPU + model gated), and
    the 4 s BS-RoFormer SW export is garbage too (its per-band mask estimators' 62-way
    `Concat` is the node fusion miscomputes). **The ONE exception is a model whose
    metadata says `dml_graph_fusion=verified`** (`OnnxSessionFactory.DirectMlGraphFusionKey`,
    read from the FILE by `OnnxModelMetadata` before the session exists): the SW
    training-chunk export (DotCharter #267), whose bands run as 7 batched groups.
    Never write that key into an export without running `BsRoformerSwDirectMlTests`
    (or the model's equivalent) with fusion ON — the test asserts parity for exactly
    the models that declare it;
    (2) **`DirectML.dll` is shipped by an explicit `None` item** in the `.Onnx`
    csproj — the package's own targets copy it only into a DIRECT referencer, so
    a consumer through a ProjectReference silently loaded the older System32 copy
    (`OnnxDirectMlTests.DirectMlDll_IsDeployedBesideTheConsumer_*`);
    (3) **pass a `deviceId`** (`OnnxGpuAdapters.Enumerate`, DXGI order): adapter 0
    is the display adapter, not the fastest. A GPU session that attaches but
    cannot load the model is rebuilt on the CPU and says why in
    `OnnxStemSeparator.FallbackReason` (`OpenWithCpuFallback`).
  - **`OnnxStemSeparator.Dispose` DRAINS an in-flight `SeparateAsync` before it
    frees the `InferenceSession`** (`InferenceGuard`, the managed twin of
    `NativeHandleOwner`). The pin is taken on the POOL thread, not the caller's:
    the old code checked a disposed flag on the calling thread, queued
    `Task.Run`, and `Dispose` freed the session with nothing waiting for a pool
    thread already inside `session.Run` — a use-after-free of a native ORT
    session for any host that disposes before awaiting (DotCharter's call site
    awaits under a `using`, which is why it never crashed there). A dispose that
    still sees an inference after `DefaultDrainTimeoutMs` LEAKS the session
    rather than freeing it under a live `Run`. Pinned by
    `OnnxStemSeparatorDisposeRaceTests` through the internal runner-injecting
    constructor — no model file involved.
  - **Every ONNX driver runs at 44.1 kHz, and `OnnxStemSeparator` is where the
    input gets there.** htdemucs always resampled inside its driver; BS-RoFormer
    and MDX-Net take "stereo at 44.1 kHz" on trust (neither reads a rate) and
    were fed a 48 kHz session's samples unconverted. Both now go through
    `OnnxStemSeparator.SeparateAtModelRate` (`SpectralModelSampleRate`), which
    Lanczos-resamples in, runs the residual (`mix − stem`) at the MODEL rate, and
    resamples every stem back to exactly the input's frame count. Pinned by
    `SpectralModelResampleTests` — the seam with a fake predictor on a synthetic
    48 kHz tone, plus a source lint that both call sites use it. The way BACK is
    `ResampleStemsBack`, ONE implementation for every driver since 2026-09-11:
    htdemucs had a second copy that had lost the `FitToFrames` correction, so its
    stems could be a frame off the mix (W4-S11, `OnnxStemRateContractTests`).
    Note that `HtdemucsOnnxInference.Predict` is PUBLIC and returns stems at
    44.1 kHz whatever rate it was given — its doc claimed otherwise until W4-S12.
  - **MDX-Net's `n_fft` is 6144, which is not a power of two, and `JuceFft`
    handles that now.** The MathNet→`JuceFft` migration left a radix-2-only
    engine behind a driver whose shipping geometry is 3 × 2048 (dim_f = 3072), so
    every one of the five MDX-Net catalogue rows — `DefaultIds[MdxNet]` included
    — threw on its first chunk, and no test constructed the driver (W4-S1,
    W4-S31). `JuceFft` now runs radix-2, a radix-3 decimation for `3 * 2^k`, or
    Bluestein; `SpectrumPlanes` requires EVEN, not power-of-two. Measured at
    6144: 110 µs radix-3 against 526 µs Bluestein and 55 µs for a 4096 radix-2.
    **Do not "fix" a model geometry by padding it** — that moves the bin
    spacing the model was trained on. `MdxNetGeometryTests` round-trips one
    chunk through EVERY catalogue entry's own geometry.
  - **The sidecar pipe is UTF-8 on BOTH sides, and the child's CWD is NOT
    ours.** `RunStreamingCore` sets `StandardOutput/ErrorEncoding` and
    `PYTHONIOENCODING` + `PYTHONUTF8`; without them CPython encoded stdout with
    the locale ANSI codepage and .NET decoded with `Console.OutputEncoding`, so
    a `STEM <name> <path>` line naming `C:\Users\Müller\…` came back as a path
    that does not exist — reported as a SUCCESSFUL separation (W4-S2). And
    because the child's working directory is forced to the interpreter's folder,
    **every path in the argv is `Path.GetFullPath`ed** — in `BuildArgs` itself,
    so the guarantee belongs to the argv builder (W4-S3). A run that TIMES OUT
    still drains the child's stderr before returning; abandoning it threw away
    the only evidence a hang leaves (W4-S7). `PythonSidecarBoundaryTests`.
  - **`Probe()` answers on POSIX too.** `ResolveLocalExecutable` falls back to
    `ResolveSystemPython3()` there; reading only the field
    `EnsureInterpreterAsync` writes made `IsReady()` — the documented UI gate
    behind a Separate-stems button — permanently false on Linux/macOS with
    python3 installed (W4-S8).
  - **`CkptToOnnxConverter` writes `<dest>.tmp.<guid>` and renames**, like every
    other download door — a killed multi-minute export otherwise leaves a
    truncated `.onnx` at the canonical path and `File.Exists` says yes to it
    (W4-S13). `ConvertCoreAsync` takes the run as a delegate so that is testable
    without a 2 GB torch install.
  - **`ChildProcessRegistry` NEVER owns the `Process` objects it tracks.** It
    kills and untracks; disposal belongs to whoever called `Process.Start`. It
    used to `Dispose()` from its own `Exited` handler — which is subscribed
    inside `Register`, i.e. BEFORE the one `WaitForExitAsync` installs, so it
    always won the race — and the owner's `p.ExitCode` read in
    `PythonEnvManager.RunStreamingCore` then threw
    `InvalidOperationException("No process is associated with this object")`.
    **A SUCCESSFUL python run was therefore reported as a separation failure**,
    and the short-lived `pip --version` probe failed so hard that pip was
    re-bootstrapped on every call. Pinned by
    `ChildProcessRegistryTests.Register_does_not_dispose_the_process_its_caller_still_owns`.
    Untrack matches on VALUE (pids are recycled), and `Register` subscribes
    before arming `EnableRaisingEvents` (the other order drops the event for an
    already-exited child).
  - **Inside DotCharter the registry's job object is a redundant SECOND layer.**
    The app puts itself into a kill-on-close job
    (`DotCharter.Services/Infrastructure/Process/ChildProcessJob`) and Windows
    children inherit it, so a python child is covered before `Register` runs;
    nesting (Win8+) just makes ours a child job firing at the same moment. Kept
    because JuceSharp is a reusable submodule and a standalone host has no outer
    job. Two asymmetries: ours does NOT set `JOB_OBJECT_LIMIT_BREAKAWAY_OK` while
    the app's does (breakaway is governed by the IMMEDIATE job), and
    `AssignProcessToJobObject` returns `BOOL` rather than throwing — an
    unchecked call let a refused assignment silently disarm the layer. The
    handle is a `Lazy<IntPtr>(ExecutionAndPublication)`, NOT a field written
    inside `EnsureHooks`: `_initialized` was flipped before the handle was
    assigned, so a concurrent `Register` read `IntPtr.Zero` and left its own
    child out of the job (W4-S9). `ChildProcessJobAssignmentTests`.
  - **Progress has a PULL surface and it is one implementation, not three.**
    `JuceSharp/CoalescingProgressStream.Create<T>` (core package) turns a
    `report`-callback operation into an `IAsyncEnumerable<T>`; all three
    backends' `SeparateWithProgressAsync` sit on it —
    `StemSeparator.SeparateWithProgressAsync` (the interface's default
    implementation bridges a push-only third-party backend),
    `OnnxStemSeparator.SeparateWithProgressAsync`,
    `JuceStemSeparation.SeparateWithProgressAsync`. The contract, all of it
    pinned by `CoverageGaps/StemProgressStreamTests` +
    `CoverageGaps/BackendProgressStreamTests`:
    **bounded channel, `FullMode = DropOldest`, depth 32** — publication is a
    `TryWrite` and therefore total (never blocks, never throws, safe from the
    demucs WORKER thread and from the sidecar's stdout reader), so a slow UI
    costs stale samples and never inference throughput; **the TERMINAL item
    carries the whole result and can never be dropped**, because DropOldest
    evicts the oldest and the terminal item is the last write before the
    writer completes — that is what makes coalescing lossless, and it is why
    `StemSeparationProgress` grew a `Result` (and a `Backend`) rather than the
    method returning a `(stream, Task<result>)` pair; **a fault keeps its
    ORIGINAL type**, because the writer is completed WITHOUT an exception and
    the worker's `Task` is awaited after the drain (`Complete(error)` would
    make the observed type depend on whether the reader used `ReadAllAsync`,
    which re-throws as-is, or `ReadAsync`, which wraps in
    `ChannelClosedException`); and **cancelling the token OR abandoning the
    enumeration cancels the work**, surfacing `OperationCanceledException`
    from `MoveNextAsync`. `juce-stems-diag` consumes the stream and its +1 %
    throttle is GONE — a second throttle over a coalescing transport only
    meant two consumers of one backend discarded different samples for
    different reasons. **Never rebuild a stream by wrapping a backend's public
    `IProgress<T>` API with another buffer**; publish into the channel from
    the same core the push path uses.
  - **A cancelled separation throws `OperationCanceledException`, not
    `StemSeparationException`.** Since 2026-09-11 `PythonEnvManager.Run` /
    `RunStreaming` throw `OperationCanceledException(ct)` THEMSELVES after
    killing the child (`Kill(entireProcessTree: true)`), so no caller can turn a
    cancel into an `InvalidOperationException("… stderr: Cancelled.")` again —
    `EnsurePipAsync`, `EnsurePackagesAsync` and `CkptToOnnxConverter.ConvertAsync`
    all did exactly that until then. A TIMEOUT still returns `ok=false` with
    `"Timed out after …"`. `SeparateTrackAsync` keeps its post-run
    `ThrowIfCancellationRequested()` as a backstop.
  - **The embed bootstrap is serialised CROSS-PROCESS** by
    `PythonEnvManager.AcquireEnvLockAsync(root, ct)` — an exclusive
    (`FileShare.None`) handle on `<root>/.bootstrap.lock`
    (`EnvLockFileName`), taken by all three `Ensure*`. It is NOT reentrant:
    `EnsurePackagesAsync` awaits `EnsurePipAsync` before taking it.
    `StemSeparator`'s GPU gate is unrelated. **Never put a named kernel object
    back**: the old `Global\JuceSharp.python-embed.<hash-of-path>` semaphore was
    nameable — and squattable — by any user, and a semaphore is not released
    when its owner dies. The file carries the root's ACL and the OS drops the
    handle on exit. The wait is still bounded by `CrossProcessEnvLockTimeout`
    (30 min) for a LIVE owner that hangs: on expiry the manager logs at Warning
    and proceeds with the process-local gate alone.
    `PythonSupplyChainTests` lints both.
  - **"`python.exe` exists" is NOT "the interpreter is usable".** The extract is
    a per-file copy into `Root`, and `EnsureInterpreterAsync`'s fast path reads
    the result BEFORE taking the gate — so a peer could see `python.exe` land
    and run pip against a runtime with no `python311.zip` and an unpatched
    `._pth`. The all-or-nothing property is the `.interpreter-ready` stamp
    (`InterpreterReadyStampName`), written as the very last step and cleared at
    the start of an extract; `ResolveLocalExecutable` gates on it and adopts a
    pre-stamp root only when the two things an extract lands last are both
    there.
  - **pip runs once per pinned set, not once per separation** — and the stamp
    is checked for TRUTH, not presence. Success writes
    `.packages-<sha256>.stamp` into the root; changing any pin changes the
    hash. The stamp's first line records the interpreter IDENTITY (path + mtime
    + length) and `EnsurePackagesAsync` additionally checks that every pin whose
    site-packages directory is known (`PythonPackagePins.InstalledDirectories`)
    is actually there, deleting the stamp when it disagrees. A deleted
    `Lib\site-packages\torch`, a failed uninstall or an antivirus quarantine
    otherwise meant pip was never run again and every separation failed with a
    Python `ImportError` for ever. A pin NOT in that map is deliberately not
    checked: guessing an import name wrong would re-run pip on every launch,
    which is what the stamp exists to prevent.
    The pins live in exactly one place, `Runtime/PythonPackagePins.cs`.
  - **`StemSeparationOptions.MaxConcurrentSeparations` applies to ONE call.**
    It admits that separation past the standing policy; it does not move
    `StemSeparator.GlobalConcurrencyLimit`. There are two gates: a CAPACITY
    semaphore (grow-only, the physical ceiling) and a POLICY semaphore sized to
    `GlobalConcurrencyLimit`, taken by every caller that did not ask for more
    than the policy, and acquired in that order. One call carrying
    `MaxConcurrentSeparations = 8` used to raise the ceiling irreversibly for
    every future default-options caller — i.e. for exactly the callers the VRAM
    gate exists for.
  - **The PUBLIC door is `Runtime/VerifiedDownload` and it is KIND-LESS.**
    `EnsureFileAsync(Uri, dest, sha256?, expectedBytes?, progress?, log?, ct)`
    + `VerifyFileAsync` + `ComputeSha256Async` + `ValidateSourceUrl`. It
    forwards to `VerifiedDownloader` and adds nothing — the point is that it
    does not ask for an `OnnxStemModelKind`. `OnnxModelCatalog.EnsureAsync`
    calls it ONCE PER MIRROR rather than the downloader directly, so the two
    paths cannot diverge, and `VerifiedDownloadTests` runs every scenario as a
    theory over BOTH doors plus a source lint that `OnnxModelCatalog` no longer
    names `VerifiedDownloader` at all. **Never reach the downloader by
    describing a file as a model kind it is not**: `OnnxModelEntry.Kind` names
    the inference driver, nothing downstream of `EnsureAsync` reads it, so a
    wrong label downloads perfectly and lies to the next reader — DotCharter
    shipped a `NearestKindLabel` helper doing exactly that, and it can go.
    A CATALOGUE adds exactly two things on top: the `allowUnverified` decision
    and the mirror walk — and since 2026-09-11 BOTH catalogues get them from
    ONE implementation, `Runtime/CatalogueDownload.EnsureAsync`, because the two
    `EnsureAsync` bodies had become line-for-line equivalent modulo the entry
    type (W4-S27). The entry RECORDS stay separate (different domain fields,
    different assemblies, public API) and each keeps its own `ValidateEntry`.
    **The two doors disagree about an unpinned fetch on PURPOSE** — a catalogue
    refuses, the one-URL primitive warns (it has no table to consult) — and a host that wants the catalogue's refusal for a single URL passes
    `EnsureFileAsync(..., requireHash: true)` rather than either side changing.
  - **`DemucsGgmlModelCatalog` (`Runtime/`) is the NATIVE backend's table**, the
    ggml twin of `OnnxModelCatalog`, on the same door. Three entries, all
    pinned from `huggingface.co/api/datasets/Retrobear/demucs.cpp/tree/main`'s
    `lfs.oid` (2026-09-11); the 6-source one is corroborated against a hash
    DotCharter computed from an installed copy. **A dataset repo DOES publish
    `lfs.oid`** — under `api/datasets/`, not `api/models/`; believing otherwise
    is what left two of three weights unpinned in the table this replaces. The
    four `_ft_` weights are deliberately absent: demucs.cpp uses that family as
    a set of four. `HashSource` is mandatory for a hash and for the absence of
    one, linted by `DemucsGgmlModelCatalogTests`.
  - **`ValidateEntry` is PUBLIC on both catalogues**, and the transport rule
    (absolute https, or http to LOOPBACK only) lives once in
    `VerifiedDownload.ValidateSourceUrl` — so a second catalogue cannot enforce
    a subtly different one.
  - **Downloads go through `Runtime/VerifiedDownloader`, never an `HttpClient`
    of your own.** It is the ONE place either stems package fetches bytes
    (`PythonEnvManager.DownloadWithProgress` and `VerifyFileHashAsync` are now
    forwarders; `OnnxModelCatalog` calls it directly), because everything these
    packages download is then EXECUTED or loaded as a model, and the
    temp-file → hash-the-COMPLETE-file → atomic-rename order is easy to write
    wrong and invisible when you do. `CoverageGaps/HttpDownloadLintTests` fails
    on any `HttpClient` / `GetAsync` / `SendAsync` / `WebClient` in
    `src/JuceSharp.StemSeparation*/` outside that file, strips comments first,
    matches the BARE TYPE NAME (the sanctioned field is target-typed,
    `HttpClient Http = new()`, which a `new HttpClient` pattern walks straight
    past), and separately asserts the verify-before-rename ORDER. **Two
    temp-file policies, both deliberate:** the Python bootstrap is
    `allowResume: false` → `{dest}.tmp.{guid}`, DELETED on any failure (a
    half-formed interpreter archive is a trap for the next boot, and
    `CleanStaleTempFiles` sweeps that shape — but only files it can open
    EXCLUSIVELY, because the downloader holds its temp file `FileShare.None`
    and a second manager on the same root used to delete the first one's
    in-flight download from its constructor); model downloads are
    `allowResume: true` → **`{dest}.{sha8(url)}.part`** (`PartialPathFor`),
    KEPT on cancel or network failure so the next attempt resumes, deleted only
    on a CHECKSUM failure — **and swept by `VerifiedDownload.CleanPartials`**,
    which the catalogue calls once the real file has landed. Until that existed
    nothing swept them at all and a twice-abandoned 300 MB model left 600 MB of
    invisible cache (W4-S14); `CleanStaleTempFiles` covers the `*.tmp.*` shape
    in the interpreter root only. `EnsureFileAsync` holds the destination gate
    across its EXISTENCE CHECK too, not just the transfer, or a concurrent
    caller could delete the canonical file under another's hash (W4-S17). **The URL digest is load-bearing**: a mirror list is
    a list of SERVERS, and resuming mirror A's prefix into mirror B's body
    concatenates two servers' bytes. A resume sends `Range: bytes=N-` and
    decides what happened from the **status code** (206 vs 200), never from the
    advertised `Accept-Ranges`: a server that ignores Range answers 200 with
    the whole body, and appending that to the prefix corrupts the file
    silently. **The status code is necessary and not sufficient** — it says a
    range was honoured, never WHICH — so since 2026-09-11 a 206 is also checked
    against `Content-Range` (unit `bytes`, `From == alreadyHave`, and not
    `multipart/byteranges`); anything else re-requests WITHOUT a Range header
    instead of appending. A proxy that clamps the range to the whole resource
    used to double the file, and with `Sha256 = null` nothing downstream
    noticed (W4-S6). A 416 retries EXACTLY ONCE with resume suppressed — the old
    same-arguments re-entry recursed to a stack overflow whenever the prefix
    could not be deleted — and a prefix that survives `TryDelete` is an
    `IOException`, not another attempt. A resumed file is verified WHOLE.
  - **A download TIMEOUT is an `IOException`, never an
    `OperationCanceledException`.** `HttpClient.Timeout` covers the content
    reads too and expires as `TaskCanceledException`, which
    `OnnxModelCatalog.EnsureAsync` correctly refuses to treat as a broken
    mirror ("a user Cancel is not a broken mirror") — so a slow first mirror
    permanently prevented the second from ever being tried, and every stem
    consumer read the failure as a user cancel. The budget is
    `VerifiedDownloader.RequestTimeout` on a token linked to the caller's, and
    expiry is translated before it leaves the type. Only the CALLER's token
    produces `OperationCanceledException`.
  - **Length is checked BEFORE the rename, twice.** A clean early close makes
    `ReadAsync` return 0 and the loop exit normally, so a truncated body used
    to be renamed into place and — for any caller without a hash — loaded or
    executed. The declared `Content-Length` is enforced at the end of
    the read loop, and `OnnxModelEntry.SizeBytes` now travels into
    `DownloadAsync` as `expectedSizeBytes` instead of being checked after the
    file was already at the canonical path. The rename itself no longer
    pre-deletes `dest`: `File.Move(overwrite: true)` replaces atomically, while
    the delete opened a window where the destination existed in NEITHER form.
  - **One download per destination.** `VerifiedDownloader.AcquireDestinationGateAsync`
    serializes concurrent downloads of the same file process-wide; the python
    side had `AcquireEnvLockAsync` for this shape and the model side had
    nothing.
  - **`OnnxModelCatalog.EnsureAsync` VALIDATES the entry it is handed.** It is
    public and takes a caller-constructed record, so `FileName` must be a bare
    file name (`..\..\startup.exe` was an arbitrary-path writer into a file
    ONNX Runtime then loads) and every URL must be absolute `https` — plain
    `http` is accepted for LOOPBACK only, which is the test server and not a
    transport an attacker is on.
  - **`OnnxModelCatalog` is the ONNX model table, and every pin records where
    it came from.** `OnnxModelEntry.HashSource` is mandatory — for a hash AND
    for the absence of one — and `CoverageGaps/OnnxModelCatalogTests` fails the
    build otherwise, because a pin nobody can trace is a pin nobody can
    re-check. The nine shipped hashes were read from the Hugging Face
    repository API on 2026-09-11 (an LFS object's `oid` IS its SHA-256); the
    htdemucs-6s one independently matches DotCharter's own 2026-09-04 pin.
    `Sha256 = null` means UNVERIFIED, not "skip the check": `EnsureAsync`
    warns and then THROWS unless the caller passes `allowUnverified: true`. An
    existing file is hashed, never trusted — a mismatch is re-downloaded, since
    "the file exists" ⇒ "the model is installed" is how a half-written model
    becomes a permanent load failure. Do NOT add a hash computed from a copy
    you fetched over the same unverified channel; that proves only that two
    GETs agreed.
  - **Downloads are checksum-verified before the atomic rename.** The 3.11.9
    embed zip is pinned (`PythonEnvManager.DefaultPythonEmbedSha256`) — bump it
    whenever `DefaultPythonVersion` moves.
  - **Everything the Python sidecar downloads is pinned by SHA-256 and fetched
    over https** (`PythonSupplyChainTests`):
    - **pip is a pinned WHEEL, not `get-pip.py`** (`DefaultPipWheelUrl` /
      `DefaultPipWheelSha256`). get-pip.py could not be fixed by pinning it: the
      script installs the LATEST pip (and setuptools/wheel on 3.11) from PyPI
      with no hash. The wheel is verified, then pip installs itself from it with
      `--no-index --no-deps`, run via `-c` with the wheel on `sys.path` (pip
      refuses to modify itself when started as `<wheel>/pip` on Windows). A
      blank or malformed `PipWheelSha256` and a non-https `PipWheelUrl` are
      refused before any transfer.
    - **Packages install from a HASH LOCK**: `PythonScripts/requirements-win_amd64-cp311.lock`,
      embedded as a resource (`PythonPackageLock`), installed with
      `--require-hashes --no-deps --only-binary :all:`. It is GENERATED from
      `PythonPackagePins` by `tools/python-lock/generate_lock.py` (uv resolve for
      cp311/win_amd64, then only the installable wheels' PyPI SHA-256s). Change a
      pin ⇒ regenerate the lock in the same change; `PythonSupplyChainTests`
      fails while a locked version does not satisfy its pin. `EnsurePackagesAsync`
      refuses any spec that is not a `PythonPackagePins` value, and any
      non-Windows platform. The lock digest is part of the package stamp.
    - **Demucs weights never come from demucs.** demucs 4.1's `get_model` takes
      the HuggingFace hub's latest revision with no hash (and imports the class
      the file's metadata names), falling back to an 8-hex-digit (32-bit) prefix
      check plus `weights_only=False`. `StemSeparator` fetches the bag through
      `VerifiedDownload` (`requireHash: true`) from the revision and SHA-256 in
      `DemucsWeightPins.g.cs` (generated by `tools/python-lock/generate_demucs_weights.py`,
      which cross-checks each bag against the hash-locked demucs wheel) and passes
      `--weights-dir`; `separate_track.py` refuses a Demucs model without it
      (`PINNED_WEIGHTS_REQUIRED`), forces `HF_HUB_OFFLINE`, and only builds
      classes from the `demucs.` package. A model name with no pin is an
      `ArgumentException` before anything is provisioned.
    - **No interpreter from PATH, and no non-Windows bootstrap.** The package
      lock is win_amd64 and python.org ships no other embeddable build, so
      `EnsureInterpreterAsync` throws `PlatformNotSupportedException` elsewhere.
  - **`Probe()` no longer spawns a process per call** (cache key: path + mtime +
    length); `ProbeAsync` / `StemSeparator.IsReadyAsync` exist for UI code.
    `ResetGlobalGateForTests` must never dispose the gate an in-flight separation
    captured.
- `JuceSharp.AudioIo/`, `JuceSharp.Diagnostics/` — device I/O + diagnostics.
  - **`dca_decode_audio_file` has a free sizing path that an `out nint`
    parameter cannot reach.** With `out_samples != NULL` it decodes the whole
    file and hands the caller OWNERSHIP of a malloc'd buffer; with `NULL` it
    fills the metadata outs and returns before the reader is created. `out nint`
    always passes a real address, so `JuceAudioDecoder.Probe` decoded every file
    in full AND leaked `frames * channels * 4` bytes per call (measured: 173 MB
    over 48 probes of a 20 s mono file). Probe goes through the separate
    `NativeMethods.ProbeAudioFile` declaration and passes `nint.Zero`; pinned by
    `JuceAudioDecoderTests.Probe_does_not_leak_the_decoded_buffer`.
