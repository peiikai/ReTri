#!/usr/bin/env python3
# Update paths across every shipped version: builds each one's OWN update check (its tag's launcher.mm /
# update_tags.h, from git) and runs it on the live releases list, then on planned releases added to it. Use it before
# changing tags, file names or which files a release carries. Needs git tags, clang++ and the network (or a saved
# releases JSON: tools/test_update_paths.py releases.json). The repo is private for now: the live list needs
# GH_TOKEN (or `gh auth login`); without it the planned releases are checked on an empty list.
# Exits 1 when this build (the working tree) would take a wrong path in a planned scenario.
# Run from the repo root: python3 tools/test_update_paths.py
import copy, json, os, re, subprocess, sys, tempfile, urllib.request

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'], capture_output=True, text=True, check=True).stdout.strip()
TMP = tempfile.mkdtemp(prefix='update-paths-')


def show(rev, path):
    if rev == 'WORKTREE':
        p = os.path.join(ROOT, path)
        return open(p).read() if os.path.exists(p) else ''
    return subprocess.run(['git', '-C', ROOT, 'show', f'{rev}:{path}'], capture_output=True, text=True).stdout


# one checker per shipped build: its tag_rank (launcher.mm up to Beta 5.2, update_tags.h after) and its decision block
# ("std::string best;" up to the log line), with the defines the shipped build had
def build(name, rev, defines):
    src = show(rev, 'runtime/ui/launcher.mm')
    tags = show(rev, 'runtime/ui/update_tags.h')
    d = os.path.join(TMP, name)
    os.makedirs(d)
    if tags:
        open(os.path.join(d, 'update_tags.h'), 'w').write(tags)
        head = '#include "update_tags.h"\n'
    else:
        head = re.search(r'static long long tag_rank\(.*?\n}\n', src, re.S).group(0)
    block = re.search(r'\n    std::string best;\n.*?\n(?=    if \(!best\.empty\(\)\))', src, re.S).group(0)
    base = {k: m.group(1) for k in ('LARECOMP_VERSION', 'LARECOMP_TEST_OF')
            if (m := re.search(r'^#define ' + k + r' (".*?")', src, re.M))}
    base.setdefault('LARECOMP_TEST_OF', '""')
    defs = ''.join(f'#define {k} {v}\n' for k, v in {**base, **defines}.items())
    open(os.path.join(d, 'check.cpp'), 'w').write(f'''#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <string>
{defs}{head}
int main(int argc, char **argv) {{
    std::string body; FILE *f = fopen(argv[1], "rb"); char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) body.append(buf, n); fclose(f);
{block}
    printf("%s\\n", best.empty() ? "-" : best.c_str());
}}
''')
    exe = os.path.join(d, 'check')
    subprocess.run(['clang++', '-std=c++17', '-O1', '-w', os.path.join(d, 'check.cpp'), '-o', exe], check=True)
    return exe


Q = lambda s: f'"{s}"'
# ReTri's platforms (runtime/ui/launcher.mm LARECOMP_PLATFORM; the release files: ReTri-<version>-<platform>.<ext>)
PLATS = ['macOS.dmg', 'windows-x64.zip', 'windows-arm64.zip', 'linux-x64.AppImage', 'linux-arm64.AppImage',
         'android.apk', 'iOS.laskit']
# Every shipped build: name -> (its tag, defines). None yet; add each release here once it's tagged (vX.Y.Z), with
# the defines its builds had that differ by platform.
BUILDS = {}
HERE = 'WORKTREE'  # (this build: the working tree's launcher.mm and update_tags.h)
for p in ('macOS', 'windows-x64', 'linux-x64', 'android'):
    BUILDS[f'this build {p}'] = (HERE, {'LARECOMP_PLATFORM': Q(p), 'LARECOMP_FILE_PLATFORM': Q(p),
                                        'LARECOMP_EXPERIMENTAL_UNTIL': '""', 'LARECOMP_EXPERIMENTAL_STAMP': '0LL',
                                        'LARECOMP_NIGHTLY_STAMP': '0LL'})


def release(tag):
    files = [f'ReTri-{tag}-{p}' for p in PLATS]
    return {'tag_name': tag, 'name': tag, 'prerelease': False, 'draft': False, 'assets': [{'name': n} for n in files]}


def live_releases():
    if len(sys.argv) > 1:
        return json.load(open(sys.argv[1]))
    tok = os.environ.get('GH_TOKEN') or subprocess.run(['gh', 'auth', 'token'], capture_output=True, text=True).stdout.strip()
    hdr = {'Accept': 'application/vnd.github+json', 'User-Agent': 'ReTri/test'}
    if tok:
        hdr['Authorization'] = f'Bearer {tok}'
    try:
        rq = urllib.request.Request('https://api.github.com/repos/peiikai/ReTri/releases?per_page=100', headers=hdr)
        return json.load(urllib.request.urlopen(rq, timeout=30))
    except Exception as e:  # (private repo without a token: 404)
        print(f'(no live releases list: {e}; planned releases only)')
        return []


def main():
    live = live_releases()
    exes = {n: build(f'b{i}', *b) for i, (n, b) in enumerate(BUILDS.items())}
    version = re.search(r'^#define LARECOMP_VERSION "(.*?)"', show(HERE, 'runtime/ui/launcher.mm'), re.M).group(1)
    # scenario -> what this build must offer ("-": nothing)
    scen = {'live releases': (live, None)}
    scen[f'+ {version} (this one)'] = ([release(version)] + live, '-')
    scen['+ v0.1.0-beta.1 (a pre-release suffix: not a release)'] = ([release('v0.1.0-beta.1')] + live, '-')
    scen['+ v0.1.1'] = ([release('v0.1.1'), release(version)] + live, 'v0.1.1')
    scen['+ v0.2.0'] = ([release('v0.2.0'), release('v0.1.1'), release(version)] + live, 'v0.2.0')
    scen['+ v1.0.0'] = ([release('v1.0.0'), release('v0.2.0')] + live, 'v1.0.0')
    bad = 0
    for sn, (rels, want) in scen.items():
        fn = os.path.join(TMP, 'releases.json')
        json.dump(rels, open(fn, 'w'), indent=2)  # (pretty, as the API sends it)
        print(f'== {sn}')
        for n, exe in exes.items():
            got = subprocess.run([exe, fn], capture_output=True, text=True).stdout.strip()
            ok = want is None or not n.startswith('this build') or got == want
            bad += not ok
            print(f'   {n:40s} -> {got}{"" if ok else f"   WRONG, expected {want}"}')
    print('all as expected' if not bad else f'{bad} wrong')
    return 1 if bad else 0


sys.exit(main())
