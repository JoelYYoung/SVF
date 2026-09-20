#!/usr/bin/env python3
"""One same-host OFF/WRITE/PUBLISH semantic gate and ordinary AE screen."""
import argparse
import json
import platform
import re
import runpy
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
BASE = runpy.run_path(str(HERE / 'run-box-page-ae-case.py'))
GROUP = runpy.run_path(str(HERE / 'run-box-grouping-case.py'))
SEM, PERF, digest = BASE['SEM'], BASE['PERF'], BASE['digest']
POLICIES = ('off', 'write', 'publish')


def pool_stats(text, policy):
    lines = [line for line in text.splitlines() if line.startswith('BOX_PAGE_POOL ')]
    if policy == 'off':
        if lines:
            raise ValueError('OFF run unexpectedly used a pool')
        return None
    if len(lines) != 1:
        raise ValueError('missing or repeated pool summary')
    fields = dict(word.split('=', 1) for word in lines[0].split()[1:])
    if fields.pop('policy') != policy:
        raise ValueError('wrong active pool policy')
    values = {key: int(value) for key, value in fields.items()}
    required = {'publications', 'candidates', 'probes', 'hits', 'comparisons',
                'expired', 'evictions', 'frozen_detaches', 'entries', 'peak_entries'}
    if values.keys() != required or not values['probes'] or values['peak_entries'] > 32768:
        raise ValueError('pool inactive, incomplete counters, or exceeded index budget')
    return values


def fingerprint(build, policy):
    result = BASE['fingerprint'](build, 'inline')
    identity = subprocess.check_output([str(build / 'bin/box-page-contract'),
                                        '--interning-identity'], text=True)
    if identity.splitlines() != ['representation=chunk8/inline8', 'pool_policy=' + policy]:
        raise ValueError('wrong core pool identity')
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    if f'SVF_BOX_PAGE_INTERNING:STRING={policy.upper()}' not in cache:
        raise ValueError('wrong pool configuration')
    for option in ('SVF_BOX_GROUP_CONTENTS', 'SVF_BOX_PACKED_PAGES',
                   'SVF_BOX_ADAPTIVE_PAGES', 'SVF_BOX_STORAGE_TELEMETRY'):
        if f'{option}:BOOL=OFF' not in cache:
            raise ValueError('unexpected configuration: ' + option)
    for executable in ('ae', 'box-page-semantic-observer'):
        for library in ('libSvfCore', 'libSvfLLVM'):
            match = re.search(rf'{library}\S* => (\S+)', result['ldd'][executable])
            if not match or Path(match[1]).resolve() != (build / 'lib' / (library + '.so.3.4')).resolve():
                raise ValueError('runtime library override: ' + library)
    result['pool_identity'] = identity
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--study', type=Path, required=True)
    parser.add_argument('--program', required=True)
    parser.add_argument('--mode', choices=('semi-sparse', 'sparse'), required=True)
    parser.add_argument('--first', choices=POLICIES, required=True)
    parser.add_argument('--cap-seconds', type=int, default=600)
    args = parser.parse_args()
    gate_path = args.study / 'results/grouping-screening-v1' / args.mode / args.program / 'result.json'
    gate = json.loads(gate_path.read_text())
    if not gate['passed'] or gate['host'] != platform.node() or gate['mode'] != args.mode or gate['program'] != args.program:
        raise ValueError('missing same-host canonical prerequisite')
    bitcode, extapi = (Path(gate[key]) for key in ('input', 'extapi'))
    for key, path in (('input', bitcode), ('extapi', extapi)):
        if digest(path) != gate[key + '_sha256']:
            raise ValueError('changed ' + key)
    builds = {name: args.study / f'build-intern-{name}-v1' for name in POLICIES}
    offset = POLICIES.index(args.first)
    order = POLICIES[offset:] + POLICIES[:offset]
    output = args.study / 'results/interning-screening-v1' / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    state = dict(passed=False, program=args.program, mode=args.mode, host=platform.node(),
                 timing_class='single-sample-shared-host-opportunity-screen',
                 scope='same logical workload; setup/hash/cleanup included; not final timing',
                 cap_seconds=args.cap_seconds, canonical=[], runs=[],
                 input=str(bitcode), input_sha256=digest(bitcode),
                 extapi=str(extapi), extapi_sha256=digest(extapi),
                 gate_sha256=digest(gate_path), runner_sha256=digest(Path(__file__)),
                 helper_sha256={name: digest(HERE / name) for name in
                                ('run-box-page-ae-case.py', 'run-box-grouping-case.py',
                                 'run-t5-semantic-case.py', 'run-t5-storage-performance-case.py')})

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    try:
        before = {name: fingerprint(build, name) for name, build in builds.items()}
        state['fingerprints'] = before
        if len({before[name]['files'][str(builds[name] / 'lib/libAbstractDomainCore.so.3.4')]
                for name in POLICIES}) != 3:
            raise ValueError('pool variants resolved to duplicate core binaries')
        save()
        for policy in order:
            directory = output / (policy + '-canonical')
            _, record = SEM['run_variant'](builds[policy] / 'bin/box-page-semantic-observer',
                                          extapi, bitcode, args.cap_seconds, directory, args.mode)
            record['policy'] = policy
            state['canonical'].append(record)
            save()
            text = (directory / 'analysis.log').read_text()
            GROUP['check_canonical'](record, gate['canonical'], text, 'registration')
            if 'BOX_PAGE_INTERNING ' + policy not in text.splitlines():
                raise ValueError('canonical pool identity mismatch')
            record['pool_stats'] = pool_stats(text, policy)
        for policy in order[::-1]:
            directory = output / (policy + '-ordinary')
            try:
                record = PERF['run_variant'](builds[policy] / 'bin/ae', extapi, bitcode,
                                            args.cap_seconds, directory, args.mode)
            except RuntimeError:
                state['runs'].append(dict(policy=policy, record=json.loads((directory / 'record.json').read_text())))
                raise
            record['policy'] = policy
            state['runs'].append(record)
            save()
            record['pool_stats'] = pool_stats((directory / 'analysis.log').read_text(), policy)
            for key in ('function_coverage_percent', 'icfg_node_trace'):
                if [record[key]] != state['canonical'][0][key]:
                    raise ValueError('ordinary workload differs: ' + key)
        if before != {name: fingerprint(build, name) for name, build in builds.items()}:
            raise ValueError('binary/configuration changed during experiment')
        state['passed'] = True
    except Exception as error:
        state['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps({'passed': True, 'result': str(output / 'result.json')}))


if __name__ == '__main__':
    main()
