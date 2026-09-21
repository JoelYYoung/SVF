#!/usr/bin/env python3

import importlib.util
from pathlib import Path
import unittest


MODULE = Path(__file__).with_name("run-box-operation-census.py")
SPEC = importlib.util.spec_from_file_location("operation_census", MODULE)
CENSUS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CENSUS)


def fixture(dropped=0):
    tracked = 3 - dropped
    lines = []
    for operation in CENSUS.OPERATIONS:
        lines.append(
            f"BOX_OPERATION_SUMMARY op={operation} calls=3 tracked={tracked} "
            f"dropped={dropped} elapsed_ns=30 normalization_ns=40 "
            "result_canonical_bytes=50")
        for capacity in CENSUS.CAPACITIES:
            hits = min(tracked, 1 if capacity == 64 else 2)
            lines.append(
                f"BOX_OPERATION_LRU op={operation} capacity={capacity} "
                f"hits={hits} hit_elapsed_ns={hits * 5}")
    lines.append(
        "BOX_OPERATION_STATES unique=4 canonical_bytes=100 "
        "canonical_budget_bytes=1000 entry_shallow_bytes=160 pending=0")
    for capacity in CENSUS.CAPACITIES:
        lines.append(
            f"BOX_OPERATION_CACHE capacity={capacity} entries=4 "
            "peak_result_canonical_bytes=80 key_shallow_bytes=96")
    return "\n".join(lines)


class OperationCensusTest(unittest.TestCase):
    def test_complete_fixture(self):
        parsed = CENSUS.parse_operations(fixture())
        self.assertEqual(parsed["states"]["unique"], 4)
        self.assertEqual(
            parsed["ideal_removable_operation_cost"]["64"]["hits"], 6)

    def test_dropped_is_explicit(self):
        parsed = CENSUS.parse_operations(fixture(dropped=1))
        self.assertEqual(parsed["summaries"]["join"]["dropped"], 1)

    def test_missing_row_rejected(self):
        with self.assertRaises(ValueError):
            CENSUS.parse_operations(fixture().replace(
                "BOX_OPERATION_CACHE capacity=4096", "BROKEN", 1))

    def test_non_monotone_hits_rejected(self):
        with self.assertRaises(ValueError):
            CENSUS.parse_operations(fixture().replace(
                "op=join capacity=256 hits=2", "op=join capacity=256 hits=0"))


if __name__ == "__main__":
    unittest.main()
