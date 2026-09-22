#!/usr/bin/env python3

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class CrossPageOpportunityRunner(unittest.TestCase):
    def test_sealed_trace_and_g0_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trace = root / "trace.tsv"
            trace.write_text(
                "# box-cowrite-trace-v2\n"
                "S 1 0 1 0 0 0\n"
                "M 2 1 0 1 0 0 0 2 +0:0:0:0 +8:0:0:0 "
                "T 2 0:0:0:0 8:0:0:0\n")
            trace_sha = hashlib.sha256(trace.read_bytes()).hexdigest()
            source = root / "source.json"
            source.write_text(json.dumps({
                "passed": True,
                "program": "tiny",
                "sparsity": "semi-sparse",
                "semantic_projection_exact": True,
                "semantic_coverage_exact": True,
                "semantic_trace_exact": True,
                "trace_matches_semantic_coverage": True,
                "trace_matches_semantic_icfg": True,
                "trace_path": str(trace),
                "trace_sha256": trace_sha,
                "trace_bytes": trace.stat().st_size,
                "replay": {
                    "validation": {"detach_match": True,
                                   "cloned_slots_match": True},
                    "raw": {"detaches": 0, "cloned_slots": 0},
                },
            }))
            completed = subprocess.run([
                sys.executable, str(HERE / "run-box-cross-page-opportunity.py"),
                "--source-result", str(source),
                "--output-root", str(root / "output"),
                "--cap-seconds", "30",
            ], text=True, capture_output=True, check=True)
            result = json.loads(completed.stdout)
            self.assertTrue(result["passed"])
            replay = result["analysis"]["replay"]
            self.assertEqual(set(replay), {
                "g0_current", "g1_registration_dense",
                "g4_retained_support"})
            self.assertEqual(replay["g0_current"]["logical_page_refs"], 2)
            self.assertEqual(
                replay["g1_registration_dense"]["logical_page_refs"], 1)


if __name__ == "__main__":
    unittest.main()
