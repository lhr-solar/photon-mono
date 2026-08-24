"""Capture a screenshot of the photon page via CDP.

Usage: python screenshot_page.py <out.png> [settle_seconds]
"""

import base64
import json
import sys
import time
import urllib.request

import websocket

out_path = sys.argv[1]
settle = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
ws = websocket.create_connection(page["webSocketDebuggerUrl"])
mid = 0


def send(method, params=None):
    global mid
    mid += 1
    ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
    return mid


def wait_for(target_id):
    ws.settimeout(15.0)
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == target_id:
            return msg


send("Emulation.setDeviceMetricsOverride", {"width": 1600, "height": 900, "deviceScaleFactor": 1, "mobile": False})
time.sleep(settle)
rid = send("Page.captureScreenshot", {"format": "png"})
msg = wait_for(rid)
with open(out_path, "wb") as f:
    f.write(base64.b64decode(msg["result"]["data"]))
print(f"saved {out_path}")
ws.close()
