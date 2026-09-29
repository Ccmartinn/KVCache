// Build: g++ -O2 -std=c++11 -pthread tests/random_read_test.cpp -o random_read_test
#define main periodic_read_program_main
#include "../periodic_read.cpp"
#undef main
#include <cassert>

int main() {
    stopped.store(false);
    const auto first = random_order(10003, 7);
    assert(first == random_order(10003, 7));
    assert(first != random_order(10003, 8));
    auto sorted = first;
    std::sort(sorted.begin(), sorted.end());
    for (std::size_t i = 0; i < sorted.size(); ++i) assert(sorted[i] == i);
    assert(random_order(0, 1).empty());
    assert(random_order(1, 1)[0] == 0);

    // One small pass followed by waiting: exercise unrolled and tail reads,
    // and prove byte accounting and checksum agree with sequential access.
    const std::uint64_t data[] = {1, 2, 3, 4, 5};
    const std::vector<std::size_t> sequential;
    const auto shuffled = random_order(5, 1);
    for (const auto* order : {&sequential, &shuffled}) {
        const auto stats = run(data, 5, 1, false, 2000, 1, *order);
        assert(stats.bursts == 1);
        assert(stats.bytes == 40);
        assert(stats.checksum == 15);
    }
    stopped.store(true);
    assert(random_order(10003, 1).empty());
    assert(run(data, 5, 1, false, 0, 0, shuffled).bytes == 0);
    std::cout << "Random read checks passed\n";
}
