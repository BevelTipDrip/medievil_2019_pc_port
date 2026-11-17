/* Guest mutexes and condition variables (src/runtime_mutex.c): the PS4 semantics the game relies on, a
 * per-operation benchmark, and how long timed condition waits really last.
 * Standalone (only runtime_mutex.c): ninja -C out/gpu mutex-test.
 *   mutex-test           semantics (exit 0 = all pass)
 *   mutex-test --bench   ns per operation (for before / after comparisons)
 *   mutex-test --timing  timed waits that expire: requested vs actual, and the waiting thread's CPU use
 *   mutex-test --handoff today's mutex vs an SRWLOCK prototype: hand-off latency, contention with work (2b)
 * Run under the switches the game uses (GOW3_FUTEX_COND, GOW3_COND_PRECISE, GOW3_COND_SPIN_US): each is read
 * once per process. --timing's "ue4 1 ms" row is UE4's FPThreadEvent::Wait(1): a deadline 1 ms after the
 * current time cut down to whole ms (the game thread's waits at guest+0x70df3a, 2026-11-15). */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#undef NDEBUG
/* runtime_mutex.c reports guest wait sites; off here. */
int runtime_wait_sites_enabled(void) { return 0; }
uint64_t runtime_wait_now(void) { /* the holder rows' clock (wait sites stay off: runtime_wait_sites_enabled is 0) */
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return ((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec) | 1;
}
void runtime_wait_site(void *a, unsigned k, uint64_t r, uint64_t n) { (void)a; (void)k; (void)r; (void)n; }
int runtime_fault_stats_enabled(void) { return 0; } /* lock counters off, as in a normal run */
/* GOW3_MUTEX_HOLDERS (runtime_waitsites.c): off except in holder_rows(), whose rows land in holder_log. */
static atomic_int holders_on;
static char holder_log[1 << 16];
static size_t holder_log_used;
static pthread_mutex_t holder_log_lock = PTHREAD_MUTEX_INITIALIZER;
int runtime_mutex_holders_enabled(void) { return atomic_load(&holders_on); }
void runtime_wait_trace_line(const char *line) {
    pthread_mutex_lock(&holder_log_lock);
    const size_t n = strlen(line);
    if (holder_log_used + n < sizeof(holder_log)) { memcpy(holder_log + holder_log_used, line, n + 1); holder_log_used += n; }
    pthread_mutex_unlock(&holder_log_lock);
}
void runtime_wait_format_site(uintptr_t at, char *out, size_t size) { snprintf(out, size, "0x%llx", (unsigned long long)at); }
void runtime_wait_thread_name(const char *name, char *out, size_t size) { snprintf(out, size, "%s", name ? name : "-"); }
unsigned long runtime_wait_thread_id(void) {
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return (unsigned long)pthread_self();
#endif
}
uint64_t runtime_wait_trace_min_ns(void) { return 500000; }
const char *runtime_thread_current_name(void) { return NULL; }
unsigned char *runtime_image_base(void) { return NULL; } /* no game image here: return addresses into the exe only */

typedef struct { int64_t sec, nsec; } GuestTime;
typedef int32_t (ABI *Op)(void **);
typedef int32_t (ABI *Init)(void **, void **, const char *);
typedef int32_t (ABI *PosixInit)(void **, void **);
typedef int32_t (ABI *AttrType)(void **, int);
typedef int32_t (ABI *TimedLock)(void **, uint32_t);
typedef int32_t (ABI *CondWait)(void **, void **);
typedef int32_t (ABI *CondTimed)(void **, void **, uint32_t);
typedef int32_t (ABI *CondAbs)(void **, void **, const GuestTime *);
static Op lock, trylock, unlock, destroy, attr_init, attr_destroy, cdestroy, csignal, cbroadcast;
static Op plock, punlock, pcsignal;
static Init init, cinit;
static PosixInit pinit, pcinit;
static AttrType attr_type;
static TimedLock timedlock;
static CondWait cond_wait_, pcond_wait_;
static CondTimed ctimed;
static CondAbs pctimed;
#define OK 0
#define EBUSY_ 0x80020010u
#define EDEADLK_ 0x8002000bu
#define EPERM_ 0x80020001u
#define EINVAL_ 0x80020016u
#define ETIMEDOUT_ 0x8002003cu
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void *fn(const char *nid) {
    void *f = (void *)runtime_mutex_resolve(nid);
    if (!f) { fprintf(stderr, "FAIL: %s not resolved\n", nid); exit(1); }
    return f;
}
static void setup(void) {
    attr_init = (Op)fn("F8bUHwAG284#libkernel");
    attr_type = (AttrType)fn("iMp8QpE+XO4#libkernel");
    attr_destroy = (Op)fn("smWEktiyyG0#libkernel");
    init = (Init)fn("cmo1RIYva9o#libkernel");
    lock = (Op)fn("9UK1vLZQft4#libkernel");
    trylock = (Op)fn("upoVrzMHFeE#libkernel");
    unlock = (Op)fn("tn3VlD0hG60#libkernel");
    destroy = (Op)fn("2Of0f+3mhhE#libkernel");
    timedlock = (TimedLock)fn("IafI2PxcPnQ#libkernel");
    cinit = (Init)fn("2Tb92quprl0#libkernel");
    cdestroy = (Op)fn("g+PZd2hiacg#libkernel");
    cond_wait_ = (CondWait)fn("WKAXJ4XBPQ4#libkernel");
    ctimed = (CondTimed)fn("BmMjYxmew1w#libkernel");
    csignal = (Op)fn("kDh-NfxgMtE#libkernel");
    cbroadcast = (Op)fn("JGgj7Uvrl+A#libkernel");
    pinit = (PosixInit)fn("ttHNfU+qDBU#libkernel");
    plock = (Op)fn("7H0iTOciTLo#libkernel");
    punlock = (Op)fn("2Z+PpY6CaJg#libkernel");
    pcinit = (PosixInit)fn("0TyVk4MSLt0#libkernel");
    pcond_wait_ = (CondWait)fn("Op8TBGY5KHg#libkernel");
    pctimed = (CondAbs)fn("27bAgiJmOh0#libkernel");
    pcsignal = (Op)fn("2MOy+rUfuhQ#libkernel");
}

static int64_t mono_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static GuestTime realtime_after(int64_t ns) {
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    int64_t n = t.tv_nsec + ns;
    return (GuestTime){ t.tv_sec + n / 1000000000, n % 1000000000 };
}
static void *make(int type) {
    void *attr, *m;
    CHECK(attr_init(&attr) == OK && attr_type(&attr, type) == OK);
    CHECK(init(&m, &attr, "mutex-test") == OK);
    CHECK(attr_destroy(&attr) == OK);
    return m;
}

/* ---- semantics ---- */
static void *other_unlock(void *m) { return (void *)(intptr_t)unlock((void **)m); }
static void *other_trylock(void *m) { return (void *)(intptr_t)trylock((void **)m); }
static void mutex_types(void) {
    void *m = make(1); /* errorcheck (the default) */
    CHECK(lock(&m) == OK);
    CHECK((uint32_t)lock(&m) == EDEADLK_);
    CHECK((uint32_t)trylock(&m) == EBUSY_);
    pthread_t t; void *r;
    CHECK(pthread_create(&t, NULL, other_unlock, &m) == 0 && pthread_join(t, &r) == 0);
    CHECK((uint32_t)(intptr_t)r == EPERM_);
    CHECK(pthread_create(&t, NULL, other_trylock, &m) == 0 && pthread_join(t, &r) == 0);
    CHECK((uint32_t)(intptr_t)r == EBUSY_);
    CHECK((uint32_t)destroy(&m) == EBUSY_);
    CHECK(unlock(&m) == OK);
    CHECK((uint32_t)unlock(&m) == EPERM_);
    CHECK(destroy(&m) == OK);
    CHECK((uintptr_t)m == 2);
    CHECK((uint32_t)lock(&m) == EINVAL_ && (uint32_t)unlock(&m) == EINVAL_ && (uint32_t)destroy(&m) == EINVAL_);

    m = make(2); /* recursive */
    CHECK(lock(&m) == OK && lock(&m) == OK && trylock(&m) == OK);
    CHECK(pthread_create(&t, NULL, other_trylock, &m) == 0 && pthread_join(t, &r) == 0);
    CHECK((uint32_t)(intptr_t)r == EBUSY_);
    CHECK(unlock(&m) == OK && unlock(&m) == OK && unlock(&m) == OK);
    CHECK((uint32_t)unlock(&m) == EPERM_);
    /* Not checked: destroying a recursive mutex its own thread still holds. FreeBSD says EBUSY; this runtime
     * (its trylock succeeds by recursion) destroys it (2026-11-15). */
    CHECK(destroy(&m) == OK);

    m = make(3); /* normal */
    CHECK(lock(&m) == OK && (uint32_t)trylock(&m) == EBUSY_ && unlock(&m) == OK && destroy(&m) == OK);
    m = make(4); /* adaptive: error-checked as the default */
    CHECK(lock(&m) == OK && (uint32_t)lock(&m) == EDEADLK_ && unlock(&m) == OK && destroy(&m) == OK);

    void *attr;
    CHECK(attr_init(&attr) == OK);
    CHECK((uint32_t)attr_type(&attr, 0) == EINVAL_ && (uint32_t)attr_type(&attr, 5) == EINVAL_);
    CHECK(attr_destroy(&attr) == OK);
    CHECK((uint32_t)init(NULL, NULL, NULL) == EINVAL_);
    CHECK((uint32_t)lock(NULL) == EINVAL_);
}
/* Statically initialized handles (0 and 1) are created on first use; once, with many threads racing. */
static void *static_racer(void *m) {
    for (int i = 0; i < 1000; ++i) { CHECK(lock((void **)m) == OK); CHECK(unlock((void **)m) == OK); }
    return NULL;
}
static void static_handles(void) {
    void *zero = NULL, *one = (void *)(uintptr_t)1;
    CHECK((uint32_t)unlock(&zero) == EPERM_);
    CHECK(lock(&zero) == OK && (uintptr_t)zero > 2 && unlock(&zero) == OK && destroy(&zero) == OK);
    pthread_t t[8];
    for (int i = 0; i < 8; ++i) CHECK(pthread_create(&t[i], NULL, static_racer, &one) == 0);
    for (int i = 0; i < 8; ++i) CHECK(pthread_join(t[i], NULL) == 0);
    CHECK((uintptr_t)one > 2 && destroy(&one) == OK);
    void *c = NULL;
    CHECK(csignal(&c) == OK && (uintptr_t)c > 2 && cdestroy(&c) == OK);
}
/* timedlock: succeeds at once when free, times out when held elsewhere, gets it when released in time. */
typedef struct { void **m; int hold_ms; } Holder;
static atomic_int held;
static void *holder(void *p) {
    Holder *h = p;
    CHECK(lock(h->m) == OK);
    atomic_store(&held, 1);
    struct timespec d = { 0, h->hold_ms * 1000000L };
    nanosleep(&d, NULL);
    CHECK(unlock(h->m) == OK);
    return NULL;
}
static void timed_lock(void) {
    void *m = make(1);
    CHECK(timedlock(&m, 1000) == OK && unlock(&m) == OK);
    for (int round = 0; round < 2; ++round) {
        Holder h = { &m, round ? 20 : 200 };
        atomic_store(&held, 0);
        pthread_t t;
        CHECK(pthread_create(&t, NULL, holder, &h) == 0);
        while (!atomic_load(&held)) sched_yield();
        int64_t t0 = mono_ns();
        int32_t e = timedlock(&m, 50000);
        int64_t took = mono_ns() - t0;
        if (round == 0) CHECK((uint32_t)e == ETIMEDOUT_ && took >= 49000000);
        else { CHECK(e == OK && took < 50000000); CHECK(unlock(&m) == OK); }
        CHECK(pthread_join(t, NULL) == 0);
    }
    CHECK(destroy(&m) == OK);
}
/* Condition variables: signal wakes one waiter, broadcast all, timed waits time out with the mutex held. */
typedef struct { void *m, *c; int ready, woken, go; } Shared;
static void *waiter(void *p) {
    Shared *s = p;
    CHECK(lock(&s->m) == OK);
    ++s->ready;
    while (!s->go) CHECK(cond_wait_(&s->c, &s->m) == OK);
    --s->go; ++s->woken;
    CHECK(unlock(&s->m) == OK);
    return NULL;
}
static void wait_ready(Shared *s, int n) {
    for (;;) {
        CHECK(lock(&s->m) == OK);
        int r = s->ready;
        CHECK(unlock(&s->m) == OK);
        if (r == n) return;
        sched_yield();
    }
}
static void conditions(void) {
    Shared s = { make(1), NULL, 0, 0, 0 };
    CHECK(cinit(&s.c, NULL, "cond-test") == OK);
    pthread_t t[4];
    for (int i = 0; i < 4; ++i) CHECK(pthread_create(&t[i], NULL, waiter, &s) == 0);
    wait_ready(&s, 4);
    CHECK(lock(&s.m) == OK); s.go = 1; CHECK(csignal(&s.c) == OK); CHECK(unlock(&s.m) == OK);
    for (int spins = 0; ; ++spins) {
        CHECK(lock(&s.m) == OK); int w = s.woken; CHECK(unlock(&s.m) == OK);
        if (w == 1) break;
        CHECK(spins < 5000000); sched_yield();
    }
    CHECK(lock(&s.m) == OK); s.go = 3; CHECK(cbroadcast(&s.c) == OK); CHECK(unlock(&s.m) == OK);
    for (int i = 0; i < 4; ++i) CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(s.woken == 4);

    /* Timed waits time out (sce: usec; POSIX: absolute CLOCK_REALTIME) holding the mutex again. */
    CHECK(lock(&s.m) == OK);
    int64_t t0 = mono_ns();
    int32_t e; /* an early return (0) is a spurious wakeup: callers wait again */
    while ((e = ctimed(&s.c, &s.m, 3000)) == OK) {}
    CHECK((uint32_t)e == ETIMEDOUT_);
    CHECK(mono_ns() - t0 >= 2900000);
    CHECK((uint32_t)trylock(&s.m) == EBUSY_);
    GuestTime past = realtime_after(-1000000), soon = realtime_after(2000000);
    CHECK(pctimed(&s.c, &s.m, &past) == 60);
    t0 = mono_ns();
    while ((e = pctimed(&s.c, &s.m, &soon)) == 0) {}
    CHECK(e == 60);
    CHECK(mono_ns() - t0 >= 1900000);
    GuestTime bad = { 0, 1000000000 };
    CHECK(pctimed(&s.c, &s.m, &bad) == 22);
    CHECK(unlock(&s.m) == OK);
    void *unset = NULL;
    CHECK((uint32_t)cond_wait_(&s.c, &unset) == EINVAL_);
    CHECK(cdestroy(&s.c) == OK && destroy(&s.m) == OK);
}
/* Timed waits woken before their deadline return 0, not a timeout. */
static void *late_signal(void *p) {
    Shared *s = p;
    struct timespec d = { 0, 5000000 };
    nanosleep(&d, NULL);
    CHECK(lock(&s->m) == OK); s->go = 1; CHECK(csignal(&s->c) == OK); CHECK(unlock(&s->m) == OK);
    return NULL;
}
static void timed_wake(void) {
    Shared s = { make(1), NULL, 0, 0, 0 };
    CHECK(cinit(&s.c, NULL, "cond-test") == OK);
    pthread_t t;
    CHECK(lock(&s.m) == OK);
    CHECK(pthread_create(&t, NULL, late_signal, &s) == 0);
    int64_t t0 = mono_ns();
    while (!s.go) CHECK(ctimed(&s.c, &s.m, 2000000) == OK);
    CHECK(mono_ns() - t0 < 1000000000);
    CHECK(unlock(&s.m) == OK && pthread_join(t, NULL) == 0);
    CHECK(cdestroy(&s.c) == OK && destroy(&s.m) == OK);
}
/* Producers and consumers through one mutex and two conditions: nothing lost, nothing doubled. */
#define ITEMS 200000
typedef struct { void *m, *not_empty, *not_full; int queue, produced, consumed; long long sum; } Pc;
static void *producer(void *p) {
    Pc *s = p;
    for (;;) {
        CHECK(plock(&s->m) == OK);
        while (s->queue >= 8 && s->produced < ITEMS) CHECK(pcond_wait_(&s->not_full, &s->m) == OK);
        if (s->produced >= ITEMS) { CHECK(punlock(&s->m) == OK); return NULL; }
        ++s->queue; ++s->produced;
        CHECK(pcsignal(&s->not_empty) == OK);
        CHECK(punlock(&s->m) == OK);
    }
}
static void *consumer(void *p) {
    Pc *s = p;
    for (;;) {
        CHECK(plock(&s->m) == OK);
        while (!s->queue && s->consumed < ITEMS) {
            GuestTime d = realtime_after(200000); /* timed, as the game's pools wait */
            int32_t e = pctimed(&s->not_empty, &s->m, &d);
            CHECK(e == 0 || e == 60);
        }
        if (s->consumed >= ITEMS) { CHECK(punlock(&s->m) == OK); return NULL; }
        --s->queue; ++s->consumed; s->sum += s->consumed;
        CHECK(pcsignal(&s->not_full) == OK);
        CHECK(punlock(&s->m) == OK);
    }
}
static void stress(void) {
    Pc s = { 0 };
    CHECK(pinit(&s.m, NULL) == OK && pcinit(&s.not_empty, NULL) == OK && pcinit(&s.not_full, NULL) == OK);
    pthread_t t[8];
    for (int i = 0; i < 4; ++i) CHECK(pthread_create(&t[i], NULL, producer, &s) == 0);
    for (int i = 4; i < 8; ++i) CHECK(pthread_create(&t[i], NULL, consumer, &s) == 0);
    for (int i = 0; i < 4; ++i) CHECK(pthread_join(t[i], NULL) == 0);
    for (;;) { /* producers done: wake the consumers out of their timed waits until the queue drains */
        CHECK(plock(&s.m) == OK);
        int done = s.consumed >= ITEMS;
        CHECK(pcsignal(&s.not_empty) == OK);
        CHECK(punlock(&s.m) == OK);
        if (done) break;
        sched_yield();
    }
    for (int i = 4; i < 8; ++i) {
        CHECK(plock(&s.m) == OK); CHECK(pcsignal(&s.not_empty) == OK); CHECK(punlock(&s.m) == OK);
        CHECK(pthread_join(t[i], NULL) == 0);
    }
    CHECK(s.produced == ITEMS && s.consumed == ITEMS && s.queue == 0);
    CHECK(s.sum == (long long)ITEMS * (ITEMS + 1) / 2);
}

/* ---- GOW3_MUTEX_HOLDERS rows (dev build) ---- */
static void *hold_20ms(void *p) {
    void **m = p;
    CHECK(lock(m) == OK);
    atomic_store(&holders_on, 2); /* "holding": the main thread may wait now (still on: nonzero) */
    const int64_t end = mono_ns() + 20000000; /* long enough for a few samples, the first one slow (cold DLL lookups) */
    while (mono_ns() < end) {}
    CHECK(unlock(m) == OK);
    return NULL;
}
static void holder_rows(void) {
#if GOW3_DEV_TOOLS
    atomic_store(&holders_on, 1);
    void *m = make(1);
    pthread_t t;
    pthread_create(&t, NULL, hold_20ms, &m);
    while (atomic_load(&holders_on) != 2) {}
    CHECK(lock(&m) == OK);   /* waits ~20 ms for the holder */
    CHECK(unlock(&m) == OK);
    pthread_join(t, NULL);
    /* A hold through a condition wait: the hold ends at the wait (site cond_wait), no h row for the wait itself. */
    void *c = NULL;
    CHECK(cinit(&c, NULL, "holders") == OK);
    CHECK(lock(&m) == OK);
    CHECK(ctimed(&c, &m, 2000) == (int32_t)ETIMEDOUT_);
    CHECK(unlock(&m) == OK);
    cdestroy(&c);
    atomic_store(&holders_on, 0);
    destroy(&m);
    char *mrow = strstr(holder_log, "m,"), *hrow = strstr(holder_log, "h,");
    CHECK(mrow && hrow);
    unsigned long long end, waiter, holder, held, waited, hend, htid, hheld;
    char wname[40], wsite[40], hname[40], hsite[40], tname[40], lsite[40], usite[40];
    CHECK(sscanf(mrow, "m,%llu,%llu,%39[^,],%39[^,],%llu,%39[^,],%39[^,],%llu,%llu", &end, &waiter, wname, wsite, &holder,
                 hname, hsite, &held, &waited) == 9);
    CHECK(holder != 0 && holder != waiter && waited >= 2000000 && strcmp(hsite, "-"));
    CHECK(sscanf(hrow, "h,%llu,%llu,%39[^,],%39[^,],%39[^,],%llu", &hend, &htid, tname, lsite, usite, &hheld) == 6);
    CHECK(htid == holder && hheld >= 15000000);
#ifdef _WIN32
    /* The holder was sampled while it spun, and the unwinder reached hold_20ms in the exe (the sampler writes after
     * its unwinding: allow it up to 200 ms). */
    int sampled = 0;
    for (int i = 0; i < 200 && !sampled; ++i) {
        pthread_mutex_lock(&holder_log_lock);
        const char *s = strstr(holder_log, "\ns,");
        sampled = s && strstr(s, ",exe+0x") != NULL;
        pthread_mutex_unlock(&holder_log_lock);
        if (!sampled) Sleep(1);
    }
    CHECK(sampled);
#endif
    CHECK(!strstr(holder_log, ",cond_wait,cond_wait,")); /* a 2 ms condition wait is not a hold */
    printf("  mutex holder rows: ok (%zu bytes)\n", holder_log_used);
    if (getenv("MUTEX_TEST_SHOW_ROWS")) fputs(holder_log, stdout);
#endif
}

/* ---- benchmark ---- */
static void bench_uncontended(void) {
    void *m = make(1), *c = NULL;
    CHECK(cinit(&c, NULL, "bench") == OK);
    enum { N = 2000000 };
    int64_t t0 = mono_ns();
    for (int i = 0; i < N; ++i) { lock(&m); unlock(&m); }
    int64_t t1 = mono_ns();
    for (int i = 0; i < N; ++i) { trylock(&m); unlock(&m); }
    int64_t t2 = mono_ns();
    for (int i = 0; i < N; ++i) csignal(&c);
    int64_t t3 = mono_ns();
    printf("  uncontended lock+unlock      %7.1f ns\n", (double)(t1 - t0) / N);
    printf("  uncontended trylock+unlock   %7.1f ns\n", (double)(t2 - t1) / N);
    printf("  signal, no waiter            %7.1f ns\n", (double)(t3 - t2) / N);
    cdestroy(&c); destroy(&m);
}
typedef struct { void *m; atomic_int *go; int n; } Contend;
static void *contender(void *p) {
    Contend *k = p;
    while (!atomic_load(k->go)) sched_yield();
    for (int i = 0; i < k->n; ++i) { lock(&k->m); unlock(&k->m); }
    return NULL;
}
static void bench_contended(int threads) {
    atomic_int go = 0;
    Contend k = { make(1), &go, 200000 };
    pthread_t t[16];
    for (int i = 0; i < threads; ++i) pthread_create(&t[i], NULL, contender, &k);
    int64_t t0 = mono_ns();
    atomic_store(&go, 1);
    for (int i = 0; i < threads; ++i) pthread_join(t[i], NULL);
    printf("  %2d threads lock+unlock       %7.1f ns per pair (all threads)\n", threads,
           (double)(mono_ns() - t0) / ((double)k.n * threads));
    destroy(&k.m);
}
/* Two threads handing a token back and forth through a condition variable: a wake's full round trip. */
typedef struct { void *m, *c; int turn, rounds; } PingPong;
static void *ponger(void *p) {
    PingPong *s = p;
    lock(&s->m);
    for (int i = 0; i < s->rounds; ++i) {
        while (s->turn != 1) cond_wait_(&s->c, &s->m);
        s->turn = 0; cbroadcast(&s->c);
    }
    unlock(&s->m);
    return NULL;
}
static void bench_pingpong(void) {
    PingPong s = { make(1), NULL, 0, 50000 };
    cinit(&s.c, NULL, "pingpong");
    pthread_t t;
    pthread_create(&t, NULL, ponger, &s);
    int64_t t0 = mono_ns();
    lock(&s.m);
    for (int i = 0; i < s.rounds; ++i) {
        s.turn = 1; cbroadcast(&s.c);
        while (s.turn != 0) cond_wait_(&s.c, &s.m);
    }
    unlock(&s.m);
    int64_t t1 = mono_ns();
    pthread_join(t, NULL);
    printf("  cond ping-pong round trip    %7.0f ns\n", (double)(t1 - t0) / s.rounds);
    cdestroy(&s.c); destroy(&s.m);
}

/* ---- change 2b, measure first: today's mutex vs an SRWLOCK one (plan-work-queue.md) ----
 * --handoff runs the same three measurements on both:
 *   hand-off: a thread holds the lock 300 us (the waiter is asleep by then), notes the time and unlocks; the
 *     waiter notes the time it has the lock. Unlock -> acquire latency: median, p90, p99, max.
 *   work: N threads each lock, spin ~0.3 us inside, unlock, spin ~2 us outside (closer to the game than the
 *     tight loop above). ns per lock+unlock pair over all threads, and the slowest thread's share.
 *   tight: lock+unlock in a loop (as bench_contended).
 * The SRWLOCK mutex is a prototype here only: an owner field for ERRORCHECK / RECURSIVE as 2b would need. */
static int cmp64(const void *a, const void *b);
typedef struct { Op lock, unlock; void *(*make)(void); void (*destroy)(void **); const char *name; } Impl;
static void *make_runtime(void) { return make(1); }
static void destroy_runtime(void **m) { destroy(m); }
#ifdef _WIN32
typedef struct { SRWLOCK l; DWORD owner; int type; unsigned depth; } SrwMutex;
static ABI int32_t srw_lock(void **p) {
    SrwMutex *m = *p;
    const DWORD self = GetCurrentThreadId();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) {
        if (m->type == 2) { ++m->depth; return 0; }
        return (int32_t)EDEADLK_;
    }
    AcquireSRWLockExclusive(&m->l);
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
    m->depth = 1;
    return 0;
}
static ABI int32_t srw_unlock(void **p) {
    SrwMutex *m = *p;
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != GetCurrentThreadId()) return (int32_t)EPERM_;
    if (--m->depth) return 0;
    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);
    ReleaseSRWLockExclusive(&m->l);
    return 0;
}
static void *make_srw(void) {
    SrwMutex *m = calloc(1, sizeof(*m));
    InitializeSRWLock(&m->l); m->type = 1;
    return m;
}
static void destroy_srw(void **m) { free(*m); *m = NULL; }
#endif
static void spin_ns(int64_t ns) { const int64_t end = mono_ns() + ns; while (mono_ns() < end) {} }

