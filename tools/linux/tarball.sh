#!/bin/bash
# make tarball ARCH=<x86_64|aarch64> (after make appimage, which leaves the AppDir): build/dist/ReTri-<version>-linux-<x64|arm64>.tar.gz
# The same files as the AppImage, unpacked into one "ReTri" folder, for places with no FUSE to mount an
# AppImage (a Flatpak sandbox such as a launcher's). Start it with ./AppRun (or ./eowrecomp, a link to it).
# portable.txt in the folder keeps the saves, settings and game files there, like the Windows zip
# (runtime/paths.c); delete it to use ~/.local/share/retri.
# See docs/linux.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=${1:?usage: tools/linux/tarball.sh x86_64|aarch64}
APPDIR=build/linux/$ARCH/AppDir
[ -x "$APPDIR/AppRun" ] || { echo "no $APPDIR: make appimage ARCH=$ARCH first"; exit 1; }
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
NAMEARCH=$([ "$ARCH" = aarch64 ] && echo arm64 || echo x64)  # (release names say x64 / arm64, as the AppImage)
OUT="build/dist/ReTri-$VERSION-linux-$NAMEARCH.tar.gz"
STAGE=build/linux/$ARCH/tarball
rm -rf "$STAGE" && mkdir -p "$STAGE/ReTri"
cp -a "$APPDIR/." "$STAGE/ReTri/"
ln -s AppRun "$STAGE/ReTri/eowrecomp"
cat > "$STAGE/ReTri/portable.txt" <<'EOF'
While this file is here, ReTri keeps its saves, settings and game files in this folder.
Delete it to keep them in ~/.local/share/retri instead.
EOF
tools/linux/check_no_game_code.sh "$STAGE/ReTri"
mkdir -p build/dist && rm -f "$OUT"
# (owner and times fixed: the same AppDir gives the same archive)
tar -C "$STAGE" --owner=0 --group=0 --numeric-owner --sort=name --mtime='@0' -czf "$OUT" "ReTri"
ls -la "$OUT"
