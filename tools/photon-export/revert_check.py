#!/usr/bin/env python3
"""Mutation driver: prove every photon-export test can fail.

For each mutation below it patches one production source file, rebuilds
photon-export-tests, runs it, and records which named test functions reported a
FAIL. The source is restored afterwards whether or not the build succeeded. A
mutation that does not make its target test fail means that test is not really
covering the behaviour it claims to.

Usage:
  python revert_check.py --build-dir ../../.artifacts
"""
from __future__ import annotations

import argparse
import bisect
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent

# (label, file, needle, replacement, tests expected to start failing)
MUTATIONS: list[tuple[str, str, str, str, list[str]]] = [
    ("varint continuation bit",
     "parquet.cpp", "out.push_back(static_cast<std::uint8_t>(v | 0x80u));",
     "out.push_back(static_cast<std::uint8_t>(v));",
     ["testVarint"]),
    ("zigzag32 sign fold",
     "parquet.cpp",
     "writeVarint(out, static_cast<std::uint64_t>((u << 1) ^ static_cast<std::uint32_t>(v >> 31)));",
     "writeVarint(out, static_cast<std::uint64_t>(u << 1));",
     ["testZigzag"]),
    ("zigzag64 sign fold",
     "parquet.cpp", "writeVarint(out, (u << 1) ^ static_cast<std::uint64_t>(v >> 63));",
     "writeVarint(out, u << 1);",
     ["testZigzag"]),
    ("short-form field delta limit",
     "parquet.cpp", "if (delta > 0 && delta <= 15) {",
     "if (delta > 0 && delta <= 31) {",
     ["testFieldHeader"]),
    ("RLE def-level run header",
     "parquet.cpp", "writeVarint(out, static_cast<std::uint64_t>(j - i) << 1);",
     "writeVarint(out, static_cast<std::uint64_t>(j - i));",
     ["testDefLevels"]),
    ("plain boolean bit order",
     "parquet.cpp", "out[base + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));",
     "out[base + i / 8] |= static_cast<std::uint8_t>(1u << (7u - (i % 8)));",
     ["testPlainBooleans"]),
    ("leading PAR1 magic",
     "parquet.cpp", "static const char kMagic[4] = {'P', 'A', 'R', '1'};",
     "static const char kMagic[4] = {'P', 'A', 'R', '0'};",
     ["testFileStructure"]),
    ("column chunk statistics",
     "parquet.cpp", "if (opts_.statistics) {",
     "if (false) {",
     ["testStatistics"]),
    ("row group flush threshold",
     "parquet.cpp", "if (groupRows_ >= opts_.rowGroupRows) {",
     "if (groupRows_ >= opts_.rowGroupRows * 4) {",
     ["testRowGroupsAndPages"]),
    ("endRow column-width check",
     "parquet.cpp", "if (c.rowCount != want) {",
     "if (false) {",
     ["testWriterRejectsBadRows"]),
    ("reject null in required column",
     "parquet.cpp", "    if (!c.spec.optional) {",
     "    if (false) {",
     ["testWriterRejectsBadRows"]),
    ("UINT_32 statistics compare unsigned",
     "parquet.cpp", "const bool unsignedCmp = c.spec.logical == Logical::Uint32;",
     "const bool unsignedCmp = false;",
     ["testStatistics"]),
    ("NaN excluded from double bounds",
     "parquet.cpp", "if (!std::isnan(v)) {",
     "if (true) {",
     ["testNanIsNotABound"]),
    ("physical scaling of raw samples",
     "export.cpp", """row.value = info != nullptr ? tsdb::physical(*info, values[k])
                                                : static_cast<double>(values[k]);""",
     "row.value = static_cast<double>(values[k]);",
     ["testRawExport"]),
    ("raw window clipping",
     "export.cpp", "if (!rawInRange(times[k], req.fromMs, req.toMs)) {",
     "if (false) {",
     ["testRawRangeClipping"]),
    ("rollup mean is a real average",
     "export.cpp", "const double m = meanRaw(slot, isSigned);",
     "const double m = static_cast<double>(slot.min + slot.max) / 2.0;",
     ["testRollupExport"]),
    ("rollup count",
     "export.cpp", "row.count = slot.count;",
     "row.count = 1;",
     ["testRollupExport", "testUnsignedRollup"]),
    ("128-bit unsigned sum high word",
     "export.cpp",
     "total = static_cast<double>(b.sumHi) * kTwo64 + static_cast<double>(static_cast<std::uint64_t>(b.sum));",
     "total = static_cast<double>(static_cast<std::uint64_t>(b.sum));",
     ["testWideRollupMean"]),
    ("128-bit signed sum high word",
     "export.cpp",
     """total = static_cast<double>(static_cast<std::int64_t>(b.sumHi)) * kTwo64 +
                static_cast<double>(static_cast<std::uint64_t>(b.sum));""",
     "total = static_cast<double>(static_cast<std::uint64_t>(b.sum));",
     ["testWideRollupMean"]),
    ("wide flag selects the signed two-word fold",
     "export.cpp", "if (isSigned && !b.wide) {",
     "if (isSigned) {",
     ["testWideRollupMean"]),
    ("empty bucket has no mean",
     "export.cpp", "if (b.count == 0) {",
     "if (false) {",
     ["testWideRollupMean"]),
    ("negative-factor min/max reorder",
     "export.cpp", """row.vMin = std::min(a, b);
                    row.vMax = std::max(a, b);""",
     """row.vMin = a;
                    row.vMax = b;""",
     ["testNegativeFactorEnvelope"]),
    ("bucket intersection test",
     "export.cpp", "return startMs <= toMs && startMs + bucketMs > fromMs;",
     "return startMs >= fromMs && startMs + bucketMs <= toMs;",
     ["testBucketEdgeSemantics"]),
    ("float value-type envelope suppression",
     "export.cpp",
     "const bool integerDomain = info == nullptr || info->valueType == dbc::ValueType::Integer;",
     "const bool integerDomain = true;",
     ["testFloatValueTypeSuppression"]),
    ("auto level selection",
     "export.cpp", """return tsdb::pickLevel(fromMs, toMs, maxPoints, store.rawCount(signalId, fromMs, toMs),
                           store.levelsOf(signalId));""",
     "return 0;",
     ["testLevelSelection", "testAutoLevelExportMixesLevels"]),
    ("forced level degrades to a built one",
     "export.cpp", "while (want > 0 && (builtLevels & static_cast<std::uint8_t>(1u << want)) == 0) {",
     "while (false) {",
     ["testLevelSelection"]),
    ("boundary samples reported",
     "export.cpp", "sr.prior = store.prior(id, req.fromMs);",
     "sr.prior = {};",
     ["testBoundarySamples"]),
    ("empty-range short circuit",
     "export.cpp", "if (req.fromMs > req.toMs) {",
     "if (false) {",
     ["testBadRequest"]),
    ("sink failure propagation (raw path)",
     "export.cpp", """                    if (!sink.row(row)) {
                        rep.error = "sink rejected a row";""",
     """                    if (false) {
                        rep.error = "sink rejected a row";""",
     ["testSinkFailurePropagates"]),
    ("one page in flight at a time",
     "export.cpp", "rep.maxPageBytes = std::max<std::uint64_t>(rep.maxPageBytes, recs[i].pageBytes);",
     "rep.maxPageBytes += recs[i].pageBytes;",
     ["testPageStreamingIsBounded"]),
]


