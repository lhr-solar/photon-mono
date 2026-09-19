"""Validate a photon-export Parquet file with real Parquet tooling.

Reads the file back with pyarrow, duckdb and fastparquet -- three independent
implementations -- and checks the schema, row count, null pattern, row-group
statistics, and the actual values against what the TSDB query engine returns.

The value spot-check deliberately goes through `photon-export --probe`, which
runs Store::query(), while the export itself scans level pages directly. So the
two paths through the store are compared, not one path against itself.

Usage:
  python validate.py --parquet FILE --exe photon-export.exe --store DIR --session ID
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import subprocess
import sys

import fastparquet
import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq

import duckdb

BUCKET_MS = {0: 0, 1: 100, 2: 1000, 3: 10000, 4: 60000, 5: 600000}

EXPECTED_DATA_SCHEMA = [
    ("signal_id", "uint32", False),
    ("t_ms", "timestamp[ms, tz=UTC]", False),
    ("level", "int32", False),
    ("bucket_ms", "int32", False),
    ("value", "double", True),
    ("raw", "int64", True),
    ("v_min", "double", True),
    ("v_max", "double", True),
    ("v_mean", "double", True),
    ("v_first", "double", True),
    ("v_last", "double", True),
    ("count", "int64", True),
]

failures: list[str] = []
checks = 0


def check(cond: bool, label: str) -> bool:
    global checks
    checks += 1
    if not cond:
        failures.append(label)
        print(f"  FAIL {label}")
    return bool(cond)


def probe(exe: str, store: str, session: str, signal_id: int, at_ms: int, window_ms: int) -> pd.DataFrame:
    out = subprocess.run(
        [exe, "--store", store, "--session", session, "--signals", str(signal_id),
         "--probe", str(at_ms), "--probe-window", str(window_ms)],
        capture_output=True, text=True, check=True,
    ).stdout
    rows = list(csv.DictReader(io.StringIO(out), delimiter="\t"))
    if not rows:
        return pd.DataFrame(columns=["signal_id", "t_ms", "raw", "value", "level"])
    df = pd.DataFrame(rows)
    for col in ("signal_id", "t_ms", "raw", "level"):
        df[col] = df[col].astype("int64")
    df["value"] = df["value"].astype("float64")
    return df


def to_frame(table: pa.Table) -> pd.DataFrame:
    """Arrow table -> pandas with t_ms back in integer milliseconds."""
    cols = {}
    for name in table.column_names:
        col = table.column(name)
        if pa.types.is_timestamp(col.type):
            cols[name] = col.cast(pa.int64()).to_pandas()
        else:
            cols[name] = col.to_pandas()
    return pd.DataFrame(cols)


def epoch_ms(series: pd.Series) -> list[int]:
    """Datetime column -> epoch milliseconds, whatever resolution it arrived in."""
    return (pd.to_datetime(series, utc=True).astype("datetime64[ms, UTC]")
            .astype("int64").tolist())


def decode_stat(raw: bytes | None, physical: str) -> object:
    """A PLAIN-encoded single statistics value, as stored in the thrift footer."""
    if raw is None:
        return None
    if physical == "INT32":
        return int.from_bytes(raw[:4], "little", signed=True)
    if physical == "INT64":
        return int.from_bytes(raw[:8], "little", signed=True)
    if physical == "DOUBLE":
        import struct
        return struct.unpack("<d", raw[:8])[0]
    if physical == "BOOLEAN":
        return bool(raw[0])
    return raw.decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--store", required=True)
    ap.add_argument("--session", required=True)
    ap.add_argument("--spot-checks", type=int, default=6)
    ap.add_argument("--l0-signals", type=int, default=8,
                    help="how many L0 signals to cross-check sample-for-sample")
    args = ap.parse_args()

    print(f"== pyarrow {pa.__version__} / duckdb {duckdb.__version__} / "
          f"fastparquet {fastparquet.__version__} / pandas {pd.__version__}")
    print(f"== file {args.parquet}")

    pf = pq.ParquetFile(args.parquet)
    meta = pf.metadata
    schema = pf.schema_arrow
    print("\n-- schema as pyarrow sees it")
    print(schema)
    print(f"\n-- num_rows={meta.num_rows} row_groups={meta.num_row_groups} "
          f"created_by={meta.created_by!r}")

    print("\n-- schema check")
    check(len(schema) == len(EXPECTED_DATA_SCHEMA), "column count")
    for i, (name, typ, nullable) in enumerate(EXPECTED_DATA_SCHEMA):
        if i >= len(schema):
            break
        f = schema.field(i)
        check(f.name == name, f"column {i} name is {name!r} (got {f.name!r})")
        check(str(f.type) == typ, f"{name} type is {typ} (got {f.type})")
        check(f.nullable == nullable, f"{name} nullable is {nullable} (got {f.nullable})")

    print("\n-- read back with three independent readers")
    arrow_tbl = pq.read_table(args.parquet)
    df = to_frame(arrow_tbl)
    check(len(df) == meta.num_rows, "pyarrow row count matches footer num_rows")

    duck = duckdb.sql(
        f"select * from read_parquet('{args.parquet.replace(chr(92), '/')}')").df()
    check(len(duck) == meta.num_rows, "duckdb row count matches")
    fp = fastparquet.ParquetFile(args.parquet).to_pandas()
    check(len(fp) == meta.num_rows, "fastparquet row count matches")

    if meta.num_rows:
        for col in ("signal_id", "level", "bucket_ms"):
            check(int(duck[col].sum()) == int(df[col].sum()), f"duckdb agrees on sum({col})")
            check(int(fp[col].sum()) == int(df[col].sum()), f"fastparquet agrees on sum({col})")
        # duckdb/fastparquet return t_ms as datetimes; compare as epoch ms.
        check(epoch_ms(duck["t_ms"]) == df["t_ms"].tolist(), "duckdb agrees on every t_ms")
        check(epoch_ms(fp["t_ms"]) == df["t_ms"].tolist(), "fastparquet agrees on every t_ms")
        # A pushdown predicate on the unsigned id column must not lose rows.
        one = int(df["signal_id"].iloc[0])
        want = int((df["signal_id"] == one).sum())
        got = duckdb.sql(f"select count(*) c from read_parquet"
                         f"('{args.parquet.replace(chr(92), '/')}') "
                         f"where signal_id = {one}").df()["c"].iloc[0]
        check(int(got) == want,
              f"duckdb filter signal_id={one} returns {want} rows (got {int(got)})")
        for col in ("value", "raw", "v_min", "v_max", "v_mean", "v_first", "v_last", "count"):
            a = df[col]
            for label, other in (("duckdb", duck[col]), ("fastparquet", fp[col])):
                check(a.isna().tolist() == other.isna().tolist(), f"{label} agrees on {col} nulls")
                m = ~a.isna()
                if m.any():
                    check(bool((a[m].to_numpy() == other[m].to_numpy()).all()),
                          f"{label} agrees on {col} values")

    print("\n-- footer key/value metadata")
    kv = {k.decode(): v.decode() for k, v in (meta.metadata or {}).items()}
    for k in sorted(kv):
        shown = kv[k] if len(kv[k]) < 120 else kv[k][:117] + "..."
        print(f"  {k} = {shown}")
    check(kv.get("photon.session_id") == args.session, "photon.session_id matches --session")
    check("photon.from_ms" in kv and "photon.to_ms" in kv, "window recorded in metadata")
    boundaries = json.loads(kv.get("photon.boundaries", "[]"))
    check(isinstance(boundaries, list), "photon.boundaries parses as JSON")

    print("\n-- level / null-pattern invariants")
    if meta.num_rows:
        raw_rows = df[df["level"] == 0]
        roll_rows = df[df["level"] > 0]
        print(f"  L0 rows={len(raw_rows)} rollup rows={len(roll_rows)} "
              f"levels={sorted(df['level'].unique().tolist())}")
        if len(raw_rows):
            check(raw_rows["value"].notna().all(), "L0 rows always carry value")
            check(raw_rows["raw"].notna().all(), "L0 rows always carry raw")
            check(raw_rows["count"].isna().all(), "L0 rows carry no bucket count")
            check(raw_rows["v_min"].isna().all(), "L0 rows carry no envelope")
            check((raw_rows["bucket_ms"] == 0).all(), "L0 rows have bucket_ms 0")
        if len(roll_rows):
            check(roll_rows["value"].isna().all(), "rollup rows carry no raw value")
            check(roll_rows["raw"].isna().all(), "rollup rows carry no raw integer")
            check(roll_rows["count"].notna().all(), "rollup rows always carry count")
            check((roll_rows["count"] > 0).all(), "rollup buckets are non-empty")
            check(roll_rows["v_first"].notna().all(), "rollup rows always carry first/last")
            bad = [(int(l), int(b)) for l, b in
                   roll_rows[["level", "bucket_ms"]].drop_duplicates().to_numpy()
                   if BUCKET_MS.get(int(l)) != int(b)]
            check(not bad, f"bucket_ms matches the level table (mismatches: {bad})")
            env = roll_rows[roll_rows["v_min"].notna()]
            if len(env):
                check(bool((env["v_min"] <= env["v_max"]).all()), "v_min <= v_max")
                check(bool((env["v_mean"] >= env["v_min"] - 1e-9).all() and
                           (env["v_mean"] <= env["v_max"] + 1e-9).all()),
                      "v_mean lies within [v_min, v_max]")
        check(df.groupby("signal_id")["t_ms"].apply(lambda s: s.is_monotonic_increasing).all(),
              "t_ms is non-decreasing within each signal")
        from_ms, to_ms = int(kv["photon.from_ms"]), int(kv["photon.to_ms"])
        if len(raw_rows):
            check(bool((raw_rows["t_ms"] >= from_ms).all() and (raw_rows["t_ms"] <= to_ms).all()),
                  "every L0 timestamp is inside the requested window")
        if len(roll_rows):
            ends = roll_rows["t_ms"] + roll_rows["bucket_ms"]
            check(bool((roll_rows["t_ms"] <= to_ms).all() and (ends > from_ms).all()),
                  "every rollup bucket slot intersects the requested window")

    print("\n-- row-group statistics vs pandas")
    # Read the statistics straight out of the thrift footer via fastparquet, so
    # every column is checked. Arrow declines to surface statistics for the
    # UNSIGNED-sorted signal_id column because it does not recognise this writer,
    # which is a trust policy rather than a defect; the bytes are still there and
    # are still checked here.
    fpf = fastparquet.ParquetFile(args.parquet)
    stats_checked = 0
    unsigned_cols = {"signal_id"}
    for g in range(meta.num_row_groups):
        gdf = to_frame(pf.read_row_group(g))
        rg = meta.row_group(g)
        check(rg.num_rows == len(gdf), f"row group {g} num_rows")
        for c in range(rg.num_columns):
            col = rg.column(c)
            name = col.path_in_schema
            raw_stats = fpf.row_groups[g].columns[c].meta_data.statistics
            if raw_stats is None:
                continue
            physical = str(col.physical_type)
            series = gdf[name]
            check(int(raw_stats.null_count) == int(series.isna().sum()), f"rg{g}.{name} null_count")
            nn = series.dropna()
            smin = decode_stat(raw_stats.min_value, physical)
            smax = decode_stat(raw_stats.max_value, physical)
            if not len(nn):
                check(smin is None and smax is None,
                      f"rg{g}.{name} all-null chunk carries no bounds")
                continue
            if name in unsigned_cols:
                # UINT_32 sorts unsigned, so the bounds must be the unsigned
                # extremes even where the signed reading would differ.
                lo, hi = int(nn.min()), int(nn.max())
                check((int(smin) & 0xFFFFFFFF) == lo and (int(smax) & 0xFFFFFFFF) == hi,
                      f"rg{g}.{name} unsigned min/max ({smin}, {smax}) vs ({lo}, {hi})")
            elif series.dtype.kind == "f":
                check(math.isclose(smin, float(nn.min())) and math.isclose(smax, float(nn.max())),
                      f"rg{g}.{name} min/max ({smin}, {smax}) vs "
                      f"({float(nn.min())}, {float(nn.max())})")
            else:
                check(int(smin) == int(nn.min()) and int(smax) == int(nn.max()),
                      f"rg{g}.{name} min/max ({smin}, {smax}) vs ({int(nn.min())}, {int(nn.max())})")
            stats_checked += 1
            # Where Arrow does trust the statistics, it must agree with the bytes.
            st = col.statistics
            if st is not None and st.has_min_max:
                if name == "t_ms":
                    check(int(pd.Timestamp(st.min).value // 1_000_000) == int(smin) and
                          int(pd.Timestamp(st.max).value // 1_000_000) == int(smax),
                          f"rg{g}.{name} arrow agrees with the raw statistics")
                elif series.dtype.kind == "f":
                    check(math.isclose(st.min, smin) and math.isclose(st.max, smax),
                          f"rg{g}.{name} arrow agrees with the raw statistics")
                else:
                    check(int(st.min) == int(smin) and int(st.max) == int(smax),
                          f"rg{g}.{name} arrow agrees with the raw statistics")
    print(f"  compared min/max on {stats_checked} column chunk(s)")

    print("\n-- signal sidecar")
    sidecar = args.parquet.rsplit(".", 1)[0] + ".signals.parquet"
    side = pq.read_table(sidecar).to_pandas()
    print(side.to_string(index=False, max_colwidth=24))
    if meta.num_rows:
        per_signal = df.groupby("signal_id").size()
        ok = True
        for _, r in side.iterrows():
            want = int(per_signal.get(int(r["signal_id"]), 0))
            if int(r["rows"]) != want:
                ok = False
                print(f"    sidecar rows={r['rows']} but table has {want} "
                      f"for signal {r['signal_id']}")
        check(ok, "sidecar row counts match the data table")
        lvl = df.groupby("signal_id")["level"].unique()
        ok = all(int(r["export_level"]) in lvl[int(r["signal_id"])].tolist()
                 for _, r in side.iterrows() if int(r["signal_id"]) in lvl.index)
        check(ok, "sidecar export_level matches the level in the data table")

    print("\n-- value spot-checks against Store::query()")
    if not meta.num_rows:
        print("  (no rows; skipped)")
    else:
        picks = []
        for sig, grp in df.groupby("signal_id"):
            idx = list(grp.index)
            step = max(1, len(idx) // max(1, args.spot_checks))
            picks.extend(idx[::step][: args.spot_checks])
        for i in picks:
            row = df.loc[i]
            sig = int(row["signal_id"])
            t = int(row["t_ms"])
            lvl = int(row["level"])
            if lvl == 0:
                # High-rate CAN signals can carry several samples in the same
                # millisecond, so compare the whole multiset at that timestamp.
                p = probe(args.exe, args.store, args.session, sig, t, 0)
                hit = p[p["t_ms"] == t]
                mine = df[(df["signal_id"] == sig) & (df["t_ms"] == t)]
                if not check(len(hit) > 0, f"query engine has a sample for signal {sig} "
                                           f"at t={t}"):
                    continue
                q_raw = sorted(int(v) for v in hit["raw"])
                q_val = sorted(float(v) for v in hit["value"])
                m_raw = sorted(int(v) for v in mine["raw"])
                m_val = sorted(float(v) for v in mine["value"])
                ok_raw = m_raw == q_raw
                ok_val = len(m_val) == len(q_val) and all(
                    math.isclose(a, b, rel_tol=0, abs_tol=1e-12) for a, b in zip(m_val, q_val))
                check(ok_raw and ok_val,
                      f"signal {sig} t={t}: parquet raw={m_raw} value={m_val} vs "
                      f"query raw={q_raw} value={q_val}")
                if ok_raw and ok_val:
                    print(f"  ok  L0 signal={sig} t_ms={t} n={len(q_raw)} raw={q_raw} "
                          f"value={q_val}")
            else:
                bms = int(row["bucket_ms"])
                mid = t + bms // 2
                p = probe(args.exe, args.store, args.session, sig, mid, bms)
                s = p[(p["t_ms"] >= t) & (p["t_ms"] < t + bms)]
                if not check(len(s) > 0, f"query engine has raw samples inside bucket "
                                         f"[{t}, {t + bms}) for signal {sig}"):
                    continue
                check(int(row["count"]) == len(s),
                      f"signal {sig} bucket {t} count={int(row['count'])} vs "
                      f"{len(s)} raw samples")
                check(math.isclose(float(row["v_first"]), float(s["value"].iloc[0]),
                                   rel_tol=1e-12, abs_tol=1e-9) and
                      math.isclose(float(row["v_last"]), float(s["value"].iloc[-1]),
                                   rel_tol=1e-12, abs_tol=1e-9),
                      f"signal {sig} bucket {t} first/last")
                if pd.notna(row["v_min"]):
                    check(math.isclose(float(row["v_min"]), float(s["value"].min()),
                                       rel_tol=1e-12, abs_tol=1e-9) and
                          math.isclose(float(row["v_max"]), float(s["value"].max()),
                                       rel_tol=1e-12, abs_tol=1e-9),
                          f"signal {sig} bucket {t} min/max vs raw samples")
                    check(math.isclose(float(row["v_mean"]), float(s["value"].mean()),
                                       rel_tol=1e-9, abs_tol=1e-9),
                          f"signal {sig} bucket {t} mean={float(row['v_mean'])!r} vs raw "
                          f"mean={float(s['value'].mean())!r}")
                    print(f"  ok  L{lvl} signal={sig} bucket={t} n={len(s)} "
                          f"min={float(row['v_min'])!r} max={float(row['v_max'])!r} "
                          f"mean={float(row['v_mean'])!r}")

    print("\n-- full L0 series cross-check against Store::query()")
    if not meta.num_rows:
        print("  (no rows; skipped)")
    else:
        from_ms, to_ms = int(kv["photon.from_ms"]), int(kv["photon.to_ms"])
        l0_sigs = [sig for sig, grp in df.groupby("signal_id")
                   if int(grp["level"].iloc[0]) == 0]
        if len(l0_sigs) > args.l0_signals:
            print(f"   {len(l0_sigs)} L0 signal(s); cross-checking the first "
                  f"{args.l0_signals}")
            l0_sigs = l0_sigs[:args.l0_signals]
        did = 0
        for sig in l0_sigs:
            grp = df[df["signal_id"] == sig]
            # Centre the window on this signal's own median timestamp so that a
            # sparse signal is still compared over samples it actually has.
            mid = int(grp["t_ms"].astype("int64").median())
            half = 1000
            lo, hi = max(from_ms, mid - half), min(to_ms, mid + half)
            p = probe(args.exe, args.store, args.session, int(sig), (lo + hi) // 2,
                      (hi - lo) // 2 + 1)
            p = p[(p["t_ms"] >= lo) & (p["t_ms"] <= hi)].sort_values(["t_ms", "raw"])
            mine = grp[(grp["t_ms"] >= lo) & (grp["t_ms"] <= hi)].sort_values(["t_ms", "raw"])
            check(len(mine) == len(p),
                  f"signal {sig} has {len(p)} raw samples in [{lo},{hi}] per the query "
                  f"engine, parquet has {len(mine)}")
            if len(mine) == len(p) and len(p):
                check(mine["t_ms"].tolist() == p["t_ms"].tolist(),
                      f"signal {sig} every timestamp in [{lo},{hi}] matches")
                check(mine["raw"].astype("int64").tolist() == p["raw"].tolist(),
                      f"signal {sig} every raw value in [{lo},{hi}] matches")
                check(all(math.isclose(a, b, rel_tol=0, abs_tol=1e-12)
                          for a, b in zip(mine["value"], p["value"])),
                      f"signal {sig} every physical value in [{lo},{hi}] matches")
                print(f"  ok  signal={sig} {len(p)} samples over [{lo}, {hi}] identical")
                did += 1
        check(bool(l0_sigs) == bool(did),
              f"every one of the {len(l0_sigs)} L0 signal(s) yielded a non-empty "
              f"cross-check window ({did} did)")
        if not l0_sigs:
            print("  (this export has no L0 signals)")

    print(f"\n== {checks - len(failures)} passed, {len(failures)} failed")
    for f in failures:
        print(f"   FAILED: {f}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
