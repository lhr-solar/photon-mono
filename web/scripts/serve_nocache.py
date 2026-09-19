"""Dev server for .artifacts-web.

photon.html / photon.js / photon.wasm / photon.data stay no-store so a rebuild
is never served from a stale browser cache. GLB models under /models/ are
cacheable — they are large and change rarely.

Usage: python serve_nocache.py [port]
"""

import http.server
import sys
from pathlib import Path
from urllib.parse import unquote, urlparse

WEB_ROOT = Path(__file__).resolve().parents[2] / ".artifacts-web"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(WEB_ROOT), **kwargs)

    def end_headers(self):
        path = unquote(urlparse(self.path).path).replace("\\", "/").lower()
        if path.startswith("/models/"):
            self.send_header("Cache-Control", "public, max-age=86400")
        else:
            self.send_header("Cache-Control", "no-store, must-revalidate")
            self.send_header("Expires", "0")
        super().end_headers()


if __name__ == "__main__":
    if not (WEB_ROOT / "photon.html").exists():
        raise SystemExit(f"photon.html not found in {WEB_ROOT}; build the frontend first")
    print(f"Serving {WEB_ROOT} on http://127.0.0.1:{PORT}/photon.html (no-cache)")
    http.server.ThreadingHTTPServer(("127.0.0.1", PORT), NoCacheHandler).serve_forever()
