#!/usr/bin/env bash
# Monitor current traffic using the installed collection wrapper.
set -euo pipefail

duration="${1:-30}"
script_path="${SCRIPT_PATH:-/home/w00850971/scripts}"
monitor="${MONITOR_SCRIPT:-${script_path}/pref_dram_l3.sh}"

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

output_root="${script_path}/result/monitor"
mkdir -p "$output_root"
# Keep every run; the random suffix also avoids same-second name collisions.
out_dir=$(mktemp -d "${output_root}/$(date +%Y%m%d_%H%M%S)_XXXXXX")
log="${out_dir}/dram_l3.log"
console="${out_dir}/monitor_console.log"
summary="${out_dir}/bandwidth_summary.log"
: > "$log"

echo "Duration: ${duration}s (includes collector startup time)"
echo "Raw metrics: $log"
echo "Console log: $console"
echo "Summary: $summary"
{
    echo "Start: $(date -Is)"
    echo "Monitor: $monitor"
    echo "Duration seconds: $duration"
    echo "Raw metrics: $log"
} > "$console"

status=0
timeout --signal=TERM --kill-after=5s "${duration}s" \
    bash "$monitor" "$log" >>"$console" 2>&1 || status=$?
{
    echo "End: $(date -Is)"
    echo "Exit status: $status (124 means the requested timeout elapsed)"
} >> "$console"

case "$status" in
    0|124) ;;
    *)
        echo "Monitor failed with exit code $status" >&2
        cat "$console" >&2
        exit "$status"
        ;;
esac

# Preserve source timestamps and totals without mixing counter definitions.
grep -E 'perf start:|perf end:|^[[:space:]]*(dram_(rd|wr)_bandwidth|hha_(rd|wr)_ddr_bandwidth|l3_cpu_(rd|wr)_bandwidth|l3c_(ref|hit)_(rd|wr)_ext_bandwidth|l3_miss)_total[[:space:]]+\|[[:space:]]*--[[:space:]]*[0-9]+([.][0-9]+)?|System (Read|Write|Memory) Throughput' \
    "$log" > "$summary" || true
if grep -qE '^[[:space:]]*(dram_(rd|wr)_bandwidth|hha_(rd|wr)_ddr_bandwidth|l3_cpu_(rd|wr)_bandwidth)_total[[:space:]]+\|[[:space:]]*--[[:space:]]*[0-9]+([.][0-9]+)?|System (Read|Write|Memory) Throughput' "$summary"; then
    echo "Recognized bandwidth samples; summary: $summary" >> "$console"
    echo "Latest bandwidth summary lines:"
    tail -n 30 "$summary"
else
    echo "No recognized bandwidth samples" >> "$console"
    echo "No bandwidth samples found; check $log and $console" >&2
    exit 1
fi
