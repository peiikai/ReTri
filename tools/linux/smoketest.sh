#!/bin/bash
# make smoketest (tools/linux/smoketest.sh [AppImage]): release check of the x86-64 AppImage on this machine, in three
# launches of the AppImage itself (default build/dist/ReTri-<version>-linux-x64.AppImage):
#   0. setup, as the start screen's Set up does it: this checkout's extracted/ (exefs, romfs) into an empty data folder,
#      and the game code built from it with the bundled generator and compiler (runtime/gamebuild.c)
#   1. start screen: rendered with the game files found ("Game files ready")
#   2. a new game: the opening plays by itself into the first room; screenshots at fixed frames must not be black,
#      and the log must show no crash
# Result: build/smoke/report.txt (PASS / FAIL per check), build/smoke/sheet.png (every screenshot, labelled) and the
# screenshots in build/smoke/shots/. Exit status 0 = ready for release.
# Uses this checkout's extracted/ (as the folder a player picks), copied into build/smoke/root by the setup (delete
# build/smoke afterwards to free the space). Nothing in ~/.local/share/eowrecomp is used or changed.
set -uo pipefail
cd "$(dirname "$0")/../.."
REPO=$PWD
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
APP=$(realpath "${1:-build/dist/ReTri-$VERSION-linux-x64.AppImage}")
O=$REPO/build/smoke
[ -x "$APP" ] || { echo "smoketest: no AppImage at $APP (make appimage)"; exit 2; }
[ -d extracted/romfs ] || { echo "smoketest: no extracted/ game files in this checkout"; exit 2; }

rm -rf "$O" && mkdir -p "$O/root/build" "$O/shots"
mkdir -p "$O/save"
# test runs: no update check, no sound, a data folder of their own (runtime/paths.h), the AppImage run without FUSE
export LARECOMP_DATA_ROOT=$O/root LARECOMP_SAVE_DIR=$O/save LARECOMP_SETTINGS=$O/root/larecomp.ini \
       LARECOMP_NO_UPDATE_CHECK=1 LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1 APPIMAGE_EXTRACT_AND_RUN=1
