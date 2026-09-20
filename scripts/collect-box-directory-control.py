#!/usr/bin/env python3
"""Reparse complete Whole/Chunk control logs, preserving pending/failed cases."""
import argparse
import hashlib
import json
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent
RUNNER = runpy.run_path(str(HERE / 'run-box-directory-control.py'))
COMMON = runpy.run_path(str(HERE / 'collect-box-grouping-screen.py'))
digest, one, seconds = COMMON['digest'], COMMON['one'], COMMON['seconds']


def collect(root, manifest):
    submissions = json.loads(manifest.read_text())
    expected = {(x['mode'], x['program']) for x in submissions['planned_tasks']}
    cases, pending = [], []
    for mode, program in sorted(expected):
        path = root / mode / program / 'result.json'
        if not path.exists():
            pending.append([mode, program])
            continue
        record = json.loads(path.read_text())
        if not record['passed'] and not record.get('error'):
            pending.append([mode, program])
            continue
        assert (record['mode'], record['program']) == (mode, program)
        assert record['runner_sha256'] == digest(HERE / 'run-box-directory-control.py')
        for name, value in record['helper_sha256'].items():
            assert value == digest(HERE / name)
        logs, values = {}, {}
        first_projection = None
        if record['passed']:
            assert {(r['variant'], r['repeat']) for r in record['canonical']} == {
                (v, repeat) for v in RUNNER['REPRESENTATIONS'] for repeat in range(2)}
            assert len(record['canonical']) == 4 and len(record['runs']) == 2
            assert {r['variant'] for r in record['runs']} == set(RUNNER['REPRESENTATIONS'])
            cores = set()
            for variant, fingerprint in record['fingerprints'].items():
                RUNNER['check_identity'](fingerprint['identity'], variant)
                cores.update(value for path, value in fingerprint['files'].items()
                             if path.endswith('/libAbstractDomainCore.so.3.4'))
            assert len(cores) == 2
            for stage, runs in [('canonical', record['canonical']), ('ordinary', record['runs'])]:
                for run in runs:
                    variant = run['variant']
                    suffix = '-' + str(run['repeat']) if stage == 'canonical' else ''
                    directory = path.parent / (variant + '-' + stage + suffix)
                    log, timing = directory / 'analysis.log', directory / 'time.txt'
                    text, clock = log.read_text(), timing.read_text()
                    logs[directory.name] = dict(log_sha256=digest(log), time_sha256=digest(timing))
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
                    if stage == 'canonical':
                        for identity in ('BOX_REPRESENTATION ' + RUNNER['REPRESENTATIONS'][variant],
                                         'BOX_CONTENT_LAYOUT registration', 'BOX_PAGE_INTERNING off'):
                            assert identity in text.splitlines()
                        projected, counts = RUNNER['SEM']['projection'](log)
                        assert counts == run['counts']
                        assert hashlib.sha256(json.dumps(sorted(projected.items())).encode()).hexdigest() == run['projection_sha256']
                        if first_projection is None:
                            first_projection = projected
                        assert projected == first_projection
                        for key in ('counts', 'function_coverage_percent', 'icfg_node_trace'):
                            assert run[key] == record['canonical'][0][key]
                    else:
                        assert one(r'^Total_Time\(sec\)\s+(.+)$', text) == run['ae_seconds']
                        assert json.loads((directory / 'record.json').read_text()) == {
                            k: v for k, v in run.items() if k != 'variant'}
                        for key in ('function_coverage_percent', 'icfg_node_trace'):
                            assert [run[key]] == record['canonical'][0][key]
                        values[variant] = dict(wall_s=seconds(run['wall_clock']),
                                               ae_s=float(run['ae_seconds']), rss_kib=int(run['max_rss_kib']))
        else:
            for raw in path.parent.glob('*/*'):
                if raw.is_file():
                    logs[str(raw.relative_to(path.parent))] = digest(raw)
        ratios = {key: values['chunk'][key] / values['whole'][key]
                  for key in ('wall_s', 'ae_s', 'rss_kib')} if values else None
        cases.append(dict(source=str(path), source_sha256=digest(path), record=record,
                          raw_hashes=logs, values=values, chunk_over_whole=ratios))
    return dict(source_commit=submissions['source_commit'], manifest=str(manifest),
                manifest_sha256=digest(manifest), collector_sha256=digest(Path(__file__)),
                scope='same-mode same-source directory COW control, single-sample concurrent screening; not Original or isolated deployment timing',
                accepted=sum(c['record']['passed'] for c in cases),
                failed=sum(not c['record']['passed'] for c in cases), pending=pending, cases=cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = collect(args.root, args.manifest)
    if args.output:
        # Never overwrite a published evidence record.
        with args.output.open('x') as stream:
            stream.write(json.dumps(result, indent=2) + '\n')
        print(json.dumps({key: result[key] for key in ('accepted', 'failed', 'pending')}))
    else:
        print(json.dumps(result, indent=2))
