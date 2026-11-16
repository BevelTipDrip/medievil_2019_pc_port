// gow3: parallel copies of guest memory. Streaming a new area uploads 50-400 MB of textures
// and buffers per frame; copied on one thread that is tens of milliseconds (stutter).
#pragma once
#include <cstddef>
#include <functional>

namespace Gow3Copy {

/// Runs `task(i)` for every i in [0, count) on the copy threads and the caller; returns when
/// all are done. Runs inline when count is 1, the pool is disabled, or when called from a copy
/// thread (no nesting).
void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task);

/// True when ParallelFor would split work (more than one thread available).
bool Enabled();

/// Starts `task` on a copy thread now (inline when the pool is disabled). Tasks run in any
/// order and concurrently.
void Async(std::function<void()> task);

/// A small copy for QueueCopy(): `run(item)` does the work on a copy thread.
struct Item {
    void (*run)(const Item&);
    void* context;
    unsigned long long source, destination, size, extra;
};

/// Queues a small copy in the calling thread's batch; the batch is started with Async() once it
/// holds about 512 KiB or 256 items, or by FlushBatch()/WaitAsync(). One wakeup per batch
/// instead of one per copy (thousands of constant buffer copies per frame).
void QueueCopy(const Item& item);

/// Starts the calling thread's batch.
void FlushBatch();

/// Starts the calling thread's batch, then waits until every task passed to Async() so far
/// has finished.
void WaitAsync();

/// Starts the calling thread's batch and runs `callback` once every task passed to Async() so
/// far has finished (at once when none is pending), on whichever thread finishes last.
/// Callbacks run in the order they were registered.
void AfterCopies(std::function<void()> callback);

/// gow3 copy-fault trace (2026-11-15): the guest range the calling thread's host copy is reading
/// (CopySparseMemory), size 0 outside one. The guest fault handler (page_manager.cpp) logs a fault
/// inside a copy: on the Vulkan recorder or a copy thread it is handed to the GPU command thread
/// and waited for, which deadlocked a 200% render scale level load at 4K. `queued_frame`: the
/// frames the game had ended (decode pacing) when a deferred copy was recorded; 0: not known.
struct ReadingGuest {
    unsigned long long source = 0, size = 0, queued_frame = 0;
};
extern thread_local ReadingGuest reading_guest;

/// GOW3_COPY_FAULT_LOG (dev build: default on, 0 off; release build: default off, 1 on; off, the scopes below
/// do nothing). Read once at start.
extern const bool fault_log_enabled;

/// Sets reading_guest's range for its lifetime.
struct ReadingGuestScope {
    ReadingGuest saved;
    ReadingGuestScope(unsigned long long source, unsigned long long size) {
        if (fault_log_enabled) {
            saved = reading_guest;
            reading_guest.source = source;
            reading_guest.size = size;
        }
    }
    ~ReadingGuestScope() {
        if (fault_log_enabled) {
            reading_guest = saved;
        }
    }
};

/// Sets reading_guest's queued_frame for its lifetime (a deferred copy being run).
struct QueuedFrameScope {
    unsigned long long saved = 0;
    explicit QueuedFrameScope(unsigned long long frame) {
        if (fault_log_enabled) {
            saved = reading_guest.queued_frame;
            reading_guest.queued_frame = frame;
        }
    }
    ~QueuedFrameScope() {
        if (fault_log_enabled) {
            reading_guest.queued_frame = saved;
        }
    }
};

} // namespace Gow3Copy
