// Periodic CPU read worker. Generic C++11; no platform-specific cache control.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "signal handling requires lock-free atomic bool");
std::atomic<bool> stopped(false);
void stop_handler(int) { stopped.store(true, std::memory_order_relaxed); }
using Clock = std::chrono::steady_clock;

std::uint64_t number(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected non-negative integer: " + value);
    return std::stoull(value);
}

struct Stats {
    long double bytes = 0;
    std::uint64_t bursts = 0, missed = 0, checksum = 0;
    double elapsed = 0;
};

Stats run(const volatile std::uint64_t* data, std::size_t block,
          std::size_t blocks, bool rotate, std::uint64_t period_ms,
          std::uint64_t seconds, bool report_progress = false) {
    Stats result;
    std::uint64_t a = 0, b = 0, c = 0, d = 0;
    std::size_t selected = 0;
    const auto start = Clock::now();
    auto next = start;
    const auto period = std::chrono::milliseconds(period_ms);
    auto last_report = start;
    long double last_bytes = 0;
    auto expired = [&]() {
        const auto now = Clock::now();
        const double interval = std::chrono::duration<double>(now - last_report).count();
        if (report_progress && interval >= 1.0) {
            // Use actual elapsed time and bytes since the previous report,
            // including sleep time. Flush so redirected logs update live too.
            std::cout << std::fixed << std::setprecision(3)
                      << "[PROGRESS] elapsed_s="
                      << std::chrono::duration<double>(now - start).count()
                      << " interval_s=" << interval
                      << " logical_read_GB_s="
                      << (result.bytes - last_bytes) / interval / 1e9L
                      << " completed_bursts=" << result.bursts
                      << " missed_slots=" << result.missed << std::endl;
            last_report = now;
            last_bytes = result.bytes;
        }
        return stopped || (seconds &&
            std::chrono::duration<double>(now - start).count() >= seconds);
    };
    while (!expired()) {
        // Bounded sleeps permit prompt shutdown, even with a long period.
        while (Clock::now() < next && !expired()) {
            std::this_thread::sleep_until(std::min(next,
                Clock::now() + std::chrono::milliseconds(1)));
        }
        if (expired()) break;
        const auto* source = data + selected * block;
        std::size_t offset = 0;
        while (offset < block && !expired()) {
            const auto end = offset + std::min<std::size_t>(8192, block - offset);
            std::size_t i = offset;
            for (; end - i >= 4; i += 4) {
                a += source[i]; b += source[i + 1];
                c += source[i + 2]; d += source[i + 3];
            }
            for (; i < end; ++i) a += source[i];
            result.bytes += static_cast<long double>(end - offset) * sizeof(*source);
            offset = end;
        }
        if (offset == block) ++result.bursts;
        if (rotate) selected = (selected + 1) % blocks;
        // Fixed start-to-start period. Skip overdue slots, never spawn extra
        // workers or build an unbounded backlog when a scan takes > period.
        next += period;
        const auto now = Clock::now();
        if (next < now) {
            const auto skip = (now - next) / period + 1;
            next += period * skip;
            result.missed += static_cast<std::uint64_t>(skip);
        }
    }
    result.elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    result.checksum = a + b + c + d;
    return result;
}

int worker(int argc, char** argv) {
    try {
        std::uint64_t size_mb = 30, pool_mb = 0, period_ms = 5, seconds = 60, warmup = 3;
        bool pool_set = false, rotate = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option(argv[i]);
            if (option == "--help" || option == "-h") {
                std::cout << "Usage: periodic_read [--size-mb N] [--pool-mb N] "
                    "[--selection fixed|rotate] [--period-ms N] [--seconds N] "
                    "[--warmup-seconds N]\n"
                    "Defaults: block=30 MB, pool=block, fixed, period=5 ms, "
                    "duration=60 s, warmup=3 s. MB=1000000 bytes.\n"
                    "Pool must be a positive multiple of block size.\n"
                    "seconds=0: until signal; warmup=0: no warmup.\n"
                    "Measurement reports logical read GB/s approximately every second.\n"
                    "Periods are start-to-start; overdue slots are skipped.\n";
                return 0;
            }
            if (option != "--size-mb" && option != "--pool-mb" &&
                option != "--selection" && option != "--period-ms" &&
                option != "--seconds" && option != "--warmup-seconds")
                throw std::invalid_argument("unknown option: " + option);
            if (++i == argc) throw std::invalid_argument("missing value for " + option);
            const std::string value(argv[i]);
            if (option == "--selection") {
                if (value != "fixed" && value != "rotate")
                    throw std::invalid_argument("selection must be fixed or rotate");
                rotate = value == "rotate";
                continue;
            }
            const auto n = number(value);
            if (option == "--size-mb") size_mb = n;
            else if (option == "--pool-mb") { pool_mb = n; pool_set = true; }
            else if (option == "--period-ms") period_ms = n;
            else if (option == "--seconds") seconds = n;
            else warmup = n;
        }
        if (!pool_set) pool_mb = size_mb;
        if (!size_mb || !pool_mb || pool_mb < size_mb || pool_mb % size_mb ||
            pool_mb > std::numeric_limits<std::size_t>::max() / 1000000)
            throw std::invalid_argument("pool must fit size_t and be a positive multiple of size-mb");
        if (!period_ms || period_ms > 86400000)
            throw std::invalid_argument("period-ms must be in [1, 86400000]");
        const auto count = static_cast<std::size_t>(pool_mb) * 1000000 / sizeof(std::uint64_t);
        const auto block = static_cast<std::size_t>(size_mb) * 1000000 / sizeof(std::uint64_t);
        std::unique_ptr<std::uint64_t[]> data(new std::uint64_t[count]);
        for (std::size_t i = 0; i < count; ++i) {
            if (i % 8192 == 0 && stopped) return 0;
            data[i] = static_cast<std::uint64_t>(i) + 1;
        }
        std::cout << "Initialized: block_MB=" << size_mb << " pool_MB=" << pool_mb
                  << " period_ms=" << period_ms << " selection=" << (rotate ? "rotate" : "fixed")
                  << " worker_threads=1\nWarmup: seconds=" << warmup << std::endl;
        if (warmup && !stopped) {
            const auto stats = run(data.get(), block, count / block, rotate, period_ms, warmup);
            std::cout << "Warmup checksum=" << stats.checksum << std::endl;
        }
        if (stopped) return 0;
        std::cout << "READY: periodic read-only measurement starts; seconds=" << seconds << std::endl;
        std::cout << "Progress reports logical reads including idle time and cache hits; "
                     "not hardware L3 or DRAM bandwidth." << std::endl;
        const auto stats = run(data.get(), block, count / block, rotate, period_ms, seconds, true);
        std::cout << std::fixed << std::setprecision(3)
                  << "elapsed_s=" << stats.elapsed << " completed_bursts=" << stats.bursts
                  << " missed_slots=" << stats.missed << " logical_read_GB=" << stats.bytes / 1e9L
                  << " logical_read_GB_s=" << (stats.elapsed > 0 ? stats.bytes / stats.elapsed / 1e9L : 0)
                  << " checksum=" << stats.checksum << '\n'
                  << "Logical rate includes idle time and cache hits; it is NOT DRAM bandwidth.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
} // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    try {
        int status = 1;
        std::thread background([&]() { status = worker(argc, argv); });
        background.join();
        return status;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
