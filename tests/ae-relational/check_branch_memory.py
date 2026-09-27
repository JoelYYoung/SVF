#!/usr/bin/env python3
"""Branch-memory semantic checks; raw failed runs are preserved in a fresh directory."""
import argparse, csv, hashlib, json, os, subprocess, tempfile, time
from pathlib import Path
from datetime import datetime, timezone

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--ae',required=True); ap.add_argument('--extapi',required=True)
    ap.add_argument('--library-path',default='')
    ap.add_argument('--output')
    args=ap.parse_args()
    out=Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='branch-memory-check-'))
    if args.output: out.mkdir(parents=True,exist_ok=False)
    print('raw artifacts:',out,flush=True)
    env=dict(os.environ)
    env['DYLD_LIBRARY_PATH']=args.library_path; env['LD_LIBRARY_PATH']=args.library_path
    cases={'R09_load_store_branch_refinement':['Safe','May'],
           'R10_stale_branch_refinement_false_safe':['May',None],
           **{f'BranchMemoryCase{i}':['May'] for i in [1,2,4,5]},
           'BranchMemoryCase3':[None,None],
           **{f'R12_branch_guards_case{i}':['May'] for i in [1,2,3,4,7,9]},
           'R12_branch_guards_case5':['May','May'],
           'R12_branch_guards_case6':['Safe'],
           'R12_branch_guards_case8':['May']}
    configs={'box':('box','dense','whole'),'dense':('octagon','dense','whole'),
             'semi':('octagon','semi-sparse','whole'),'pack':('octagon','dense','syntax-pack'),
             'oh':('octagon','oh-packed','syntax-pack')}
    records=[]; failures=[]
    for name,expected in cases.items():
        inp=Path(__file__).parent/(name+'.ll'); ident=hashlib.sha256(inp.read_bytes()).hexdigest()
        for cfg,(domain,mode,policy) in configs.items():
            stem=out/(name+'-'+cfg)
            cmd=[args.ae,'-extapi='+args.extapi,'-ae-domain='+domain,'-ae-backend=native',
                 '-ae-sparsity='+mode,'-ae-relational-policy='+policy,'-ae-pack-max-vars=10',
                 '-ae-relational-calls=through','-handle-recur=top','-model-consts=true',
                 '-model-arrays=true','-pre-field-sensitive=false','-stat=false',
                 '-overflow=false','-null-deref=false','-ae-query-input-id='+ident,
                 '-ae-query-ledger='+str(stem)+'.queries.tsv',
                 '-ae-post-check='+str(stem)+'.post.tsv',str(inp)]
            start=datetime.now(timezone.utc).isoformat(); t=time.monotonic()
            result=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=60)
            end=datetime.now(timezone.utc).isoformat(); wall=time.monotonic()-t
            Path(str(stem)+'.stdout').write_text(result.stdout)
            Path(str(stem)+'.stderr').write_text(result.stderr)
            qp=Path(str(stem)+'.queries.tsv'); pp=Path(str(stem)+'.post.tsv')
            qs=list(csv.DictReader(qp.open(),delimiter='\t')) if qp.exists() else []
            ps=list(csv.DictReader(pp.open(),delimiter='\t')) if pp.exists() else []
            actual=[q['outcome'] for q in qs]
            want=list(expected)
            # Box ignores scalar input guards; limited packs can lose the
            # fresh relation. These ideal Safe queries are precision probes,
            # not grounds for accepting a false Safe on the stale queries.
            if name=='BranchMemoryCase3' and cfg in ('dense','semi'): want=['Safe','Safe']
            if name=='R12_branch_guards_case8' and cfg in ('dense','semi'): want=['Safe']
            ok=result.returncode==0 and len(actual)==len(want) and all(
                e is None or a==e for a,e in zip(actual,want)) and ps and all(
                p['status'] in ('Pass','Infeasible','Unreachable') for p in ps)
            rec=dict(case=name,config=cfg,command=cmd,status=result.returncode,
                     started_at=start,ended_at=end,wall_seconds=wall,queries=qs,post=ps,
                     expected=want,accepted=bool(ok),
                     role=('false-safe-red-regression' if name in
                           [f'R12_branch_guards_case{i}' for i in [1,2,3,7]] else
                           'known-precision-cost' if name in
                           [f'R12_branch_guards_case{i}' for i in [4,5,8,9]] else
                           'semantic-regression'))
            records.append(rec); (out/'results.json').write_text(json.dumps(records,indent=2))
            print(name,cfg,result.returncode,actual,'PASS' if ok else 'FAIL',flush=True)
            if not ok: failures.append((name,cfg))
    if failures: raise SystemExit('failed gates: '+str(failures))

if __name__=='__main__': main()
