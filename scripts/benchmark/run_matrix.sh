#!/bin/bash
#
# run_matrix.sh: reproducible benchmark matrix for ROS 2 logging backends
# (design.md, section 8). Compares rcl_logging_spdlog (the ROS 2 default) and
# rcl_logging_journal with the same driver (bench_backend) and writes JSON
# lines into a results directory that report.py turns into markdown tables.
# Other backends (e.g. rcl_logging_syslog) benchmark themselves in their own
# repository; the driver is generic, so BACKENDS can name any backend library.
#
#   B1  client call latency (p50/p95/p99, calls/sec) per size and severity,
#       sustained (as fast as possible, throughput bound) and in bursts
#       (B1_BURST calls every B1_GAP_MS ms, the shape of a real node)
#   B2  system CPU per record: app + journald CPU time
#   B3  sustained throughput ramp: delivered vs. sent, journald suppression
#   B4  storage footprint on disk for an identical workload
#   B5  query time: journalctl field match vs. grep over text logs
#
# Prerequisites
#   - source a workspace where all backends to compare are built
#     (librcl_logging_spdlog.so / librcl_logging_journal.so in LD_LIBRARY_PATH)
#   - systemd-journald running
#   - run as root (or a user allowed to read the journal and daemon CPU stats)
#
# Environment knobs (defaults in parentheses)
#   BACKENDS   ("rcl_logging_spdlog rcl_logging_journal")
#   COUNT      (1000000) records per sustained B1 run
#   B1_BURST   (100) calls per burst in the burst B1 run, 0 disables it
#   B1_GAP_MS  (10) pause between bursts in ms
#   B1_BURSTS  (500) number of bursts per burst B1 run
#   SIZES      ("64 256 4096") message sizes in bytes
#   SEVERITIES ("INFO FATAL")
#   B2_COUNT   (100000) records for the CPU accounting run
#   B3_RATES   ("1000 5000 20000 50000 100000 200000") msg/s ramp, 5 s each
#   B4_COUNT   (1000000) records for the storage footprint run
#   RESULTS    (scripts/benchmark/results/<timestamp>)
#   SKIP       space separated list of benchmark ids to skip, e.g. "B3 B5"
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKENDS="${BACKENDS:-rcl_logging_spdlog rcl_logging_journal}"
COUNT="${COUNT:-1000000}"
B1_BURST="${B1_BURST:-100}"
B1_GAP_MS="${B1_GAP_MS:-10}"
B1_BURSTS="${B1_BURSTS:-500}"
SIZES="${SIZES:-64 256 4096}"
SEVERITIES="${SEVERITIES:-INFO FATAL}"
B2_COUNT="${B2_COUNT:-100000}"
B3_RATES="${B3_RATES:-1000 5000 20000 50000 100000 200000}"
B4_COUNT="${B4_COUNT:-1000000}"
SKIP="${SKIP:-}"
STAMP="$(date +%Y%m%d_%H%M%S)"
RESULTS="${RESULTS:-${HERE}/results/${STAMP}}"
BUILD_DIR="${HERE}/build"
BENCH="${BUILD_DIR}/bench_backend"

mkdir -p "${RESULTS}"
log() { echo "[run_matrix] $*" >&2; }
skipped() { [[ " ${SKIP} " == *" $1 "* ]]; }

# ---------------------------------------------------------------- environment
{
  echo "date: $(date -Is)"
  echo "host: $(uname -a)"
  echo "systemd: $(journalctl --version 2>/dev/null | head -1 || echo n/a)"
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | xargs || true)"
  echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "ros_distro: ${ROS_DISTRO:-n/a}"
  echo "backends: ${BACKENDS}"
  echo "journald_conf:"
  systemd-analyze cat-config systemd/journald.conf 2>/dev/null | grep -vE '^\s*(#|$)' | sed 's/^/  /' || true
} > "${RESULTS}/environment.txt"
log "results in ${RESULTS}"

# ---------------------------------------------------------------- build driver
# Always configure and build: the build is incremental and takes well under a
# second when nothing changed, and a stale binary would silently skew results.
log "building bench_backend"
cmake -S "${HERE}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release > /dev/null
cmake --build "${BUILD_DIR}" > /dev/null

