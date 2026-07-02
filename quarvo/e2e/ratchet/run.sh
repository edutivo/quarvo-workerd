#!/usr/bin/env bash
# Drives the ratchet workload against a quarvo-workerd image in a memory-capped container and
# asserts: MODE=off -> usage ratchets up (bug reproduced); MODE=on -> usage stays bounded around
# the threshold (feature works). Samples the container's own cgroup memory.current.
set -euo pipefail

IMG="${1:?usage: run.sh <image> <on|off>}"
MODE="${2:?usage: run.sh <image> <on|off>}"
case "$MODE" in on|off) ;; *) echo "FATAL: MODE must be 'on' or 'off'"; exit 2;; esac

# OFF mode runs UNCAPPED and shorter: with a memory cap the ratchet would OOM-kill the
# container mid-run (that's the production bug!). Growth measurement doesn't need a cap.
# ON mode runs capped at 512 MiB to prove bounded operation under the real constraint.
if [ "$MODE" = "on" ]; then
  REQUESTS="${REQUESTS:-2500}"
else
  REQUESTS="${REQUESTS:-800}"
fi
SAMPLE_EVERY=100
MEM_LIMIT="${MEM_LIMIT:-512m}"
THRESHOLD_MB="${THRESHOLD_MB:-150}"
MIN_INTERVAL_MS="${MIN_INTERVAL_MS:-2000}"
# Assertion bounds (MiB). off: growth from warm baseline must exceed OFF_MIN_GROWTH (proves the
# ratchet). on: peak must stay under ON_MAX_PEAK (threshold + one interval of accumulation +
# slack; well under the 512 MiB cap).
OFF_MIN_GROWTH="${OFF_MIN_GROWTH:-80}"
ON_MAX_PEAK="${ON_MAX_PEAK:-260}"

DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="quarvo-ratchet-$MODE-$$"

RUN_ARGS=()
if [ "$MODE" = "on" ]; then
  RUN_ARGS+=(--memory="$MEM_LIMIT"
             -e QUARVO_GC_PRESSURE=on
             -e QUARVO_GC_PRESSURE_THRESHOLD_MB="$THRESHOLD_MB"
             -e QUARVO_GC_PRESSURE_MIN_INTERVAL_MS="$MIN_INTERVAL_MS")
fi

# Register cleanup BEFORE docker run so an early failure still cleans up; the container may not
# exist yet when the trap fires, so guard both commands. $? at trap entry is the exiting
# command's status: dump the container logs only on failure (quiet on success).
trap '{ rc=$?; if [ "$rc" -ne 0 ]; then docker logs "$NAME" 2>&1 | tail -50; fi; docker rm -f "$NAME" >/dev/null; } || true' EXIT

docker run -d --name "$NAME" -p 127.0.0.1:0:8080 \
  -v "$DIR:/app:ro" "${RUN_ARGS[@]}" \
  "$IMG" serve --experimental --verbose /app/config.capnp

PORT="$(docker inspect -f '{{(index (index .NetworkSettings.Ports "8080/tcp") 0).HostPort}}' "$NAME")"

# Wait for readiness (up to 30s).
for i in $(seq 1 60); do
  if curl -fsS --max-time 2 -o /dev/null "http://127.0.0.1:$PORT/"; then break; fi
  [ "$i" = 60 ] && { echo "FATAL: server never became ready"; exit 1; }
  sleep 0.5
done

sample_mib() { docker exec "$NAME" cat /sys/fs/cgroup/memory.current | awk '{printf "%d", $1/1048576}'; }

BASELINE="$(sample_mib)"
PEAK=0
echo "req,mem_mib"
for i in $(seq 1 "$REQUESTS"); do
  curl -fsS --max-time 30 -o /dev/null "http://127.0.0.1:$PORT/" || { echo "FATAL: request $i failed"; exit 1; }
  if [ $((i % SAMPLE_EVERY)) -eq 0 ]; then
    M="$(sample_mib)"
    echo "$i,$M"
    [ "$M" -gt "$PEAK" ] && PEAK="$M"
  fi
done
FINAL="$(sample_mib)"
[ "$FINAL" -gt "$PEAK" ] && PEAK="$FINAL"
GROWTH=$((FINAL - BASELINE))
# Off-mode asserts on PEAK growth, not FINAL growth: a late natural GC could deflate FINAL, but
# the peak preserves the ratchet evidence.
PEAK_GROWTH=$((PEAK - BASELINE))
echo "mode=$MODE baseline=${BASELINE}MiB final=${FINAL}MiB peak=${PEAK}MiB growth=${GROWTH}MiB peak_growth=${PEAK_GROWTH}MiB"

if [ "$MODE" = "off" ]; then
  [ "$PEAK_GROWTH" -ge "$OFF_MIN_GROWTH" ] || {
    echo "FAIL: expected the ratchet to grow >= ${OFF_MIN_GROWTH}MiB with the feature off"; exit 1; }
  echo "PASS: ratchet reproduced (feature off)"
else
  [ "$PEAK" -le "$ON_MAX_PEAK" ] || {
    echo "FAIL: peak ${PEAK}MiB exceeded ${ON_MAX_PEAK}MiB with the feature on"; exit 1; }
  echo "PASS: usage bounded (feature on)"
fi
