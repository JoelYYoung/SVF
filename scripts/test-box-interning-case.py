#!/usr/bin/env python3
from pathlib import Path
import runpy
import unittest

PARSE = runpy.run_path(str(Path(__file__).with_name('run-box-interning-case.py')))['pool_stats']


class PoolGate(unittest.TestCase):
    line = ('BOX_PAGE_POOL policy=publish publications=5 candidates=4 probes=3 '
            'hits=1 comparisons=1 expired=0 evictions=0 frozen_detaches=2 entries=2 peak_entries=2\n')

    def test_valid_and_off(self):
        self.assertEqual(PARSE(self.line, 'publish')['hits'], 1)
        self.assertIsNone(PARSE('ordinary output\n', 'off'))

    def test_wrong_policy_and_missing(self):
        for text, policy in [(self.line, 'off'), (self.line, 'write'), ('', 'publish'),
                             (self.line * 2, 'publish')]:
            with self.assertRaises(ValueError):
                PARSE(text, policy)

    def test_inactive_or_over_budget(self):
        for text in [self.line.replace('probes=3', 'probes=0'),
                     self.line.replace('peak_entries=2', 'peak_entries=32769'),
                     self.line.replace(' hits=1', '')]:
            with self.assertRaises(ValueError):
                PARSE(text, 'publish')


if __name__ == '__main__':
    unittest.main()
