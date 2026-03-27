#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$ROOT_DIR/BasiliskII"
UNIX_DIR="$PROJECT_ROOT/src/Unix"
ROM_PATH="/workspace/fixtures/basilisk/images/Quadra800.ROM"
DISK_PATH="/workspace/fixtures/basilisk/images/HD200MB"
CORE_MODE="${B2_CPU_CORE_MODE:-uae_cpu}"
CORE_CONFIG_ARG=""
JIT_PREF="${B2_JIT_PREF:-false}"
AARCH64_JIT_EXPERIMENTAL="${B2_ENABLE_AARCH64_JIT_EXPERIMENTAL:-false}"

TS="$(date +%Y%m%d-%H%M%S)"
OUTDIR="/workspace/tmp/autoresearch-boot-divergence-$TS"
RUN_DIR="$OUTDIR/run"
mkdir -p "$RUN_DIR/home" "$RUN_DIR/xdg"

build_ok=0
boot_alive=0
reset_seen=0
clknomem_calls=0
intmask_transition_count=0
patch_boot_globs_seen=0
checkload_seen=0
video_interrupt_seen=0
framebuffer_write_seen=0
screenshot_count=0
core_is_original=0
core_is_2021=0
reserved_assert=0
popall_alloc_fail=0
block_pool_fail=0
jit_high_addr_warn=0
mac_ram_low32=0
jit_code_low32=0

xvfb_pid=""
emu_pid=""

terminate_pid() {
  local pid="$1"
  [[ -z "$pid" ]] && return 0
  if kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.1
    done
    if kill -0 "$pid" 2>/dev/null; then
      kill -KILL "$pid" 2>/dev/null || true
    fi
  fi
}

cleanup() {
  terminate_pid "$emu_pid"
  terminate_pid "$xvfb_pid"
}
trap cleanup EXIT

emit_metrics() {
  local stage_coverage_score=0
  stage_coverage_score=$((stage_coverage_score + reset_seen * 15))
  stage_coverage_score=$((stage_coverage_score + (clknomem_calls > 0 ? 15 : 0)))
  stage_coverage_score=$((stage_coverage_score + (intmask_transition_count > 0 ? 15 : 0)))
  stage_coverage_score=$((stage_coverage_score + patch_boot_globs_seen * 15))
  stage_coverage_score=$((stage_coverage_score + checkload_seen * 15))
  stage_coverage_score=$((stage_coverage_score + video_interrupt_seen * 15))
  stage_coverage_score=$((stage_coverage_score + framebuffer_write_seen * 10))

  local jit_alive_sec=$((boot_alive * 20))

  echo "METRIC jit_alive_sec=$jit_alive_sec"
  echo "METRIC stage_coverage_score=$stage_coverage_score"
  echo "METRIC build_ok=$build_ok"
  echo "METRIC boot_alive=$boot_alive"
  echo "METRIC reset_seen=$reset_seen"
  echo "METRIC clknomem_calls=$clknomem_calls"
  echo "METRIC intmask_transition_count=$intmask_transition_count"
  echo "METRIC patch_boot_globs_seen=$patch_boot_globs_seen"
  echo "METRIC checkload_seen=$checkload_seen"
  echo "METRIC video_interrupt_seen=$video_interrupt_seen"
  echo "METRIC framebuffer_write_seen=$framebuffer_write_seen"
  echo "METRIC screenshot_count=$screenshot_count"
  echo "METRIC core_is_original=$core_is_original"
  echo "METRIC core_is_2021=$core_is_2021"
  echo "METRIC reserved_assert=$reserved_assert"
  echo "METRIC popall_alloc_fail=$popall_alloc_fail"
  echo "METRIC block_pool_fail=$block_pool_fail"
  echo "METRIC jit_high_addr_warn=$jit_high_addr_warn"
  echo "METRIC mac_ram_low32=$mac_ram_low32"
  echo "METRIC jit_code_low32=$jit_code_low32"
  echo "ARTIFACT_DIR $OUTDIR"
}

pick_display() {
  local n
  for n in $(seq 99 199); do
    if [[ ! -e "/tmp/.X${n}-lock" && ! -S "/tmp/.X11-unix/X${n}" ]]; then
      echo ":$n"
      return 0
    fi
  done
  echo ":199"
}

