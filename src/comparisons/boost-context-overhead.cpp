#include <boost/context/fiber.hpp>
#include <chrono>

using fiber = boost::context::fiber;

fiber bar(fiber&& c, int count) {
    for (int i = 0; i < count; ++i) {
        c = std::move(c).resume();
    }
    return std::move(c);
}

void runner() {
    int count = 100'000'000;
    auto start = std::chrono::steady_clock::now();

    fiber c([count](fiber&& c) {
        return bar(std::move(c), count);
    });
    while (c) {
        c = std::move(c).resume();
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    auto callNs = double(elapsedNs) / double(count);
    printf("Single call is %fns\n", callNs);
}

int main() {
    runner();
    return 0;
}
