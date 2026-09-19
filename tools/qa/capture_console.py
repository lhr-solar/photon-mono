"""Attach to the debug Chrome tab via CDP and capture console output.

Usage: python capture_console.py [seconds]
Reloads the photon page and prints every console message received.
"""

import asyncio
import json
import sys
import urllib.request

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 12.0


async def main() -> None:
    try:
        import websockets
    except ImportError:
        import websocket  # websocket-client fallback, sync API

        run_sync(websocket)
        return

    targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
    page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
    async with websockets.connect(page["webSocketDebuggerUrl"], max_size=None) as ws:
        mid = 0

        async def send(method, params=None):
            nonlocal mid
            mid += 1
            await ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))

        await send("Runtime.enable")
        await send("Log.enable")
        await send("Page.enable")
        await send("Page.reload", {"ignoreCache": True})

        loop = asyncio.get_event_loop()
        deadline = loop.time() + SECONDS
        while loop.time() < deadline:
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=max(0.1, deadline - loop.time()))
            except asyncio.TimeoutError:
                break
            msg = json.loads(raw)
            method = msg.get("method", "")
            if method == "Runtime.consoleAPICalled":
                args = msg["params"].get("args", [])
                text = " ".join(str(a.get("value", a.get("description", ""))) for a in args)
                print(f"[console.{msg['params']['type']}] {text}")
            elif method == "Runtime.exceptionThrown":
                detail = msg["params"]["exceptionDetails"]
                print(f"[exception] {detail.get('text')} {detail.get('exception', {}).get('description', '')}")
            elif method == "Log.entryAdded":
                entry = msg["params"]["entry"]
                print(f"[{entry['level']}] {entry['text']}")


def run_sync(websocket_mod) -> None:
    targets = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
    page = next(t for t in targets if t["type"] == "page" and "photon.html" in t["url"])
    ws = websocket_mod.create_connection(page["webSocketDebuggerUrl"])
    for i, method in enumerate(("Runtime.enable", "Log.enable", "Page.enable"), start=1):
        ws.send(json.dumps({"id": i, "method": method, "params": {}}))
    ws.send(json.dumps({"id": 99, "method": "Page.reload", "params": {"ignoreCache": True}}))
    ws.settimeout(1.0)
    import time

    deadline = time.time() + SECONDS
    while time.time() < deadline:
        try:
            msg = json.loads(ws.recv())
        except Exception:
            continue
        method = msg.get("method", "")
        if method == "Runtime.consoleAPICalled":
            args = msg["params"].get("args", [])
            text = " ".join(str(a.get("value", a.get("description", ""))) for a in args)
            print(f"[console.{msg['params']['type']}] {text}")
        elif method == "Runtime.exceptionThrown":
            detail = msg["params"]["exceptionDetails"]
            print(f"[exception] {detail.get('text')} {detail.get('exception', {}).get('description', '')}")
        elif method == "Log.entryAdded":
            entry = msg["params"]["entry"]
            print(f"[{entry['level']}] {entry['text']}")
    ws.close()


if __name__ == "__main__":
    asyncio.run(main())