typedef struct { const Impl *impl; void *m; atomic_int phase; int64_t t_unlock, *lat; int rounds; } Handoff;
static void *handoff_waiter(void *p) {
    Handoff *h = p;
    for (int i = 0; i < h->rounds; ++i) {
        while (atomic_load(&h->phase) != 1) {}       /* the holder has the lock */
        atomic_store(&h->phase, 2);
        h->impl->lock(&h->m);                         /* blocks */
        const int64_t t = mono_ns();
        h->lat[i] = t - h->t_unlock;
        h->impl->unlock(&h->m);
        atomic_store(&h->phase, 3);
    }
    return NULL;
}
static void bench_handoff(const Impl *impl, int64_t hold_ns) {
    enum { ROUNDS = 400 };
    static int64_t lat[ROUNDS];
    Handoff h = { impl, impl->make(), 0, 0, lat, ROUNDS };
    pthread_t t;
    pthread_create(&t, NULL, handoff_waiter, &h);
    for (int i = 0; i < ROUNDS; ++i) {
        impl->lock(&h.m);
        atomic_store(&h.phase, 1);
        while (atomic_load(&h.phase) != 2) {}
        spin_ns(hold_ns);                             /* the waiter goes to sleep in lock */
        h.t_unlock = mono_ns();
        impl->unlock(&h.m);
        while (atomic_load(&h.phase) != 3) {}
        atomic_store(&h.phase, 0);
    }
    pthread_join(t, NULL);
    qsort(lat, ROUNDS, sizeof(lat[0]), cmp64);
    printf("  %-9s hand-off after a %3lld us hold: median %6.1f us, p90 %6.1f, p99 %6.1f, max %6.1f\n", impl->name,
           (long long)(hold_ns / 1000), lat[ROUNDS / 2] / 1e3, lat[ROUNDS * 9 / 10] / 1e3, lat[ROUNDS * 99 / 100] / 1e3,
           lat[ROUNDS - 1] / 1e3);
    impl->destroy(&h.m);
}
typedef struct { const Impl *impl; void *m; atomic_int *go; int n; int64_t inside, outside, took; } Worker;
static void *worker(void *p) {
    Worker *w = p;
    while (!atomic_load(w->go)) {}
    const int64_t t0 = mono_ns();
    for (int i = 0; i < w->n; ++i) {
        w->impl->lock(&w->m);
        if (w->inside) spin_ns(w->inside);
        w->impl->unlock(&w->m);
        if (w->outside) spin_ns(w->outside);
    }
    w->took = mono_ns() - t0;
    return NULL;
}
static void bench_work(const Impl *impl, int threads, int64_t inside, int64_t outside, int n, const char *label) {
    atomic_int go = 0;
    void *m = impl->make();
    Worker w[16];
    pthread_t t[16];
    for (int i = 0; i < threads; ++i) {
        w[i] = (Worker){ impl, m, &go, n, inside, outside, 0 };
        pthread_create(&t[i], NULL, worker, &w[i]);
    }
    const int64_t t0 = mono_ns();
    atomic_store(&go, 1);
    int64_t slowest = 0, fastest = INT64_MAX;
    for (int i = 0; i < threads; ++i) {
        pthread_join(t[i], NULL);
        if (w[i].took > slowest) slowest = w[i].took;
        if (w[i].took < fastest) fastest = w[i].took;
    }
    const int64_t all = mono_ns() - t0;
    printf("  %-9s %-5s %d threads: %7.1f ns per pair (all threads), slowest thread %.2fx the fastest\n", impl->name,
           label, threads, (double)all / ((double)n * threads), (double)slowest / (double)fastest);
    impl->destroy(&m);
}
static void handoff_compare(void) {
    const Impl impls[] = {
        { lock, unlock, make_runtime, destroy_runtime, "today" },
#ifdef _WIN32
        { srw_lock, srw_unlock, make_srw, destroy_srw, "SRWLOCK" },
#endif
    };
    const int count = (int)(sizeof(impls) / sizeof(impls[0]));
    for (int round = 0; round < 2; ++round) {         /* twice: the first pass warms up */
        printf("pass %d\n", round + 1);
        for (int i = 0; i < count; ++i) bench_handoff(&impls[i], 300000);
        for (int i = 0; i < count; ++i) bench_handoff(&impls[i], 2000000);
        for (int n = 2; n <= 8; n *= 2)
            for (int i = 0; i < count; ++i) bench_work(&impls[i], n, 300, 2000, 20000, "work");
        for (int n = 1; n <= 8; n *= 2) /* 1 thread: uncontended */
            for (int i = 0; i < count; ++i) bench_work(&impls[i], n, 0, 0, n == 1 ? 2000000 : 200000, "tight");
    }
}

