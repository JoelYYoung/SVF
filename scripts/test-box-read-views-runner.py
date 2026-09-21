#!/usr/bin/env python3
"""Identity and artifact contracts for the borrowed-read comparison."""

from pathlib import Path
import runpy
import tempfile
import unittest
from unittest.mock import patch

RUNNER = runpy.run_path(
    str(Path(__file__).with_name("run-box-read-views-case.py"))
)


class ReadViewsFingerprint(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.build = Path(self.temporary.name)
        files = (
            "bin/ae",
            "bin/box-page-contract",
            "bin/box-page-semantic-observer",
            "lib/libAbstractDomainCore.so.3.4",
            "lib/libSvfCore.so.3.4",
            "lib/libSvfLLVM.so.3.4",
        )
        for name in files:
            path = self.build / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(name)
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            "SVF_BOX_PAGE_INTERNING:STRING=OFF\n"
            "SVF_BOX_WHOLE_DIRECTORY:BOOL=OFF\n"
            "SVF_BOX_GROUP_CONTENTS:BOOL=OFF\n"
            "SVF_BOX_PACKED_PAGES:BOOL=OFF\n"
            "SVF_BOX_ADAPTIVE_PAGES:BOOL=OFF\n"
            "SVF_BOX_STORAGE_TELEMETRY:BOOL=OFF\n"
            "SVF_ENABLE_ASSERTIONS:BOOL=OFF\n"
            "SVF_WARN_AS_ERROR:BOOL=ON\n"
        )

    def output(self, command, **_kwargs):
        if command[-1] == "--interning-identity":
            return "representation=chunk8/inline8\npool_policy=off\n"
        libraries = [
            self.build / "lib" / f"{name}.so.3.4"
            for name in ("libAbstractDomainCore", "libSvfCore", "libSvfLLVM")
        ]
        return "".join(
            f"{library.name} => {library} (0x1234)\n"
            for library in libraries
        )

    def test_baseline_does_not_require_candidate_contract(self):
        with patch("subprocess.check_output", side_effect=self.output):
            result = RUNNER["fingerprint"](self.build)
        self.assertEqual(result["identity"], [
            "representation=chunk8/inline8", "pool_policy=off"
        ])

    def test_candidate_requires_read_contract(self):
        with patch("subprocess.check_output", side_effect=self.output):
            with self.assertRaises(FileNotFoundError):
                RUNNER["fingerprint"](self.build, True)
            contract = self.build / "bin/box-read-view-contract"
            contract.write_text("contract")
            result = RUNNER["fingerprint"](self.build, True)
        self.assertIn(str(contract), result["files"])

    def test_wrong_representation_is_rejected(self):
        def wrong(command, **kwargs):
            if command[-1] == "--interning-identity":
                return "representation=whole/inline8\npool_policy=off\n"
            return self.output(command, **kwargs)
        with patch("subprocess.check_output", side_effect=wrong):
            with self.assertRaisesRegex(RuntimeError, "unexpected Box representation"):
                RUNNER["fingerprint"](self.build)


if __name__ == "__main__":
    unittest.main()
