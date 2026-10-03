#!/usr/bin/env python3
"""Tiny remaining-range MLP (or numpy ridge fallback).

Heads (positive p_net_w = SOC falling):
  p_net_w   net pack electrical power (W)
  remain_s  seconds of observed endurance left in the stint
  remain_m  metres of observed remaining distance in the stint
"""

from __future__ import annotations

from typing import Any

import numpy as np

FEATURE_DIM = 6
HEADS = 3


def try_import_torch():
    try:
        import torch
        import torch.nn as nn

        return torch, nn
    except Exception:
        return None, None


def build_mlp(hidden: int = 32):
    """Return a torch.nn.Module. Raises if torch is missing."""
    torch, nn = try_import_torch()
    if nn is None:
        raise ImportError("torch is not installed")

    class ChargeMLP(nn.Module):
        def __init__(self, h: int) -> None:
            super().__init__()
            self.hidden = h
            self.net = nn.Sequential(
                nn.Linear(FEATURE_DIM, h),
                nn.ReLU(),
                nn.Linear(h, h),
                nn.ReLU(),
                nn.Linear(h, HEADS),
            )

        def forward(self, x):
            return self.net(x)

    return ChargeMLP(hidden)


class RidgeChargeModel:
    """Closed-form multi-output ridge if torch is unavailable."""

    def __init__(self, l2: float = 1.0) -> None:
        self.l2 = l2
        self.coef_: np.ndarray | None = None  # (F+1, 3)

    def fit(self, x: np.ndarray, y: np.ndarray, sample_weight: np.ndarray | None = None) -> None:
        n, f = x.shape
        xb = np.concatenate([x, np.ones((n, 1), dtype=np.float64)], axis=1)
        w = np.ones(n, dtype=np.float64) if sample_weight is None else sample_weight.astype(np.float64)
        xw = xb * w[:, None]
        a = xw.T @ xb
        a += self.l2 * np.eye(f + 1)
        b = xw.T @ y.astype(np.float64)
        self.coef_ = np.linalg.solve(a, b)

    def predict(self, x: np.ndarray) -> np.ndarray:
        if self.coef_ is None:
            raise RuntimeError("RidgeChargeModel.fit() has not been called")
        xb = np.concatenate([x, np.ones((x.shape[0], 1), dtype=np.float64)], axis=1)
        return xb @ self.coef_

    def state_dict(self) -> dict[str, Any]:
        return {
            "kind": "ridge",
            "l2": self.l2,
            "coef": None if self.coef_ is None else self.coef_.tolist(),
        }

    def load_state_dict(self, state: dict[str, Any]) -> None:
        self.l2 = float(state.get("l2", 1.0))
        coef = state.get("coef")
        self.coef_ = None if coef is None else np.asarray(coef, dtype=np.float64)
