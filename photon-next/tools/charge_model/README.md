# Remaining-range / charge model

Learned endurance model that sits **beside** C++ `tracksim` (do not rewrite the QSS solver). Trained on on-track VSD1 extracts. Question it answers: given pack state, speed, and array power, how much longer can we stay on track (time and distance) before SOC hits a floor.

SOC is the same voltage curve as `tracksim::estimatePackSoc`:

`soc = clip(((pack_v - 83.2) / 51.2) ** 0.55, 0, 1)`

Pack energy window matches `Car`: `batteryWh=5000`, `socMin=0.10`.

## Train / val split

**Hold out `2026-07-23-03`** (longest stint, ~120 min) as validation. All other **2026-07-23** sessions are train. Rows are **not** shuffled across time inside a stint for the split.

July 18 / 19 stints are short and low-speed — cached and scored as **sanity only**, not the train set.

## How to train

From this folder (CPU torch is enough):

```bat
cd F:\lhr\photon-next\backend\scripts\charge_model
python train.py
```

`train.py` builds the 1 Hz cache if missing, then writes:

| File | Where |
|---|---|
| `dataset.parquet` / `dataset.npz` / per-session parquet | `F:\lhr\_logs_extract\on_track\charge_model\` |
| `charge_model.pt` | same cache dir |
| `scaler.json` | same |
| `metrics.json` | same |

Rebuild the 1 Hz tables after a parser change:

```bat
python train.py --rebuild
```

Dataset only:

```bat
python dataset.py
```

Does **not** rescan the original 5 GB `F:\lhr\_logs_extract\logs` — only the already-extracted on-track session files.

## How to infer (Track Sim later)

Standalone Python, no new protocol messages, not wired into WASM / C++:

```bat
python infer.py --speed 12 --pack-v 112 --pack-i 18 --array-w 700 --soc 0.72
```

`--soc` is optional; default is `estimatePackSoc(pack_v)`.

```python
from infer import load, predict
load()  # reads cache dir
predict(speed=12.0, pack_v=112.0, pack_i=18.0, array_w=700.0, soc=0.72)
# -> {"p_net_w": ..., "remain_s": ..., "remain_m": ...}
```

Track Sim / Pit can call that later with live `MeasuredDrive` fields (`speedMpsMean`, `packVMean`, `packIMean`, `arrayWMean`, `socEst`). Keep the C++ QSS solver as the physics path.

## Features / heads

Inputs (no bus current — that made `p = V*I` a cheat):

`speed, pack_v, array_w, soc, v^3, p_phys_w`

The MLP predicts a **residual on QSS physics** (`p_net = p_phys + f(x)`). Time/distance are **not** learned stint lengths. At infer:

`remain_s = (soc - 0.10) * 5000 Wh * 3600 / max(p, 50 W)`

If `pack_i` is present and non-zero, remain uses live `V*I`. If current is missing (some July 23 stints), remain uses the model power.

Baselines in `metrics.json` (remain scored only while `speed≥5` and `p≥200 W`):

- Physics: `0.5*1.225*0.12*v^3 + 0.0045*280*9.81*v - array_w`
- Naive remain: same hybrid formula with **measured** pack power

## Caveats

- Voltage SOC is noisy (IR drop, rest recovery). Remaining energy is not coulomb-counted. Lowest July 23 pack V in the 1 Hz tables is ~92 V → SOC ≈ 0.39, never near `soc_min=0.10`.
- **Every extracted stint is censored** (`censored=1`): the car stopped before SOC 0.10, so `remain_s` / `remain_m` are time/distance-to-end-of-stint, not true pack-empty endurance.
- July 18/19 have `array_w=0` (no MPPT in those logs) and are 1–3 min / <7 m/s — sanity only.
- Sessions `2026-07-23-07` and `-10` have measured `MC_BusCurrent` ≈ 0 while moving (CAN gap or motor telem off); `p_net_w` labels there are 0.
- Array power is `sum(Vin*Iin)` over MPPT A/B/C; missing MPPTs are omitted from the sum.
- July 18/19 GPS is often 0,0; July 23 GPS is Brainerd-area and sometimes drops to 0.

## Metrics

CPU MLP `64×64`, residual on physics, early-stop epoch 36. Val = `2026-07-23-03`. Remain MAE is vs energy÷power (time-to-floor), not time-to-end-of-stint.

| Split | Head | Model MAE | Baseline MAE |
|---|---|---:|---:|
| Val | pack power (no current in) | **156 W** | 1066 W physics |
| Val | time-to-floor (model P) | 40 min | **6.4 min** (measured P) |
| Val | distance-to-floor (model P) | 17.4 km | **2.7 km** (measured P) |

Without bus current the model is ~7× better than the QSS drag−array formula. With a live current sensor, use `V*I` for remain (infer does this). The 40 min vs 6 min gap is the cost of not having current.

Full dump: `F:\lhr\_logs_extract\on_track\charge_model\metrics.json`.
