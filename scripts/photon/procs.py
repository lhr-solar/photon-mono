"""Which running programs live under a folder. Windows will not delete a running
exe, so clean and the bundle swap stop on these instead of leaving half a folder.
macOS and Linux delete a running binary without complaint, so there it is always none."""

from __future__ import annotations

from pathlib import Path

from photon.common import IS_WINDOWS


def running_under(dirs):
    """(name, pid) for each process whose exe is inside one of dirs."""
    if not IS_WINDOWS:
        return []
    import ctypes
    from ctypes import wintypes

    psapi = ctypes.WinDLL("psapi")
    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    query_limited_information = 0x1000

    size = 1024
    while True:
        pids = (wintypes.DWORD * size)()
        needed = wintypes.DWORD()
        if not psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(needed)):
            return []
        if needed.value < ctypes.sizeof(pids):
            break
        size *= 2
    count = needed.value // ctypes.sizeof(wintypes.DWORD)

    prefixes = [str(Path(d).resolve()).lower().rstrip("\\") + "\\" for d in dirs]
    found = []
    for pid in pids[:count]:
        if pid == 0:
            continue
        handle = kernel32.OpenProcess(query_limited_information, False, pid)
        if not handle:
            continue
        try:
            buf = ctypes.create_unicode_buffer(32768)
            length = wintypes.DWORD(len(buf))
            if kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(length)):
                if any(buf.value.lower().startswith(p) for p in prefixes):
                    found.append((Path(buf.value).stem, pid))
        finally:
            kernel32.CloseHandle(handle)
    return found


def describe(procs) -> str:
    return ", ".join(name + " (pid " + str(pid) + ")" for name, pid in procs)
