// Single CPU worker; portable C++11, no Ascend/CANN dependency.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
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

std::uint64_t number(const std::string& text) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("expected a non-negative integer: " + text);
    std::size_t used = 0;
    const auto result = std::stoull(text, &used);
    if (used != text.size()) throw std::invalid_argument("invalid integer");
    return result;
}

struct Result {
    long double bytes = 0;
    double seconds = 0;
    std::uint64_t checksum = 0;
};

Result read_loop(const volatile std::uint64_t* data, std::size_t count,
                 std::uint64_t duration) {
    Result result;
    std::uint64_t a = 0, b = 0, c = 0, d = 0;
    std::size_t offset = 0;
    const auto start = Clock::now();
    auto now = start;
    // Check time/signals every 64 KiB; no logging or array stores in this loop.
    while (!stopped && (duration == 0 ||
           std::chrono::duration<double>(now - start).count() < duration)) {
        const std::size_t end = offset + std::min<std::size_t>(8192, count - offset);
        std::size_t i = offset;
        // Volatile loads prevent removal/reuse of reads. Four independent sums
        // reduce the dependency chain; unsigned checksum overflow is defined.
        for (; end - i >= 4; i += 4) {
            a += data[i];
            b += data[i + 1];
            c += data[i + 2];
            d += data[i + 3];
        }
        for (; i < end; ++i) a += data[i];
        result.bytes += static_cast<long double>(end - offset) * sizeof(*data);
        offset = end == count ? 0 : end;
        now = Clock::now();
    }
    result.seconds = std::chrono::duration<double>(now - start).count();
    result.checksum = a + b + c + d;
    return result;
}
} // namespace

int worker(int argc, char** argv) {
    try {
        std::uint64_t size_mb = 30, seconds = 60, warmup = 3;
        for (int i = 1; i < argc; ++i) {
            const std::string arg(argv[i]);
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: background_read [--size-mb N] [--seconds N] "
                             "[--warmup-seconds N]\n"
                             "Defaults: 30 MB (1 MB = 1000000 bytes), 60 s, 3 s warmup.\n"
                             "--seconds 0 runs until SIGINT/SIGTERM; warmup 0 skips warmup.\n"
                             "One CPU worker, sequential reads, no steady-state array writes.\n";
                return 0;
            }
            if (arg != "--size-mb" && arg != "--seconds" && arg != "--warmup-seconds")
                throw std::invalid_argument("unknown option: " + arg);
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            const auto value = number(argv[i]);
            if (arg == "--size-mb") size_mb = value;
            else if (arg == "--seconds") seconds = value;
            else warmup = value;
        }
        if (size_mb == 0 || size_mb > std::numeric_limits<std::size_t>::max() / 1000000)
            throw std::invalid_argument("size-mb must be positive and fit in size_t");
        const auto bytes = static_cast<std::size_t>(size_mb) * 1000000;
        const auto count = bytes / sizeof(std::uint64_t);
        std::unique_ptr<std::uint64_t[]> data(new std::uint64_t[count]);
        // First-touch real pages before measuring; initialization writes are
        // intentional and excluded from software timing (not hardware counters).
        for (std::size_t i = 0; i < count; ++i) {
            if ((i % 8192 == 0) && stopped) return 0;
            data[i] = static_cast<std::uint64_t>(i) + 1;
        }
        std::cout << "Initialized: bytes=" << bytes << " worker_threads=1\n"
                  << "Warmup: seconds=" << warmup << std::endl;
        if (warmup && !stopped) {
            const auto result = read_loop(data.get(), count, warmup);
            std::cout << "Warmup checksum=" << result.checksum << std::endl;
        }
        if (stopped) return 0;
        std::cout << "READY: read-only measurement starts; seconds=" << seconds
                  << " (0=until signal)" << std::endl;
        const auto result = read_loop(data.get(), count, seconds);
        const long double rate = result.seconds > 0
            ? result.bytes / result.seconds / 1e9L : 0;
        std::cout << std::fixed << std::setprecision(3)
                  << "elapsed_s=" << result.seconds
                  << " logical_read_GB=" << result.bytes / 1e9L
                  << " logical_read_GB_s=" << rate
                  << " checksum=" << result.checksum << '\n'
                  << "Logical read rate includes cache hits; it is NOT DRAM bandwidth.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}

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
