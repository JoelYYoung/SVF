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
COWRITE = runpy.run_path(str(HERE / 'analyze-box-cowrite-trace.py'))


class Controls(unittest.TestCase):
    def test_cowrite_no_edges_falls_back_to_marginal_packing(self):
        variables = {(index, 0, 0, 0) for index in range(19)}
        marginal = {key: (key[0] * 7) % 11 for key in variables}
        self.assertEqual(
            COWRITE['relational_mapping'](variables, marginal, {}),
            COWRITE['marginal_mapping'](variables, marginal),
        )

    def test_cowrite_missing_top_noop_does_not_detach_shared_page(self):
        variables = [(index, 0, 0, 0) for index in range(8)]
        mapping = {key: 0 for key in variables}
        changed = [(key, True) for key in variables[:6]]
        replay = COWRITE['Replay'](mapping)
        result = replay.run([
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0, changed, variables[:6]),
            ('S', 3, COWRITE['COPY_CONSTRUCT'], 2, 1, 0, 0),
            ('M', 4, 2, 0, 2, 0, 0, 0, [], [variables[7]]),
        ])
        self.assertEqual(result.get('detaches', 0), 0)
        self.assertEqual(result.get('cloned_slots', 0), 0)

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
