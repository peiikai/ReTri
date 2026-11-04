#!/usr/bin/env python3
"""Merge shader variant lists (runtime/nvn/shader_prep.h: G / C records, hashes and pipeline state only) into one.

    tools/shader_list_merge.py OUT IN [IN ...]

Each IN is a list in the shipped format or a player's cache/shader_seen.txt (its "# shader_seen" header and any other
comment lines are dropped). OUT gets every record once, sorted, so a re-run after more tours changes only what's new.
OUT is read too when it exists: records are only ever added.
"""
import os
import sys


def records(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            if line and line[0] in 'GC' and line[1:2] == ' ':
                yield line


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, ins = sys.argv[1], sys.argv[2:]
    have = set(records(out)) if os.path.exists(out) else set()
    before = len(have)
    for p in ins:
        have.update(records(p))
    with open(out, 'w', encoding='utf-8') as f:
        for r in sorted(have):
            f.write(r + '\n')
    g = sum(1 for r in have if r[0] == 'G')
    print(f'{out}: {len(have)} records ({g} pipelines, {len(have) - g} compute), {len(have) - before} new')


if __name__ == '__main__':
    main()
