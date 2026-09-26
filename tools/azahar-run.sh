#!/usr/bin/env bash
# azahar-run.sh — run a .3dsx in the Azahar flatpak unattended and collect
#
# Generic copy kept with the port-stereo-game-to-3ds skill. The game side must
# implement the AUTOTEST.TXT / AUTOTEST.LOG hooks described in SKILL.md §9;
# pass the .3dsx by its path INSIDE the emulator's virtual SD card, since the
# script file is copied next to it and the log is read from there.
# evidence: the emulator log (which carries the game's stderr via
# svcOutputDebugString) and periodic screenshots of the emulator window.
#
#   azahar-run.sh <file.3dsx> [-t SECONDS] [-s SHOT_EVERY] [-o OUTDIR]
#                 [-3 MODE] [-d DEPTH] [--old3ds] [--gdb PORT]
#                 [--until REGEX] [--movie FILE.ctm]
#
#   -t        total run time before the emulator is killed (default 30)
#   -s        screenshot interval in seconds (default 5; 0 = none)
#   -o        output directory (default ./azahar-out/<timestamp>)
#   -3        render_3d: 0 off, 1 side-by-side, 2 anaglyph, 3 interlaced,
#             4 reverse-interlaced, 5 cardboard (default 0)
#   -d        factor_3d 0..100, the virtual 3D slider (default 0)
#   --old3ds  emulate an Old 3DS (is_new_3ds=false)
#   --gdb     enable the gdb stub on PORT (emulation waits for a debugger)
#   --until   stop early once the log matches this extended regex
#   --movie   play a TAS movie (.ctm) for scripted input
#   --script  copy this AUTOTEST.TXT next to the .3dsx for the run (see
#             platform_3ds.c for the format); without it any existing one stays
#
# Exit status: 0 if --until matched (or no --until given and the run
# completed), 2 if the emulator died early, 3 on timeout with --until unmet.
#
# Requires: flatpak org.azahar_emu.Azahar, xdotool, ImageMagick `import`.
# Runs on the user's X display (DISPLAY, Xwayland is fine) with the xcb Qt
# platform so xdotool/import can see the window. Azahar has no headless mode.
set -uo pipefail

APP=org.azahar_emu.Azahar
BASE=$HOME/.var/app/$APP
CFG=$BASE/config/azahar-emu/qt-config.ini
LOG=$BASE/data/azahar-emu/log/azahar_log.txt
SDMC=$BASE/data/azahar-emu/sdmc
# GLOG (the game's own flushed log, next to the .3dsx) is set once FILE is known.

FILE=""; TOTAL=30; SHOT=5; OUT=""; R3D=0; DEPTH=0; OLD3DS=0; GDB=""; UNTIL=""; MOVIE=""; SCRIPT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -t) TOTAL=$2; shift 2;;
        -s) SHOT=$2; shift 2;;
        -o) OUT=$2; shift 2;;
        -3) R3D=$2; shift 2;;
        -d) DEPTH=$2; shift 2;;
        --old3ds) OLD3DS=1; shift;;
        --gdb) GDB=$2; shift 2;;
        --until) UNTIL=$2; shift 2;;
        --movie) MOVIE=$2; shift 2;;
        --script) SCRIPT=$2; shift 2;;
        -*) echo "unknown option $1" >&2; exit 64;;
        *) FILE=$1; shift;;
    esac
done
[ -n "$FILE" ] && [ -f "$FILE" ] || { echo "usage: $0 <file.3dsx> [options]" >&2; exit 64; }
FILE=$(readlink -f "$FILE")
GLOG=$(dirname "$FILE")/AUTOTEST.LOG   # written by the game itself when AUTOTEST.TXT exists
[ -n "$OUT" ] || OUT=./azahar-out/$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"

# --- config: set the keys this run needs (QSettings ini; key=value lines) ---
setkey() {   # setkey SECTION KEY VALUE  — replaces or appends within the section
    python3 - "$CFG" "$1" "$2" "$3" <<'PY'
import sys, re
path, section, key, value = sys.argv[1:]
lines = open(path, encoding="utf-8").read().split("\n")
out, cur, done, seen = [], None, False, False
for ln in lines:
    m = re.match(r"^\[(.*)\]$", ln)
    if m:
        if cur == section and not done:
            out.append(f"{key}={value}"); done = True
        cur = m.group(1); seen = seen or cur == section
    elif cur == section and re.match(re.escape(key) + r"=", ln):
        ln = f"{key}={value}"; done = True
    elif cur == section and re.match(re.escape(key) + r"\\default=", ln):
        ln = f"{key}\\default=false"
    out.append(ln)
if not done:
    if not seen:
        out.append(f"[{section}]")
    out.append(f"{key}={value}")
open(path, "w", encoding="utf-8").write("\n".join(out))
PY
}
setkey Layout render_3d "$R3D"
setkey Layout factor_3d "$DEPTH"
setkey System is_new_3ds "$([ $OLD3DS = 1 ] && echo false || echo true)"
setkey Miscellaneous log_filter "*:Info Debug.Emulated:Debug"
setkey Debugging use_gdbstub "$([ -n "$GDB" ] && echo true || echo false)"
[ -n "$GDB" ] && setkey Debugging gdbstub_port "$GDB"
setkey UI confirmClose false
setkey UI firstStart false
setkey UI calloutFlags 4294967295
setkey UI pauseWhenInBackground false
setkey UI Paths\\screenshotPath "$OUT"
mkdir -p "$SDMC/3ds"; [ -e "$SDMC/3ds/dspfirm.cdc" ] || : > "$SDMC/3ds/dspfirm.cdc"

