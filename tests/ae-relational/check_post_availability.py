#!/usr/bin/env python3
"""Post reconstruction must use the same available names as dense replay."""
import argparse
import csv
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--ae',required=True)
p.add_argument('--extapi',required=True)
p.add_argument('--library-path',required=True)
a=p.parse_args()
fixture=Path(__file__).with_name('PostAvailabilityLoadWitness.ll')
env=dict(os.environ,DYLD_LIBRARY_PATH=a.library_path)
with tempfile.TemporaryDirectory(prefix='svf-post-availability-') as temp:
    for mode in ('dense','semi-sparse','d3','oh-packed'):
        ledger=Path(temp)/(mode+'.queries.tsv')
        post=Path(temp)/(mode+'.post.tsv')
        command=[a.ae,'-extapi='+a.extapi,'-ae-domain=octagon','-ae-backend=native',
                 '-ae-sparsity='+mode,'-ae-relational-policy=syntax-pack','-ae-pack-max-vars=10',
                 '-stat=false','-ae-query-input-id='+hashlib.sha256(fixture.read_bytes()).hexdigest(),
                 '-ae-query-ledger='+str(ledger),'-ae-post-check='+str(post),str(fixture)]
        run=subprocess.run(command,env=env,capture_output=True,text=True,timeout=30)
        if run.returncode:
            raise RuntimeError(mode+': '+run.stderr)
        qs=list(csv.DictReader(ledger.open(),delimiter='\t'))
        ps=list(csv.DictReader(post.open(),delimiter='\t'))
        assert len(qs)==1 and qs[0]['outcome']=='Safe',(mode,qs)
        assert ps and all(r['status'] in ('Pass','Infeasible','Unreachable') for r in ps),(mode,ps)
        print(mode,'PASS')
