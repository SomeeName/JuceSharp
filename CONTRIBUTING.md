# Contributing to JuceSharp

Thanks for wanting to help. The full guide lives in
**[`docs/contributing.md`](docs/contributing.md)** — branches, coding style,
how to add an ABI export, and what a change needs before it lands. This page
is the two-minute version.

## Before you start

* Read [`AGENTS.md`](AGENTS.md). It is the router for every hard-won rule in
  this repository — the ABI contract, the real-time safety rules, the wrapper
  lifetime contract — and it points at the one file in
  [`docs/agents/`](docs/agents/) that covers whatever you are touching. Read
  the routed file *before* you write code in that area.
* This repository is a **git submodule** of DotCharter. Commit here first, then
  bump the submodule pointer in the parent — and push here before you push the
  parent, or the gitlink dangles for everyone else.

## Build and test

Every path below is relative to **this** repository's root (the folder holding
`JuceSharp.sln`), not to a parent checkout that embeds it as a submodule.

```powershell
# Native (Windows). Use the generator matching your toolset — VS 2022 or later.
# On the maintainer's machine the only toolchain installed is VS Build Tools
# 2026, so the generator there is "Visual Studio 18 2026"; "Visual Studio 17
# 2022" fails to configure. See docs/getting-started/building-native.md.
cd src/JuceSharp.Native
cmake -S . -B build -G "Visual Studio 17 2022" -A x64   # or "Visual Studio 18 2026"
cmake --build build --config Release

# Managed. global.json selects Microsoft.Testing.Platform, so pass --project.
# JUCESHARP_REQUIRE_NATIVE=1 turns "native DLL unavailable" from a silent skip
# of ~a quarter of the suite into a failure. CI sets it; set it locally too once
# you have built the native side.
dotnet test --project tests/JuceSharp.Tests/JuceSharp.Tests.csproj

# Native gates (allocation/lock gate over the real device callback, plus DSP).
ctest --test-dir src/JuceSharp.Native/build -C Release --output-on-failure --no-tests=error

# Docs: every relative Markdown link in README/CONTRIBUTING/SECURITY/docs/**.
python tools/check-doc-links.py
```

CI is [`.github/workflows/build.yml`](.github/workflows/build.yml): it runs the
native build, `ctest`, the managed suite (with `DCA_ENABLE_NULL_DEVICE=1` and
`JUCESHARP_REQUIRE_NATIVE=1`), the doc-link check and `dotnet pack`. It runs on
every push to `main` and every pull request here; the private development
repository runs the same workflow only by hand (every job is gated on that). It
is not a substitute for running the gates locally — the RT gate and the
device-dependent tests behave differently on a runner with no sound card.

## The rules that bite hardest

1. **An ABI change means bumping `DCA_ABI_VERSION` and rebuilding the DLL** —
   including a purely additive export. The wrapper demands exact equality.
   Confirm the symbol landed with `dumpbin /EXPORTS`.
2. **Nothing allocates, resizes or takes an unlisted lock on the audio
   thread.** `juce_sharp_rtcheck` will fail you.
3. **Every user-facing gain is ramped across the block**, never applied as a
   per-block constant.
4. **Every string crossing the ABI is UTF-8** (`LPUTF8Str`, never `LPStr`), and
   fixed out buffers truncate at a code point.
5. **Every managed delegate handed to native needs a catch-all body.** A
   reverse-P/Invoke that throws kills the process.
6. **A change ships with its own evidence** — a test that would fail if the
   change regressed, or a stated reason one is impossible. Break it on purpose
   once and watch it fail before you trust it.
7. **Update the docs in the same change.** If you make a page in `docs/` or a
   guide in `docs/agents/` misleading, fix it now; a stale safety claim is
   worse than no claim.

## Reporting a security issue

See [`SECURITY.md`](SECURITY.md).
