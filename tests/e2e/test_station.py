"""A whole station: photon-server with photon-index and the local UI, fed the example clip.

One server for the class, fed once in setUpClass; each test checks one thing that
should have happened downstream of that feed. The local UI ports are fixed in
photon-server (8765 WebSocket, 8080 page), so this cannot run beside a live station.
"""

from __future__ import annotations

import base64
import json
import os
import socket
import tempfile
import threading
import time
import unittest
from pathlib import Path

from support import (
    BUILD,
    STRICT,
    Browser,
    PhotonClient,
    Proc,
    client_auth,
    clip_batches,
    copy_ship,
    exe,
    feed,
    find_browser,
    free_port,
    http_get,
    port_open,
    require,
    vsd1_payloads,
    wait_until,
)

UI_WS_PORT = 8765
UI_HTTP_PORT = 8080
PASSWORD = "e2e-" + os.urandom(4).hex()


class FanoutProbe:
    """A downstream client of the CANP fan-out that keeps every byte it is sent."""

    def __init__(self, port: int, password: str):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.sock.sendall(client_auth(password))
        self.data = bytearray()
        self.closed = False
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        self.sock.settimeout(None)
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                self.data += chunk
        except OSError:
            pass
        self.closed = True

    def close(self):
        self.sock.close()


