#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="/workspace/projects/macemu"
UNIX_DIR="$PROJECT_ROOT/BasiliskII/src/Unix"
ROM_PATH="/workspace/fixtures/basilisk/images/Quadra800.ROM"
DISK_PATH="/workspace/fixtures/basilisk/images/HD200MB"

TS="$(date +%Y%m%d-%H%M%S)"
OUTDIR="/workspace/tmp/autoresearch-control-$TS"
mkdir -p "$OUTDIR"

build_ok=0
boot_alive=0
window_found=0
boot_progress=0
disk_activity=0
dump_activity=0
window_nonsolid=0
boot_steps_seen=0
png_dump_count=0
dump_signal_seen=0
dump_attempt_seen=0
dump_save_seen=0
crash_count=0
CRASH_PENALTY=20

emit_metrics() {
  local score
  score=$(( build_ok * 40 + boot_alive * 20 + window_found * 10 + boot_progress * 10 + disk_activity * 10 + dump_activity * 5 + window_nonsolid * 5 - crash_count * CRASH_PENALTY ))
  echo "METRIC control_boot_score=$score"
  echo "METRIC build_ok=$build_ok"
  echo "METRIC boot_alive=$boot_alive"
  echo "METRIC window_found=$window_found"
  echo "METRIC boot_progress=$boot_progress"
  echo "METRIC disk_activity=$disk_activity"
  echo "METRIC dump_activity=$dump_activity"
  echo "METRIC window_nonsolid=$window_nonsolid"
  echo "METRIC boot_steps_seen=$boot_steps_seen"
  echo "METRIC png_dump_count=$png_dump_count"
  echo "METRIC dump_signal_seen=$dump_signal_seen"
  echo "METRIC dump_attempt_seen=$dump_attempt_seen"
  echo "METRIC dump_save_seen=$dump_save_seen"
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

is_nonsolid_xwd() {
  local xwd_file="$1"
  local stats_file="$2"

  python3 - "$xwd_file" "$stats_file" <<'PY'
import struct
import sys
from pathlib import Path

xwd_path = Path(sys.argv[1])
stats_path = Path(sys.argv[2])

if not xwd_path.exists() or xwd_path.stat().st_size < 100:
    stats_path.write_text("error=missing_or_too_small\n")
    print(0)
    raise SystemExit

data = xwd_path.read_bytes()
hdr = struct.unpack('>25I', data[:100])
header_size = hdr[0]
width = hdr[4]
height = hdr[5]
byte_order = hdr[7]
bits_per_pixel = hdr[11]
bytes_per_line = hdr[12]
red_mask = hdr[14]
green_mask = hdr[15]
blue_mask = hdr[16]
ncolors = hdr[19]
bytes_per_pixel = max(1, (bits_per_pixel + 7) // 8)
pixels = data[header_size + ncolors * 12:]

if width <= 0 or height <= 0 or len(pixels) < bytes_per_line * height:
    stats_path.write_text(
        f"width={width}\nheight={height}\nerror=invalid_geometry_or_pixel_data\n"
    )
    print(0)
    raise SystemExit

x0 = width // 10
x1 = max(x0 + 1, width - width // 10)
y0 = height // 10
y1 = max(y0 + 1, height - height // 10)
step_x = max(1, (x1 - x0) // 80)
step_y = max(1, (y1 - y0) // 80)

def extract_component(pixel, mask):
    if mask == 0:
        return 0
    shift = (mask & -mask).bit_length() - 1
    value = (pixel & mask) >> shift
    maxv = mask >> shift
    if maxv <= 0:
        return 0
    return int(round(value * 255 / maxv))

unique = set()
y_min = 255
max_y = 0
nonblack = 0
sampled = 0
endian = 'little' if byte_order == 0 else 'big'

for y in range(y0, y1, step_y):
    row = pixels[y * bytes_per_line:(y + 1) * bytes_per_line]
    for x in range(x0, x1, step_x):
        off = x * bytes_per_pixel
        chunk = row[off:off + bytes_per_pixel]
        if len(chunk) < bytes_per_pixel:
            continue
        pixel = int.from_bytes(chunk[:min(4, len(chunk))], endian)
        r = extract_component(pixel, red_mask)
        g = extract_component(pixel, green_mask)
        b = extract_component(pixel, blue_mask)
        unique.add((r, g, b))
        lum = int(round((r + g + b) / 3))
        y_min = min(y_min, lum)
        max_y = max(max_y, lum)
        if lum > 3:
            nonblack += 1
        sampled += 1

if sampled == 0:
    stats_path.write_text("error=no_samples\n")
    print(0)
    raise SystemExit

y_range = max_y - y_min
nonblack_frac = nonblack / sampled
unique_colors = len(unique)
nonsolid = int(unique_colors >= 2 and (y_range >= 4 or nonblack_frac >= 0.02))

stats_path.write_text(
    f"width={width}\n"
    f"height={height}\n"
    f"bits_per_pixel={bits_per_pixel}\n"
    f"bytes_per_pixel={bytes_per_pixel}\n"
    f"sampled={sampled}\n"
    f"y_min={y_min}\n"
    f"y_max={max_y}\n"
    f"y_range={y_range}\n"
    f"nonblack_frac={nonblack_frac:.6f}\n"
    f"unique_colors={unique_colors}\n"
    f"nonsolid={nonsolid}\n"
)
print(nonsolid)
PY
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

run_control() {
  local run_dir="$OUTDIR/control"
  mkdir -p "$run_dir/home" "$run_dir/xdg"

  local display xvfb_pid emu_pid win_id="" survived=0 exit_code=0
  display="$(pick_display)"

  Xvfb "$display" -screen 0 1152x870x24 >"$run_dir/xvfb.log" 2>&1 &
  xvfb_pid=$!
  sleep 1

  cat >"$run_dir/prefs" <<EOF
rom $ROM_PATH
disk $DISK_PATH
bootdrive 0
bootdriver 0
ramsize 67108864
frameskip 0
modelid 14
cpu 4
fpu true
jit false
jitfpu false
screen win/640/480
displaycolordepth 8
sdlrender software
nosound true
nocdrom true
nogui false
EOF

  HOME="$run_dir/home" \
  XDG_CONFIG_HOME="$run_dir/xdg" \
  DISPLAY="$display" \
  B2_DUMP_DIR="$run_dir" \
  B2_DUMP_ON_VIDEO_INIT=1 \
  stdbuf -oL -eL "$UNIX_DIR/BasiliskII" --config "$run_dir/prefs" \
    >"$run_dir/basilisk.log" 2>&1 &
  emu_pid=$!

  local start_epoch now i
  local -a dump_times=(5 10 15 19)
  local next_dump_index=0
  : >"$run_dir/dump_signal_events.log"
  start_epoch="$(date +%s)"

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

  while true; do
    now="$(date +%s)"
    if (( now - start_epoch >= 20 )); then
      break
    fi
    if ! kill -0 "$emu_pid" 2>/dev/null; then
      break
    fi

    local elapsed=$(( now - start_epoch ))
    if (( next_dump_index < ${#dump_times[@]} )) && (( elapsed >= dump_times[next_dump_index] )); then
      if kill -USR2 "$emu_pid" 2>/dev/null; then
        printf 'elapsed=%s signal=USR2 status=sent\n' "$elapsed" >>"$run_dir/dump_signal_events.log"
      else
        printf 'elapsed=%s signal=USR2 status=failed\n' "$elapsed" >>"$run_dir/dump_signal_events.log"
      fi
      next_dump_index=$((next_dump_index + 1))
    fi
    sleep 1
  done

  DISPLAY="$display" xwininfo -root -tree >"$run_dir/xwin_tree_final.txt" 2>"$run_dir/xwininfo_final.err" || true
  if [[ -z "$win_id" ]]; then
    win_id="$(awk '/BasiliskII/ {print $1; exit}' "$run_dir/xwin_tree_final.txt")"
    if [[ -n "$win_id" ]]; then
      window_found=1
    fi
  fi

  DISPLAY="$display" xwd -silent -root -out "$run_dir/root.xwd" >"$run_dir/xwd_root.log" 2>&1 || true
  if (( window_found == 1 )); then
    echo "$win_id" >"$run_dir/window.id"
    DISPLAY="$display" xwd -silent -id "$win_id" -out "$run_dir/window.xwd" >"$run_dir/xwd_window.log" 2>&1 || true
  fi

  if kill -0 "$emu_pid" 2>/dev/null; then
    survived=1
    kill "$emu_pid" 2>/dev/null || true
    for _ in $(seq 1 30); do
      if ! kill -0 "$emu_pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$emu_pid" 2>/dev/null; then
      kill -KILL "$emu_pid" 2>/dev/null || true
    fi
  fi

  set +e
  wait "$emu_pid" 2>/dev/null
  exit_code=$?
  set -e

  kill "$xvfb_pid" 2>/dev/null || true
  wait "$xvfb_pid" 2>/dev/null || true

  if (( survived == 1 )); then
    boot_alive=1
  fi

  boot_steps_seen="$(rg -c '^BOOT ' "$run_dir/basilisk.log" || true)"
  if [[ -z "$boot_steps_seen" ]]; then
    boot_steps_seen=0
  fi
  if (( boot_steps_seen >= 6 )); then
    boot_progress=1
  fi

  if rg -q 'DiskOpen|disk inserted|mounting drive|HFS partition found|SCSI' "$run_dir/basilisk.log"; then
    disk_activity=1
  fi

  png_dump_count="$(find "$run_dir" -maxdepth 1 -name 'basiliskii-frame-*.png' | wc -l | awk '{print $1}')"
  if [[ -z "$png_dump_count" ]]; then
    png_dump_count=0
  fi
  if (( png_dump_count > 0 )); then
    dump_activity=1
  fi

  if rg -q 'BOOT dump: SIGUSR2 received' "$run_dir/basilisk.log"; then
    dump_signal_seen=1
  fi
  if rg -q 'BOOT dump: handling request' "$run_dir/basilisk.log"; then
    dump_attempt_seen=1
  fi
  if rg -q 'BOOT dump: saved ' "$run_dir/basilisk.log"; then
    dump_save_seen=1
  fi

  if [[ -f "$run_dir/window.xwd" ]]; then
    window_nonsolid="$(is_nonsolid_xwd "$run_dir/window.xwd" "$run_dir/window.analysis.txt")"
  fi
  if [[ -f "$run_dir/root.xwd" ]]; then
    is_nonsolid_xwd "$run_dir/root.xwd" "$run_dir/root.analysis.txt" >"$run_dir/root.nonsolid" || true
  fi

  if is_crash "$exit_code" "$run_dir/basilisk.log"; then
    crash_count=$((crash_count + 1))
  fi

  {
    echo "display=$display"
    echo "window_found=$window_found"
    echo "boot_alive=$boot_alive"
    echo "boot_progress=$boot_progress"
    echo "disk_activity=$disk_activity"
    echo "dump_activity=$dump_activity"
    echo "window_nonsolid=$window_nonsolid"
    echo "boot_steps_seen=$boot_steps_seen"
    echo "png_dump_count=$png_dump_count"
    echo "dump_signal_seen=$dump_signal_seen"
    echo "dump_attempt_seen=$dump_attempt_seen"
    echo "dump_save_seen=$dump_save_seen"
    echo "exit_code=$exit_code"
  } >"$run_dir/result.env"
}

for cmd in make Xvfb xwininfo xwd rg awk python3 stdbuf; do
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

if [[ ! -f "$ROM_PATH" || ! -f "$DISK_PATH" ]]; then
  echo "missing_assets rom=$ROM_PATH disk=$DISK_PATH" >"$OUTDIR/assets_missing.log"
  emit_metrics
  exit 0
fi

if (
  cd "$UNIX_DIR"
  make distclean >"$OUTDIR/make-distclean.log" 2>&1 || make clean >"$OUTDIR/make-clean.log" 2>&1 || true
  ac_cv_have_asm_extended_signals=yes ./configure \
    --enable-sdl-video \
    --disable-sdl-audio \
    --disable-vosf \
    --disable-jit-compiler \
    --disable-xf86-dga \
    --disable-xf86-vidmode \
    --disable-fbdev-dga \
    --without-mon \
    --without-esd \
    --without-gtk \
    --disable-nls \
    >"$OUTDIR/configure.log" 2>&1
  make -j"$(nproc)" >"$OUTDIR/make.log" 2>&1
); then
  build_ok=1
else
  build_ok=0
  emit_metrics
  exit 0
fi

run_control
emit_metrics
