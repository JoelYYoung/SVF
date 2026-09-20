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

HISTOGRAM_FIXTURE = """BOX_STORAGE_CARRIER role=scalar unique_pages=2 occupied_slots=3
BOX_STORAGE_CARRIER role=memory unique_pages=0 occupied_slots=0
BOX_STORAGE_EVENTS allocate=2 detach=1 release=1 write_unique=3 erase_unique=0 join_shared=0 join_materialized=0
BOX_STORAGE_OCCUPANCY scope=retained name=scalar used1=1 used2=1
BOX_STORAGE_OCCUPANCY scope=retained name=memory
BOX_STORAGE_OCCUPANCY scope=event name=allocate_empty used0=2
BOX_STORAGE_OCCUPANCY scope=event name=detach_before used2=1
BOX_STORAGE_OCCUPANCY scope=event name=release used0=1
BOX_STORAGE_OCCUPANCY scope=event name=write_unique_before used1=2 used2=1
BOX_STORAGE_OCCUPANCY scope=event name=erase_unique_before
BOX_STORAGE_OCCUPANCY scope=event name=join_shared
BOX_STORAGE_OCCUPANCY scope=event name=join_clone_before
"""

WORK_FIXTURE = HISTOGRAM_FIXTURE + "".join(
    f"BOX_STORAGE_WORK kind={kind} count={1 if kind == 'clone' else 0} direct=0 "
    "occupied_slots=0 copied_slots=0 relocated_slots=0 allocated_slot_bytes=0 "
    "peak_operation_overlap_slot_bytes=0\n"
    for kind in ("clone", "grow", "shrink", "promote", "demote", "insert", "update", "erase"))


class CensusRecords(unittest.TestCase):
    def test_work_records_are_complete_and_match_clone_events(self):
        result = PARSE(WORK_FIXTURE)
        self.assertEqual(len(result["work"]), 8)
        self.assertEqual(result["work"]["clone"]["count"], 1)
        self.assertNotIn("work", PARSE(HISTOGRAM_FIXTURE))

    def test_work_corruption_rejected(self):
        lines = WORK_FIXTURE.splitlines()
        first = next(line for line in lines if "kind=clone" in line)
        invalid = [WORK_FIXTURE.replace(first + "\n", ""), WORK_FIXTURE + first + "\n",
                   WORK_FIXTURE.replace("kind=clone count=1", "kind=clone count=2"),
                   WORK_FIXTURE.replace("copied_slots=0", "copied_slots=-1", 1),
                   WORK_FIXTURE.replace("copied_slots=0", "copied_slots=9", 1),
                   WORK_FIXTURE.replace("direct=0", "direct=2", 1),
                   WORK_FIXTURE.replace("kind=grow count=0", "kind=grow count=0 direct=1")]
        for text in invalid:
            with self.subTest(text=text), self.assertRaises(ValueError):
                PARSE(text)

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

    def test_histograms_preserve_event_weighting_and_empty_carriers(self):
        histograms = PARSE(HISTOGRAM_FIXTURE)["occupancy"]
        self.assertEqual(histograms["event:write_unique_before"], {1: 2, 2: 1})
        self.assertEqual(histograms["retained:memory"], {})
        self.assertNotIn("occupancy", PARSE(FIXTURE))

    def test_histogram_missing_duplicate_and_inconsistent_records_rejected(self):
        line = "BOX_STORAGE_OCCUPANCY scope=event name=join_shared\n"
        for invalid in (HISTOGRAM_FIXTURE.replace(line, ""),
                        HISTOGRAM_FIXTURE + line,
                        HISTOGRAM_FIXTURE.replace("used0=2", "used0=3"),
                        HISTOGRAM_FIXTURE.replace("occupied_slots=3", "occupied_slots=4"),
                        HISTOGRAM_FIXTURE.replace("used1=1 used2=1", "used1=1"),
                        HISTOGRAM_FIXTURE.replace("used0=2", "used0=-2"),
                        HISTOGRAM_FIXTURE.replace("used0=2", "used0=2 used0=2"),
                        HISTOGRAM_FIXTURE.replace("used0=2", "usedX=2")):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                PARSE(invalid)


if __name__ == "__main__":
    unittest.main()
