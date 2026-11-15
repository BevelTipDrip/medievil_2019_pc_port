/* Guest rwlocks (src/runtime_rwlock.c): the PS4 semantics the game relies on, and a per-operation benchmark.
 * Standalone (only runtime_rwlock.c): builds on Windows too (ninja -C out/gpu rwlock-test).
 *   rwlock-test           semantics (exit 0 = all pass)
 *   rwlock-test --bench   ns per operation, uncontended and contended (for before / after comparisons)
 * The same checks as test_runtime.c's rw_lifecycle / rw_concurrency / rw_timeouts, plus static initialization
 * under contention, many locks held at once, deep read recursion, unlock from a non-holder, and a stress run. */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#undef NDEBUG
typedef int32_t (ABI *Op)(void **);
typedef int32_t (ABI *Init)(void **, void *const *, const char *);
typedef struct { int64_t seconds, nanoseconds; } TestTime;
typedef int32_t (ABI *Timed)(void **, const TestTime *);
static Op rd, wr, tryrd, trywr, unl, destroy;
static Init init;
static Timed timedrd, timedwr;
#define OK 0
#define EBUSY_ 0x80020010u
#define EDEADLK_ 0x8002000bu
#define EPERM_ 0x80020001u
#define EINVAL_ 0x80020016u
#define ETIMEDOUT_ 0x8002003cu
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); } } while (0)

static void setup(void) {
    init = (Init)runtime_rwlock_resolve("6ULAa0fq4jA#libkernel");
    destroy = (Op)runtime_rwlock_resolve("BB+kb08Tl9A#libkernel");
    rd = (Op)runtime_rwlock_resolve("Ox9i0c7L5w0#libkernel");
    wr = (Op)runtime_rwlock_resolve("mqdNorrB+gI#libkernel");
    tryrd = (Op)runtime_rwlock_resolve("XD3mDeybCnk#libkernel");
    trywr = (Op)runtime_rwlock_resolve("bIHoZCTomsI#libkernel");
    unl = (Op)runtime_rwlock_resolve("+L98PIbGttk#libkernel");
    timedrd = (Timed)runtime_rwlock_resolve("iPtZRWICjrM#libkernel");
    timedwr = (Timed)runtime_rwlock_resolve("adh--6nIqTk#libkernel");
    CHECK(init && destroy && rd && wr && tryrd && trywr && unl && timedrd && timedwr);
}
static void barrier(pthread_barrier_t *b) {
    int e = pthread_barrier_wait(b); CHECK(e == 0 || e == PTHREAD_BARRIER_SERIAL_THREAD);
}

/* The same sequence as test_runtime.c rw_lifecycle. */
static void lifecycle(void) {
    void *lock;
    CHECK((uint32_t)init(NULL, NULL, NULL) == EINVAL_);
    CHECK(init(&lock, NULL, "rw-test") == OK);
    CHECK(rd(&lock) == OK && rd(&lock) == OK);
    CHECK((uint32_t)trywr(&lock) == EBUSY_);
    CHECK((uint32_t)wr(&lock) == EDEADLK_);
    CHECK((uint32_t)destroy(&lock) == EBUSY_);
    CHECK(unl(&lock) == OK && unl(&lock) == OK);
    CHECK((uint32_t)unl(&lock) == EPERM_);
    CHECK(wr(&lock) == OK);
    CHECK((uint32_t)tryrd(&lock) == EBUSY_);
    CHECK((uint32_t)rd(&lock) == EDEADLK_);
    CHECK(unl(&lock) == OK && destroy(&lock) == OK);
    CHECK((uintptr_t)lock == 1);
    CHECK((uint32_t)rd(&lock) == EINVAL_);
    CHECK((uint32_t)destroy(&lock) == EINVAL_);
    lock = NULL;
    CHECK(destroy(&lock) == OK);
    CHECK(rd(&lock) == OK && unl(&lock) == OK && destroy(&lock) == OK);
    lock = (void *)(uintptr_t)0xdead;
    CHECK((uint32_t)tryrd(&lock) == EINVAL_);
}