class Station(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        names = ["photon-server", "photon-index", "photon-backend"]
        require(*[exe(n) for n in names], BUILD / "assets" / "dbc", BUILD / "ui" / "photon.html")
        for port in (UI_WS_PORT, UI_HTTP_PORT):
            if port_open(port):
                message = f"port {port} is in use; stop the running station first"
                if STRICT:
                    raise AssertionError(message)
                raise unittest.SkipTest(message)

        cls.tmp = Path(tempfile.mkdtemp(prefix="photon-e2e-station-"))
        cls.ship = copy_ship(cls.tmp / "ship", [exe(n).name for n in names], ["assets", "ui"])
        cls.data = cls.tmp / "data"
        cls.ports = {k: free_port() for k in ("ingress", "client", "observer", "video-ingress", "video-client", "video-udp")}
        args = [cls.ship / exe("photon-server").name, "-data-dir", cls.data, "-password", PASSWORD]
        for flag, port in cls.ports.items():
            args += ["-" + flag, port]
        cls.server = Proc("station-server", args, cwd=cls.ship, env={"PHOTON_NO_BROWSER": "1"})
        cls.payloads = vsd1_payloads()
        cls.fed = b"".join(cls.payloads)
        cls.probe = cls.ui = None
        try:
            cls.server.wait_for_output("Ready to process streams", 30)
            cls.server.wait_for_output("TSDB live writer pid", 30)
            cls.server.wait_for_port(UI_WS_PORT, 30)
            cls.server.wait_for_port(UI_HTTP_PORT, 30)

            cls.probe = FanoutProbe(cls.ports["client"], PASSWORD)
            cls.ui = PhotonClient(UI_WS_PORT, client="e2e-station")
            cls.hello = cls.ui.wait_message("hello_ack")
            catalog = cls.ui.wait_message("signal_catalog")
            cls.ui.send({"type": "subscribe_signals", "signal_ids": [s["id"] for s in catalog["signals"][:64]]})
            # Raw frames for the CAN screen only flow to a client that asks for them.
            cls.ui.send({"type": "subscribe_can"})
            # The local backend dials the fan-out on its own; give it the first batch.
            wait_until(lambda: "daq_tcp connected" in cls.backend_log(), 30, "the local backend to reach the fan-out")

            feeder = threading.Thread(target=feed, args=(cls.ports["ingress"], cls.payloads))
            feeder.start()
            while feeder.is_alive():
                cls.ui.pump(0.1)
            cls.ui.wait(lambda: cls.ui.telemetry, 20, "live telemetry through the station")
            wait_until(lambda: len(cls.probe.data) >= len(cls.fed), 20, "the fan-out to relay the whole clip")
            wait_until(cls.tsdb_sessions, 60, "photon-index to catalog a TSDB session")
        except BaseException:
            cls.shutdown()
            raise

    @classmethod
    def backend_log(cls) -> str:
        path = cls.ship / "photon-backend-stdout.log"
        return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""

    @classmethod
    def tsdb_sessions(cls) -> list:
        catalog = cls.data / "tsdb" / "catalog.json"
        # photon-index replaces the file as it goes; Windows refuses a read mid-replace.
        for _ in range(50):
            try:
                return json.loads(catalog.read_text(encoding="utf-8")).get("sessions", [])
            except FileNotFoundError:
                return []
            except (PermissionError, ValueError):
                time.sleep(0.1)
        return []

    @classmethod
    def shutdown(cls) -> int | None:
        for held in (cls.ui, cls.probe):
            if held is not None:
                held.close()
        cls.ui = cls.probe = None
        if cls.server.p.poll() is None:
            return cls.server.stop()
        return cls.server.p.returncode

    @classmethod
    def tearDownClass(cls):
        cls.shutdown()

    def observer(self) -> dict:
        with socket.create_connection(("127.0.0.1", self.ports["observer"]), timeout=10) as sock:
            sock.sendall(client_auth(PASSWORD))
            buf = b""
            while b"\n" not in buf:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        return json.loads(buf.split(b"\n", 1)[0])

    # Ingest and fan-out ------------------------------------------------------

    def test_ingest_stores_the_clip(self):
        stored = sum(p.stat().st_size for p in (self.data / "segments").glob("*.log"))
        self.assertGreaterEqual(stored, len(self.fed))

    def test_fanout_relays_the_clip_byte_for_byte(self):
        self.assertEqual(bytes(self.probe.data[: len(self.fed)]), self.fed)

    def test_observer_reports_ingest_and_clients(self):
        stats = self.observer()
        self.assertEqual(stats["ingress"]["bytes"], len(self.fed))
        self.assertTrue(stats["clients"]["listening"])
        # photon-index, the local backend, and the probe.
        self.assertGreaterEqual(stats["clients"]["active"], 3)

    def test_wrong_password_is_dropped(self):
        with socket.create_connection(("127.0.0.1", self.ports["client"]), timeout=10) as sock:
            sock.sendall(client_auth("not-" + PASSWORD))
            sock.settimeout(10)
            self.assertEqual(sock.recv(1), b"")
        self.server.wait_for_output("authentication failed", 10)

    # The live TSDB ----------------------------------------------------------

    def test_live_writer_catalogs_the_session(self):
        session = self.tsdb_sessions()[0]
        stamps = sorted(ts for ts, _ in clip_batches(self.payloads))
        self.assertLess(abs(session["min_ms"] - stamps[0]), 1000)
        wait_until(
            lambda: abs(self.tsdb_sessions()[0]["max_ms"] - stamps[-1]) < 2000,
            30,
            "the TSDB session to reach the end of the clip",
        )
        folder = self.data / "tsdb" / session["id"]
        self.assertTrue((folder / "signals.json").exists())
        self.assertTrue(list(folder.glob("l0/*.pcol")), "no L0 pages written")

    def test_export_reads_the_live_session(self):
        require(exe("photon-export"))
        session = self.tsdb_sessions()[0]["id"]
        out = self.tmp / "live.parquet"
        export = Proc(
            "station-export",
            [exe("photon-export"), "--store", self.data / "tsdb", "--session", session, "--out", out, "--all-signals"],
        )
        self.assertEqual(export.wait(120), 0, export.tail())
        body = out.read_bytes()
        self.assertEqual(body[:4], b"PAR1")
        self.assertEqual(body[-4:], b"PAR1")
        self.assertTrue((self.tmp / "live.signals.parquet").exists())

    # The local UI -----------------------------------------------------------

    def test_local_backend_reads_the_fanout(self):
        self.assertEqual(self.hello["active_source"]["kind"], "daq_tcp")
        self.assertTrue(self.ui.telemetry)
        self.assertGreater(self.ui.can_batches, 0)

    def test_page_needs_the_password(self):
        base = f"http://127.0.0.1:{UI_HTTP_PORT}"
        self.assertEqual(http_get(base + "/photon.html")[0], 401)
        self.assertEqual(http_get(base + "/photon.html", "wrong")[0], 401)
        status, body = http_get(base + "/photon.html", PASSWORD)
        self.assertEqual(status, 200)
        self.assertIn(b"photon.js", body)

    def test_page_serves_wasm_and_models(self):
        base = f"http://127.0.0.1:{UI_HTTP_PORT}"
        for name in ("photon.js", "photon.wasm", "photon.data"):
            self.assertEqual(http_get(f"{base}/{name}", PASSWORD)[0], 200, name)
        models = sorted((self.ship / "ui" / "models").glob("*.glb"))
        if STRICT:
            self.assertTrue(models, "ui/models is empty")
        for model in models:
            self.assertEqual(http_get(f"{base}/models/{model.name}", PASSWORD, timeout=60)[0], 200, model.name)

    def test_page_loads_and_connects_in_a_browser(self):
        path = find_browser()
        if not path:
            if STRICT:
                self.fail("no Chrome or Edge found; set PHOTON_BROWSER")
            self.skipTest("no Chrome or Edge found; set PHOTON_BROWSER")
        before = self.backend_log().count("(photon-ui)")
        browser = Browser(path, self.tmp / "browser-profile")
        try:
            token = base64.b64encode(f"photon:{PASSWORD}".encode()).decode()
            browser.call("Runtime.enable")
            browser.call("Network.enable")
            browser.call("Network.setExtraHTTPHeaders", {"headers": {"Authorization": "Basic " + token}})
            browser.call("Page.navigate", {"url": f"http://127.0.0.1:{UI_HTTP_PORT}/photon.html"})
            deadline = time.monotonic() + 90
            # The WASM UI says hello as photon-ui once it is running and its /ws proxy is up.
            while self.backend_log().count("(photon-ui)") <= before:
                self.assertLess(time.monotonic(), deadline, "the page never said hello to the backend")
                browser.pump(0.5)
            browser.pump(3)
            self.assertEqual(browser.exceptions(), [])
            canvas = browser.call("Runtime.evaluate", {"expression": "!!document.querySelector('canvas')", "returnByValue": True})
            self.assertTrue(canvas["result"]["value"], "no canvas on the page")
        finally:
            browser.close()

    # Last, since it stops the server ---------------------------------------

    def test_zz_interrupt_shuts_down_cleanly(self):
        self.assertEqual(self.shutdown(), 0, self.server.tail())
        self.assertIn("Graceful shutdown completed successfully", self.server.output())


if __name__ == "__main__":
    unittest.main()
