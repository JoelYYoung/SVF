"""Typed external-memory query roles. Preserve raw before/after schema evidence."""
import argparse, collections, csv, hashlib, json, os, subprocess, tempfile, time
from pathlib import Path
from datetime import datetime, timezone

p=argparse.ArgumentParser()
p.add_argument('--ae', required=True); p.add_argument('--extapi', required=True)
p.add_argument('--library-path', default=''); p.add_argument('--output')
p.add_argument('--reference-before', action='store_true')
a=p.parse_args()
out=Path(a.output) if a.output else Path(tempfile.mkdtemp(prefix='memcpy-query-check-'))
if a.output: out.mkdir(parents=True,exist_ok=False)
print('raw artifacts:',out,flush=True)
env=dict(os.environ,DYLD_LIBRARY_PATH=a.library_path,LD_LIBRARY_PATH=a.library_path)
configs={'box':('box','dense','whole'), 'dense':('octagon','dense','whole'),
         'semi':('octagon','semi-sparse','whole'), 'pack':('octagon','dense','syntax-pack'),
         'pack-semi':('octagon','semi-sparse','syntax-pack'), 'oh':('octagon','oh-packed','syntax-pack')}
rows=[]; failed=[]
for case in ('Safe','Null','Iconv','Malformed'):
    inp=Path(__file__).with_name('MemcpyQuery'+case+'.ll')
    for name,(domain,mode,policy) in configs.items():
        stem=out/(case+'-'+name)
        cmd=[a.ae,'-extapi='+a.extapi,'-ae-domain='+domain,'-ae-sparsity='+mode,
             '-ae-relational-policy='+policy,'-ae-pack-max-vars=10','-stat=false',
             '-null-deref=true','-overflow=false','-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
             '-ae-query-ledger='+str(stem)+'.queries.tsv','-ae-post-check='+str(stem)+'.post.tsv',str(inp)]
        start=datetime.now(timezone.utc).isoformat(); t=time.monotonic()
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=20)
        elapsed=time.monotonic()-t
        Path(str(stem)+'.stdout').write_text(r.stdout); Path(str(stem)+'.stderr').write_text(r.stderr)
        qp=Path(str(stem)+'.queries.tsv'); pp=Path(str(stem)+'.post.tsv')
        qs=list(csv.DictReader(qp.open(),delimiter='\t')) if qp.exists() else []
        ps=list(csv.DictReader(pp.open(),delimiter='\t')) if pp.exists() else []
        ext=[q for q in qs if q['detector']=='null-dereference' and q['query_kind'].startswith('ext-arg-')]
        kinds=collections.Counter(q['query_kind'] for q in ext)
        if case=='Malformed':
            ok=(r.returncode==2 and 'memory role does not match pointer argument type' in r.stderr) if not a.reference_before else r.returncode==0
        else:
            expected={'ext-arg-'+str(i):1 for i in (1,2,3,4)} if case=='Iconv' else (
                {'ext-arg-'+str(i):2 for i in (1,2,3)} if a.reference_before else {'ext-arg-0':2,'ext-arg-1':2})
            ok=r.returncode==0 and dict(kinds)==expected and bool(ps) and not any(s['status'] in ('Fail','Unsupported') for s in ps)
            if case in ('Safe','Null'):
                ok=ok and all(q['outcome']==('May' if (a.reference_before and q['query_kind']!='ext-arg-1') or (not a.reference_before and case=='Null' and q['query_kind']=='ext-arg-0') else 'Safe') for q in ext)
        rows.append(dict(case=case,config=name,command=cmd,started_at=start,ended_at=datetime.now(timezone.utc).isoformat(),
                         status=r.returncode,wall_seconds=elapsed,queries=qs,post=ps,passed=bool(ok)))
        (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(case,name,r.returncode,dict(kinds),bool(ok),flush=True)
        if not ok: failed.append(case+'-'+name)
(out/'manifest.json').write_text(json.dumps({'args':vars(a),'binary_sha256':hashlib.sha256(Path(a.ae).read_bytes()).hexdigest(),
                                          'extapi_sha256':hashlib.sha256(Path(a.extapi).read_bytes()).hexdigest(),
                                          'failed':failed},indent=2)+'\n')
if failed: raise SystemExit(str(failed))
