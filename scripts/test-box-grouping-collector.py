#!/usr/bin/env python3
"""Parser boundary checks; the real-log gate separately checks all 20 cases."""
from pathlib import Path
import runpy
import unittest

MODULE = runpy.run_path(str(Path(__file__).with_name('collect-box-grouping-screen.py')))


class CollectorTests(unittest.TestCase):
    def test_wall_clock_units(self):
        self.assertAlmostEqual(MODULE['seconds']('0:04.72'), 4.72)
        self.assertAlmostEqual(MODULE['seconds']('1:37.82'), 97.82)
        self.assertAlmostEqual(MODULE['seconds']('2:01:37.82'), 7297.82)

    def test_requires_exactly_one_metric(self):
        self.assertEqual(MODULE['one'](r'^value (.+)$', 'value 17\n'), '17')
        for text in ('', 'value 17\nvalue 18\n'):
            with self.assertRaises(AssertionError):
                MODULE['one'](r'^value (.+)$', text)


if __name__ == '__main__':
    unittest.main()
