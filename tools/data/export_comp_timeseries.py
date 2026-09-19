#!/usr/bin/env python3
"""Build a shareable zip of 1 Hz competition / on-track power time series."""

from __future__ import annotations

import shutil
import zipfile
from pathlib import Path
from zoneinfo import ZoneInfo

import numpy as np
import pandas as pd

SRC = Path(r"F:\lhr\_logs_extract\on_track\charge_model\dataset.parquet")
OUT_ROOT = Path(r"F:\lhr\_logs_extract\on_track\_comp_timeseries_export")
ZIP_PATH = Path(r"F:\lhr\_logs_extract\on_track\comp-power-timeseries.zip")
CHICAGO = ZoneInfo("America/Chicago")

README = """Longhorn Racing — competition / on-track power time series
==========================================================

1 Hz tables derived from recorded CAN (VSD1) for FSGP week, Brainerd.

Source: F:\\lhr\\_logs_extract\\on_track\\charge_model\\dataset.parquet
On-track windows: speed >= 2 m/s, >= 45 s, gaps <= 180 s merged.
Timezone: timestamps are unix ms; chicago_iso is America/Chicago.

What is in here
---------------
comp/                 July 23 2026 race-day stints (Brainerd). THIS is competition.
  2026-07-23-all.csv  all 10 stints concatenated
  2026-07-23-all.parquet
  2026-07-23-NN.csv   one file per stint
other_on_track/       July 18/19 short low-speed stints, no MPPT. Not race data.
summary.csv           per-stint aggregates

Columns (all CSV)
-----------------
session_id         stint id, e.g. 2026-07-23-03
date               local calendar date
utc_iso            UTC timestamp
chicago_iso        America/Chicago timestamp
t_s                seconds from start of this stint (1 Hz)
ts_ms              unix milliseconds (car CANP clock)
speed_mps          motor/GPS speed, m/s
speed_kph          same, km/h
pack_v             BPS Main_Battery_Voltage, V
pack_i_a           MC_BusCurrent, A (motor-controller DC bus; pack-current proxy)
pack_power_w       pack_v * pack_i_a, W  (positive = discharging)
array_power_w      sum of MPPT A/B/C Vin*Iin, W
soc                voltage SOC: clip(((pack_v-83.2)/51.2)^0.55, 0, 1)
pack_energy_wh     cumulative integral of pack_power_w / 3600 from stint start
array_energy_wh    cumulative integral of array_power_w / 3600 from stint start
lat, lon           GPS (zeros treated as missing)
have_pack_current  1 if MC_BusCurrent was seen in this stint
have_array         1 if any MPPT power was seen in this stint

There is no dedicated PowerDraw CAN signal. Pack power is bus V * MC_BusCurrent.
There is no pack shunt in the DBC. MC_BusCurrent is current drawn from the DC
bus by the controller — motor-controller draw, not whole-car LV/aux.

Do not use
----------
2026-07-23-07 and 2026-07-23-10: MC_BusCurrent is ~0 while moving, so
pack_power_w is not usable. Speed / GPS / array still valid.

July 18 and 19: not competition. array_power_w is 0 (no MPPT in those logs).

GPS dropouts appear as empty lat/lon (original logs sometimes stamp 0,0).

Suggested plot
--------------
x = chicago_iso or t_s
y = pack_power_w, array_power_w, speed_kph
Longest race stint is 2026-07-23-03 (~2 h, mean pack ~814 W, mean array ~357 W).
"""

COLS = [
    "session_id",
    "date",
    "utc_iso",
    "chicago_iso",
    "t_s",
    "ts_ms",
    "speed_mps",
    "speed_kph",
    "pack_v",
    "pack_i_a",
    "pack_power_w",
    "array_power_w",
    "soc",
    "pack_energy_wh",
    "array_energy_wh",
    "lat",
    "lon",
    "have_pack_current",
    "have_array",
]


def build_frame() -> pd.DataFrame:
    df = pd.read_parquet(SRC)
    lat = df["lat"].to_numpy(dtype=np.float64)
    lon = df["lon"].to_numpy(dtype=np.float64)
    gps_bad = (
        (~np.isfinite(lat))
        | (~np.isfinite(lon))
        | ((np.abs(lat) < 1e-6) & (np.abs(lon) < 1e-6))
    )
    lat = lat.copy()
    lon = lon.copy()
    lat[gps_bad] = np.nan
    lon[gps_bad] = np.nan

    ts_ms = pd.to_numeric(df["ts_ms"], errors="coerce")
    t_s = df["t_s"].to_numpy(dtype=np.int64)
    # Bins with no CANP stamp fall back to the 1 Hz bin epoch.
    missing_ts = ~np.isfinite(ts_ms.to_numpy(dtype=np.float64))
    if missing_ts.any():
        session_start = df.groupby("session_id")["ts_ms"].transform("min")
        fallback = pd.to_numeric(session_start, errors="coerce") + (t_s * 1000)
        ts_ms = ts_ms.where(~missing_ts, fallback)
    ts_ms = ts_ms.round().astype("int64")
    utc = pd.to_datetime(ts_ms.to_numpy(), unit="ms", utc=True)
    local = utc.tz_convert(CHICAGO)

    pack_i = df["pack_i"].to_numpy(dtype=np.float64)
    have_i = df["have_pack_i"].to_numpy() > 0
    p_net = df["p_net_w"].to_numpy(dtype=np.float64)
    p_net = np.where(have_i & np.isfinite(p_net), p_net, np.nan)

    out = pd.DataFrame(
        {
            "session_id": df["session_id"].astype(str),
            "date": df["date"].astype(str),
            "utc_iso": utc.strftime("%Y-%m-%dT%H:%M:%SZ"),
            "chicago_iso": local.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "t_s": t_s,
            "ts_ms": ts_ms.to_numpy(),
            "speed_mps": df["speed"].astype(np.float64),
            "speed_kph": df["speed"].astype(np.float64) * 3.6,
            "pack_v": df["pack_v"].astype(np.float64),
            "pack_i_a": np.where(have_i, pack_i, np.nan),
            "pack_power_w": p_net,
            "array_power_w": df["array_w"].astype(np.float64),
            "soc": df["soc"].astype(np.float64),
            "lat": lat,
            "lon": lon,
            "have_pack_current": have_i.astype(np.int8),
            "have_array": (df["have_array"].to_numpy() > 0).astype(np.int8),
        }
    )

    out["pack_energy_wh"] = np.nan
    out["array_energy_wh"] = np.nan
    for _, g in out.groupby("session_id", sort=False):
        idx = g.index
        p = g["pack_power_w"].to_numpy()
        a = g["array_power_w"].to_numpy()
        p_fill = np.where(np.isfinite(p), p, 0.0)
        a_fill = np.where(np.isfinite(a), a, 0.0)
        out.loc[idx, "pack_energy_wh"] = np.cumsum(p_fill) / 3600.0
        out.loc[idx, "array_energy_wh"] = np.cumsum(a_fill) / 3600.0
    return out[COLS]