/* ---- timed waits that expire ---- */
static uint64_t thread_cycles(void) {
#ifdef _WIN32
    ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return c;
#else
    struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
#endif
}
static double cycles_per_ns(void) { /* the waiting thread's CPU use, as a share of a busy core */
    int64_t t0 = mono_ns(); uint64_t c0 = thread_cycles();
    while (mono_ns() - t0 < 20000000) {}
    return (double)(thread_cycles() - c0) / (double)(mono_ns() - t0);
}
static int cmp64(const void *a, const void *b) { int64_t x = *(const int64_t *)a, y = *(const int64_t *)b; return (x > y) - (x < y); }
/* requested_ns > 0: a deadline that far ahead; 0: UE4's Wait(1) (now cut to whole ms, plus 1 ms). */
static void timing_row(const char *name, int64_t requested_ns, int n, double cpn) {
    void *m = make(1), *c = NULL;
    pcinit(&c, NULL);
    int64_t *over = malloc(sizeof(int64_t) * n), *req = malloc(sizeof(int64_t) * n);
    int64_t busy_cycles = 0, wall = 0;
    int early = 0;
    plock(&m);
    for (int i = 0; i < n; ++i) {
        GuestTime d;
        if (requested_ns) d = realtime_after(requested_ns);
        else {
            struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
            int64_t ms = now.tv_nsec / 1000000 + 1;
            d = (GuestTime){ now.tv_sec + ms / 1000, (ms % 1000) * 1000000 };
        }
        struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
        req[i] = (d.sec - now.tv_sec) * 1000000000 + (d.nsec - now.tv_nsec);
        int64_t t0 = mono_ns(); uint64_t c0 = thread_cycles();
        int32_t e;
        while ((e = pctimed(&c, &m, &d)) == 0) ++early; /* back before the deadline: wait again, as UE4 does */
        uint64_t c1 = thread_cycles(); int64_t took = mono_ns() - t0;
        CHECK(e == 60);
        over[i] = took - req[i]; busy_cycles += (int64_t)(c1 - c0); wall += took;
    }
    punlock(&m);
    qsort(over, n, sizeof(int64_t), cmp64);
    double req_avg = 0; for (int i = 0; i < n; ++i) req_avg += req[i]; req_avg /= n;
    printf("  %-9s requested avg %6.3f ms  overshoot median %6.3f  p90 %6.3f  max %6.3f ms  CPU while waiting %3.0f%%"
           "  early returns %d\n", name, req_avg / 1e6, over[n / 2] / 1e6, over[n * 9 / 10] / 1e6, over[n - 1] / 1e6,
           100.0 * busy_cycles / (cpn * wall), early);
    free(over); free(req);
    cdestroy(&c); destroy(&m);
}
static void timing(void) {
    double cpn = cycles_per_ns();
    printf("timed condition waits that expire (GOW3_FUTEX_COND=%s GOW3_COND_PRECISE=%s GOW3_COND_SPIN_US=%s):\n",
           getenv("GOW3_FUTEX_COND") ? getenv("GOW3_FUTEX_COND") : "-",
           getenv("GOW3_COND_PRECISE") ? getenv("GOW3_COND_PRECISE") : "-",
           getenv("GOW3_COND_SPIN_US") ? getenv("GOW3_COND_SPIN_US") : "-");
    timing_row("0.1 ms", 100000, 400, cpn);
    timing_row("0.5 ms", 500000, 400, cpn);
    timing_row("0.9 ms", 900000, 400, cpn);
    timing_row("ue4 1 ms", 0, 400, cpn);
    timing_row("2 ms", 2000000, 200, cpn);
    timing_row("9.5 ms", 9500000, 60, cpn);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    runtime_host_scheduling_init(); /* the game's 1 ms timer resolution and power-throttling opt-out */
#endif
    setup();
    if (argc > 1 && !strcmp(argv[1], "--bench")) {
        bench_uncontended();
        for (int n = 2; n <= 8; n *= 2) bench_contended(n);
        bench_pingpong();
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--timing")) { timing(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--handoff")) { handoff_compare(); return 0; }
    mutex_types();
    static_handles();
    timed_lock();
    conditions();
    timed_wake();
    stress();
    holder_rows();
    puts("mutex-test: all passed");
    return 0;
}