typedef struct { void *lock; pthread_barrier_t a, b, c; _Atomic int value; } Fix;
static void fix_init(Fix *f) {
    CHECK(pthread_barrier_init(&f->a, NULL, 2) == 0 && pthread_barrier_init(&f->b, NULL, 2) == 0 &&
          pthread_barrier_init(&f->c, NULL, 2) == 0);
}
static void fix_done(Fix *f) {
    CHECK(destroy(&f->lock) == OK);
    pthread_barrier_destroy(&f->a); pthread_barrier_destroy(&f->b); pthread_barrier_destroy(&f->c);
}
static void *reader(void *p) {
    Fix *f = p; CHECK(rd(&f->lock) == OK);
    barrier(&f->a); barrier(&f->b);
    CHECK(atomic_load(&f->value) == 0); CHECK(unl(&f->lock) == OK); return NULL;
}
static void *writer(void *p) {
    Fix *f = p; CHECK((uint32_t)trywr(&f->lock) == EBUSY_);
    barrier(&f->c); CHECK(wr(&f->lock) == OK);
    atomic_store(&f->value, 1); CHECK(unl(&f->lock) == OK); return NULL;
}
/* test_runtime.c rw_concurrency: two readers at once, a writer waits for both, destroy is EBUSY meanwhile. */
static void concurrency(void) {
    Fix f = {0}; fix_init(&f);
    pthread_t r, w;
    CHECK(rd(&f.lock) == OK);
    CHECK(pthread_create(&r, NULL, reader, &f) == 0);
    barrier(&f.a);
    CHECK(unl(&f.lock) == OK);
    CHECK(pthread_create(&w, NULL, writer, &f) == 0);
    barrier(&f.c);
    CHECK((uint32_t)destroy(&f.lock) == EBUSY_);
    CHECK(atomic_load(&f.value) == 0);
    barrier(&f.b);
    CHECK(pthread_join(r, NULL) == 0 && pthread_join(w, NULL) == 0);
    CHECK(atomic_load(&f.value) == 1);
    fix_done(&f);
}
static void *holding_writer(void *p) {
    Fix *f = p; CHECK(wr(&f->lock) == OK);
    barrier(&f->a); barrier(&f->b); CHECK(unl(&f->lock) == OK); return NULL;
}
/* test_runtime.c rw_timeouts, plus a real wait that times out and one that succeeds. */
static void timeouts(void) {
    Fix f = {0}; fix_init(&f);
    pthread_t w; CHECK(pthread_create(&w, NULL, holding_writer, &f) == 0);
    barrier(&f.a);
    TestTime expired = {0, 0}, invalid = {0, 1000000000};
    CHECK((uint32_t)timedrd(&f.lock, &expired) == ETIMEDOUT_);
    CHECK((uint32_t)timedwr(&f.lock, &expired) == ETIMEDOUT_);
    CHECK((uint32_t)timedrd(&f.lock, &invalid) == EINVAL_);
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    TestTime soon = {now.tv_sec, now.tv_nsec + 30000000};
    if (soon.nanoseconds >= 1000000000) { soon.seconds++; soon.nanoseconds -= 1000000000; }
    CHECK((uint32_t)timedwr(&f.lock, &soon) == ETIMEDOUT_); /* waits ~30 ms, the writer still holds it */
    CHECK((uint32_t)unl(&f.lock) == EPERM_);
    barrier(&f.b); CHECK(pthread_join(w, NULL) == 0);
    CHECK(timedrd(&f.lock, &invalid) == OK); /* an available lock ignores the deadline */
    CHECK(unl(&f.lock) == OK);
    fix_done(&f);
}

/* Statically initialized (NULL) handle used by many threads at once: one lock, every call succeeds. */
static void *static_user(void *p) {
    void **lock = p;
    for (int i = 0; i < 2000; ++i) { CHECK(rd(lock) == OK); CHECK(unl(lock) == OK); }
    return NULL;
}
static void static_init(void) {
    void *lock = NULL; pthread_t t[8];
    for (int i = 0; i < 8; ++i) CHECK(pthread_create(&t[i], NULL, static_user, &lock) == 0);
    for (int i = 0; i < 8; ++i) CHECK(pthread_join(t[i], NULL) == 0);
    CHECK(lock && (uintptr_t)lock > 2);
    CHECK(destroy(&lock) == OK);
}

/* One thread holding many locks at once (more than any small per-thread table), each read twice. */
static void many_held(void) {
    enum { N = 100 };
    void *locks[N];
    for (int i = 0; i < N; ++i) { CHECK(init(&locks[i], NULL, NULL) == OK); CHECK(rd(&locks[i]) == OK); }
    for (int i = 0; i < N; ++i) CHECK(rd(&locks[i]) == OK);
    for (int i = 0; i < N; ++i) CHECK((uint32_t)wr(&locks[i]) == EDEADLK_);
    for (int i = 0; i < N; ++i) CHECK((uint32_t)destroy(&locks[i]) == EBUSY_);
    for (int i = N - 1; i >= 0; --i) CHECK(unl(&locks[i]) == OK && unl(&locks[i]) == OK);
    for (int i = 0; i < N; ++i) { CHECK((uint32_t)unl(&locks[i]) == EPERM_); CHECK(destroy(&locks[i]) == OK); }
}

/* Read recursion: 1000 read locks by one thread, all released, then EPERM. */
static void deep_reads(void) {
    void *lock; CHECK(init(&lock, NULL, NULL) == OK);
    for (int i = 0; i < 1000; ++i) CHECK(rd(&lock) == OK);
    CHECK((uint32_t)trywr(&lock) == EBUSY_);
    for (int i = 0; i < 1000; ++i) CHECK(unl(&lock) == OK);
    CHECK((uint32_t)unl(&lock) == EPERM_);
    CHECK(destroy(&lock) == OK);
}

