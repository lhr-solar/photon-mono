"""Set a stable viewport, click, then screenshot. Usage: shot_after_click.py x y out.png"""

import base64
import json
import sys
import time
import urllib.request

import websocket

x, y, out_path = float(sys.argv[1]), float(sys.argv[2]), sys.argv[3]
targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
ws = websocket.create_connection(page["webSocketDebuggerUrl"])
mid = 0


def send(method, params=None):
    global mid
    mid += 1
    ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
    return mid


def wait(tid, timeout=15.0):
    ws.settimeout(timeout)
    while True:
        msg = json.loads(ws.recv())
        if msg.get("id") == tid:
            return msg


wait(
    send(
        "Emulation.setDeviceMetricsOverride",
        {"width": 1600, "height": 900, "deviceScaleFactor": 1, "mobile": False},
    )
)
time.sleep(0.8)
wait(send("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x, "y": y}))
wait(
    send(
        "Input.dispatchMouseEvent",
        {"type": "mousePressed", "x": x, "y": y, "button": "left", "buttons": 1, "clickCount": 1},
    )
)
wait(
    send(
        "Input.dispatchMouseEvent",
        {"type": "mouseReleased", "x": x, "y": y, "button": "left", "buttons": 0, "clickCount": 1},
    )
)
time.sleep(1.2)
rid = send("Page.captureScreenshot", {"format": "png"})
msg = wait(rid)
with open(out_path, "wb") as f:
    f.write(base64.b64decode(msg["result"]["data"]))
print(f"clicked {x},{y} saved {out_path}")
ws.close()
