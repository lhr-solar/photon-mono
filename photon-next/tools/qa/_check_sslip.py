import base64
import re
import ssl
import urllib.request

ctx = ssl.create_default_context()
auth = base64.b64encode(b"photon:laptop").decode()
url = "https://150.136.85.207.sslip.io/photon.html"
req = urllib.request.Request(url)
req.add_header("Authorization", "Basic " + auth)
with urllib.request.urlopen(req, timeout=10, context=ctx) as r:
    body = r.read().decode("utf-8", "replace")
    print("status", r.status, "len", len(body))
    m = re.search(r"photon\.js\?v=([^\"\s]+)", body)
    print("js token", m.group(1) if m else "NONE")