# ---------------------------------------------------------------- preflight
# systemd-journald must already be running: the harness measures it, it does
# not start it.
preflight_failed=0
for backend in ${BACKENDS}; do
  if ! ldconfig -p 2>/dev/null | grep -q "lib${backend}.so" && \
     ! (IFS=:; for d in ${LD_LIBRARY_PATH:-}; do [ -e "$d/lib${backend}.so" ] && exit 0; done; exit 1); then
    log "ERROR: lib${backend}.so not found in LD_LIBRARY_PATH; source the workspace first"
    preflight_failed=1
  fi
  if [ "${backend}" = "rcl_logging_journal" ]; then
    if [ -z "$(pidof systemd-journald || true)" ] || [ ! -S /run/systemd/journal/socket ]; then
      log "ERROR: systemd-journald is not running (needed by rcl_logging_journal)"
      preflight_failed=1
    fi
  fi
done
if [ "${preflight_failed}" -ne 0 ]; then
  log "preflight failed, aborting (drop a backend from BACKENDS to skip it)"
  exit 1
fi

# ---------------------------------------------------------------- helpers
# CPU ticks (user+system) of a process from /proc, in clock ticks.
proc_ticks() {
  local pid="$1"
  [ -r "/proc/${pid}/stat" ] || { echo 0; return; }
  awk '{print $14 + $15}' "/proc/${pid}/stat"
}
CLK_TCK="$(getconf CLK_TCK)"
daemon_pids() {
  case "$1" in
    rcl_logging_journal) pidof systemd-journald || true ;;
    *) ;;
  esac
}
sum_ticks() { local total=0; for p in "$@"; do total=$(( total + $(proc_ticks "$p") )); done; echo "${total}"; }

# Where the workload of a backend lands on disk, for B4 and B5.
spdlog_dir() { echo "${ROS_LOG_DIR:-${ROS_HOME:-$HOME/.ros}/log}"; }
disk_bytes() {
  case "$1" in
    rcl_logging_journal) du -sb /var/log/journal /run/log/journal 2>/dev/null | awk '{s+=$1} END {print s+0}' ;;
    rcl_logging_spdlog) du -sb "$(spdlog_dir)" 2>/dev/null | awk '{print $1+0}' ;;
  esac
}
delivered_records() {
  # $1 backend, $2 logger name token. grep -c exits 1 on zero matches.
  case "$1" in
    rcl_logging_journal) { journalctl -q -o cat "ROS2_NODE_NAME=$2" 2>/dev/null || true; } | wc -l ;;
    rcl_logging_spdlog) { cat "$(spdlog_dir)"/bench_backend_*.log 2>/dev/null || true; } | { grep -c "\[$2\]" || true; } ;;
  esac
}
journald_suppressed_since() {
  # grep exits 1 when nothing was suppressed; that is the normal case.
  { journalctl -q -o cat SYSLOG_IDENTIFIER=systemd-journald --since "$1" 2>/dev/null || true; } | \
    { grep -oE 'Suppressed [0-9]+ messages' || true; } | awk '{s+=$2} END {print s+0}'
}

# ---------------------------------------------------------------- B1 latency
if ! skipped B1; then
  log "B1 client call latency"
  : > "${RESULTS}/b1.jsonl"
  for backend in ${BACKENDS}; do
    for size in ${SIZES}; do
      for severity in ${SEVERITIES}; do
        log "  ${backend} size=${size} severity=${severity} count=${COUNT}"
        "${BENCH}" --backend "${backend}" --count "${COUNT}" --size "${size}" \
          --severity "${severity}" --logger-name "b1_${STAMP}" >> "${RESULTS}/b1.jsonl"
      done
    done
  done
  if [ "${B1_BURST}" -gt 0 ]; then
    for backend in ${BACKENDS}; do
      for size in ${SIZES}; do
        log "  ${backend} size=${size} burst=${B1_BURST} gap=${B1_GAP_MS}ms x${B1_BURSTS}"
        "${BENCH}" --backend "${backend}" --count "$(( B1_BURST * B1_BURSTS ))" --size "${size}" \
          --severity INFO --burst "${B1_BURST}" --gap-ms "${B1_GAP_MS}" \
          --logger-name "b1_${STAMP}" >> "${RESULTS}/b1.jsonl"
      done
    done
  fi
fi

