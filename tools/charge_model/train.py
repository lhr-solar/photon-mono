#!/usr/bin/env python3
"""Train the remaining-range model and write weights + metrics to the cache dir.

Split: hold out 2026-07-23-03 (longest stint). Other July 23 sessions train.
July 18/19 are sanity-only and are not shuffled into train.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import numpy as np
import pandas as pd

from dataset import (
    DEFAULT_CACHE,
    DEFAULT_CATALOG,
    FEATURE_COLS,
    TARGET_COLS,
    VAL_SESSION_ID,
    build_dataset,
    feature_matrix,
    hybrid_remain,
    naive_remain_s,
    physics_pack_power,
)
from model import RidgeChargeModel, build_mlp, try_import_torch

BATTERY_WH = 5000.0
SOC_MIN = 0.10


def _metrics(y_true: np.ndarray, y_pred: np.ndarray, mask: np.ndarray | None = None,
             row_ok: np.ndarray | None = None) -> dict[str, float]:
    out: dict[str, float] = {}
    names = TARGET_COLS
    for i, name in enumerate(names):
        t = y_true[:, i]
        p = y_pred[:, i]
        m = np.isfinite(t) & np.isfinite(p)
        if name == "p_net_w" and mask is not None:
            m = m & mask.astype(bool)
        if name != "p_net_w" and row_ok is not None:
            m = m & row_ok.astype(bool)
        if name == "remain_s":
            m = m & (t < 8 * 3600) & (p < 8 * 3600)
        if name == "remain_m":
            m = m & (t < 200_000) & (p < 200_000)
        if not m.any():
            out[f"{name}_mae"] = float("nan")
            out[f"{name}_rmse"] = float("nan")
            out[f"{name}_n"] = 0
            continue
        err = p[m] - t[m]
        out[f"{name}_mae"] = float(np.mean(np.abs(err)))
        out[f"{name}_rmse"] = float(np.sqrt(np.mean(err**2)))
        out[f"{name}_n"] = int(m.sum())
    return out


def _drive_ok(frame: pd.DataFrame) -> np.ndarray:
    speed = frame["speed"].fillna(0.0).to_numpy()
    p = frame["p_net_w"].to_numpy()
    rwh = frame["remaining_wh_floor"].fillna(0.0).to_numpy() if "remaining_wh_floor" in frame.columns else np.zeros(len(frame))
    return (speed >= 5.0) & np.isfinite(p) & (p >= 200.0) & (rwh >= 150.0)


def _baselines(df: pd.DataFrame) -> np.ndarray:
    speed = df["speed"].fillna(0.0).to_numpy(np.float64)
    array_w = df["array_w"].fillna(0.0).to_numpy(np.float64)
    p_phys = physics_pack_power(speed, array_w)
    p_batt = df["p_net_w"].to_numpy(np.float64)
    p_for_naive = np.where(np.isfinite(p_batt), p_batt, p_phys)
    remain_wh = df["remaining_wh"].fillna(0.0).to_numpy(np.float64)
    remain_s = naive_remain_s(remain_wh, p_for_naive)
    remain_m = remain_s * np.maximum(speed, 0.0)
    return np.column_stack([p_phys, remain_s, remain_m])


def _standardize(x: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    return (x - mean) / std


def _fit_scaler(x: np.ndarray, y: np.ndarray, mask: np.ndarray) -> dict:
    x_mean = np.nanmean(x, axis=0)
    x_std = np.nanstd(x, axis=0)
    x_std = np.where(x_std < 1e-6, 1.0, x_std)
    y_work = y.copy()
    if mask is not None:
        y_work[:, 0] = np.where(mask.astype(bool), y[:, 0], np.nan)
    y_mean = np.nanmean(y_work, axis=0)
    y_std = np.nanstd(y_work, axis=0)
    y_std = np.where(y_std < 1e-6, 1.0, y_std)
    return {
        "features": FEATURE_COLS,
        "mean": x_mean.astype(float).tolist(),
        "std": x_std.astype(float).tolist(),
        "target_names": TARGET_COLS,
        "target_mean": y_mean.astype(float).tolist(),
        "target_std": y_std.astype(float).tolist(),
        "soc_min": SOC_MIN,
        "battery_wh": BATTERY_WH,
        "residual_p_net": True,
    }


def _arrays(scaler: dict) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    return (
        np.asarray(scaler["mean"], dtype=np.float32),
        np.asarray(scaler["std"], dtype=np.float32),
        np.asarray(scaler["target_mean"], dtype=np.float32),
        np.asarray(scaler["target_std"], dtype=np.float32),
    )


def _train_torch(
    x_tr: np.ndarray,
    y_tr: np.ndarray,
    m_tr: np.ndarray,
    x_va: np.ndarray,
    y_va: np.ndarray,
    m_va: np.ndarray,
    scaler: dict,
    epochs: int,
    batch: int,
    lr: float,
    hidden: int,
    seed: int,
) -> tuple[object, dict]:
    torch, nn = try_import_torch()
    assert torch is not None and nn is not None
    torch.manual_seed(seed)
    np.random.seed(seed)

    x_mean, x_std, y_mean, y_std = _arrays(scaler)
    device = torch.device("cpu")
    model = build_mlp(hidden).to(device)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    huber = nn.SmoothL1Loss(reduction="none")

    y_tr_fill = np.where(np.isfinite(y_tr), y_tr, y_mean)
    xt = torch.from_numpy(np.nan_to_num(_standardize(x_tr, x_mean, x_std), nan=0.0).astype(np.float32))
    yt = torch.from_numpy(np.nan_to_num(_standardize(y_tr_fill, y_mean, y_std), nan=0.0).astype(np.float32))
    mt = torch.from_numpy(m_tr.astype(np.float32))
    xv = torch.from_numpy(np.nan_to_num(_standardize(x_va, x_mean, x_std), nan=0.0).astype(np.float32))
    yv = torch.from_numpy(_standardize(y_va, y_mean, y_std).astype(np.float32))
    n = xt.shape[0]
    history: list[dict] = []
    best_state = None
    best_val = float("inf")
    patience = 20
    stale = 0

    for epoch in range(1, epochs + 1):
        model.train()
        # shuffle training *rows* only (split is session-held-out; no time leak)
        perm = torch.randperm(n)
        losses = []
        for start in range(0, n, batch):
            idx = perm[start : start + batch]
            pred = model(xt[idx])
            tgt = yt[idx]
            finite = torch.isfinite(tgt)
            tgt = torch.where(finite, tgt, torch.zeros_like(tgt))
            raw = huber(pred, tgt)
            w = finite.float()
            w[:, 0] = w[:, 0] * mt[idx]
            w[:, 1] = 0.15 * w[:, 1]
            w[:, 2] = 0.15 * w[:, 2]
            loss = (raw * w).sum() / w.sum().clamp_min(1.0)
            if not torch.isfinite(loss):
                continue
            opt.zero_grad()
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            losses.append(float(loss.item()))

        model.eval()
        with torch.no_grad():
            pred_v = model(xv).numpy()
        pred_v = pred_v * y_std + y_mean
        pred_v[:, 1] = np.maximum(pred_v[:, 1], 0.0)
        pred_v[:, 2] = np.maximum(pred_v[:, 2], 0.0)
        vm = _metrics(y_va, pred_v, m_va)
        val_score = vm["p_net_w_mae"]
        train_loss = float(np.mean(losses)) if losses else float("nan")
        history.append({"epoch": epoch, "train_loss": train_loss, **vm})
        if epoch == 1 or epoch % 10 == 0 or val_score < best_val:
            print(
                f"  epoch {epoch:3d}  loss {train_loss:.4f}  "
                f"val p_net MAE {vm['p_net_w_mae']:.1f} W  "
                f"remain_s MAE {vm['remain_s_mae']:.1f} s  "
                f"remain_m MAE {vm['remain_m_mae']:.1f} m"
            )
        if val_score < best_val - 1e-3:
            best_val = val_score
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
            stale = 0
        else:
            stale += 1
            if stale >= patience:
                print(f"  early stop at epoch {epoch} (best p_net MAE {best_val:.1f} W)")
                break

    if best_state is not None:
        model.load_state_dict(best_state)
    return model, {"history": history, "best_val_score": best_val, "epochs_ran": epoch}


def _predict_torch(model, x: np.ndarray, scaler: dict) -> np.ndarray:
    torch, _ = try_import_torch()
    x_mean, x_std, y_mean, y_std = _arrays(scaler)
    model.eval()
    with torch.no_grad():
        xt = torch.from_numpy(_standardize(x, x_mean, x_std).astype(np.float32))
        pred = model(xt).numpy()
    pred = pred * y_std + y_mean
    pred[:, 1] = np.maximum(pred[:, 1], 0.0)
    pred[:, 2] = np.maximum(pred[:, 2], 0.0)
    return pred


def train(
    catalog: Path,
    cache_dir: Path,
    dates: list[str],
    rebuild: bool,
    epochs: int,
    batch: int,
    lr: float,
    hidden: int,
    seed: int,
) -> dict:
    df = build_dataset(catalog, cache_dir, dates=dates, rebuild=rebuild)
    train_df = df[df["split"] == "train"].copy()
    val_df = df[df["split"] == "val"].copy()
    sanity_df = df[df["split"] == "sanity"].copy()
    if train_df.empty or val_df.empty:
        raise SystemExit(f"Need train and val rows; got {len(train_df)} / {len(val_df)}")

    x_tr, y_tr, m_tr = feature_matrix(train_df, p_col="p_net_smooth")
    x_va, y_va, m_va = feature_matrix(val_df, p_col="p_net_w")
    _, y_va_raw, _ = feature_matrix(val_df, p_col="p_net_w")
    _, y_tr_raw, _ = feature_matrix(train_df, p_col="p_net_w")
    p_phys_tr = train_df["p_phys_w"].fillna(0.0).to_numpy(np.float32)
    p_phys_va = val_df["p_phys_w"].fillna(0.0).to_numpy(np.float32)
    y_tr = y_tr.copy()
    y_va_fit = y_va.copy()
    y_tr[:, 0] = y_tr[:, 0] - p_phys_tr
    y_va_fit[:, 0] = y_va[:, 0] - p_phys_va
    scaler = _fit_scaler(x_tr, y_tr, m_tr)
    scaler["split"] = {
        "rule": f"hold out {VAL_SESSION_ID}; other 2026-07-23 = train; 18/19 = sanity",
        "val_session": VAL_SESSION_ID,
        "train_sessions": sorted(train_df["session_id"].unique().tolist()),
        "train_rows": int(len(train_df)),
        "val_rows": int(len(val_df)),
        "sanity_rows": int(len(sanity_df)),
    }

    torch, _ = try_import_torch()
    backend = "torch" if torch is not None else "ridge"
    t0 = time.perf_counter()

    if torch is not None:
        print(f"Training tiny MLP ({hidden}x{hidden}) on CPU, {len(train_df)} train / {len(val_df)} val")
        model, train_info = _train_torch(
            x_tr, y_tr, m_tr, x_va, y_va_fit, m_va, scaler, epochs, batch, lr, hidden, seed
        )
        pred_tr = _predict_torch(model, x_tr, scaler)
        pred_va = _predict_torch(model, x_va, scaler)
        pred_tr[:, 0] = pred_tr[:, 0] + p_phys_tr
        pred_va[:, 0] = pred_va[:, 0] + p_phys_va
        weights_path = cache_dir / "charge_model.pt"
        torch.save(
            {"hidden": hidden, "state_dict": model.state_dict(), "backend": "torch", "residual_p_net": True},
            weights_path,
        )
    else:
        print("torch missing — fitting numpy ridge fallback (same features / infer API)")
        x_mean, x_std, y_mean, y_std = _arrays(scaler)
        xs = _standardize(x_tr, x_mean, x_std)
        ys = _standardize(y_tr, y_mean, y_std)
        ys[:, 0] = np.where(m_tr.astype(bool), ys[:, 0], 0.0)
        w = np.where(m_tr.astype(bool), 1.0, 0.35)
        model = RidgeChargeModel(l2=1.0)
        model.fit(xs, ys, sample_weight=w)
        pred_tr = model.predict(_standardize(x_tr, x_mean, x_std)) * y_std + y_mean
        pred_va = model.predict(_standardize(x_va, x_mean, x_std)) * y_std + y_mean
        pred_tr[:, 1:] = np.maximum(pred_tr[:, 1:], 0.0)
        pred_va[:, 1:] = np.maximum(pred_va[:, 1:], 0.0)
        train_info = {"history": [], "backend": "ridge"}
        weights_path = cache_dir / "charge_model.pt"
        (cache_dir / "charge_model_ridge.json").write_text(json.dumps(model.state_dict()), encoding="utf-8")
        # still write a marker pt-less sidecar; infer.py reads ridge json
        weights_path.write_text(json.dumps({"backend": "ridge", "path": "charge_model_ridge.json"}), encoding="utf-8")

    def _with_hybrid(pred: np.ndarray, frame: pd.DataFrame) -> np.ndarray:
        rwh = frame["remaining_wh_floor"].fillna(0.0).to_numpy(np.float64) if "remaining_wh_floor" in frame.columns else frame["remaining_wh"].fillna(0.0).to_numpy(np.float64)
        speed = frame["speed"].fillna(0.0).to_numpy(np.float64)
        remain_s, remain_m = hybrid_remain(rwh, pred[:, 0], speed)
        out = pred.copy()
        out[:, 1] = remain_s
        out[:, 2] = remain_m
        return out

    pred_tr = _with_hybrid(pred_tr, train_df)
    pred_va = _with_hybrid(pred_va, val_df)
    y_empty_tr = y_tr_raw.copy()
    y_empty_va = y_va_raw.copy()
    if "remain_s_empty" in train_df.columns:
        y_empty_tr[:, 1] = train_df["remain_s_empty"].to_numpy(np.float32)
        y_empty_tr[:, 2] = train_df["remain_m_empty"].to_numpy(np.float32)
        y_empty_va[:, 1] = val_df["remain_s_empty"].to_numpy(np.float32)
        y_empty_va[:, 2] = val_df["remain_m_empty"].to_numpy(np.float32)

    base_tr = _baselines(train_df)
    base_va = _baselines(val_df)
    if "remaining_wh_floor" in train_df.columns:
        base_tr[:, 1], base_tr[:, 2] = hybrid_remain(
            train_df["remaining_wh_floor"].fillna(0.0),
            np.where(np.isfinite(train_df["p_net_w"]), train_df["p_net_w"], train_df["p_phys_w"]),
            train_df["speed"].fillna(0.0),
        )
        base_va[:, 1], base_va[:, 2] = hybrid_remain(
            val_df["remaining_wh_floor"].fillna(0.0),
            np.where(np.isfinite(val_df["p_net_w"]), val_df["p_net_w"], val_df["p_phys_w"]),
            val_df["speed"].fillna(0.0),
        )
    naive_tr = _metrics(y_empty_tr, base_tr, m_tr, _drive_ok(train_df))
    naive_va = _metrics(y_empty_va, base_va, m_va, _drive_ok(val_df))
    model_tr = _metrics(y_empty_tr, pred_tr, m_tr, _drive_ok(train_df))
    model_va = _metrics(y_empty_va, pred_va, m_va, _drive_ok(val_df))

    sanity_metrics = None
    if len(sanity_df):
        x_s, y_s, m_s = feature_matrix(sanity_df)
        if torch is not None:
            pred_s = _predict_torch(model, x_s, scaler)
        else:
            x_mean, x_std, y_mean, y_std = _arrays(scaler)
            pred_s = model.predict(_standardize(x_s, x_mean, x_std)) * y_std + y_mean
            pred_s[:, 1:] = np.maximum(pred_s[:, 1:], 0.0)
        pred_s[:, 0] = pred_s[:, 0] + sanity_df["p_phys_w"].fillna(0.0).to_numpy(np.float32)
        pred_s = _with_hybrid(pred_s, sanity_df)
        y_s_empty = y_s.copy()
        if "remain_s_empty" in sanity_df.columns:
            y_s_empty[:, 1] = sanity_df["remain_s_empty"].to_numpy(np.float32)
            y_s_empty[:, 2] = sanity_df["remain_m_empty"].to_numpy(np.float32)
        base_s = _baselines(sanity_df)
        if "remaining_wh_floor" in sanity_df.columns:
            base_s[:, 1], base_s[:, 2] = hybrid_remain(
                sanity_df["remaining_wh_floor"].fillna(0.0),
                np.where(np.isfinite(sanity_df["p_net_w"]), sanity_df["p_net_w"], sanity_df["p_phys_w"]),
                sanity_df["speed"].fillna(0.0),
            )
        sanity_metrics = {
            "model": _metrics(y_s_empty, pred_s, m_s, _drive_ok(sanity_df)),
            "baseline": _metrics(y_s_empty, base_s, m_s, _drive_ok(sanity_df)),
        }

    metrics = {
        "backend": backend,
        "split": scaler["split"],
        "train": {"model": model_tr, "baseline": naive_tr},
        "val": {"model": model_va, "baseline": naive_va},
        "sanity_july_18_19": sanity_metrics,
        "baselines": {
            "physics_p_net_w": "0.5*1.225*0.12*v^3 + 0.0045*280*9.81*v - array_w",
            "naive_remain_s": "remaining_wh * 3600 / max(p_batt_now, 50)",
            "naive_remain_m": "naive_remain_s * speed",
        },
        "caveats": [
            "SOC is a voltage curve (estimatePackSoc), not coulomb-counted — noisy at rest and under load.",
            "remain_s / remain_m at infer are hybrid: (soc-0.10)*5000 Wh / max(predicted p_net, 50 W).",
            "Stint-end remain labels are not the infer target; every extract is censored before SOC 0.10.",
            "July 18/19 stints are short and low-speed; reported as sanity only.",
            "p_net_w label is pack_v * MC_BusCurrent (positive = discharging).",
        ],
        "train_seconds": time.perf_counter() - t0,
        "train_info": {k: train_info[k] for k in train_info if k != "history"} | {
            "last_epochs": train_info.get("history", [])[-3:]
        },
        "weights": str(weights_path),
    }

    scaler_path = cache_dir / "scaler.json"
    metrics_path = cache_dir / "metrics.json"
    scaler_path.write_text(json.dumps(scaler, indent=2), encoding="utf-8")
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")

    def _line(title: str, d: dict) -> None:
        print(
            f"  {title:18s}  p_net MAE {d['p_net_w_mae']:.1f} / RMSE {d['p_net_w_rmse']:.1f} W   "
            f"remain_s MAE {d['remain_s_mae']:.1f} / RMSE {d['remain_s_rmse']:.1f} s   "
            f"remain_m MAE {d['remain_m_mae']:.1f} / RMSE {d['remain_m_rmse']:.1f} m"
        )

    print()
    print(f"Backend: {backend}   val session: {VAL_SESSION_ID}")
    print("Train")
    _line("model", model_tr)
    _line("baseline", naive_tr)
    print("Val")
    _line("model", model_va)
    _line("baseline", naive_va)
    if sanity_metrics:
        print("Sanity (July 18/19)")
        _line("model", sanity_metrics["model"])
        _line("baseline", sanity_metrics["baseline"])
    print(f"Wrote {weights_path}")
    print(f"Wrote {scaler_path}")
    print(f"Wrote {metrics_path}")
    return metrics


def main() -> int:
    ap = argparse.ArgumentParser(description="Train remaining-range charge model")
    ap.add_argument("--catalog", type=Path, default=DEFAULT_CATALOG)
    ap.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    ap.add_argument("--dates", nargs="*", default=["2026-07-18", "2026-07-19", "2026-07-23"])
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument("--epochs", type=int, default=120)
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--lr", type=float, default=8e-4)
    ap.add_argument("--hidden", type=int, default=64)
    ap.add_argument("--seed", type=int, default=23)
    args = ap.parse_args()
    train(
        args.catalog,
        args.cache,
        args.dates,
        args.rebuild,
        args.epochs,
        args.batch,
        args.lr,
        args.hidden,
        args.seed,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
