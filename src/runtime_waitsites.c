/* GOW3_WAIT_SITES=1 (diagnostics): which guest code yields, sleeps or waits, how often, and how
 * long the calls take against what they asked for. The guest's yield / sleep / condition variable /
 * semaphore / event flag / mutex calls record their return address (the call site in guest code);
 * whichever thread crosses a 5 s boundary prints the sites with the most time spent since the last
 * report, as guest+offset (eboot image offsets, for Ghidra: add 0x400000). Off: one load.
 * Panning stalls (2026-10-19): the game thread spins while workers hand work to each other; timed
 * waits on Windows round up to the timer resolution, which this shows as actual >> requested. */
/* GOW3_WAIT_TRACE=<file.csv> (diagnostics, 2026-11-15): every guest wait that took at least
 * GOW3_WAIT_TRACE_MIN_US (default 500) microseconds, one row each, to line up with the flip trace's slow frames
 * (research/tools/wait_frames.py; card tooling/wait-trace). It turns on the measurement above without the 5 s
 * report (that stays GOW3_WAIT_SITES=1). Rows:
 *   wall,<Unix time ns>,<monotonic ns>                      once, the two clocks read together
 *   w,<monotonic ns at the end>,<thread id>,<guest thread>,<kind>,<site>,<requested ns>,<actual ns>
 * site: guest+0x... (eboot image offset; Ghidra: add 0x400000) or the raw address. Written under one mutex,
 * flushed every 1,024 rows: event-driven, a few hundred to ~1,000 rows a second in MediEvil's game. */
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define SLOTS 4096
#define REPORT_NS UINT64_C(5000000000)
static const char *const kind_names[] = {"yield", "pthread_yield", "usleep(0)", "usleep", "nanosleep",
                                         "cond_wait", "cond_timedwait", "sema_wait", "eventflag_wait",
                                         "mutex_lock", "equeue_wait"};
typedef struct {
    uint64_t key;   /* return address | (kind+1) << 56; 0 = empty (atomic) */
    uint64_t calls, ns, requested; /* atomic */
    uint64_t reported_calls, reported_ns, reported_requested; /* the reporting thread's */
} Site;
static Site sites[SLOTS];
static uint64_t next_report, lost; /* atomic */
static int reporting;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
uint64_t runtime_wait_now(void) { return now_ns() | 1; /* never 0: 0 means "not measured" */ }

/* GOW3_WAIT_SITES=1: the table and its 5 s report. */
static int sites_on(void) {
    static int on = -1;
    if (on < 0) { const char *value = getenv("GOW3_WAIT_SITES"); on = value && value[0] == '1'; }
    return on;
}
static const char *trace_path(void) {
    static const char *path; static int read;
    if (!read) { const char *value = getenv("GOW3_WAIT_TRACE"); path = value && value[0] ? value : NULL; read = 1; }
    return path;
}
/* Either switch measures every wait; off: one cached load per wait (RUNTIME_WAIT_BEGIN). */
int runtime_wait_sites_enabled(void) {
    if (!GOW3_DEV_TOOLS) return 0; /* release build */
    static int enabled = -1;
    if (enabled < 0) enabled = sites_on() || trace_path() != NULL;
    return enabled;
}

static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *trace_file;
static int trace_failed;
static unsigned trace_rows;
static uint64_t trace_min_ns(void) {
    static uint64_t min = 0; static int read;
    if (!read) {
        const char *value = getenv("GOW3_WAIT_TRACE_MIN_US");
        min = (uint64_t)(value && value[0] ? strtoull(value, NULL, 10) : 500) * 1000;
        read = 1;
    }
    return min;
}
static uint64_t wall_ns(void) {
#ifdef _WIN32
    FILETIME ft; GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - UINT64_C(116444736000000000)) * 100;
