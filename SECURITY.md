# Security Policy

## Supported versions

JuceSharp is pre-1.0 and only the tip of `main` is supported. There are no
maintenance branches, and every release is treated as ABI-incompatible with
every other — the managed wrapper requires an exact `DCA_ABI_VERSION` match
and will refuse a mismatched `juce_sharp.dll`.

| Version | Supported |
|---|---|
| `main` (currently 1.3.0, ABI v49) | ✅ |
| Anything older | ❌ — upgrade |

## Reporting a vulnerability

Please **do not** open a public issue for a security problem.

Report it privately through GitHub's *Report a vulnerability* flow on the
repository's **Security** tab, or to the repository owner directly. Include:

* what you found and where (file and, ideally, line);
* how to reproduce it, and what an attacker gains;
* the commit or version you tested.

Expect an acknowledgement within a few days. There is no bounty programme.

## What is in scope

* The managed wrapper (`src/JuceSharp/` and the sibling managed projects).
* The native engine and its C ABI (`src/JuceSharp.Native/`), including memory
  safety at the ABI boundary — handle lifetime, buffer sizing, UTF-8
  truncation, and the shared-memory layout of the sandboxed VST host bridge.
* The `juce-stems-diag` CLI and the Python sidecar's process handling and
  download verification.

## What is out of scope

* **Third-party VST3 plugins.** The host loads arbitrary native code by
  design. A plugin that corrupts memory or crashes runs at native privilege
  and JuceSharp cannot contain it — that is exactly what the optional
  out-of-process VST host (`SetRemoteBridge`, ABI v19) exists for. Use it if
  you load plugins you do not trust.
* **Vulnerabilities in upstream dependencies** (JUCE, Rubber Band, ONNX
  Runtime, torch, demucs, …). Report those to their own maintainers; open an
  issue here only if JuceSharp needs to bump a pin in response.
* **Model weights and checkpoints** fetched from Hugging Face or
  `dl.fbaipublicfiles.com`. They are untrusted third-party artifacts loaded by
  torch and ONNX Runtime; treat any checkpoint you did not produce as
  untrusted input.
* Denial of service caused by the user's own configuration — an absurd buffer
  size, a 4-hour separation timeout, a saturated concurrency limit.

## Notes for integrators

* The Python sidecar downloads and executes an interpreter and pip packages
  into `%LOCALAPPDATA%\JuceSharp\python-embed\`. If that is unacceptable in
  your environment, pre-populate the directory and never call the install
  path, or use the in-process ONNX backend instead.
* `juce_vst_host.exe` is launched by *you*, not by the engine — you name the
  shared-memory section, the two events **and the control pipe** (the fourth
  argument, which the managed API does not take; see
  [VST3 Hosting](docs/guides/vst3-hosting.md)). Use names an unprivileged
  process cannot guess or squat: anything that can open that pipe can make the
  child load an arbitrary VST3.
