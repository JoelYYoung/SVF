#!/usr/bin/env python3
"""Same-source Whole/Chunk control. Coarse shared-host screen, not final timing."""
import argparse
import json
import platform
from pathlib import Path
import re
import runpy
import subprocess

HERE = Path(__file__).resolve().parent
SEM = runpy.run_path(str(HERE / 'run-t5-semantic-case.py'))
PERF = runpy.run_path(str(HERE / 'run-t5-storage-performance-case.py'))
digest = SEM['digest']
REPRESENTATIONS = {'whole': 'whole/inline8', 'chunk': 'chunk8/inline8'}


def check_identity(identity, variant):
    if identity.splitlines() != ['representation=' + REPRESENTATIONS[variant], 'pool_policy=off']:
        raise ValueError('wrong runtime directory representation or pool policy')


def fingerprint(build, variant):
    identity = subprocess.check_output([str(build / 'bin/box-page-contract'),
                                        '--interning-identity'], text=True)
    check_identity(identity, variant)
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    flags = dict(SVF_BOX_WHOLE_DIRECTORY='ON' if variant == 'whole' else 'OFF',
                 SVF_BOX_GROUP_CONTENTS='OFF', SVF_BOX_PACKED_PAGES='OFF',
                 SVF_BOX_ADAPTIVE_PAGES='OFF', SVF_BOX_STORAGE_TELEMETRY='OFF',
                 SVF_ENABLE_ASSERTIONS='OFF', SVF_WARN_AS_ERROR='ON')
    if any(f'{k}:BOOL={v}' not in cache for k, v in flags.items()):
        raise ValueError('unexpected CMake flags')
    if 'CMAKE_BUILD_TYPE:STRING=Release' not in cache or 'SVF_BOX_PAGE_INTERNING:STRING=OFF' not in cache:
        raise ValueError('wrong release/pool configuration')
    files = [build / 'bin' / n for n in ('ae', 'box-page-semantic-observer', 'box-page-contract')]
    libraries = [build / 'lib' / (n + '.so.3.4') for n in
                 ('libAbstractDomainCore', 'libSvfCore', 'libSvfLLVM')]
    linkage = {}
    for executable in files:
        text = subprocess.check_output(['ldd', str(executable)], text=True)
        linkage[executable.name] = re.sub(r'\(0x[0-9a-fA-F]+\)', '(address)', text)
        for library in libraries[:1] if executable.name == 'box-page-contract' else libraries:
            match = re.search(re.escape(library.name.split('.so')[0]) + r'\S* => (\S+)', text)
            if not match or Path(match[1]).resolve() != library.resolve():
                raise ValueError('unexpected runtime library: ' + str(library))
    return dict(identity=identity, files={str(p): digest(p) for p in files + libraries},
                ldd=linkage, cache_sha256=digest(build / 'CMakeCache.txt'))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--study', type=Path, required=True)
    parser.add_argument('--program', required=True)
    parser.add_argument('--mode', choices=('semi-sparse', 'sparse'), required=True)
    parser.add_argument('--first', choices=REPRESENTATIONS, required=True)
    parser.add_argument('--cap-seconds', type=int, default=600)
    args = parser.parse_args()
    gate_path = args.study / 'results/grouping-screening-v1' / args.mode / args.program / 'result.json'
    gate = json.loads(gate_path.read_text())
    if not gate['passed'] or (gate['host'], gate['mode'], gate['program']) != (platform.node(), args.mode, args.program):
        raise ValueError('missing same-host logical workload prerequisite')
    bitcode, extapi = (Path(gate[k]) for k in ('input', 'extapi'))
    for key, path in (('input', bitcode), ('extapi', extapi)):
        if digest(path) != gate[key + '_sha256']:
            raise ValueError('changed ' + key)
    builds = {v: args.study / f'build-directory-control-{v}-v1' for v in REPRESENTATIONS}
    output = args.study / 'results/directory-control-v1' / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    state = dict(passed=False, host=platform.node(), mode=args.mode, program=args.program,
                 timing_class='single-sample-shared-host-screen-not-final-timing',
                 input=str(bitcode), input_sha256=digest(bitcode), extapi=str(extapi),
                 extapi_sha256=digest(extapi), old_gate_sha256=digest(gate_path),
                 runner_sha256=digest(Path(__file__)), cap_seconds=args.cap_seconds,
                 helper_sha256={n: digest(HERE / n) for n in
                                ('run-t5-semantic-case.py', 'run-t5-storage-performance-case.py')},
                 canonical=[], runs=[])

    def save():
        (output / 'result.json').write_text(json.dumps(state, indent=2) + '\n')

    try:
        before = {v: fingerprint(b, v) for v, b in builds.items()}
        state['fingerprints'] = before
        hashes = {before[v]['files'][str(b / 'lib/libAbstractDomainCore.so.3.4')] for v, b in builds.items()}
        if len(hashes) != 2:
            raise ValueError('two labels resolved to the same core binary')
        save()
        order = (args.first, next(v for v in builds if v != args.first))
        projections = []
        # Two reversed-order canonical repetitions gate both representation and repeat stability.
        for repeat in range(2):
            for variant in order if repeat == 0 else order[::-1]:
                directory = output / f'{variant}-canonical-{repeat}'
                projection, record = SEM['run_variant'](builds[variant] / 'bin/box-page-semantic-observer',
                                                       extapi, bitcode, args.cap_seconds, directory, args.mode)
                record.update(variant=variant, repeat=repeat)
                state['canonical'].append(record)
                save()
                if not record['completed'] or projection is None:
                    raise ValueError('incomplete canonical execution')
                text = (directory / 'analysis.log').read_text().splitlines()
                for identity in ('BOX_REPRESENTATION ' + REPRESENTATIONS[variant],
                                 'BOX_CONTENT_LAYOUT registration', 'BOX_PAGE_INTERNING off'):
                    if identity not in text:
                        raise ValueError('observer identity mismatch')
                for key in ('projection_sha256', 'counts', 'function_coverage_percent', 'icfg_node_trace'):
                    if not record[key] or any(record[key] != ref[key] for ref in gate['canonical']):
                        raise ValueError('historical logical workload mismatch: ' + key)
                projections.append(projection)
        if any(p != projections[0] for p in projections):
            raise ValueError('exact canonical projection mismatch')
        for variant in order[::-1]:
            directory = output / (variant + '-ordinary')
            try:
                record = PERF['run_variant'](builds[variant] / 'bin/ae', extapi, bitcode,
                                             args.cap_seconds, directory, args.mode)
            except RuntimeError:
                state['runs'].append(dict(variant=variant, record=json.loads((directory / 'record.json').read_text())))
                raise
            record['variant'] = variant
            state['runs'].append(record)
            save()
            for key in ('function_coverage_percent', 'icfg_node_trace'):
                if [record[key]] != state['canonical'][0][key]:
                    raise ValueError('ordinary workload mismatch: ' + key)
        if before != {v: fingerprint(b, v) for v, b in builds.items()}:
            raise ValueError('build changed during experiment')
        state['passed'] = True
    except Exception as error:
        state['error'] = str(error)
        raise
    finally:
        save()
    print(json.dumps(dict(passed=True, result=str(output / 'result.json'))))


if __name__ == '__main__':
    main()
