"""Capture a zoomed clip of the photon page via CDP.

Usage: python screenshot_clip.py <out.png> <x> <y> <w> <h> <scale>
"""

import base64
import json
import sys
import time
import urllib.request

import websocket

out_path, x, y, w, h, scale = (
    sys.argv[1],
    float(sys.argv[2]),
    float(sys.argv[3]),
    float(sys.argv[4]),
    float(sys.argv[5]),
    float(sys.argv[6]),
)

targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
ws = websocket.create_connection(page["webSocketDebuggerUrl"])
mid = 0


def send(method, params=None):
    global mid
    mid += 1
    ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
    return mid


time.sleep(2)
rid = send(
    "Page.captureScreenshot",
    {"format": "png", "clip": {"x": x, "y": y, "width": w, "height": h, "scale": scale}},
)
ws.settimeout(15.0)
while True:
    msg = json.loads(ws.recv())
    if msg.get("id") == rid:
        break
with open(out_path, "wb") as f:
    f.write(base64.b64decode(msg["result"]["data"]))
print(f"saved {out_path}")
ws.close()
