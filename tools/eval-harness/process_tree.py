# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned evaluation process trees: POSIX sessions or Windows kill-on-close Jobs.

On Windows a Python bootstrap waits at a gate before executing user code. The
parent assigns it to the Job before releasing the gate, so descendants cannot
escape during the Popen/AssignProcessToJobObject interval.
"""
import os
from pathlib import Path
import signal
import subprocess
import sys
import uuid

if os.name == "nt":
    import ctypes
    from ctypes import wintypes

    class BasicLimits(ctypes.Structure):
        _fields_ = [("per_process_time", ctypes.c_longlong),
                    ("per_job_time", ctypes.c_longlong), ("flags", wintypes.DWORD),
                    ("min_working_set", ctypes.c_size_t), ("max_working_set", ctypes.c_size_t),
                    ("active_processes", wintypes.DWORD), ("affinity", ctypes.c_size_t),
                    ("priority", wintypes.DWORD), ("scheduling", wintypes.DWORD)]

    class IoCounters(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ("read_ops", "write_ops", "other_ops", "read_bytes", "write_bytes", "other_bytes")]

    class ExtendedLimits(ctypes.Structure):
        _fields_ = [("basic", BasicLimits), ("io", IoCounters),
                    ("process_memory", ctypes.c_size_t), ("job_memory", ctypes.c_size_t),
                    ("peak_process_memory", ctypes.c_size_t), ("peak_job_memory", ctypes.c_size_t)]

    _kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
    _kernel.CreateJobObjectW.restype = wintypes.HANDLE
    _kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    _kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
    _kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _kernel.CloseHandle.argtypes = [wintypes.HANDLE]


def spawn_group(command, **kwargs):
    if os.name != "nt":
        return subprocess.Popen(command, start_new_session=True, **kwargs)
    job = _kernel.CreateJobObjectW(None, None)
    if not job:
        raise ctypes.WinError(ctypes.get_last_error())
    limits = ExtendedLimits()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    process = None
    gate = Path(kwargs.get("cwd") or Path.cwd()).resolve() / (".eval-start-" + uuid.uuid4().hex)
    bootstrap = (
        "import subprocess,sys,time; from pathlib import Path; "
        "gate=Path(sys.argv[1]); deadline=time.monotonic()+60\n"
        "while not gate.exists():\n"
        " if time.monotonic()>deadline: sys.exit(125)\n"
        " time.sleep(.01)\n"
        "gate.unlink(); sys.exit(subprocess.call(sys.argv[2:]))\n"
    )
    try:
        if not _kernel.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            raise ctypes.WinError(ctypes.get_last_error())
        process = subprocess.Popen([sys.executable, "-u", "-c", bootstrap, str(gate), *command], **kwargs)
        if not _kernel.AssignProcessToJobObject(job, int(process._handle)):
            raise ctypes.WinError(ctypes.get_last_error())
        process._eval_job = job
        gate.touch(exist_ok=False)
        return process
    except BaseException:
        if process is not None:
            process.kill()
            process.wait()
        _kernel.CloseHandle(job)
        gate.unlink(missing_ok=True)
        raise


def stop_group(process):
    """Kill descendants even after the leader exits, then reap the leader."""
    if os.name == "nt":
        job = getattr(process, "_eval_job", None)
        if job:
            try:
                if not _kernel.TerminateJobObject(job, 1):
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                _kernel.CloseHandle(job)
                process._eval_job = None
        process.wait()
        return
    # The group is ours (same user), so a refusal can only mean nothing in it
    # is left to signal: macOS answers EPERM, not ESRCH, while the last
    # member is a zombie its new parent has not reaped yet.
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass
    process.wait()
