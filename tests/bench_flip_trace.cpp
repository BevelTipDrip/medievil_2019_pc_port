// GOW3_FLIP_TRACE cost per row (gpu/shim/gow3_flip_trace.h): the old writer (fprintf under a mutex on the
// calling thread, the disk writes on whichever thread filled the buffer) against the in-memory rows.
// 3 threads note ~650 rows a second each (~1,950 a second, MediEvil at 237 FPS) for 20 s per variant.
//   flip-trace-bench <dir>      writes <dir>/flip-bench-old.csv and <dir>/flip-bench-new.csv
// The worst calls matter: a row noted on a frame thread that stalls is a frame-time spike.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
#include "gow3_flip_trace.h"

namespace {
using Clock = std::chrono::steady_clock;
std::FILE* old_file;
// The writer before 2026-11-15, as it was.
void OldNote(const char* stage, std::int64_t flip_arg) {
    static std::mutex mutex;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Gow3FlipTrace::Start()).count();
    std::scoped_lock lock{mutex};
    std::fprintf(old_file, "%s,%lld,%lld\n", stage, static_cast<long long>(flip_arg), static_cast<long long>(ns));
    static unsigned lines;
    if (++lines % 2048 == 0) std::fflush(old_file);
}
void Run(const char* name, void (*note)(const char*, std::int64_t)) {
    constexpr int kThreads = 3, kSeconds = 20;
    std::vector<std::vector<long long>> calls(kThreads);
    std::atomic<long long> frame{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            const char* stages[] = {"submit", "irq", "presented"};
            const auto end = Clock::now() + std::chrono::seconds(kSeconds);
            auto next = Clock::now();
            while (Clock::now() < end) {
                next += std::chrono::microseconds(1538); // ~650 a second
                std::this_thread::sleep_until(next);
                const auto t0 = Clock::now();
                note(stages[t], frame.fetch_add(1, std::memory_order_relaxed));
                calls[t].push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
            }
        });
    }
    for (auto& th : threads) th.join();
    std::vector<long long> all;
    for (auto& c : calls) all.insert(all.end(), c.begin(), c.end());
    std::sort(all.begin(), all.end());
    double sum = 0;
    for (long long v : all) sum += double(v);
    const auto over = [&](long long ns) { return all.end() - std::upper_bound(all.begin(), all.end(), ns); };
    std::printf("  %-4s %7zu rows  avg %7.0f ns  p99 %7lld  p99.9 %8lld  max %9lld ns  >100 us %4lld  >1 ms %3lld\n", name,
                all.size(), sum / double(all.size()), all[all.size() * 99 / 100], all[all.size() * 999 / 1000],
                all.back(), static_cast<long long>(over(100000)), static_cast<long long>(over(1000000)));
}
} // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
#ifdef _WIN32
    timeBeginPeriod(1);
#endif
    const std::string old_path = dir + "/flip-bench-old.csv", new_path = dir + "/flip-bench-new.csv";
#ifdef _WIN32
    _putenv_s("GOW3_FLIP_TRACE", new_path.c_str());
#else
    setenv("GOW3_FLIP_TRACE", new_path.c_str(), 1);
#endif
    old_file = std::fopen(old_path.c_str(), "w");
    if (!old_file) { std::fprintf(stderr, "cannot write %s\n", old_path.c_str()); return 1; }
    std::printf("per-row cost, 3 threads x ~650 rows a second, 20 s each:\n");
    Run("old", OldNote);
    std::fclose(old_file);
    Run("new", Gow3FlipTrace::Note);
    const auto t0 = Clock::now();
    Gow3FlipTrace::Write();
    std::printf("  new: the write at exit took %.1f ms\n",
                std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
    return 0;
}
