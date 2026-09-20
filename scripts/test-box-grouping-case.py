#!/usr/bin/env python3
import copy
from pathlib import Path
import runpy
import unittest

CHECK = runpy.run_path(str(Path(__file__).with_name('run-box-grouping-case.py')))['check_canonical']


class CanonicalGate(unittest.TestCase):
    def setUp(self):
        self.record = dict(completed=True, projection_sha256='abc', counts={'VALUE': 3},
                           function_coverage_percent=['10%'], icfg_node_trace=['20'])
        self.text = 'BOX_CONTENT_LAYOUT function-base\nBOX_REPRESENTATION chunk8/inline8\n'

    def test_accept(self):
        CHECK(self.record, [copy.deepcopy(self.record)], self.text, 'function-base')

    def test_reject_identity(self):
        for text in ('', self.text.replace('function-base', 'registration'),
                     self.text.replace('inline8', 'adaptive8')):
            with self.assertRaises(ValueError):
                CHECK(self.record, [self.record], text, 'function-base')

    def test_reject_semantics_or_workload(self):
        for key, value in [('projection_sha256', 'wrong'), ('counts', {'VALUE': 2}),
                           ('function_coverage_percent', ['9%']), ('icfg_node_trace', ['21'])]:
            other = dict(self.record, **{key: value})
            with self.assertRaises(ValueError):
                CHECK(self.record, [self.record, other], self.text, 'function-base')

    def test_reject_incomplete(self):
        for key, value in [('completed', False), ('projection_sha256', None), ('counts', {})]:
            with self.assertRaises(ValueError):
                CHECK(dict(self.record, **{key: value}), [self.record], self.text, 'function-base')


if __name__ == '__main__':
    unittest.main()
