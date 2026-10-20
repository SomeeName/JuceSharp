# Third-Party Notices

JuceSharp's own source is MIT ([`LICENSE`](LICENSE)). The native binaries are
not: **every build of `juce_sharp.dll` and `juce_vst_host.exe`, and every
package or application that carries one, is distributed under the AGPLv3
(JUCE 9) together with the GPL terms of Rubber Band and the ASIO SDK** — see
the statement at the end of `LICENSE`. This file lists every third-party
component the repository builds into those binaries, compiles in as data, or
references, with its version, copyright line and licence.

`ThirdPartyNoticeCoverageTests` (in `tests/JuceSharp.Tests/CoverageGaps/`)
fails the default test gate when a `FetchContent_Declare` in a
`CMakeLists.txt` has no entry here naming its repository and its pinned tag.

This file is informational, not legal advice. Verify the current terms with
each vendor before you redistribute anything.

## 1. Linked into the native library

Every component below is fetched by CMake at a pinned tag
(`src/JuceSharp.Native/CMakeLists.txt`) or vendored inside one that is. None of
their source is committed to this repository.

### JUCE

- Version: **9.0.3** (`JUCE_TAG`), fetched as commit be29c81492b6151c8ea8d14c840e1311963b3a83 (`JUCE_COMMIT`), <https://github.com/juce-framework/JUCE>
- Copyright (c) Raw Material Software Limited
- Linked into: `juce_sharp.dll` and `juce_vst_host.exe`
- Licence: **AGPLv3 or the commercial JUCE 9 licence.** JUCE 9.0.3's
  `LICENSE.md` says:

  > The JUCE Framework modules are dual-licensed under the
  > [AGPLv3](https://www.gnu.org/licenses/agpl-3.0.en.html) and the commercial
  > [JUCE licence](https://juce.com/legal/juce-9-licence/).

  > If you are not licensing the JUCE Framework modules under the AGPLv3 then
  > by downloading, installing, or using the JUCE Framework, or combining the
  > JUCE Framework with any other source code, object code, content or any
  > other copyrightable work, you agree to the terms of the JUCE 9 End User
  > Licence Agreement […]

  JuceSharp holds no commercial JUCE licence, so its binaries use JUCE under
  the AGPLv3. JUCE's own inventory of the code it vendors (versions,
  licences, locations) is `JUCE.spdx.json` in its source tree.

### Rubber Band Library

- Version: **v4.0.0** (`RUBBERBAND_TAG`), fetched as commit 1d95888bec3ae0a17c0c4af791810d5a63f6bc35 (`RUBBERBAND_COMMIT`), <https://github.com/breakfastquay/rubberband>,
  statically linked through upstream's single-file build
  (`single/RubberBandSingle.cpp`)
- Copyright 2007-2024 Particular Programs Ltd.
- Linked into: `juce_sharp.dll` (time-stretch and pitch-shift,
  `RubberBandAudioSource.cpp`)
- Licence: **GPLv2 or later, or a commercial licence.** Its README: "Rubber
  Band Library is distributed under the GNU General Public License (GPL). You
  can redistribute it and/or modify it under the terms of the GPL; either
  version 2 of the License, or (at your option) any later version." —
  <https://breakfastquay.com/rubberband/licensing.html>

### Steinberg VST 3 SDK

- Version: **3.8.0**, the copy bundled inside JUCE 9.0.3
  (`modules/juce_audio_processors_headless/format_types/VST3_SDK/`, which JUCE
  modified in two files to silence clang-tidy warnings),
  <https://github.com/steinbergmedia/vst3sdk>
- Copyright (c) 2025, Steinberg Media Technologies GmbH
- Linked into: `juce_sharp.dll` and `juce_vst_host.exe` (VST3 hosting)
- Licence: **MIT** (the SDK's `LICENSE.txt`; the text is reproduced in
  [§ 6](#6-licence-texts)).

### Steinberg ASIO SDK headers

- Version: **ASIO 2.3**, the headers bundled inside JUCE 9.0.3
  (`modules/juce_audio_devices/native/asio/`). This repository vendors no
  Steinberg SDK of its own.
- Copyright (c) 2025, Steinberg Media Technologies GmbH
- Linked into: `juce_sharp.dll` on Windows (the ASIO device type)
- Licence: **GPLv3 or the proprietary Steinberg ASIO licence.** The bundled
  `LICENSE.txt` says: "This Software Development Kit is licensed under the
  terms of the Steinberg ASIO License, or alternatively under the terms of the
  General Public License (GPL) Version 3. You may use the Software Development
  Kit according to either of these licenses as it is most appropriate for your
  project on a case-by-case basis (commercial or not)." JuceSharp uses the
  GPLv3 option.

### ww2ogg (Vorbis codebooks and the Wwise header rebuild)

- <https://github.com/hcs64/ww2ogg> (upstream `master`)
- Copyright (c) 2002 Xiph.org Foundation, (c) 2009-2016 Adam Gashlin
- In `juce_sharp.dll`: the Wwise RIFF Vorbis header and codebook rebuild,
  ported to C++ in `src/JuceSharp.Native/src/wem/WemVorbisDecoder.cpp`, and the
  two packed codebook libraries (the stock libvorbis 1.x and aoTuV beta 6.03
  codebooks), generated into `src/wem/wem_codebooks.h` by
  `tools/gen_wem_codebooks.py`
- Licence: **BSD-style** (ww2ogg's `COPYING`); the codebook data is under
  Xiph's BSD licence.

### NeuralAudio and its dependencies (neural amp captures)

On by default (`-DDOTCHARTER_DSP_NEURAL=ON`), linked into `juce_sharp.dll` for
the `namamp` module.

| Component | Version | Copyright | Licence |
|---|---|---|---|
| **NeuralAudio**, <https://github.com/mikeoliphant/NeuralAudio> | **v0.1.3** (`NEURALAUDIO_TAG`), commit b1137e1db2d43670e7dcf6345ade7808613cac20 (`NEURALAUDIO_COMMIT`) | Copyright (c) 2024 Mike Oliphant | MIT |
| NeuralAmpModelerCore (NAM Core), submodule of NeuralAudio | as pinned by NeuralAudio v0.1.3 | Copyright (c) 2023 Steven Atkinson | MIT |
| RTNeural, submodule of NeuralAudio | as pinned by NeuralAudio v0.1.3 | Copyright (c) 2020, jatinchowdhury18 | BSD-3-Clause |
| math_approx, submodule of NeuralAudio | as pinned by NeuralAudio v0.1.3 | Copyright (c) 2024, jatinchowdhury18 | BSD-3-Clause |
| xsimd, submodule of RTNeural | as pinned by RTNeural | Copyright (c) 2016, Johan Mabille, Sylvain Corlay, Wolf Vollprecht and Martin Renou; (c) 2016, QuantStack; (c) 2018, Serge Guelton | BSD-3-Clause |
| nlohmann/json, vendored in RTNeural | 3.11.1 | Copyright (c) 2013-2022 Niels Lohmann | MIT |
| Eigen, vendored in RTNeural | 3.4 | Eigen contributors | MPL-2.0 (<https://www.mozilla.org/MPL/2.0/>); its source is available from <https://gitlab.com/libeigen/eigen> and inside the pinned RTNeural tree |

### demucs.cpp (native stem separation, optional)

Off by default; linked into `juce_sharp.dll` only with
`-DDOTCHARTER_STEMS_DEMUCS_CPP=ON`.

- <https://github.com/sevagh/demucs.cpp> at commit
  **f1206e9adeea103aef4a636b9e62297cf1f8e34e** (`DEMUCS_CPP_TAG`)
- Copyright (c) 2023 Sevag H
- Licence: **MIT** (the project's `LICENSE`); it vendors **Eigen** (MPL-2.0).

## 2. Data compiled into the native library

### Impulse responses (`dcdsp_cab_irs.h`, `dcdsp_acoustic_irs.h`)

`src/JuceSharp.Native/src/dsp/cores/dcdsp_cab_irs.h` (26 cabinet IRs) and
`dcdsp_acoustic_irs.h` (3 acoustic-body IRs) are **synthetic, made for this
project** — no recorded or third-party impulse response is used:

- Cabinet models 0–11 and the three acoustic bodies: minimum-phase IRs
  reconstructed from authored magnitude targets (roll-offs, resonances,
  reflection combs), as each header's comment describes. Their offline
  generator was never committed; the rows are kept verbatim.
- Cabinet models 12–25: generated by `src/JuceSharp.Native/tools/cab_irs/gen_cab_irs.py`
  from published loudspeaker frequency-response data (Jensen's 1 W / 1 m
  anechoic sweeps in `tools/cab_irs/driver_curves.json`, and KEF's published
  R7 Meta response) through standard box, crossover and directivity formulas.
  Only measured facts are used; no third-party file is redistributed.

Licence: MIT, as the rest of this repository's source (the binary they are
compiled into is under the terms in § 1).

### The chart voice's excitation bank (`dcvoice_excitation_data.h`)

`src/JuceSharp.Native/src/dsp/voice/dcvoice_excitation_data.h` holds short
pick / finger / ghost / dead-string excitation residuals inverse-filtered from
DI recordings by **Karoryfer Samples** (Karoryfer Lecolds): *Emilyguitar*,
*Growlybass* and *Black And Blue Basses*, all released under **CC0 1.0
Universal** (public domain dedication; no attribution required, credited
anyway). Sources, pinned commits and the extraction tool:
[`src/JuceSharp.Native/src/dsp/voice/CREDITS.md`](src/JuceSharp.Native/src/dsp/voice/CREDITS.md).

## 3. Voice packs: the format vs. the recordings

- **The format and its code are JuceSharp's** and MIT like the rest of the
  source: the `.dcvp` reader (`src/dsp/voice/dcvoice_pack.h`,
  `dcvoice_codec.h`) and the builder (`src/JuceSharp.Native/tools/voice/build_voice_pack.py`).
- **The recordings are not in this repository.** The packs (`voices/*.dcvp`)
  are built outside git from pinned recordings and shipped by the application
  that uses them (DotCharter), whose own `THIRD-PARTY-NOTICES.md` carries their
  notices; `voices/VOICE-CREDITS.txt` and every pack carry the attribution too.
  Every pack a DotCharter release ships is built from CC0 1.0 (Karoryfer
  Samples) or CC BY 4.0 (EGFxSet, Guitar-TECHS) recordings. `CREDITS.md` (linked
  above) lists every source.

## 4. Managed code and packages

### OwnAudioSharp — Modernmube

- <https://github.com/ModernMube/OwnAudioSharp>
- Licence: **MIT** — `Copyright (c) 2025 Modernmube` (MIT text in
  [§ 6](#6-licence-texts)).
- What ships: no OwnAudioSharp code. The HTDemucs ONNX inference in
  `JuceSharp.StemSeparation.Onnx` (`HtdemucsOnnxInference`) was rewritten from
  OwnAudioSharp's HTDemucs sample and follows it (STFT padding, bin layout,
  window and chunk-crossfade scheme), so its notice is kept.

### NuGet package references

- **`Microsoft.ML.OnnxRuntime.DirectML`** (`JuceSharp.StemSeparation.Onnx`) — MIT,
  Microsoft.
  - **DirectML** (`DirectML.dll`, from `Microsoft.AI.DirectML`) is a Microsoft
    proprietary redistributable under the DirectML redistribution licence, not
    MIT. The package references it and ships it beside the consumer
    (`runtimes/win-x64/native`).
- **`Microsoft.Extensions.Logging.Abstractions`** — MIT, Microsoft.
- **`YamlDotNet`** — MIT (test project only, not shipped).

### Examples

`examples/` is MIT source like the rest of the repository. A built example
carries `juce_sharp.dll`, so a built example is under the terms in § 1.

## 5. Downloaded at run time, never redistributed

### By `JuceSharp.StemSeparation` (the Python sidecar)

Fetched into a per-user cache on first use, every file pinned by SHA-256
(`PythonEnvManager.DefaultPythonEmbedSha256`, `DefaultPipWheelSha256`, and the
hash-locked package set `requirements-win_amd64-cp311.lock`, which also lists
each package's own dependencies):

- **CPython 3.11.9 embeddable distribution** — Python Software Foundation
  License, from python.org.
- **pip** — MIT, the wheel from PyPI.
- **torch / torchaudio** — BSD-3-Clause.
- **demucs** — MIT.
- **bs_roformer** — see the project's own terms.
- **librosa** — ISC.
- **soundfile** — BSD-3-Clause, wrapping **libsndfile** (**LGPL-2.1**).
- **numpy** (BSD-3-Clause), **pyyaml** (MIT), **onnx** (Apache-2.0),
  **safetensors** (Apache-2.0).

### Model weights

- **Demucs bags** (`htdemucs`, `htdemucs_ft`, `htdemucs_6s`, `hdemucs_mmi`,
  `mdx`, `mdx_extra`) — Meta, MIT; the safetensors exports in the Hugging Face
  repositories `adefossez/*`, fetched by `StemSeparator` at a pinned revision
  with a pinned SHA-256 per file (`DemucsWeightPins`).
- **htdemucs ggml conversions** fetched by `DemucsGgmlModelCatalog` for the
  native `demucs.cpp` backend — a third-party re-host of the Meta MIT weights
  (Hugging Face dataset `Retrobear/demucs.cpp`); each catalogue row carries a
  `LicenceNote`, and the dataset card governs redistribution.
- **BS-RoFormer / MDX-Net** checkpoints from the Hugging Face Hub carry their
  **own** per-repository terms. Check the model card before shipping anything
  produced with one.

## 6. Licence texts

### MIT

Applies, with the copyright lines given above, to the VST 3 SDK, NeuralAudio,
NAM Core, nlohmann/json, demucs.cpp and OwnAudioSharp.

```
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### BSD-3-Clause

Applies, with the copyright lines given above, to RTNeural, math_approx and
xsimd.

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

### GPL / AGPL / MPL

The full texts of the GNU AGPLv3, GPLv2 and GPLv3 and the MPL-2.0 are at
<https://www.gnu.org/licenses/agpl-3.0.html>,
<https://www.gnu.org/licenses/old-licenses/gpl-2.0.html>,
<https://www.gnu.org/licenses/gpl-3.0.html> and
<https://www.mozilla.org/MPL/2.0/>, and in each component's source tree
(JUCE's `LICENSE.md`, Rubber Band's `COPYING`).
