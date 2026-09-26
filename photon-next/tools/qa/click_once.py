"""Click one point on the photon page via CDP.

Usage: python click_once.py <x> <y>
"""

import json
import sys
import time
import urllib.request

import websocket

x, y = float(sys.argv[1]), float(sys.argv[2])
targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
ws = websocket.create_connection(page["webSocketDebuggerUrl"])
mid = 0


def send(method, params):
    global mid
    mid += 1
    ws.send(json.dumps({"id": mid, "method": method, "params": params}))


for kind in ("mousePressed", "mouseReleased"):
    send("Input.dispatchMouseEvent", {"type": kind, "x": x, "y": y, "button": "left", "clickCount": 1})
time.sleep(1.0)
print("clicked")
ws.close()
