"""Dump large text parts (plans) from an opencode session mentioning a term."""

import json
import sqlite3
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DB = r"C:\Users\alper\.local\share\opencode\opencode.db"
sid, term = sys.argv[1], sys.argv[2]
conn = sqlite3.connect(DB)
conn.row_factory = sqlite3.Row

rows = conn.execute(
    """
    select p.time_created, p.data from part p
    where p.session_id = ? and p.data like ? order by p.time_created
    """,
    (sid, f"%{term}%",),
).fetchall()

for r in rows:
    try:
        d = json.loads(r["data"])
    except Exception:
        continue
    if d.get("type") == "text" and len(d.get("text", "")) > 400:
        print(f"===== text @{r['time_created']} ({len(d['text'])} chars)")
        print(d["text"][:6000])
        print()
