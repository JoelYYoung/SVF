"""Read-only validation of full Post identities. Does not run an analyzer."""
import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def read_tsv(path):
    with Path(path).open() as source:
        return list(csv.DictReader(source, delimiter='\t'))


def decode_key(key):
    assert key.startswith('post-v2:'), 'wrong Post schema'
    rest = key[len('post-v2:'):]
    fields = []
    while rest:
        size, rest = rest.split(':', 1)
        # C++ std::string lengths count UTF-8 bytes, not Python characters.
        raw = rest.encode('utf-8')
        count = int(size)
        assert count <= len(raw), 'truncated identity'
        fields.append(raw[:count].decode('utf-8'))
        rest = raw[count:].decode('utf-8')
    assert len(fields) == 5, 'wrong identity arity'
    return fields


def validate(records, case, expected, input_sha, extapi_sha):
    assert records, 'missing Post report'
    semantic = {}
    old_ids = set()
    for row in records:
        assert row['post_schema'] == 'post-v2'
        fields = decode_key(row['semantic_equation_id'])
        assert fields == [row['input_id'], row['equation_kind'], row['semantic_source'],
                          row['semantic_target'], row['semantic_discriminator']]
        assert row['input_id'] == input_sha, 'input identity drift'
        assert row['equation_id'] not in old_ids, 'old ID not injective'
        assert row['semantic_equation_id'] not in semantic, 'semantic collision'
        for site in (row['semantic_source'], row['semantic_target']):
            assert 'module-sha256=' in site, 'missing module origin'
        old_ids.add(row['equation_id'])
        semantic[row['semantic_equation_id']] = (row['status'], row['reason'])
    if case == 'G_multi':
        assert len(records) == 1
        assert records[0]['status'] == expected['only_status']
        assert records[0]['semantic_discriminator'] == expected['only_equation']
        return semantic
    assert all(r['status'] in ('Pass', 'Unreachable', 'Infeasible') for r in records)
    counts = Counter(r['equation_kind'] for r in records)
    for kind in ('call', 'return', 'recursive-summary', 'initial', 'entry'):
        key = kind.replace('-', '_') + '_count'
        if key in expected:
            assert counts[kind] == expected[key], (case, kind, counts)
    discriminators = [r['semantic_discriminator'] for r in records]
    if 'switch_labels' in expected:
        switches = [d for d in discriminators if ';label=' in d and 'switch:width=' in d]
        assert len(switches) == len(expected['switch_labels'])
        for label in expected['switch_labels']:
            assert sum(d.endswith(label) for d in switches) == 1, label
    if 'branch_labels' in expected:
        branches = [d for d in discriminators if ';label=' in d and 'br:polarity=' in d]
        assert len(branches) == len(expected['branch_labels'])
        for label in expected['branch_labels']:
            assert sum(d.endswith(label) for d in branches) == 1, label
        assert all(';condition=' in d and ':inst=' in d for d in branches)
    if case == 'S3':
        summaries = [r for r in records if r['equation_kind'] == 'recursive-summary']
        assert len({r['semantic_source'] for r in summaries}) == 2
        assert all(r['semantic_source'].endswith(':call') and
                   r['semantic_target'].endswith(':ret') for r in summaries)
        assert any(r['status'] == 'Unreachable' and 'recursive body' in r['reason'] for r in records)
    if case == 'S8':
        sites = {r[k] for r in records for k in ('semantic_source', 'semantic_target')}
        for fn, origin in [('strchr', input_sha), ('strtok', extapi_sha)]:
            selected = [s for s in sites if ';fn='+fn+':' in s]
            assert selected and all(s.startswith('module-sha256='+origin+';') for s in selected), fn
    return semantic


def self_test():
    fields = ['input', 'intra', 'module-sha256=x;fn=f:%0:inst=1',
              'module-sha256=x;fn=f:%0:inst=2', 'br:polarity=true']
    encode = lambda xs: 'post-v2:' + ''.join(str(len(x.encode('utf-8')))+':'+x for x in xs)
    assert decode_key(encode(fields)) == fields
    assert decode_key(encode(['中文', *fields[1:]]))[0] == '中文'
    row = dict(post_schema='post-v2', input_id='input', equation_kind='intra',
               equation_id='old:1:2', semantic_source=fields[2], semantic_target=fields[3],
               semantic_discriminator=fields[4], semantic_equation_id=encode(fields),
               status='Pass', reason='covered')
    validate([row], 'unit', {}, 'input', '')
    for bad in ([row, row], [dict(row, semantic_equation_id='')],
                [dict(row, semantic_source='missing')]):
        try:
            validate(bad, 'unit', {}, 'input', '')
        except (AssertionError, ValueError):
            continue
        raise AssertionError('checker accepted malformed metadata')
    print('Parser self-test passed; no analyzer or C++ validation performed.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--post')
    parser.add_argument('--case')
    parser.add_argument('--input-sha')
    parser.add_argument('--extapi-sha')
    parser.add_argument('--compare', help='A second complete Post report from the same semantic configuration')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    else:
        expectations = json.loads(Path(__file__).with_name('post-identity').joinpath('expected.json').read_text())
        expected = expectations['cases'][args.case]
        first = validate(read_tsv(args.post), args.case, expected, args.input_sha, args.extapi_sha)
        if args.compare:
            second = validate(read_tsv(args.compare), args.case, expected, args.input_sha, args.extapi_sha)
            assert first == second, 'full stable-equation comparison failed'
        print(json.dumps({'case': args.case, 'validated_equations': len(first), 'compared': bool(args.compare)}))
