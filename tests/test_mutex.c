/* Guest mutexes and condition variables (src/runtime_mutex.c): the PS4 semantics the game relies on, a
 * per-operation benchmark, and how long timed condition waits really last.
 * Standalone (only runtime_mutex.c): ninja -C out/gpu mutex-test.
 *   mutex-test           semantics (exit 0 = all pass)
 *   mutex-test --bench   ns per operation (for before / after comparisons)
 *   mutex-test --timing  timed waits that expire: requested vs actual, and the waiting thread's CPU use
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
uint64_t runtime_wait_now(void) { return 0; }
void runtime_wait_site(void *a, unsigned k, uint64_t r, uint64_t n) { (void)a; (void)k; (void)r; (void)n; }

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
    mutex_types();
    static_handles();
    timed_lock();
    conditions();
    timed_wake();
    stress();
    puts("mutex-test: all passed");
    return 0;
}
