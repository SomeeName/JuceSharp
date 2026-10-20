# BS-RoFormer SW: the training-chunk ONNX export

Rebuilds `bs_roformer_sw_6stem_13s_fp16.onnx` (DotCharter #267) from the PyTorch
checkpoint. The STFT stays OUTSIDE the graph, with the same IO as the 4 s third-party
export, so `BsRoformerInference` (layout `SplitComplex`) drives either file:

- in: `spec_real` / `spec_imag` `[1, 2, 1025, 1151]`, `torch.stft(center=True)` of a
  588,800-sample chunk (13.35 s at 44.1 kHz, the model's training chunk), periodic Hann,
  n_fft 2048, hop 512;
- out: `out_spec_real` / `out_spec_imag` `[1, 6, 2, 1025, 1151]`, the masked spectrogram
  per stem (bass, drums, other, vocals, guitar, piano), DC bin zeroed;
- metadata: `n_fft`, `hop_length`, `chunk_samples`, `sample_rate`, `stems`, and
  `dml_graph_fusion=verified` (see below).

## Reproduce

```
uv venv --python 3.13 venv
uv pip install --python venv/Scripts/python.exe torch==2.11.0 --index-url https://download.pytorch.org/whl/cu128
uv pip install --python venv/Scripts/python.exe -r requirements.txt
git clone https://github.com/ZFTurbo/Music-Source-Separation-Training msst   # used at 84b1eac
# from https://huggingface.co/enerjazzer/BS-ROFO-SW-Fixed :
#   BS-Rofo-SW-Fixed.ckpt  sha256 24e7d35ee9c64415673d3fd33e06a67cac2c103c5df6267ba1576459c775916e
#   BS-Rofo-SW-Fixed.yaml
venv/Scripts/python.exe export.py --out bs_roformer_sw_6stem_13s_fp32.onnx
venv/Scripts/python.exe to_fp16.py bs_roformer_sw_6stem_13s_fp32.onnx bs_roformer_sw_6stem_13s_fp16.onnx
venv/Scripts/python.exe make_ref.py SONG.wav --out reference.npz      # for the env-gated tests
```

Run from the work folder that holds `msst/`, the checkpoint and the yaml (or set
`SW_EXPORT_WORK`). With the pinned packages both files come out byte for byte the same
on every run (checked twice, 2026-10-05):

| File | Bytes | SHA-256 |
|---|---|---|
| `bs_roformer_sw_6stem_13s_fp32.onnx` | 714,313,918 | `ac3345f23a87c387ed6af9453b5e327a9675eadb99cd8c72a065c2e7ff75eb58` |
| `bs_roformer_sw_6stem_13s_fp16.onnx` | 357,936,365 | `2fd77c88e3fb049eed18d7e3b3c1dbf3c401f61e9c04e58375c557a5a27157fe` |

## What differs from the checkpoint's own code, and why

The arithmetic is the checkpoint's (`make_ref.py` prints the agreement with `forward()`:
112-144 dB per stem). The FORM is rewritten for ONNX Runtime's DirectML provider:

1. **Batched band groups** (`GroupedBandSplit`, `GroupedMaskEstimators`). The model's band
   split and each of its six mask estimators are 62 small per-band networks joined by a
   62-way Split and a 62-way Concat. DirectML's graph fusion miscomputes that Concat
   (located by exposing every intermediate: all nodes before it agree, relative L2 1.2 at
   it) - which is why fusion had to be off for every model (#151, #258). The bands of equal
   width are contiguous, so they run as 7 batched MatMuls instead: same arithmetic, ~2,700
   fewer nodes, and fusion gives the right answer.
2. **`com.microsoft.MultiHeadAttention`** instead of MatMul-Softmax-MatMul: 11 % faster per
   chunk on DirectML, and the CPU provider runs it too.
3. **fp16 compute** (`to_fp16.py`) with every norm's ReduceL2 -> Clip -> Reciprocal and the
   band split's input normalisation kept in fp32: in fp16 a silent band is 0 * inf = NaN,
   which turned six of the test song's 36 chunks into silence before this was found.
4. **Fixed shapes, batch 1.** The rotary tables are baked for T = 1151 (the time axis cannot
   flex anyway), and a dynamic batch axis cost 10 % with no gain from batch 2.

`dml_graph_fusion=verified` tells `OnnxStemSeparator` to leave DirectML's graph fusion on
for this file (`OnnxSessionFactory.DirectMlGraphFusionKey`). Only write it into an export
that `BsRoformerSwDirectMlTests` passes WITH fusion on.

The weights' licence is unknown (see the catalogue's `LicenceNote`).
