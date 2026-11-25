"""Map tools/hot_functions.txt (made on the game's 1.0.0) onto another game version's build of the main program.

Usage (tools/venv/bin/python):
  hot_functions_map.py GAME_1_0_0 GAME_OTHER [--to v1_0_1]
  hot_functions_map.py --eow GAME_1_0_2 GAME_OTHER --from v1_0_2 --to v1_0_0   (Echoes of Wisdom:
      tools/hot_functions_eow_<from>.txt onto tools/hot_functions_eow_<to>.txt, both builds checked against games.h;
      --gaps for a build with many changes: changed functions by place and a different sdk too, as for 1.1.0)

GAME_1_0_0 / GAME_OTHER: folders with the player's own extracted game files (exefs/main, exefs/sdk). Writes
tools/hot_functions_<to>.txt: the same functions at the other build's addresses, mapped as tools/version_map.py maps the
address tables (a function identical in both builds moves with it; one in a changed function is left out, so it builds
with optnone there). Nothing from the game files is written: only addresses. The Android setup picks the list by the
game version (runtime/gamebuild.c).
"""
import argparse
import bisect
import difflib
import os
from pathlib import Path
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from version_map import MAIN_BASE, Mapper, functions  # noqa: E402


def match(sa, sb):
    """Pairs (i, j) of equal items of sa and sb in order, as difflib's matching blocks would give, in about linear
    time: functions unique in both builds anchor (patience diff), and difflib runs only between anchors. difflib alone
    over EoW's whole main program didn't finish in half an hour."""
    out = []

    def rec(a0, a1, b0, b1, depth):
        if a0 >= a1 or b0 >= b1:
            return
        ca, cb = {}, {}
        for i in range(a0, a1):
            ca[sa[i]] = ca.get(sa[i], 0) + 1
        for j in range(b0, b1):
            cb[sb[j]] = cb.get(sb[j], 0) + 1
        pos_b = {sb[j]: j for j in range(b0, b1) if cb[sb[j]] == 1}
        pairs = [(i, pos_b[sa[i]]) for i in range(a0, a1) if ca[sa[i]] == 1 and sa[i] in pos_b]
        # the longest run of pairs increasing in both (patience sorting)
        tails, tail_idx, prev = [], [], [None] * len(pairs)
        for k, (_, j) in enumerate(pairs):
            t = bisect.bisect_left(tails, j)
            if t == len(tails):
                tails.append(j), tail_idx.append(k)
            else:
                tails[t], tail_idx[t] = j, k
            prev[k] = tail_idx[t - 1] if t else None
        lis, k = [], tail_idx[-1] if tail_idx else None
        while k is not None:
            lis.append(pairs[k])
            k = prev[k]
        lis.reverse()
        if not lis or depth > 8:  # (no anchors: difflib on the gap, when small enough)
            if (a1 - a0) * (b1 - b0) <= 4_000_000:
                sm = difflib.SequenceMatcher(None, sa[a0:a1], sb[b0:b1], autojunk=False)
                for i, j, n in sm.get_matching_blocks():
                    out.extend((a0 + i + d, b0 + j + d) for d in range(n))
            return
        ia, ib = a0, b0
        for i, j in lis:
            rec(ia, i, ib, j, depth + 1)
            out.append((i, j))
            ia, ib = i + 1, j + 1
        rec(ia, a1, ib, b1, depth + 1)

    rec(0, len(sa), 0, len(sb), 0)
    return out


def gap_pairs(starts_a, starts_b, same):
    """Changed functions paired by place: between two identical functions (or a program end), when both builds have as
    many functions there, the k-th of one build with the k-th of the other. (For the hot lists only: a wrong pair just
    compiles a function at another optimisation level.)"""
    out, inv = {}, {b: a for a, b in same.items()}
    ia = [i for i, f in enumerate(starts_a) if f in same] + [len(starts_a)]
    jb = {f: j for j, f in enumerate(starts_b)}
    prev_i, prev_j = -1, -1
    for i in ia:
        j = jb[same[starts_a[i]]] if i < len(starts_a) else len(starts_b)
        if j > prev_j and i - prev_i == j - prev_j:
            for k in range(1, i - prev_i):
                out[starts_a[prev_i + k]] = starts_b[prev_j + k]
        if j > prev_j:
            prev_i, prev_j = i, j
    assert all(b not in inv for b in out.values())
    return out


