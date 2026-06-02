#!/bin/bash
# Compare QPS at fixed concurrency across worker-thread counts.
# This is the "what's the right --threads value" data.
set -euo pipefail
cd "$(dirname "$0")/../.."

DURATION="${1:-10}"
PORT=9051
HOST="http://127.0.0.1:$PORT/stats"
LOG=bench/results/scale.log
OUT=bench/results/scale.md
mkdir -p bench/results
: > "$LOG"

[ -x flux-server ] || make >/dev/null

{
    echo "# Worker-thread scaling — /stats endpoint"
    echo
    echo "Fixed: wrk -t4 -c100 -d${DURATION}s. Varies: server --threads"
    echo
    echo "| --threads |     QPS    | P50 lat | P99 lat |"
    echo "|----------:|-----------:|--------:|--------:|"

    for T in 1 2 4 8 12; do
        pkill -9 -f "flux-server" >/dev/null 2>&1 || true
        sleep 0.3
        ./flux-server --port "$PORT" --threads "$T" --log-level warn \
            >> "$LOG" 2>&1 &
        SVR=$!
        sleep 0.4
        curl -sS "$HOST" >/dev/null
        txt=$(wrk -t4 -c100 -d"${DURATION}s" --latency "$HOST")
        echo "===== threads=$T =====" >> "$LOG"
        echo "$txt" >> "$LOG"
        qps=$(echo "$txt" | awk '/Requests\/sec:/{print $2}')
        p50=$(echo "$txt" | awk '/^ +50%/{print $2}')
        p99=$(echo "$txt" | awk '/^ +99%/{print $2}')
        printf '| %9d | %10s | %7s | %7s |\n' "$T" "$qps" "$p50" "$p99"
        kill -TERM $SVR 2>/dev/null || true
        wait $SVR 2>/dev/null || true
    done
} > "$OUT"

cat "$OUT"
echo "(full wrk in $LOG)"
