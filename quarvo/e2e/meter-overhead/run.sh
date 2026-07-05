#!/usr/bin/env bash
# quarvo meter-overhead e2e: drives N requests through a loader-loaded echo quant and reports
# throughput with QUARVO_RUNTIME_METERING off vs on. Also asserts the boot config banner
# contract (spec §6.1): the "quarvo-workerd: " line always prints, and shows metering=on|off.
#
# The published FEATURES.md overhead number comes from a quiet host; the CI bound
# (MAX_REGRESSION_PCT, asserted by the compare step in quarvo-image-test.yml or run manually)
# is a loose regression tripwire because shared runners are noisy.
#
# Usage: run.sh <image> <off|on>
# Writes req/s to /tmp/meter-overhead-rps-<mode> for the off/on compare step.
set -euo pipefail

IMG="${1:?usage: run.sh <image> <off|on>}"
MODE="${2:?usage: run.sh <image> <off|on>}"
case "$MODE" in on|off) ;; *) echo "FATAL: MODE must be 'on' or 'off'"; exit 2;; esac

N="${N:-3000}"
CONCURRENCY="${CONCURRENCY:-8}"
DIR="$(cd "$(dirname "$0")" && pwd)"
NAME="quarvo-meter-overhead-$MODE-$$"

RUN_ARGS=()
if [ "$MODE" = "on" ]; then
  RUN_ARGS+=(-e QUARVO_RUNTIME_METERING=on)
fi

trap '{ rc=$?; if [ "$rc" -ne 0 ]; then docker logs "$NAME" 2>&1 | tail -50; fi; docker rm -f "$NAME" >/dev/null; } || true' EXIT

docker run -d --name "$NAME" -p 127.0.0.1:0:8080 \
  -v "$DIR:/app:ro" "${RUN_ARGS[@]}" \
  "$IMG" serve --experimental /app/config.capnp

PORT="$(docker inspect -f '{{(index (index .NetworkSettings.Ports "8080/tcp") 0).HostPort}}' "$NAME")"

# Wait for readiness (up to 30s).
for i in $(seq 1 60); do
  if curl -fsS --max-time 2 -o /dev/null "http://127.0.0.1:$PORT/"; then break; fi
  [ "$i" = 60 ] && { echo "FATAL: server never became ready"; exit 1; }
  sleep 0.5
done

# --- Boot banner contract (spec §6.1): always present, reflects resolved metering state. ---
docker logs "$NAME" 2>&1 | grep -F "quarvo-workerd: " >/dev/null || {
  echo "FAIL: boot banner ('quarvo-workerd: ' line) missing from container log"; exit 1; }
docker logs "$NAME" 2>&1 | grep -F "quarvo-workerd: " | grep -q "metering=$MODE" || {
  echo "FAIL: boot banner does not show metering=$MODE"; exit 1; }
echo "banner: $(docker logs "$NAME" 2>&1 | grep -F 'quarvo-workerd: ' | head -1)"

# --- Throughput: N requests at fixed concurrency, wall-clocked. ---
START="$(date +%s.%N)"
seq "$N" | xargs -P "$CONCURRENCY" -I{} curl -fsS --max-time 30 -o /dev/null \
  "http://127.0.0.1:$PORT/" || { echo "FATAL: request batch failed"; exit 1; }
END="$(date +%s.%N)"
ELAPSED="$(echo "$END - $START" | bc)"
RPS="$(echo "$N / $ELAPSED" | bc)"
echo "meter-overhead mode=$MODE n=$N concurrency=$CONCURRENCY elapsed=${ELAPSED}s rps=$RPS"

if [ "$MODE" = "on" ]; then
  # Sanity: metering is actually on and counting (getStats exposed, counters advanced).
  STATS="$(curl -fsS --max-time 5 "http://127.0.0.1:$PORT/stats")"
  echo "stats: $STATS"
  echo "$STATS" | grep -q '"cpuMs"' || { echo "FAIL: metering on but getStats absent"; exit 1; }
  echo "$STATS" | grep -q '"cpuMs":0,' && { echo "FAIL: cpuMs still zero after $N requests"; exit 1; }
else
  STATS="$(curl -fsS --max-time 5 "http://127.0.0.1:$PORT/stats")"
  echo "$STATS" | grep -q '"metering":false' || {
    echo "FAIL: metering off but getStats present"; exit 1; }
fi

echo "$RPS" > "/tmp/meter-overhead-rps-$MODE"
echo "PASS: mode=$MODE"
