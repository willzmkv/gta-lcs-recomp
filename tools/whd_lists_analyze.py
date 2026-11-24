#!/usr/bin/env python3
"""Analyses the HD World cell-list dumps written with LCS_DIAG_WHD_DUMP=1
(scratch/whd_lists_N.csv): how instance ids split into detailed and resident low-detail (LQ)
geometry, per render bucket, and how many LQ instances the replacement rule in
src/game/lcs_world_hd.cpp (rebuild_combined_chunk) drops.

Usage: python tools/whd_lists_analyze.py scratch/whd_lists_2.csv
"""
import collections
import csv
import math
import sys

LQ_ID_MIN = 4700  # kLqInstanceIdMin


def main(path):
    rows = list(csv.DictReader(open(path)))
    lists = collections.defaultdict(set)
    info = {}
    detail = collections.defaultdict(list)
    for r in rows:
        nat = int(r['native'])
        raw = int(r['raw_id'])
        if nat and raw & 0x8000:
            continue  # hidden by the game in its own list
        key = (raw & 0x7fff, int(r['gid']))
        src = (int(r['src_cx']), int(r['src_cy']), nat)
        lists[src].add(key)
        info.setdefault(key, (float(r['wx']), float(r['wy']), float(r['r']), int(r['bucket'])))
        if key[0] < LQ_ID_MIN:
            detail[src].append((float(r['wx']), float(r['wy'])))
    hd = [s for s in lists if s[2] == 0]
    count = collections.Counter(k for s in hd for k in lists[s])
    print(f'{len(lists)} lists, {len(info)} unique instances')
    for b in range(7):
        ks = [k for k in count if info[k][3] == b]
        if not ks:
            continue
        rs = sorted(info[k][2] for k in ks)
        ids = sorted(k[0] for k in ks)
        cs = sorted(count[k] for k in ks)
        q = lambda a, p: a[int(p * (len(a) - 1))]
        print(f'bucket {b}: n={len(ks)} radius p10/50/90={q(rs,.1):.0f}/{q(rs,.5):.0f}/{q(rs,.9):.0f} '
              f'id p10/50/90={q(ids,.1)}/{q(ids,.5)}/{q(ids,.9)} in lists p10/50/90={q(cs,.1)}/{q(cs,.5)}/{q(cs,.9)}')
    union_lq = {k for ks in lists.values() for k in ks if k[0] >= LQ_ID_MIN and info[k][3] != 0}
    dropped = set()
    for k in union_lq:
        x, y, r, _ = info[k]
        for s, ks in lists.items():
            if k not in ks and any(math.hypot(dx - x, dy - y) <= r for dx, dy in detail[s]):
                dropped.add(k)
                break
    print(f'LQ instances (buckets 1-6) in the union: {len(union_lq)}, replaced by a drawn list: {len(dropped)}')


if __name__ == '__main__':
    main(sys.argv[1])
