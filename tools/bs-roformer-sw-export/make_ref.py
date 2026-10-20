"""reference.npz for JuceSharp's env-gated BsRoformerSwModelTests: PyTorch fp32 (CUDA, TF32 off)
for ONE chunk of a song, and the core checked against the checkpoint's own forward().

python make_ref.py SONG.wav [--frames 1151] [--offset 60] [--out reference.npz]

Keys, as the 4 s export's reference.npz: input_audio [1, 2, N], spec_real / spec_imag
[1, 2, 1025, T], out_spec_real / out_spec_imag [1, 6, 2, 1025, T] (GroupedSwCore, the exported
module) and ref_audio [1, 6, 2, N] (the checkpoint's own forward(), STFT and iSTFT included).
The song is resampled to 44.1 kHz with soxr VHQ. Prints the core's agreement with forward().
"""
import argparse, os, sys
import numpy as np
import soundfile as sf
import soxr
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from swcore import load_model, GroupedSwCore, stft, istft, STEM_NAMES, _patch_rmsnorm

p = argparse.ArgumentParser()
p.add_argument("song")
p.add_argument("--frames", type=int, default=1151)
p.add_argument("--offset", type=float, default=60.0, help="seconds into the song")
p.add_argument("--out", default="reference.npz")
a = p.parse_args()

torch.backends.cuda.matmul.allow_tf32 = False
torch.backends.cudnn.allow_tf32 = False
dev = torch.device("cuda:0" if torch.cuda.is_available() else "cpu")
chunk = (a.frames - 1) * 512
x, sr = sf.read(a.song, dtype="float32", always_2d=True)
mix = soxr.resample(x, sr, 44100, quality="VHQ").T.copy()
start = int(a.offset * 44100)
audio = torch.from_numpy(mix[:, start:start + chunk].copy()).unsqueeze(0).to(dev)
assert audio.shape[-1] == chunk, "the song is too short for that offset"

model, _ = load_model(dev, patch=False)
with torch.no_grad():
    ref = model(audio)                     # the checkpoint's own forward(), unmodified MSST code
_patch_rmsnorm()                           # then the exportable form of the same weights
core = GroupedSwCore(model, a.frames, mode="mha", device=dev).to(dev).eval()
with torch.no_grad():
    re, im = stft(audio)
    ore, oim = core(re, im)
    via_core = istft(ore, oim, chunk)
for s, n in enumerate(STEM_NAMES):
    r, c = ref[0, s].double(), via_core[0, s].double()
    print(f"{n}: core vs forward() {10 * torch.log10((r ** 2).sum() / ((r - c) ** 2).sum().clamp_min(1e-30)).item():.1f} dB")
np.savez(a.out, input_audio=audio.cpu().numpy(), spec_real=re.cpu().numpy(), spec_imag=im.cpu().numpy(),
         out_spec_real=ore.cpu().numpy(), out_spec_imag=oim.cpu().numpy(), ref_audio=ref.cpu().numpy())
print("saved", a.out)
