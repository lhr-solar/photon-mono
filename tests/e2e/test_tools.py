"""Offline tools: photon-index imports the example clip, photon-export reads it back."""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from support import BUILD, EXAMPLE_DIR, Proc, exe, require

DBC = BUILD / "assets" / "dbc" / "carcan_telemcan.dbc"


def run(name: str, args: list, timeout: float = 300) -> Proc:
    proc = Proc(name, args)
    proc.wait(timeout)
    return proc


class ImportAndExport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        require(exe("photon-index"), DBC)
        cls.tmp = Path(tempfile.mkdtemp(prefix="photon-e2e-tools-"))
        cls.store = cls.tmp / "tsdb"
        cls.index = run(
            "index-import",
            [exe("photon-index"), "--logs", EXAMPLE_DIR, "--out", cls.store, "--dbc", DBC, "--validate"],
        )

    def sessions(self) -> list:
        return json.loads((self.store / "catalog.json").read_text(encoding="utf-8"))["sessions"]

    def test_import_validates_against_a_live_decode(self):
        self.assertEqual(self.index.p.returncode, 0, self.index.tail())
        self.assertIn("validate ok", self.index.output())

    def test_import_catalogs_one_session(self):
        sessions = self.sessions()
        self.assertEqual(len(sessions), 1, sessions)
        self.assertGreater(sessions[0]["max_ms"], sessions[0]["min_ms"])
        self.assertTrue((self.store / sessions[0]["id"] / "signals.json").exists())

    def test_export_lists_the_session_and_its_signals(self):
        require(exe("photon-export"))
        session = self.sessions()[0]["id"]
        listed = run("export-list-sessions", [exe("photon-export"), "--store", self.store, "--list-sessions"])
        self.assertEqual(listed.p.returncode, 0, listed.tail())
        self.assertIn(session, listed.output())
        signals = run(
            "export-list-signals",
            [exe("photon-export"), "--store", self.store, "--session", session, "--list-signals"],
        )
        self.assertEqual(signals.p.returncode, 0, signals.tail())
        self.assertIn("signal(s) with data", signals.output())
        self.assertIn("IMU_Accel_FR.Accel_X", signals.output())

    def test_export_writes_parquet(self):
        require(exe("photon-export"))
        session = self.sessions()[0]["id"]
        out = self.tmp / "all.parquet"
        export = run(
            "export-all",
            [exe("photon-export"), "--store", self.store, "--session", session, "--out", out, "--all-signals"],
        )
        self.assertEqual(export.p.returncode, 0, export.tail())
        body = out.read_bytes()
        self.assertEqual((body[:4], body[-4:]), (b"PAR1", b"PAR1"))
        sidecar = (self.tmp / "all.signals.parquet").read_bytes()
        self.assertEqual((sidecar[:4], sidecar[-4:]), (b"PAR1", b"PAR1"))

    def test_export_rejects_an_unknown_session(self):
        require(exe("photon-export"))
        export = run(
            "export-unknown",
            [exe("photon-export"), "--store", self.store, "--session", "no-such-session", "--out", self.tmp / "x.parquet", "--all-signals"],
        )
        self.assertNotEqual(export.p.returncode, 0)


if __name__ == "__main__":
    unittest.main()
