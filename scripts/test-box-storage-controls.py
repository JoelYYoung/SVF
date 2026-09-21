#!/usr/bin/env python3
"""Fail-closed identities and terminal-only collection contracts."""
import collections
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
    def test_cowrite_v1_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.tsv'
            trace.write_text('# box-cowrite-trace-v1\n')
            with self.assertRaisesRegex(ValueError, 'conflates Bottom'):
                COWRITE['read_trace'](trace)

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

    def test_cowrite_hyperedges_preserve_explicit_pair_mapping(self):
        variables = {(index, 0, 0, 0) for index in range(37)}
        marginal = collections.Counter()
        explicit = collections.Counter()
        hyperedges = collections.Counter()
        touched_sets = [
            tuple(sorted(variables)),
            tuple(sorted(key for key in variables if key[0] % 2 == 0)),
            tuple(sorted(key for key in variables if key[0] % 3 != 0)),
        ]
        for weight, touched in enumerate(touched_sets, 1):
            for key in touched:
                marginal[key] += weight
            hyperedges[touched] += weight
            for index, left in enumerate(touched):
                for right in touched[index + 1:]:
                    explicit[(left, right)] += weight
        self.assertEqual(
            COWRITE['relational_mapping'](variables, marginal, {}, hyperedges),
            COWRITE['relational_mapping'](variables, marginal, explicit),
        )

    def test_cowrite_large_set_does_not_materialize_pairs(self):
        variables = tuple((index, 0, 0, 0) for index in range(1025))
        mapping = COWRITE['relational_mapping'](
            set(variables), collections.Counter(dict.fromkeys(variables, 1)),
            {}, {variables: 7})
        self.assertEqual(len(mapping), len(variables))
        self.assertLessEqual(max(collections.Counter(mapping.values()).values()), 8)

    def test_cowrite_relation_uses_changes_not_physical_touches(self):
        variables = [(index, 0, 0, 0) for index in range(40)]
        changed = variables[:2]
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.tsv'
            trace.write_text(
                'M 1 1 0 1 0 0 0 2 '
                + ' '.join('+' + ':'.join(map(str, key)) for key in changed)
                + ' T 40 '
                + ' '.join(':'.join(map(str, key)) for key in variables)
                + '\n')
            _, _, marginal, pair, hyperedges, _, _ = COWRITE['read_trace'](trace)
        self.assertEqual(pair, {(changed[0], changed[1]): 1})
        self.assertEqual(hyperedges, {})
        self.assertTrue(all(marginal[key] == 1 for key in variables))

    def test_cowrite_bottom_clear_does_not_create_layout_hyperedge(self):
        variables = [(index, 0, 0, 0) for index in range(40)]
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.tsv'
            trace.write_text(
                'M 1 1 3 1 0 0 1 40 '
                + ' '.join('-' + ':'.join(map(str, key)) for key in variables)
                + ' T 1 ' + ':'.join(map(str, variables[0])) + '\n')
            _, _, marginal, pair, hyperedges, _, _ = COWRITE['read_trace'](trace)
        self.assertEqual(pair, {})
        self.assertEqual(hyperedges, {})
        self.assertEqual(marginal, {variables[0]: 1})

    def test_cowrite_bottom_transition_detaches_before_clear(self):
        variables = [(index, 0, 0, 0) for index in range(2)]
        replay = COWRITE['Replay']({key: 0 for key in variables})
        result = replay.run([
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0,
             [(key, True) for key in variables], variables),
            ('S', 3, COWRITE['COPY_CONSTRUCT'], 2, 1, 0, 0),
            ('M', 4, 2, 3, 2, 0, 0, 1,
             [(key, False) for key in variables], [variables[0]]),
        ])
        self.assertEqual(result.get('detaches', 0), 1)
        self.assertEqual(result.get('cloned_slots', 0), 2)
        self.assertEqual(result.get('live_pages', 0), 1)

    def test_cowrite_trace_records_storage_work_by_epoch(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace.tsv'
            trace.write_text('D 10 7 1 0 3\nW 7 1 3 3\n')
            *_, raw, raw_epochs = COWRITE['read_trace'](trace)
        self.assertEqual(raw, {'detaches': 1, 'cloned_slots': 3})
        self.assertEqual(raw_epochs[7], {'detaches': 1, 'cloned_slots': 3})

    def test_cowrite_replay_reports_first_epoch_mismatch(self):
        key = (1, 0, 0, 0)
        replay = COWRITE['Replay']({key: 0})
        replay.run([
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0, [(key, True)], [key]),
        ], {1: {'detaches': 1, 'cloned_slots': 1}})
        self.assertEqual(replay.diagnostics['mismatch_count'], 1)
        mismatch = replay.diagnostics['first_mismatches'][0]
        self.assertEqual(mismatch['epoch'], 1)
        self.assertEqual(mismatch['expected_detaches'], 1)
        self.assertEqual(mismatch['actual_detaches'], 0)

    def test_clone_conflict_separates_trigger_from_cloned_slots(self):
        variables = [(index, 0, 0, 0) for index in range(9)]
        current = {key: key[0] // 8 for key in variables}
        events = [
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0,
             [(key, True) for key in variables], variables),
            ('S', 3, COWRITE['COPY_CONSTRUCT'], 2, 1, 0, 0),
            ('M', 4, 2, 0, 2, 0, 0, 0, [], [variables[0]]),
        ]
        observed = COWRITE['Replay'](
            current, collect_clone_conflicts=True)
        baseline = observed.run(events)
        self.assertEqual(baseline.get('detaches', 0), 1)
        self.assertEqual(baseline.get('cloned_slots', 0), 8)
        self.assertEqual(sum(observed.clone_conflicts.values()), 7)
        candidate = COWRITE['clone_conflict_mapping'](
            set(variables), collections.Counter(), observed.clone_conflicts)
        self.assertNotEqual(candidate[variables[0]], candidate[variables[1]])
        improved = COWRITE['Replay'](candidate).run(events)
        self.assertEqual(improved.get('detaches', 0), 1)
        self.assertEqual(improved.get('cloned_slots', 0), 1)

    def test_clone_conflict_ignores_unique_writes_and_missing_top(self):
        variables = [(index, 0, 0, 0) for index in range(3)]
        replay = COWRITE['Replay'](
            {key: 0 for key in variables}, collect_clone_conflicts=True)
        replay.run([
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0,
             [(variables[0], True), (variables[1], True)], variables[:2]),
            ('M', 3, 2, 0, 1, 0, 0, 0, [], [variables[0]]),
            ('S', 4, COWRITE['COPY_CONSTRUCT'], 2, 1, 0, 0),
            ('M', 5, 3, 0, 2, 0, 0, 0, [], [variables[2]]),
        ])
        self.assertEqual(replay.clone_conflicts, {})
        self.assertEqual(replay.clone_triggers, {})

    def test_future_conflict_search_reports_feasible_best_round(self):
        variables = [(index, 0, 0, 0) for index in range(9)]
        events = [
            ('S', 1, COWRITE['CREATE'], 1, 0, 0, 0),
            ('M', 2, 1, 0, 1, 0, 0, 0,
             [(key, True) for key in variables], variables),
            ('S', 3, COWRITE['COPY_CONSTRUCT'], 2, 1, 0, 0),
            ('M', 4, 2, 0, 2, 0, 0, 0, [], [variables[0]]),
        ]
        mapping, report = COWRITE['future_conflict_search'](
            events, set(variables), collections.Counter(),
            {key: key[0] // 8 for key in variables}, 4)
        self.assertGreaterEqual(report['rounds_evaluated'], 2)
        self.assertEqual(report['candidates'][report['best_round']]
                         ['metrics']['cloned_slots'], 1)
        self.assertEqual(COWRITE['Replay'](mapping).run(events)
                         ['cloned_slots'], 1)

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
