// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 GOW3_FLIP_TRACE=<file> (diagnostics): one CSV line per stage of every flip, to see where
// frame pacing turns uneven under a frame cap: the game registers the flip (submit), the draw
// recording thread reaches it (irq), its present is recorded (queued), the present thread takes
// it (release), the swap thread has presented it (presented), the game's flip event fired
// (flipdone). Columns: stage, flip argument (the game's frame tag), nanoseconds since start. The
// first row, "start", holds the start itself in steady_clock nanoseconds (QueryPerformanceCounter on
// Windows), to line the trace up with an ETW / WPR capture.
//
// Rows are kept in memory and the file is written once, at exit (2026-11-15): writing as it went
// (fprintf under a mutex on the frame threads, the disk writes on whichever thread filled the
// buffer) cost ~3% FPS and isolated spikes every few seconds in a 237 FPS static view. Write() runs
// from the window-close exit (gow3gpu.cpp, std::_Exit), atexit (the game's exit()) and the Windows
// crash handler (gow3gpu_flip_trace_write); a process killed from outside writes nothing. ~24 bytes
// a row, ~1,900 rows a second at 237 FPS: ~80 MB an hour.
// GOW3_FLIP_TRACE_GPU_READY=1: also gpu_ready rows (when the GPU finished each frame), from a thread
// that waits on every frame's GPU work. Off by default since 2026-11-16: that thread cost small
// overshoots in a 237 FPS static view (worst frame 5.9 ms with it, 4.66 ms without).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Gow3FlipTrace {
inline std::chrono::steady_clock::time_point Start() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}
struct Row {
    const char* stage; // a string literal
    std::int64_t flip_arg;
    std::int64_t ns;
};
struct Trace {
    static constexpr std::size_t kChunk = 65536; // rows (1.5 MB), allocated as they fill
    std::string path;
    long long start_ns = 0, wall_ns = 0, wall_at_ns = 0;
    std::mutex mutex;
    std::vector<std::unique_ptr<Row[]>> chunks;
    std::size_t used = kChunk; // rows used in the last chunk
    bool written = false;
};
inline void Write();
inline Trace* Get() {
    static Trace* trace = [] {
        const char* path = std::getenv("GOW3_FLIP_TRACE");
        if (!path || !path[0]) {
            return static_cast<Trace*>(nullptr);
        }
        auto* t = new Trace; // never freed: rows may arrive until the process ends
        t->path = path;
        const auto ns = [](auto d) {
            return static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count());
        };
        t->start_ns = ns(Start().time_since_epoch());
        // The wall clock at a known trace time, to line the trace up with an ETW / WPR capture
        // to the microsecond (an ETL's start time is wall clock; its boot time is printed to
        // 0.1 s only): "wall,<Unix time ns>,<ns since start>", read between two steady clock
        // reads (their midpoint). Its flip_arg never gets a "presented" row, so the frame tools
        // skip it. (libc++'s system_clock is GetSystemTimePreciseAsFileTime on Windows.)
        const auto before = std::chrono::steady_clock::now();
        const auto wall = std::chrono::system_clock::now();
        const auto after = std::chrono::steady_clock::now();
        t->wall_ns = ns(wall.time_since_epoch());
        t->wall_at_ns = ns(before - Start() + (after - before) / 2);
        t->chunks.reserve(4096);
        std::atexit([] { Write(); });
        return t;
    }();
    return trace;
}
inline bool Enabled() {
    return Get() != nullptr;
}
inline bool GpuReadyEnabled() {
    static const bool on = [] {
        const char* value = std::getenv("GOW3_FLIP_TRACE_GPU_READY");
        return Enabled() && value && value[0] && value[0] != '0';
    }();
    return on;
}
inline void Note(const char* stage, std::int64_t flip_arg) {
    Trace* trace = Get();
    if (!trace) return;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - Start()).count();
    std::scoped_lock lock{trace->mutex};
    if (trace->used == Trace::kChunk) {
        trace->chunks.emplace_back(new Row[Trace::kChunk]);
        trace->used = 0;
    }
    trace->chunks.back()[trace->used++] = {stage, flip_arg, static_cast<std::int64_t>(ns)};
}
// Writes the file once (later calls do nothing). Rows noted afterwards are not written.
inline void Write() {
    Trace* trace = Get();
    if (!trace) return;
    // From a crash handler the faulting thread may hold the lock: give up after ~200 ms, never hang the exit.
    std::unique_lock lock{trace->mutex, std::defer_lock};
    for (int tries = 0; !lock.try_lock(); ++tries) {
        if (tries == 200) {
            std::fprintf(stderr, "Flip trace: not written (busy)\n");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (trace->written) return;
    trace->written = true;
    std::FILE* f = std::fopen(trace->path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "Flip trace: cannot write %s\n", trace->path.c_str());
        return;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    std::fputs("stage,flip_arg,ns\n", f);
    std::fprintf(f, "start,0,%lld\n", trace->start_ns);
    std::fprintf(f, "wall,%lld,%lld\n", trace->wall_ns, trace->wall_at_ns);
    std::size_t rows = 0;
    for (std::size_t c = 0; c < trace->chunks.size(); ++c) {
        const std::size_t n = c + 1 == trace->chunks.size() ? trace->used : Trace::kChunk;
        for (std::size_t i = 0; i < n; ++i) {
            const Row& r = trace->chunks[c][i];
            std::fprintf(f, "%s,%lld,%lld\n", r.stage, static_cast<long long>(r.flip_arg),
                         static_cast<long long>(r.ns));
        }
        rows += n;
    }
    std::fclose(f);
    std::printf("Flip trace: %zu rows written to %s\n", rows, trace->path.c_str());
    std::fflush(stdout);
}
} // namespace Gow3FlipTrace
