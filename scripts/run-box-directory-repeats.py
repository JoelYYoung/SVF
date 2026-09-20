#!/usr/bin/env python3
"""Balanced directory-control repeats; campaign-serial, not host-exclusive."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import platform
import runpy
import statistics
import time

HERE = Path(__file__).resolve().parent
CONTROL = runpy.run_path(str(HERE / 'run-box-directory-control.py'))
COMMON = runpy.run_path(str(HERE / 'collect-box-grouping-screen.py'))
digest, one, seconds = COMMON['digest'], COMMON['one'], COMMON['seconds']


def schedule(first):
    order = (first, 'chunk' if first == 'whole' else 'whole')
    return [('warmup', 0, v) for v in order] + [
        ('measured', r, v) for r in range(4)
        for v in (order if r % 2 == 0 else order[::-1])]


def resource_snapshot():
    return dict(epoch=time.time(), loadavg=os.getloadavg(),
                affinity=sorted(os.sched_getaffinity(0)),
                cpu=Path('/proc/stat').read_text().splitlines()[0],
                memory=Path('/proc/meminfo').read_text())


def validate_run(directory, record, gate):
    text = (directory / 'analysis.log').read_text()
    clock = (directory / 'time.txt').read_text()
    assert record['completed'] and record['exit_code'] == 0
    assert record['command'] in [r['command'] for r in gate['runs']]
    assert one(r'^\s*Exit status:\s*(\d+)$', clock) == '0'
    metrics = dict(
        wall_clock=one(r'^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$', clock),
        max_rss_kib=one(r'^\s*Maximum resident set size \(kbytes\):\s*(\d+)$', clock),
        ae_seconds=one(r'^Total_Time\(sec\)\s+(.+)$', text),
        function_coverage_percent=one(r'^Func_Coverage_Percent\s+(.+)$', text),
        icfg_node_trace=one(r'^ICFG_Node_Trace\s+(.+)$', text))
    assert all(record[k] == v for k, v in metrics.items())
    for key in ('function_coverage_percent', 'icfg_node_trace'):
        assert [record[key]] == gate['canonical'][0][key]
    assert json.loads((directory / 'record.json').read_text()) == record
    return dict(wall_s=seconds(record['wall_clock']), ae_s=float(record['ae_seconds']),
                rss_kib=int(record['max_rss_kib']),
                user_s=float(one(r'^\s*User time \(seconds\):\s*(.+)$', clock)),
                system_s=float(one(r'^\s*System time \(seconds\):\s*(.+)$', clock)))


def run(args):
    gate_path = args.study / 'results/directory-control-v1' / args.mode / args.program / 'result.json'
    gate = json.loads(gate_path.read_text())
    assert gate['passed'] and not gate.get('error')
    assert (gate['host'], gate['mode'], gate['program']) == (platform.node(), args.mode, args.program)
    builds = {v: args.study / f'build-directory-control-{v}-v1' for v in CONTROL['REPRESENTATIONS']}
    output = args.study / 'results/directory-repeats-v1' / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    (output / 'gate.json').write_text(gate_path.read_text())
    state = dict(passed=False, host=platform.node(), mode=args.mode, program=args.program,
                 first=args.first, gate_sha256=digest(gate_path), runner_sha256=digest(Path(__file__)),
                 helper_sha256={n: digest(HERE / n) for n in (
                     'run-box-directory-control.py', 'run-t5-semantic-case.py',
                     'run-t5-storage-performance-case.py', 'collect-box-grouping-screen.py')},
                 timing_class='campaign-serial-shared-host-balanced-four-repeat',
                 cap_seconds=args.cap_seconds, runs=[], lock_requested_epoch=time.time())

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    save()
    try:
        # This is measurement exclusion, not a second job scheduler. Each managed
        # task owns one case; waiting is outside the analyzer timing and recorded.
        lock_path = args.study / ('directory-repeats-v1-' + platform.node() + '.lock')
        with lock_path.open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            state['lock_acquired_epoch'] = time.time()
            state['lock_wait_s'] = state['lock_acquired_epoch'] - state['lock_requested_epoch']
            save()
            before = {v: CONTROL['fingerprint'](b, v) for v, b in builds.items()}
            assert before == gate['fingerprints'], 'changed frozen build'
            for key in ('input', 'extapi'):
                assert digest(Path(gate[key])) == gate[key + '_sha256']
            for phase, repeat, variant in schedule(args.first):
                directory = output / f'{phase}-{repeat}-{variant}'
                sample = dict(phase=phase, repeat=repeat, variant=variant, before=resource_snapshot())
                record = CONTROL['PERF']['run_variant'](
                    builds[variant] / 'bin/ae', Path(gate['extapi']), Path(gate['input']),
                    args.cap_seconds, directory, args.mode)
                sample.update(after=resource_snapshot(), record=record)
                state['runs'].append(sample)
                save()
                validate_run(directory, record, gate)
            assert before == {v: CONTROL['fingerprint'](b, v) for v, b in builds.items()}
            assert digest(gate_path) == state['gate_sha256']
            for key in ('input', 'extapi'):
                assert digest(Path(gate[key])) == gate[key + '_sha256']
            state['lock_released_epoch'] = time.time()
            state['passed'] = True
    except Exception as error:
        state['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps(dict(passed=True, result=str(output / 'result.json'))))


def collect(root, manifest):
    submissions = json.loads(manifest.read_text())
    cases, pending = [], []
    intervals = {}
    for task in submissions['planned_tasks']:
        mode, program = task['mode'], task['program']
        path = root / mode / program / 'result.json'
        if not path.exists():
            pending.append([mode, program])
            continue
        state = json.loads(path.read_text())
        if not state['passed'] and not state.get('error'):
            pending.append([mode, program])
            continue
        assert (state['mode'], state['program']) == (mode, program)
        assert state['runner_sha256'] == digest(Path(__file__))
        for name, value in state['helper_sha256'].items():
            assert value == digest(HERE / name)
        raw = {str(p.relative_to(path.parent)): digest(p)
               for p in path.parent.glob('*/*') if p.is_file()}
        values = {'whole': [], 'chunk': []}
        gate = json.loads((path.parent / 'gate.json').read_text())
        assert digest(path.parent / 'gate.json') == state['gate_sha256']
        assert state['gate_sha256'] == task['gate_sha256']
        assert gate['passed'] and not gate.get('error')
        assert (gate['host'], gate['mode'], gate['program']) == (state['host'], mode, program)
        if state['passed']:
            assert state['first'] == task['first']
            assert [(r['phase'], r['repeat'], r['variant']) for r in state['runs']] == schedule(state['first'])
            for sample in state['runs']:
                expected = next(r['command'] for r in gate['runs'] if r['variant'] == sample['variant'])
                assert sample['record']['command'] == expected
                stage = '{phase}-{repeat}-{variant}'.format(**sample)
                value = validate_run(path.parent / stage, sample['record'], gate)
                assert state['lock_acquired_epoch'] <= sample['before']['epoch'] <= sample['after']['epoch'] <= state['lock_released_epoch']
                if sample['phase'] == 'measured':
                    values[sample['variant']].append(value)
            intervals.setdefault(state['host'], []).append(
                (state['lock_acquired_epoch'], state['lock_released_epoch']))
        summary = {v: {k: dict(median=statistics.median(r[k] for r in runs),
                               minimum=min(r[k] for r in runs), maximum=max(r[k] for r in runs))
                       for k in runs[0]} for v, runs in values.items() if runs}
        ratios = {k: summary['chunk'][k]['median'] / summary['whole'][k]['median']
                  for k in ('wall_s', 'ae_s', 'rss_kib')} if summary else None
        cases.append(dict(source=str(path), source_sha256=digest(path), record=state,
                          raw_hashes=raw, values=values, summary=summary, chunk_over_whole=ratios))
    for runs in intervals.values():
        ordered = sorted(runs)
        assert all(a[1] <= b[0] for a, b in zip(ordered, ordered[1:])), 'overlapping campaign cases'
    return dict(manifest_sha256=digest(manifest), runner_sha256=digest(Path(__file__)),
                scope='same-mode fixed-binary four-repeat medians; campaign exclusion only, external host load retained',
                accepted=sum(c['record']['passed'] for c in cases),
                failed=sum(not c['record']['passed'] for c in cases), pending=pending, cases=cases)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest='action', required=True)
    launch = sub.add_parser('run')
    launch.add_argument('--study', type=Path, required=True)
    launch.add_argument('--program', required=True)
    launch.add_argument('--mode', choices=('semi-sparse', 'sparse'), required=True)
    launch.add_argument('--first', choices=CONTROL['REPRESENTATIONS'], required=True)
    launch.add_argument('--cap-seconds', type=int, default=600)
    archive = sub.add_parser('collect')
    archive.add_argument('--root', type=Path, required=True)
    archive.add_argument('--manifest', type=Path, required=True)
    archive.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.action == 'run':
        run(args)
    else:
        result = collect(args.root, args.manifest)
        with args.output.open('x') as stream:
            stream.write(json.dumps(result, indent=2) + '\n')
        print(json.dumps({k: result[k] for k in ('accepted', 'failed', 'pending')}))
