#!/usr/bin/env python3
"""Negative checks for phase output and archived raw timing validation."""
import copy
import json
import runpy
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
PHASE = runpy.run_path(str(HERE / "run-box-page-phases.py"))
SCREEN = runpy.run_path(str(HERE / "summarize-box-page-screening.py"))


class PhaseTests(unittest.TestCase):
    output = ("representation=chunk8/adaptive8\nphase=write_cow occupancy=3 "
              "rounds=100 pages=64 seconds=0.2 semantic_digest=123\n")

    def test_parse(self):
        self.assertEqual(PHASE["parse"](self.output, "adaptive", "write_cow", 3, 100), (0.2, 123))

    def test_wrong_representation(self):
        with self.assertRaises(ValueError):
            PHASE["parse"](self.output, "inline", "write_cow", 3, 100)

    def test_wrong_workload(self):
        with self.assertRaises(ValueError):
            PHASE["parse"](self.output, "adaptive", "write_unique", 3, 100)

    def test_invalid_time(self):
        for invalid in ("0", "-1", "nan", "inf"):
            with self.assertRaises(ValueError):
                PHASE["parse"](self.output.replace("seconds=0.2", "seconds=" + invalid),
                               "adaptive", "write_cow", 3, 100)

    def test_wall_clock(self):
        self.assertEqual(SCREEN["seconds"]("1:02:03.5"), 3723.5)

    def test_raw_log_validation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            row = dict(exit_code=0, completed=True, ae_seconds="1.25",
                       function_coverage_percent="2.00%", icfg_node_trace="42",
                       wall_clock="0:02.50", max_rss_kib="1024")
            (root / "record.json").write_text(json.dumps(row))
            (root / "analysis.log").write_text(
                "Total_Time(sec) 1.25\nFunc_Coverage_Percent 2.00%\nICFG_Node_Trace 42\n")
            timing = ("Elapsed (wall clock) time (h:mm:ss or m:ss): 0:02.50\n"
                      "Maximum resident set size (kbytes): 1024\nExit status: 0\n")
            (root / "time.txt").write_text(timing)
            self.assertEqual(len(SCREEN["validate_run"](root, row)), 3)
            changed = copy.deepcopy(row)
            changed["ae_seconds"] = "0.25"
            with self.assertRaises(ValueError):
                SCREEN["validate_run"](root, changed)
            (root / "time.txt").write_text(timing.replace("status: 0", "status: 124"))
            with self.assertRaises(ValueError):
                SCREEN["validate_run"](root, row)
            (root / "time.txt").write_text(timing)
            with (root / "analysis.log").open("a") as stream:
                stream.write("Total_Time(sec) 1.25\n")
            with self.assertRaises(ValueError):
                SCREEN["validate_run"](root, row)


if __name__ == "__main__":
    unittest.main()
