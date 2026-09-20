#!/usr/bin/env python3
"""Exercise balanced-repeat gates using disposable real-log fixtures."""
import argparse
import json
from pathlib import Path
import runpy
import shutil
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
MODULE = runpy.run_path(str(HERE / 'run-box-directory-repeats.py'))


class Repeats(unittest.TestCase):
    fixture = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.gate = json.loads((self.fixture / 'result.json').read_text())
        self.case = self.root / self.gate['mode'] / self.gate['program']
        self.case.mkdir(parents=True)
        shutil.copyfile(self.fixture / 'result.json', self.case / 'gate.json')
        task = dict(mode=self.gate['mode'], program=self.gate['program'], first='whole',
                    gate_sha256=MODULE['digest'](self.case / 'gate.json'))
        self.manifest = self.root / 'manifest.json'
        self.manifest.write_text(json.dumps(dict(planned_tasks=[task])))
        self.state = dict(**task, passed=True, host=self.gate['host'],
                          runner_sha256=MODULE['digest'](HERE / 'run-box-directory-repeats.py'),
                          helper_sha256={}, lock_acquired_epoch=0, lock_released_epoch=100, runs=[])
        # These copied logs are collector test data, never performance evidence.
        for n, (phase, repeat, variant) in enumerate(MODULE['schedule']('whole')):
            directory = self.case / f'{phase}-{repeat}-{variant}'
            shutil.copytree(self.fixture / (variant + '-ordinary'), directory)
            record = json.loads((directory / 'record.json').read_text())
            self.state['runs'].append(dict(phase=phase, repeat=repeat, variant=variant,
                                          before=dict(epoch=n * 2), after=dict(epoch=n * 2 + 1), record=record))

    def collect(self):
        (self.case / 'result.json').write_text(json.dumps(self.state))
        return MODULE['collect'](self.root, self.manifest)

    def test_balanced(self):
        for first in ('whole', 'chunk'):
            plan = MODULE['schedule'](first)
            self.assertEqual(len(plan), 10)
            self.assertEqual(sum(v == 'whole' for p, r, v in plan[2::2]), 2)
        result = self.collect()
        self.assertEqual((result['accepted'], result['failed'], result['pending']), (1, 0, []))
        self.assertEqual(len(result['cases'][0]['values']['whole']), 4)

    def test_missing_repeat(self):
        self.state['runs'].pop()
        with self.assertRaises(AssertionError):
            self.collect()

    def test_coverage_mismatch(self):
        self.state['runs'][0]['record']['icfg_node_trace'] = 'wrong'
        with self.assertRaises(AssertionError):
            self.collect()

    def test_duplicate_metric(self):
        path = self.case / 'measured-0-whole/time.txt'
        path.write_text(path.read_text() + '\nMaximum resident set size (kbytes): 1\n')
        with self.assertRaises(AssertionError):
            self.collect()

    def test_wrong_variant(self):
        self.state['runs'][0]['record'] = self.state['runs'][1]['record']
        with self.assertRaises(AssertionError):
            self.collect()

    def test_outside_lock(self):
        self.state['runs'][0]['before']['epoch'] = -1
        with self.assertRaises(AssertionError):
            self.collect()

    def test_pending_and_failed(self):
        self.state['passed'] = False
        self.assertEqual(len(self.collect()['pending']), 1)
        self.state['error'] = 'timeout'
        result = self.collect()
        self.assertEqual(result['failed'], 1)
        self.assertIsNone(result['cases'][0]['chunk_over_whole'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', type=Path, required=True)
    args, remainder = parser.parse_known_args()
    Repeats.fixture = args.fixture
    unittest.main(argv=[__file__, *remainder])
