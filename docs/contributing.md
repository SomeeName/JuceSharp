# Contributing

Pull requests welcome! Please follow the conventions below.

## Repository

* `main` — the one long-lived branch. Everything merges here.
* Feature work happens on short-lived topic branches (`feat/…`, `fix/…`,
  `docs/…`, `perf/…`, `test/…`) cut from `main` and merged back.

There is no `master` and no `develop-juce` branch; both names are historical.

## Build & test before pushing

Paths are relative to **this** repository's root — the folder holding
`JuceSharp.sln`.

```powershell
# Native (Windows). The generator must match your toolset: "Visual Studio 17
# 2022" on a VS-2022 box, "Visual Studio 18 2026" on the maintainer's (VS Build
# Tools 2026 is the only toolchain installed there). See
# docs/getting-started/building-native.md.
cd src/JuceSharp.Native
cmake --build build --config Release --target juce_sharp

# Managed. JUCESHARP_REQUIRE_NATIVE=1 makes an unloadable juce_sharp.dll a
# FAILURE instead of a silent skip of ~a quarter of the suite.
dotnet test --project tests/JuceSharp.Tests/JuceSharp.Tests.csproj

# Native gates
ctest --test-dir src/JuceSharp.Native/build -C Release --output-on-failure --no-tests=error

# Docs — every relative Markdown link across README/CONTRIBUTING/SECURITY/docs.
python tools/check-doc-links.py
```

`global.json` selects the Microsoft.Testing.Platform runner (xunit.v3), so pass
`--project <test csproj>` rather than the solution.

> ⚠️ **Warning**
> `.github/workflows/build.yml` builds the native engine, runs the RT gate, the
> managed suite (`DCA_ENABLE_NULL_DEVICE=1`, `JUCESHARP_REQUIRE_NATIVE=1`) and
> `tools/check-doc-links.py` on `windows-latest`. It has not yet been exercised
> on a hosted runner (the VS-2022 generator auto-detection is the unverified
> step), so run the gates locally before you push as well.

## Coding style

### C# (managed)

* `dotnet format` clean.
* Prefer `init`-only properties on DTOs.
* Public types and members get XML doc comments.
* `Check(NativeMethods.X(...), "op_name")` for every ABI call — this
  routes errors through `JuceAudioException` with the correct
  `Operation`.

### C++ (native)

* **C++17** (`CMAKE_CXX_STANDARD 17`, extensions off).
* No raw `new` / `delete` in new code — use `std::unique_ptr`.
* Every `extern "C"` ABI entry point goes through `guard("name", [&]{})`.
* No allocations, no `resize`/`setSize`, and no unlisted locks on the audio
  thread — `juce_sharp_rtcheck` fails the build if you add one.

## Adding a feature

1. Open an issue describing the intent.
2. Draft the public managed surface first (the API a consumer would
   write against). Get it approved before implementing.
3. Bump the ABI version if you change the C header (see
   [Building from Source](native/building.md#adding-a-new-abi-export)) and
   rebuild the DLL — additive exports bump it too.
4. Write tests:
   * Managed-only tests in `JuceSharp.Tests` for behaviour visible from
     C#.
   * A native CTest target if the change is purely native.
5. Update [Changelog](changelog.md).

## License headers

JuceSharp's source is MIT; the native binaries are AGPLv3 because of what they
link — see the statement at the end of the repository-root
[`LICENSE`](https://github.com/SomeeName/JuceSharp/blob/main/LICENSE), the full
list in [`THIRD-PARTY-NOTICES.md`](https://github.com/SomeeName/JuceSharp/blob/main/THIRD-PARTY-NOTICES.md)
(a new `FetchContent` dependency needs an entry there, or the test gate fails)
and the
[ABI Policy](abi-policy.md) page.

## Code of conduct

Be kind. Assume good intent. Audio engineering is hard; everyone is
learning.
