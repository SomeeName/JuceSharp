"""
Audio stem separation entry point bundled with JuceSharp.StemSeparation.

Speaks a tiny line-based protocol on stdout so the C# wrapper can surface
real-time progress without parsing model-specific log noise:

    PROGRESS <0..1> [message]   periodic progress updates
    DEVICE   <cuda|mps|cpu>     once after device selection
    READY                       once when the model is ready
    STEM     <name> <path>      once per written stem file
    INFO     <message>          informational; safe to log

Anything written to stderr is captured by the wrapper and surfaced on
failure. Exit code 0 == success.

Supports two backends:

  --model htdemucs|htdemucs_ft|htdemucs_6s|mdx_extra|...
        Loads the corresponding pretrained Demucs bag and runs
        `demucs.apply.apply_model`. No --model-path needed.

  --model bs_roformer  --model-path <onnx-or-ckpt>  [--config <yaml>]
        Loads a BS-RoFormer state_dict + yaml from disk.
"""
import argparse
import os
import sys
import time
import traceback


def _emit(line: str) -> None:
    sys.stdout.write(line.rstrip() + "\n")
    sys.stdout.flush()


def _progress(frac: float, msg: str = "") -> None:
    _emit(f"PROGRESS {max(0.0, min(1.0, float(frac))):.4f} {msg}")


def _pick_device(requested: str) -> str:
    import torch
    if requested == "auto":
        if torch.cuda.is_available():
            return "cuda"
        if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
            return "mps"
        return "cpu"
    if requested == "cuda" and not torch.cuda.is_available():
        _emit("INFO CUDA requested but unavailable; falling back to CPU.")
        return "cpu"
    if requested == "mps" and not (
        getattr(torch.backends, "mps", None) and torch.backends.mps.is_available()
    ):
        _emit("INFO MPS requested but unavailable; falling back to CPU.")
        return "cpu"
    return requested


def _load_audio(path: str, sample_rate: int):
    import numpy as np
    import soundfile as sf

    data, sr = sf.read(path, always_2d=True, dtype="float32")
    # Convert (frames, channels) -> (channels, frames)
    audio = data.T.copy()
    if audio.shape[0] == 1:
        audio = np.repeat(audio, 2, axis=0)
    elif audio.shape[0] > 2:
        audio = audio[:2, :]
    if sr != sample_rate:
        try:
            import librosa
            audio = librosa.resample(audio, orig_sr=sr, target_sr=sample_rate)
        except Exception as e:
            _emit(f"INFO librosa resample failed ({e}); using raw rate {sr}.")
            sample_rate = sr
    return audio, sample_rate


def _save_stem(out_dir: str, name: str, audio, sample_rate: int) -> str:
    import soundfile as sf
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{name}.wav")
    sf.write(path, audio.T, sample_rate, subtype="FLOAT")
    return path


def _run_demucs(args) -> None:
    import numpy as np
    import torch
    from demucs.apply import apply_model
    from demucs.pretrained import get_model

    _progress(0.06, "Fetching Demucs bag…")
    bag = get_model(args.model)
    device = _pick_device(args.device)
    _emit(f"DEVICE {device}")
    bag.to(device)
    sample_rate = int(getattr(bag, "samplerate", 44100))
    _progress(0.10, f"Decoding audio @ {sample_rate} Hz…")
    audio, sample_rate = _load_audio(args.input, sample_rate)
    tensor = torch.from_numpy(audio).unsqueeze(0).to(device)
    _emit("READY")

    def on_progress(frac):
        _progress(0.10 + 0.85 * float(frac), "Inference…")

    # Demucs's apply_model supports a `callback` kw in 4.0.x for chunk progress
    try:
        result = apply_model(
            bag, tensor,
            device=device, shifts=args.shifts, overlap=args.overlap,
            split=True, progress=False,
            callback=lambda d: on_progress(d.get("frac", d.get("progress", 0))) if isinstance(d, dict) else None,
        )
    except TypeError:
        # Older / forked demucs without callback support
        result = apply_model(
            bag, tensor,
            device=device, shifts=args.shifts, overlap=args.overlap,
            split=True, progress=False,
        )
        on_progress(1.0)

    sources = result.squeeze(0).cpu().numpy()
    names = list(getattr(bag, "sources", ["drums", "bass", "other", "vocals"]))
    wanted = set(s.strip().lower() for s in args.stems.split(",")) if args.stems else None
    for i, name in enumerate(names):
        if wanted and name.lower() not in wanted:
            continue
        path = _save_stem(args.out, name, sources[i], sample_rate)
        _emit(f"STEM {name} {path}")
    _progress(1.0, "Done.")