FAILS=0
report() {  # PASS|FAIL, text
    printf '%-4s  %s\n' "$1" "$2" | tee -a "$O/report.txt"
    [ "$1" = PASS ] || FAILS=$((FAILS + 1))
}
dark() { python3 -I -c "
import sys
from PIL import Image, ImageStat
print(1 if sum(ImageStat.Stat(Image.open(sys.argv[1]).convert('L')).mean) < 12 else 0)" "$1"; }

# The AppImage runtime starts the game as a child process: each run gets a process group of its own (setsid), so
# stopping it stops the game too - nothing it uses (the seed) is deleted while it still runs.
start_app() {  # VAR=value...: the AppImage in the data folder, with those settings
    (cd "$O/root" && exec env "$@" setsid "$APP" </dev/null >/dev/null 2>&1) &
    APP_PG=$!
}
app_alive() { ps -e -o pgid=,stat= | awk -v g="$APP_PG" '$1 == g && $2 !~ /^Z/ {f = 1} END {exit !f}'; }
stop_app() {
    kill -TERM -- -"$APP_PG" 2>/dev/null
    for i in $(seq 1 20); do app_alive || break; sleep 0.5; done
    app_alive && kill -KILL -- -"$APP_PG" 2>/dev/null
    while app_alive; do sleep 0.5; done
    wait "$APP_PG" 2>/dev/null
}
wait_app() {  # seconds: status 0 = the game ended by itself in time, else it is stopped
    for i in $(seq 1 "$1"); do app_alive || { wait "$APP_PG" 2>/dev/null; return 0; }; sleep 1; done
    stop_app
    return 1
}

# ---- 0. setup (headless: LARECOMP_LAUNCHER_EXTRACT runs the start screen's Set up, logs the result and exits)
t0=$(date +%s)
start_app LARECOMP_LAUNCHER_EXTRACT="$REPO/extracted"
wait_app 1800
mv "$O/root/eowrecomp.log" "$O/setup.log" 2>/dev/null
lib=$(ls "$O/root/extracted"/eowgame.* 2>/dev/null | head -1)
if grep -q '\[launcher\] setup: .* -> READY' "$O/setup.log" && [ -n "$lib" ]; then
    report PASS "setup: game files and game code built ($(( $(date +%s) - t0 )) s, $(basename "$lib") $(du -m "$lib" | cut -f1) MB)"
else
    report FAIL "setup: $(grep -m1 '\[launcher\] setup:' "$O/setup.log" || echo 'no result') (see build/smoke/setup.log)"
fi

# ---- 1. start screen
# (no terminal: the app logs to eowrecomp.log in its data folder, runtime/platform/linux.c)
start_app LARECOMP_LAUNCHER_SHOT=150 LARECOMP_SHOT_PATH="$O/shots/00_start_screen.png"
wait_app 90
mv "$O/root/eowrecomp.log" "$O/start_screen.log" 2>/dev/null
if [ -f "$O/shots/00_start_screen.png" ] && [ "$(dark "$O/shots/00_start_screen.png")" = 0 ] &&
   ! grep -qE '\*\*\* |FATAL' "$O/start_screen.log"; then
    report PASS "start screen ($(grep -o 'using .*' "$O/start_screen.log" | head -1))"
else
    report FAIL "start screen (see build/smoke/start_screen.log)"
fi

# ---- 2. game session: a new game from the start, no input (EoW's opening plays by itself into the first room).
# Screenshots at fixed frames; the last one ends the run. ~1 min on a GPU at 60 fps; software Vulkan (llvmpipe) is
# far slower, so the run gets up to SMOKE_GAME_SECONDS (default 600).
SHOTS=(300 900 1800 3600)
NAMES=("opening cutscene" "first room" "first room" "first room, HUD")
LAST=${SHOTS[${#SHOTS[@]}-1]}
AT=$(IFS=,; echo "${SHOTS[*]::${#SHOTS[@]}-1}")
start_app LARECOMP_NO_LAUNCHER=1 LARECOMP_SHOT_AT="$AT" LARECOMP_WINDOW_SHOT=$LAST \
    LARECOMP_SHOT_PATH="$O/root/build/frames/frame_$(printf %05d $LAST).png" LARECOMP_EXIT_AFTER_SHOT=1
wait_app "${SMOKE_GAME_SECONDS:-600}"
rc=$?
mv "$O/root/eowrecomp.log" "$O/game.log" 2>/dev/null
L=$O/game.log
for i in "${!SHOTS[@]}"; do
    shot=$(printf '%s/root/build/frames/frame_%05d.png' "$O" "${SHOTS[$i]}")
    out=$(printf '%s/shots/%02d_frame_%d_%s.png' "$O" $((i + 1)) "${SHOTS[$i]}" "$(echo "${NAMES[$i]}" | tr -c 'A-Za-z0-9\n' '_')")
    [ -f "$shot" ] && cp "$shot" "$out"
    if [ ! -f "$out" ]; then report FAIL "frame ${SHOTS[$i]}: no screenshot"
    elif [ "$(dark "$out")" = 1 ]; then report FAIL "frame ${SHOTS[$i]}: screenshot is black"
    else report PASS "frame ${SHOTS[$i]} (${NAMES[$i]})"; fi
done
if grep -qE '\*\*\* |FATAL' "$L"; then report FAIL "crash: $(grep -m1 -E '\*\*\* |FATAL' "$L")"
elif [ $rc -ne 0 ]; then report FAIL "game session didn't end by itself (stopped after ${SMOKE_GAME_SECONDS:-600} s)"
else report PASS "no crash, clean exit"; fi
fps=$(grep -oE '[0-9.]+ fps' "$L" | awk '{s += $1; n++; if ($1 < m || !m) m = $1} END {if (n) printf "%.1f avg, %.1f min", s / n, m}')
echo "frame rate (2 s samples): $fps" | tee -a "$O/report.txt"

python3 -I - "$O/sheet.png" "$O"/shots/*.png <<'PY'
import os, sys
from PIL import Image, ImageDraw
out, fs = sys.argv[1], sys.argv[2:]
w, h, cols = 400, 225, 4
rows = (len(fs) + cols - 1) // cols
m = Image.new('RGB', (w * cols, h * rows))
for i, f in enumerate(fs):
    im = Image.open(f).convert('RGB').resize((w, h))
    d = ImageDraw.Draw(im)
    label = os.path.basename(f)[3:-4].replace('_', ' ')
    d.rectangle((0, 0, 8 + 7 * len(label), 16), fill=(0, 0, 0))
    d.text((4, 2), label, fill=(255, 230, 80))
    m.paste(im, ((i % cols) * w, (i // cols) * h))
m.save(out)
PY
if [ $FAILS -eq 0 ]; then echo "READY FOR RELEASE: every check passed" | tee -a "$O/report.txt"
else echo "NOT READY: $FAILS check(s) failed" | tee -a "$O/report.txt"; fi
[ $FAILS -eq 0 ]
