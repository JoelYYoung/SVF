#!/usr/bin/env python3
"""One coarse, same-mode grouping opportunity screen; never final timing."""
import argparse
import json
import platform
import re
import runpy
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = runpy.run_path(str(HERE / 'run-box-page-ae-case.py'))
SEM, PERF, digest = BASE['SEM'], BASE['PERF'], BASE['digest']


def check_canonical(record, references, text, layout):
    if not record['completed'] or not record['projection_sha256']:
        raise ValueError('canonical execution incomplete')
    if f'BOX_CONTENT_LAYOUT {layout}' not in text.splitlines():
        raise ValueError('content layout identity mismatch')
    if 'BOX_REPRESENTATION chunk8/inline8' not in text.splitlines():
        raise ValueError('page representation changed')
    for key in ('projection_sha256', 'counts', 'function_coverage_percent', 'icfg_node_trace'):
        if not record[key] or any(record[key] != ref[key] for ref in references):
            raise ValueError(f'canonical workload differs: {key}')


def fingerprint(build, layout):
    result = BASE['fingerprint'](build, 'inline')
    cache = (build / 'CMakeCache.txt').read_text()
    flags = {'SVF_BOX_GROUP_CONTENTS': 'ON' if layout == 'function-base' else 'OFF',
             'SVF_BOX_PACKED_PAGES': 'OFF', 'SVF_BOX_ADAPTIVE_PAGES': 'OFF',
             'SVF_BOX_STORAGE_TELEMETRY': 'OFF'}
    for key, value in flags.items():
        if f'{key}:BOOL={value}' not in cache.splitlines():
            raise ValueError(f'wrong configuration: {key}')
    for executable in ('ae', 'box-page-semantic-observer'):
        for library in ('libSvfCore', 'libSvfLLVM'):
            match = re.search(rf'{library}\S* => (\S+)', result['ldd'][executable])
            if not match or Path(match[1]).resolve() != (build / 'lib' / (library + '.so.3.4')).resolve():
                raise ValueError(f'library override: {library}')
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--study', type=Path, required=True)
    parser.add_argument('--program', required=True)
    parser.add_argument('--mode', choices=('semi-sparse', 'sparse'), required=True)
    parser.add_argument('--first', choices=('registration', 'function-base'), required=True)
    parser.add_argument('--cap-seconds', type=int, default=600)
    args = parser.parse_args()
    gate_path = args.study / 'results/page-layout-semantic-v1' / args.mode / args.program / 'result.json'
    gate = json.loads(gate_path.read_text())
    if not gate['passed'] or gate['host'] != platform.node() or gate['mode'] != args.mode or gate['program'] != args.program:
        raise ValueError('missing same-host historical canonical gate')
    bitcode, extapi = (Path(gate['manifest'][key]) for key in ('input', 'extapi'))
    for key, path in (('input', bitcode), ('extapi', extapi)):
        if digest(path) != gate['manifest'][key + '_sha256']:
            raise ValueError(f'changed {key}')
    output = args.study / 'results/grouping-screening-v1' / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    layouts = ('registration', 'function-base')
    order = (args.first, next(name for name in layouts if name != args.first))
    builds = {name: args.study / f'build-grouping-{name}-v1' for name in layouts}
    state = dict(passed=False, program=args.program, mode=args.mode, host=platform.node(),
                 timing_class='single-sample-shared-host-opportunity-screen',
                 scope='one canonical and one ordinary AE per layout; not a deployment timing gate',
                 cap_seconds=args.cap_seconds, canonical=[], runs=[],
                 old_gate_sha256=digest(gate_path), runner_sha256=digest(Path(__file__)),
                 helper_sha256={name: digest(HERE / name) for name in
                                ('run-box-page-ae-case.py', 'run-t5-semantic-case.py', 'run-t5-storage-performance-case.py')},
                 input=str(bitcode), input_sha256=digest(bitcode), extapi=str(extapi), extapi_sha256=digest(extapi))

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    try:
        before = {name: fingerprint(build, name) for name, build in builds.items()}
        state['fingerprints'] = before
        core = [before[n]['files'][str(builds[n] / 'lib/libSvfCore.so.3.4')] for n in layouts]
        if core[0] == core[1]:
            raise ValueError('identical grouping and baseline core binaries')
        for name in order:
            directory = output / (name + '-canonical')
            _, record = SEM['run_variant'](builds[name] / 'bin/box-page-semantic-observer',
                                          extapi, bitcode, args.cap_seconds, directory, args.mode)
            record['layout'] = name
            state['canonical'].append(record)
            save()
            check_canonical(record, gate['runs'], (directory / 'analysis.log').read_text(), name)
        # Include registration/preanalysis/setup in complete-process time.
        # One sample per layout is only an opportunity screen, not a speedup claim.
        for name in order[::-1]:
            directory = output / (name + '-ordinary')
            try:
                record = PERF['run_variant'](builds[name] / 'bin/ae', extapi, bitcode,
                                            args.cap_seconds, directory, args.mode)
            except RuntimeError:
                state['runs'].append(dict(layout=name, record=json.loads((directory / 'record.json').read_text())))
                raise
            record['layout'] = name
            state['runs'].append(record)
            save()
            for key in ('function_coverage_percent', 'icfg_node_trace'):
                if [record[key]] != state['canonical'][0][key]:
                    raise ValueError(f'ordinary workload differs: {key}')
        if before != {name: fingerprint(build, name) for name, build in builds.items()}:
            raise ValueError('binary/configuration changed during run')
        state['passed'] = True
    except Exception as error:
        state['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps({'passed': True, 'result': str(output / 'result.json')}))


if __name__ == '__main__':
    main()
