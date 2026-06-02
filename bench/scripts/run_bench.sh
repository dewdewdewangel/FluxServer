#!/bin/bash
# FluxServer wrk benchmark suite.
#
# Spins up the release server, runs wrk against /, /style.css and /stats
# at several (threads × connections) profiles, captures QPS / P50 / P99 / err
# into a markdown table, then shuts the server down gracefully.
#
# Usage:  bench/scripts/run_bench.sh [duration_seconds=15]
set -euo pipefail

cd "$(dirname "$0")/../.."   # repo root: phase6-production/

DURATION="${1:-15}"
PORT=9050
HOST="http://127.0.0.1:$PORT"
LOG=bench/results/bench.log
OUT=bench/results/bench.md
mkdir -p bench/results
: > "$LOG"

pkill -9 -f "flux-server" >/dev/null 2>&1 || true
sleep 0.4

[ -x flux-server ] || make >/dev/null

./flux-server --port "$PORT" --threads 4 --log-level warn \
    >> "$LOG" 2>&1 &
SVR=$!
trap 'kill -TERM $SVR 2>/dev/null || true; wait $SVR 2>/dev/null || true' EXIT
sleep 0.4

# Warm up so first numbers don't include cold-start noise.
curl -sS "$HOST/" >/dev/null

run_wrk() {
    local label="$1" path="$2" threads="$3" conns="$4"
    local txt
    txt=$(wrk -t"$threads" -c"$conns" -d"${DURATION}s" --latency "$HOST$path")
    echo "===== $label  t=$threads c=$conns =====" >> "$LOG"
    echo "$txt" >> "$LOG"
    # Parse
    local qps p50 p99 errs
    qps=$(echo "$txt" | awk '/Requests\/sec:/{print $2}')
    p50=$(echo "$txt" | awk '/^ +50%/{print $2}')
    p99=$(echo "$txt" | awk '/^ +99%/{print $2}')
    errs=$(echo "$txt" | awk '/Socket errors/{print}' || true)
    printf '| %-15s | %4s | %5s | %10s | %8s | %8s | %s |\n' \
           "$label" "$threads" "$conns" "$qps" "$p50" "$p99" "${errs:-}"
}

{
    echo "# FluxServer benchmark"
    echo
    echo "- date: $(date -u +%FT%TZ)"
    echo "- host: \`$(uname -mr) / nproc=$(nproc)\`"
    echo "- duration: ${DURATION}s per scenario"
    echo "- server: flux-server (release), 4 worker threads, port $PORT"
    echo "- tool: wrk $(wrk -v 2>&1 | head -1 || true)"
    echo
    echo "| scenario        | wrk-t |  conn |     QPS    |  P50 lat | P99 lat | errors |"
    echo "|-----------------|------:|------:|-----------:|---------:|--------:|--------|"
    run_wrk "/ (357B html)"   "/"          1   10
    run_wrk "/ (357B html)"   "/"          4  100
    run_wrk "/ (357B html)"   "/"          8  400
    run_wrk "/style.css"      "/style.css" 4  100
    run_wrk "/stats (JSON)"   "/stats"     4  100
} > "$OUT"

echo
echo "=== summary ==="
cat "$OUT"
echo
echo "(full wrk output in $LOG)"