# ---------------------------------------------------------------- B2 system CPU
if ! skipped B2; then
  log "B2 system CPU per record"
  : > "${RESULTS}/b2.jsonl"
  for backend in ${BACKENDS}; do
    pids="$(daemon_pids "${backend}")"
    before="$(sum_ticks ${pids})"
    out="$("${BENCH}" --backend "${backend}" --count "${B2_COUNT}" --size 256 \
      --severity INFO --logger-name "b2_${STAMP}")"
    sleep 2  # let the daemons drain their queues
    after="$(sum_ticks ${pids})"
    daemon_sec="$(awk -v a="${after}" -v b="${before}" -v t="${CLK_TCK}" 'BEGIN {printf "%.6f", (a-b)/t}')"
    echo "${out}" | sed "s/}$/,\"daemon_cpu_sec\":${daemon_sec},\"daemon_pids\":\"${pids// /,}\"}/" >> "${RESULTS}/b2.jsonl"
  done
fi

# ---------------------------------------------------------------- B3 throughput
if ! skipped B3; then
  log "B3 sustained throughput ramp"
  : > "${RESULTS}/b3.jsonl"
  for backend in ${BACKENDS}; do
    for rate in ${B3_RATES}; do
      token="b3_${STAMP}_${backend}_${rate}"
      count=$(( rate * 5 ))
      since="$(date '+%Y-%m-%d %H:%M:%S')"
      log "  ${backend} rate=${rate}/s for 5 s"
      out="$("${BENCH}" --backend "${backend}" --count "${count}" --size 256 \
        --severity INFO --rate "${rate}" --logger-name "${token}")"
      sleep 3
      delivered="$(delivered_records "${backend}" "${token}")"
      suppressed=0
      [ "${backend}" != "rcl_logging_spdlog" ] && suppressed="$(journald_suppressed_since "${since}")"
      echo "${out}" | sed "s/}$/,\"delivered\":${delivered},\"suppressed\":${suppressed}}/" >> "${RESULTS}/b3.jsonl"
    done
  done
fi

# ---------------------------------------------------------------- B4 storage
if ! skipped B4; then
  log "B4 storage footprint"
  : > "${RESULTS}/b4.jsonl"
  for backend in ${BACKENDS}; do
    [ "${backend}" = "rcl_logging_journal" ] && journalctl --sync 2>/dev/null || true
    before="$(disk_bytes "${backend}")"
    # realistic mix: mostly short INFO lines, some medium, a few large
    for spec in "64 INFO $(( B4_COUNT * 70 / 100 ))" "256 INFO $(( B4_COUNT * 25 / 100 ))" "4096 WARN $(( B4_COUNT * 5 / 100 ))"; do
      set -- ${spec}
      "${BENCH}" --backend "${backend}" --count "$3" --size "$1" --severity "$2" \
        --logger-name "b4_${STAMP}" > /dev/null
    done
    sleep 3
    [ "${backend}" = "rcl_logging_journal" ] && journalctl --sync 2>/dev/null || true
    after="$(disk_bytes "${backend}")"
    echo "{\"backend\":\"${backend}\",\"records\":${B4_COUNT},\"bytes_before\":${before:-0},\"bytes_after\":${after:-0},\"bytes_delta\":$(( ${after:-0} - ${before:-0} ))}" >> "${RESULTS}/b4.jsonl"
  done
fi

# ---------------------------------------------------------------- B5 query
if ! skipped B5; then
  log "B5 query performance (WARN+ for one logger)"
  : > "${RESULTS}/b5.jsonl"
  time_ms() { local s e; s=$(date +%s%N); "$@" > /dev/null 2>&1 || true; e=$(date +%s%N); echo $(( (e - s) / 1000000 )); }
  for backend in ${BACKENDS}; do
    token="b4_${STAMP}"
    case "${backend}" in
      rcl_logging_journal)
        cold="$(time_ms journalctl -q -p warning "ROS2_NODE_NAME=${token}" -o cat)"
        warm="$(time_ms journalctl -q -p warning "ROS2_NODE_NAME=${token}" -o cat)" ;;
      rcl_logging_spdlog)
        cold="$(time_ms grep -rE "\[(WARN|ERROR|FATAL)\] .*\[${token}\]" "$(spdlog_dir)")"
        warm="$(time_ms grep -rE "\[(WARN|ERROR|FATAL)\] .*\[${token}\]" "$(spdlog_dir)")" ;;
    esac
    echo "{\"backend\":\"${backend}\",\"query\":\"WARN+ for logger ${token}\",\"first_ms\":${cold},\"second_ms\":${warm}}" >> "${RESULTS}/b5.jsonl"
  done
fi

# ---------------------------------------------------------------- report
if command -v python3 > /dev/null; then
  python3 "${HERE}/report.py" "${RESULTS}" > "${RESULTS}/REPORT.md"
  log "report written to ${RESULTS}/REPORT.md"
  cat "${RESULTS}/REPORT.md"
fi
