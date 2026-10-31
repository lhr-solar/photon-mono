"""The WASM page: emsdk, the web-release preset, and the fixups after the link."""

from __future__ import annotations

import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

from photon.common import IS_WINDOWS, UI_DIR, WEB_TREE, fail, rel, run_step, show_command, state

# The emsdk release the page is built and tested with. Bump with the emsdk
# cache key in .github/workflows/ci.yml.
EMSDK_VERSION = "6.0.13"
EMSDK = UI_DIR / ".external" / "emsdk"


def installed_version():
    version_file = EMSDK / "upstream" / "emscripten" / "emscripten-version.txt"
    if not version_file.exists():
        return None
    return version_file.read_text(encoding="utf-8").strip().strip('"')


def install_emsdk(version: str = EMSDK_VERSION) -> None:
    """Clone emsdk into photon-next/.external if it is not there, then install and activate version."""
    if not (EMSDK / "emsdk.py").exists():
        if not state.dry_run:
            EMSDK.parent.mkdir(parents=True, exist_ok=True)
        run_step("emsdk", ["git", "clone", "--depth", "1", "https://github.com/emscripten-core/emsdk.git", EMSDK])
    # A shallow clone may not list a release newer than the clone itself.
    run_step("emsdk", ["git", "pull", "--ff-only"], cwd=EMSDK)
    run_step("emsdk", [sys.executable, "emsdk.py", "install", version], cwd=EMSDK)
    run_step("emsdk", [sys.executable, "emsdk.py", "activate", version], cwd=EMSDK)
    if not state.dry_run:
        print("emsdk " + version + " ready at " + rel(EMSDK))


def ensure_emsdk() -> None:
    """photon setup's step: install the pinned emsdk unless it is already there."""
    have = installed_version()
    if have == EMSDK_VERSION:
        print("have    emsdk " + EMSDK_VERSION)
        return
    label = "emsdk " + EMSDK_VERSION + (" (have " + have + ")" if have else "")
    print("install " + label)
    install_emsdk()


def emsdk_env() -> dict:
    """The environment emsdk_env would give a shell, from emsdk's own construct_env."""
    env = dict(os.environ)
    child = dict(os.environ, EMSDK_QUIET="1")
    cmd = [sys.executable, "emsdk.py", "construct_env"]
    if IS_WINDOWS:
        # In cmd mode emsdk writes SET lines to emsdk_set_env.bat rather than stdout.
        child["EMSDK_CMD"] = "1"
        subprocess.run(cmd, cwd=EMSDK, env=child, check=True, stdout=subprocess.DEVNULL)
        text = (EMSDK / "emsdk_set_env.bat").read_text(encoding="utf-8", errors="replace")
        assignments = re.findall(r"^set (\w+)=(.*?)\s*$", text, re.M | re.I)
    else:
        child["EMSDK_BASH"] = "1"
        text = subprocess.run(cmd, cwd=EMSDK, env=child, check=True, capture_output=True, text=True).stdout
        assignments = re.findall(r'^export (\w+)="(.*)";\s*$', text, re.M)
        assignments += [(key, "") for key in re.findall(r"^unset (\w+);\s*$", text, re.M)]
    for key, value in assignments:
        if value:
            env[key] = value
        else:
            env.pop(key, None)
    return env


def build_web() -> None:
    """Build the page with the web-release preset, then stamp and check it."""
    configure = ["cmake", "--preset", "web-release"]
    build = ["cmake", "--build", "--preset", "web-release"]
    if state.dry_run:
        if installed_version() != EMSDK_VERSION:
            install_emsdk()
        show_command("web", configure, UI_DIR)
        show_command("web", build, UI_DIR)
        print("web: stamp one cache token into photon.html and photon.js, check photon.js against photon.wasm")
        return
    if installed_version() is None:
        install_emsdk()
    env = emsdk_env()
    if not env.get("EMSDK"):
        fail("EMSDK is not set after emsdk construct_env")
    # The preset finds the Emscripten toolchain through EMSDK.
    run_step("web", configure, cwd=UI_DIR, env=env)
    run_step("web", build, cwd=UI_DIR, env=env)
    version = stamp_cache_token(WEB_TREE)
    check_glue(WEB_TREE / "photon.js", WEB_TREE / "photon.wasm")
    print("built " + rel(WEB_TREE / "photon.html") + " ?v=" + version)


