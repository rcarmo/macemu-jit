#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="/workspace/projects/macemu"
UNIX_DIR="$PROJECT_ROOT/BasiliskII/src/Unix"
ASSET_REPO="/workspace/projects/rpi-basilisk2-sdl2-nox"
ASSET_DIR="/workspace/tmp/rpi-basilisk-assets"
ROM_PATH="$ASSET_REPO/Quadra800.ROM"
DISK_PATH="$ASSET_DIR/HD200MB"
ZIP_PATH="$ASSET_REPO/HD200MB-POP.zip"

TS="$(date +%Y%m%d-%H%M%S)"
OUTDIR="/workspace/tmp/autoresearch-jit-$TS"
mkdir -p "$OUTDIR"

build_ok=0
off_alive=0
off_nonsolid=0
on_alive=0
on_nonsolid=0
crash_count=0
CRASH_PENALTY=15

emit_metrics() {
  local score
  score=$(( build_ok * 30 + off_alive * 20 + off_nonsolid * 20 + on_alive * 20 + on_nonsolid * 10 - crash_count * CRASH_PENALTY ))
  echo "METRIC jit_smoke_score=$score"
  echo "METRIC build_ok=$build_ok"
  echo "METRIC off_alive=$off_alive"
  echo "METRIC off_nonsolid=$off_nonsolid"
  echo "METRIC on_alive=$on_alive"
  echo "METRIC on_nonsolid=$on_nonsolid"
  echo "METRIC crash_count=$crash_count"
  echo "ARTIFACT_DIR $OUTDIR"
}

pick_display() {
  local n
  for n in $(seq 99 180); do
    if [[ ! -e "/tmp/.X${n}-lock" && ! -S "/tmp/.X11-unix/X${n}" ]]; then
      echo ":$n"
      return 0
    fi
  done
  echo ":199"
}

is_nonsolid_png() {
  local png="$1"
  local stats_file="$2"
  local stats
  stats="$(ffmpeg -v info -i "$png" -vf signalstats,metadata=print -frames:v 1 -f null - 2>&1 || true)"
  printf '%s\n' "$stats" > "$stats_file"

  local ymin ymax satavg
  read -r ymin ymax satavg < <(
    printf '%s\n' "$stats" | awk -F= '
      /lavfi.signalstats.YMIN=/{ymin=$2}
      /lavfi.signalstats.YMAX=/{ymax=$2}
      /lavfi.signalstats.SATAVG=/{satavg=$2}
      END {
        if (ymin == "" || ymax == "" || satavg == "") {
          print "nan nan nan"
        } else {
          print ymin, ymax, satavg
        }
      }
    '
  )

  if [[ "$ymin" == "nan" ]]; then
    echo 0
    return 0
  fi

  awk -v ymin="$ymin" -v ymax="$ymax" -v satavg="$satavg" 'BEGIN {
    yrange = ymax - ymin;
    if (yrange >= 4.0 || satavg >= 1.0) {
      print 1;
    } else {
      print 0;
    }
  }'
}

is_crash() {
  local exit_code="$1"
  local log_file="$2"

  if [[ "$exit_code" -eq 134 || "$exit_code" -eq 139 ]]; then
    return 0
  fi

  if [[ "$exit_code" -ge 128 ]]; then
    local sig=$((exit_code - 128))
    if [[ "$sig" -eq 6 || "$sig" -eq 11 ]]; then
      return 0
    fi
  fi

  if [[ -f "$log_file" ]] && rg -q "Segmentation fault|SIGSEGV|Aborted|SIGABRT|Assertion .* failed|core dumped" "$log_file"; then
    return 0
  fi

  return 1
}