#else
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}
static unsigned long thread_id(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (unsigned long)syscall(SYS_gettid);
#endif
}
/* Opens the trace on first use; call with trace_lock held. 0: no file. */
static int trace_open_locked(void) {
    if (!trace_file && !trace_failed) {
        trace_file = fopen(trace_path(), "w");
        if (!trace_file) { trace_failed = 1; fprintf(stderr, "Wait trace: cannot open %s\n", trace_path()); }
        else {
            setvbuf(trace_file, NULL, _IOFBF, 1 << 20);
            const uint64_t before = now_ns(), wall = wall_ns(), after = now_ns();
            fprintf(trace_file, "wall,%llu,%llu\n", (unsigned long long)wall, (unsigned long long)(before + (after - before) / 2));
#ifdef _WIN32
            /* Host addresses in m / h / s rows: llvm-symbolizer on this exe at address - base + its ImageBase. */
            char exe[512] = "";
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            for (char *c = exe; *c; ++c) if (*c == ',') *c = ';';
            fprintf(trace_file, "exe,%s,0x%llx\n", exe, (unsigned long long)(uintptr_t)GetModuleHandleW(NULL));
#endif
            printf("Wait trace: guest waits of %llu us or more to %s%s\n", (unsigned long long)(trace_min_ns() / 1000),
                   trace_path(), runtime_mutex_holders_enabled() ? " (with mutex holders, GOW3_MUTEX_HOLDERS=1)" : "");
        }
    }
    return trace_file != NULL;
}
/* One raw row (with its newline) into the trace, for the mutex holder rows (runtime_mutex.c). */
void runtime_wait_trace_line(const char *line) {
    pthread_mutex_lock(&trace_lock);
    if (trace_open_locked()) {
        fputs(line, trace_file);
        if (++trace_rows % 1024 == 0) fflush(trace_file);
    }
    pthread_mutex_unlock(&trace_lock);
}
/* guest+0x... for an eboot address, else 0x... (host: the exe, a DLL, the driver). */
void runtime_wait_format_site(uintptr_t at, char *out, size_t size) {
    const uintptr_t image = (uintptr_t)runtime_image_base();
    if (!at) snprintf(out, size, "-");
    else if (image && at - image < 0x20000000) snprintf(out, size, "guest+0x%llx", (unsigned long long)(at - image));
    else snprintf(out, size, "0x%llx", (unsigned long long)at);
}
/* The current guest thread's name for a CSV field (commas and newlines replaced). */
void runtime_wait_thread_name(const char *name, char *out, size_t size) {
    snprintf(out, size, "%s", name ? name : "-");
    for (char *c = out; *c; ++c) if (*c == ',' || *c == '\n') *c = ';';
}
unsigned long runtime_wait_thread_id(void) { return thread_id(); }
uint64_t runtime_wait_trace_min_ns(void) { return trace_min_ns(); }
/* GOW3_MUTEX_HOLDERS=1 with GOW3_WAIT_TRACE (dev build; card tooling/mutex-holders): guest mutexes record their
 * holder, long waits name it (m rows), long holds are written (h rows), and the holder is sampled while a thread
 * waits on it (s rows, Windows). */
int runtime_mutex_holders_enabled(void) {
    if (!GOW3_DEV_TOOLS) return 0;
    static int on = -1;
    if (on < 0) { const char *value = getenv("GOW3_MUTEX_HOLDERS"); on = value && value[0] == '1' && trace_path() != NULL; }
    return on;
}

static void trace_wait(void *return_address, unsigned kind, uint64_t requested_ns, uint64_t ns) {
    const uint64_t end = now_ns();
    char thread[40], where[40];
    runtime_wait_thread_name(runtime_thread_current_name(), thread, sizeof(thread));
    runtime_wait_format_site((uintptr_t)return_address, where, sizeof(where));
    if (where[0] == '0') snprintf(where, sizeof(where), "%p", return_address); /* as before for host sites */
    pthread_mutex_lock(&trace_lock);
    if (trace_open_locked()) {
        fprintf(trace_file, "w,%llu,%lu,%s,%s,%s,%llu,%llu\n", (unsigned long long)end, thread_id(), thread,
                kind_names[kind], where, (unsigned long long)requested_ns, (unsigned long long)ns);
        if (++trace_rows % 1024 == 0) fflush(trace_file);
    }
    pthread_mutex_unlock(&trace_lock);
}

