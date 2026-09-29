"""Read-only aggregation of explicitly listed global-query batches."""
import argparse, collections, hashlib, json
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--before',required=True)
p.add_argument('--after',required=True)
p.add_argument('--output',required=True)
a=p.parse_args()
old=json.loads(Path(a.before).read_text()); new=json.loads(Path(a.after).read_text())
rows=[]
for current in new:
    prior=next(x for x in old if x['input']==current['input'] and x['config']==current['config'])
    left={q['query_id']:q for q in prior['queries']}; right={q['query_id']:q for q in current['queries']}
    assert len(left)==len(prior['queries']) and len(right)==len(current['queries']), 'duplicate IDs'
    shared=sorted(left.keys()&right.keys())
    changed=[dict(before=left[k],after=right[k]) for k in shared if left[k]['outcome']!=right[k]['outcome']]
    added=[right[k] for k in sorted(right.keys()-left.keys())]
    removed=[left[k] for k in sorted(left.keys()-right.keys())]
    groups=collections.defaultdict(collections.Counter)
    for q in current['queries']:
        groups[(q['function']=='<global>',q['query_kind'])][q['outcome']]+=1
    ps=collections.Counter(x['status'] for x in current['post'])
    rows.append(dict(input=current['input'],config=current['config'],status=current['status'],
        post=dict(ps),shared=len(shared),changed=changed,removed=removed,added=added,
        outcomes=[dict(global_node=g,kind=k,outcomes=dict(v)) for (g,k),v in sorted(groups.items())]))
    print(Path(current['input']).stem,'exit',current['status'],'shared',len(shared),
          'changed',len(changed),'removed',len(removed),'added',len(added),'post',dict(ps))
    print('global',[(r['kind'],r['outcomes']) for r in rows[-1]['outcomes'] if r['global_node']])
payload=dict(before=a.before,after=a.after,
             before_sha256=hashlib.sha256(Path(a.before).read_bytes()).hexdigest(),
             after_sha256=hashlib.sha256(Path(a.after).read_bytes()).hexdigest(),rows=rows)
with Path(a.output).open('x') as f: json.dump(payload,f,indent=2); f.write('\n')