run_mode() {
  local jit_flag="$1"
  local prefix="$2"
  local run_dir="$OUTDIR/$prefix"
  mkdir -p "$run_dir"

  local display xvfb_pid emu_pid
  local window_found=0
  local win_id=""
  local survived=0
  local exit_code=0

  display="$(pick_display)"
  Xvfb "$display" -screen 0 1152x870x24 >"$run_dir/xvfb.log" 2>&1 &
  xvfb_pid=$!

  sleep 1

  DISPLAY="$display" "$UNIX_DIR/BasiliskII" \
    --config "$run_dir/prefs" \
    --rom "$ROM_PATH" \
    --disk "$DISK_PATH" \
    --screen "win/800/600" \
    --ramsize 134217728 \
    --frameskip 1 \
    --nosound true \
    --nocdrom true \
    --noclipconversion true \
    --nogui true \
    --jit "$jit_flag" \
    --jitfpu false \
    --jitcachesize 8192 \
    >"$run_dir/basilisk.log" 2>&1 &
  emu_pid=$!

  local start_epoch now
  start_epoch="$(date +%s)"

  local i
  for i in $(seq 1 20); do
    if ! kill -0 "$emu_pid" 2>/dev/null; then
      break
    fi

    DISPLAY="$display" xwininfo -root -tree >"$run_dir/xwin_tree.txt" 2>"$run_dir/xwininfo.err" || true
    win_id="$(awk '/BasiliskII/ {print $1; exit}' "$run_dir/xwin_tree.txt")"
    if [[ -n "$win_id" ]]; then
      window_found=1
      break
    fi

    sleep 0.5
  done

  DISPLAY="$display" xwd -silent -root -out "$run_dir/root.xwd" >"$run_dir/xwd_root.log" 2>&1 || true
  if [[ -s "$run_dir/root.xwd" ]]; then
    ffmpeg -v error -y -f xwd -i "$run_dir/root.xwd" "$run_dir/root.png" >"$run_dir/ffmpeg_root.log" 2>&1 || true
  fi

  if (( window_found == 1 )); then
    echo "$win_id" > "$run_dir/window.id"
    DISPLAY="$display" xwd -silent -id "$win_id" -out "$run_dir/window.xwd" >"$run_dir/xwd_window.log" 2>&1 || true
    if [[ -s "$run_dir/window.xwd" ]]; then
      ffmpeg -v error -y -f xwd -i "$run_dir/window.xwd" "$run_dir/window.png" >"$run_dir/ffmpeg_window.log" 2>&1 || true
    fi
  fi

  while true; do
    now="$(date +%s)"
    if (( now - start_epoch >= 20 )); then
      break
    fi
    if ! kill -0 "$emu_pid" 2>/dev/null; then
      break
    fi
    sleep 1
  done

  if kill -0 "$emu_pid" 2>/dev/null; then
    survived=1
    kill "$emu_pid" 2>/dev/null || true
    set +e
    wait "$emu_pid" 2>/dev/null
    exit_code=$?
    set -e
  else
    set +e
    wait "$emu_pid" 2>/dev/null
    exit_code=$?
    set -e
  fi

  kill "$xvfb_pid" 2>/dev/null || true
  wait "$xvfb_pid" 2>/dev/null || true

  local alive=0
  local nonsolid=0

  if (( survived == 1 && window_found == 1 )); then
    alive=1
  fi

  if [[ -f "$run_dir/window.png" ]]; then
    nonsolid="$(is_nonsolid_png "$run_dir/window.png" "$run_dir/signalstats.txt")"
  fi

  if is_crash "$exit_code" "$run_dir/basilisk.log"; then
    crash_count=$((crash_count + 1))
  fi

  {
    echo "jit=$jit_flag"
    echo "display=$display"
    echo "window_found=$window_found"
    echo "survived_20s=$survived"
    echo "exit_code=$exit_code"
    echo "alive=$alive"
    echo "nonsolid=$nonsolid"
  } >"$run_dir/result.env"

  if [[ "$prefix" == "jit_off" ]]; then
    off_alive="$alive"
    off_nonsolid="$nonsolid"
  else
    on_alive="$alive"
    on_nonsolid="$nonsolid"
  fi
}

# Fast precheck
for cmd in git make Xvfb xwininfo xwd ffmpeg unzip rg awk; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "missing_command=$cmd" >"$OUTDIR/precheck.log"
    emit_metrics
    exit 0
  fi
done

if [[ ! -d "$UNIX_DIR" ]]; then
  echo "missing_dir=$UNIX_DIR" >"$OUTDIR/precheck.log"
  emit_metrics
  exit 0
fi

# Build with JIT enabled (opt-in path)
if (
  cd "$UNIX_DIR"
  ac_cv_have_asm_extended_signals=yes ./configure --enable-aarch64-jit-experimental >"$OUTDIR/configure.log" 2>&1
  make -j"$(nproc)" >"$OUTDIR/make.log" 2>&1
); then
  build_ok=1
else
  build_ok=0
  emit_metrics
  exit 0
fi

# Assets setup (ROM + disk)
if [[ ! -d "$ASSET_REPO/.git" ]]; then
  git clone https://github.com/ekbann/rpi-basilisk2-sdl2-nox.git "$ASSET_REPO" >"$OUTDIR/assets_clone.log" 2>&1 || true
fi

mkdir -p "$ASSET_DIR"
if [[ ! -f "$DISK_PATH" && -f "$ZIP_PATH" ]]; then
  unzip -o "$ZIP_PATH" -d "$ASSET_DIR" >"$OUTDIR/assets_unzip.log" 2>&1 || true
fi

if [[ ! -f "$ROM_PATH" || ! -f "$DISK_PATH" ]]; then
  echo "missing_assets rom=$ROM_PATH disk=$DISK_PATH" >"$OUTDIR/assets_missing.log"
  emit_metrics
  exit 0
fi

# Smoke runs
run_mode false jit_off
run_mode true jit_on

emit_metrics
