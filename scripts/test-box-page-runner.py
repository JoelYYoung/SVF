#!/usr/bin/env python3
"""Unit tests for the experiment identity gate, without running AE."""
import runpy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

RUNNER = runpy.run_path(str(Path(__file__).with_name("run-box-page-ae-case.py")))


class IdentityGate(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        for file in ("bin/ae", "bin/box-page-contract", "bin/box-page-semantic-observer",
                     "lib/libAbstractDomainCore.so.3.4", "lib/libSvfCore.so.3.4",
                     "lib/libSvfLLVM.so.3.4", "CMakeCache.txt"):
            path = self.build / file
            path.parent.mkdir(exist_ok=True)
            path.write_text(file)

    def commands(self, address="1111", identity="inline", wrong_library=False):
        def output(command, **kwargs):
            if command[-1] == "--identity":
                return f"representation=chunk8/{identity}8\n"
            path = self.build / "lib/libAbstractDomainCore.so.3.4"
            if wrong_library:
                path = self.build / "wrong.so"
            return f"libAbstractDomainCore.so.3 => {path} (0x{address})\n"
        return output

    def test_aslr_is_not_a_binary_change(self):
        with patch("subprocess.check_output", side_effect=self.commands("1234")):
            first = RUNNER["fingerprint"](self.build, "inline")
        with patch("subprocess.check_output", side_effect=self.commands("5678")):
            second = RUNNER["fingerprint"](self.build, "inline")
        self.assertEqual(first, second)

    def test_wrong_layout_is_rejected(self):
        with patch("subprocess.check_output", side_effect=self.commands(identity="packed")):
            with self.assertRaisesRegex(RuntimeError, "wrong runtime layout"):
                RUNNER["fingerprint"](self.build, "inline")

    def test_loader_override_is_rejected(self):
        with patch("subprocess.check_output", side_effect=self.commands(wrong_library=True)):
            with self.assertRaisesRegex(RuntimeError, "unexpected core library"):
                RUNNER["fingerprint"](self.build, "inline")


if __name__ == "__main__":
    unittest.main()
