#!/usr/bin/env python3
"""Reparse terminal grouping logs; emit a reproducible screening archive."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import runpy


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def seconds(value):
    result = 0.0
    for field in value.split(':'):
        result = result * 60 + float(field)
    return result


def one(pattern, text):
    matches = re.findall(pattern, text, re.M)
    assert len(matches) == 1, (pattern, matches)
    return matches[0]


def collect(root, scripts, manifest):
    project = runpy.run_path(str(scripts / 'run-t5-semantic-case.py'))['projection']
    submissions = json.loads(manifest.read_text())
    expected = {(x['mode'], x['program']) for x in submissions['submissions']}
    cases = []
    for path in sorted(root.glob('*/*/result.json')):
        record = json.loads(path.read_text())
        assert record['passed'] or record.get('error'), 'nonterminal record'
        key = (record['mode'], record['program'])
        assert key in expected
        assert record['runner_sha256'] == digest(scripts / 'run-box-grouping-case.py')
        for helper, sha in record['helper_sha256'].items():
            assert sha == digest(scripts / helper)
        logs, projections = {}, []
        for stage in ('canonical', 'ordinary'):
            runs = record['canonical'] if stage == 'canonical' else record['runs']
            for run in runs:
                layout = run['layout']
                directory = path.parent / (layout + '-' + stage)
                log, timing = directory / 'analysis.log', directory / 'time.txt'
                text, time_text = log.read_text(), timing.read_text()
                logs[directory.name] = dict(log_sha256=digest(log), time_sha256=digest(timing))
                assert run['completed'] and run['exit_code'] == 0
                assert one(r'^\s*Exit status:\s*(\d+)$', time_text) == '0'
                parsed = {
                    'wall_clock': one(r'^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$', time_text),
                    'max_rss_kib': one(r'^\s*Maximum resident set size \(kbytes\):\s*(\d+)$', time_text),
                    'function_coverage_percent': one(r'^Func_Coverage_Percent\s+(.+)$', text),
                    'icfg_node_trace': one(r'^ICFG_Node_Trace\s+(.+)$', text),
                }
                for field, value in parsed.items():
                    assert run[field] == ([value] if stage == 'canonical' else value), field
                if stage == 'canonical':
                    assert f'BOX_CONTENT_LAYOUT {layout}' in text.splitlines()
                    assert 'BOX_REPRESENTATION chunk8/inline8' in text.splitlines()
                    projection, counts = project(log)
                    assert counts == run['counts']
                    assert hashlib.sha256(json.dumps(sorted(projection.items())).encode()).hexdigest() == run['projection_sha256']
                    projections.append(projection)
                else:
                    assert one(r'^Total_Time\(sec\)\s+(.+)$', text) == run['ae_seconds']
                    raw = json.loads((directory / 'record.json').read_text())
                    assert raw == {k: v for k, v in run.items() if k != 'layout'}
                    for field in ('function_coverage_percent', 'icfg_node_trace'):
                        assert [run[field]] == record['canonical'][0][field]
        derived = None
        if record['passed']:
            assert len(projections) == len(record['runs']) == 2
            assert projections[0] == projections[1]
            values = {x['layout']: dict(wall_s=seconds(x['wall_clock']),
                                       ae_s=float(x['ae_seconds']),
                                       rss_kib=int(x['max_rss_kib'])) for x in record['runs']}
            ratios = {k: values['function-base'][k] / values['registration'][k]
                      for k in ('wall_s', 'ae_s', 'rss_kib')}
            derived = dict(values=values, candidate_over_registration=ratios,
                           followup_signal=ratios['wall_s'] <= .95 or ratios['rss_kib'] <= .98)
        cases.append(dict(source=str(path), source_sha256=digest(path), record=record,
                          raw_log_hashes=logs, derived=derived))
    assert {(c['record']['mode'], c['record']['program']) for c in cases} == expected
    summary = {}
    for mode in ('semi-sparse', 'sparse'):
        rows = [c for c in cases if c['record']['mode'] == mode and c['derived']]
        summary[mode] = dict(accepted=len(rows),
            geometric_mean_candidate_over_registration={k: math.exp(sum(math.log(c['derived']['candidate_over_registration'][k]) for c in rows) / len(rows))
                                                       for k in ('wall_s', 'ae_s', 'rss_kib')},
            signal_programs=[c['record']['program'] for c in rows if c['derived']['followup_signal']])
    return dict(source_commit=submissions['source_commit'], manifest=str(manifest),
                manifest_sha256=digest(manifest), collector=str(Path(__file__).resolve()),
                collector_sha256=digest(Path(__file__)),
                scope='single-sample concurrent opportunity screen; not isolated timing, Original comparison, or deployment evidence',
                comparison='function-base / registration, same mode and host; all setup included',
                validation='all 80 raw execution logs and GNU time records reparsed; canonical per-key projections compared; input/runtime fingerprints retained from runner',
                accepted=sum(c['record']['passed'] for c in cases), failed=sum(not c['record']['passed'] for c in cases),
                summary=summary, cases=cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--scripts', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(collect(args.root, args.scripts, args.manifest), indent=2))
