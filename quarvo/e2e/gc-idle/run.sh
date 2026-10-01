#!/usr/bin/env bash
# Regression for the reclaimer SIGSEGV (1.20260623.1-quarvo.4/.5): the reclaim thread was created
# in the Server constructor, BEFORE the V8 platform allocated its memory-protection key, so on
# PKU-capable CPUs its first forced GC died with SEGV_PKUERR. Runs a workerd binary with a 1 MB
# threshold (every tick is a reclaim round) against an IDLE single-worker config and asserts it
# survives several rounds. Needs a host with PKU to be meaningful (check `grep -c ospke
# /proc/cpuinfo`); on other hosts it passes vacuously — the unit test
# (quarvo-gc-pressure-test.c++) covers the ordering invariant everywhere.
#
# usage: run.sh /path/to/workerd
set -euo pipefail
BIN="${1:?usage: run.sh <workerd-binary>}"
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$(mktemp)"
trap 'kill "${PID:-}" 2>/dev/null || true; rm -f "$LOG"' EXIT

if grep -qw ospke /proc/cpuinfo 2>/dev/null; then echo "host has PKU: test is meaningful"; else echo "WARNING: host lacks PKU: passes vacuously"; fi

QUARVO_GC_PRESSURE=on QUARVO_GC_PRESSURE_THRESHOLD_MB=1 QUARVO_GC_PRESSURE_THRESHOLD_PCT=100 \
QUARVO_GC_PRESSURE_MIN_INTERVAL_MS=1000 "$BIN" serve --experimental "$DIR/config.capnp" >"$LOG" 2>&1 &
PID=$!

# ~10 reclaim rounds (1s tick, 1s min interval). The pre-fix binary died on the first one.
for i in $(seq 1 12); do
  sleep 1
  if ! kill -0 "$PID" 2>/dev/null; then echo "FAIL: workerd died after ~${i}s"; cat "$LOG"; exit 1; fi
done
curl -fsS --max-time 3 http://127.0.0.1:18080/ >/dev/null || { echo "FAIL: server not answering"; cat "$LOG"; exit 1; }
if grep -q "Segmentation fault" "$LOG"; then echo "FAIL: segfault in log"; cat "$LOG"; exit 1; fi
echo "PASS: survived 12s of reclaim rounds"
