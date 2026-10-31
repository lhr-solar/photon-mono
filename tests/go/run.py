"""Run Go commands with tests from tests/go overlaid onto their photon-server packages.

Examples (from the repository root):
    python tests/go/run.py test -count=1 ./server ./observer ./sshd ./cmd/...
    python tests/go/run.py test -run TestDefaultConfig ./server
    python tests/go/run.py vet ./...
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


TESTS = Path(__file__).resolve().parent
SERVER = TESTS.parent.parent / "photon-server"


def main(words: list[str]) -> int:
    if not words or words[0] not in ("test", "vet", "list"):
        print("usage: run.py {test|vet|list} [go flags and packages]", file=sys.stderr)
        return 2

    replacements = {}
    for source in sorted(TESTS.rglob("*_test.go")):
        target = SERVER / source.relative_to(TESTS)
        if not target.parent.is_dir():
            raise ValueError(f"No Go package directory for {source}: {target.parent}")
        if target.exists():
            raise ValueError(f"Test exists in both source and test folders: {target}")
        replacements[str(target)] = str(source)
    if not replacements:
        raise ValueError(f"No Go tests found in {TESTS}")

    # Go sees the tests in their original packages, retaining access to private
    # symbols and the original working directories. Nothing is copied into the
    # source tree, so concurrent builds and interrupted tests leave it untouched.
    with tempfile.TemporaryDirectory(prefix="photon-go-tests-") as directory:
        overlay = Path(directory) / "overlay.json"
        overlay.write_text(json.dumps({"Replace": replacements}), encoding="utf-8")
        return subprocess.call(
            ["go", words[0], f"-overlay={overlay}", *words[1:]], cwd=SERVER
        )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
