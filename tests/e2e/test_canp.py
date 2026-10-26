"""The car-side C code against photon-server. Linux only: canp and the streamer use Linux headers.

canp/test/source.c dials ingress on 6100 and sends two frames (0x7FF, 0x7FE) every 10 ms
until it is stopped, the same writer the car runs.
"""

from __future__ import annotations

import json
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from support import (
    REPO,
    STRICT,
    Proc,
    canp_batches,
    client_auth,
    exe,
    free_port,
    port_open,
    require,
    vsd1_payloads,
    wait_until,
)

SERVER_SRC = REPO / "photon-server"
CANP = SERVER_SRC / "canp"
PASSWORD = "e2e-canp"


def need_linux_gcc():
    if not sys.platform.startswith("linux"):
        raise unittest.SkipTest("Linux only: canp and the car streamer use Linux headers")
    gcc = shutil.which("gcc")
    if gcc and Path("/usr/include/linux/can.h").exists():
        return gcc
    if STRICT:
        raise AssertionError("gcc or linux/can.h is missing")
    raise unittest.SkipTest("needs gcc and the kernel headers")


class CarSideC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gcc = need_linux_gcc()
        cls.tmp = Path(tempfile.mkdtemp(prefix="photon-e2e-canp-"))

    def compile(self, sources: list[Path], out: Path, extra: list[str]) -> None:
        result = subprocess.run([self.gcc, "-Wall", *map(str, sources), "-o", str(out), *extra], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_car_streamer_compiles(self):
        # The car build sits beside canp/ and includes "canp.h" from it.
        self.compile([SERVER_SRC / "car" / "stream_can_gps_v3.c"], self.tmp / "stream.o", ["-c", "-I", str(CANP)])

    def test_canp_sink_compiles(self):
        self.compile([CANP / "canp.c", CANP / "test" / "sink.c"], self.tmp / "sink", ["-lpthread"])

    def test_canp_source_reaches_the_server(self):
        require(exe("server", "photon-server"))
        if port_open(6100):
            self.skipTest("port 6100 is in use; the CANP source only dials 6100")
        source = self.tmp / "source"
        self.compile([CANP / "canp.c", CANP / "test" / "source.c"], source, ["-lm"])

        data = self.tmp / "data"
        observer = free_port()
        args = [exe("server", "photon-server"), "-data-dir", data, "-password", PASSWORD, "-ui", "off", "-live-writer", "off", "-ingress", 6100, "-observer", observer]
        for flag in ("client", "video-ingress", "video-client", "video-udp"):
            args += ["-" + flag, free_port(udp=flag in ("video-ingress", "video-udp"))]
        server = Proc("canp-server", args, cwd=self.tmp)

        def ingested() -> int:
            with socket.create_connection(("127.0.0.1", observer), timeout=5) as sock:
                sock.sendall(client_auth(PASSWORD))
                line = sock.makefile("rb").readline()
            return json.loads(line)["ingress"]["bytes"]

        try:
            server.wait_for_output("Ready to process streams", 30)
            # printf into a file is block-buffered, so "connected" may never reach the log;
            # line-buffer it for reading, but judge the connection by what the server counts.
            line_buffered = ["stdbuf", "-oL"] if shutil.which("stdbuf") else []
            writer = Proc("canp-source", [*line_buffered, source, "127.0.0.1"])
            try:

                def flowing():
                    if writer.p.poll() is not None:
                        raise AssertionError(f"the CANP source exited with {writer.p.returncode}:\n{writer.tail()}")
                    return ingested() > 0

                wait_until(flowing, 15, "the server to count CANP bytes", step=0.25)
                time.sleep(2)
            finally:
                writer.kill()
            self.assertGreater(ingested(), 100 * 29)
        finally:
            self.assertEqual(server.stop(), 0, server.tail())

        stream = b"".join(
            payload for segment in sorted((data / "segments").glob("*.log")) for payload in vsd1_payloads(segment.read_bytes())
        )
        batches = canp_batches(stream)
        self.assertGreater(len(batches), 10)
        self.assertEqual({i for _, ids in batches for i in ids}, {0x7FF, 0x7FE})
        stamps = [ts for ts, _ in batches]
        self.assertEqual(stamps, sorted(stamps), "batch times go backwards")


if __name__ == "__main__":
    unittest.main()
