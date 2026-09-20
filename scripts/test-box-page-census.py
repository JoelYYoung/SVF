#!/usr/bin/env python3
"""Fail closed on incomplete or duplicated physical storage records."""
import runpy
import unittest
from pathlib import Path

RUNNER = runpy.run_path(str(Path(__file__).with_name("run-box-page-census.py")))
PARSE = RUNNER["parse_storage"]
FIXTURE = """unrelated diagnostic
BOX_STORAGE_CARRIER role=scalar states=1 unique_pages=2 occupied_slots=3
BOX_STORAGE_CARRIER role=memory states=9 unique_pages=3 occupied_slots=12
BOX_STORAGE_EVENTS allocate=10 detach=4
"""


class CensusRecords(unittest.TestCase):
    def test_preserves_roles_and_numeric_counts(self):
        result = PARSE(FIXTURE)
        self.assertEqual(result["carriers"], {
            "scalar": {"states": 1, "unique_pages": 2, "occupied_slots": 3},
            "memory": {"states": 9, "unique_pages": 3, "occupied_slots": 12}})
        self.assertEqual(result["events"], {"allocate": 10, "detach": 4})

    def test_missing_record_is_rejected(self):
        for missing in ("role=scalar", "role=memory", "BOX_STORAGE_EVENTS"):
            with self.subTest(missing=missing):
                truncated = "\n".join(line for line in FIXTURE.splitlines()
                                      if missing not in line)
                with self.assertRaisesRegex(ValueError, "missing storage"):
                    PARSE(truncated)

    def test_duplicate_record_is_rejected(self):
        for line in FIXTURE.splitlines()[1:]:
            with self.subTest(line=line):
                with self.assertRaisesRegex(ValueError, "duplicate"):
                    PARSE(FIXTURE + line + "\n")

    def test_malformed_numeric_field_is_rejected(self):
        with self.assertRaises(ValueError):
            PARSE(FIXTURE.replace("detach=4", "detach=unknown"))


if __name__ == "__main__":
    unittest.main()
