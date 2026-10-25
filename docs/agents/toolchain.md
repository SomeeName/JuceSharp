# Toolchain and native rebuild details (moved verbatim out of AGENTS.md)

AGENTS.md keeps the commands and the key paths; this is the full text.

**On the owner's machine neither of those commands is on PATH.** The only
toolchain installed is **Visual Studio Build Tools 2026** at
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`, and the
working CMake is the copy it bundles:
`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
(`ctest.exe` beside it, v4.3.1-msvc1 on 2026-09-11). This file pointed at a
CLion-bundled CMake under `%LOCALAPPDATA%\Programs\CLion` until 2026-09-11;
CLion is not installed on this box and that path does not exist. The
generator is `"Visual Studio 18 2026"`; `"Visual Studio 17 2022"` (what this
file said until 0.56) fails to configure. In a fresh agent worktree the
FetchContent deps are re-cloned from scratch unless you point them at the main
checkout's copies:
`-DFETCHCONTENT_SOURCE_DIR_JUCE=<main>/build/_deps/juce-src` and the same for
`RUBBERBAND` and `NEURALAUDIO` (~33 s to configure instead of a full clone).
**A `FETCHCONTENT_SOURCE_DIR_*` directory is used AS-IS — FetchContent never
checks it out at the pinned tag.** The main checkout's own cache pointed JUCE
at its own `_deps/juce-src` and so compiled 8.0.13 for the whole 8.0.15
release; configure now FAILS when the JUCE sources are not the `JUCE_TAG`
version (the check follows `FetchContent_MakeAvailable(JUCE)`). After a
`JUCE_TAG` bump, clear that cache entry (or delete `build/`) and let JUCE be
fetched fresh; only point it at a copy that is already the new tag.
