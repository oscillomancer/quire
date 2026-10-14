# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
"""Bounded readiness for subprocess stdout, including Windows anonymous pipes."""
import os
import select
import time

if os.name == "nt":
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _peek = ctypes.WinDLL("kernel32", use_last_error=True).PeekNamedPipe
    _peek.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                      ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
                      ctypes.POINTER(wintypes.DWORD)]
    _peek.restype = wintypes.BOOL


def wait_pipe(pipe, timeout):
    """True for bytes or EOF; leave the bytes for the existing protocol reader."""
    if os.name != "nt":
        return bool(select.select([pipe], [], [], timeout)[0])
    deadline = time.monotonic() + timeout
    handle = msvcrt.get_osfhandle(pipe.fileno())
    while True:
        available = wintypes.DWORD()
        if not _peek(handle, None, 0, None, ctypes.byref(available), None):
            error = ctypes.get_last_error()
            if error in (109, 233):  # broken/disconnected pipe: let os.read observe EOF
                return True
            raise ctypes.WinError(error)
        if available.value:
            return True
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return False
        time.sleep(min(0.01, remaining))
