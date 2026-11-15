/* PS4 rwlocks (pointer-to-handle ABI) over host rwlocks: SRWLOCK on Windows, pthread_rwlock elsewhere.
 *
 * gow3 (2026-11-15, card runtime/rwlock-fast-path): the lock and unlock paths take no global lock and allocate
 * nothing. Before, every operation took one process-wide registry mutex twice, walked the lock's holder list and
 * calloc'd / freed a holder record: in MediEvil's frame-time streaks the game thread spent ~17% in it, and 8
 * threads reading one lock took ~1.9 us per lock + unlock (tests/test_rwlock.c --bench).
 * - Handles are validated through the open-addressing set of live locks (R2), read without a lock: slots are
 *   written with release stores under registry_lock, a rebuilt table is published with one release store and the
 *   old one retired, never freed while a lookup could still read it. An unknown pointer is never dereferenced.
 * - Ownership (EDEADLK, EPERM, a thread's read recursion) is a per-thread list of the locks it holds. A thread
 *   that already holds a lock for reading takes it again by counting only: SRW locks must not be acquired
 *   recursively (a waiting writer would deadlock it), and FreeBSD's libthr grants such a recursive read too.
 * - Destroy: EBUSY while any thread holds the lock or an acquire is in progress (one atomic count on the lock,
 *   raised when an acquire starts and kept while that thread holds it). A destroyed lock is marked dead and
 *   retired, not freed at once: a racing call sees it dead (EINVAL).
 * Only create, static initialization and destroy take registry_lock. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define RW_LIVE UINT64_C(0x52574c4f434b4c56) /* "RWLOCKLV" */
#define RW_DEAD UINT64_C(0x52574c4f434b4444)
typedef struct Rwlock {
    uint64_t magic;     /* RW_LIVE, RW_DEAD after destroy (atomic) */
#ifdef _WIN32
    SRWLOCK native;
#else
    pthread_rwlock_t native;
#endif
    unsigned users;     /* threads holding it plus acquires in progress (atomic): destroy's EBUSY */
} Rwlock;
typedef struct { int64_t seconds, nanoseconds; } GuestTime;

static int32_t error(int e) {
    if (!e) return 0;
    unsigned code;
    switch(e) {
    case EPERM: code=1; break;
    case EDEADLK: code=11; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case ETIMEDOUT: code=60; break;
    default: fprintf(stderr,"STOP: unmapped rwlock error %d\n",e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000)|code);
}

/* ---- the set of live locks (R2): lock-free lookups, edits under registry_lock ---- */
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct Table { size_t size; Rwlock *slot[]; } Table;
static Table *table;                 /* atomic pointer; slots atomic */
static size_t registry_used, registry_live, created; /* registry_lock */
/* Deleted slots hold &deleted, never 1, the value rw_destroy leaves in a destroyed handle. */
static Rwlock deleted;
/* Replaced tables and destroyed locks are freed only after many later ones: a lookup or a racing call can still
 * be reading them (a lookup takes nanoseconds; RETIRED destroys take far longer). */
#define RETIRED_TABLES 8
#define RETIRED_LOCKS 4096
static Table *retired_tables[RETIRED_TABLES]; static size_t retired_table_next;
static Rwlock *retired_locks[RETIRED_LOCKS]; static size_t retired_lock_next;

