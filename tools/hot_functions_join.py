"""Join LARECOMP_FNPROF profiles of Echoes of Wisdom into its hot function list.

Usage:
  hot_functions_join.py [--to v1_0_2] [--busy-min 100] PROFILE... [--busy PROFILE]...

PROFILE: a file LARECOMP_FNPROF wrote (one "<address> <count>" line per game function entered; docs/debug-hooks.md).
Every function entered in any of them goes into tools/hot_functions_eow_<to>.txt as "main <offset>" / "sdk <offset>",
the form runtime/gamebuild.c reads: the Android setup builds those at -O1 and the rest with optnone. --busy profiles
add only the functions entered at least --busy-min times: from the warp tour (LARECOMP_WARP_TOUR, a few seconds in
every place), its places' per-frame code, not what only ran once while each loaded. Then map the list
onto the other builds with hot_functions_map.py --eow. Only addresses are written, nothing from the game files.
"""
import argparse
from pathlib import Path

MAIN_BASE = 0x7000000000
SDK_BASE = 0x7008000000  # (EoW's: runtime/game_addresses/games.h)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('profiles', nargs='+', type=Path)
    ap.add_argument('--busy', action='append', default=[], type=Path)
    ap.add_argument('--busy-min', type=int, default=100)
    ap.add_argument('--to', default='v1_0_2')
    ap.add_argument('--note', default='', help='how the profiles were recorded, for the header')
    args = ap.parse_args()
    hot = set()
    for p in args.profiles + args.busy:
        n = 0
        for line in p.read_text().splitlines():
            if line.strip() and (p not in args.busy or int(line.split()[1]) >= args.busy_min):
                hot.add(int(line.split()[0], 16))
                n += 1
        print(f'{p.name}: {n} functions')
    rows, outside = [], 0
    for a in sorted(hot):
        if MAIN_BASE <= a < SDK_BASE:
            rows.append(f'main {a - MAIN_BASE:x}')
        elif SDK_BASE <= a < SDK_BASE + 0x4000000:
            rows.append(f'sdk {a - SDK_BASE:x}')
        else:
            outside += 1
    head = ['# Hot game functions of Echoes of Wisdom for the hot/cold game code build (runtime/gamebuild.c): these at '
            '-O1, the rest with optnone.',
            f'# The functions entered in {len(args.profiles)} LARECOMP_FNPROF runs on {args.to}'
            + (f', and those entered {args.busy_min}+ times in {len(args.busy)} more' if args.busy else '')
            + ' (tools/hot_functions_join.py). Addresses only.']
    if args.note:
        head.append(f'# {args.note}')
    dst = Path(__file__).resolve().parent / f'hot_functions_eow_{args.to}.txt'
    dst.write_text('\n'.join(head + rows) + '\n')
    print(f'{dst.name}: {len(rows)} functions' + (f' ({outside} outside main and the sdk left out)' if outside else ''))


if __name__ == '__main__':
    main()
