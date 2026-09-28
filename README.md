# NVMe-oF 场景的周期性 CPU 后台读负载

| 程序 | 行为 | 默认值 |
| --- | --- | --- |
| `periodic_read.cpp` | 常驻后台线程定时读取一块数组 | 每 5 ms 读取 30 MB；固定块 |

程序使用一个 `std::thread` 执行负载，主线程等待退出。初始化完整写入一次后，测量阶段只读数组，不拷贝、不修改数据；使用 volatile 读取防止编译器消除读操作。不使用 NPU/CANN、MPAM、缓存刷新指令或显式大页。

## NVMe-oF 参考与场景构建

本地仓库没有 NVMe-oF 实现代码。此次阅读了公开的 [SPDK NVMe-oF 示例源码](https://github.com/spdk/spdk/blob/master/examples/nvmf/nvmf/nvmf.c) 和 [目标端说明](https://spdk.io/doc/nvmf.html)，作为设计参考，不代表服务器实际运行的就是 SPDK。

示例中的 `nvmf_reactor_run()` 持续调用 `spdk_thread_poll()`；`nvmf_init_threads()` 为 CPU 核建立 reactor 并启动绑定的系统线程。由此，若研究内存干扰，应将后台读取放在独立 OS 线程或进程里，不应直接在前台轮询回调中插入 sleep 或长时间数组扫描，否则会混入阻塞前台处理的影响。

先单独运行后台程序，确认其自身的 DRAM 读写表现；以后再与 NVMe-oF 服务同时运行。前后台使用不同 CPU 核，但应共享目标 LLC/L3 域，并将数据放在预期 NUMA 节点。可在启动命令前使用服务器已有的 `taskset` 或 `numactl`，CPU/节点编号按拓扑选择。普通 `nice` 只改变 CPU 调度优先级，不等于内存带宽限速。

## 编译

将本目录文件放到服务器，在 Linux 上执行：

```bash
bash build.sh
```

等价命令（通用 C++11，适用于具备对应工具链的 ARM64/x86 Linux）：

```bash
g++ -O3 -std=c++11 -Wall -Wextra -Wpedantic -pthread periodic_read.cpp -o periodic_read
```

## 每 5 ms 读取一块内存

```bash
# 固定读取同一个 30 MB 数组，预热 3 秒，正式运行 60 秒
./periodic_read --size-mb 30 --period-ms 5 --seconds 60

# 可选：从 240 MB 池中轮转读取，每次仍读 30 MB
./periodic_read --size-mb 30 --pool-mb 240 --selection rotate --period-ms 5 --seconds 60
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--size-mb` | 30 | 每轮读取的块大小，正整数 MB |
| `--pool-mb` | 等于块大小 | 总分配大小，必须是块大小的正整数倍 |
| `--selection` | fixed | fixed 重读首块；rotate 按顺序轮转池内各块 |
| `--period-ms` | 5 | 两轮计划开始时间的间隔，范围 1～86400000 ms |
| `--seconds` | 60 | 正式运行秒数；0 表示持续运行直到停止信号 |
| `--warmup-seconds` | 3 | 预热秒数；0 跳过预热 |

第一轮立即读取，之后按单调时钟的固定周期调度。若扫描超过周期，跳过错过的时隙并记录 `missed_slots`，不会叠加线程或积压任务。Linux 普通调度不保证精确到每个 5 ms。

30 MB / 5 ms 的理论逻辑读取速率为 6 GB/s，包含缓存命中，不是保证的 DRAM 带宽。结束时输出完成轮数、错过时隙、逻辑读取量和校验和；平均速率包含休眠时间。

正式运行阶段约每秒输出一行 `[PROGRESS]`，并立即刷新输出，重定向到日志时也可以用 `tail -f periodic.log` 实时查看。`logical_read_GB_s` 是最近一个报告区间的读取字节数除以实际区间时长（`interval_s`），包含休眠与缓存命中，不是硬件 L3 或 DRAM 带宽。`completed_bursts` 和 `missed_slots` 为正式运行以来的累计值。预热阶段不输出这些进度，结束时仍保留全程统计；输出日志本身会带来少量额外开销。

固定块模式可能在预热后主要命中缓存。如果实际 DRAM 读流量低，可以显式增大池并轮转，使活跃工作集超过可用 LLC；这改变了总工作集，实验记录中必须保留 `pool_MB`。240 MB 只是示例，不保证适合所有服务器。

统一 **1 MB = 1,000,000 字节**，默认 30 MB 约为 28.61 MiB。数组虚拟地址连续，不保证物理连续，也不锁定页面。

## 后台运行及监控

后台启动并记录进程 ID：

```bash
./periodic_read --size-mb 30 --period-ms 5 --seconds 0 > periodic.log 2>&1 &
bg_pid=$!

cat periodic.log
# 看到 READY 后，在另一个终端执行：
# bash pref_dram_l3.sh dram.log

# 实验完成，在启动负载的终端停止该进程
kill -TERM "$bg_pid"
wait "$bg_pid"
```

`pref_dram_l3.sh` 保留原方案的 3 秒等待、默认每 2 秒采样和追加日志行为，默认程序路径是 `/home/w00850971/scripts/tools/pcm/build/bin/pcm-memory`。可覆盖路径及采样间隔：

```bash
PCM_BIN=/absolute/path/pcm-memory bash pref_dram_l3.sh dram.log 2
```

监控独立运行，结束负载不会自动结束监控；在监控终端按 Ctrl+C 停止。按服务器现有的计数器权限运行。相对日志路径相对于调用时的工作目录，不会因为切换工具目录而改变。

### 按运行时间保存独立采集日志

运行 `bash run_monitor_once.sh 30`，每次在 `${SCRIPT_PATH:-/home/w00850971/scripts}/result/monitor/年月日_时分秒_随机后缀/` 下保存 `dram_l3.log`（采集器原始输出）、`monitor_console.log`（开始/结束时间、退出状态和包装脚本输出）以及 `bandwidth_summary.log`（原始时间戳和关键带宽汇总行）。这是每次运行独立建目录，不是在一次运行中定时轮转文件。时长包含采集器启动时间。

汇总支持 Intel PCM 和鲲鹏 `dram_*_bandwidth_total`、`l3_cpu_*_bandwidth_total` 等日志格式；不改变采集器的计数器配置。服务器如已安装鲲鹏采集脚本，应保留该脚本，用 `MONITOR_SCRIPT=/absolute/path/to/collector.sh bash run_monitor_once.sh 30` 指定，采集脚本须接受第一个参数作为输出日志路径，并在采集期间保持前台运行。不要用仓库的 Intel PCM 包装脚本覆盖现场的鲲鹏采集器。

官方 [Intel PCM](https://github.com/intel/pcm) 针对 Intel CPU；如果主机为鲲鹏/ARM，需确认这个路径是否是现场适配的工具，否则应使用对应平台的 DDR/内存控制器计数器工具。脚本不自动安装工具或修改权限。

## 如何判断效果

1. 先记录空载 DRAM 读写带宽，再单独启动后台程序。
2. 日志出现 READY 表示初始化、预热结束；观察后续稳定区间，初始化写流量及脏行写回仍可能延续到最初几个样本。
3. 对比 DRAM 读带宽增量与写带宽增量，判断是否接近“读多写少”的目标。实际 L3 填充/未命中需平台对应的缓存计数器佐证；`pcm-memory` 不能单独证明 L3 驻留。

CPU 读取可能填充缓存，但普通读取不会保证只填 L3、绕过其他缓存或把整个数组锁在 L3。干净缓存行逐出不要求写回 DRAM；本程序不主动写脏数组，因此不会有意制造持续的数组写回流量。volatile 仅约束编译器，不绕过缓存。

`logical_read_GB_s` 包含缓存命中，不能当作 DRAM 带宽。2 秒采样只能显示平均流量，不能分辨每个 5 ms 突发。程序不承诺固定 30 MB 工作集必然产生大量 DRAM 读流量，最终以服务器计数器为准。

## 验证情况

当前 Windows 环境未找到可用 C++ 编译器，尚未编译运行 C++ 程序，也没有目标服务器的硬件带宽结果。两个 shell 脚本已通过 Bash 语法检查。服务器上可先做以下冒烟检查：

```bash
bash build.sh
./periodic_read --help
./periodic_read --size-mb 1 --pool-mb 4 --selection rotate --period-ms 5 --warmup-seconds 0 --seconds 1
# 以下参数应报错并返回非零状态：
./periodic_read --period-ms 0
./periodic_read --size-mb 30 --pool-mb 31
```
