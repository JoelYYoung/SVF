#!/usr/bin/env python3
"""Semantic regression for recursive TOP effects; D3 integration is separate."""
import argparse, csv, hashlib, json, os, subprocess, tempfile, time
from datetime import datetime, timezone
from pathlib import Path

p=argparse.ArgumentParser(); p.add_argument('--ae',required=True)
p.add_argument('--extapi',required=True); p.add_argument('--library-path',default='')
p.add_argument('--output'); a=p.parse_args()
out=Path(a.output) if a.output else Path(tempfile.mkdtemp(prefix='recursive-mod-check-'))
if a.output: out.mkdir(parents=True,exist_ok=False)
print('raw artifacts:',out,flush=True)
env=dict(os.environ,DYLD_LIBRARY_PATH=a.library_path,LD_LIBRARY_PATH=a.library_path,
         SVF_AE_TRACE_RECURSIVE_TOP='1')
root=Path(__file__).parent
inputs=sorted(root.glob('R15*.ll'))+[root/f'RecursiveModCase{i}.ll' for i in range(1,10)]
configs={'box':('box','dense','whole'),'dense':('octagon','dense','whole'),
         'semi':('octagon','semi-sparse','whole'),'pack':('octagon','dense','syntax-pack'),
         'pack-semi':('octagon','semi-sparse','syntax-pack'),'oh':('octagon','oh-packed','syntax-pack')}
records=[]; failed=[]
for inp in inputs:
    identity=hashlib.sha256(inp.read_bytes()).hexdigest(); keys=[]
    for cfg,(domain,mode,policy) in configs.items():
        stem=out/(inp.stem+'-'+cfg)
        cmd=[a.ae,'-extapi='+a.extapi,'-ae-domain='+domain,'-ae-backend=native',
             '-ae-sparsity='+mode,'-ae-relational-policy='+policy,'-ae-pack-max-vars=10',
             '-ae-relational-calls=through','-handle-recur=top','-model-consts=true',
             '-model-arrays=true','-pre-field-sensitive=false','-stat=false',
             '-overflow=false','-null-deref=false','-ae-query-input-id='+identity,
             '-ae-query-ledger='+str(stem)+'.queries.tsv','-ae-post-check='+str(stem)+'.post.tsv',str(inp)]
        start=datetime.now(timezone.utc).isoformat(); t=time.monotonic()
        try:
            r=subprocess.run(cmd,env=env,text=True,capture_output=True,timeout=60)
            status,stdout,stderr=r.returncode,r.stdout,r.stderr
        except subprocess.TimeoutExpired as e:
            status=124; stdout=e.stdout or b''; stderr=e.stderr or b''
            if isinstance(stdout,bytes): stdout=stdout.decode(errors='replace')
            if isinstance(stderr,bytes): stderr=stderr.decode(errors='replace')
        end=datetime.now(timezone.utc).isoformat(); wall=time.monotonic()-t
        Path(str(stem)+'.stdout').write_text(stdout); Path(str(stem)+'.stderr').write_text(stderr)
        qp=Path(str(stem)+'.queries.tsv'); pp=Path(str(stem)+'.post.tsv')
        qs=list(csv.DictReader(qp.open(),delimiter='\t')) if qp.exists() else []
        ps=list(csv.DictReader(pp.open(),delimiter='\t')) if pp.exists() else []
        want=['Safe'] if inp.stem in ('RecursiveModCase1','RecursiveModCase5') else ['May']
        if inp.stem=='RecursiveModCase9': want=['May','May']
        if inp.stem=='R15j_recursion_no_write_positive': want=['Safe','Safe']
        actual=[q['outcome'] for q in qs]
        ok=status==0 and actual==want and ps and all(
            x['status'] in ('Pass','Infeasible','Unreachable') for x in ps)
        invoked='AE_RECURSIVE_TOP' in stderr
        ok=bool(ok and invoked)
        if inp.stem in ('RecursiveModCase7','RecursiveModCase8'):
            ok=ok and 'all=1' in stderr
        if inp.stem=='RecursiveModCase9':
            ok=ok and 'return_successors=2' in stderr
        if inp.stem=='RecursiveModCase6':
            ok=status==2 and 'AE_RECURSIVE_TOP_FAIL_CLOSED' in stderr and not qs
        keys.append({q['query_id'] for q in qs})
        records.append(dict(input=str(inp),config=cfg,command=cmd,status=status,
                            started_at=start,ended_at=end,wall_seconds=wall,
                            queries=qs,post=ps,expected=want,test_passed=ok,
                            summary_invoked_in_solve_or_replay=invoked))
        (out/'results.json').write_text(json.dumps(records,indent=2))
        print(inp.stem,cfg,status,actual,ok,flush=True)
        if not ok: failed.append(stem.name)
    if any(x!=keys[0] for x in keys): failed.append(inp.stem+'-query-identity')
if failed: raise SystemExit(str(failed))
