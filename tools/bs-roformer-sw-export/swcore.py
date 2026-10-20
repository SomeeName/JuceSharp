"""BS-RoFormer SW: the model between the STFT and the iSTFT, as one exportable module.

The graph takes spec_real / spec_imag [B, 2, 1025, T] (torch.stft(center=True), periodic Hann,
n_fft 2048, hop 512) and returns out_spec_real / out_spec_imag [B, 6, 2, 1025, T]: the masked
spectrogram per stem, DC bin zeroed - the same IO contract as elicwhite/bs-roformer-web's export,
so JuceSharp's BsRoformerInference (SplitComplex layout) drives either file.

Every op is the checkpoint's own module (BandSplit, the Transformers, MaskEstimator) except the
pieces that are written out here so they export as plain, static-shaped ONNX:
  * the stereo/complex interleave is explicit permute + reshape (batch as -1);
  * rotary embeddings use precomputed cos / sin tables of the fixed T (time) and 62 (bands);
  * attention is either plain MatMul-Softmax-MatMul ("math") or ONNX Runtime's fused
    com.microsoft.MultiHeadAttention ("mha", what ships: 11 % faster on DirectML);
  * the 62 per-band networks of the band split and of each mask estimator run as 7 batched
    groups (GroupedSwCore), which is what lets DirectML's graph fusion stay on;
  * every RMSNorm is x * (1 / max(||x||, 1e-12)) (safe_normalize) so to_fp16.py can keep the
    small norm ops in fp32.
make_ref.py checks it against the checkpoint's own forward(): 112-144 dB per stem.
"""
import os
import sys

import torch
import torch.nn.functional as F
from torch import nn

# The work folder holds the MSST checkout (msst/), BS-Rofo-SW-Fixed.ckpt and its yaml (README.md).
WORK = os.environ.get("SW_EXPORT_WORK", os.getcwd())
sys.path.insert(0, os.path.join(WORK, "msst"))

N_FFT, HOP, BINS, STEMS = 2048, 512, 1025, 6
STEM_NAMES = ["bass", "drums", "other", "vocals", "guitar", "piano"]


def safe_normalize(x):
    """F.normalize(x, dim=-1) as ReduceL2 -> Clip -> Reciprocal -> Mul. The same value in fp32;
    in an fp16 graph the three small [.., 1] ops stay fp32 (to_fp16.py's block list), so the
    1e-12 floor does not underflow to 0 (0 / 0 = NaN on a silent frame) and the big tensor is
    never cast for a Div."""
    return x * torch.linalg.vector_norm(x, dim=-1, keepdim=True).clamp_min(1e-12).reciprocal()


def _patch_rmsnorm():
    from models.bs_roformer import bs_roformer as bsr
    bsr.RMSNorm.forward = lambda self, x: safe_normalize(x) * self.scale * self.gamma


def load_model(device="cpu", patch=True):
    import yaml
    from ml_collections import ConfigDict
    from models.bs_roformer.bs_roformer import BSRoformer
    with open(os.path.join(WORK, "BS-Rofo-SW-Fixed.yaml")) as f:
        cfg = ConfigDict(yaml.load(f, Loader=yaml.FullLoader))
    if patch:
        _patch_rmsnorm()
    model = BSRoformer(**dict(cfg.model))
    sd = torch.load(os.path.join(WORK, "BS-Rofo-SW-Fixed.ckpt"), map_location="cpu", weights_only=False)
    if isinstance(sd, dict) and "state_dict" in sd:
        sd = sd["state_dict"]
    model.load_state_dict(sd, strict=True)
    return model.to(device).eval(), cfg


def rope_tables(rotary, n, device):
    """cos / sin [n, 64] exactly as rotary_embedding_torch builds them for positions 0..n-1."""
    seq = torch.arange(n, device=device, dtype=torch.float32)
    freqs = torch.einsum("i,f->if", seq, rotary.freqs.float().to(device))
    freqs = torch.repeat_interleave(freqs, 2, dim=-1)
    return freqs.cos(), freqs.sin()


