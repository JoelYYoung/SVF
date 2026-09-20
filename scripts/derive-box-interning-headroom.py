#!/usr/bin/env python3
"""Bound retained shallow page duplication; never predict net/peak RSS."""
import argparse
import hashlib
import json
from pathlib import Path


def derive(path):
    source = json.loads(path.read_text())
    rows = []
    for case in source['rows']:
        run, = (x for x in case['record']['runs'] if x['layout'] == 'inline')
        assert run['completed'] and run['exit_code'] == 0
        carriers = {}
        for role, data in run['carriers'].items():
            refs, physical, classes = (data[key] for key in
                                      ('logical_page_refs', 'unique_pages', 'content_classes'))
            assert classes <= physical <= refs
            assert refs - physical == data['cow_saved_refs']
            assert physical - classes == data['duplicate_physical_pages']
            if physical:
                size, remainder = divmod(data['unique_page_shallow_bytes'], physical)
                assert remainder == 0, 'fixed inline page object size required'
            else:
                size = 0
            carriers[role] = dict(logical_refs=refs, physical_pages=physical,
                                  content_classes=classes, extra_duplicate_pages=physical - classes,
                                  diagnostic_page_bytes=size,
                                  removable_shallow_bytes=(physical - classes) * size)
        byte_bound = sum(c['removable_shallow_bytes'] for c in carriers.values())
        rows.append(dict(program=case['program'], mode=case['mode'],
                         source_pair_status=case['status'],
                         accepted=case['status'] == 'passed',
                         source_log_sha256=run['log_sha256'], carriers=carriers,
                         removable_shallow_bytes=byte_bound,
                         diagnostic_peak_rss_kib=int(run['max_rss_kib']),
                         scale_percent=100 * byte_bound / (int(run['max_rss_kib']) * 1024)))
    return dict(source=str(path), source_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                generator=str(Path(__file__).resolve()),
                generator_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                scope='final retained snapshot only; sum of separate scalar/memory pools; fixed inline diagnostic objects',
                exclusions='failed source pairs remain visible but are not acceptance evidence',
                boundary='shallow object opportunity before pool/weak controls/directory detach; excludes GMP limbs, allocation metadata and transient peaks. Scale percent divides retained bytes by diagnostic peak RSS only for magnitude, not a predicted RSS reduction or bound on total deduplication gains. Does not bound CPU or temporal reuse.',
                rows=rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('census', type=Path)
    args = parser.parse_args()
    print(json.dumps(derive(args.census), indent=2))
