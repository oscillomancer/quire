# SPDX-License-Identifier: GPL-3.0-or-later
"""Process cleanup shared by the integration drivers: every child they start is
ended and reaped on every path, including failures during startup."""
import subprocess


def stop(process, timeout=10, graceful=True):
    """Ends `process` (SIGTERM when `graceful`, else SIGKILL), waits up to
    `timeout` seconds, then kills it, and always reaps it. Returns the exit
    status; never raises, so it is safe in a `finally` block."""
    if process is None:
        return None
    if process.poll() is None:
        try:
            if graceful:
                process.terminate()
            else:
                process.kill()
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        except OSError:
            process.wait()
    return process.returncode