# --- scripted input ---
if [ -n "$SCRIPT" ]; then cp "$SCRIPT" "$(dirname "$FILE")/AUTOTEST.TXT"; fi

# --- launch ---
: > "$LOG"
rm -f "$GLOG"
ARGS=(-w)
[ -n "$GDB" ] && ARGS+=(-g "$GDB")
[ -n "$MOVIE" ] && ARGS+=(-p "$(readlink -f "$MOVIE")")
export DISPLAY=${DISPLAY:-:0}
flatpak run --env=QT_QPA_PLATFORM=xcb "$APP" "${ARGS[@]}" "$FILE" >"$OUT/azahar.stdout" 2>&1 &
FPID=$!
echo "azahar pid $FPID, output in $OUT"

win=""
find_window() {
    # the render window title is "Azahar <ver> | <title>"; pick the largest match
    xdotool search --onlyvisible --name "Azahar" 2>/dev/null | tail -1
}
shoot() {
    local tag=$1
    [ -z "$win" ] && win=$(find_window)
    [ -n "$win" ] || return 1
    import -window "$win" "$OUT/shot-$tag.png" 2>/dev/null \
        || magick x:"$win" "$OUT/shot-$tag.png" 2>/dev/null
}

rc=0
start=$(date +%s); next=$SHOT; matched=0; markers=0
while :; do
    now=$(( $(date +%s) - start ))
    if ! kill -0 $FPID 2>/dev/null; then echo "emulator exited early"; rc=2; break; fi
    # screenshots requested by the game itself ("AUTOTEST shot <name>" on stderr)
    n=$(grep -c "AUTOTEST shot" "$GLOG" 2>/dev/null); n=${n:-0}
    while [ "$n" -gt "$markers" ]; do
        markers=$((markers + 1))
        name=$(grep "AUTOTEST shot" "$GLOG" | sed -n "${markers}p" | sed -E 's/.*AUTOTEST shot ([A-Za-z0-9_.-]+).*/\1/')
        sleep 0.3; shoot "mark-$(printf %02d $markers)-$name"
    done
    if [ -n "$UNTIL" ] && { grep -Eq "$UNTIL" "$GLOG" 2>/dev/null || grep -Eq "$UNTIL" "$LOG" 2>/dev/null; }; then
        echo "log matched: $UNTIL"; matched=1; shoot "final-$now"; break
    fi
    if [ "$SHOT" -gt 0 ] && [ "$now" -ge "$next" ]; then shoot "$(printf %03d $now)s"; next=$(( next + SHOT )); fi
    if [ "$now" -ge "$TOTAL" ]; then
        shoot "final-$now"
        if [ -n "$UNTIL" ]; then echo "timeout without match"; rc=3; fi
        break
    fi
    sleep 1
done

# stop the emulator: a clean close flushes its log file, so wait for it
for w in $(xdotool search --onlyvisible --name "Azahar" 2>/dev/null); do xdotool key --window "$w" ctrl+q 2>/dev/null; done
sleep 2
[ -z "$win" ] && win=$(find_window)
[ -n "$win" ] && kill -0 $FPID 2>/dev/null && xdotool windowclose "$win" 2>/dev/null
for i in 1 2 3 4 5 6 7 8 9 10; do kill -0 $FPID 2>/dev/null || break; sleep 1; done
kill -0 $FPID 2>/dev/null && { echo "emulator did not close; killing"; flatpak kill "$APP" 2>/dev/null; sleep 1; kill -9 $FPID 2>/dev/null; }
wait $FPID 2>/dev/null
cp "$LOG" "$OUT/azahar_log.txt" 2>/dev/null
# the game's stderr lines, extracted for convenience
if [ -s "$GLOG" ]; then
    cp "$GLOG" "$OUT/game_stderr.txt"
else
    grep -E "Debug\.Emulated" "$OUT/azahar_log.txt" | sed -E 's/^.*OutputDebugString[^:]*: ?//' > "$OUT/game_stderr.txt" 2>/dev/null
fi
echo "log lines: $(wc -l < "$OUT/azahar_log.txt"), game stderr lines: $(wc -l < "$OUT/game_stderr.txt"), shots: $(ls "$OUT"/*.png 2>/dev/null | wc -l)"
exit $rc
