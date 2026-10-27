"""What a build or a setup is for. photon build and photon setup both read these words.

  server   photon-server, photon-sshd, photon-index, assets
  client   photon-backend, assets, and the UI that --ui names (desktop when omitted)
  station  server and client with the web page, which photon-server serves itself
  tools    photon-export, photon-observer

  --ui desktop|web|both     which client UI; naming a UI implies client
  --config Debug|Release    native build type (Debug when omitted); picks the
                            headless-<config> or desktop-<config> CMake preset

No component words means server and the desktop client.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List


class SelectionError(ValueError):
    pass


@dataclass
class Selection:
    server: bool = False
    client: bool = False
    desktop: bool = False
    web: bool = False
    tools: bool = False
    config: str = "Debug"
    rest: List[str] = field(default_factory=list)


def read_selection(words) -> Selection:
    sel = Selection()
    ui = None
    i = 0
    while i < len(words):
        word = words[i]
        key = word.lower()
        if key in ("--ui", "-ui", "--config", "-config"):
            if i + 1 >= len(words):
                raise SelectionError(word + " needs a value")
            value = words[i + 1].lower()
            if key.lstrip("-") == "ui":
                if value not in ("desktop", "web", "both"):
                    raise SelectionError("--ui is desktop, web, or both, not " + words[i + 1])
                ui = value
            else:
                # The presets in photon-next/CMakePresets.json.
                configs = {"debug": "Debug", "release": "Release"}
                if value not in configs:
                    raise SelectionError("--config is Debug or Release, not " + words[i + 1])
                sel.config = configs[value]
            i += 2
            continue
        if key == "server":
            sel.server = True
        elif key == "client":
            sel.client = True
        elif key == "station":
            sel.server = sel.client = sel.web = True
        elif key == "tools":
            sel.tools = True
        else:
            sel.rest.append(word)
        i += 1
    if ui:
        sel.client = True
        sel.desktop = sel.desktop or ui in ("desktop", "both")
        sel.web = sel.web or ui in ("web", "both")
    if not (sel.server or sel.client or sel.tools):
        sel.server = sel.client = True
    if sel.client and not (sel.desktop or sel.web):
        sel.desktop = True
    return sel


def format_selection(sel: Selection) -> str:
    parts = []
    if sel.server:
        parts.append("server")
    if sel.client:
        uis = [name for name, on in (("desktop", sel.desktop), ("web", sel.web)) if on]
        parts.append("client (" + " + ".join(uis) + ")")
    if sel.tools:
        parts.append("tools")
    return ", ".join(parts)
