#!/bin/bash
set -euo pipefail

UNIX_DIR="$(cd "$(dirname "$0")/BasiliskII/src/Unix" && pwd)"
ROM="/workspace/projects/rpi-basilisk2-sdl2-nox/Quadra800.ROM"
DISK="/workspace/tmp/rpi-basilisk-assets/HD200MB"
RUN_DIR="/workspace/tmp/autoresearch-jit-run-$$"

mkdir -p "$RUN_DIR/home" "$RUN_DIR/xdg"
cat >"$RUN_DIR/prefs" <<EOF
rom $ROM
disk $DISK
ramsize 67108864
modelid 14
cpu 4
fpu true
jit true
jitfpu false
jitcachesize 8192
screen win/640/480
displaycolordepth 8
nosound true
nocdrom true
nogui false
EOF

# Build
cd "$UNIX_DIR"
make clean >/dev/null 2>&1 || true
NO_CONFIGURE=1 ./autogen.sh >/dev/null 2>&1
ac_cv_have_asm_extended_signals=yes ./configure --with-uae-core=2021 --enable-aarch64-jit-experimental --disable-vosf >/dev/null 2>&1
if ! make -j12 >"$RUN_DIR/make.log" 2>&1; then
  echo "METRIC build_ok=0"
  echo "METRIC score=0"
  exit 0
fi
echo "METRIC build_ok=1"

# Run emulator for 120 seconds
Xvfb :23 -screen 0 800x600x24 &>/dev/null &
XVFB_PID=$!
sleep 1

SDL_VIDEODRIVER=x11 DISPLAY=:23 HOME="$RUN_DIR/home" XDG_CONFIG_HOME="$RUN_DIR/xdg" \
  "$UNIX_DIR/BasiliskII" --config "$RUN_DIR/prefs" >"$RUN_DIR/emu.log" 2>&1 &
EMU_PID=$!

# Monitor for 120 seconds
ALIVE_CHECKS=0
BOOT_OK=0
for i in $(seq 1 120); do
  sleep 1
  if ! kill -0 $EMU_PID 2>/dev/null; then
    echo "METRIC crash_time=$i"
    break
  fi
  ALIVE_CHECKS=$((ALIVE_CHECKS + 1))
  if [ $BOOT_OK -eq 0 ] && grep -q 'DiskStatus' "$RUN_DIR/emu.log" 2>/dev/null; then
    BOOT_OK=1
    echo "METRIC boot_time=$i"
  fi
done

# Check final state
if kill -0 $EMU_PID 2>/dev/null; then
  echo "METRIC alive_120s=1"
  echo "METRIC uptime=120"
else
  echo "METRIC alive_120s=0"
  echo "METRIC uptime=$ALIVE_CHECKS"
fi

DISK_STATUS=$(grep -c 'DiskStatus' "$RUN_DIR/emu.log" 2>/dev/null || echo 0)
SEGFAULTS=$(grep -c 'SIGSEGV\|Segmentation' "$RUN_DIR/emu.log" 2>/dev/null || echo 0)
JIT_BLOCKS=$(grep -c 'JIT_COMPILE' "$RUN_DIR/emu.log" 2>/dev/null || echo 0)

echo "METRIC boot_ok=$BOOT_OK"
echo "METRIC disk_status=$DISK_STATUS"
echo "METRIC segfaults=$SEGFAULTS"
echo "METRIC jit_blocks=$JIT_BLOCKS"

# Score: 0-100
# 40 pts for boot, 40 pts for 120s uptime, 10 pts for 0 segfaults, 10 pts for native blocks
SCORE=0
[ $BOOT_OK -eq 1 ] && SCORE=$((SCORE + 40))
[ $ALIVE_CHECKS -ge 120 ] && SCORE=$((SCORE + 40))
[ $SEGFAULTS -eq 0 ] && SCORE=$((SCORE + 10))
[ $JIT_BLOCKS -gt 0 ] && SCORE=$((SCORE + 10))
echo "METRIC score=$SCORE"

# Cleanup
kill $EMU_PID 2>/dev/null; sleep 1; kill -9 $EMU_PID 2>/dev/null
kill $XVFB_PID 2>/dev/null
rm -rf "$RUN_DIR"

echo "ARTIFACT_DIR $RUN_DIR"
