"""photon setup: install what photon build needs on this machine, for the pieces named.
Safe to rerun: each step checks first and skips what is already there.

  photon setup server            CMake, Ninja, clang, Go (and the C++ build tools on Windows)
  photon setup client            CMake, Ninja, clang, Git (and SDL2 on Linux)
  photon setup client --ui web   the same, plus the pinned emsdk
  photon setup station           server and client with the web page
  photon setup ... --pcan        the PEAK PCAN driver (Windows), for photon client pcan
  photon setup ... --build       then run photon build with the same words

Windows installs with winget, macOS with Homebrew and the Xcode Command Line Tools,
Linux with apt, dnf, pacman, or zypper. No component words means server and the
desktop client, the same as photon build.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

from photon import build_cmd, web
from photon.common import IS_LINUX, IS_MAC, IS_WINDOWS, fail, output, resolve, state
from photon.selection import Selection, SelectionError, format_selection, read_selection


def wanted(sel: Selection):
    """The tools the selection needs, by the keys the package tables below use."""
    keys = []
    if sel.server or sel.client or sel.tools:
        keys += ["cmake", "ninja", "clang"]
    if sel.server or sel.tools:
        keys.append("go")
    if sel.client:
        # CMake fetches ImGui, ImPlot, and tinygltf with git; emsdk is a git clone.
        keys.append("git")
    if sel.desktop:
        keys.append("sdl2")
    return keys


# What macOS and Linux call each key in setup's output.
LABELS = {"cmake": "CMake", "ninja": "Ninja", "clang": "clang", "cxx": "g++ (the C++ library clang uses)",
          "go": "Go", "git": "Git", "sdl2": "SDL2 development files"}


def have(label: str) -> None:
    print("have    " + label)


def version_of(tool: str, pattern: str):
    """The (major, minor) a tool's --version prints, or None."""
    match = re.search(pattern, output([tool, "version" if tool == "go" else "--version"]))
    return (int(match.group(1)), int(match.group(2))) if match else None


def warn_old_versions(keys) -> None:
    # The C++ uses C++23; the Go module asks for go 1.26, which go 1.21+ fetches by itself.
    clang = version_of("clang", r"version (\d+)\.(\d+)") if "clang" in keys else None
    if clang and clang[0] < 17 and not IS_MAC:
        print("warning: clang %d.%d is older than 17, which photon-next's C++23 needs" % clang)
    go = version_of("go", r"go(\d+)\.(\d+)") if "go" in keys else None
    if go and go < (1, 21):
        print("warning: go %d.%d cannot fetch the go 1.26 toolchain go.mod asks for; install Go from"
              " https://go.dev/dl" % go)


# Windows ----------------------------------------------------------------------------------

WINGET = {
    "cmake": ("Kitware.CMake", "CMake"),
    "ninja": ("Ninja-build.Ninja", "Ninja"),
    "clang": ("LLVM.LLVM", "LLVM (clang)"),
    "go": ("GoLang.Go", "Go"),
    "git": ("Git.Git", "Git"),
}
# winget's "already installed, nothing newer" result.
WINGET_ALREADY_INSTALLED = 0x8A15002B


def _registry_path(machine: bool, raw: bool = False):
    import winreg
    if machine:
        root, sub = winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
    else:
        root, sub = winreg.HKEY_CURRENT_USER, "Environment"
    try:
        with winreg.OpenKey(root, sub) as key:
            value, kind = winreg.QueryValueEx(key, "Path")
    except FileNotFoundError:
        value, kind = "", winreg.REG_EXPAND_SZ
    return (value, kind) if raw else winreg.ExpandEnvironmentStrings(value)


def _refresh_path() -> None:
    """Pick up what installers just added, keeping what this session added itself."""
    parts = [p for p in (_registry_path(True) + ";" + _registry_path(False)).split(";") if p]
    known = {p.lower().rstrip("\\") for p in parts}
    parts += [p for p in os.environ.get("PATH", "").split(";") if p and p.lower().rstrip("\\") not in known]
    os.environ["PATH"] = ";".join(parts)


