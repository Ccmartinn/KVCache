#!/usr/bin/env bash
set -eu
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

# 1. 需要遍历的参数，修改括号里的值即可
page_sizes=(512K 1M)
thread_counts=(7 14)
hugepage_counts=(16 28 32)
cache_sizes=(70 140)

# 2. 每组固定参数
rounds=15
start_seed=10
measure_seconds=10
warmup_seconds=10

# 3. 使用已编译的程序，为本次运行创建日志目录
[[ -x ./hugepage_scan_70140 ]] || { echo '缺少同目录下的可执行程序 hugepage_scan_70140' >&2; exit 1; }
mkdir -p results/hugepage_scan_70140
log_dir=$(mktemp -d "results/hugepage_scan_70140/$(date +%Y%m%d_%H%M%S)_XXXXXX")

# 4. 逐组运行；上一组结束后才启动下一组
for page_size in "${page_sizes[@]}"; do
    for thread_count in "${thread_counts[@]}"; do
        for hugepage_count in "${hugepage_counts[@]}"; do
            for cache_size in "${cache_sizes[@]}"; do
                log_file="$log_dir/${page_size}_${thread_count}threads_${hugepage_count}pages_${cache_size}MiB.log"
                echo "运行：$page_size / ${thread_count}线程 / ${hugepage_count}大页 / ${cache_size}MiB；日志：$log_file"
                taskset -c 0-15 ./hugepage_scan_70140 \
                    "$page_size" "$thread_count" random "$rounds" "$start_seed" \
                    "$measure_seconds" "$warmup_seconds" "$hugepage_count" "$cache_size" \
                    > "$log_file" 2>&1
            done
        done
    done
done

echo "全部完成，日志目录：$log_dir"
