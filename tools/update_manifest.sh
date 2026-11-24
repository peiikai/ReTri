#!/bin/bash
# The self-updater's signed manifest for one release file (runtime/update_core.h): build/dist/update-<platform>.txt.
# Run after each platform's release build, with UPDATE_SIGNING_KEY in the
# environment. Without the key (a fork, or a run before the key existed) it only warns: the release still ships, and
# the start screen's Update needs the manifest, so players there use Download.
# Usage: tools/update_manifest.sh <version> <platform> <file> [<folder the zip holds>]
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { echo "usage: tools/update_manifest.sh <version> <platform> <file> [<folder>]" >&2; exit 2; }
out=build/dist/update-$2.txt
rm -f "$out"
if [ -z "${UPDATE_SIGNING_KEY:-}" ]; then
    echo "::warning::UPDATE_SIGNING_KEY is not set: no update manifest for $2 (players there update with Download)"
    exit 0
fi
mkdir -p build/dist
# (-lbcrypt: BCryptGenRandom. update_sign.cpp's #pragma comment(lib) is ignored by MinGW's ld and lld)
case $(uname -s) in MINGW*|MSYS*) LIBS=-lbcrypt ;; *) LIBS= ;; esac
"${CXX_UPDATE:-clang++}" -std=c++17 -O2 -Iruntime tools/update_sign.cpp -o build/update_sign $LIBS
build/update_sign manifest "$@" > "$out.part"
# (read back from the file as the app reads it: catches anything the way out changed, e.g. CR LF line ends)
build/update_sign verify "$out.part" "$(build/update_sign public | awk '/^public /{print $2}')"
mv "$out.part" "$out"
head -4 "$out"
echo "$(grep -c '^file ' "$out" || true) files; $out"
