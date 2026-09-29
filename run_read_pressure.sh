#!/usr/bin/env bash
# Parallel read-only workers; foreground launcher owns and stops its children.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
workers=${1:-8}
pool_mb=${2:-256}
duration=${3:-0}
for value in "$workers" "$pool_mb" "$duration"; do
    [[ "$value" =~ ^(0|[1-9][0-9]{0,6})$ ]] || {
        echo "Usage: bash $0 [workers=8] [MB_per_worker=256] [seconds=0]" >&2
        exit 2
    }
done
(( workers > 0 && workers <= 320 && pool_mb > 0 )) || exit 2
[[ -x ./periodic_read ]] || { echo 'Run bash build.sh first' >&2; exit 1; }
(( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 3) )) || {
    echo 'Bash 4.3 or newer is required' >&2; exit 1;
}
# Reject allocations likely to exhaust host memory. Container limits may be lower.
available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
required_kb=$((workers * pool_mb * 1000))
if [[ -n "$available_kb" ]] && (( required_kb > available_kb / 2 )); then
    echo "Requested ${workers} x ${pool_mb} MB exceeds half of MemAvailable" >&2
    exit 1
fi
mkdir -p result/read_pressure
out_dir=$(mktemp -d "result/read_pressure/$(date +%Y%m%d_%H%M%S)_XXXXXX")
pids=()
cleanup() {
    trap '' INT TERM
    for pid in "${pids[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "workers=$workers MB_per_worker=$pool_mb total_MB=$((workers * pool_mb)) seconds=$duration"
echo "Logs: $PWD/$out_dir"
echo "Ctrl+C stops all workers. Initialization writes memory; measure after every worker reports READY."
for ((i=0; i<workers; i++)); do
    ./periodic_read --size-mb "$pool_mb" --period-ms 0 \
        --seconds "$duration" > "$out_dir/worker_$i.log" 2>&1 &
    pids+=("$!")
done
printf '%s\n' "${pids[@]}" > "$out_dir/pids.txt"
status=0
for ((i=0; i<workers; i++)); do
    wait -n || { status=$?; break; }
done
exit "$status"
