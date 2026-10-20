# ABI & Versioning Policy

JuceSharp follows a **strict ABI versioning** policy. The integer
returned by `dca_abi_version()` bumps every time:

* a function is added, removed, or its signature changes;
* a struct layout changes;
* an enum value is added or renumbered;
* the calling convention changes.

A mismatch between the managed `NativeMethods.ExpectedAbiVersion` and
the native `dca_abi_version()` throws
`JuceAbiMismatchException("JuceSharp.Native ABI mismatch …")` (a subclass of
`InvalidOperationException`, so older `catch` blocks still match) from
`JuceAudioEngine`'s constructor.

## SemVer mapping

| ABI bump                         | SemVer impact                |
| -------------------------------- | ---------------------------- |
| New function appended            | `MINOR`                      |
| Existing function changed        | `MAJOR`                      |
| Struct or enum layout changed    | `MAJOR`                      |
| Bug fix, no ABI change           | `PATCH` (no ABI bump)        |

Managed-only additions (new property on `JuceEngineConfig`, new
overload on `JuceAudioEngine`) do **not** bump the ABI version.

## Logger interface stability

The `dca_log_callback` signature is part of the ABI. Adding a new field
to the message (e.g. structured key/value pairs) would require an ABI
bump; the current text format is considered stable.

## Status enum stability

`dca_status` values **never** change ordinal once shipped. New status
codes are appended.

## Additive changes bump it too

An export appended to the end of the header changes no existing signature and
breaks no existing caller — but the version is still bumped, because that
integer is how a managed caller knows the new symbols are there to resolve at
all. ABI v30, for example, was ten purely additive exports across two
independent surfaces.

## No back-version shim

There is **no** compatibility shim, and none is planned before 1.0.
`VerifyAbi` requires **exact equality** between
`NativeMethods.ExpectedAbiVersion` and `dca_abi_version()`; any difference in
either direction throws. Treat every release as ABI-incompatible and rebuild
the DLL alongside the wrapper.
