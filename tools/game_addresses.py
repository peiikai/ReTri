"""Read the game-address tables also included by the C/C++ runtime.

runtime/game_addresses/versions.h lists the supported game versions by the main program's build ID; each has its own
patches_<key>.h (used by the patch generator) and runtime_<key>.h (runtime only, so adding one does not invalidate
generated code). The headers are the authoritative data; no generated copy or Python address list needs updating
alongside them.

ADDRESS is empty until a version is selected (select_version / select_build_id): the patch tables in patches.py are
built from it on import, so select first. An unknown build ID is an error: there is no fallback table.
"""
from pathlib import Path
import re
from types import SimpleNamespace


class UnknownGameVersion(Exception):
    pass


def load_table(path):
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_ADDR'):
            continue
        match = re.fullmatch(r'GA_ADDR\(([A-Z][A-Z0-9_]*),\s+(0x[0-9a-f]+)\)', line)
        if not match or match[1] in values:
            raise ValueError(f'Invalid or duplicate game-address definition: {line}')
        values[match[1]] = int(match[2], 16)
    if not values:
        raise ValueError('Empty game-address table')
    return values


def load_versions(path):
    """{key: (label, build ID as 40 upper-case hex digits)} from versions.h, in file order."""
    versions = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_VERSION'):
            continue
        match = re.fullmatch(r'GA_VERSION\((v\d+_\d+_\d+),\s+"([^"]+)",\s+"([0-9A-F]{40})"\)', line)
        if not match or match[1] in versions:
            raise ValueError(f'Invalid or duplicate game version: {line}')
        versions[match[1]] = (match[2], match[3])
    if not versions:
        raise ValueError('No game versions')
    return versions


TABLE_DIR = Path(__file__).resolve().parent.parent / 'runtime/game_addresses'
VERSIONS = load_versions(TABLE_DIR / 'versions.h')


def load_games(path):
    """[{key, table, label, release, build_id, sdk_base}] from games.h, in file order: other games on the same engine,
    one row per build (its own address table, <key>_<table>.h)."""
    builds = []
    if not path.exists():
        raise FileNotFoundError(f'{path} is missing')
    for line in path.read_text(encoding='utf-8').splitlines():
        if not line.startswith('GA_GAME'):
            continue
        match = re.fullmatch(r'GA_GAME\(([a-z][a-z0-9_]*),\s+(v\d+_\d+_\d+),\s+"([^"]+)",\s+"([^"]+)",\s+'
                             r'"([0-9A-F]{40})",\s+(0x[0-9a-f]+)\)', line)
        if not match or any(b['build_id'] == match[5] or (b['key'], b['table']) == (match[1], match[2]) for b in builds):
            raise ValueError(f'Invalid or duplicate game build: {line}')
        builds.append(dict(key=match[1], table=match[2], label=match[3], release=match[4], build_id=match[5],
                           sdk_base=int(match[6], 16)))
    return builds


def load_other_table(path):
    """{name: address} from an other game's table (OTHER_ADDR lines)."""
    values = {}
    for line in Path(path).read_text(encoding='utf-8').splitlines():
        if not line.startswith('OTHER_ADDR'):
            continue
        match = re.fullmatch(r'OTHER_ADDR\(([A-Z][A-Z0-9_]*),\s+(0x[0-9a-f]+)\)', line)
        if not match or match[1] in values:
            raise ValueError(f'Invalid or duplicate game-address definition: {line}')
        values[match[1]] = int(match[2], 16)
    if not values:
        raise ValueError(f'Empty game-address table {path}')
    return values


GAME_BUILDS = load_games(TABLE_DIR / 'games.h')
GAMES = {b['key']: b['label'] for b in GAME_BUILDS}  # key -> label


def table_path(key, kind='patches'):
    return TABLE_DIR / f'{kind}_{key}.h'


def version_for_build_id(build_id):
    """The version key for a main build ID (hex, any case; Nso.build_id strips trailing zero bytes)."""
    want = build_id.upper()
    padded = want.ljust(64, '0')  # (the header's 32-byte field: a 20-byte ID, zero-filled)
    for key, (label, bid) in VERSIONS.items():
        if padded[:40] == bid and not padded[40:].strip('0'):
            return key
    supported = ', '.join(f'{label} ({bid[:16]})' for label, bid in VERSIONS.values())
    raise UnknownGameVersion(f'These game files are an unsupported version of the game (main build ID '
                             f'{want[:16] or "missing"}). Supported: {supported}.')


def game_build(build_id):
    """The games.h row of a main build ID, or None (LA, or unknown)."""
    padded = build_id.upper().ljust(64, '0')
    for b in GAME_BUILDS:
        if padded[:40] == b['build_id'] and not padded[40:].strip('0'):
            return b
    return None


def game_for_build_id(build_id):
    """The games.h key for a main build ID, or None (LA, or unknown)."""
    b = game_build(build_id)
    return b['key'] if b else None


def other_table(b):
    """The address table of a games.h row: {name: address}."""
    return load_other_table(TABLE_DIR / f"{b['key']}_{b['table']}.h")


ADDRESS = SimpleNamespace()
SELECTED = None


def select_version(key):
    """Fill ADDRESS from patches_<key>.h (in place: modules that imported it see the new values)."""
    global SELECTED
    if key not in VERSIONS:
        raise UnknownGameVersion(f'Unknown game version {key}')
    ADDRESS.__dict__.clear()
    ADDRESS.__dict__.update(load_table(table_path(key)))
    SELECTED = key
    return key


def select_build_id(build_id):
    return select_version(version_for_build_id(build_id))


def addr_text(code):
    """Expand @NAME@ as hex digits inside generated-C addresses, symbols or labels."""
    return re.sub(r'@([A-Z][A-Z0-9_]*)@', lambda m: f'{getattr(ADDRESS, m[1]):x}', code)
