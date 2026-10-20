"""fp32 export -> fp16 compute, IO kept fp32.

python to_fp16.py IN OUT

Kept in fp32 (everything else is fp16):
  * every norm's ReduceL2 -> Clip -> Reciprocal (small [..., 1] tensors): in fp16 the 1e-12 floor
    underflows and 1 / ||x|| overflows for a tiny vector;
  * the whole input normalisation of the band split (/gbs/: Slice, Reshape, ReduceL2, Clip,
    Reciprocal, Mul). Its input is the raw spectrogram, where a band can be all zero (digital
    silence) or ~1e-7 (a lowpassed source): torch's F.normalize scales the latter to unit norm,
    and in fp16 x * (1 / ||x||) is 0 * inf = NaN for the former. Measured: without this, six
    of the test song's 36 chunks came back entirely NaN.
"""
import argparse, os
import onnx
from onnxconverter_common import float16

p = argparse.ArgumentParser()
p.add_argument("src"); p.add_argument("dst")
p.add_argument("--block", default="ReduceL2,Clip,Reciprocal", help="op types kept in fp32")
p.add_argument("--node-prefix", default="/gbs/", help="nodes under this prefix of --node-ops are kept fp32")
p.add_argument("--node-ops", default="Slice,Reshape,ReduceL2,Clip,Reciprocal,Mul")
p.add_argument("--top-level-fp32", type=int, default=0)
a = p.parse_args()
m = onnx.load(a.src)
block = [b for b in a.block.split(",") if b]
ops = set(a.node_ops.split(","))
nodes = [n.name for n in m.graph.node if a.node_prefix and n.name.startswith(a.node_prefix) and n.op_type in ops]
if a.top_level_fp32:
    # The spectrogram plumbing and the final complex multiply (graph top level, no module path):
    # the spectrogram is O(300) where fp16 keeps ~3 decimal digits, which alone limits a stem to
    # ~60 dB. In fp32 the mask is the only fp16 factor, as under PyTorch autocast.
    nodes += [n.name for n in m.graph.node if n.name.count("/") == 1 and n.op_type != "Constant"]
m16 = float16.convert_float_to_float16(m, keep_io_types=True, op_block_list=block, node_block_list=nodes,
                                      disable_shape_infer=False)
for e in m16.metadata_props:
    if e.key == "precision":
        e.value = f"fp16 (fp32: {','.join(block)}; band-split input norm" + ("; spectrogram in/out" if a.top_level_fp32 else "") + ")"
onnx.save(m16, a.dst)
print(f"{a.dst}: {os.path.getsize(a.dst) / 2**20:.1f} MiB, fp32 ops: {block}, fp32 nodes: {len(nodes)}")