static size_t slot_of(const Rwlock *r, size_t size) {
    return (size_t)(((uintptr_t)r >> 4) * UINT64_C(0x9E3779B97F4A7C15) >> 32) & (size-1);
}
/* The lock a guest handle names, or NULL. Never dereferences the handle. */
static Rwlock *lookup(const Rwlock *handle) {
    if (!handle || handle==&deleted) return NULL;
    Table *t=__atomic_load_n(&table,__ATOMIC_ACQUIRE);
    if (!t) return NULL;
    for (size_t i=slot_of(handle,t->size);; i=(i+1)&(t->size-1)) {
        Rwlock *s=__atomic_load_n(&t->slot[i],__ATOMIC_ACQUIRE);
        if (!s) return NULL;
        if (s==handle) return s;
    }
}
static Rwlock **find_slot_locked(const Rwlock *handle) {
    if (!handle || handle==&deleted || !table) return NULL;
    for (size_t i=slot_of(handle,table->size);; i=(i+1)&(table->size-1)) {
        if (!table->slot[i]) return NULL;
        if (table->slot[i]==handle) return &table->slot[i];
    }
}
/* Keeps at most half the slots used (live and deleted): rebuilt at 4x the live count, published at once. */
static int reserve_locked(void) {
    if (table && (registry_used+1)*2<=table->size) return 0;
    size_t size=64;
    while (size<(registry_live+1)*4) size*=2;
    Table *t=calloc(1,sizeof(Table)+size*sizeof(Rwlock *));
    if (!t) return ENOMEM;
    t->size=size;
    size_t used=0;
    if (table) for (size_t i=0;i<table->size;++i) {
        Rwlock *r=table->slot[i];
        if (!r || r==&deleted) continue;
        size_t j=slot_of(r,size);
        while (t->slot[j]) j=(j+1)&(size-1);
        t->slot[j]=r; ++used;
    }
    Table *old=table;
    __atomic_store_n(&table,t,__ATOMIC_RELEASE);
    registry_used=used;
    if (old) {
        free(retired_tables[retired_table_next]);
        retired_tables[retired_table_next]=old;
        retired_table_next=(retired_table_next+1)%RETIRED_TABLES;
    }
    return 0;
}
static int create_locked(Rwlock **out) {
    if (reserve_locked()) return ENOMEM;
    Rwlock *r=calloc(1,sizeof(*r));
    if (!r) return ENOMEM;
#ifdef _WIN32
    InitializeSRWLock(&r->native);
#else
    int e=pthread_rwlock_init(&r->native,NULL);
    if (e) { free(r); return e; }
#endif
    __atomic_store_n(&r->magic,RW_LIVE,__ATOMIC_RELAXED);
    size_t i=slot_of(r,table->size);
    while (table->slot[i] && table->slot[i]!=&deleted) i=(i+1)&(table->size-1);
    if (!table->slot[i]) ++registry_used;
    __atomic_store_n(&table->slot[i],r,__ATOMIC_RELEASE);
    ++registry_live; ++created;
    __atomic_store_n(out,r,__ATOMIC_RELEASE);
    return 0;
}

/* ---- per-thread ownership ---- */
typedef struct { Rwlock *lock; unsigned readers, writer; } Held;
#define HELD_INLINE 16
static __thread Held held_inline[HELD_INLINE];
static __thread Held *held_heap;      /* when a thread holds more than HELD_INLINE locks at once */
static __thread unsigned held_count, held_capacity;
static Held *held_at(unsigned i) { return i<HELD_INLINE ? &held_inline[i] : &held_heap[i-HELD_INLINE]; }
static Held *held_find(const Rwlock *r) {
    for (unsigned i=0;i<held_count;++i) { Held *h=held_at(i); if (h->lock==r) return h; }
    return NULL;
}
static Held *held_add(Rwlock *r) {
    if (held_count>=HELD_INLINE+held_capacity) {
        unsigned capacity=held_capacity ? held_capacity*2 : 16;
        Held *grown=realloc(held_heap,capacity*sizeof(Held));
        if (!grown) return NULL;
        held_heap=grown; held_capacity=capacity;
    }
    Held *h=held_at(held_count++);
    *h=(Held){r,0,0};
    return h;
}
/* The thread's hold ends: its share of the lock's users count (raised by its acquire) goes. */
static void held_remove(Held *h) {
    __atomic_sub_fetch(&h->lock->users,1,__ATOMIC_SEQ_CST);
    *h=*held_at(--held_count);
}

/* ---- host lock ---- */
static void host_unlock(Rwlock *r, int writer) {
#ifdef _WIN32
    if (writer) ReleaseSRWLockExclusive(&r->native); else ReleaseSRWLockShared(&r->native);
#else
    (void)writer; pthread_rwlock_unlock(&r->native);
#endif
}
static int host_try(Rwlock *r, int writer) {
#ifdef _WIN32
    return (writer ? TryAcquireSRWLockExclusive(&r->native) : TryAcquireSRWLockShared(&r->native)) ? 0 : EBUSY;
#else
    return writer ? pthread_rwlock_trywrlock(&r->native) : pthread_rwlock_tryrdlock(&r->native);
#endif
}
static int host_lock(Rwlock *r, int writer) {
#ifdef _WIN32
    if (writer) AcquireSRWLockExclusive(&r->native); else AcquireSRWLockShared(&r->native);
    return 0;
#else
    return writer ? pthread_rwlock_wrlock(&r->native) : pthread_rwlock_rdlock(&r->native);
#endif
}
/* Until an absolute CLOCK_REALTIME deadline (POSIX timed rwlocks). SRW locks have no timed acquire: retried,
 * yielding, then sleeping 1 ms at a time; the game's timed rwlock calls are rare. */