build_reference_configure() {
  (
    cd "$UNIX_DIR"
    make distclean >"$OUTDIR/make-distclean.log" 2>&1 || make clean >"$OUTDIR/make-clean.log" 2>&1 || true

    if [[ ! -x "./configure" || "./configure.ac" -nt "./configure" ]]; then
      NO_CONFIGURE=1 ./autogen.sh >"$OUTDIR/autogen.log" 2>&1
    fi

    configure_basilisk() {
      local framework_flag="$1"
      local log_file="$2"
      local jit_flag="--disable-jit-compiler"
      if [[ "$AARCH64_JIT_EXPERIMENTAL" == "true" ]]; then
        jit_flag="--enable-aarch64-jit-experimental"
      fi
      ./configure \
        --enable-sdl-audio \
        "$framework_flag" \
        --enable-sdl-video \
        --disable-vosf \
        --without-mon \
        --without-esd \
        --without-gtk \
        "$jit_flag" \
        "$CORE_CONFIG_ARG" \
        >"$log_file" 2>&1
    }

    configure_basilisk --enable-sdl-framework "$OUTDIR/configure.log"

    if ! make -j"$(nproc)" >"$OUTDIR/make.log" 2>&1; then
      if rg -q 'cc1obj|SDLMain\.m' "$OUTDIR/make.log"; then
        echo "fallback_disable_sdl_framework=1" >"$OUTDIR/build_fallback.env"
        make distclean >"$OUTDIR/make-distclean-fallback.log" 2>&1 || make clean >"$OUTDIR/make-clean-fallback.log" 2>&1 || true
        configure_basilisk --disable-sdl-framework "$OUTDIR/configure-fallback.log"
        make -j"$(nproc)" >"$OUTDIR/make-fallback.log" 2>&1
      else
        return 1
      fi
    fi
  )
}

run_under_xvfb() {
  local display
  local emu_wait_rc=0
  display="$(pick_display)"

  Xvfb "$display" -screen 0 1152x870x24 >"$RUN_DIR/xvfb.log" 2>&1 &
  xvfb_pid=$!
  sleep 1

  cat >"$RUN_DIR/prefs" <<EOF
rom $ROM_PATH
disk $DISK_PATH
bootdrive 0
bootdriver 0
ramsize 67108864
frameskip 0
modelid 14
cpu 4
fpu true
jit $JIT_PREF
jitfpu false
screen win/640/480
displaycolordepth 8
sdlrender software
nosound true
nocdrom true
nogui false
EOF

  HOME="$RUN_DIR/home" \
  XDG_CONFIG_HOME="$RUN_DIR/xdg" \
  DISPLAY="$display" \
  stdbuf -oL -eL "$UNIX_DIR/BasiliskII" --config "$RUN_DIR/prefs" \
    >"$RUN_DIR/basilisk.log" 2>&1 &
  emu_pid=$!

  local start now
  start="$(date +%s)"
  while true; do
    now="$(date +%s)"
    if (( now - start >= 20 )); then
      boot_alive=1
      break
    fi
    if ! kill -0 "$emu_pid" 2>/dev/null; then
      break
    fi
    sleep 1
  done

  # Dismiss any startup dialog (e.g. "improper shutdown") by sending Return.
  if kill -0 "$emu_pid" 2>/dev/null; then
    DISPLAY="$display" xdotool key Return 2>/dev/null || true
    sleep 3
    DISPLAY="$display" xdotool key Return 2>/dev/null || true
    sleep 5
  fi

  DISPLAY="$display" xwininfo -root -tree >"$RUN_DIR/xwin_tree.txt" 2>"$RUN_DIR/xwin_tree.err" || true
  DISPLAY="$display" xwd -silent -root -out "$RUN_DIR/root.xwd" >"$RUN_DIR/root_xwd.log" 2>&1 || true

  local win_id=""
  win_id="$(awk '/BasiliskII/ {print $1; exit}' "$RUN_DIR/xwin_tree.txt" || true)"
  if [[ -n "$win_id" ]]; then
    DISPLAY="$display" xwd -silent -id "$win_id" -out "$RUN_DIR/window.xwd" >"$RUN_DIR/window_xwd.log" 2>&1 || true
  fi

  screenshot_count="$(find "$RUN_DIR" -maxdepth 1 -name '*.xwd' | wc -l | awk '{print $1}')"

  terminate_pid "$emu_pid"

  set +e
  wait "$emu_pid" 2>/dev/null
  emu_wait_rc=$?
  set -e
  emu_pid=""

  local term_sig=$((emu_wait_rc - 128))
  if (( term_sig == 6 || term_sig == 11 )); then
    {
      echo "wait_rc=$emu_wait_rc"
      echo "signal=$term_sig"
      date -u '+utc=%Y-%m-%dT%H:%M:%SZ'
    } >"$RUN_DIR/crash.meta"
    tail -n 200 "$RUN_DIR/basilisk.log" >"$RUN_DIR/crash.log.tail" || true
  fi

  terminate_pid "$xvfb_pid"
  wait "$xvfb_pid" 2>/dev/null || true
  xvfb_pid=""
}

