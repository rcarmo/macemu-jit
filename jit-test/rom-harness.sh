#!/bin/bash
# Headless Mac — boots Quadra 800 ROM through JIT, no display server needed.
# Usage:
#   ./jit-test/rom-harness.sh                      # 2 min default
#   B2_TIMEOUT=600 ./jit-test/rom-harness.sh       # 10 min full boot
#   B2_JIT=false ./jit-test/rom-harness.sh         # interpreter comparison
set -uo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$DIR/BasiliskII/src/Unix/BasiliskII"
ROM="${B2_ROM:-/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM}"
JIT="${B2_JIT:-true}"
SECS="${B2_TIMEOUT:-120}"

[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
[ -x "$BIN" ] || { echo "Binary not found: $BIN" >&2; exit 1; }

W=$(mktemp -d /tmp/headless-mac-XXXXXX)
trap 'rm -rf "$W"' EXIT
cat >"$W/prefs" <<EOF
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

echo "Headless Mac: jit=$JIT timeout=${SECS}s" >&2
env HOME="$W" B2_ROM_HARNESS=999999 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
  timeout -k5 "${SECS}" "$BIN" --config "$W/prefs" >"$W/out" 2>"$W/err"
RC=$?

# Parse
LAST=$(grep '^DC\[' "$W/err" | tail -1 || true)
DC_NUM=$(echo "$LAST" | sed -n 's/DC\[\([0-9]*\)\].*/\1/p')
PC=$(echo "$LAST" | sed -n 's/.* pc=\([0-9a-f]*\) .*/\1/p')
SR=$(echo "$LAST" | sed -n 's/.* sr=\([0-9a-f]*\) .*/\1/p')
SCSI=$(grep -c SCSIGet "$W/err" || true)
SEGV=$(grep -c SEGV_SKIP "$W/err" || true)

IN_RAM=no
if [ -n "$PC" ]; then
  PCV=$((16#$PC))
  ! ([ "$PCV" -ge $((16#800000)) ] && [ "$PCV" -le $((16#8FFFFF)) ]) && IN_RAM=yes
fi

echo "" >&2
echo "=== Headless Mac ===" >&2
grep '^DC\[' "$W/err" | tail -5 >&2
for m in "PatchROM ok" SCSIGet set_dsk_err DiskControl; do
  grep -q "$m" "$W/err" 2>/dev/null && echo "  ✅ $m" >&2 || echo "  ❌ $m" >&2
done
echo "pc=${PC:-?} sr=${SR:-?} dc=${DC_NUM:-0} in_ram=$IN_RAM scsi=$SCSI segv=$SEGV rc=$RC" >&2

echo "METRIC headless_pc=${PC:-?}"
echo "METRIC headless_dc=${DC_NUM:-0}"
echo "METRIC headless_in_ram=$IN_RAM"
echo "METRIC headless_scsi=$SCSI"
echo "METRIC headless_segv=$SEGV"
