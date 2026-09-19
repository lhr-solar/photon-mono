"""Validate a multi-gigabyte photon-export file without materializing it.

validate.py reads the whole table into pandas, which is right for a slice and
impossible for a whole-session raw export. This checks the same file with
aggregate queries and a few sampled row groups instead, so the claim that a
4 GB / 458-row-group file is genuinely readable is tested rather than assumed.

Usage:
  python validate_large.py --parquet FILE --exe photon-export.exe --store DIR
                           --session ID [--expect-rows N]
"""

from __future__ import annotations

import argparse
import csv
import io
import math
import subprocess
import sys

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

import duckdb

failures: list[str] = []
checks = 0


def check(cond: bool, label: str) -> bool:
    global checks
    checks += 1
    if not cond:
        failures.append(label)
        print(f"  FAIL {label}")
    else:
        print(f"  ok   {label}")
    return bool(cond)


def probe(exe, store, session, signal_id, at_ms, window_ms) -> pd.DataFrame:
    out = subprocess.run(
        [exe, "--store", store, "--session", session, "--signals", str(signal_id),
         "--probe", str(at_ms), "--probe-window", str(window_ms)],
        capture_output=True, text=True, check=True,
    ).stdout
    rows = list(csv.DictReader(io.StringIO(out), delimiter="\t"))
    df = pd.DataFrame(rows)
    for col in ("signal_id", "t_ms", "raw", "level"):
        df[col] = df[col].astype("int64")
    df["value"] = df["value"].astype("float64")
    return df


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--store", required=True)
    ap.add_argument("--session", required=True)
    ap.add_argument("--expect-rows", type=int, default=0)
    args = ap.parse_args()
    duck_path = args.parquet.replace("\\", "/")

    pf = pq.ParquetFile(args.parquet)
    meta = pf.metadata
    print(f"== {args.parquet}")
    print(f"   num_rows={meta.num_rows:,} row_groups={meta.num_row_groups} "
          f"columns={meta.num_columns} serialized_size={meta.serialized_size:,}")
    kv = {k.decode(): v.decode() for k, v in (meta.metadata or {}).items()}

    print("\n-- footer")
    if args.expect_rows:
        check(meta.num_rows == args.expect_rows,
              f"footer num_rows is {args.expect_rows:,} (got {meta.num_rows:,})")
    check(meta.num_row_groups > 1, f"file has {meta.num_row_groups} row groups")
    rg_rows = sum(meta.row_group(g).num_rows for g in range(meta.num_row_groups))
    check(rg_rows == meta.num_rows, "row-group row counts sum to footer num_rows")

    print("\n-- duckdb reads every row group")
    agg = duckdb.sql(f"""
        select count(*) n, count(distinct signal_id) sigs,
               min(t_ms) tmin, max(t_ms) tmax,
               count(value) vals, count(v_min) mins, min(level) lmin, max(level) lmax
        from read_parquet('{duck_path}')
    """).df().iloc[0]
    print(f"   duckdb: n={int(agg['n']):,} signals={int(agg['sigs'])} "
          f"levels=[{int(agg['lmin'])},{int(agg['lmax'])}]")
    check(int(agg["n"]) == meta.num_rows, "duckdb counts the same number of rows")
    from_ms, to_ms = int(kv["photon.from_ms"]), int(kv["photon.to_ms"])
    tmin = int(pd.Timestamp(agg["tmin"]).value // 1_000_000)
    tmax = int(pd.Timestamp(agg["tmax"]).value // 1_000_000)
    check(from_ms <= tmin and tmax <= to_ms,
          f"duckdb timestamps [{tmin}, {tmax}] lie inside the requested window")
    if int(agg["lmax"]) == 0:
        check(int(agg["vals"]) == meta.num_rows, "every L0 row carries a value")
        check(int(agg["mins"]) == 0, "no L0 row carries an envelope")

    print("\n-- sidecar row counts vs duckdb group-by")
    sidecar = args.parquet.rsplit(".", 1)[0] + ".signals.parquet"
    side = pq.read_table(sidecar).to_pandas()
    per = duckdb.sql(f"select signal_id, count(*) n from read_parquet('{duck_path}') "
                     f"group by 1").df()
    per = dict(zip(per["signal_id"].astype("int64"), per["n"].astype("int64")))
    mismatched = [(int(r["signal_id"]), int(r["rows"]), per.get(int(r["signal_id"]), 0))
                  for _, r in side.iterrows()
                  if int(r["rows"]) != per.get(int(r["signal_id"]), 0)]
    check(not mismatched, f"all {len(side)} sidecar row counts match duckdb "
                          f"(mismatches: {mismatched[:3]})")
    check(sum(per.values()) == meta.num_rows, "per-signal counts sum to num_rows")

    print("\n-- sampled row groups read with pyarrow")
    picks = sorted({0, meta.num_row_groups // 2, meta.num_row_groups - 1})
    for g in picks:
        tbl = pf.read_row_group(g)
        check(tbl.num_rows == meta.row_group(g).num_rows,
              f"row group {g} reads back {tbl.num_rows} rows")
        check(tbl.column("signal_id").null_count == 0,
              f"row group {g} has no null signal_id")

    print("\n-- deep value check against Store::query()")
    # Pick a busy signal and compare a 2 s window read out of the middle of the
    # file against what the query engine returns for the same window.
    sig = int(side.sort_values("rows", ascending=False)["signal_id"].iloc[0])
    mid = (from_ms + to_ms) // 2
    lo, hi = mid - 1000, mid + 1000
    q = duckdb.sql(f"""
        select epoch_ms(t_ms) t, raw, value
        from read_parquet('{duck_path}')
        where signal_id = {sig}
          and epoch_ms(t_ms) between {lo} and {hi}
        order by t, raw
    """).df()
    p = probe(args.exe, args.store, args.session, sig, mid, 1000)
    p = p[(p["t_ms"] >= lo) & (p["t_ms"] <= hi)].sort_values(["t_ms", "raw"])
    print(f"   signal {sig} over [{lo}, {hi}]: parquet {len(q)} rows, "
          f"query engine {len(p)} rows")
    if check(len(q) == len(p) and len(p) > 0, "same sample count in the window"):
        check(q["t"].astype("int64").tolist() == p["t_ms"].tolist(),
              "every timestamp matches")
        check(q["raw"].astype("int64").tolist() == p["raw"].tolist(),
              "every raw integer matches")
        check(all(math.isclose(a, b, rel_tol=0, abs_tol=1e-12)
                  for a, b in zip(q["value"], p["value"])), "every physical value matches")

    print(f"\n== {checks - len(failures)} passed, {len(failures)} failed")
    for f in failures:
        print(f"   FAILED: {f}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
