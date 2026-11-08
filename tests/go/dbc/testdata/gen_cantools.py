"""Decode frames for every message in the spec with cantools, the reference decoder.

The Go decoder must give the same value for every signal. Run from the repository
root after the spec changes, and commit the JSON it writes:

    python tests/go/dbc/testdata/gen_cantools.py

Values are written with repr() so nan and inf survive; Go reads them with ParseFloat.
"""

from __future__ import annotations

import json
import random
from pathlib import Path

import cantools

HERE = Path(__file__).resolve().parent
SPEC = HERE.parents[3] / "photon-server" / "dbc" / "spec" / "dbc"
OUT = HERE / "cantools_frames.json"
RANDOM_FRAMES = 6


def selector_byte(message) -> int:
    """The byte holding the mux selector. Only whole-byte selectors are supported here."""
    sel = next(s for s in message.signals if s.is_multiplexer)
    aligned = (sel.byte_order == "big_endian" and sel.start % 8 == 7) or (
        sel.byte_order == "little_endian" and sel.start % 8 == 0
    )
    if sel.length != 8 or not aligned:
        raise SystemExit(f"{message.name}: selector {sel.name} is not one whole byte")
    return sel.start // 8


def frames_for(message, rng: random.Random) -> list[bytearray]:
    n = message.length
    frames = [bytearray(n), bytearray(b"\xff" * n)]
    frames += [bytearray(rng.randbytes(n)) for _ in range(RANDOM_FRAMES)]
    if not message.is_multiplexed():
        return frames
    # A random selector is usually not a valid page; give every page its own frames.
    at = selector_byte(message)
    pages = sorted({p for s in message.signals for p in (s.multiplexer_ids or [])})
    out = []
    for page in pages:
        for f in frames:
            f = bytearray(f)
            f[at] = page
            out.append(f)
    return out


def main() -> None:
    rng = random.Random(2027)
    cases = []
    for path in sorted(SPEC.glob("*.dbc")):
        db = cantools.database.load_file(path, strict=False)
        for message in db.messages:
            wire_id = message.frame_id | (0x80000000 if message.is_extended_frame else 0)
            for data in frames_for(message, rng):
                values = message.decode(bytes(data), decode_choices=False, scaling=True)
                cases.append({
                    "bus": path.stem,
                    "message": message.name,
                    "id": wire_id,
                    "data": data.hex(),
                    "values": {k: repr(float(v)) for k, v in values.items()},
                })
    OUT.write_text(json.dumps(cases, indent=0) + "\n", encoding="utf-8")
    print(f"{len(cases)} frames -> {OUT}")


if __name__ == "__main__":
    main()
