"""photon-backend replaying the example clip, driven the way the UI drives it."""

from __future__ import annotations

import shutil
import tempfile
import unittest
from pathlib import Path

from support import (
    EXAMPLE_DIR,
    EXAMPLE_LOG,
    MODE_PAUSE,
    MODE_PLAY,
    PhotonClient,
    Proc,
    bundle,
    clip_batches,
    exe,
    free_port,
    require,
)


def fnv1a32(text: str) -> int:
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


class ReplayBackend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assets = bundle("client") / "assets"
        require(exe("client", "photon-backend"), assets / "dbc", assets / "tracks")
        # The backend writes a .photon-idx cache beside the log; keep that out of examples\.
        cls.tmp = Path(tempfile.mkdtemp(prefix="photon-e2e-replay-"))
        cls.log = cls.tmp / EXAMPLE_LOG.name
        shutil.copy2(EXAMPLE_LOG, cls.log)
        cls.port = free_port()
        cls.backend = Proc(
            "backend-replay",
            [
                exe("client", "photon-backend"),
                "--ws-port", cls.port,
                "--source", "log_replay",
                "--log-path", cls.log,
                "--no-live-tail",
                "--assets-dir", assets,
            ],
        )
        try:
            cls.backend.wait_for_port(cls.port, 30)
            cls.ui = PhotonClient(cls.port, client="e2e-replay")
            cls.hello = cls.ui.wait_message("hello_ack")
            cls.catalog = cls.ui.wait_message("signal_catalog")
            cls.ui.wait(
                lambda: any(t["batches"] > 0 and t["max_ms"] > t["min_ms"] for t in cls.ui.timelines),
                30,
                "the replay index",
            )
        except BaseException:
            cls.backend.stop()
            raise

    @classmethod
    def tearDownClass(cls):
        cls.ui.close()
        cls.backend.stop()

    def test_hello_names_the_replay(self):
        self.assertEqual(self.hello["protocol_version"], 1)
        self.assertEqual(self.hello["server"], "photon-backend")
        source = self.hello["active_source"]
        self.assertEqual(source["kind"], "log_replay")
        self.assertEqual(Path(source["path"]).resolve(), self.log.resolve())
        self.assertLessEqual({"daq_tcp", "log_replay", "pcan", "dbc", "track_sim"}, set(self.hello["features"]))
        self.assertEqual(self.hello["dbc"]["id"], "carcan_telemcan")

    def test_signal_catalog_matches_the_dbc(self):
        signals = self.catalog["signals"]
        self.assertGreater(len(signals), 400)
        ids = [s["id"] for s in signals]
        self.assertEqual(len(ids), len(set(ids)), "signal ids repeat")
        self.assertNotIn(0, ids)
        # signalId is FNV-1a-32 of Message.Signal; only collisions may differ.
        hashed = sum(1 for s in signals if fnv1a32(s["message"] + "." + s["name"]) == s["id"])
        self.assertGreater(hashed, len(signals) * 0.99)

    def test_timeline_spans_the_clip(self):
        batches = clip_batches()
        stamps = [ts for ts, _ in batches]
        tl = next(t for t in self.ui.timelines if t["batches"] > 0)
        self.assertEqual(tl["min_ms"], min(stamps))
        self.assertEqual(tl["max_ms"], max(stamps))
        self.assertEqual(tl["batches"], len(batches))

    def test_streams_only_subscribed_signals(self):
        wanted = [s["id"] for s in self.catalog["signals"][:48]]
        start = next(t for t in self.ui.timelines if t["batches"] > 0)["min_ms"]
        self.ui.telemetry.clear()
        self.ui.send({"type": "subscribe_signals", "signal_ids": wanted})
        # Seek to the start (cursor 0 means "no seek"), so earlier tests cannot have played it out.
        self.ui.send({"type": "timeline_control", "mode": "play", "cursor_ms": start + 1, "speed": 4})
        self.ui.wait(lambda: self.ui.telemetry, 20, "telemetry")
        self.assertLessEqual(set(self.ui.telemetry), set(wanted))
        self.ui.send({"type": "unsubscribe_signals", "signal_ids": wanted})

    def test_pause_and_seek(self):
        tl = next(t for t in self.ui.timelines if t["batches"] > 0)
        middle = (tl["min_ms"] + tl["max_ms"]) // 2
        seen = len(self.ui.timelines)
        # The control message names the mode; TimelineStatus carries its number.
        self.ui.send({"type": "timeline_control", "mode": "pause", "cursor_ms": middle})
        paused = self.ui.wait(
            lambda: next(
                (t for t in self.ui.timelines[seen:] if t["mode"] == MODE_PAUSE and abs(t["cursor_ms"] - middle) < 1500),
                None,
            ),
            15,
            "a paused timeline at the seek point",
        )
        self.assertEqual(paused["min_ms"], tl["min_ms"])
        seen = len(self.ui.timelines)
        self.ui.send({"type": "timeline_control", "mode": "play", "cursor_ms": 0})
        self.ui.wait(lambda: any(t["mode"] == MODE_PLAY for t in self.ui.timelines[seen:]), 15, "play to resume")

    def test_select_dbc_swaps_the_catalog(self):
        other = EXAMPLE_DIR / "HighNoon.dbc"
        seen = len(self.ui.messages)
        self.ui.send({"type": "select_dbc", "path": str(other)})
        swapped = self.ui.wait_message("signal_catalog", after=seen)
        self.assertNotEqual(len(swapped["signals"]), len(self.catalog["signals"]))
        seen = len(self.ui.messages)
        self.ui.send({"type": "select_dbc", "id": "carcan_telemcan"})
        back = self.ui.wait_message("signal_catalog", after=seen)
        self.assertEqual(len(back["signals"]), len(self.catalog["signals"]))

    def test_a_second_ui_is_served(self):
        other = PhotonClient(self.port, client="e2e-second")
        try:
            hello = other.wait_message("hello_ack")
            self.assertEqual(hello["active_source"]["kind"], "log_replay")
            other.wait_message("signal_catalog")
        finally:
            other.close()


if __name__ == "__main__":
    unittest.main()