static void report(uint64_t now) {
    Site *top[16] = {0};
    uint64_t top_ns[16] = {0};
    for (unsigned i = 0; i < SLOTS; ++i) {
        Site *s = &sites[i];
        if (!__atomic_load_n(&s->key, __ATOMIC_ACQUIRE)) continue;
        const uint64_t ns = __atomic_load_n(&s->ns, __ATOMIC_RELAXED) - s->reported_ns;
        for (unsigned j = 0; j < 16; ++j) {
            if (ns > top_ns[j]) {
                for (unsigned k = 15; k > j; --k) { top[k] = top[k - 1]; top_ns[k] = top_ns[k - 1]; }
                top[j] = s; top_ns[j] = ns;
                break;
            }
        }
    }
    const uintptr_t image = (uintptr_t)runtime_image_base();
    printf("Wait sites (5 s, by time spent; calls/s, avg requested / actual): %llu lost\n",
           (unsigned long long)__atomic_load_n(&lost, __ATOMIC_RELAXED));
    for (unsigned j = 0; j < 16 && top[j]; ++j) {
        Site *s = top[j];
        const uint64_t key = __atomic_load_n(&s->key, __ATOMIC_ACQUIRE),
                       calls = __atomic_load_n(&s->calls, __ATOMIC_RELAXED) - s->reported_calls,
                       requested = __atomic_load_n(&s->requested, __ATOMIC_RELAXED) - s->reported_requested;
        const uintptr_t at = (uintptr_t)(key & ((UINT64_C(1) << 56) - 1));
        const unsigned kind = (unsigned)(key >> 56) - 1;
        char where[40];
        if (image && at - image < 0x20000000) snprintf(where, sizeof(where), "guest+0x%llx", (unsigned long long)(at - image));
        else snprintf(where, sizeof(where), "%p", (void *)at);
        printf("  %8.0f/s  %-14s %-20s %8.1f ms total, avg %.1f / %.1f us\n", calls / 5.0, kind_names[kind], where,
               top_ns[j] / 1e6, calls ? requested / 1000.0 / calls : 0.0, calls ? top_ns[j] / 1000.0 / calls : 0.0);
    }
    for (unsigned i = 0; i < SLOTS; ++i) {
        sites[i].reported_calls = __atomic_load_n(&sites[i].calls, __ATOMIC_RELAXED);
        sites[i].reported_ns = __atomic_load_n(&sites[i].ns, __ATOMIC_RELAXED);
        sites[i].reported_requested = __atomic_load_n(&sites[i].requested, __ATOMIC_RELAXED);
    }
    (void)now;
}

void runtime_wait_site(void *return_address, unsigned kind, uint64_t requested_ns, uint64_t ns) {
    if (trace_path()) {
        runtime_thread_sampler_start(); /* GOW3_SAMPLE_THREADS (runtime_mutex.c) */
        if (ns >= trace_min_ns()) trace_wait(return_address, kind, requested_ns, ns);
    }
    if (!sites_on()) return;
    const uint64_t key = (uint64_t)(uintptr_t)return_address | (uint64_t)(kind + 1) << 56;
    unsigned i = (unsigned)((key * UINT64_C(0x9E3779B97F4A7C15)) >> 52) & (SLOTS - 1);
    for (unsigned probe = 0; probe < 64; ++probe, i = (i + 1) & (SLOTS - 1)) {
        uint64_t seen = __atomic_load_n(&sites[i].key, __ATOMIC_ACQUIRE);
        if (!seen) {
            uint64_t empty = 0;
            if (__atomic_compare_exchange_n(&sites[i].key, &empty, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) seen = key;
            else seen = empty;
        }
        if (seen != key) continue;
        __atomic_fetch_add(&sites[i].calls, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sites[i].ns, ns, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sites[i].requested, requested_ns, __ATOMIC_RELAXED);
        goto counted;
    }
    __atomic_fetch_add(&lost, 1, __ATOMIC_RELAXED);
counted:;
    const uint64_t now = now_ns(), due = __atomic_load_n(&next_report, __ATOMIC_RELAXED);
    if (!due) { uint64_t zero = 0; __atomic_compare_exchange_n(&next_report, &zero, now + REPORT_NS, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); return; }
    if (now < due) return;
    int idle = 0;
    if (!__atomic_compare_exchange_n(&reporting, &idle, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (__atomic_load_n(&next_report, __ATOMIC_RELAXED) == due) {
        __atomic_store_n(&next_report, now + REPORT_NS, __ATOMIC_RELAXED);
        report(now);
    }
    __atomic_store_n(&reporting, 0, __ATOMIC_RELEASE);
}