class FastMapper(Mapper):
    """version_map.Mapper with the functions matched by match() instead of difflib over the whole program."""

    def __init__(self, old_dir, new_dir, gaps=False):
        from nso import Nso  # noqa: E402
        self.a = Nso(str(Path(old_dir) / 'exefs/main'))
        self.b = Nso(str(Path(new_dir) / 'exefs/main'))
        sdk = [Path(d) / 'exefs/sdk' for d in (old_dir, new_dir)]
        self.sdk_same = all(p.exists() for p in sdk) and sdk[0].read_bytes() == sdk[1].read_bytes()
        self.starts_a, sa = functions(self.a)
        self.starts_b, sb = functions(self.b)
        self.same = {self.starts_a[i]: self.starts_b[j] for i, j in match(sa, sb)}
        self.sdk_map = None
        if gaps:  # (--gaps) changed functions by place too, and the sdk's functions when it differs
            self.same.update(gap_pairs(self.starts_a, self.starts_b, self.same))
            if not self.sdk_same:
                ka, kb = Nso(str(sdk[0])), Nso(str(sdk[1]))
                (ta, ha), (tb, hb) = functions(ka), functions(kb)
                m = {ta[i]: tb[j] for i, j in match(ha, hb)}
                m.update(gap_pairs(ta, tb, m))
                self.sdk_map = m
        self.changed = [f for f in self.starts_a if f not in self.same]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('old_game', type=Path)
    ap.add_argument('new_game', type=Path)
    ap.add_argument('--from', dest='src', default='v1_0_0')
    ap.add_argument('--to', default='v1_0_1')
    ap.add_argument('--eow', action='store_true', help="Echoes of Wisdom's lists (hot_functions_eow_<version>.txt)")
    ap.add_argument('--gaps', action='store_true', help='also map changed functions by their place between identical '
                    'ones, and the sdk when it differs (a build with many changes, such as EoW 1.1.0)')
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    if args.eow:
        from game_addresses import game_build  # noqa: E402
        from nso import Nso  # noqa: E402
        for key, folder in ((args.src, args.old_game), (args.to, args.new_game)):
            b = game_build(Nso(str(folder / 'exefs/main')).build_id)
            if not b or b['key'] != 'eow' or b['table'] != key:
                raise SystemExit(f'{folder}: not Echoes of Wisdom {key}')
        src, dst = here / f'hot_functions_eow_{args.src}.txt', here / f'hot_functions_eow_{args.to}.txt'
    else:
        src, dst = here / 'hot_functions.txt', here / f'hot_functions_{args.to}.txt'
    m = FastMapper(args.old_game, args.new_game, args.gaps) if args.eow else Mapper(args.old_game, args.new_game)
    out, dropped = [], []
    for line in src.read_text().splitlines():
        if line.startswith('#') or not line.strip():
            continue
        mod, off = line.split()
        if mod == 'sdk':  # (an offset in the sdk module: kept when the sdk is byte-identical)
            if getattr(m, 'sdk_map', None) is not None:
                o = int(off, 16)
                if o in m.sdk_map:
                    out.append(f'sdk {m.sdk_map[o]:x}')
                else:
                    dropped.append(f'{line}: not matched in the other sdk')
                continue
            if not m.sdk_same:
                dropped.append(f'{line}: the sdk differs')
                continue
            out.append(line)
            continue
        new, how = m.map(MAIN_BASE + int(off, 16))
        if new is None:
            dropped.append(f'{line}: {how}')
            continue
        out.append(f'{mod} {new - MAIN_BASE:x}')
    head = [l.replace(', unchanged below this header', ', mapped below') for l in src.read_text().splitlines()
            if l.startswith('#') and not l.startswith('# Mapped onto')]
    how = ' --gaps' if args.gaps else ''
    text = '\n'.join(head + [f'# Mapped onto {args.to} by tools/hot_functions_map.py{how} ({len(out)} functions, {len(dropped)} left '
                              'out: in a changed function).'] + out) + '\n'
    dst.write_text(text)
    print(f'{dst.name}: {len(out)} functions, {len(dropped)} left out ({len(m.same)} of {len(m.starts_a)} functions identical)')
    for d in dropped:
        print('  left out:', d)


if __name__ == '__main__':
    main()
