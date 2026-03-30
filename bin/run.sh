#!/bin/bash
# Usage: ./run.sh [jit|nojit] [--vnc]
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-jit}"
EXTRA_ARGS="${@:2}"

BIN="$DIR/BasiliskII-$MODE"
if [ ! -f "$BIN" ]; then echo "No binary: $BIN"; exit 1; fi

SESS="/workspace/tmp/mac_session_$$"
mkdir -p "$SESS/home" "$SESS/xdg"
cp "$DIR/prefs" "$SESS/prefs"

# Override jit pref based on mode
if [ "$MODE" = "nojit" ]; then
  sed -i 's/^jit true/jit false/' "$SESS/prefs"
fi

# Start Xvfb if not running
DISPLAY_NUM=":22"
if ! xdpyinfo -display "$DISPLAY_NUM" >/dev/null 2>&1; then
  Xvfb "$DISPLAY_NUM" -screen 0 800x600x24 &>/dev/null &
  sleep 1
fi

echo "Starting BasiliskII ($MODE) ..."
echo "  Binary: $BIN"
echo "  Session: $SESS"
echo "  VNC: port 5999 (if --vnc)"

SDL_VIDEODRIVER=x11 DISPLAY="$DISPLAY_NUM" \
  HOME="$SESS/home" XDG_CONFIG_HOME="$SESS/xdg" \
  exec "$BIN" --config "$SESS/prefs" $EXTRA_ARGS
