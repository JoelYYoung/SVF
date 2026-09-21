#!/usr/bin/env python3
"""Contracts for retry isolation and subprocess-group cleanup."""

import json
import os
from pathlib import Path
import runpy
import signal
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
HELPER = runpy.run_path(str(HERE / "experiment_process.py"))


def process_exists(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


class ExperimentProcess(unittest.TestCase):
    def test_attempts_never_reuse_partial_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "attempts"
            first = HELPER["create_attempt"](root, "case")
            (first / "partial").touch()
            second = HELPER["create_attempt"](root, "case")
            self.assertNotEqual(first, second)
            self.assertTrue((first / "partial").exists())
            for attempt in (first, second):
                metadata = json.loads((attempt / "attempt.json").read_text())
                self.assertEqual(metadata["attempt_id"], attempt.name)

    def test_timeout_cleans_grandchild(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / "grandchild.pid"
            source = (
                "import pathlib,subprocess,time; "
                "p=subprocess.Popen(['python3','-c',"
                "'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(str(p.pid)); "
                "time.sleep(60)"
            )
            result = HELPER["run_managed"](
                [sys.executable, "-c", source],
                timeout_seconds=0.25,
                grace_seconds=0.25,
            )
            self.assertEqual(result.returncode, 124)
            self.assertTrue(result.timed_out)
            pid = int(pid_file.read_text())
            for _ in range(100):
                if not process_exists(pid):
                    break
                time.sleep(0.01)
            self.assertFalse(process_exists(pid))

    def test_parent_sigterm_cleans_grandchild(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pid_file = root / "grandchild.pid"
            wrapper = root / "wrapper.py"
            wrapper.write_text(
                "import runpy,sys\n"
                f"h=runpy.run_path({str(HERE / 'experiment_process.py')!r})\n"
                "source=(\"import pathlib,subprocess,time; \"\n"
                "        \"p=subprocess.Popen(['python3','-c',\"\n"
                "        \"'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(60)']); \"\n"
                f"        \"pathlib.Path({str(pid_file)!r}).write_text(str(p.pid)); \"\n"
                "        \"time.sleep(60)\")\n"
                "h['run_managed']([sys.executable,'-c',source], grace_seconds=.25)\n"
            )
            wrapper_process = subprocess.Popen([sys.executable, str(wrapper)])
            for _ in range(200):
                if pid_file.exists():
                    break
                time.sleep(0.01)
            self.assertTrue(pid_file.exists())
            os.kill(wrapper_process.pid, signal.SIGTERM)
            self.assertEqual(wrapper_process.wait(timeout=5), 128 + signal.SIGTERM)
            pid = int(pid_file.read_text())
            for _ in range(100):
                if not process_exists(pid):
                    break
                time.sleep(0.01)
            self.assertFalse(process_exists(pid))

    def test_success_returns_child_status(self):
        result = HELPER["run_managed"](
            [sys.executable, "-c", "raise SystemExit(7)"],
            timeout_seconds=5,
        )
        self.assertEqual(result.returncode, 7)
        self.assertFalse(result.timed_out)


if __name__ == "__main__":
    unittest.main()
