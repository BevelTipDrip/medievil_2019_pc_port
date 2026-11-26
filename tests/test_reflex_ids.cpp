// Reflex R1 frame IDs (gpu/shim/gow3_reflex.h FrameIds): present IDs rise strictly whatever mix of
// game frames, non-game presents and dropped frames comes, and a game frame keeps the ID stage A
// gave it unless a non-game present came between its reservation and its present.
// Build and run: ninja -C out/gpu reflex-ids-test && out/gpu/reflex-ids-test
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include "gow3_reflex.h"

using Gow3Reflex::FrameIds;
using u64 = std::uint64_t;

namespace {

struct Frame {
    u64 id;
    bool after_non_game; ///< reserved after the latest non-game present so far (time order)
};

void SteadyState() {
    FrameIds ids;
    u64 prev = 0;
    for (int i = 0; i < 1000; ++i) {
        const u64 frame = ids.Reserve();
        assert(frame == prev + 1);
        assert(ids.Peek(frame) == frame);
        assert(ids.Present(frame) == frame);
        prev = frame;
    }
    // Two frames in flight (stage A one frame ahead), still every frame keeps its ID.
    std::deque<u64> flight{ids.Reserve(), ids.Reserve()};
    for (int i = 0; i < 1000; ++i) {
        flight.push_back(ids.Reserve());
        const u64 frame = flight.front();
        flight.pop_front();
        assert(ids.Present(frame) == frame);
    }
}

void NonGameBetween() {
    FrameIds ids;
    const u64 a = ids.Reserve(), b = ids.Reserve(); // 1, 2 in flight
    const u64 menu = ids.Present(0);                // a redraw takes 1: above every present
    assert(menu == 1);
    const u64 c = ids.Reserve();                    // after the raise: above menu + Window
    assert(c > menu + FrameIds::Window);
    const u64 pa = ids.Present(a), pb = ids.Present(b); // overtaken: 2, 3
    assert(pa == 2 && pb == 3);
    assert(ids.Present(c) == c);                    // reserved after the redraw: keeps its ID
    const u64 d = ids.Reserve();
    assert(ids.Present(d) == d);
    // Dropped frames leave gaps only.
    const u64 e = ids.Reserve(), f = ids.Reserve();
    (void)e;
    assert(ids.Present(f) == f);
    assert(ids.Present(0) == f + 1);
}

// Random mix: reserve (stage A), present the oldest, drop the oldest, non-game present; in-flight
// frames stay under Window, as in the port (flip_pending_num <= 16).
void Random(u64 seed) {
    std::mt19937_64 rng{seed};
    FrameIds ids;
    std::deque<Frame> flight;
    u64 last = 0, kept = 0, game = 0;
    for (int step = 0; step < 200000; ++step) {
        const int action = int(rng() % 100);
        if (action < 45 && flight.size() < 40) {
            flight.push_back({ids.Reserve(), true});
        } else if (action < 85 && !flight.empty()) {
            const Frame frame = flight.front();
            flight.pop_front();
            const u64 id = ids.Present(frame.id);
            assert(id > last);
            last = id;
            ++game;
            if (frame.after_non_game) {
                assert(id == frame.id);
            }
            kept += id == frame.id;
        } else if (action < 90 && !flight.empty()) {
            flight.pop_front(); // dropped: never presented
        } else {
            const u64 id = ids.Present(0);
            assert(id > last);
            last = id;
            for (Frame& frame : flight) {
                frame.after_non_game = false;
            }
        }
        assert(ids.Last() == last);
    }
    std::printf("seed %llu: %llu game presents, %llu kept their stage A ID\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(game),
                static_cast<unsigned long long>(kept));
}

// Stage A reserving on one thread while the presenter presents and redraws on another.
void Threads() {
    FrameIds ids;
    std::mutex mutex;
    std::deque<u64> queue;
    bool done = false;
    std::thread stage_a([&] {
        for (int i = 0; i < 200000; ++i) {
            const u64 frame = ids.Reserve();
            std::scoped_lock lock{mutex};
            queue.push_back(frame);
        }
        std::scoped_lock lock{mutex};
        done = true;
    });
    u64 last = 0, presents = 0;
    std::mt19937 rng{7};
    while (true) {
        u64 frame = 0;
        bool finished = false;
        {
            std::scoped_lock lock{mutex};
            if (!queue.empty()) {
                frame = queue.front();
                queue.pop_front();
            } else {
                finished = done;
            }
        }
        if (finished) {
            break;
        }
        if (frame == 0 && rng() % 8 != 0) {
            continue;
        }
        const u64 id = ids.Present(frame); // frame 0: a redraw with nothing queued
        assert(id > last);
        last = id;
        ++presents;
    }
    stage_a.join();
    std::printf("threads: %llu presents, strictly increasing\n",
                static_cast<unsigned long long>(presents));
}

} // namespace

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    SteadyState();
    NonGameBetween();
    for (u64 seed = 1; seed <= 8; ++seed) {
        Random(seed);
    }
    Threads();
    std::printf("reflex-ids-test: ok\n");
    return 0;
}