def test_index() -> tuple[list[int], list[str]]:
    src = (HERE / "test_export.cpp").read_text(encoding="utf-8").splitlines()
    lines, names = [], []
    for i, line in enumerate(src, start=1):
        m = re.match(r"void (test\w+)\(", line)
        if m:
            lines.append(i)
            names.append(m.group(1))
    return lines, names


def failing_tests(output: str, lines: list[int], names: list[str]) -> set[str]:
    out = set()
    for m in re.finditer(r"FAIL \S*test_export\.cpp:(\d+)", output):
        n = int(m.group(1))
        idx = bisect.bisect_right(lines, n) - 1
        if idx >= 0:
            out.add(names[idx])
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--only", default="")
    args = ap.parse_args()

    build = pathlib.Path(args.build_dir).resolve()
    exe = build / "tools" / "photon-export" / "photon-export-tests.exe"
    if not exe.exists():
        exe = build / "tools" / "photon-export" / "photon-export-tests"
    lines, names = test_index()

    def rebuild() -> tuple[bool, str]:
        p = subprocess.run(["cmake", "--build", str(build), "--target", "photon-export-tests"],
                           capture_output=True, text=True)
        return p.returncode == 0, p.stdout + p.stderr

    ok, log = rebuild()
    if not ok:
        print("baseline build failed:\n" + log)
        return 2
    p = subprocess.run([str(exe)], capture_output=True, text=True)
    base = failing_tests(p.stdout + p.stderr, lines, names)
    print(f"baseline: {p.stdout.strip().splitlines()[-1] if p.stdout.strip() else '?'}")
    if base:
        print(f"baseline already failing: {sorted(base)}")
        return 2

    bad: list[str] = []
    for label, fname, needle, repl, expect in MUTATIONS:
        if args.only and args.only not in label:
            continue
        path = HERE / fname
        original = path.read_text(encoding="utf-8")
        if original.count(needle) != 1:
            print(f"SKIP {label}: needle appears {original.count(needle)}x in {fname}")
            bad.append(f"{label} (needle not unique)")
            continue
        path.write_text(original.replace(needle, repl), encoding="utf-8")
        try:
            ok, log = rebuild()
            if not ok:
                print(f"  {label}: build failed (counts as detected by the compiler)")
                got: set[str] = set()
                compiled = False
                clean = False
            else:
                compiled = True
                r = subprocess.run([str(exe)], capture_output=True, text=True)
                got = failing_tests(r.stdout + r.stderr, lines, names)
                # A mutation that crashes the binary takes every later test with
                # it, so the surviving FAIL lines say nothing about coverage.
                clean = "photon-export-tests:" in r.stdout
        finally:
            path.write_text(original, encoding="utf-8")
        if not compiled:
            bad.append(f"{label} (did not compile)")
            continue
        if not clean:
            print(f"  CRSH {label}: binary did not finish; failed so far {sorted(got)}")
            bad.append(f"{label} (crashed instead of failing cleanly)")
            continue
        missing = [t for t in expect if t not in got]
        status = "ok  " if not missing else "MISS"
        print(f"  {status} {label}: failed {sorted(got)}"
              + (f"; expected but passing: {missing}" if missing else ""))
        if missing:
            bad.append(f"{label} -> {missing}")

    ok2, log = rebuild()
    if not ok2:
        print("RESTORE BUILD FAILED:\n" + log)
        return 2
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    tail = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "?"
    print(f"restored: {tail}")
    if failing_tests(r.stdout + r.stderr, lines, names):
        print("RESTORE LEFT FAILURES")
        return 2
    if bad:
        print(f"\n{len(bad)} mutation(s) not detected:")
        for b in bad:
            print(f"  - {b}")
        return 1
    print("\nevery mutation was detected by the test it was aimed at")
    return 0


if __name__ == "__main__":
    sys.exit(main())
