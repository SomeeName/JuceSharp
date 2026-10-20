"""
Generic PyTorch-checkpoint to ONNX exporter for stem-separation models.

Handles the three real-world cases we see in the BS-RoFormer / MDX-Net
ecosystem:

    1. The .ckpt file is a fully pickled torch.nn.Module — export directly.
    2. The .ckpt file holds a state_dict and a sibling .yaml that names a
       model class (e.g. bs_roformer.BSRoformer); instantiate and export.
    3. Otherwise, fail with a clear instruction to run the upstream
       project-specific export_onnx.py from the training repository.

Stays a single file so JuceSharp.StemSeparation can ship it as a
content asset without dragging a venv into the repo.
"""
import argparse
import importlib
import sys


def _err(msg, code=2):
    sys.stderr.write(msg.rstrip() + "\n")
    sys.exit(code)


def _load_ckpt(path):
    import torch
    return torch.load(path, map_location="cpu", weights_only=False)


def _try_export_module(module, out_path, sample_seconds=8.0, sample_rate=44100):
    import torch
    module.eval()
    chunk = int(sample_seconds * sample_rate)
    x = torch.zeros(1, 2, chunk, dtype=torch.float32)
    torch.onnx.export(
        module,
        (x,),
        out_path,
        input_names=["input"],
        output_names=["output"],
        opset_version=17,
        dynamic_axes={"input": {2: "time"}, "output": {2: "time"}},
    )


def _instantiate_bsroformer(config_path, state_dict):
    import yaml

    # BS-RoFormer / MDX-Net training configs (e.g. jarredou/BS-Rofo-SW-Fixed)
    # frequently emit `!!python/tuple [a, b]` for window/kernel shapes. The
    # default SafeLoader rejects those tags, so we register a constructor
    # that turns them back into a real Python tuple. This keeps the loader
    # safe (no arbitrary object construction, no eval), it only opts in to
    # the one sequence-shaped tag that these model authors use.
    def _tuple_constructor(loader, node):
        return tuple(loader.construct_sequence(node))
    yaml.SafeLoader.add_constructor(
        "tag:yaml.org,2002:python/tuple", _tuple_constructor)

    with open(config_path, "r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    model_kwargs = cfg.get("model") or cfg.get("roformer") or cfg.get("Model") or {}
    cls_name = cfg.get("model_class") or cfg.get("class") or "BSRoformer"
    pkg_options = ["bs_roformer", "bs_roformer.bs_roformer", "roformer.bs_roformer"]
    cls = None
    last_err = None
    for pkg in pkg_options:
        try:
            mod = importlib.import_module(pkg)
            cls = getattr(mod, cls_name, None)
            if cls is not None:
                break
        except Exception as e:
            last_err = e
    if cls is None:
        _err(
            f"Could not import '{cls_name}' from any of {pkg_options}. "
            f"Install with `pip install bs_roformer`. Last error: {last_err}",
            4,
        )
    # Training-fork checkpoints (e.g. ZFTurbo/Music-Source-Separation-Training,
    # jarredou/BS-Rofo-SW-Fixed) carry kwargs such as `linear_transformer_depth`
    # that aren't present in the upstream PyPI `bs_roformer` constructor.
    # Drop unknown kwargs with a warning so the export at least produces a
    # weight-compatible ONNX (load_state_dict already runs with strict=False).
    import inspect
    try:
        sig = inspect.signature(cls.__init__)
        has_var_kw = any(
            p.kind == inspect.Parameter.VAR_KEYWORD for p in sig.parameters.values()
        )
        if not has_var_kw:
            accepted = set(sig.parameters.keys()) - {"self"}
            filtered = {k: v for k, v in model_kwargs.items() if k in accepted}
            dropped = sorted(set(model_kwargs) - set(filtered))
            if dropped:
                sys.stderr.write(
                    f"Warning: dropping {len(dropped)} kwarg(s) unknown to "
                    f"{cls_name}: {dropped}\n"
                )
            model_kwargs = filtered
    except (TypeError, ValueError):
        # Built-in / C-extension class without an introspectable signature;
        # fall through and let the original kwargs raise if incompatible.
        pass
    model = cls(**model_kwargs)
    missing, unexpected = model.load_state_dict(state_dict, strict=False)
    if missing or unexpected:
        sys.stderr.write(
            f"Warning: state_dict had {len(missing)} missing and "
            f"{len(unexpected)} unexpected keys; exporting anyway.\n"
        )
    return model


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--config", default=None)
    args = ap.parse_args()

    try:
        import torch  # noqa: F401
    except Exception as e:
        _err(f"PyTorch is required: {e}", 3)

    try:
        obj = _load_ckpt(args.ckpt)
    except Exception as e:
        _err(f"Failed to load checkpoint: {e}", 5)

    import torch

    if isinstance(obj, torch.nn.Module):
        try:
            _try_export_module(obj, args.out)
            return
        except Exception as e:
            _err(f"Direct ONNX export of pickled module failed: {e}", 6)

    state_dict = obj
    if isinstance(obj, dict) and "state_dict" in obj:
        state_dict = obj["state_dict"]
    elif isinstance(obj, dict) and "model" in obj and isinstance(obj["model"], dict):
        state_dict = obj["model"]

    if not args.config:
        _err(
            "Checkpoint is a state_dict but no --config yaml was supplied. "
            "BS-RoFormer / MDX-Net repos publish a matching .yaml next to the "
            ".ckpt; download both and re-import.",
            7,
        )

    try:
        model = _instantiate_bsroformer(args.config, state_dict)
        _try_export_module(model, args.out)
    except SystemExit:
        raise
    except Exception as e:
        _err(
            f"Architecture-aware export failed: {e}.\n"
            f"Workaround: clone the upstream training repo for this model "
            f"(e.g. ZFTurbo/Music-Source-Separation-Training) and run its "
            f"`export_onnx.py {args.ckpt} {args.config} {args.out}` script.",
            8,
        )


if __name__ == "__main__":
    main()
