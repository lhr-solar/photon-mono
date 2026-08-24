"""Find opencode sessions mentioning a term and dump the tail of a session.

Usage:
  python opencode_tail.py search <term>
  python opencode_tail.py tail <session_id> [n_messages]
"""

import json
import sqlite3
import sys

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

DB = r"C:\Users\alper\.local\share\opencode\opencode.db"
conn = sqlite3.connect(DB)
conn.row_factory = sqlite3.Row

mode = sys.argv[1]

if mode == "search":
    term = sys.argv[2]
    rows = conn.execute(
        """
        select m.session_id, s.title, count(*) as hits, max(m.time_created) as last
        from part p join message m on p.message_id = m.id
        join session s on s.id = m.session_id
        where p.data like ?
        group by m.session_id order by last desc limit 10
        """,
        (f"%{term}%",),
    ).fetchall()
    for r in rows:
        print(dict(r))
elif mode == "tail":
    sid = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    rows = conn.execute(
        """
        select m.id, m.time_created, m.data as mdata, p.data as pdata
        from message m join part p on p.message_id = m.id
        where m.session_id = ?
        order by m.time_created desc, p.id desc limit ?
        """,
        (sid, n * 3),
    ).fetchall()
    for r in reversed(rows):
        try:
            md = json.loads(r["mdata"] or "{}")
            role = md.get("role", "?")
        except Exception:
            role = "?"
        try:
            d = json.loads(r["pdata"] or "{}")
            ptype = d.get("type", "?")
            if ptype == "text":
                text = d.get("text", "")
            elif ptype == "tool":
                tool = d.get("tool", "?")
                state = d.get("state", {})
                inp = json.dumps(state.get("input", {}))[:300]
                out = str(state.get("output", ""))[:300]
                text = f"tool={tool} status={state.get('status')} input={inp} output={out}"
            else:
                text = json.dumps(d)[:400]
        except Exception:
            ptype = "raw"
            text = str(r["pdata"])[:400]
        text = str(text)[:900]
        print(f"--- [{role}] {ptype} @{r['time_created']}")
        print(text)
