#!/usr/bin/env bash
# Invoke after READY. pcm-memory observes DRAM bandwidth, not L3 occupancy.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash pref_dram_l3.sh LOG_FILE [SAMPLE_SECONDS]" >&2
    echo "Optional environment: PCM_BIN=/absolute/path/pcm-memory" >&2
    exit 2
fi
log_file=$1
sample_seconds=${2:-2}
pcm_bin=${PCM_BIN:-/home/w00850971/scripts/tools/pcm/build/bin/pcm-memory}
if [[ ! "$sample_seconds" =~ ^[1-9][0-9]*$ ]]; then
    echo "SAMPLE_SECONDS must be a positive integer" >&2
    exit 2
fi
if [[ ! -x "$pcm_bin" ]]; then
    echo "pcm-memory is not executable: $pcm_bin; set PCM_BIN to the correct path" >&2
    exit 1
fi
sleep 3
# Do not cd: a relative log path stays relative to the caller's directory.
exec "$pcm_bin" "$sample_seconds" >> "$log_file" 2>&1
