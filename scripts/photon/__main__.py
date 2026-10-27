"""photon: run one Photon piece, and say what that run is for.

The repo is the server (photon-server) and the UI (photon-next). photon.cmd on
Windows and ./photon on macOS and Linux run this package with Python 3.9+.
"""

import sys
from pathlib import Path

if sys.version_info < (3, 9):
    sys.exit("photon needs Python 3.9 or newer; this is " + sys.version.split()[0])

# Run as a folder (python scripts/photon), so make the package importable by name.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from photon import build_cmd, clean_cmd, run_cmd, setup_cmd, test_cmd  # noqa: E402
from photon.common import state  # noqa: E402
from photon.usage import USE  # noqa: E402

COMMANDS = {
    "setup": setup_cmd.main,
    "build": build_cmd.main,
    "test": test_cmd.main,
    "clean": clean_cmd.main,
    "server": lambda words: run_cmd.server(words, with_ui=False),
    "station": lambda words: run_cmd.server(words, with_ui=True),
    "client": run_cmd.client,
}


def main(argv) -> int:
    # -DryRun is what the PowerShell photon took; both spellings work anywhere on the line.
    args = [a for a in argv if a.lower() not in ("--dry-run", "-dryrun")]
    state.dry_run = len(args) != len(argv)
    if not args or args[0].lower() in ("help", "-h", "--help"):
        print(USE)
        return 0
    command = COMMANDS.get(args[0].lower())
    if command is None:
        print("unknown command: " + args[0])
        print(USE)
        return 2
    command(args[1:])
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except KeyboardInterrupt:
        sys.exit(130)
