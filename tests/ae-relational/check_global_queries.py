"""Global schema gates; raw attempts and old/new IDs are never overwritten."""
import argparse, collections, csv, hashlib, json, os, subprocess, tempfile, time
from pathlib import Path
from datetime import datetime, timezone

p = argparse.ArgumentParser()
p.add_argument('--ae', required=True)
p.add_argument('--before')
p.add_argument('--extapi', required=True)
p.add_argument('--library-path', default='')
p.add_argument('--output')
p.add_argument('--r18')
p.add_argument('--only', help='Run only the named input stem; useful for an isolated added gate')
a = p.parse_args()
out = Path(a.output) if a.output else Path(tempfile.mkdtemp(prefix='global-query-'))
if a.output: out.mkdir(parents=True, exist_ok=False)
env = dict(os.environ, DYLD_LIBRARY_PATH=a.library_path, LD_LIBRARY_PATH=a.library_path)
configs = {'box': ('box','dense','whole'), 'dense': ('octagon','dense','whole'),
           'semi': ('octagon','semi-sparse','whole'), 'pack': ('octagon','dense','syntax-pack'),
           'pack-semi': ('octagon','semi-sparse','syntax-pack'), 'oh': ('octagon','oh-packed','syntax-pack')}
inputs = [Path(__file__).with_name('GlobalQuery'+s+'.ll') for s in ('Formation','Access','Edges','Negative')]
if a.r18: inputs.append(Path(a.r18))
if a.only: inputs = [inp for inp in inputs if inp.stem == a.only]
if not inputs or not all(inp.is_file() for inp in inputs):
    p.error('all selected inputs must exist')
rows, failures, identities, diffs = [], [], {}, []
for inp in inputs:
    versions = [('after',a.ae)] + ([('before',a.before)] if a.before else [])
    for version, binary in versions:
        for name,(domain,mode,policy) in configs.items():
            stem = out / (inp.stem+'-'+version+'-'+name)
            cmd = [binary,'-extapi='+a.extapi,'-ae-domain='+domain,'-ae-sparsity='+mode,
                   '-ae-relational-policy='+policy,'-ae-pack-max-vars=10','-stat=false',
                   '-overflow=true','-null-deref=true', '-model-arrays=true','-model-consts=true',
                   '-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
                   '-ae-query-ledger='+str(stem)+'.queries.tsv',
                   '-ae-post-check='+str(stem)+'.post.tsv',str(inp)]
            started = datetime.now(timezone.utc).isoformat(); start = time.monotonic()
            try:
                r = subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=30)
                status, stdout, stderr = r.returncode, r.stdout, r.stderr
            except subprocess.TimeoutExpired as e:
                status = 124
                stdout = (e.stdout or b'').decode() if isinstance(e.stdout,bytes) else (e.stdout or '')
                stderr = (e.stderr or b'').decode() if isinstance(e.stderr,bytes) else (e.stderr or '')
            wall = time.monotonic()-start
            Path(str(stem)+'.stdout').write_text(stdout)
            Path(str(stem)+'.stderr').write_text(stderr)
            qp, pp = Path(str(stem)+'.queries.tsv'), Path(str(stem)+'.post.tsv')
            qs = list(csv.DictReader(qp.open(),delimiter='\t')) if qp.exists() else []
            ps = list(csv.DictReader(pp.open(),delimiter='\t')) if pp.exists() else []
            ok = status==0 and bool(ps) and not any(s['status'] in ('Fail','Unsupported') for s in ps)
            mapping = {q['query_id']:q['outcome'] for q in qs}
            ok = ok and len(mapping)==len(qs)
            if version=='after':
                gs = [q for q in qs if q['function']=='<global>' and q['query_kind']=='gep-allocation-range']
                expected = {'GlobalQueryAccess':1,'GlobalQueryEdges':3}.get(inp.stem,4)
                ok = ok and len(gs)==expected and all(q['outcome']!='Unreachable' for q in gs)
                ok = ok and not any(q['query_kind'] in ('gep-bounds','gep-address') for q in qs)
                expected_outcomes = {1:{'Safe':1},3:{'May':2,'Unsupported':1},4:{'Safe':3,'Unsupported':1}}[expected]
                if inp.stem=='GlobalQueryNegative': expected_outcomes={'Safe':4}
                ok = ok and dict(collections.Counter(q['outcome'] for q in gs))==expected_outcomes
                if inp.stem=='GlobalQueryNegative':
                    # Exact R21 IR from supervisor fixtures b085ce4: i==-1,
                    # then arr[i]; also a null pointer's second struct field.
                    sites = {q.get('semantic_site'):q for q in qs if q['query_kind']=='gep-allocation-range'}
                    negative = sites.get('fn=main:%5:inst=1:gep-field=0', {})
                    ok = ok and negative.get('outcome')=='May'
                    access = [q for q in qs if q['query_kind']=='load-bounds' and q.get('semantic_site')=='fn=main:%5:inst=2']
                    ok = ok and len(access)==1 and access[0]['outcome']=='May'
                    ok = ok and any(q['query_kind']=='load-address' and q['outcome']=='May' for q in qs)
                if expected==1:
                    ok = ok and any(q['query_kind']=='load-bounds' and q['outcome']=='May' for q in qs)
                if expected==3:
                    ok = ok and any(q['query_kind']=='store-bounds' and q['outcome']=='May' for q in qs)
                    ok = ok and any(q['query_kind']=='load-bounds' and q['outcome']=='Unsupported' for q in qs)
                    ok = ok and any(q['query_kind']=='load-bounds' and q['outcome']=='Safe' for q in qs)
                baseline = identities.setdefault((inp.name,version),set(mapping))
                ok = ok and set(mapping)==baseline
            rows.append(dict(input=str(inp),version=version,config=name,command=cmd,
                             started_at=started,ended_at=datetime.now(timezone.utc).isoformat(),
                             wall_seconds=wall,status=status,queries=qs,post=ps,passed=ok))
            (out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
            print(inp.name,version,name,status,ok,flush=True)
            if not ok: failures.append(str(stem))
for row in rows:
    if row['version']!='after': continue
    before = next((b for b in rows if b['version']=='before' and b['input']==row['input'] and b['config']==row['config']),None)
    if not before: continue
    old = {q['query_id']:q for q in before['queries']}; new = {q['query_id']:q for q in row['queries']}
    diffs.append(dict(input=row['input'],config=row['config'],removed=[old[k] for k in sorted(old.keys()-new.keys())],
                      added=[new[k] for k in sorted(new.keys()-old.keys())],
                      changed=[dict(before=old[k],after=new[k]) for k in sorted(old.keys()&new.keys()) if old[k]['outcome']!=new[k]['outcome']]))
(out/'schema-diff.json').write_text(json.dumps(diffs,indent=2)+'\n')
(out/'manifest.json').write_text(json.dumps(dict(args=vars(a),failures=failures,
    binary_sha256=hashlib.sha256(Path(a.ae).read_bytes()).hexdigest(),
    before_sha256=hashlib.sha256(Path(a.before).read_bytes()).hexdigest() if a.before else None,
    extapi_sha256=hashlib.sha256(Path(a.extapi).read_bytes()).hexdigest()),indent=2)+'\n')
if failures: raise SystemExit(str(failures))
