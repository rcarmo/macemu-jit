#!/bin/bash
# BasiliskII ROM Boot Harness — exercises the JIT with real ROM code
# Uses Xvfb for headless SDL video. Parses DC[] dispatch counter output for progress.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
UNIX_DIR="$(cd "$SCRIPT_DIR/../BasiliskII/src/Unix" && pwd)"
ROM="${B2_ROM:-/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM}"
MAX_TICKS="${B2_MAX_TICKS:-6000}"
JIT="${B2_JIT:-true}"
TIMEOUT="${B2_TIMEOUT:-120}"

if [ ! -f "$ROM" ]; then echo "ERROR: ROM not found: $ROM" >&2; exit 1; fi
if [ ! -x "$UNIX_DIR/BasiliskII" ]; then echo "ERROR: binary not found" >&2; exit 1; fi

WORKDIR=$(mktemp -d /tmp/rom-harness-XXXXXX)
trap 'pkill -f "Xvfb :98" 2>/dev/null; rm -rf "$WORKDIR"' EXIT

cat > "$WORKDIR/prefs" <<EOF
rom $ROM
ramsize 8388608
modelid 14
cpu 4
fpu false
jit $JIT
jitfpu false
jitcachesize 8192
screen win/640/480
nosound true
nocdrom true
ignoresegv true
EOF

pkill -f "Xvfb :98" 2>/dev/null || true
rm -f /tmp/.X98-lock /tmp/.X11-unix/X98
Xvfb :98 -screen 0 640x480x24 >/dev/null 2>&1 &
sleep 0.5

echo "=== ROM Boot Harness ===" >&2
echo "ROM: $ROM | JIT: $JIT | max_ticks: $MAX_TICKS | timeout: ${TIMEOUT}s" >&2

RC=0
env HOME="$WORKDIR" \
    B2_ROM_HARNESS="$MAX_TICKS" \
    SDL_VIDEODRIVER=x11 DISPLAY=:98 SDL_AUDIODRIVER=dummy \
    timeout -k 5s "${TIMEOUT}s" \
    "$UNIX_DIR/BasiliskII" --config "$WORKDIR/prefs" \
    > "$WORKDIR/stdout.log" 2> "$WORKDIR/stderr.log" || RC=$?

# Parse DC[] dispatch counter lines for PC progress
LAST_DC=$(grep '^DC\[' "$WORKDIR/stderr.log" | tail -1 || true)
DC_COUNT=$(grep -c '^DC\[' "$WORKDIR/stderr.log" || true)
FINAL_PC=$(echo "$LAST_DC" | grep -oP 'pc=\K[^ ]*' || echo "?")
FINAL_SR=$(echo "$LAST_DC" | grep -oP 'sr=\K[^ ]*' || echo "?")

# Check for harness result
RESULT="UNKNOWN"
grep -q 'ROM_HARNESS: result=MAX_TICKS' "$WORKDIR/stderr.log" && RESULT="MAX_TICKS"
grep -q 'ROM_HARNESS: result=IDLE_LOOP' "$WORKDIR/stderr.log" && RESULT="IDLE_LOOP"
[ "$RC" -eq 124 ] || [ "$RC" -eq 137 ] && RESULT="TIMEOUT"
grep -q 'SIGSEGV\|SIGILL' "$WORKDIR/stderr.log" && RESULT="CRASH"
[ "$RESULT" = "UNKNOWN" ] && [ "$RC" -ne 0 ] && RESULT="EXIT_$RC"
[ "$RESULT" = "UNKNOWN" ] && RESULT="CLEAN_EXIT"

# Detect if boot reached RAM (PC < 0x800000)
IN_RAM="no"
if [ "$FINAL_PC" != "?" ]; then
    pc_val=$((16#${FINAL_PC}))
    [ "$pc_val" -lt $((16#800000)) ] && IN_RAM="yes"
fi

# Milestones
echo "" >&2
echo "--- Milestones ---" >&2
for p in "PatchROM ok" "Init680x0 ok" "SCSIGet" "set_dsk_err" "DiskControl"; do
    grep -q "$p" "$WORKDIR/stderr.log" && echo "  ✅ $p" >&2 || echo "  ❌ $p" >&2
done

# PC progression
echo "--- PC progression (every 10K dispatches) ---" >&2
grep '^DC\[' "$WORKDIR/stderr.log" | awk 'NR%10==0 || NR<=5' | tail -10 >&2

echo "" >&2
echo "=== RESULT: $RESULT ===" >&2
echo "pc=$FINAL_PC sr=$FINAL_SR dispatches=$DC_COUNT in_ram=$IN_RAM rc=$RC" >&2

# Machine-readable metrics
echo "METRIC rom_result=$RESULT"
echo "METRIC rom_pc=$FINAL_PC"
echo "METRIC rom_sr=$FINAL_SR"
echo "METRIC rom_dispatches=$DC_COUNT"
echo "METRIC rom_in_ram=$IN_RAM"

case "$RESULT" in
    IDLE_LOOP|MAX_TICKS|CLEAN_EXIT) exit 0 ;;
    CRASH)   exit 1 ;;
    TIMEOUT) exit 2 ;;
    *)       exit 0 ;;
esac