def _run_roformer(args) -> None:
    import numpy as np
    import torch
    import yaml

    if not args.model_path or not os.path.isfile(args.model_path):
        raise FileNotFoundError(
            f"--model-path is required for bs_roformer and must point to a .ckpt or .onnx (got: {args.model_path!r})"
        )
    if not args.config or not os.path.isfile(args.config):
        raise FileNotFoundError(
            f"--config yaml is required for bs_roformer (got: {args.config!r})"
        )

    with open(args.config, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)

    model_kwargs = cfg.get("model") or cfg.get("roformer") or {}
    stems_cfg = list(model_kwargs.get("stems") or cfg.get("stems") or ["vocals", "other"])
    sample_rate = int(model_kwargs.get("sample_rate") or cfg.get("sample_rate") or 44100)

    _progress(0.06, "Loading RoFormer checkpoint…")
    state = torch.load(args.model_path, map_location="cpu", weights_only=False)
    if isinstance(state, dict) and "state_dict" in state:
        state = state["state_dict"]

    cls = None
    last_err = None
    for pkg_name in ("bs_roformer", "bs_roformer.bs_roformer"):
        try:
            mod = __import__(pkg_name, fromlist=["BSRoformer"])
            cls = getattr(mod, "BSRoformer", None)
            if cls is not None:
                break
        except Exception as e:
            last_err = e
    if cls is None:
        raise RuntimeError(
            f"BSRoformer class unavailable; install bs_roformer. Last error: {last_err}"
        )
    model = cls(**{k: v for k, v in model_kwargs.items() if k != "stems" and k != "sample_rate"})
    model.load_state_dict(state, strict=False)

    device = _pick_device(args.device)
    _emit(f"DEVICE {device}")
    model.to(device).eval()

    _progress(0.10, f"Decoding audio @ {sample_rate} Hz…")
    audio, sample_rate = _load_audio(args.input, sample_rate)
    x = torch.from_numpy(audio).unsqueeze(0).to(device)
    _emit("READY")

    chunk = sample_rate * 8
    hop = int(chunk * (1.0 - args.overlap))
    n = x.shape[-1]
    total_chunks = max(1, (n + hop - 1) // hop)
    out = torch.zeros((len(stems_cfg), 2, n), device=device)
    weights = torch.zeros(n, device=device)
    window = torch.hann_window(chunk, device=device)

    with torch.no_grad():
        for ci, start in enumerate(range(0, n, hop)):
            end = min(n, start + chunk)
            piece = x[..., start:end]
            if piece.shape[-1] < chunk:
                pad = chunk - piece.shape[-1]
                piece = torch.nn.functional.pad(piece, (0, pad))
            y = model(piece)
            if y.dim() == 4:
                y = y.squeeze(0)
            seg = y[..., : end - start]
            w = window[: end - start]
            for si in range(len(stems_cfg)):
                out[si, :, start:end] += seg[si] * w
            weights[start:end] += w
            _progress(0.10 + 0.85 * (ci + 1) / total_chunks, f"Chunk {ci+1}/{total_chunks}")

    weights = torch.clamp(weights, min=1e-8)
    out = out / weights
    sources = out.cpu().numpy()

    wanted = set(s.strip().lower() for s in args.stems.split(",")) if args.stems else None
    for i, name in enumerate(stems_cfg):
        if wanted and name.lower() not in wanted:
            continue
        path = _save_stem(args.out, name, sources[i], sample_rate)
        _emit(f"STEM {name} {path}")
    _progress(1.0, "Done.")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--device", default="auto")
    ap.add_argument("--shifts", type=int, default=1)
    ap.add_argument("--overlap", type=float, default=0.25)
    ap.add_argument("--model-path", dest="model_path", default=None)
    ap.add_argument("--config", default=None)
    ap.add_argument("--stems", default=None)
    ap.add_argument("--selftest", action="store_true",
                    help="Validate argument parsing and emit a fake STEM line. "
                         "Used by JuceSharp.Tests to exercise the wire protocol "
                         "without requiring torch / model weights.")
    args = ap.parse_args()

    if args.selftest:
        _emit(f"DEVICE cpu")
        _progress(0.0, "Selftest start.")
        _emit("READY")
        os.makedirs(args.out, exist_ok=True)
        path = os.path.join(args.out, "selftest.wav")
        with open(path, "wb") as f:
            f.write(b"\x00" * 8)
        _emit(f"STEM selftest {path}")
        _progress(1.0, "Selftest done.")
        return 0

    try:
        if args.model.lower() == "bs_roformer":
            _run_roformer(args)
        else:
            _run_demucs(args)
        return 0
    except KeyboardInterrupt:
        sys.stderr.write("Cancelled.\n")
        return 130
    except Exception as e:
        sys.stderr.write(f"{type(e).__name__}: {e}\n")
        traceback.print_exc(file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
