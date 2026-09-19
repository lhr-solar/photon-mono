"""Focus the photon canvas and click a list of points via CDP."""

import json
import sys
import time
import urllib.request

import websocket

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


def click(x, y):
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
    print(f"clicked {x},{y}")


wait(send("Emulation.clearDeviceMetricsOverride"))
wait(send("Runtime.evaluate", {"expression": "document.querySelector('canvas').focus(); true"}))

pairs = [(float(sys.argv[i]), float(sys.argv[i + 1])) for i in range(1, len(sys.argv), 2)]
if not pairs:
    pairs = [(1320, 22), (1360, 22), (80, 22), (145, 22)]
for x, y in pairs:
    click(x, y)
    time.sleep(0.25)

ws.close()
