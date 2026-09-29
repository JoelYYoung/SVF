"""R23 red/green contract. Preserve each invocation's raw query/Post/final."""
import argparse,csv,hashlib,json,os,re,subprocess,tempfile,time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--ae',required=True);p.add_argument('--extapi',required=True)
p.add_argument('--library-path',default='');p.add_argument('--output');p.add_argument('--expect-old',action='store_true')
a=p.parse_args();out=Path(a.output) if a.output else Path(tempfile.mkdtemp(prefix='switch-refinement-'))
if a.output:out.mkdir(exist_ok=False,parents=True)
configs={'box':('box','dense','whole'),'dense':('octagon','dense','whole'),
 'semi':('octagon','semi-sparse','whole'),'pack':('octagon','dense','syntax-pack'),
 'pack-semi':('octagon','semi-sparse','syntax-pack'),'oh':('octagon','oh-packed','syntax-pack'),
 'd3':('octagon','d3','whole'),'pack-d3':('octagon','d3','syntax-pack')}
env=dict(os.environ,DYLD_LIBRARY_PATH=a.library_path,LD_LIBRARY_PATH=a.library_path,SVF_AE_QUERY_RECHECK='auto',SVF_AE_TRACE_SWITCH_CASES='1')
results=[];failures=[]
def rows(path):
 return list(csv.DictReader(path.open(),delimiter='\t')) if path.exists() else []
for case in ['R23','S1','Wide','DefaultShared','Scalar','Narrow']:
 inp=Path(__file__).with_name('SwitchRefinement'+case+'.ll')
 for label,(domain,mode,policy) in configs.items():
  stem=out/(case+'-'+label);start=time.monotonic()
  cmd=[a.ae,'-extapi='+a.extapi,'-ae-domain='+domain,'-ae-sparsity='+mode,
       '-ae-relational-policy='+policy,'-ae-pack-max-vars=10','-handle-recur=top',
       '-model-consts=true','-model-arrays=true','-overflow=true','-null-deref=true',
       '-ae-query-input-id='+hashlib.sha256(inp.read_bytes()).hexdigest(),
       '-ae-query-ledger='+str(stem)+'.q.tsv','-ae-post-check='+str(stem)+'.p.tsv',str(inp)]
  try:
   r=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=10);status=r.returncode;stdout=r.stdout;stderr=r.stderr
  except subprocess.TimeoutExpired as e:
   status=124;stdout=str(e.stdout or '');stderr=str(e.stderr or '')
  Path(str(stem)+'.stdout').write_text(stdout);Path(str(stem)+'.stderr').write_text(stderr)
  q=rows(Path(str(stem)+'.q.tsv'));post=rows(Path(str(stem)+'.p.tsv'));final=rows(Path(str(stem)+'.q.tsv.recheck.tsv'))
  assertions=[x for x in q if x['detector']=='assertion']
  ok=status==0 and bool(post) and all(x['status'] in ('Pass','Infeasible','Unreachable') for x in post)
  key=lambda xs:{x['query_id']:(x['outcome'],x['reason']) for x in xs}
  ok=ok and key(q)==key(final)
  if case in ('R23','Scalar'):
   byline={int(re.search(r'"ln": (\d+)',x['source_location'])[1]):x['outcome'] for x in assertions}
   expected={9:'Safe',10:'May' if a.expect_old else 'Safe',11:'Safe',12:'Safe' if a.expect_old else 'May'}
   # The frozen packed D3 implementation already loses these memory facts.
   # Its four May results are a retained precision limit, not false Safe.
   if case=='R23' and label=='pack-d3':expected={9:'May',10:'May',11:'May',12:'May'}
   if case=='Scalar':expected={8:'Safe',9:'Safe',10:'Safe',11:'May'}
   ok=ok and byline==expected
  elif case=='S1' and not a.expect_old:
   labels=re.findall(r'AE_SWITCH_CASES width=(\d+) default=([01]) values=([^\n]*)',stderr)
   ok=ok and sorted(labels)==sorted([('32','0','-1,'),('32','0','1,2,'),('32','0','3,'),('32','1','')])
  elif case=='Narrow':
   # Both false assertions are concretely reachable. A dropped case must fail.
   ok=ok and len(assertions)==2 and all(x['outcome']=='May' for x in assertions)
   ok=ok and not any(x['status']=='Infeasible' for x in post)
  elif case in ('Wide','DefaultShared') and not a.expect_old:
   ok=ok and len(assertions)==(2 if case=='Wide' else 1) and all(x['outcome']=='May' for x in assertions)
  results.append(dict(case=case,config=label,command=cmd,status=status,wall_seconds=time.monotonic()-start,passed=ok,queries=q,post=post))
  (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
  print(case,label,status,ok,flush=True)
  if not ok:failures.append(case+'/'+label)
(out/'manifest.json').write_text(json.dumps(dict(binary_sha256=hashlib.sha256(Path(a.ae).read_bytes()).hexdigest(),extapi_sha256=hashlib.sha256(Path(a.extapi).read_bytes()).hexdigest(),failures=failures),indent=2)+'\n')
if failures:raise SystemExit(str(failures))
