#!/usr/bin/env python3
"""Tamper tests on a disposable copy of one completed real control case."""
import argparse
import json
from pathlib import Path
import runpy
import shutil
import tempfile
import unittest

COLLECT = runpy.run_path(str(Path(__file__).with_name('collect-box-directory-control.py')))['collect']


class ArchiveGate(unittest.TestCase):
    fixture = None

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.state = json.loads((self.fixture / 'result.json').read_text())
        self.case = self.root / self.state['mode'] / self.state['program']
        self.case.parent.mkdir()
        shutil.copytree(self.fixture, self.case)
        self.manifest = self.root / 'manifest.json'
        self.manifest.write_text(json.dumps(dict(source_commit='fixture', planned_tasks=[
            dict(mode=self.state['mode'], program=self.state['program'])])))

    def read(self):
        return COLLECT(self.root, self.manifest)

    def save(self):
        (self.case / 'result.json').write_text(json.dumps(self.state))

    def test_complete(self):
        result = self.read()
        self.assertEqual((result['accepted'], result['failed'], result['pending']), (1, 0, []))

    def test_live_prefix(self):
        self.state['passed'] = False
        self.save()
        self.assertEqual(self.read()['accepted'], 0)
        self.assertEqual(len(self.read()['pending']), 1)

    def test_terminal_failure(self):
        self.state.update(passed=False, error='timeout')
        self.save()
        result = self.read()
        self.assertEqual(result['failed'], 1)
        self.assertIsNone(result['cases'][0]['chunk_over_whole'])

    def test_wrong_binary_identity(self):
        self.state['fingerprints']['whole']['identity'] = self.state['fingerprints']['chunk']['identity']
        self.save()
        with self.assertRaises(ValueError):
            self.read()

    def test_changed_raw_timing(self):
        log = self.case / 'whole-ordinary/time.txt'
        # A second metric line must not silently override the real value.
        log.write_text(log.read_text() + '\nMaximum resident set size (kbytes): 1\n')
        with self.assertRaises(AssertionError):
            self.read()

    def test_missing_canonical_repeat(self):
        self.state['canonical'].pop()
        self.save()
        with self.assertRaises(AssertionError):
            self.read()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixture', type=Path, required=True)
    args, remainder = parser.parse_known_args()
    ArchiveGate.fixture = args.fixture
    unittest.main(argv=[__file__, *remainder])
