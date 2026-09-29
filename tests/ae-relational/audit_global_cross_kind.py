"""Structural old/new GEP correspondence; changed properties are not precision gains."""
import argparse
import collections
import hashlib
import json
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--before', required=True)
p.add_argument('--after', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
before = json.loads(Path(a.before).read_text())
after = json.loads(Path(a.after).read_text())
rows = []
def key(q):
    return tuple(q[k] for k in ('input_id','function','icfg_node','operand','source_location'))
for new in after:
    old = next(r for r in before if r['input']==new['input'] and r['config']==new['config'])
    grouped = collections.defaultdict(list)
    for q in new['queries']:
        if q['query_kind']=='gep-allocation-range': grouped[key(q)].append(q)
    pairs, missing, used = [], [], set()
    for q in old['queries']:
        if q['query_kind']!='gep-bounds': continue
        candidates = grouped.get(key(q), [])
        if not candidates: missing.append(q); continue
        used.update(c['query_id'] for c in candidates)
        pairs.append(dict(before=q, after=candidates,
                          match='one-to-one' if len(candidates)==1 else 'old-key-collision-one-to-many',
                          old_safe_new_may=q['outcome']=='Safe' and any(c['outcome']=='May' for c in candidates)))
    additions = collections.defaultdict(collections.Counter)
    for q in new['queries']:
        if q['query_kind'] in ('store-address','store-bounds','load-bounds'):
            additions[q['query_kind']][q['outcome']]+=1
    row = dict(input=new['input'], config=new['config'], status=new['status'],
               correspondence=pairs, unmatched_old=missing,
               unmatched_new=[q for q in new['queries'] if q['query_kind']=='gep-allocation-range' and q['query_id'] not in used],
               old_safe_new_may=[pair for pair in pairs if pair['old_safe_new_may']],
               added_access_queries={k:dict(v) for k,v in additions.items()},
               removed_gep_address=sum(q['query_kind']=='gep-address' for q in old['queries']),
               global_before= dict(collections.Counter(q['outcome'] for q in old['queries'] if q['function']=='<global>' and q['query_kind']=='gep-bounds')),
               global_after=dict(collections.Counter(q['outcome'] for q in new['queries'] if q['function']=='<global>' and q['query_kind']=='gep-allocation-range')))
    rows.append(row)
    print(Path(row['input']).stem, row['config'], 'pairs',len(pairs),
          'old-Safe/new-May',len(row['old_safe_new_may']), 'missing',len(missing),
          'unmatched-new',len(row['unmatched_new']), 'access',row['added_access_queries'])
payload = dict(method='Fixed input; exact function/node/operand/source correspondence. Node IDs alone are not portable semantic identities.',
               limitation='Kinds have different properties: old exclusive bounds vs new inclusive formation range. Multiple new sites sharing an old key remain explicit. Changes are not certified precision gains or standalone causal attribution.',
               before_sha256=hashlib.sha256(Path(a.before).read_bytes()).hexdigest(),
               after_sha256=hashlib.sha256(Path(a.after).read_bytes()).hexdigest(), rows=rows)
with Path(a.output).open('x') as f:
    json.dump(payload, f, indent=2)
    f.write('\n')