def _add_user_path(folder: Path) -> None:
    import ctypes
    import winreg
    value, kind = _registry_path(False, raw=True)
    entries = [p for p in value.split(";") if p]
    known = {p.lower().rstrip("\\") for p in (_registry_path(True) + ";" + _registry_path(False)).split(";") if p}
    if str(folder).lower().rstrip("\\") in known:
        have(str(folder) + " on PATH")
        return
    print("path    add " + str(folder))
    if state.dry_run:
        return
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_SET_VALUE) as key:
        kind = kind if kind in (winreg.REG_SZ, winreg.REG_EXPAND_SZ) else winreg.REG_EXPAND_SZ
        winreg.SetValueEx(key, "Path", 0, kind, ";".join(entries + [str(folder)]))
    # Tell Explorer, so terminals opened from now on see it.
    result = ctypes.c_ulong()
    ctypes.windll.user32.SendMessageTimeoutW(0xFFFF, 0x001A, 0, "Environment", 0x0002, 5000, ctypes.byref(result))


def _have_vc_tools() -> bool:
    x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    vswhere = x86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return False
    found = output([vswhere, "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                    "-property", "installationPath"])
    return bool(found) and (x86 / "Windows Kits" / "10" / "Lib").exists()


def _winget(package: str, label: str, present: bool, override: str = None) -> None:
    if present:
        have(label)
        return
    if state.dry_run:
        print("install " + label + "  (winget " + package + ")")
        return
    print("install " + label, flush=True)
    args = [resolve("winget"), "install", "--id", package, "-e", "--accept-package-agreements",
            "--accept-source-agreements"]
    if override:
        args += ["--override", override]
    code = subprocess.call(args)
    if code != 0 and (code & 0xFFFFFFFF) != WINGET_ALREADY_INSTALLED:
        fail("winget install " + package + " failed with " + str(code))


def setup_windows(sel: Selection, pcan: bool) -> None:
    if resolve("winget") is None:
        fail('winget is missing. Install "App Installer" from the Microsoft Store, then rerun.')
    llvm_bin = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin"
    _refresh_path()
    keys = wanted(sel)
    for key in keys:
        if key == "sdl2":
            continue  # The desktop build fetches and builds SDL2 itself on Windows.
        package, label = WINGET[key]
        present = resolve(key) is not None or (key == "clang" and (llvm_bin / "clang.exe").exists())
        _winget(package, label, present)
        if key == "clang":
            # clang links against the MSVC libraries and the Windows SDK.
            _winget("Microsoft.VisualStudio.BuildTools", "Visual Studio Build Tools (C++ and Windows SDK)",
                    _have_vc_tools(),
                    "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended")
    if pcan:
        dll = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "PCANBasic.dll"
        _winget("PEAKSystem.PEAKDrivers", "PEAK PCAN driver", dll.exists())
    if "clang" in keys:
        # The LLVM installer does not add itself to PATH.
        _add_user_path(llvm_bin)
    _refresh_path()
    if "clang" in keys and not state.dry_run and not _have_vc_tools():
        fail('\nVisual Studio is installed without the C++ build tools. Open "Visual Studio Installer",\n'
             'choose Modify, and add "Desktop development with C++".')


# macOS ------------------------------------------------------------------------------------

BREW = {"cmake": "cmake", "ninja": "ninja", "go": "go"}


def setup_mac(sel: Selection, pcan: bool) -> None:
    keys = wanted(sel)
    if output(["xcode-select", "-p"]):
        # Apple's clang and git come with them; the desktop build fetches SDL2 itself.
        have("Xcode Command Line Tools (clang, git)")
    elif state.dry_run:
        print("install Xcode Command Line Tools  (xcode-select --install)")
    else:
        print("install Xcode Command Line Tools")
        subprocess.call(["xcode-select", "--install"])
        fail("Finish the Command Line Tools installer that just opened, then rerun photon setup.")
    brew = resolve("brew")
    if brew is None:
        for candidate in ("/opt/homebrew/bin/brew", "/usr/local/bin/brew"):
            if Path(candidate).exists():
                brew = candidate
                print("note: Homebrew is not on PATH. Add this to your shell profile:")
                print('  eval "$(' + candidate + ' shellenv)"')
                break
    missing = []
    for key in keys:
        if key not in BREW:
            continue
        if resolve(key):
            have(LABELS[key])
        else:
            missing.append(BREW[key])
    if missing:
        if brew is None:
            fail("Homebrew is missing. Install it from https://brew.sh, then rerun photon setup.")
        if state.dry_run:
            print("install " + " ".join(missing) + "  (brew install)")
        else:
            print("install " + " ".join(missing), flush=True)
            if subprocess.call([brew, "install"] + missing) != 0:
                fail("brew install " + " ".join(missing) + " failed")
    if pcan:
        print("skip    PEAK PCAN driver: photon client pcan is Windows only for now")
    warn_old_versions(keys)


# Linux ------------------------------------------------------------------------------------

# How each package manager installs, and its name for each tool. cxx is the C++ standard
# library clang compiles against, which comes with g++.
LINUX = {
    "apt-get": (["apt-get", "install", "-y"], dict(
        cmake="cmake", ninja="ninja-build", clang="clang", cxx="build-essential", go="golang-go", git="git",
        sdl2="libsdl2-dev")),
    "dnf": (["dnf", "install", "-y"], dict(
        cmake="cmake", ninja="ninja-build", clang="clang", cxx="gcc-c++", go="golang", git="git",
        sdl2="SDL2-devel")),
    "pacman": (["pacman", "-S", "--needed", "--noconfirm"], dict(
        cmake="cmake", ninja="ninja", clang="clang", cxx="base-devel", go="go", git="git", sdl2="sdl2")),
    "zypper": (["zypper", "install", "-y"], dict(
        cmake="cmake", ninja="ninja", clang="clang", cxx="gcc-c++", go="go", git="git", sdl2="SDL2-devel")),
}


def _linux_present(key: str) -> bool:
    if key == "cxx":
        return resolve("g++") is not None
    if key == "sdl2":
        return resolve("sdl2-config") is not None or bool(output(["pkg-config", "--modversion", "sdl2"]))
    return resolve(key) is not None


def setup_linux(sel: Selection, pcan: bool) -> None:
    keys = wanted(sel)
    if "clang" in keys:
        keys.insert(keys.index("clang") + 1, "cxx")
    manager = next((m for m in LINUX if resolve(m)), None)
    missing = []
    for key in keys:
        if _linux_present(key):
            have(LABELS[key])
        else:
            missing.append(key)
    if missing:
        if manager is None:
            fail("no apt-get, dnf, pacman, or zypper here. Install these yourself, then rerun: "
                 + ", ".join(LABELS[key] for key in missing))
        install, names = LINUX[manager]
        packages = [names[key] for key in missing]
        sudo = [] if os.geteuid() == 0 else ["sudo"]
        if state.dry_run:
            print("install " + " ".join(packages) + "  (" + " ".join(sudo + install[:2]) + ")")
        else:
            print("install " + " ".join(packages), flush=True)
            if manager == "apt-get":
                subprocess.call(sudo + ["apt-get", "update"])
            if subprocess.call(sudo + install + packages) != 0:
                fail(" ".join(install[:2]) + " " + " ".join(packages) + " failed")
    if pcan:
        print("skip    PEAK PCAN driver: photon client pcan is Windows only for now")
    warn_old_versions(keys)


def main(args) -> None:
    pcan = build = False
    words = []
    for word in args:
        if word.lower() == "--pcan":
            pcan = True
        elif word.lower() == "--build":
            build = True
        else:
            words.append(word)
    try:
        sel = read_selection(words)
    except SelectionError as e:
        fail(str(e), 2)
    if sel.rest:
        fail("unknown setup word: " + " ".join(sel.rest), 2)

    print("setup for " + format_selection(sel))
    if IS_WINDOWS:
        setup_windows(sel, pcan)
    elif IS_MAC:
        setup_mac(sel, pcan)
    elif IS_LINUX:
        setup_linux(sel, pcan)
    else:
        fail("photon setup does not know this OS; install CMake, Ninja, clang, Go, and Git yourself.")
    if sel.web:
        web.ensure_emsdk()

    if build and not state.dry_run:
        build_cmd.main(words)
    print("")
    if IS_WINDOWS:
        print("Setup done. Restart VS Code and any open terminals so they pick up the new PATH.")
    else:
        print("Setup done.")
    if not build:
        print("Then run: photon build " + " ".join(words))