/* Another thread's unlock of a lock it does not hold is EPERM and leaves the holder's lock intact. */
static void *foreign_unlock(void *p) { void **lock = p; CHECK((uint32_t)unl(lock) == EPERM_); return NULL; }
static void not_holder(void) {
    void *lock; CHECK(init(&lock, NULL, NULL) == OK);
    CHECK(wr(&lock) == OK);
    pthread_t t; CHECK(pthread_create(&t, NULL, foreign_unlock, &lock) == 0); CHECK(pthread_join(t, NULL) == 0);
    CHECK((uint32_t)tryrd(&lock) == EBUSY_);
    CHECK(unl(&lock) == OK && destroy(&lock) == OK);
}

/* Stress: 8 threads, 4 locks, mixed reads / writes. A writer sets a pair to (n, -n); readers must always see
 * a consistent pair, and the writes counted under the lock must add up. */
enum { LOCKS = 4, THREADS = 8, ROUNDS = 20000 };
static void *locks4[LOCKS];
static volatile long pair_a[LOCKS], pair_b[LOCKS];
static long writes_done[LOCKS];
static void *stress_worker(void *p) {
    unsigned seed = (unsigned)(uintptr_t)p * 2654435761u;
    for (int i = 0; i < ROUNDS; ++i) {
        seed = seed * 1103515245u + 12345u;
        int k = (seed >> 16) % LOCKS;
        if ((seed >> 8) % 10 == 0) {
            CHECK(wr(&locks4[k]) == OK);
            long n = pair_a[k] + 1; pair_a[k] = n; pair_b[k] = -n; ++writes_done[k];
            CHECK(unl(&locks4[k]) == OK);
        } else {
            CHECK(rd(&locks4[k]) == OK);
            CHECK(pair_a[k] == -pair_b[k]);
            CHECK(unl(&locks4[k]) == OK);
        }
    }
    return NULL;
}
static void stress(void) {
    for (int k = 0; k < LOCKS; ++k) { CHECK(init(&locks4[k], NULL, NULL) == OK); pair_a[k] = pair_b[k] = 0; writes_done[k] = 0; }
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; ++i) CHECK(pthread_create(&t[i], NULL, stress_worker, (void *)(uintptr_t)(i + 1)) == 0);
    for (int i = 0; i < THREADS; ++i) CHECK(pthread_join(t[i], NULL) == 0);
    for (int k = 0; k < LOCKS; ++k) { CHECK(pair_a[k] == writes_done[k]); CHECK(destroy(&locks4[k]) == OK); }
}

/* ---- benchmark ---- */
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
typedef struct { void *lock; int writes_per_100; int iterations; double ns; pthread_barrier_t *start; } Bench;
static void *bench_worker(void *p) {
    Bench *b = p; unsigned seed = (unsigned)(uintptr_t)p;
    barrier(b->start);
    double t0 = now_ns();
    for (int i = 0; i < b->iterations; ++i) {
        seed = seed * 1103515245u + 12345u;
        if ((int)((seed >> 16) % 100) < b->writes_per_100) { wr(&b->lock); unl(&b->lock); }
        else { rd(&b->lock); unl(&b->lock); }
    }
    b->ns = (now_ns() - t0) / b->iterations;
    return NULL;
}
static void bench_case(const char *name, int threads, int writes_per_100) {
    void *lock; CHECK(init(&lock, NULL, NULL) == OK);
    pthread_barrier_t start; CHECK(pthread_barrier_init(&start, NULL, (unsigned)threads) == 0);
    Bench b[16]; pthread_t t[16];
    for (int i = 0; i < threads; ++i) { b[i] = (Bench){lock, writes_per_100, threads == 1 ? 2000000 : 300000, 0, &start}; }
    for (int i = 0; i < threads; ++i) { b[i].lock = lock; CHECK(pthread_create(&t[i], NULL, bench_worker, &b[i]) == 0); }
    double sum = 0;
    for (int i = 0; i < threads; ++i) { CHECK(pthread_join(t[i], NULL) == 0); sum += b[i].ns; }
    printf("  %-44s %8.1f ns per lock+unlock pair (mean over %d threads)\n", name, sum / threads, threads);
    pthread_barrier_destroy(&start);
    /* the lock pointer was copied into each Bench; destroy through the original handle */
    CHECK(destroy(&lock) == OK);
}
static void bench(void) {
    puts("rwlock benchmark (guest exports, as the game calls them):");
    bench_case("1 thread, reads", 1, 0);
    bench_case("1 thread, writes", 1, 100);
    bench_case("4 threads, one lock, reads", 4, 0);
    bench_case("4 threads, one lock, 10% writes", 4, 10);
    bench_case("8 threads, one lock, reads", 8, 0);
}

int main(int argc, char **argv) {
    setup();
    if (argc > 1 && !strcmp(argv[1], "--bench")) { bench(); return 0; }
    lifecycle(); concurrency(); timeouts(); static_init(); many_held(); deep_reads(); not_holder(); stress();
    puts("PASS: rwlock lifecycle, ownership errors, readers / writers, timeouts, static init, many held, deep reads, non-holder unlock, stress");
    return 0;
}
