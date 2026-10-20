"""Export BS-RoFormer SW (enerjazzer/BS-ROFO-SW-Fixed) to ONNX with the STFT outside the graph.

python export.py --out bs_roformer_sw_6stem_13s_fp32.onnx            # what ships (then to_fp16.py)
python export.py --frames 345 --out sw_4s_fp32.onnx                  # 4 s chunks, for comparison
Defaults: T = 1151 frames (588,800 samples, the training chunk), MultiHeadAttention, grouped bands,
batch 1. Byte-for-byte reproducible with the pinned requirements.txt.
"""
import argparse, json, os, sys, time
import onnx
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swcore import load_model, SwCore, GroupedSwCore, BINS, STEM_NAMES, N_FFT, HOP

p = argparse.ArgumentParser()
p.add_argument("--frames", type=int, default=1151)
p.add_argument("--mode", choices=["math", "mha"], default="mha")
p.add_argument("--batch", default="1", help="an int, or 'dynamic'")
p.add_argument("--opset", type=int, default=17)
p.add_argument("--grouped", type=int, default=1)
p.add_argument("--out", required=True)
a = p.parse_args()

model, cfg = load_model("cpu")
core = (GroupedSwCore if a.grouped else SwCore)(model, a.frames, mode=a.mode, device="cpu").eval()
b = 1 if a.batch == "dynamic" else int(a.batch)
x = torch.randn(b, 2, BINS, a.frames)
dyn = {"spec_real": {0: "batch"}, "spec_imag": {0: "batch"},
       "out_spec_real": {0: "batch"}, "out_spec_imag": {0: "batch"}} if a.batch == "dynamic" else None
t = time.perf_counter()
with torch.no_grad():
    torch.onnx.export(core, (x, x.clone()), a.out, input_names=["spec_real", "spec_imag"],
                      output_names=["out_spec_real", "out_spec_imag"], opset_version=a.opset,
                      dynamic_axes=dyn, do_constant_folding=True, dynamo=False,
                      custom_opsets={"com.microsoft": 1} if a.mode == "mha" else None)
m = onnx.load(a.out)
chunk = (a.frames - 1) * HOP
meta = {
    "model": "BS-RoFormer SW (BS-Rofo-SW-Fixed.ckpt, sha256 24e7d35e...916e)",
    "n_fft": str(N_FFT), "hop_length": str(HOP), "chunk_samples": str(chunk), "sample_rate": "44100",
    "stems": json.dumps(STEM_NAMES), "dynamic_time": "false",
    "dynamic_batch": "true" if a.batch == "dynamic" else "false",
    "attention": a.mode, "bands": "grouped" if a.grouped else "per-band", "precision": "fp32",
    "export": "DotCharter #267 (JuceSharp tools/bs-roformer-sw-export)",
}
if a.grouped:
    # Checked against the PyTorch reference with DirectML graph fusion ON (the per-band
    # 62-way Concat that fusion miscomputes is gone); JuceSharp leaves fusion on for it.
    meta["dml_graph_fusion"] = "verified"
del m.metadata_props[:]
for k, v in meta.items():
    e = m.metadata_props.add(); e.key = k; e.value = v
onnx.save(m, a.out)
print(f"exported {a.out} in {time.perf_counter() - t:.1f}s: {os.path.getsize(a.out) / 2**20:.1f} MiB, "
      f"{len(m.graph.node)} nodes, chunk {chunk}")
