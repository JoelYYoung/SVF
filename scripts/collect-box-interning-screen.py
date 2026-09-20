#!/usr/bin/env python3
"""Revalidate terminal interning pilot records without interpreting live prefixes."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent
COMMON = runpy.run_path(str(HERE / 'collect-box-grouping-screen.py'))
RUNNER = runpy.run_path(str(HERE / 'run-box-interning-case.py'))
PROJECT = RUNNER['SEM']['projection']
digest, one, seconds = COMMON['digest'], COMMON['one'], COMMON['seconds']


def collect(root, manifest_path):
    manifest = json.loads(manifest_path.read_text())
    expected = {(x['mode'], x['program']) for x in manifest['planned_tasks']}
    cases, pending = [], []
    for mode, program in sorted(expected):
        path = root / mode / program / 'result.json'
        if not path.exists():
            pending.append([mode, program])
            continue
        state = json.loads(path.read_text())
        if not state['passed'] and not state.get('error'):
            pending.append([mode, program])
            continue
        assert (state['mode'], state['program']) == (mode, program)
        assert state['runner_sha256'] == digest(HERE / 'run-box-interning-case.py')
        for name, value in state['helper_sha256'].items():
            assert digest(HERE / name) == value
        hashes, projections, values = {}, [], {}
        if state['passed']:
            assert len(state['canonical']) == len(state['runs']) == 3
            for stage, runs in [('canonical', state['canonical']), ('ordinary', state['runs'])]:
                assert {run['policy'] for run in runs} == set(RUNNER['POLICIES'])
                for run in runs:
                    policy = run['policy']
                    directory = path.parent / (policy + '-' + stage)
                    log, timing = directory / 'analysis.log', directory / 'time.txt'
                    text, clock = log.read_text(), timing.read_text()
                    hashes[directory.name] = dict(log=digest(log), timing=digest(timing))
                    assert run['completed'] and run['exit_code'] == 0
                    assert one(r'^\s*Exit status:\s*(\d+)$', clock) == '0'
                    metrics = {
                        'wall_clock': one(r'^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$', clock),
                        'max_rss_kib': one(r'^\s*Maximum resident set size \(kbytes\):\s*(\d+)$', clock),
                        'function_coverage_percent': one(r'^Func_Coverage_Percent\s+(.+)$', text),
                        'icfg_node_trace': one(r'^ICFG_Node_Trace\s+(.+)$', text),
                    }
                    for key, value in metrics.items():
                        assert run[key] == ([value] if stage == 'canonical' else value)
                    assert run['pool_stats'] == RUNNER['pool_stats'](text, policy)
                    if stage == 'canonical':
                        for identity in ('BOX_REPRESENTATION chunk8/inline8',
                                         'BOX_CONTENT_LAYOUT registration',
                                         'BOX_PAGE_INTERNING ' + policy):
                            assert identity in text.splitlines()
                        projected, counts = PROJECT(log)
                        assert counts == run['counts']
                        assert hashlib.sha256(json.dumps(sorted(projected.items())).encode()).hexdigest() == run['projection_sha256']
                        projections.append(projected)
                    else:
                        assert one(r'^Total_Time\(sec\)\s+(.+)$', text) == run['ae_seconds']
                        assert json.loads((directory / 'record.json').read_text()) == {
                            k: v for k, v in run.items() if k not in ('policy', 'pool_stats')}
                        for key in ('function_coverage_percent', 'icfg_node_trace'):
                            assert [run[key]] == state['canonical'][0][key]
                        values[policy] = dict(wall_s=seconds(run['wall_clock']),
                                              ae_s=float(run['ae_seconds']),
                                              rss_kib=int(run['max_rss_kib']),
                                              pool_stats=run['pool_stats'])
            assert all(p == projections[0] for p in projections)
        else:
            # Preserve unsuccessful attempts; do not turn a partial log into a
            # complete projection or performance sample.
            for raw in path.parent.glob('*/*'):
                if raw.is_file():
                    hashes[str(raw.relative_to(path.parent))] = digest(raw)
        ratios = {p: {metric: values[p][metric] / values['off'][metric]
                       for metric in ('wall_s', 'ae_s', 'rss_kib')}
                  for p in ('write', 'publish')} if values else None
        cases.append(dict(source=str(path), source_sha256=digest(path), record=state,
                          raw_hashes=hashes, ordinary_values=values,
                          candidate_over_off=ratios))
    return dict(source_commit=manifest['source_commit'], manifest=str(manifest_path),
                manifest_sha256=digest(manifest_path), collector=str(Path(__file__).resolve()),
                collector_sha256=digest(Path(__file__)),
                scope='four-program mechanism pilot, same-mode candidates/own OFF; concurrent single samples, not Original or deployment evidence',
                counter_boundary='publications counts calls to internPendingPages (including write flushes), not unique ICFG publications. Hits are lookup events, not unique retained bytes saved.',
                accepted=sum(c['record']['passed'] for c in cases),
                failed=sum(not c['record']['passed'] for c in cases),
                pending=pending, cases=cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(collect(args.root, args.manifest), indent=2))