def rotate_half(x):
    # (d r) interleaved pairs: [x0, x1, x2, x3, ...] -> [-x1, x0, -x3, x2, ...]
    shape = x.shape
    x = x.reshape(*shape[:-1], shape[-1] // 2, 2)
    x1, x2 = x[..., 0], x[..., 1]
    return torch.stack((-x2, x1), dim=-1).reshape(shape)


class MhaOp(torch.autograd.Function):
    """Placeholder that exports as com.microsoft.MultiHeadAttention (BSNH q/k/v as [B, S, N*H])."""

    @staticmethod
    def forward(ctx, q, k, v, heads):
        b, s, _ = q.shape
        h = q.shape[-1] // heads
        qh = q.reshape(b, s, heads, h).transpose(1, 2)
        kh = k.reshape(b, s, heads, h).transpose(1, 2)
        vh = v.reshape(b, s, heads, h).transpose(1, 2)
        o = F.scaled_dot_product_attention(qh, kh, vh)
        return o.transpose(1, 2).reshape(b, s, heads * h)

    @staticmethod
    def symbolic(g, q, k, v, heads):
        return g.op("com.microsoft::MultiHeadAttention", q, k, v, num_heads_i=heads)


class CoreAttention(nn.Module):
    def __init__(self, attn, cos, sin, mode):
        super().__init__()
        self.a = attn
        self.heads = attn.heads
        self.mode = mode
        self.register_buffer("cos", cos, persistent=False)
        self.register_buffer("sin", sin, persistent=False)

    def forward(self, x):  # x [b, n, d]
        a = self.a
        x = a.norm(x)
        b, n, _ = x.shape
        qkv = a.to_qkv(x).reshape(b, n, 3, self.heads, 64)  # 'b n (qkv h d)'
        q = qkv[:, :, 0].transpose(1, 2)  # b h n d
        k = qkv[:, :, 1].transpose(1, 2)
        v = qkv[:, :, 2].transpose(1, 2)
        q = q * self.cos + rotate_half(q) * self.sin
        k = k * self.cos + rotate_half(k) * self.sin
        if self.mode == "mha":
            qf = q.transpose(1, 2).reshape(b, n, self.heads * 64)
            kf = k.transpose(1, 2).reshape(b, n, self.heads * 64)
            vf = v.transpose(1, 2).reshape(b, n, self.heads * 64)
            out = MhaOp.apply(qf, kf, vf, self.heads).reshape(b, n, self.heads, 64).transpose(1, 2)
        else:
            scale = 64 ** -0.5
            sim = torch.matmul(q * scale, k.transpose(-1, -2))
            out = torch.matmul(sim.softmax(dim=-1), v)
        gates = a.to_gates(x)  # b n h
        out = out * gates.transpose(1, 2).unsqueeze(-1).sigmoid()
        out = out.transpose(1, 2).reshape(b, n, self.heads * 64)
        return a.to_out(out)


class CoreTransformer(nn.Module):
    def __init__(self, tr, n, mode, device):
        super().__init__()
        rotary = tr.layers[0][0].rotary_embed
        cos, sin = rope_tables(rotary, n, device)
        self.blocks = nn.ModuleList(
            [nn.ModuleList([CoreAttention(attn, cos, sin, mode), ff]) for attn, ff in tr.layers])
        self.norm = tr.norm

    def forward(self, x):
        for attn, ff in self.blocks:
            x = attn(x) + x
            x = ff(x) + x
        return self.norm(x)


class SwCore(nn.Module):
    def __init__(self, model, frames, mode="math", device="cpu"):
        super().__init__()
        self.m = model
        self.frames = frames
        self.bands = len(model.band_split.dim_inputs)
        self.layers = nn.ModuleList([
            nn.ModuleList([CoreTransformer(t, frames, mode, device), CoreTransformer(f, self.bands, mode, device)])
            for t, f in model.layers])
        dc = torch.ones(1, 1, 1, BINS, 1)
        dc[..., 0, :] = 0
        self.register_buffer("dc", dc, persistent=False)

    def forward(self, spec_real, spec_imag):
        m, T, Fb = self.m, self.frames, self.bands
        # [b, s, f, t] x2 -> [b, (f s), t, c]  (frequency-major, stereo interleaved)
        st = torch.stack((spec_real, spec_imag), dim=-1)            # b s f t c
        st = st.permute(0, 2, 1, 3, 4).reshape(-1, BINS * 2, T, 2)  # b (f s) t c
        x = st.permute(0, 2, 1, 3).reshape(-1, T, BINS * 4)          # b t (f c)
        x = m.band_split(x)                                           # b t bands d
        d = x.shape[-1]
        for time_t, freq_t in self.layers:
            x = x.permute(0, 2, 1, 3).reshape(-1, T, d)               # (b bands) t d
            x = time_t(x)
            x = x.reshape(-1, Fb, T, d).permute(0, 2, 1, 3).reshape(-1, Fb, d)  # (b t) bands d
            x = freq_t(x)
            x = x.reshape(-1, T, Fb, d)
        x = m.final_norm(x)
        mask = torch.stack([fn(x) for fn in m.mask_estimators], dim=1)  # b n t (f c)
        mask = mask.reshape(-1, STEMS, T, BINS * 2, 2).permute(0, 1, 3, 2, 4)  # b n (f s) t c
        sr = st[..., 0].unsqueeze(1)
        si = st[..., 1].unsqueeze(1)
        mr, mi = mask[..., 0], mask[..., 1]
        re = sr * mr - si * mi                                         # b n (f s) t
        im = sr * mi + si * mr
        re = re.reshape(-1, STEMS, BINS, 2, T).permute(0, 1, 3, 2, 4) * self.dc  # b n s f t
        im = im.reshape(-1, STEMS, BINS, 2, T).permute(0, 1, 3, 2, 4) * self.dc
        return re, im


def stft(audio):
    """audio [b, 2, n] -> spec_real, spec_imag [b, 2, 1025, T] (torch.stft center=True, periodic Hann)."""
    b, s, n = audio.shape
    w = torch.hann_window(N_FFT, device=audio.device)
    z = torch.stft(audio.reshape(b * s, n), n_fft=N_FFT, hop_length=HOP, win_length=N_FFT, window=w,
                   center=True, normalized=False, return_complex=True)
    z = z.reshape(b, s, BINS, -1)
    return z.real.contiguous(), z.imag.contiguous()


def istft(re, im, length):
    """re / im [b, n, 2, 1025, T] -> audio [b, n, 2, length]."""
    b, n, s, f, t = re.shape
    z = torch.complex(re, im).reshape(b * n * s, f, t)
    w = torch.hann_window(N_FFT, device=re.device)
    y = torch.istft(z, n_fft=N_FFT, hop_length=HOP, win_length=N_FFT, window=w, center=True, length=length)
    return y.reshape(b, n, s, length)


# ---------------------------------------------------------------- grouped band split / mask estimators
#
# The checkpoint's BandSplit and MaskEstimator are 62 small per-band networks each, joined by a
# 62-way Split and (per stem) a 62-way Concat. DirectML's graph fusion miscomputes the 62-way
# Concat (measured: the first mask estimator's Concat differs, relL2 1.2, everything before it
# matches), and 6 x 62 tiny MLPs are ~3,000 of the graph's ~6,300 nodes. Bands of equal width are
# contiguous (24 x 2 bins, 12 x 4, 8 x 12, 8 x 24, 8 x 48, 128, 129), so each run of equal bands
# becomes ONE batched MatMul over stacked weights - the same arithmetic, 7 groups instead of 62.

def band_groups(dims):
    groups, start = [], 0
    while start < len(dims):
        end = start
        while end < len(dims) and dims[end] == dims[start]:
            end += 1
        groups.append((start, end - start, dims[start]))
        start = end
    return groups


class GroupedBandSplit(nn.Module):
    def __init__(self, bs):
        super().__init__()
        self.groups = band_groups(list(bs.dim_inputs))
        self.gammas = nn.ParameterList()
        self.weights = nn.ParameterList()
        self.biases = nn.ParameterList()
        self.scales = []
        for start, count, d in self.groups:
            nets = [bs.to_features[b] for b in range(start, start + count)]
            self.gammas.append(nn.Parameter(torch.stack([n[0].gamma.detach() for n in nets])))           # [G, d]
            self.weights.append(nn.Parameter(torch.stack([n[1].weight.detach().t() for n in nets])))     # [G, d, D]
            self.biases.append(nn.Parameter(torch.stack([n[1].bias.detach() for n in nets]).unsqueeze(1)))  # [G, 1, D]
            self.scales.append(nets[0][0].scale)

    def forward(self, x):  # [b, t, 4100] -> [b, t, 62, D]
        bt = x.shape[0] * x.shape[1]
        t = x.shape[1]
        outs, off = [], 0
        for (start, count, d), g, w, bias, sc in zip(self.groups, self.gammas, self.weights, self.biases, self.scales):
            xg = x[..., off:off + count * d].reshape(-1, count, d)            # [bt, G, d]
            off += count * d
            xg = safe_normalize(xg) * (g * sc)
            y = torch.matmul(xg.transpose(0, 1), w) + bias                    # [G, bt, D]
            outs.append(y.transpose(0, 1))                                    # [bt, G, D]
        y = torch.cat(outs, dim=1)                                            # [bt, 62, D]
        return y.reshape(-1, t, y.shape[1], y.shape[2])


class GroupedMaskEstimators(nn.Module):
    """All stems' MaskEstimators: [b, t, 62, D] -> [b, stems, t, 4100]."""

    def __init__(self, ests):
        super().__init__()
        self.stems = len(ests)
        self.groups = band_groups(list(ests[0].dim_inputs))
        self.w1 = nn.ParameterList(); self.b1 = nn.ParameterList()
        self.w2 = nn.ParameterList(); self.b2 = nn.ParameterList()
        for start, count, d in self.groups:
            def mlp(e, b):
                return e.to_freqs[b][0]  # Sequential(Linear, Tanh, Linear); [1] is the GLU
            l1 = [[mlp(e, b)[0] for b in range(start, start + count)] for e in ests]
            l2 = [[mlp(e, b)[2] for b in range(start, start + count)] for e in ests]
            self.w1.append(nn.Parameter(torch.stack([torch.stack([l.weight.detach().t() for l in row]) for row in l1])))  # [S, G, D, H]
            self.b1.append(nn.Parameter(torch.stack([torch.stack([l.bias.detach() for l in row]) for row in l1]).unsqueeze(2)))  # [S, G, 1, H]
            self.w2.append(nn.Parameter(torch.stack([torch.stack([l.weight.detach().t() for l in row]) for row in l2])))  # [S, G, H, 2d]
            self.b2.append(nn.Parameter(torch.stack([torch.stack([l.bias.detach() for l in row]) for row in l2]).unsqueeze(2)))  # [S, G, 1, 2d]

    def forward(self, x):
        b, t, nb, dd = x.shape
        xs = x.reshape(-1, nb, dd).transpose(0, 1).unsqueeze(0)             # [1, 62, bt, D]
        outs = []
        for (start, count, d), w1, b1, w2, b2 in zip(self.groups, self.w1, self.b1, self.w2, self.b2):
            xg = xs[:, start:start + count]                                    # [1, G, bt, D]
            h = torch.tanh(torch.matmul(xg, w1) + b1)                          # [S, G, bt, H]
            y = torch.matmul(h, w2) + b2                                       # [S, G, bt, 2d]
            y = y[..., :d] * torch.sigmoid(y[..., d:])                         # GLU -> [S, G, bt, d]
            outs.append(y.permute(0, 2, 1, 3).reshape(self.stems, -1, count * d))  # [S, bt, G*d]
        y = torch.cat(outs, dim=-1)                                            # [S, bt, 4100]
        return y.reshape(self.stems, -1, t, y.shape[-1]).transpose(0, 1)      # [b, S, t, 4100]


class GroupedSwCore(SwCore):
    def __init__(self, model, frames, mode="math", device="cpu"):
        super().__init__(model, frames, mode, device)
        self.gbs = GroupedBandSplit(model.band_split)
        self.gme = GroupedMaskEstimators(model.mask_estimators)

    def forward(self, spec_real, spec_imag):
        m, T, Fb = self.m, self.frames, self.bands
        st = torch.stack((spec_real, spec_imag), dim=-1)
        st = st.permute(0, 2, 1, 3, 4).reshape(-1, BINS * 2, T, 2)
        x = st.permute(0, 2, 1, 3).reshape(-1, T, BINS * 4)
        x = self.gbs(x)
        d = x.shape[-1]
        for time_t, freq_t in self.layers:
            x = x.permute(0, 2, 1, 3).reshape(-1, T, d)
            x = time_t(x)
            x = x.reshape(-1, Fb, T, d).permute(0, 2, 1, 3).reshape(-1, Fb, d)
            x = freq_t(x)
            x = x.reshape(-1, T, Fb, d)
        x = m.final_norm(x)
        mask = self.gme(x)                                                     # b n t (f c)
        mask = mask.reshape(-1, STEMS, T, BINS * 2, 2).permute(0, 1, 3, 2, 4)
        sr = st[..., 0].unsqueeze(1)
        si = st[..., 1].unsqueeze(1)
        mr, mi = mask[..., 0], mask[..., 1]
        re = sr * mr - si * mi
        im = sr * mi + si * mr
        re = re.reshape(-1, STEMS, BINS, 2, T).permute(0, 1, 3, 2, 4) * self.dc
        im = im.reshape(-1, STEMS, BINS, 2, T).permute(0, 1, 3, 2, 4) * self.dc
        return re, im