# Tokens earlier builds stamped; a page that still carries one is moved to the new token.
OLD_TOKENS = ("20260906a", "20260905ab", "20260905aa", "20260905z", "20260905y", "20260905x", "20260905w",
              "20260905v", "20260905u", "20260905t", "20260905s", "20260905r", "20260905q", "20260905p",
              "20260905n", "20260905m", "20260905l", "20260905k", "20260905j", "20260905i", "20260905h",
              "20260905g", "20260905f", "20260905e", "20260905d", "20260905c", "20260905a", "20260904c")


def _read(path: Path) -> str:
    # Bytes in and out, so line endings stay exactly as the linker wrote them.
    return path.read_bytes().decode("utf-8")


def _write(path: Path, text: str) -> None:
    path.write_bytes(text.encode("utf-8"))


def stamp_cache_token(tree: Path) -> str:
    """One cache token for html+js+wasm+data, so a partial deploy cannot mix glue
    (LinkError: import "a" "a" is not a callable). Returns the token."""
    html, js, wasm = tree / "photon.html", tree / "photon.js", tree / "photon.wasm"
    version = "20260906a"
    if wasm.exists():
        version = "20260905ab-" + hashlib.sha256(wasm.read_bytes()).hexdigest()[:10]
    if html.exists():
        text = _read(html)
        text = text.replace("__PHOTON_ASSET_V__", version)
        text = text.replace("src=photon.js", "src=photon.js?v=" + version)
        text = text.replace('src="photon.js"', 'src="photon.js?v=' + version + '"')
        for old in OLD_TOKENS:
            # Skip prefixes of the current token (20260905a inside 20260905ab-...).
            if version.startswith(old):
                continue
            text = text.replace("?v=" + old, "?v=" + version)
        _write(html, text)
    if js.exists():
        text = _read(js)
        old = "var args=readEmAsmArgs(sigPtr,argbuf);return ASM_CONSTS[emAsmAddr](...args)}"
        new = ('var args=readEmAsmArgs(sigPtr,argbuf);var fn=ASM_CONSTS[emAsmAddr];'
               'if(typeof fn!=="function"){return 0}return fn(...args)}')
        if old in text:
            _write(js, text.replace(old, new))
    return version


def _leb(data: bytes, i: int):
    n = shift = 0
    while True:
        byte = data[i]
        i += 1
        n |= (byte & 0x7F) << shift
        if byte < 0x80:
            return n, i
        shift += 7


def wasm_import_modules(data: bytes):
    """The module names photon.wasm imports from (section 2), sorted."""
    if data[:4] != b"\x00asm":
        return ["NOT_WASM"]
    i = 8
    while i < len(data):
        section = data[i]
        i += 1
        size, i = _leb(data, i)
        end = i + size
        if section == 2:
            count, i = _leb(data, i)
            modules = []
            for _ in range(count):
                length, i = _leb(data, i)
                modules.append(data[i:i + length].decode())
                i += length
                length, i = _leb(data, i)
                i += length
                kind = data[i]
                i += 1
                if kind == 0:
                    _, i = _leb(data, i)
                elif kind in (1, 2):
                    i += 1
                    limits = data[i]
                    i += 1
                    _, i = _leb(data, i)
                    if limits == 1:
                        _, i = _leb(data, i)
                elif kind == 3:
                    i += 2
            return sorted(set(modules))
        i = end
    return []


def check_glue(js: Path, wasm: Path) -> None:
    """Fail if photon.js and photon.wasm disagree on the import module name."""
    modules = wasm_import_modules(wasm.read_bytes())
    wrapped = "imports={a:wasmImports}" in js.read_text(encoding="utf-8", errors="replace")
    print("wasm mods", modules, "js a_wrap", wrapped)
    if wrapped and "env" in modules:
        fail("glue/wasm mismatch: JS minified imports as a, WASM expects env")
    if not wrapped and modules == ["a"]:
        fail("glue/wasm mismatch: JS expects env, WASM minified as a")
