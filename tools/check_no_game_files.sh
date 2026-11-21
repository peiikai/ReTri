#!/bin/bash
# tools/check_no_game_files.sh <folder or file> [...]: fails if a build that ships holds anything of the game - game
# files, generated game code, a built game library, or a program / library with game functions in it (f_70...: the
# game's own addresses, f_80... under --dyn-base). Releases and nightlies run it on every file they put up
# (.github/workflows/nightly.yml, release.yml); the players' start screen builds the game code from their own files.
# Takes a folder (an .app, an AppDir, the Windows zip's folder) or a packed file: .zip, .laskit, .tar.gz, .dmg (macOS),
# .AppImage (Linux, same arch), .apk (also tools/check_apk.sh's list of what an APK may hold).
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: tools/check_no_game_files.sh <folder or file> [...]" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NM=$(command -v llvm-nm || command -v nm || true)  # (llvm-nm reads Mach-O, ELF and PE alike)
T=$(mktemp -d)
MNT=
cleanup() { [ -z "$MNT" ] || hdiutil detach -quiet "$MNT" || true; rm -rf "$T"; }
trap cleanup EXIT
fail=0
err() { echo "::error::$1"; fail=1; }

check_dir() {  # <folder> <name for messages>
    local A=$1 what=$2 f
    # game files and generated code by name (gamelib-src/gamelib_entry.c: the stub the start screen builds with;
    # zig/lib: zig's own runtime sources)
    while IFS= read -r f; do [ -n "$f" ] && err "$what: must not ship: ${f#"$A"/}"; done < <(
        find "$A" -type d \( -name extracted -o -name 'generated*' -o -name exefs -o -name romfs \) -prune -print
        find "$A" -name '*.c' ! -path '*/zig/lib/*' ! -path '*/gamelib-src/gamelib_entry.c'
        # (LASGame.framework itself ships, with only its Info.plist: the start screen adds the library on the device)
        find "$A" -type f \( -iname 'eowgame*' -o -iname 'lasgame*' \) ! -name '*.h'
        find "$A" -type f \( -name main -o -name sdk -o -name 'subsdk[0-9]*' -o -name rtld -o -name '*.npdm' -o -name '*.nso' \
            -o -name '*.nca' -o -name '*.nsp' -o -name '*.xci' -o -name '*.bfres' -o -name '*.bntx' -o -name '*.sarc' \
            -o -name '*.pack' -o -name '*.zs' -o -name '*.bars' -o -name '*.bwav' -o -name '*.bfsar' -o -name '*.bfstm' \
            -o -name '*.byml' -o -name '*.bgyml' -o -name '*.msbt' -o -name '*.rstbl' -o -name '*.bfevfl' -o -name '*.bcsv' \)
    )
    # game functions linked into a program or library (Mach-O, ELF, PE)
    [ -n "$NM" ] || { err "$what: no nm or llvm-nm to look inside the programs"; return; }
    while IFS= read -r f; do
        case $(head -c 4 "$f" | od -An -tx1 | tr -d ' \n') in
            7f454c46|cffaedfe|cafebabe|4d5a*) ;;  # ELF, Mach-O 64, universal, PE
            *) continue ;;
        esac
        if "$NM" "$f" 2>/dev/null | grep -Eq ' _?f_[78]0[0-9a-f]'; then err "$what: game code in ${f#"$A"/}"; fi
    done < <(find "$A" -type f \( -perm -u+x -o -name '*.dylib' -o -name '*.so' -o -name '*.so.*' -o -name '*.dll' -o -name '*.exe' \))
}

for P in "$@"; do
    name=$(basename "$P")
    if [ -d "$P" ]; then check_dir "$P" "$name"; continue; fi
    [ -f "$P" ] || { err "no file $P"; continue; }
    D="$T/$(printf '%s' "$name" | tr -c 'A-Za-z0-9._-' _)"
    mkdir -p "$D"
    case $name in
        *.zip|*.laskit) unzip -q "$P" -d "$D" ;;
        *.apk) "$ROOT/tools/check_apk.sh" "$P" || fail=1; unzip -q "$P" -d "$D"
               [ ! -f "$D/assets/gen.zip" ] || unzip -q "$D/assets/gen.zip" -d "$D/assets/gen" ;;
        *.tar.gz) tar -xzf "$P" -C "$D" ;;
        *.dmg) MNT="$T/mnt"; mkdir -p "$MNT"
               hdiutil attach -quiet -nobrowse -readonly -mountpoint "$MNT" "$P"
               cp -R "$MNT/." "$D/"; hdiutil detach -quiet "$MNT"; MNT= ;;
        *.AppImage) [ -x "$P" ] || chmod +x "$P"
                    A="$(cd "$(dirname "$P")" && pwd)/$name"  # (absolute: extracted from inside $D)
                    (cd "$D" && "$A" --appimage-extract >/dev/null) ;;
        *) err "$name: not a file type this check knows"; continue ;;
    esac
    check_dir "$D" "$name"
done
[ $fail = 0 ] || exit 1
echo "no game files or game code: $*"
