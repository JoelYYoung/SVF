#!/usr/bin/env python3
"""Concrete counterexamples gate memory effects independently of shared Post."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ae', required=True)
    ap.add_argument('--extapi', required=True)
    ap.add_argument('--library-path', default='')
    a = ap.parse_args()
    out = Path(tempfile.mkdtemp(prefix='memory-effects-check-'))
    env = os.environ.copy()
    if a.library_path:
        env['DYLD_LIBRARY_PATH'] = env['LD_LIBRARY_PATH'] = a.library_path
    cases = {'UnknownTargetStoreWitness': ['May'], 'ExternalCopyWitness': ['May', 'Safe'],
             'PointerRoundTripStoreWitness': ['May', None],
             'MixedPointerRoundTripWitness': ['May'],
             'MemoryCopyFallbackWitness': ['May', 'Safe', 'May', 'May'],
             'AnnotatedCopyFallbackWitness': ['May', 'May']}
    configs = [('box', 'whole', 'dense'), ('box', 'whole', 'semi-sparse'),
               ('octagon', 'whole', 'dense'), ('octagon', 'whole', 'semi-sparse'),
               ('octagon', 'syntax-pack', 'dense'), ('octagon', 'syntax-pack', 'semi-sparse'),
               ('octagon', 'syntax-pack', 'oh-packed')]
    records = []
    for name, expected in cases.items():
        inp = Path(__file__).parent/(name+'.ll')
        ids = None
        for domain, policy, mode in configs:
            stem = out/f'{name}-{domain}-{policy}-{mode}'
            ledger, post = Path(str(stem)+'.queries.tsv'), Path(str(stem)+'.post.tsv')
            cmd = [a.ae, '-extapi='+a.extapi, '-ae-domain='+domain, '-ae-backend=native',
                   '-ae-sparsity='+mode, '-ae-relational-policy='+policy, '-ae-pack-max-vars=10',
                   '-ae-relational-calls=through', '-handle-recur=top', '-model-consts=true',
                   '-model-arrays=true', '-pre-field-sensitive=false', '-stat=false',
                   '-overflow=false', '-null-deref=false', '-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
                   '-ae-query-ledger='+str(ledger), '-ae-post-check='+str(post), str(inp)]
            r = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=60)
            Path(str(stem)+'.stdout').write_text(r.stdout)
            Path(str(stem)+'.stderr').write_text(r.stderr)
            records.append({'command': cmd, 'status': r.returncode})
            (out/'commands.json').write_text(json.dumps(records, indent=2))
            assert r.returncode == 0, (out, name, mode, r.stderr)
            qs = list(csv.DictReader(ledger.open(), delimiter='\t'))
            ps = list(csv.DictReader(post.open(), delimiter='\t'))
            current_ids = [q['query_id'] for q in qs]
            assert ids is None or ids == current_ids, (out, name, 'identity')
            ids = current_ids
            assert len(qs) == len(expected), (out, name, qs)
            for q, e in zip(qs, expected):
                assert q['outcome'] in ([e] if e else ['Safe', 'May']), (out, name, domain, policy, mode, q, e)
            assert ps and all(p['status'] in ['Pass', 'Infeasible', 'Unreachable'] for p in ps), (out, name, ps)
    print('Memory-effect regressions passed; raw artifacts:', out)


if __name__ == '__main__':
    main()
