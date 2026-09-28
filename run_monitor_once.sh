#!/usr/bin/env bash
# Monitor current DRAM traffic using the existing PCM wrapper.
set -euo pipefail

duration="${1:-30}"
script_path="${SCRIPT_PATH:-/home/w00850971/scripts}"
monitor="${script_path}/pref_dram_l3.sh"

[[ "$duration" =~ ^[1-9][0-9]*$ ]] || {
    echo "Usage: bash $0 [positive integer seconds]" >&2
    exit 1
}
[[ -f "$monitor" ]] || {
    echo "Monitor script not found: $monitor" >&2
    exit 1
}
command -v timeout >/dev/null || {
    echo "GNU timeout is required" >&2
    exit 1
}

out_dir="${script_path}/result/monitor"
mkdir -p "$out_dir"
log="${out_dir}/dram_l3.log"
console="${out_dir}/monitor_console.log"
# The PCM wrapper appends, so reset the log for this run.
: > "$log"

echo "Duration: ${duration}s (includes the wrapper's 3s startup delay)"
echo "DRAM metrics: $log"
echo "Console log: $console"

status=0
timeout --signal=TERM --kill-after=5s "${duration}s" \
    bash "$monitor" "$log" >"$console" 2>&1 || status=$?

case "$status" in
    0|124|137) ;;
    *)
        echo "Monitor failed with exit code $status" >&2
        cat "$console" >&2
        exit "$status"
        ;;
esac

if grep -qE 'System (Read|Write|Memory) Throughput' "$log"; then
    echo "Latest system DRAM bandwidth samples:"
    grep -E 'System (Read|Write|Memory) Throughput' "$log" | tail -n 9
else
    echo "No bandwidth samples found; check $log and $console" >&2
    exit 1
fi