def session_notes(sid: str, g: pd.DataFrame) -> str:
    notes: list[str] = []
    p = g["pack_power_w"]
    a = g["array_power_w"]
    moving = g["speed_mps"] >= 2.0
    mean_abs_p = float(p.fillna(0).abs().mean())
    if sid.endswith(("-07", "-10")) or (mean_abs_p < 1 and bool(moving.any())):
        notes.append("MC_BusCurrent missing/zero while moving — pack_power_w not usable")
    if g["date"].iloc[0] != "2026-07-23":
        notes.append("not competition (short/low-speed, no MPPT)")
    if int(g["have_array"].max()) == 0 or float(a.fillna(0).abs().mean()) < 1:
        notes.append("no MPPT array power in this stint")
    return "; ".join(notes)


def build_summary(out: pd.DataFrame) -> pd.DataFrame:
    rows = []
    for sid, g in out.groupby("session_id", sort=True):
        p = g["pack_power_w"]
        a = g["array_power_w"]
        spd = g["speed_mps"]
        gps_ok = g["lat"].notna() & g["lon"].notna()
        rows.append(
            {
                "session_id": sid,
                "date": g["date"].iloc[0],
                "is_comp": int(g["date"].iloc[0] == "2026-07-23"),
                "start_utc": g["utc_iso"].iloc[0],
                "end_utc": g["utc_iso"].iloc[-1],
                "duration_s": int(g["t_s"].iloc[-1] - g["t_s"].iloc[0] + 1),
                "samples": len(g),
                "speed_mean_mps": float(spd.mean()),
                "speed_max_mps": float(spd.max()),
                "speed_mean_kph": float(spd.mean() * 3.6),
                "distance_km": float(spd.clip(lower=0).sum() / 1000.0),
                "pack_v_mean": float(g["pack_v"].mean()),
                "soc_start": float(g["soc"].iloc[0]),
                "soc_end": float(g["soc"].iloc[-1]),
                "pack_power_mean_w": float(p.mean()) if p.notna().any() else np.nan,
                "pack_power_max_w": float(p.max()) if p.notna().any() else np.nan,
                "pack_energy_wh": float(g["pack_energy_wh"].iloc[-1]),
                "array_power_mean_w": float(a.mean()) if a.notna().any() else np.nan,
                "array_energy_wh": float(g["array_energy_wh"].iloc[-1]),
                "gps_samples": int(gps_ok.sum()),
                "pack_current_ok": int(
                    p.notna().any() and float(p.fillna(0).abs().mean()) > 10.0
                ),
                "notes": session_notes(sid, g),
            }
        )
    return pd.DataFrame(rows)


def main() -> None:
    if not SRC.exists():
        raise SystemExit(f"missing source parquet: {SRC}")
    if OUT_ROOT.exists():
        shutil.rmtree(OUT_ROOT)
    (OUT_ROOT / "comp").mkdir(parents=True)
    (OUT_ROOT / "other_on_track").mkdir()

    out = build_frame()
    comp = out[out["date"] == "2026-07-23"].copy()
    other = out[out["date"] != "2026-07-23"].copy()

    comp.to_csv(OUT_ROOT / "comp" / "2026-07-23-all.csv", index=False, float_format="%.6g")
    comp.to_parquet(OUT_ROOT / "comp" / "2026-07-23-all.parquet", index=False)
    for sid, g in comp.groupby("session_id", sort=True):
        g.to_csv(OUT_ROOT / "comp" / f"{sid}.csv", index=False, float_format="%.6g")
    for sid, g in other.groupby("session_id", sort=True):
        g.to_csv(OUT_ROOT / "other_on_track" / f"{sid}.csv", index=False, float_format="%.6g")

    summary = build_summary(out)
    summary.to_csv(OUT_ROOT / "summary.csv", index=False, float_format="%.6g")
    (OUT_ROOT / "README.txt").write_text(README, encoding="utf-8")

    if ZIP_PATH.exists():
        ZIP_PATH.unlink()
    with zipfile.ZipFile(ZIP_PATH, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for path in sorted(OUT_ROOT.rglob("*")):
            if path.is_file():
                zf.write(path, path.relative_to(OUT_ROOT).as_posix())

    print(f"comp rows {len(comp)}  other rows {len(other)}")
    print(summary.to_string(index=False))
    print(f"zip {ZIP_PATH}  {ZIP_PATH.stat().st_size} bytes")


if __name__ == "__main__":
    main()
