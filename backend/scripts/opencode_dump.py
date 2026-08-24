"""Dump recent opencode sessions: projects, latest session, and its messages."""

import json
import sqlite3
import sys

DB = r"C:\Users\alper\.local\share\opencode\opencode.db"
conn = sqlite3.connect(DB)
conn.row_factory = sqlite3.Row

print("== session columns ==")
print([r[1] for r in conn.execute("PRAGMA table_info(session)")])
print("== recent sessions ==")
for row in conn.execute("select * from session order by time_updated desc limit 5"):
    d = dict(row)
    for k, v in list(d.items()):
        if isinstance(v, str) and len(v) > 120:
            d[k] = v[:120] + "..."
    print(json.dumps(d, default=str)[:600])

print("== project dirs ==")
try:
    for row in conn.execute("select * from project_directory limit 10"):
        print(dict(row))
except Exception as e:
    print("err", e)
