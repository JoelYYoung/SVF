#!/usr/bin/env python3
"""R14: immutable caller-frame meet and conservative non-reentry admission."""
import argparse, csv, hashlib, json, os, re, subprocess, tempfile, time
from datetime import datetime, timezone
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--ae', required=True); p.add_argument('--extapi', required=True)
p.add_argument('--library-path', default=''); p.add_argument('--output')
p.add_argument('--reference-before', action='store_true')
p.add_argument('--without-harness-contract', action='store_true')
p.add_argument('--configs', default='dense,semi,pack,pack-semi,oh',
               help='D3 integration is separate; add d3,pack-d3 on an integrated build')
a = p.parse_args()
out = Path(a.output) if a.output else Path(tempfile.mkdtemp(prefix='caller-frame-check-'))
if a.output: out.mkdir(parents=True, exist_ok=False)
print('raw artifacts:', out, flush=True)
env = dict(os.environ, DYLD_LIBRARY_PATH=a.library_path, LD_LIBRARY_PATH=a.library_path,
           SVF_AE_TRACE_CALLER_FRAME='1')
cases = {'R14_shared_callee_frame': [None, None], 'R14n_shared_callee_writes': ['May', 'May'],
         'CallerFrameCase1': [None, 'May'], 'CallerFrameCase2': ['May'],
         'CallerFrameCase3': [None, 'May'], 'CallerFrameCase4': [], 'CallerFrameCase5': []}
configs = {'dense': ('dense', 'whole'), 'semi': ('semi-sparse', 'whole'),
           'pack': ('dense', 'syntax-pack'), 'pack-semi': ('semi-sparse', 'syntax-pack'),
           'oh': ('oh-packed', 'syntax-pack'), 'd3': ('d3', 'whole'),
           'pack-d3': ('d3', 'syntax-pack')}
configs = {key: configs[key] for key in a.configs.split(',')}
records = []; failed = []
for name, expected in cases.items():
    inp = Path(__file__).parent / (name + '.ll')
    identity = hashlib.sha256(inp.read_bytes()).hexdigest()
    keys = []
    for cfg, (mode, policy) in configs.items():
        stem = out / (name + '-' + cfg)
        cmd = [a.ae, '-extapi=' + a.extapi, '-ae-domain=octagon', '-ae-backend=native',
               '-ae-sparsity=' + mode, '-ae-relational-policy=' + policy, '-ae-pack-max-vars=10',
               '-ae-relational-calls=through', '-handle-recur=top', '-model-consts=true',
               '-model-arrays=true', '-pre-field-sensitive=false', '-stat=false',
               '-overflow=false', '-null-deref=false', '-ae-query-input-id=' + identity,
               '-ae-query-ledger=' + str(stem) + '.queries.tsv',
               '-ae-post-check=' + str(stem) + '.post.tsv', str(inp)]
        if not a.reference_before and not a.without_harness_contract:
            cmd.insert(1, '-ae-harness-nondet-no-callback=true')
        started = datetime.now(timezone.utc).isoformat(); t = time.monotonic()
        try:
            r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
            status, stdout, stderr = r.returncode, r.stdout, r.stderr
        except subprocess.TimeoutExpired as e:
            status = 124
            stdout = e.stdout or b''; stderr = e.stderr or b''
            if isinstance(stdout, bytes): stdout = stdout.decode(errors='replace')
            if isinstance(stderr, bytes): stderr = stderr.decode(errors='replace')
        wall = time.monotonic() - t; ended = datetime.now(timezone.utc).isoformat()
        Path(str(stem) + '.stdout').write_text(stdout)
        Path(str(stem) + '.stderr').write_text(stderr)
        qp = Path(str(stem) + '.queries.tsv'); pp = Path(str(stem) + '.post.tsv')
        qs = list(csv.DictReader(qp.open(), delimiter='\t')) if qp.exists() else []
        ps = list(csv.DictReader(pp.open(), delimiter='\t')) if pp.exists() else []
        actual = [q['outcome'] for q in qs]; want = list(expected)
        if name == 'R14_shared_callee_frame' and mode != 'd3':
            want = ['May', 'May'] if (a.reference_before or a.without_harness_contract) and mode == 'semi-sparse' else ['Safe', 'Safe']
        decisions = [int(x) for x in re.findall(r'AE_CALLER_FRAME return=\d+ non_reentrant=(\d)', stderr)]
        ok = status == 0 and len(actual) == len(want) and all(
            e is None or v == e for v, e in zip(actual, want)) and ps and all(
            x['status'] in ['Pass', 'Infeasible', 'Unreachable'] for x in ps)
        if not a.reference_before and mode in ('semi-sparse', 'd3'):
            if name in ('R14_shared_callee_frame', 'R14n_shared_callee_writes', 'CallerFrameCase3'):
                ok = ok and bool(decisions) and (
                    set(decisions) == {0} if a.without_harness_contract else 1 in decisions)
            if name in ('CallerFrameCase4', 'CallerFrameCase5'):
                ok = ok and bool(decisions) and set(decisions) == {0}
        keys.append({q['query_id'] for q in qs})
        records.append(dict(case=name, config=cfg, command=cmd, status=status,
                            started_at=started, ended_at=ended, wall_seconds=wall,
                            queries=qs, post=ps, expected=want, admission=decisions,
                            test_passed=bool(ok)))
        (out / 'results.json').write_text(json.dumps(records, indent=2))
        print(name, cfg, status, actual, decisions, bool(ok), flush=True)
        if not ok: failed.append(stem.name)
    if any(k != keys[0] for k in keys): failed.append(name + '-query-identity')
if failed: raise SystemExit(str(failed))
