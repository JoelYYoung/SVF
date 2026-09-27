#!/usr/bin/env python3
"""Integration, missing-definition mutation, and mutable-borrow cache regression."""
import argparse, csv, hashlib, json, os, subprocess, tempfile
from pathlib import Path

def main():
    p=argparse.ArgumentParser(); p.add_argument('--ae',required=True)
    p.add_argument('--extapi',required=True); p.add_argument('--library-path',default='')
    a=p.parse_args(); out=Path(tempfile.mkdtemp(prefix='oh-production-check-'))
    env=os.environ.copy()
    if a.library_path:
        env['DYLD_LIBRARY_PATH']=a.library_path
        env['LD_LIBRARY_PATH']=a.library_path
    records=[]
    for name in ['RelationalLoopWitness','RelationalCallWitness','SharedCalleeWitness']:
        inp=Path(__file__).parent/(name+'.ll'); query_sets={}
        for mode in ['dense','oh-packed','cache','mutation']:
            if mode=='mutation' and name=='SharedCalleeWitness': continue
            run_env=env.copy()
            if mode=='cache': run_env['SVF_AE_OH_CACHE_CHECK']='1'
            if mode=='mutation': run_env['SVF_AE_OH_DROP_DEFINITION']='1'
            ledger=out/(name+'-'+mode+'.queries.tsv'); post=out/(name+'-'+mode+'.post.tsv')
            cmd=[a.ae,'-extapi='+a.extapi,'-ae-domain=octagon','-ae-backend=native',
                '-ae-relational-policy=syntax-pack','-ae-pack-max-vars=10','-ae-relational-calls=through',
                '-ae-sparsity='+('dense' if mode=='dense' else 'oh-packed'),'-handle-recur=top',
                '-model-consts=true','-model-arrays=true','-pre-field-sensitive=false','-stat=false',
                '-overflow=true','-null-deref=true','-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
                '-ae-query-ledger='+str(ledger),'-ae-post-check='+str(post),str(inp)]
            result=subprocess.run(cmd,env=run_env,text=True,capture_output=True,timeout=60)
            stem=out/(name+'-'+mode)
            stem.with_suffix('.stdout').write_text(result.stdout)
            stem.with_suffix('.stderr').write_text(result.stderr)
            records.append({'command':cmd,'mode':mode,'status':result.returncode})
            (out/'commands.json').write_text(json.dumps(records,indent=2))
            if mode=='mutation':
                assert result.returncode!=0 and 'Oh undeclared pack write' in result.stderr, (out,name,mode)
                continue
            assert result.returncode==0,(out,name,mode,result.stderr)
            if mode=='cache': assert 'AE_OH_CACHE_CHECK Pass' in result.stdout,(out,name)
            queries=list(csv.DictReader(ledger.open(),delimiter='\t'))
            posts=list(csv.DictReader(post.open(),delimiter='\t'))
            assert posts and all(r['status'] in ['Pass','Infeasible','Unreachable'] for r in posts),(out,name,mode)
            query_sets[mode]={r['query_id']:r['outcome'] for r in queries}
        assert query_sets['dense']==query_sets['oh-packed']==query_sets['cache'],(out,name,query_sets)
    print('Oh production regressions passed; raw artifacts:',out)

if __name__=='__main__': main()
