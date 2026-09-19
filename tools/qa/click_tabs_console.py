"""Click across the top tab bar via CDP while capturing console output.

Usage: python click_tabs_console.py
"""

import json
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


def click(x, y):
    for kind, count in (("mousePressed", 1), ("mouseReleased", 1)):
        send(
            "Input.dispatchMouseEvent",
            {"type": kind, "x": x, "y": y, "button": "left", "clickCount": count},
        )


def drain(seconds):
    ws.settimeout(0.5)
    deadline = time.time() + seconds
    while time.time() < deadline:
        try:
            msg = json.loads(ws.recv())
        except Exception:
            continue
        method = msg.get("method", "")
        if method == "Runtime.consoleAPICalled":
            args = msg["params"].get("args", [])
            text = " ".join(str(a.get("value", a.get("description", ""))) for a in args)
            if "Failed to load resource" not in text:
                print(f"[console.{msg['params']['type']}] {text[:400]}")
        elif method == "Runtime.exceptionThrown":
            detail = msg["params"]["exceptionDetails"]
            print(f"[exception] {detail.get('text')} {str(detail.get('exception', {}).get('description', ''))[:400]}")
        elif method == "Log.entryAdded":
            entry = msg["params"]["entry"]
            if "Failed to load resource" not in entry["text"]:
                print(f"[{entry['level']}] {entry['text'][:400]}")


send("Runtime.enable")
send("Log.enable")
drain(1)

# Sweep across the tab bar (top ~28px) at several x positions, dwell on each.
for x in range(40, 900, 70):
    click(x, 24)
    print(f"--- clicked x={x}")
    drain(2.5)

print("done")
ws.close()
