"""photon's help text, which photon help and a mistyped build both print."""

import os

DIST = os.path.join("out", "dist")
BUILD = os.path.join("out", "build")

USE = f"""Photon is two programs. Run the one you need, and name the use case.

  photon setup [server] [client] [station] [tools] [--ui desktop|web|both] [--pcan] [--build]
      Install what those pieces need to build on this machine.

  photon build [server] [client] [station] [tools] [--ui desktop|web|both] [--config Debug|Release]
      Build those pieces, then assemble one runnable bundle each under {DIST}:
        server   photon-server, photon-sshd, photon-index (no desktop UI libraries)
        client   photon-backend and the UI --ui names (desktop when omitted)
        station  server and client with the web page photon-server serves
        tools    photon-export, photon-observer
      symbols holds the Windows .pdb files. Build trees are {os.path.join(BUILD, "<preset>")}.
      Plain photon build is server and the desktop client.

  photon test [go] [native] [e2e] [--config Debug|Release] [--strict]
      go       go vet and the photon-server package tests
      native   ctest in each native tree for --config, and the video decoder glue test
      e2e      tests/e2e against {DIST}: replay, a whole station, the page
               in a headless browser, import and export. --strict fails
               on a missing piece instead of skipping it.
      No words runs all three.

  photon clean [--deps]
      Remove out{os.sep} (build trees and bundles), and the old build{os.sep} and .artifacts* trees.
      --deps also removes {os.path.join("photon-next", ".external")} (fetched libraries, emsdk).
      data{os.sep} is never touched.

  photon server [ingest PORT]
      Ingest only, from {os.path.join(DIST, "server")}. The DAQ dials the CAN ingress on port 6100, or PORT.
      The video store and the fan-out stay up. The UI stays down.

  photon station [ingest PORT]
      Server and the web UI on this machine, from {os.path.join(DIST, "station")}.

  photon client [FROM] [--ui desktop|web]
      The UI from {os.path.join(DIST, "client")}, reading FROM:
        (nothing)     the server on this machine, 127.0.0.1:6500
        HOST[:PORT]   a server's fan-out; the port defaults to 6500
        oracle        the public Oracle pipe, 150.136.85.207:6110
        pcan          a USB PCAN on this machine
        PATH          replay a VSD1 log or an on_track folder
      Video comes from the same host, TCP :6700 and UDP :6800.
      --ui picks the desktop window or the page; desktop when both are built.

Flags after that are passed through to photon-server or photon-backend (--password, -ingress, ...).
PHOTON_PASSWORD defaults to laptop. PHOTON_DATA_DIR defaults to <repo>/data.
--dry-run on any command prints what it would run instead of running it.
"""
