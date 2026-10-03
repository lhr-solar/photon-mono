#!/usr/bin/env python3
"""Load charge_model.pt + scaler.json and predict remaining range.

Standalone Python API for a later Track Sim / Pit hook. No new protocol
messages; do not wire this into the WASM UI from here.

    from infer import load, predict
    load()
    predict(speed=12.0, pack_v=112.0, pack_i=18.0, array_w=700.0, soc=0.72)
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import numpy as np

from dataset import (
    BATTERY_WH,
    DEFAULT_CACHE,
    FEATURE_COLS,
    P_NET_FLOOR_W,
    SOC_MIN,
    estimate_pack_soc,
    hybrid_remain,
    physics_pack_power,
)
from model import RidgeChargeModel, build_mlp, try_import_torch

_STATE: dict[str, Any] = {}


def load(cache_dir: Path | str = DEFAULT_CACHE) -> dict[str, Any]:
    """Load weights + scaler into process-global state. Safe to call twice."""
    cache_dir = Path(cache_dir)
    scaler = json.loads((cache_dir / "scaler.json").read_text(encoding="utf-8"))
    pt_path = cache_dir / "charge_model.pt"
    ridge_path = cache_dir / "charge_model_ridge.json"

    backend = "torch"
    model: Any
    if ridge_path.exists() and not _is_torch_file(pt_path):
        backend = "ridge"
        model = RidgeChargeModel()
        model.load_state_dict(json.loads(ridge_path.read_text(encoding="utf-8")))
    else:
        torch, _ = try_import_torch()
        if torch is None:
            if ridge_path.exists():
                backend = "ridge"
                model = RidgeChargeModel()
                model.load_state_dict(json.loads(ridge_path.read_text(encoding="utf-8")))
            else:
                raise RuntimeError("torch not installed and no ridge sidecar at " + str(ridge_path))
        else:
            blob = torch.load(pt_path, map_location="cpu", weights_only=False)
            hidden = int(blob.get("hidden", 64))
            model = build_mlp(hidden)
            model.load_state_dict(blob["state_dict"])
            model.eval()
            backend = "torch"

    _STATE.clear()
    _STATE.update({"model": model, "scaler": scaler, "backend": backend, "cache_dir": str(cache_dir)})
    return {"backend": backend, "cache_dir": str(cache_dir)}


def _is_torch_file(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        with path.open("rb") as fh:
            magic = fh.read(2)
        return magic != b"{'" and magic != b'{"' and magic != b"{'"
    except OSError:
        return False


def _features(speed: float, pack_v: float, array_w: float, soc: float) -> np.ndarray:
    p_phys = float(physics_pack_power(float(speed), float(array_w)))
    speed3 = max(float(speed), 0.0) ** 3
    by_name = {
        "speed": float(speed),
        "pack_v": float(pack_v),
        "array_w": float(array_w),
        "soc": float(soc),
        "speed3": speed3,
        "p_phys_w": p_phys,
    }
    return np.asarray([[by_name[name] for name in FEATURE_COLS]], dtype=np.float32)


def predict(
    speed: float,
    pack_v: float,
    pack_i: float,
    array_w: float,
    soc: float | None = None,
) -> dict[str, float]:
    """Return {p_net_w, remain_s, remain_m} for one sample.

    soc is optional: if omitted, uses estimatePackSoc(pack_v).
    """
    if not _STATE:
        load()
    if soc is None:
        soc = float(estimate_pack_soc(float(pack_v)))
    x = _features(speed, pack_v, array_w, float(soc))
    scaler = _STATE["scaler"]
    mean = np.asarray(scaler["mean"], dtype=np.float32)
    std = np.asarray(scaler["std"], dtype=np.float32)
    y_mean = np.asarray(scaler["target_mean"], dtype=np.float32)
    y_std = np.asarray(scaler["target_std"], dtype=np.float32)
    xs = (x - mean) / std

    model = _STATE["model"]
    if _STATE["backend"] == "ridge":
        y = model.predict(xs)[0]
    else:
        torch, _ = try_import_torch()
        model.eval()
        with torch.no_grad():
            y = model(torch.from_numpy(xs)).numpy()[0]
        y = y * y_std + y_mean

    if _STATE["backend"] == "ridge":
        y = y * y_std + y_mean

    p_net = float(y[0])
    if scaler.get("residual_p_net"):
        p_net = p_net + float(physics_pack_power(float(speed), float(array_w)))
    if np.isfinite(pack_i) and abs(float(pack_i)) > 0.15:
        p_live = float(pack_v) * float(pack_i)
        p_use = p_live
    else:
        p_live = None
        p_use = p_net
    remaining_wh = max(0.0, (float(soc) - SOC_MIN) * BATTERY_WH)
    remain_s, remain_m = hybrid_remain(remaining_wh, p_use, speed)
    out = {
        "p_net_w": float(p_use),
        "p_net_model_w": p_net,
        "remain_s": float(remain_s),
        "remain_m": float(remain_m),
        "remaining_wh": remaining_wh,
        "p_floor_w": P_NET_FLOOR_W,
        "used_bus_current": p_live is not None,
    }
    if p_live is not None:
        out["p_net_live_w"] = p_live
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="Charge-model remaining-range inference")
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    ap.add_argument("--speed", type=float, required=True, help="m/s")
    ap.add_argument("--pack-v", type=float, required=True, help="pack voltage (V)")
    ap.add_argument("--pack-i", type=float, required=True, help="MC bus current (A)")
    ap.add_argument("--array-w", type=float, required=True, help="sum of MPPT Vin*Iin (W)")
    ap.add_argument("--soc", type=float, default=None, help="0-1; default estimatePackSoc(pack_v)")
    args = ap.parse_args()
    load(args.cache)
    out = predict(args.speed, args.pack_v, args.pack_i, args.array_w, args.soc)
    print(json.dumps(out, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
