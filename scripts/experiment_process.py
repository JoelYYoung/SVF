#!/usr/bin/env python3
"""Retry-safe attempt directories and bounded subprocess-group execution."""

from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import threading
import time
import uuid


@dataclass(frozen=True)
class ProcessResult:
    returncode: int
    timed_out: bool


def create_attempt(root, label):
    """Create a unique directory without reusing a partial scheduler attempt."""
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", label):
        raise ValueError(f"invalid attempt label: {label!r}")
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    attempt_id = (
        f"{label}-{time.time_ns()}-{os.getpid()}-{uuid.uuid4().hex[:12]}"
    )
    attempt = root / attempt_id
    attempt.mkdir()
    metadata = {
        "schema": 1,
        "attempt_id": attempt_id,
        "label": label,
        "host": platform.node(),
        "pid": os.getpid(),
        "started_epoch": time.time(),
    }
    (attempt / "attempt.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return attempt


def _group_exists(process_group):
    if platform.system() == "Linux" and Path("/proc").is_dir():
        for entry in Path("/proc").iterdir():
            if not entry.name.isdigit():
                continue
            try:
                text = (entry / "stat").read_text()
                # comm is parenthesized and may contain spaces or parentheses.
                fields = text[text.rfind(")") + 2:].split()
                state, group = fields[0], int(fields[2])
            except (FileNotFoundError, PermissionError, IndexError, ValueError):
                continue
            if group == process_group and state != "Z":
                return True
        return False
    try:
        os.killpg(process_group, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def _stop_group(process, grace_seconds):
    """Terminate the complete session created for process, then reap its leader."""
    process_group = process.pid
    try:
        os.killpg(process_group, signal.SIGTERM)
    except ProcessLookupError:
        process.poll()
        return
    except PermissionError:
        if platform.system() == "Darwin" and process.poll() is not None:
            return
        raise
    deadline = time.monotonic() + grace_seconds
    while time.monotonic() < deadline:
        process.poll()
        if not _group_exists(process_group):
            return
        time.sleep(0.01)
    try:
        os.killpg(process_group, signal.SIGKILL)
    except ProcessLookupError:
        pass
    except PermissionError:
        # Darwin can report EPERM for the just-exited session leader after
        # a signal interrupted waitpid before poll() can observe the exit.
        if platform.system() != "Darwin" and process.poll() is None:
            raise
    try:
        process.wait(timeout=max(1.0, grace_seconds))
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("subprocess-group leader could not be reaped") from error


def run_managed(argv, *, stdout=None, stderr=None, timeout_seconds=None,
                grace_seconds=30):
    """Run argv in its own session and clean the session on timeout or parent signal."""
    if timeout_seconds is not None and timeout_seconds <= 0:
        raise ValueError("timeout_seconds must be positive")
    if grace_seconds <= 0:
        raise ValueError("grace_seconds must be positive")
    process = subprocess.Popen(
        [str(argument) for argument in argv],
        stdout=stdout,
        stderr=stderr,
        start_new_session=True,
    )
    caught_signal = None
    forced_stop = None
    old_handlers = {}

    def interrupt(signum, _frame):
        nonlocal caught_signal, forced_stop
        caught_signal = signum
        for handled in old_handlers:
            signal.signal(handled, signal.SIG_IGN)
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return

        def force_stop():
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass

        forced_stop = threading.Timer(grace_seconds, force_stop)
        forced_stop.daemon = True
        forced_stop.start()

    for handled in (signal.SIGTERM, signal.SIGINT):
        old_handlers[handled] = signal.signal(handled, interrupt)
    timed_out = False
    try:
        try:
            returncode = process.wait(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            timed_out = True
            _stop_group(process, grace_seconds)
            returncode = 124
        if caught_signal is not None:
            _stop_group(process, grace_seconds)
            if forced_stop is not None:
                forced_stop.cancel()
            raise SystemExit(128 + caught_signal)
        # A well-behaved direct child must not leave members of its session behind.
        if platform.system() != "Darwin" and _group_exists(process.pid):
            _stop_group(process, grace_seconds)
            raise RuntimeError("subprocess leader exited with live session members")
        return ProcessResult(returncode=returncode, timed_out=timed_out)
    finally:
        if forced_stop is not None:
            forced_stop.cancel()
        for handled, previous in old_handlers.items():
            signal.signal(handled, previous)
