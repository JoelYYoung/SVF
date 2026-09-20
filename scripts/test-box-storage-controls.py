#!/usr/bin/env python3
"""Fail-closed identities and terminal-only collection contracts."""
import json
from pathlib import Path
import runpy
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
DIRECTORY = runpy.run_path(str(HERE / 'run-box-directory-control.py'))
COLLECT = runpy.run_path(str(HERE / 'collect-box-interning-screen.py'))


class Controls(unittest.TestCase):
    def test_actual_identity(self):
        for variant, representation in DIRECTORY['REPRESENTATIONS'].items():
            identity = 'representation=' + representation + '\npool_policy=off\n'
            DIRECTORY['check_identity'](identity, variant)
            other = 'chunk' if variant == 'whole' else 'whole'
            for wrong in (identity.replace('off', 'write'), identity + 'extra\n'):
                with self.assertRaises(ValueError):
                    DIRECTORY['check_identity'](wrong, variant)
            with self.assertRaises(ValueError):
                DIRECTORY['check_identity'](identity, other)

    def test_live_prefix_not_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = root / 'manifest.json'
            manifest.write_text(json.dumps(dict(source_commit='test', planned_tasks=[
                dict(mode='semi-sparse', program='p')])))
            def read():
                return COLLECT['collect'](root, manifest)
            self.assertEqual(read()['pending'], [['semi-sparse', 'p']])
            path = root / 'semi-sparse/p/result.json'
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(dict(passed=False)))
            self.assertEqual(read()['accepted'], 0)
            self.assertEqual(read()['pending'], [['semi-sparse', 'p']])
            state = dict(passed=False, error='timeout', mode='semi-sparse', program='p',
                         runner_sha256=COLLECT['digest'](HERE / 'run-box-interning-case.py'),
                         helper_sha256={})
            path.write_text(json.dumps(state))
            result = read()
            self.assertEqual((result['failed'], result['accepted'], result['pending']), (1, 0, []))
            self.assertIsNone(result['cases'][0]['candidate_over_off'])
            state['runner_sha256'] = 'changed'
            path.write_text(json.dumps(state))
            with self.assertRaises(AssertionError):
                read()


if __name__ == '__main__':
    unittest.main()
