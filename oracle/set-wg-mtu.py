#!/usr/bin/env python3
import re
from pathlib import Path

p = Path("/etc/wireguard/wg0.conf")
t = p.read_text()
if not re.search(r"(?im)^MTU\s*=", t):
    t = t.replace("[Interface]\n", "[Interface]\nMTU = 1420\n", 1)
    p.write_text(t)
ok = bool(re.search(r"(?im)^MTU\s*=\s*1420", p.read_text()))
print("mtu_conf", ok)
