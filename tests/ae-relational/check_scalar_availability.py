#!/usr/bin/env python3
"""R08: entry-policy reachability must not introduce an unvisited caller."""
import argparse, csv, hashlib, json, os, subprocess, tempfile, time
from pathlib import Path
from datetime import datetime, timezone

p=argparse.ArgumentParser(); p.add_argument('--ae',required=True)
p.add_argument('--extapi',required=True); p.add_argument('--library-path',default='')
a=p.parse_args(); out=Path(tempfile.mkdtemp(prefix='scalar-availability-'))
print('raw artifacts:',out,flush=True)
env=dict(os.environ,DYLD_LIBRARY_PATH=a.library_path,LD_LIBRARY_PATH=a.library_path)
inp=Path(__file__).parent/'R08_unvisited_caller.ll'
identity=hashlib.sha256(inp.read_bytes()).hexdigest(); records=[]; failed=[]
for entry in ['main','no-main']:
    keys=[]
    for policy in ['whole','syntax-pack']:
        for mode in ['dense','semi-sparse','d3']:
            stem=out/(entry+'-'+policy+'-'+mode)
            cmd=[a.ae,'-extapi='+a.extapi,'-ae-domain=octagon','-ae-backend=native',
                 '-ae-sparsity='+mode,'-ae-relational-policy='+policy,'-ae-pack-max-vars=10',
                 '-ae-fun-entry='+entry,'-ae-relational-calls=through','-handle-recur=top',
                 '-model-consts=true','-model-arrays=true','-pre-field-sensitive=false',
                 '-stat=false','-overflow=false','-null-deref=false','-ae-query-input-id='+identity,
                 '-ae-query-ledger='+str(stem)+'.queries.tsv','-ae-post-check='+str(stem)+'.post.tsv',str(inp)]
            start=datetime.now(timezone.utc).isoformat(); t=time.monotonic()
            r=subprocess.run(cmd,env=env,text=True,capture_output=True,timeout=60)
            end=datetime.now(timezone.utc).isoformat(); wall=time.monotonic()-t
            Path(str(stem)+'.stdout').write_text(r.stdout); Path(str(stem)+'.stderr').write_text(r.stderr)
            qp=Path(str(stem)+'.queries.tsv'); pp=Path(str(stem)+'.post.tsv')
            qs=list(csv.DictReader(qp.open(),delimiter='\t')) if qp.exists() else []
            ps=list(csv.DictReader(pp.open(),delimiter='\t')) if pp.exists() else []
            # The positive roundtrip remains May in the shared context-insensitive
            # engine. Require coverage, the negative control, and original Post.
            ok=r.returncode==0 and len(qs)==2 and qs[1]['outcome']=='May' and ps and all(
                x['status'] in ['Pass','Infeasible','Unreachable'] for x in ps)
            if entry=='no-main':
                # The shared Post checker requires exactly one selected entry;
                # no-main selects both main and g on this input.
                # Test that other selected roots execute (including g), that
                # D3 no longer rejects the store, and certification is refused.
                # This does not certify no-main semantics or precision.
                ok=r.returncode==2 and len(qs)==2 and qs[1]['outcome']=='May' and len(ps)==1 and ps[0]['status']=='Unsupported'
            keys.append({q['query_id'] for q in qs})
            records.append(dict(command=cmd,status=r.returncode,started_at=start,ended_at=end,
                                wall_seconds=wall,queries=qs,post=ps,test_passed=bool(ok),
                                post_certified=entry=='main' and bool(ok),
                                expected_certification_refusal=entry=='no-main'))
            (out/'results.json').write_text(json.dumps(records,indent=2))
            print(entry,policy,mode,r.returncode,[q['outcome'] for q in qs],bool(ok),flush=True)
            if not ok: failed.append(stem.name)
    if any(k!=keys[0] for k in keys): failed.append(entry+'-query-identity')
if failed: raise SystemExit(str(failed))