extract_milestones() {
  local log="$RUN_DIR/basilisk.log"

  if rg -q 'BOOT_STAGE RESET fired' "$log"; then
    reset_seen=1
  fi

  local last_clk
  last_clk="$(rg -o 'BOOT_STAGE CLKNOMEM count=[0-9]+' "$log" | awk -F= 'END{print $2}' || true)"
  if [[ -n "$last_clk" ]]; then
    clknomem_calls="$last_clk"
  else
    clknomem_calls="$(rg -c 'BOOT_STAGE CLKNOMEM' "$log" || true)"
    clknomem_calls="${clknomem_calls:-0}"
  fi

  intmask_transition_count="$(rg -c 'BOOT_STAGE INTMASK transition' "$log" || true)"
  intmask_transition_count="${intmask_transition_count:-0}"

  if rg -q 'BOOT_STAGE PATCH_BOOT_GLOBS reached' "$log"; then
    patch_boot_globs_seen=1
  fi

  if rg -q 'BOOT_STAGE CHECKLOAD reached' "$log"; then
    checkload_seen=1
  fi

  if rg -q 'BOOT_STAGE VIDEOINT first' "$log"; then
    video_interrupt_seen=1
  fi

  if rg -q 'BOOT_STAGE FRAMEBUFFER first_write' "$log"; then
    framebuffer_write_seen=1
  fi

  reserved_assert="$(rg -c 'reserved_buf && size <= RESERVED_SIZE' "$log" || true)"
  reserved_assert="${reserved_assert:-0}"
  popall_alloc_fail="$(rg -c 'Could not allocate popallspace' "$log" || true)"
  popall_alloc_fail="${popall_alloc_fail:-0}"
  block_pool_fail="$(rg -c 'Could not allocate block pool' "$log" || true)"
  block_pool_fail="${block_pool_fail:-0}"
  jit_high_addr_warn="$(rg -c 'allocated above 32-bit boundary' "$log" || true)"
  jit_high_addr_warn="${jit_high_addr_warn:-0}"

  local ram_addr_hex=""
  ram_addr_hex="$(rg -o 'Mac RAM starts at 0x[0-9a-fA-F]+' "$log" | awk 'NR==1 {print $5; exit}' || true)"
  ram_addr_hex="${ram_addr_hex#0x}"
  if [[ -n "$ram_addr_hex" && ${#ram_addr_hex} -le 8 ]]; then
    mac_ram_low32=1
  fi

  local jit_code_addr_hex=""
  jit_code_addr_hex="$(rg -o 'allocation at 0x[0-9a-fA-F]+' "$log" | awk 'NR==1 {print $3; exit}' || true)"
  jit_code_addr_hex="${jit_code_addr_hex#0x}"
  if [[ -n "$jit_code_addr_hex" && ${#jit_code_addr_hex} -le 8 ]]; then
    jit_code_low32=1
  fi
}

for cmd in make Xvfb xwininfo xwd awk rg stdbuf; do
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

case "$CORE_MODE" in
  uae_cpu)
    core_is_original=1
    CORE_CONFIG_ARG="--with-uae-core=legacy"
    ;;
  uae_cpu_2021)
    core_is_2021=1
    CORE_CONFIG_ARG="--with-uae-core=2021"
    ;;
  *)
    echo "invalid_core_mode=$CORE_MODE" >"$OUTDIR/invalid_core_mode.log"
    emit_metrics
    exit 0
    ;;
esac

if build_reference_configure; then
  build_ok=1
else
  build_ok=0
  emit_metrics
  exit 0
fi

run_under_xvfb
extract_milestones
emit_metrics
