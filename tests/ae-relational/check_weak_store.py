#!/usr/bin/env python3
"""R03 has a concrete counterexample; Post alone cannot validate the transfer."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ae', required=True)
    parser.add_argument('--extapi', required=True)
    parser.add_argument('--library-path', default='')
    args = parser.parse_args()
    out = Path(tempfile.mkdtemp(prefix='weak-store-check-'))
    inp = Path(__file__).parent / 'MultiTargetStoreWitness.ll'
    env = os.environ.copy()
    if args.library_path:
        env['DYLD_LIBRARY_PATH'] = args.library_path
        env['LD_LIBRARY_PATH'] = args.library_path
    records = []
    identities = None
    for domain, policy, modes in [('box', 'whole', ['dense', 'semi-sparse']),
                                   ('octagon', 'whole', ['dense', 'semi-sparse']),
                                   ('octagon', 'syntax-pack', ['dense', 'semi-sparse', 'oh-packed'])]:
        for mode in modes:
            stem = out / f'{domain}-{policy}-{mode}'
            ledger, post = Path(str(stem)+'.queries.tsv'), Path(str(stem)+'.post.tsv')
            cmd = [args.ae, '-extapi='+args.extapi, '-ae-domain='+domain,
                   '-ae-backend=native', '-ae-relational-policy='+policy,
                   '-ae-sparsity='+mode, '-ae-pack-max-vars=10', '-ae-relational-calls=through',
                   '-handle-recur=top', '-model-consts=true', '-model-arrays=true',
                   '-pre-field-sensitive=false', '-stat=false', '-overflow=false', '-null-deref=false',
                   '-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
                   '-ae-query-ledger='+str(ledger), '-ae-post-check='+str(post), str(inp)]
            result = subprocess.run(cmd, env=env, text=True, capture_output=True, timeout=60)
            Path(str(stem)+'.stdout').write_text(result.stdout)
            Path(str(stem)+'.stderr').write_text(result.stderr)
            records.append({'command': cmd, 'status': result.returncode})
            (out/'commands.json').write_text(json.dumps(records, indent=2))
            assert result.returncode == 0, (out, cmd, result.stderr)
            queries = list(csv.DictReader(ledger.open(), delimiter='\t'))
            posts = list(csv.DictReader(post.open(), delimiter='\t'))
            ids = {q['query_id'] for q in queries}
            assert len(ids) == 3, (out, queries)
            assert identities is None or identities == ids, (out, 'query identity changed')
            identities = ids
            by_line = {json.loads(q['source_location'].split(': ', 1)[1])['ln']: q['outcome']
                       for q in queries}
            # p=&q, o=5, v=100 refutes y>=100. The two interval bounds hold.
            assert by_line[13] == 'May', (out, domain, policy, mode, by_line)
            if domain == 'octagon':
                assert by_line[14] == by_line[15] == 'Safe', (out, by_line)
            assert posts and all(p['status'] in ['Pass', 'Infeasible', 'Unreachable'] for p in posts), (out, posts)
    print('Weak-store regression passed; raw artifacts:', out)


if __name__ == '__main__':
    main()