static int host_timed(Rwlock *r, int writer, const struct timespec *deadline) {
#ifdef _WIN32
    for (unsigned spin=0;;++spin) {
        if (!host_try(r,writer)) return 0;
        struct timespec now; clock_gettime(CLOCK_REALTIME,&now);
        if (now.tv_sec>deadline->tv_sec || (now.tv_sec==deadline->tv_sec && now.tv_nsec>=deadline->tv_nsec)) return ETIMEDOUT;
        if (spin<64) SwitchToThread(); else Sleep(1);
    }
#else
    return writer ? pthread_rwlock_timedwrlock(&r->native,deadline) : pthread_rwlock_timedrdlock(&r->native,deadline);
#endif
}

/* ---- guest exports ---- */
static ABI int32_t rw_init(Rwlock **out, void *const *attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return error(EINVAL);
    if (attr) { fputs("STOP: non-default PS4 rwlock attributes are not implemented\n",stderr); exit(21); }
    pthread_mutex_lock(&registry_lock);
    /* POSIX allows uninitialized storage, so never read *out during init. */
    int e=create_locked(out);
    pthread_mutex_unlock(&registry_lock); return error(e);
}
/* The lock behind a handle; a statically initialized (NULL) handle is created once, under contention too. */
static int resolve(Rwlock **handle, Rwlock **out) {
    Rwlock *h=__atomic_load_n(handle,__ATOMIC_ACQUIRE);
    if (!h) {
        pthread_mutex_lock(&registry_lock);
        int e=0;
        if (!*handle) e=create_locked(handle);
        h=*handle;
        pthread_mutex_unlock(&registry_lock);
        if (e) return e;
    }
    Rwlock *r=lookup(h);
    if (!r || __atomic_load_n(&r->magic,__ATOMIC_ACQUIRE)!=RW_LIVE) return EINVAL;
    *out=r; return 0;
}
enum Operation { READ, WRITE, TRY_READ, TRY_WRITE, TIMED_READ, TIMED_WRITE };
static int32_t acquire(Rwlock **handle, enum Operation op, const GuestTime *time) {
    if (!handle) return error(EINVAL);
    int writer=op==WRITE || op==TRY_WRITE || op==TIMED_WRITE;
    int attempt=op==TRY_READ || op==TRY_WRITE;
    int timed=op==TIMED_READ || op==TIMED_WRITE;
    Rwlock *r;
    int e=resolve(handle,&r);
    if (e) return error(e);
    Held *h=held_find(r);
    if (h && (h->writer || (writer && h->readers))) return error(attempt ? EBUSY : EDEADLK);
    if (h && h->readers) { /* a read this thread already holds: counted, the host lock is not taken again */
        if (h->readers==UINT_MAX) return error(EAGAIN);
        ++h->readers; return 0;
    }
    /* Here the thread holds nothing of r (h is NULL): a new holder or a failed attempt. */
    __atomic_add_fetch(&r->users,1,__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&r->magic,__ATOMIC_SEQ_CST)!=RW_LIVE) { /* destroyed meanwhile */
        __atomic_sub_fetch(&r->users,1,__ATOMIC_SEQ_CST); return error(EINVAL);
    }
    /* Timed operations first try the lock, matching POSIX timeout validation. */
    if (attempt || timed) e=host_try(r,writer);
    else e=host_lock(r,writer);
    if (timed && e==EBUSY) {
        if (!time || time->nanoseconds<0 || time->nanoseconds>=1000000000) e=EINVAL;
        else {
            struct timespec deadline={.tv_sec=(time_t)time->seconds,.tv_nsec=(long)time->nanoseconds};
            e=host_timed(r,writer,&deadline);
        }
    }
    if (!e) {
        h=held_add(r);
        if (!h) { host_unlock(r,writer); e=ENOMEM; }
        else if (writer) h->writer=1;
        else h->readers=1;
    }
    if (e) __atomic_sub_fetch(&r->users,1,__ATOMIC_SEQ_CST); /* kept while held, dropped by held_remove */
    return error(e);
}
static ABI int32_t rw_read(Rwlock **r) { return acquire(r,READ,NULL); }
static ABI int32_t rw_write(Rwlock **r) { return acquire(r,WRITE,NULL); }
static ABI int32_t rw_tryread(Rwlock **r) { return acquire(r,TRY_READ,NULL); }
static ABI int32_t rw_trywrite(Rwlock **r) { return acquire(r,TRY_WRITE,NULL); }
static ABI int32_t rw_timedread(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_READ,t); }
static ABI int32_t rw_timedwrite(Rwlock **r,const GuestTime *t) { return acquire(r,TIMED_WRITE,t); }
static ABI int32_t rw_unlock(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    Rwlock *r=lookup(__atomic_load_n(handle,__ATOMIC_ACQUIRE));
    if (!r) return error(EINVAL);
    Held *h=held_find(r);
    if (!h || (!h->readers && !h->writer)) return error(EPERM);
    if (h->writer) { h->writer=0; host_unlock(r,1); }
    else if (--h->readers==0) host_unlock(r,0);
    if (!h->readers && !h->writer) held_remove(h);
    return 0;
}
static ABI int32_t rw_destroy(Rwlock **handle) {
    if (!handle) return error(EINVAL);
    pthread_mutex_lock(&registry_lock);
    if (!*handle) { pthread_mutex_unlock(&registry_lock); return 0; }
    Rwlock **slot=find_slot_locked(*handle);
    if (!slot) { pthread_mutex_unlock(&registry_lock); return error(EINVAL); }
    Rwlock *r=*slot;
    /* Dead first, then the count: an acquire raises users before it checks the mark (both seq_cst), so one of
     * the two sees the other. */
    __atomic_store_n(&r->magic,RW_DEAD,__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&r->users,__ATOMIC_SEQ_CST)) {
        __atomic_store_n(&r->magic,RW_LIVE,__ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&registry_lock); return error(EBUSY);
    }
    __atomic_store_n(slot,&deleted,__ATOMIC_RELEASE); --registry_live;
    Rwlock *oldest=retired_locks[retired_lock_next];
    retired_locks[retired_lock_next]=r;
    retired_lock_next=(retired_lock_next+1)%RETIRED_LOCKS;
    *handle=(Rwlock *)(uintptr_t)1;
    pthread_mutex_unlock(&registry_lock);
    if (oldest) {
#ifndef _WIN32
        pthread_rwlock_destroy(&oldest->native);
#endif
        free(oldest);
    }
    return 0;
}
uintptr_t runtime_rwlock_resolve(const char *name) {
    if (!strcmp(name,"6ULAa0fq4jA#libkernel")) return (uintptr_t)rw_init;
    if (!strcmp(name,"BB+kb08Tl9A#libkernel")) return (uintptr_t)rw_destroy;
    if (!strcmp(name,"Ox9i0c7L5w0#libkernel")) return (uintptr_t)rw_read;
    if (!strcmp(name,"mqdNorrB+gI#libkernel")) return (uintptr_t)rw_write;
    if (!strcmp(name,"XD3mDeybCnk#libkernel")) return (uintptr_t)rw_tryread;
    if (!strcmp(name,"bIHoZCTomsI#libkernel")) return (uintptr_t)rw_trywrite;
    if (!strcmp(name,"+L98PIbGttk#libkernel")) return (uintptr_t)rw_unlock;
    if (!strcmp(name,"iPtZRWICjrM#libkernel")) return (uintptr_t)rw_timedread;
    if (!strcmp(name,"adh--6nIqTk#libkernel")) return (uintptr_t)rw_timedwrite;
    return 0;
}
void runtime_rwlock_report(void) {
    pthread_mutex_lock(&registry_lock);
    printf("Runtime: rwlocks created=%zu, live=%zu\n",created,registry_live);
    pthread_mutex_unlock(&registry_lock);
}
