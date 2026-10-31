/* PS4 virtual memory: direct (physical) pool, flexible memory, reservations,
 * protection changes and queries. One sparse memfd backs the whole direct
 * pool, so any mapping of any physical range aliases the same storage.
 * Windows: a pagefile-backed section plays the memfd's part, and the PS4 user range is
 * reserved as one placeholder that views of the section replace (VirtualAlloc2/MapViewOfFile3,
 * as shadPS4 does); see the "host mappings" section below.
 * Guest addresses are placed below 1 TiB (PS4 user range): GPU descriptors
 * encode 40-bit addresses, so host-default 0x7f... addresses would not fit. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
/* sceKernelGetDirectMemorySize on retail PS4: 5056 MiB. GOW3_DMEM_MB raises it (the resolution
 * patches above 1080p need about 4 GiB more; run.sh sets it). */
static uint64_t pool_size_bytes(void) {
    static uint64_t size;
    if (!size) {
        const char *env = getenv("GOW3_DMEM_MB");
        uint64_t mb = env ? strtoull(env, NULL, 10) : 0;
        if (mb < 5056 || mb > 16384) mb = 5056;
        size = mb * 1024 * 1024;
    }
    return size;
}
#define POOL_SIZE pool_size_bytes()
#define FLEXIBLE_SIZE (UINT64_C(448) * 1024 * 1024)
#define PAGE UINT64_C(16384)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define LIMIT 4096
#define INVALID ((int32_t)UINT32_C(0x80020016))
#define NO_MEMORY ((int32_t)UINT32_C(0x8002000c))
#define ACCESS ((int32_t)UINT32_C(0x8002000d))
#define NO_SPACE ((int32_t)UINT32_C(0x80020023))
#define MAP_FIXED_FLAG 0x10
#define MAP_NO_OVERWRITE 0x80
enum { KIND_RESERVED=1, KIND_DIRECT, KIND_FLEXIBLE };
typedef struct { uint64_t start, size; int type, used; } Block;
typedef struct { uintptr_t start, end; int kind, prot, type; uint64_t phys; } Vma;
typedef struct {
    uintptr_t start, end; uint64_t offset; int32_t protection, memory_type;
    uint32_t flags; char name[32];
} VirtualQueryInfo; /* flags: flexible, direct, stack, pooled, committed */
typedef struct { void *start; uint64_t offset, length; int8_t prot, type; int16_t reserved; int32_t operation; } BatchEntry;
_Static_assert(sizeof(VirtualQueryInfo)==72,"PS4 virtual query info layout");
_Static_assert(sizeof(BatchEntry)==32,"PS4 batch map entry layout");

/* Region table lock. Lookups (GPU per-draw queries, VirtualQuery, GPU page
 * protection) share it; map/unmap/protect take it exclusively. Both nest: code
 * under the lock may write guest memory, and a GPU-tracked page then faults into
 * runtime_memory_gpu_protect on the same thread. Readers nest because glibc's
 * default rwlock prefers readers; a thread holding it exclusively skips relocking. */
static __thread unsigned exclusive_depth;
/* Odd while a writer holds the lock; readers' region caches are valid for one even value. */
static uint64_t table_generation;
#ifdef _WIN32
/* Windows: an SRW lock (no system call without contention; winpthreads' rwlock made one per
 * lookup, 8% of the draw recording thread). It is not reentrant and a queued writer blocks new
 * readers, so a thread takes it once and counts nested reads. */
static SRWLOCK lock=SRWLOCK_INIT;
static __thread unsigned shared_depth;
/* GOW3_HOST_MAP_STATS: exclusive hold time of the table lock. */
static int host_stats_enabled(void);
static uint64_t host_now_ns(void);
static __thread uint64_t write_lock_start;
static uint64_t write_lock_held_ns, write_lock_acquires; /* atomic */
static void write_lock(void) {
    if (!exclusive_depth++) {
        AcquireSRWLockExclusive(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL);
        if (host_stats_enabled()) write_lock_start=host_now_ns();
    }
}
static void write_unlock(void) {
    if (!--exclusive_depth) {
        if (write_lock_start) {
            __atomic_fetch_add(&write_lock_held_ns,host_now_ns()-write_lock_start,__ATOMIC_RELAXED);
            __atomic_fetch_add(&write_lock_acquires,1,__ATOMIC_RELAXED);
            write_lock_start=0;
        }
        __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); ReleaseSRWLockExclusive(&lock);
    }
}
static void read_lock(void) { if (!exclusive_depth && !shared_depth++) AcquireSRWLockShared(&lock); }
static void read_unlock(void) { if (!exclusive_depth && !--shared_depth) ReleaseSRWLockShared(&lock); }
#else
static pthread_rwlock_t lock=PTHREAD_RWLOCK_INITIALIZER;
static void write_lock(void) {
    if (!exclusive_depth++) { pthread_rwlock_wrlock(&lock); __atomic_add_fetch(&table_generation,1,__ATOMIC_ACQ_REL); }
}
static void write_unlock(void) {
    if (!--exclusive_depth) { __atomic_add_fetch(&table_generation,1,__ATOMIC_RELEASE); pthread_rwlock_unlock(&lock); }
}
static void read_lock(void) { if (!exclusive_depth) pthread_rwlock_rdlock(&lock); }
static void read_unlock(void) { if (!exclusive_depth) pthread_rwlock_unlock(&lock); }
#endif
static Block blocks[LIMIT];
static Vma *vmas; static size_t vma_count, vma_capacity;
/* gow3 (T-P2): lock-free reads of the region table for runtime_memory_clamp. A writer holds the
 * table lock across the host mapping calls (tens to hundreds of microseconds); the draw-command
 * thread's per-draw lookups waited behind them while UE4 streamed pool pages in (5% of that
 * thread while panning). vma_seq is odd only while table entries change (insert, erase, split),
 * not across the host calls; the table never moves: VMA_FIXED_CAPACITY entries up front, and
 * past them lock-free reads stop. GOW3_OPTIMISTIC_CLAMP=0: always locked. */
#define VMA_FIXED_CAPACITY 131072u
static uint64_t vma_seq; /* atomic */
static unsigned vma_seq_depth; /* writer only, under the table lock */
static int vma_optimistic=1; /* atomic */
static void vma_begin(void) {
    if (!vma_seq_depth++) { __atomic_add_fetch(&vma_seq,1,__ATOMIC_SEQ_CST); __atomic_thread_fence(__ATOMIC_RELEASE); }
}
static void vma_end(void) {
    if (!--vma_seq_depth) { __atomic_thread_fence(__ATOMIC_RELEASE); __atomic_add_fetch(&vma_seq,1,__ATOMIC_SEQ_CST); }
}
#ifndef _WIN32
static int pool_fd=-1;
#endif
static unsigned char *backing_base; /* second view of the pool: host writes bypass guest/GPU page protection */
/* Live performance switches (in-game menu > Performance patches; their environment at start). */
static int perf_keep_access=-1, perf_optimistic=-1; /* atomic; -1: not read yet */
static int decommit_keep_access(void) {
    int v=__atomic_load_n(&perf_keep_access,__ATOMIC_RELAXED);
    if (v<0) {
        const char *e=getenv("GOW3_DECOMMIT_KEEP_ACCESS");
        v=!(e && e[0]=='0');
        __atomic_store_n(&perf_keep_access,v,__ATOMIC_RELAXED);
    }
    return v;
}
void runtime_memory_set_perf(int optimistic, int keep_access) {
    __atomic_store_n(&perf_optimistic,optimistic ? 1 : 0,__ATOMIC_RELAXED);
    __atomic_store_n(&perf_keep_access,keep_access ? 1 : 0,__ATOMIC_RELAXED);
}
#define FLEX_SPAN (UINT64_C(1024) * 1024 * 1024)
static uint64_t flex_bitmap[FLEX_SPAN/PAGE/64];
/* GPU hooks (gow3gpu): notified outside the lock, in order, after each operation. */
typedef void (*GpuRange)(uintptr_t address, uint64_t size);
static GpuRange hook_map, hook_unmap, hook_invalidate;
enum { HOOK_MAP, HOOK_UNMAP, HOOK_INVALIDATE };
typedef struct { int kind; uintptr_t address; uint64_t size; } PendingHook;
static PendingHook pending[64]; static size_t pending_count;
static size_t allocations, maps, flexible_maps, protects, queries;
static uint64_t live_bytes, flexible_bytes;

static int valid_alignment(uint64_t a) { return a >= PAGE && a <= POOL_SIZE && !(a & (a-1)); }
static uint64_t align_up(uint64_t n, uint64_t a) { return (n+a-1) & ~(a-1); }
static int host_prot(int prot) {
    /* GPU read/write bits (0x10/0x20) need host access for the future GPU backend. */
    return ((prot & 0x11) ? PROT_READ : 0) | ((prot & 0x22) ? PROT_WRITE|PROT_READ : 0) | ((prot & 4) ? PROT_EXEC|PROT_READ : 0);
}
#ifdef _WIN32
/* ---- host mappings (Windows) ----
 * [USER_MIN,USER_MAX) is covered by regions, each one placeholder or one view of the
 * section. Mapping a range carves it out as a single placeholder first: views overlapping
 * it are unmapped, placeholders coalesced and split, and the parts of views outside the
 * range mapped again with the page protections they had (GPU page tracking included). */
typedef struct { uintptr_t start, end; uint64_t phys; int mapped; } HostRegion;
typedef struct { uintptr_t start, end; DWORD protect; } ProtRun;
static HostRegion *regions; static size_t region_count, region_capacity;
static HANDLE section;
static DWORD win_prot(int prot) {
    if (prot & PROT_EXEC) return (prot & PROT_WRITE) ? PAGE_EXECUTE_READWRITE : (prot & PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    if (prot & PROT_WRITE) return PAGE_READWRITE;
    return (prot & PROT_READ) ? PAGE_READONLY : PAGE_NOACCESS;
}
static void win_fail(const char *what, uintptr_t address, uint64_t size) {
    fprintf(stderr,"STOP: %s failed at %p (+0x%" PRIx64 "), error %lu\n",what,(void *)address,size,GetLastError());
    exit(21);
}
/* GOW3_HOST_MAP_STATS=1 (diagnostics): host map / unmap / protect calls, their time, and the
 * views carve_host unmapped and mapped again, per 5 s; any call over 1 ms is printed when it
 * happens. MediEvil's frame pacing episodes are the render thread waiting on a kernel push lock
 * while a System worker trims the guest memory section (MiTrimSection, 2026-10-18). Off: one load. */
enum { HOST_MAP, HOST_UNMAP, HOST_PROTECT };
static struct {
    uint64_t calls[3], bytes[3], ns[3], max_ns[3];
    uint64_t views_unmapped, unmapped_bytes, views_remapped, remapped_bytes, views_kept, views_reused;
    /* Sorted-array edits (guest map vmas, host views regions): count, bytes moved, time. */
    uint64_t vma_edits, vma_moved, vma_ns, view_edits, view_moved, view_ns;
    uint64_t commits, commit_ns, decommits, decommit_ns;
} host_stats; /* atomic */
static uint64_t host_stats_next; /* atomic */
static int host_stats_enabled(void) {
    static int enabled=-1;
    if (enabled<0) { const char *v=getenv("GOW3_HOST_MAP_STATS"); enabled=v && v[0]=='1'; }
    return enabled;
}
static uint64_t host_now_ns(void) {
    static LARGE_INTEGER frequency;
    LARGE_INTEGER t;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart*1e9/(double)frequency.QuadPart);
}
#define HOST_STAT_ADD(field,n) __atomic_fetch_add(&host_stats.field,(uint64_t)(n),__ATOMIC_RELAXED)
static uint64_t host_stats_take(uint64_t *field) { return __atomic_exchange_n(field,0,__ATOMIC_RELAXED); }
static void host_stats_record(int op, uintptr_t address, uint64_t size, uint64_t start_ns,
                              uint64_t views_before, uint64_t unmapped_before) {
    static const char *const names[]={"map","unmap","protect"};
    const uint64_t now=host_now_ns(), ns=now-start_ns;
    HOST_STAT_ADD(calls[op],1); HOST_STAT_ADD(bytes[op],size); HOST_STAT_ADD(ns[op],ns);
    for (uint64_t max=__atomic_load_n(&host_stats.max_ns[op],__ATOMIC_RELAXED);
         ns>max && !__atomic_compare_exchange_n(&host_stats.max_ns[op],&max,ns,1,__ATOMIC_RELAXED,__ATOMIC_RELAXED);) {}
    if (ns>1000000)
        printf("Host map stats: slow %s %p +0x%" PRIx64 ": %.1f ms, %" PRIu64 " views unmapped (%" PRIu64 " MiB), %zu regions\n",
               names[op],(void *)address,size,ns/1e6,
               __atomic_load_n(&host_stats.views_unmapped,__ATOMIC_RELAXED)-views_before,
               (__atomic_load_n(&host_stats.unmapped_bytes,__ATOMIC_RELAXED)-unmapped_before)>>20,region_count);
    uint64_t next=__atomic_load_n(&host_stats_next,__ATOMIC_RELAXED);
    if (next && now<next) return;
    if (!__atomic_compare_exchange_n(&host_stats_next,&next,now+UINT64_C(5000000000),0,__ATOMIC_RELAXED,__ATOMIC_RELAXED) || !next) return;
    printf("Host map stats (5 s): ");
    for (int i=0;i<3;++i) {
        const uint64_t c=host_stats_take(&host_stats.calls[i]), by=host_stats_take(&host_stats.bytes[i]),
                       t=host_stats_take(&host_stats.ns[i]), mx=host_stats_take(&host_stats.max_ns[i]);
        printf("%s %" PRIu64 " (%" PRIu64 " MiB, %.1f ms, max %.2f ms); ",names[i],c,by>>20,t/1e6,mx/1e6);
    }
    const uint64_t vu=host_stats_take(&host_stats.views_unmapped), ub=host_stats_take(&host_stats.unmapped_bytes),
                   vr=host_stats_take(&host_stats.views_remapped), rb=host_stats_take(&host_stats.remapped_bytes);
    const uint64_t vk=host_stats_take(&host_stats.views_kept), vre=host_stats_take(&host_stats.views_reused);
    printf("carve: %" PRIu64 " views unmapped (%" PRIu64 " MiB), %" PRIu64 " mapped again (%" PRIu64 " MiB); "
           "pool views kept %" PRIu64 ", reused %" PRIu64 "; %zu regions\n",
           vu,ub>>20,vr,rb>>20,vk,vre,region_count);
    {
        const uint64_t ve=host_stats_take(&host_stats.vma_edits), vm=host_stats_take(&host_stats.vma_moved),
                       vn=host_stats_take(&host_stats.vma_ns), he=host_stats_take(&host_stats.view_edits),
                       hm=host_stats_take(&host_stats.view_moved), hn=host_stats_take(&host_stats.view_ns),
                       pc=host_stats_take(&host_stats.commits), pcn=host_stats_take(&host_stats.commit_ns),
                       pd=host_stats_take(&host_stats.decommits), pdn=host_stats_take(&host_stats.decommit_ns),
                       lh=host_stats_take(&write_lock_held_ns), la=host_stats_take(&write_lock_acquires);
        printf("Host map stats (5 s): table edits: guest map %" PRIu64 " (%" PRIu64 " MiB moved, %.1f ms), host views %"
               PRIu64 " (%" PRIu64 " MiB moved, %.1f ms); %zu guest entries; pool commits %" PRIu64 " (%.1f ms), "
               "decommits %" PRIu64 " (%.1f ms); table lock held exclusively %" PRIu64 " times, %.1f ms\n",
               ve,vm>>20,vn/1e6,he,hm>>20,hn/1e6,vma_count,pc,pcn/1e6,pd,pdn/1e6,la,lh/1e6);
    }
}
static size_t region_index(uintptr_t a) { /* first region with end > a */
    size_t lo=0, hi=region_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (regions[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
static void region_insert(size_t at, HostRegion r) {
    if (region_count==region_capacity) {
        size_t capacity=region_capacity ? region_capacity*2 : 256;
        HostRegion *next=realloc(regions,capacity*sizeof(*regions));
        if (!next) { fputs("Out of memory (host regions)\n",stderr); exit(1); }
        regions=next; region_capacity=capacity;
    }
    const uint64_t t0=host_stats_enabled() ? host_now_ns() : 0;
    memmove(regions+at+1,regions+at,(region_count-at)*sizeof(*regions));
    if (t0) { HOST_STAT_ADD(view_edits,1); HOST_STAT_ADD(view_moved,(region_count-at)*sizeof(*regions)); HOST_STAT_ADD(view_ns,host_now_ns()-t0); }
    regions[at]=r; ++region_count;
}
static void region_erase(size_t at, size_t n) {
    const uint64_t t0=host_stats_enabled() ? host_now_ns() : 0;
    memmove(regions+at,regions+at+n,(region_count-at-n)*sizeof(*regions));
    if (t0) { HOST_STAT_ADD(view_edits,1); HOST_STAT_ADD(view_moved,(region_count-at-n)*sizeof(*regions)); HOST_STAT_ADD(view_ns,host_now_ns()-t0); }
    region_count-=n;
}
/* The guest range is reserved before anything else can be placed there (runtime_start). */
void runtime_memory_reserve(void) {
    if (region_count) return;
    if (!VirtualAlloc2(NULL,(void *)USER_MIN,USER_MAX-USER_MIN,MEM_RESERVE|MEM_RESERVE_PLACEHOLDER,PAGE_NOACCESS,NULL,0))
        win_fail("reserving the PS4 address range (link with --disable-high-entropy-va)",USER_MIN,USER_MAX-USER_MIN);
    region_insert(0,(HostRegion){USER_MIN,USER_MAX,0,0});
}
static void view_map(const HostRegion *r, DWORD protect) {
    DWORD initial=protect==PAGE_NOACCESS ? PAGE_READWRITE : protect, old;
    if (!MapViewOfFile3(section,GetCurrentProcess(),(void *)r->start,r->phys,r->end-r->start,MEM_REPLACE_PLACEHOLDER,initial,NULL,0))
        win_fail("MapViewOfFile3",r->start,r->end-r->start);
    if (initial!=protect && !VirtualProtect((void *)r->start,r->end-r->start,protect,&old))
        win_fail("VirtualProtect",r->start,r->end-r->start);
}
static size_t save_protections(uintptr_t start, uintptr_t end, ProtRun **runs, size_t count, size_t *capacity) {
    for (uintptr_t at=start; at<end;) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void *)at,&m,sizeof(m))) win_fail("VirtualQuery",at,0);
        uintptr_t next=(uintptr_t)m.BaseAddress+m.RegionSize;
        if (next>end) next=end;
        if (count==*capacity) { *capacity=*capacity ? *capacity*2 : 16; *runs=realloc(*runs,*capacity*sizeof(**runs)); }
        (*runs)[count++]=(ProtRun){at,next,m.Protect};
        at=next;
    }
    return count;
}
/* Leaves [a,b) as exactly one unmapped placeholder; returns its region index. */
static size_t carve_host(uintptr_t a, uintptr_t b) {
    size_t first=region_index(a), last=first;
    if (first==region_count || regions[first].start>a) win_fail("guest range outside the reservation",a,b-a);
    while (last<region_count && regions[last].start<b) ++last;
    if (regions[last-1].end<b) win_fail("guest range outside the reservation",a,b-a);
    uintptr_t lo=regions[first].start, hi=regions[last-1].end;
    HostRegion left=regions[first], right=regions[last-1];
    int keep_left=left.mapped && lo<a, keep_right=right.mapped && b<hi;
    if (first==last-1 && !left.mapped && lo==a && hi==b) return first;
    ProtRun *runs=NULL; size_t nruns=0, capacity=0, left_runs=0;
    if (keep_left) left_runs=nruns=save_protections(lo,a,&runs,nruns,&capacity);
    if (keep_right) nruns=save_protections(b,hi,&runs,nruns,&capacity);
    for (size_t i=first;i<last;++i) if (regions[i].mapped) {
        if (host_stats_enabled()) { HOST_STAT_ADD(views_unmapped,1); HOST_STAT_ADD(unmapped_bytes,regions[i].end-regions[i].start); }
        if (!UnmapViewOfFile2(GetCurrentProcess(),(void *)regions[i].start,MEM_PRESERVE_PLACEHOLDER))
            win_fail("UnmapViewOfFile2",regions[i].start,regions[i].end-regions[i].start);
    }
    if (last-first>1 && !VirtualFree((void *)lo,hi-lo,MEM_RELEASE|MEM_COALESCE_PLACEHOLDERS))
        win_fail("coalescing placeholders",lo,hi-lo);
    region_erase(first,last-first);
    region_insert(first,(HostRegion){lo,hi,0,0});
    size_t at=first;
    if (lo<a) {
        if (!VirtualFree((void *)lo,a-lo,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",lo,a-lo);
        regions[at].end=a;
        region_insert(at+1,(HostRegion){a,hi,0,0});
        if (keep_left) {
            if (host_stats_enabled()) { HOST_STAT_ADD(views_remapped,1); HOST_STAT_ADD(remapped_bytes,a-lo); }
            regions[at].phys=left.phys; regions[at].mapped=1;
            view_map(&regions[at],PAGE_READWRITE);
        }
        ++at;
    }
    if (b<hi) {
        if (!VirtualFree((void *)a,b-a,MEM_RELEASE|MEM_PRESERVE_PLACEHOLDER)) win_fail("splitting a placeholder",a,b-a);
        regions[at].end=b;
        region_insert(at+1,(HostRegion){b,hi,0,0});
        if (keep_right) {
            if (host_stats_enabled()) { HOST_STAT_ADD(views_remapped,1); HOST_STAT_ADD(remapped_bytes,hi-b); }
            regions[at+1].phys=right.phys+(b-right.start); regions[at+1].mapped=1;
            view_map(&regions[at+1],PAGE_READWRITE);
        }
    }
    for (size_t i=0;i<nruns;++i) {
        DWORD old;
        if (runs[i].protect!=PAGE_READWRITE && !VirtualProtect((void *)runs[i].start,runs[i].end-runs[i].start,runs[i].protect,&old))
            win_fail("restoring page protection",runs[i].start,runs[i].end-runs[i].start);
    }
    (void)left_runs;
    free(runs);
    return at;
}
/* Pool views kept across a decommit (pool_decommit, toggle bit 31): a commit that gets the same
 * physical pages back only changes the protection; no view is unmapped or mapped. */
static int host_view_reusable(uintptr_t address, uint64_t size, uint64_t phys) {
    size_t i=region_index(address);
    return i<region_count && regions[i].mapped && regions[i].start<=address && address+size<=regions[i].end &&
           regions[i].phys+(address-regions[i].start)==phys;
}
/* The physical address a kept view maps at `address`, and how many bytes of it follow. */
static int host_view_phys(uintptr_t address, uint64_t *phys, uint64_t *bytes) {
    size_t i=region_index(address);
    if (i==region_count || !regions[i].mapped || regions[i].start>address) return 0;
    *phys=regions[i].phys+(address-regions[i].start); *bytes=regions[i].end-address;
    return 1;
}
static int host_map_impl(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    if (!reserved && host_view_reusable(address,size,phys)) {
        DWORD old;
        if (!VirtualProtect((void *)address,size,win_prot(prot),&old)) win_fail("VirtualProtect",address,size);
        if (host_stats_enabled()) HOST_STAT_ADD(views_reused,1);
        return 0;
    }
    size_t i=carve_host(address,address+size);
    if (reserved) return 0;
    regions[i].phys=phys; regions[i].mapped=1;
    view_map(&regions[i],win_prot(prot));
    return 0;
}
static int host_protect_impl(uintptr_t address, uint64_t size, int prot) {
    DWORD protect=win_prot(prot), old;
    uintptr_t end=address+size;
    for (size_t i=region_index(address); i<region_count && regions[i].start<end; ++i) {
        if (!regions[i].mapped) continue;
        uintptr_t a=regions[i].start>address ? regions[i].start : address, b=regions[i].end<end ? regions[i].end : end;
        if (!VirtualProtect((void *)a,b-a,protect,&old)) return -1;
    }
    return 0;
}
#define HOST_TIMED(op,address,size,call) do {                                                   \
        if (!host_stats_enabled()) return (call);                                               \
        const uint64_t views=__atomic_load_n(&host_stats.views_unmapped,__ATOMIC_RELAXED),        \
                       unmapped=__atomic_load_n(&host_stats.unmapped_bytes,__ATOMIC_RELAXED),     \
                       start=host_now_ns();                                                     \
        const int result=(call);                                                                \
        host_stats_record(op,address,size,start,views,unmapped);                                \
        return result;                                                                          \
    } while (0)
static void region_changed(uintptr_t start, uintptr_t end);
/* gow3 L2: the GPU threads' region caches answer while a writer holds the lock (L1's buckets);
 * a range's buckets are bumped before its host mapping changes, so no cached answer covers
 * memory a writer is unmapping. */
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    region_changed(address,address+size);
    HOST_TIMED(HOST_MAP,address,size,host_map_impl(address,size,prot,reserved,phys));
}
static int host_unmap(uintptr_t address, uint64_t size) {
    region_changed(address,address+size);
    HOST_TIMED(HOST_UNMAP,address,size,(carve_host(address,address+size),0));
}
static int host_keep_view(uintptr_t address, uint64_t size) {
    DWORD old;
    region_changed(address,address+size); /* the view stays but becomes no access */
    if (host_stats_enabled()) HOST_STAT_ADD(views_kept,1);
    /* gow3 default since T-M3 (GOW3_DECOMMIT_KEEP_ACCESS=0: off; T-M1: ~1,100 decommits a second at ~60 us each, most of it
     * revoking access: page entries rewritten and a TLB flush on every core, under the table
     * lock): the kept view keeps its access. A guest read of decommitted memory then sees stale
     * data instead of faulting. */
    if (decommit_keep_access()) return 0;
    return VirtualProtect((void *)address,size,PAGE_NOACCESS,&old) ? 0 : -1;
}
static int host_protect(uintptr_t address, uint64_t size, int prot) {
    HOST_TIMED(HOST_PROTECT,address,size,host_protect_impl(address,size,prot));
}
/* Released pages read as zero when reused, like the memfd's punched holes. */
static void host_discard(uint64_t phys, uint64_t size) { memset(backing_base+phys,0,size); }
/* Direct memory occupies [0,POOL_SIZE) of the section, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (backing_base) return 0;
    runtime_memory_reserve();
    uint64_t bytes=POOL_SIZE+FLEX_SPAN;
    section=CreateFileMapping2(INVALID_HANDLE_VALUE,NULL,FILE_MAP_ALL_ACCESS,PAGE_EXECUTE_READWRITE,SEC_COMMIT,bytes,NULL,NULL,0);
    if (!section) { fprintf(stderr,"Runtime: cannot create %" PRIu64 " MiB of guest memory (error %lu); is the page file large enough?\n",bytes>>20,GetLastError()); return -1; }
    backing_base=MapViewOfFile3(section,GetCurrentProcess(),NULL,0,bytes,0,PAGE_READWRITE,NULL,0);
    return backing_base ? 0 : -1;
}
#else
/* Direct memory occupies [0,POOL_SIZE) of the memfd, flexible memory [POOL_SIZE,+FLEX_SPAN). */
static int pool(void) {
    if (pool_fd>=0) return 0;
    pool_fd=memfd_create("gow3-guest-memory", MFD_CLOEXEC);
    if (pool_fd<0 || ftruncate(pool_fd,(off_t)(POOL_SIZE+FLEX_SPAN))) return -1;
    void *view=mmap(NULL,POOL_SIZE+FLEX_SPAN,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_NORESERVE,pool_fd,0);
    if (view==MAP_FAILED) return -1;
    backing_base=view;
    return 0;
}
void runtime_memory_reserve(void) {}
static int host_map(uintptr_t address, uint64_t size, int prot, int reserved, uint64_t phys) {
    void *mapped = !reserved
        ? mmap((void *)address,size,prot,MAP_SHARED|MAP_FIXED,pool_fd,(off_t)phys)
        : mmap((void *)address,size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED,-1,0);
    return mapped==MAP_FAILED ? -1 : 0;
}
static void region_changed(uintptr_t start, uintptr_t end);
static int host_unmap(uintptr_t address, uint64_t size) { region_changed(address,address+size); return munmap((void *)address,size); }
static int host_view_phys(uintptr_t address, uint64_t *phys, uint64_t *bytes) { (void)address; (void)phys; (void)bytes; return 0; }
static int host_keep_view(uintptr_t address, uint64_t size) { (void)address; (void)size; return -1; }
static int host_protect(uintptr_t address, uint64_t size, int prot) { return mprotect((void *)address,size,prot); }
static void host_discard(uint64_t phys, uint64_t size) {
    fallocate(pool_fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,(off_t)phys,(off_t)size);
}
#endif
static void queue_hook(int kind, uintptr_t address, uint64_t size) {
    GpuRange hook = kind==HOOK_MAP ? hook_map : kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
    if (!hook) return;
    if (pending_count==sizeof(pending)/sizeof(*pending)) { fputs("STOP: GPU hook queue overflow\n",stderr); exit(21); }
    pending[pending_count++]=(PendingHook){kind,address,size};
}
/* Runs queued GPU notifications; callers invoke it after dropping the lock. */
static void flush_hooks(void) {
    for (;;) {
        write_lock();
        if (!pending_count) { write_unlock(); return; }
        PendingHook h=pending[0];
        memmove(pending,pending+1,--pending_count*sizeof(*pending));
        GpuRange hook = h.kind==HOOK_MAP ? hook_map : h.kind==HOOK_UNMAP ? hook_unmap : hook_invalidate;
        write_unlock();
        if (hook) hook(h.address,h.size);
    }
}
static int flex_test(uint64_t page) { return (int)((flex_bitmap[page/64]>>(page%64))&1); }
static void flex_set(uint64_t first, uint64_t count, int used) {
    for (uint64_t p=first;p<first+count;++p) {
        if (used) flex_bitmap[p/64]|=UINT64_C(1)<<(p%64); else flex_bitmap[p/64]&=~(UINT64_C(1)<<(p%64));
    }
}
static uint64_t flex_alloc(uint64_t size) {
    uint64_t pages=size/PAGE, total=FLEX_SPAN/PAGE, run=0;
    for (uint64_t p=0;p<total;++p) {
        run = flex_test(p) ? 0 : run+1;
        if (run==pages) { flex_set(p+1-pages,pages,1); return POOL_SIZE+(p+1-pages)*PAGE; }
    }
    return UINT64_MAX;
}
static void flex_free(uint64_t phys, uint64_t size) {
    flex_set((phys-POOL_SIZE)/PAGE,size/PAGE,0);
    host_discard(phys,size);
}
static size_t vma_index(uintptr_t a) { /* first VMA with end > a */
    size_t lo=0, hi=vma_count;
    while (lo<hi) { size_t mid=(lo+hi)/2; if (vmas[mid].end<=a) lo=mid+1; else hi=mid; }
    return lo;
}
/* gow3: generations of 16 MiB buckets of the guest address space (hashed into 64K counters),
 * bumped wherever a mapping in the bucket changes. The GPU threads' region caches check the
 * buckets their region spans instead of one table-wide generation: UE4 commits / decommits pool
 * pages ~6,000 times a second in MediEvil, and each change emptied every cache, so the per-draw
 * lookups (vertex and constant buffers) kept taking the lock and searching ~13,000 regions
 * (vma_index ~5% of both GPU threads, plus waits for the writers; P2a). */
#define REGION_BUCKET_SHIFT 24
#define REGION_BUCKETS 65536u
static uint64_t region_buckets[REGION_BUCKETS]; /* atomic */
static void region_changed(uintptr_t start, uintptr_t end) {
    if (end<=start) return;
    const uintptr_t first=start>>REGION_BUCKET_SHIFT, last=(end-1)>>REGION_BUCKET_SHIFT;
    if (last-first>=REGION_BUCKETS) {
        for (unsigned b=0;b<REGION_BUCKETS;++b) __atomic_add_fetch(&region_buckets[b],1,__ATOMIC_RELEASE);
        return;
    }
    for (uintptr_t b=first;b<=last;++b) __atomic_add_fetch(&region_buckets[b%REGION_BUCKETS],1,__ATOMIC_RELEASE);
}
/* Sum of the generations of the buckets [start,end) spans; 0 when it spans more than 64. */
static uint64_t region_stamp(uintptr_t start, uintptr_t end) {
    const uintptr_t first=start>>REGION_BUCKET_SHIFT, last=(end-1)>>REGION_BUCKET_SHIFT;
    if (last-first>=64) return 0;
    uint64_t sum=1;
    for (uintptr_t b=first;b<=last;++b) sum+=__atomic_load_n(&region_buckets[b%REGION_BUCKETS],__ATOMIC_ACQUIRE);
    return sum;
}
static int vma_insert(size_t at, Vma v) {
    region_changed(v.start,v.end);
    if (vma_count==vma_capacity) {
        /* The first table holds VMA_FIXED_CAPACITY entries; a larger one ends lock-free reads,
         * and the old table stays allocated (a reader may still be in it). */
        size_t capacity=vma_capacity ? vma_capacity*2 : VMA_FIXED_CAPACITY;
        Vma *next=malloc(capacity*sizeof(*vmas));
        if (!next) return -1;
        if (vma_capacity) __atomic_store_n(&vma_optimistic,0,__ATOMIC_SEQ_CST);
        if (vma_count) memcpy(next,vmas,vma_count*sizeof(*vmas));
        vma_begin();
        __atomic_store_n(&vmas,next,__ATOMIC_RELEASE); vma_capacity=capacity;
        vma_end();
    }
    vma_begin();
    const uint64_t t0=host_stats_enabled() ? host_now_ns() : 0;
    memmove(vmas+at+1,vmas+at,(vma_count-at)*sizeof(*vmas));
    if (t0) { HOST_STAT_ADD(vma_edits,1); HOST_STAT_ADD(vma_moved,(vma_count-at)*sizeof(*vmas)); HOST_STAT_ADD(vma_ns,host_now_ns()-t0); }
    vmas[at]=v; __atomic_store_n(&vma_count,vma_count+1,__ATOMIC_RELEASE);
    vma_end();
    return 0;
}
static void vma_erase(size_t at, size_t n) {
    for (size_t k=0;k<n;++k) region_changed(vmas[at+k].start,vmas[at+k].end);
    vma_begin();
    const uint64_t t0=host_stats_enabled() ? host_now_ns() : 0;
    memmove(vmas+at,vmas+at+n,(vma_count-at-n)*sizeof(*vmas));
    if (t0) { HOST_STAT_ADD(vma_edits,1); HOST_STAT_ADD(vma_moved,(vma_count-at-n)*sizeof(*vmas)); HOST_STAT_ADD(vma_ns,host_now_ns()-t0); }
    __atomic_store_n(&vma_count,vma_count-n,__ATOMIC_RELEASE);
    vma_end();
}
static int split_at(uintptr_t a) {
    size_t i=vma_index(a);
    if (i==vma_count || vmas[i].start>=a) return 0;
    Vma right=vmas[i];
    right.start=a;
    if (right.kind==KIND_DIRECT) right.phys+=a-vmas[i].start;
    region_changed(vmas[i].start,vmas[i].end);
    vma_begin();
    vmas[i].end=a;
    const int r=vma_insert(i+1,right);
    vma_end();
    return r;
}
/* Range [start,end) split on its edges; returns the first index inside it. */
static size_t carve(uintptr_t start, uintptr_t end, int *error) {
    *error = split_at(start) || split_at(end);
    return vma_index(start);
}
static int covered(uintptr_t start, uintptr_t end, int allow_reserved) {
    size_t i=vma_index(start); uintptr_t at=start;
    for (; at<end; ++i) {
        if (i==vma_count || vmas[i].start>at || (!allow_reserved && vmas[i].kind==KIND_RESERVED)) return 0;
        at=vmas[i].end;
    }
    return 1;
}
static int overlaps(uintptr_t start, uintptr_t end, int ignore_reserved) {
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (!ignore_reserved || vmas[i].kind!=KIND_RESERVED) return 1;
    return 0;
}
/* First-fit search in the PS4 user range, starting from the hint. */
static uintptr_t find_free(uintptr_t hint, uint64_t size, uint64_t alignment) {
    uintptr_t at=align_up(hint<USER_MIN ? USER_MIN : hint, alignment);
    for (size_t i=vma_index(at); at+size<=USER_MAX; ++i) {
        if (i==vma_count || vmas[i].start>=at+size) return at;
        at=align_up(vmas[i].end,alignment);
    }
    return 0;
}
static void drop_range(uintptr_t start, uintptr_t end) {
    int error; size_t i=carve(start,end,&error), n=0;
    while (i+n<vma_count && vmas[i+n].end<=end && vmas[i+n].start>=start) {
        if (vmas[i+n].kind==KIND_FLEXIBLE) {
            flexible_bytes-=vmas[i+n].end-vmas[i+n].start;
            flex_free(vmas[i+n].phys,vmas[i+n].end-vmas[i+n].start);
        }
        ++n;
    }
    vma_erase(i,n);
}
/* Places a host mapping; with MAP_FIXED it replaces what the guest had there. */
static int32_t place(void **inout, uint64_t size, int prot, int flags, uint64_t alignment,
                     int kind, int type, uint64_t phys) {
    uintptr_t address=(uintptr_t)*inout;
    if (flags & MAP_FIXED_FLAG) {
        if (!address || address%PAGE) return INVALID;
        if ((flags & MAP_NO_OVERWRITE) && overlaps(address,address+size,1)) return NO_MEMORY;
    } else {
        address=find_free(address,size,alignment);
        if (!address) return NO_MEMORY;
    }
    if (host_map(address,size,host_prot(prot),kind==KIND_RESERVED,phys)) return NO_MEMORY;
    void *mapped=(void *)address;
    drop_range(address,address+size);
    if (vma_insert(vma_index(address),(Vma){address,address+size,kind,prot,type,phys})) return NO_MEMORY;
    if (kind==KIND_FLEXIBLE) flexible_bytes+=size;
    if (kind!=KIND_RESERVED) queue_hook(HOOK_MAP,address,size);
    *inout=mapped;
    return 0;
}
static int block_type(uint64_t phys) {
    for (int i=0;i<LIMIT;++i)
        if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) return blocks[i].type;
    return -1;
}
static int direct_range_allocated(uint64_t phys, uint64_t size) {
    for (uint64_t at=phys; at<phys+size;) {
        int found=0;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && at>=blocks[i].start && at-blocks[i].start<blocks[i].size) {
            at=blocks[i].start+blocks[i].size; found=1; break;
        }
        if (!found) return 0;
    }
    return 1;
}

static ABI uint64_t direct_size(void) { return POOL_SIZE; }
uint64_t runtime_memory_flexible_available(void) {
    read_lock();
    uint64_t used=flexible_bytes;
    read_unlock();
    return used<FLEXIBLE_SIZE ? FLEXIBLE_SIZE-used : 0;
}
static ABI int32_t direct_allocate(int64_t low, int64_t high, uint64_t size,
                                  uint64_t alignment, int type, int64_t *out) {
    if (!alignment) alignment = PAGE;
    if (!out || low < 0 || high < 0 || !size || size % PAGE || !valid_alignment(alignment) || type < 0 || type > 10)
        return INVALID;
    uint64_t end = (uint64_t)high < POOL_SIZE ? (uint64_t)high : POOL_SIZE;
    if ((uint64_t)low >= end || size > end - (uint64_t)low) return NO_SPACE;
    write_lock();
    if (pool()) { write_unlock(); return NO_MEMORY; }
    uint64_t start = align_up((uint64_t)low, alignment);
    int slot = -1;
    for (int i=0; i<LIMIT; ++i) if (!blocks[i].used) { slot=i; break; }
    for (;;) {
        if (slot<0 || start > end || size > end-start) { write_unlock(); return NO_SPACE; }
        int overlap = 0;
        for (int i=0; i<LIMIT; ++i) {
            Block b = blocks[i];
            if (b.used && start < b.start+b.size && b.start < start+size) {
                start = align_up(b.start+b.size, alignment); overlap = 1; break;
            }
        }
        if (!overlap) break;
    }
    blocks[slot] = (Block){start, size, type, 1}; *out = (int64_t)start;
    ++allocations; live_bytes += size;
    write_unlock();
    printf("Runtime: direct allocation offset=0x%" PRIx64 ", size=%" PRIu64 " bytes, type=%d\n", start, size, type);
    return 0;
}
static int32_t map_direct_locked(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!out || physical < 0 || (uint64_t)physical % PAGE || !size || size % PAGE || !valid_alignment(alignment) ||
        (prot & ~0x37)) return INVALID;
    if (!direct_range_allocated((uint64_t)physical,size)) return INVALID;
    int32_t result=place(out,size,prot,flags,alignment,KIND_DIRECT,block_type((uint64_t)physical),(uint64_t)physical);
    if (!result) ++maps;
    return result;
}
static ABI int32_t direct_map(void **out, uint64_t size, int prot, int flags, int64_t physical, uint64_t alignment) {
    write_lock();
    int32_t result=map_direct_locked(out,size,prot,flags,physical,alignment);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped direct memory %p, size=%" PRIu64 ", prot=0x%x\n", *out, size, prot);
    return result;
}
static ABI int32_t direct_map_named(void **out, uint64_t size, int prot, int flags, int64_t physical,
                                    uint64_t alignment, const char *name) {
    (void)name; return direct_map(out,size,prot,flags,physical,alignment);
}
static int32_t unmap_locked(uintptr_t start, uint64_t size) {
    if (start%PAGE || !size) return INVALID;
    uint64_t end=start+align_up(size,PAGE);
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i)
        if (vmas[i].kind!=KIND_RESERVED) {
            uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
            queue_hook(HOOK_UNMAP,a,b-a);
        }
    for (size_t i=vma_index(start); i<vma_count && vmas[i].start<end; ++i) {
        uintptr_t a=vmas[i].start>start ? vmas[i].start : start, b=vmas[i].end<end ? vmas[i].end : end;
        if (host_unmap(a,b-a)) return INVALID;
    }
    drop_range(start,end);
    return 0;
}
static ABI int32_t direct_unmap(void *address, uint64_t size) {
    /* The GPU forgets the range before it disappears (as shadPS4's early GPU unmap). */
    if (hook_unmap && !((uintptr_t)address%PAGE) && size) hook_unmap((uintptr_t)address,align_up(size,PAGE));
    write_lock();
    size_t before=pending_count;
    int32_t result=unmap_locked((uintptr_t)address,size);
    pending_count=before; /* already notified above */
    write_unlock();
    return result;
}
static ABI int32_t direct_release(uint64_t start, uint64_t size) {
    if (start % PAGE || size % PAGE) return INVALID;
    if (!size) return 0;
    write_lock();
    if (!direct_range_allocated(start,size)) { write_unlock(); return INVALID; }
    /* Mappings of released physical pages disappear with them. */
    for (size_t i=0; i<vma_count;) {
        Vma v=vmas[i];
        uint64_t length=v.end-v.start;
        if (v.kind==KIND_DIRECT && v.phys<start+size && start<v.phys+length) {
            uint64_t a=v.phys>start ? v.phys : start, b=v.phys+length<start+size ? v.phys+length : start+size;
            unmap_locked(v.start+(a-v.phys),b-a);
            i=vma_index(v.start);
            continue;
        }
        ++i;
    }
    for (int i=0; i<LIMIT; ++i) {
        Block *b=&blocks[i];
        if (!b->used || b->start>=start+size || start>=b->start+b->size) continue;
        uint64_t a=b->start>start ? b->start : start, e=b->start+b->size<start+size ? b->start+b->size : start+size;
        Block left=*b, right=*b;
        left.size=a-b->start; right.start=e; right.size=b->start+b->size-e;
        b->used=0;
        if (left.size) *b=left;
        if (right.size) for (int j=0;j<LIMIT;++j) if (!blocks[j].used) { blocks[j]=right; break; }
        live_bytes-=e-a;
    }
    host_discard(start,size); /* zero on reuse */
    write_unlock();
    flush_hooks();
    return 0;
}
static int32_t map_flexible_locked(void **inout, uint64_t size, int prot, int flags) {
    if (!inout || !size || size%PAGE || (prot & ~0x37)) return INVALID;
    if (flexible_bytes+size>FLEXIBLE_SIZE || pool()) return NO_MEMORY;
    uint64_t phys=flex_alloc(size);
    if (phys==UINT64_MAX) return NO_MEMORY;
    int32_t result=place(inout,size,prot,flags,PAGE,KIND_FLEXIBLE,0,phys);
    if (result) flex_free(phys,size); else ++flexible_maps;
    return result;
}
static ABI int32_t map_flexible(void **inout, uint64_t size, int prot, int flags) {
    write_lock();
    int32_t result=map_flexible_locked(inout,size,prot,flags);
    write_unlock();
    flush_hooks();
    if (!result) printf("Runtime: mapped flexible memory %p, size=%" PRIu64 ", prot=0x%x\n", *inout, size, prot);
    else fprintf(stderr,"Runtime: flexible map of %" PRIu64 " bytes failed (0x%x), in use=%" PRIu64 "\n",
                 size, (unsigned)result, flexible_bytes);
    return result;
}
static ABI int32_t map_flexible_named(void **inout, uint64_t size, int prot, int flags, const char *name) {
    (void)name; return map_flexible(inout,size,prot,flags);
}
static ABI int32_t release_flexible(void *address, uint64_t size) { return direct_unmap(address,size); }
/* POSIX mmap as libc uses it: anonymous memory from the flexible pool. File mappings fail with
 * ENODEV (no guest file descriptor can be mapped). */
static ABI void *posix_mmap(void *address, uint64_t size, int prot, int flags, int fd, int64_t offset) {
    if (!(flags & 0x1000) || fd!=-1 || offset) { *runtime_errno()=19; return (void *)-1; }
    void *out=address;
    int32_t r=map_flexible(&out,(size+PAGE-1)&~(PAGE-1),prot,flags & 0x10);
    if (r) { *runtime_errno()=12; return (void *)-1; }
    return out;
}
static ABI int32_t posix_msync(void *address, uint64_t size, int flags) { (void)address; (void)size; (void)flags; return 0; }
static ABI int32_t reserve_range(void **inout, uint64_t size, int flags, uint64_t alignment) {
    if (!alignment) alignment=PAGE;
    if (!inout || !size || size%PAGE || !valid_alignment(alignment)) return INVALID;
    write_lock();
    if ((flags & MAP_FIXED_FLAG) && overlaps((uintptr_t)*inout,(uintptr_t)*inout+size,0)) {
        write_unlock(); return NO_MEMORY;
    }
    int32_t result=place(inout,size,0,flags,alignment,KIND_RESERVED,0,0);
    write_unlock();
    if (!result) printf("Runtime: reserved virtual range %p, size=%" PRIu64 "\n", *inout, size);
    return result;
}
static int32_t protect_locked(uintptr_t start, uint64_t size, int prot, int type) {
    uintptr_t end=start+align_up(size,PAGE);
    start&=~(uintptr_t)(PAGE-1);
    if ((prot & ~0x37) || !covered(start,end,0)) return INVALID;
    if (host_protect(start,end-start,host_prot(prot))) return INVALID;
    int error; size_t i=carve(start,end,&error);
    if (error) return NO_MEMORY;
    for (; i<vma_count && vmas[i].start<end; ++i) { vmas[i].prot=prot; if (type>=0) vmas[i].type=type; }
    queue_hook(HOOK_INVALIDATE,start,end-start);
    ++protects;
    return 0;
}
static ABI int32_t kernel_mprotect(const void *address, uint64_t size, int prot) {
    write_lock();
    int32_t result=protect_locked((uintptr_t)address,size,prot,-1);
    write_unlock();
    flush_hooks();
    return result;
}
static ABI int32_t query_protection(void *address, void **start, void **end, uint32_t *prot) {
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || vmas[i].start>(uintptr_t)address || vmas[i].kind==KIND_RESERVED) {
        read_unlock(); return ACCESS;
    }
    /* Report the whole contiguous run with identical protection. */
    size_t first=i, last=i;
    while (first && vmas[first-1].end==vmas[first].start && vmas[first-1].kind!=KIND_RESERVED && vmas[first-1].prot==vmas[i].prot) --first;
    while (last+1<vma_count && vmas[last+1].start==vmas[last].end && vmas[last+1].kind!=KIND_RESERVED && vmas[last+1].prot==vmas[i].prot) ++last;
    if (start) *start=(void *)vmas[first].start;
    if (end) *end=(void *)vmas[last].end;
    if (prot) *prot=(uint32_t)vmas[i].prot;
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t virtual_query(const void *address, int flags, VirtualQueryInfo *info, uint64_t info_size) {
    if (!info || info_size!=sizeof(*info)) return INVALID;
    read_lock();
    size_t i=vma_index((uintptr_t)address);
    if (i==vma_count || (vmas[i].start>(uintptr_t)address && !(flags & 1))) { read_unlock(); return ACCESS; }
    Vma v=vmas[i];
    memset(info,0,sizeof(*info));
    info->start=v.start; info->end=v.end;
    info->offset=v.kind==KIND_DIRECT ? v.phys : 0;
    info->protection=v.prot; info->memory_type=v.type;
    info->flags=(v.kind==KIND_FLEXIBLE ? 1u : 0) | (v.kind==KIND_DIRECT ? 2u : 0) | (v.kind!=KIND_RESERVED ? 16u : 0);
    strcpy(info->name, v.kind==KIND_RESERVED ? "reserved" : v.kind==KIND_DIRECT ? "direct" : "flexible");
    __atomic_add_fetch(&queries,1,__ATOMIC_RELAXED);
    read_unlock();
    return 0;
}
static ABI int32_t direct_memory_type(uint64_t phys, int *type, void **start, void **end) {
    read_lock();
    for (int i=0;i<LIMIT;++i) if (blocks[i].used && phys>=blocks[i].start && phys-blocks[i].start<blocks[i].size) {
        if (type) *type=blocks[i].type;
        if (start) *start=(void *)(uintptr_t)blocks[i].start;
        if (end) *end=(void *)(uintptr_t)(blocks[i].start+blocks[i].size);
        read_unlock(); return 0;
    }
    read_unlock();
    return ACCESS;
}
static ABI int32_t batch_map2(BatchEntry *entries, int count, int *processed, int flags) {
    if (!entries || count<0) return INVALID;
    int32_t result=0; int done=0;
    write_lock();
    for (; done<count; ++done) {
        BatchEntry *e=&entries[done];
        switch (e->operation) {
        case 0: result=map_direct_locked(&e->start,e->length,(uint8_t)e->prot,flags,(int64_t)e->offset,0); break;
        case 1: result=unmap_locked((uintptr_t)e->start,e->length); break;
        case 2: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,-1); break;
        case 3: result=map_flexible_locked(&e->start,e->length,(uint8_t)e->prot,flags); break;
        case 4: result=protect_locked((uintptr_t)e->start,e->length,(uint8_t)e->prot,e->type); break;
        default: result=INVALID;
        }
        if (result) break;
    }
    write_unlock();
    flush_hooks();
    if (processed) *processed=done;
    return result;
}
static ABI int32_t batch_map(BatchEntry *entries, int count, int *processed) {
    return batch_map2(entries,count,processed,MAP_FIXED_FLAG);
}
static ABI int32_t main_allocate(uint64_t size, uint64_t alignment, int type, int64_t *out) {
    return direct_allocate(0,(int64_t)POOL_SIZE,size,alignment,type,out);
}
/* Largest free direct-memory range in [low,high) at the given alignment. */
static ABI int32_t direct_available(int64_t low, int64_t high, uint64_t alignment, int64_t *out_start, uint64_t *out_size) {
    if (!alignment) alignment=PAGE;
    if (!out_start || !out_size || low<0 || high<=low || !valid_alignment(alignment)) return INVALID;
    uint64_t end=(uint64_t)high<POOL_SIZE ? (uint64_t)high : POOL_SIZE, best=0, best_start=0;
    read_lock();
    for (uint64_t at=(uint64_t)low; at<end;) {
        uint64_t next=end, resume=end;
        for (int i=0;i<LIMIT;++i) if (blocks[i].used && blocks[i].start+blocks[i].size>at && blocks[i].start<next) {
            next=blocks[i].start>at ? blocks[i].start : at; resume=blocks[i].start+blocks[i].size;
        }
        uint64_t start=align_up(at,alignment);
        if (next>start && next-start>best) { best=next-start; best_start=start; }
        if (next==end) break;
        at=resume>at ? resume : at+PAGE;
    }
    read_unlock();
    if (!best) return NO_SPACE;
    *out_start=(int64_t)best_start; *out_size=best;
    return 0;
}
/* Memory pools: Expand adds direct memory to the pool, Reserve takes virtual space, Commit maps
 * free pool pages into it (possibly as several physical runs), Decommit gives them back. */
extern uint64_t runtime_disabled_optimizations;
#define POOL_CHUNKS 64
/* free_hint: every page below it is used. Commits searched from page 0 each time, under the
 * memory write lock (MediEvil: up to 28.7% of a game thread in that scan). */
static struct { uint64_t phys, pages, free_hint; unsigned char *used; } pool_chunks[POOL_CHUNKS];
static ABI int32_t pool_expand(int64_t low, int64_t high, uint64_t size, uint64_t alignment, int64_t *out) {
    if (!out || !size || size%(64*1024)) return INVALID;
    int32_t r=direct_allocate(low,high,size,alignment ? alignment : 64*1024,0,out);
    if (r) return r;
    write_lock();
    int slot=-1;
    for (int i=0;i<POOL_CHUNKS;++i) if (!pool_chunks[i].pages) { slot=i; break; }
    unsigned char *used=slot>=0 ? calloc(size/PAGE,1) : NULL;
    if (used) pool_chunks[slot].phys=(uint64_t)*out, pool_chunks[slot].pages=size/PAGE, pool_chunks[slot].free_hint=0,
              pool_chunks[slot].used=used;
    write_unlock();
    if (!used) return NO_MEMORY;
    printf("Runtime: memory pool expanded by %" PRIu64 " bytes at 0x%" PRIx64 "\n",size,(uint64_t)*out);
    return 0;
}
static ABI int32_t pool_reserve(void *address, uint64_t size, uint64_t alignment, int flags, void **out) {
    if (!out) return INVALID;
    *out=address;
    return reserve_range(out,size,flags,alignment ? alignment : 64*1024);
}
/* Pool page states (pool_chunks[].used): free, taken, or free but still mapped by a view kept
 * at its last address (pool_decommit), which a commit there takes back without a new view. */
enum { POOL_FREE, POOL_TAKEN, POOL_KEPT };
/* The first free run (at most want pages) in the pool; marks it taken. Pages a kept view waits
 * for are only handed out when no other page is free: taking them makes the next commit at the
 * view's address unmap it, which stalls on the kernel (8-37 ms with ~13000 views, T-M1). */
static int pool_take_locked(uint64_t want, uint64_t *phys, uint64_t *pages) {
    for (int c=0;c<POOL_CHUNKS;++c) {
        uint64_t p=pool_chunks[c].free_hint;
        while (p<pool_chunks[c].pages && pool_chunks[c].used[p]!=POOL_FREE) ++p;
        pool_chunks[c].free_hint=p;
        if (p>=pool_chunks[c].pages) continue;
        uint64_t n=0;
        while (p+n<pool_chunks[c].pages && n<want && pool_chunks[c].used[p+n]==POOL_FREE) ++n;
        memset(pool_chunks[c].used+p,POOL_TAKEN,n);
        pool_chunks[c].free_hint=p+n;
        *phys=pool_chunks[c].phys+p*PAGE; *pages=n;
        return 1;
    }
    for (int c=0;c<POOL_CHUNKS;++c) {
        uint64_t p=0;
        while (p<pool_chunks[c].pages && pool_chunks[c].used[p]!=POOL_KEPT) ++p;
        if (p>=pool_chunks[c].pages) continue;
        uint64_t n=0;
        while (p+n<pool_chunks[c].pages && n<want && pool_chunks[c].used[p+n]==POOL_KEPT) ++n;
        memset(pool_chunks[c].used+p,POOL_TAKEN,n);
        *phys=pool_chunks[c].phys+p*PAGE; *pages=n;
        return 1;
    }
    return 0;
}
/* Takes up to `want` free pages starting exactly at `phys` (a kept view's pages); 0 if none. */
static int pool_take_at_locked(uint64_t phys, uint64_t want, uint64_t *pages) {
    for (int c=0;c<POOL_CHUNKS;++c)
        if (pool_chunks[c].pages && phys>=pool_chunks[c].phys && phys<pool_chunks[c].phys+pool_chunks[c].pages*PAGE) {
            const uint64_t first=(phys-pool_chunks[c].phys)/PAGE;
            uint64_t n=0;
            while (first+n<pool_chunks[c].pages && n<want && pool_chunks[c].used[first+n]!=POOL_TAKEN) ++n;
            if (!n) return 0;
            memset(pool_chunks[c].used+first,POOL_TAKEN,n);
            if (pool_chunks[c].free_hint==first) pool_chunks[c].free_hint=first+n;
            *pages=n;
            return 1;
        }
    return 0;
}
/* gow3 (Windows): UE4 commits and decommits 64 KiB pool pages ~6000 times a second (MediEvil,
 * T-H1). Unmapping and mapping a view of the guest memory section each time left ~15000 views,
 * and the kernel's section trims (MiTrimSection, one System worker for seconds) then blocked the
 * render thread on a push lock: 2-5 s frame pacing episodes. A decommit now keeps the view,
 * inaccessible, and a commit at that address takes the same physical pages back when they are
 * free, so it only changes the protection. Toggle bit 31 restores unmapping. */
#define POOL_KEEP_VIEWS UINT64_C(0x80000000)
static int pool_keep_views(void) {
#ifdef _WIN32
    return !(__atomic_load_n(&runtime_disabled_optimizations,__ATOMIC_RELAXED) & POOL_KEEP_VIEWS);
#else
    return 0;
#endif
}
static void pool_give_locked(uint64_t phys, uint64_t size, int state) {
    for (int c=0;c<POOL_CHUNKS;++c)
        if (pool_chunks[c].pages && phys>=pool_chunks[c].phys && phys<pool_chunks[c].phys+pool_chunks[c].pages*PAGE) {
            const uint64_t first=(phys-pool_chunks[c].phys)/PAGE;
            memset(pool_chunks[c].used+first,state,size/PAGE);
            if (state==POOL_FREE && first<pool_chunks[c].free_hint) pool_chunks[c].free_hint=first;
            return;
        }
}
static int32_t pool_commit_impl(void *address, uint64_t size, int type, int prot);
static ABI int32_t pool_commit(void *address, uint64_t size, int type, int prot, int flags) {
    (void)flags;
    if (!host_stats_enabled()) return pool_commit_impl(address,size,type,prot);
    const uint64_t t0=host_now_ns();
    const int32_t r=pool_commit_impl(address,size,type,prot);
    HOST_STAT_ADD(commits,1); HOST_STAT_ADD(commit_ns,host_now_ns()-t0);
    return r;
}
static int32_t pool_commit_impl(void *address, uint64_t size, int type, int prot) {
    if (!address || (uintptr_t)address%PAGE || !size || size%PAGE) return INVALID;
    /* One physical run per step; GPU notifications drain between steps (bounded queue). */
    for (uint64_t done=0; done<size;) {
        uint64_t phys, pages;
        write_lock();
        void *at=(char *)address+done;
        uint64_t kept_bytes;
        int got=0;
        if (pool_keep_views() && host_view_phys((uintptr_t)at,&phys,&kept_bytes)) {
            const uint64_t want=size-done<kept_bytes ? size-done : kept_bytes;
            got=pool_take_at_locked(phys,want/PAGE,&pages);
        }
        if (!got && !pool_take_locked((size-done)/PAGE,&phys,&pages)) { write_unlock(); return NO_MEMORY; }
        int32_t r=place(&at,pages*PAGE,prot,MAP_FIXED_FLAG,PAGE,KIND_DIRECT,type,phys);
        if (r) pool_give_locked(phys,pages*PAGE,POOL_FREE);
        write_unlock();
        flush_hooks();
        if (r) return r;
        done+=pages*PAGE;
    }
    return 0;
}
static int32_t pool_decommit_impl(void *address, uint64_t size);
static ABI int32_t pool_decommit(void *address, uint64_t size, int flags) {
    (void)flags;
    if (!host_stats_enabled()) return pool_decommit_impl(address,size);
    const uint64_t t0=host_now_ns();
    const int32_t r=pool_decommit_impl(address,size);
    HOST_STAT_ADD(decommits,1); HOST_STAT_ADD(decommit_ns,host_now_ns()-t0);
    return r;
}
static int32_t pool_decommit_impl(void *address, uint64_t size) {
    uintptr_t start=(uintptr_t)address, end=start+size;
    if (start%PAGE || !size || size%PAGE) return INVALID;
    /* One mapped region per step, returned to the pool and left reserved. */
    for (uintptr_t at=start; at<end;) {
        write_lock();
        size_t i=vma_index(at);
        while (i<vma_count && vmas[i].end<=at) ++i;
        if (i>=vma_count || vmas[i].start>=end) { write_unlock(); break; }
        uintptr_t a=vmas[i].start>at ? vmas[i].start : at, b=vmas[i].end<end ? vmas[i].end : end;
        int32_t r=0;
        if (vmas[i].kind==KIND_DIRECT && pool_keep_views()) {
            pool_give_locked(vmas[i].phys+(a-vmas[i].start),b-a,POOL_KEPT);
            queue_hook(HOOK_UNMAP,a,b-a);
            r=host_keep_view(a,b-a);
            drop_range(a,b);
            if (!r && vma_insert(vma_index(a),(Vma){a,b,KIND_RESERVED,0,0,0})) r=NO_MEMORY;
        } else if (vmas[i].kind!=KIND_RESERVED) {
            if (vmas[i].kind==KIND_DIRECT) pool_give_locked(vmas[i].phys+(a-vmas[i].start),b-a,POOL_FREE);
            r=unmap_locked(a,b-a);
            void *p=(void *)a;
            if (!r) r=place(&p,b-a,0,MAP_FIXED_FLAG,PAGE,KIND_RESERVED,0,0);
        }
        write_unlock();
        flush_hooks();
        if (r) return r;
        at=b;
    }
    return 0;
}
static const RuntimeExport exports[]={
    {"mmap",posix_mmap}, {"msync",posix_msync},
    {"sceKernelAllocateMainDirectMemory",main_allocate}, {"sceKernelAvailableDirectMemorySize",direct_available},
    {"sceKernelMemoryPoolExpand",pool_expand}, {"sceKernelMemoryPoolReserve",pool_reserve},
    {"sceKernelMemoryPoolCommit",pool_commit}, {"sceKernelMemoryPoolDecommit",pool_decommit},
    {"sceKernelGetDirectMemorySize",direct_size}, {"sceKernelAllocateDirectMemory",direct_allocate},
    {"sceKernelMapDirectMemory",direct_map}, {"sceKernelMapNamedDirectMemory",direct_map_named},
    {"sceKernelReleaseDirectMemory",direct_release}, {"sceKernelMunmap",direct_unmap}, {"munmap",direct_unmap},
    {"sceKernelMapFlexibleMemory",map_flexible}, {"sceKernelMapNamedFlexibleMemory",map_flexible_named},
    {"sceKernelReleaseFlexibleMemory",release_flexible}, {"sceKernelReserveVirtualRange",reserve_range},
    {"sceKernelMprotect",kernel_mprotect}, {"sceKernelQueryMemoryProtection",query_protection},
    {"sceKernelVirtualQuery",virtual_query}, {"sceKernelGetDirectMemoryType",direct_memory_type},
    {"sceKernelBatchMap",batch_map}, {"sceKernelBatchMap2",batch_map2},
};
uintptr_t runtime_memory_resolve(const char *name) {
    /* Direct-memory NIDs are exercised by test_runtime.c before names are loaded. */
    if (!strcmp(name, "pO96TwzOm5E#libkernel")) return (uintptr_t)direct_size;
    if (!strcmp(name, "rTXw65xmLIA#libkernel")) return (uintptr_t)direct_allocate;
    if (!strcmp(name, "L-Q3LEjIbgA#libkernel")) return (uintptr_t)direct_map;
    if (!strcmp(name, "MBuItvba6z8#libkernel")) return (uintptr_t)direct_release;
    if (!strcmp(name, "cQke9UuBQOk#libkernel")) return (uintptr_t)direct_unmap;
    return RUNTIME_LOOKUP(exports,name);
}
/* Host-owned memory the guest can see (image, stacks, trampolines) lives in
 * [LOW_MIN, USER_MIN): PS4 code packs pointers into 40-bit fields. */
#define LOW_MIN UINT64_C(0x0800000000)
static uintptr_t low_next=LOW_MIN;
void *runtime_low_map(size_t size, int prot) {
    size=align_up(size,PAGE);
    write_lock();
#ifdef _WIN32
    void *p=NULL;
    while (!p && low_next+size<=USER_MIN) {
        p=VirtualAlloc((void *)low_next,size,MEM_RESERVE|MEM_COMMIT,win_prot(prot));
        low_next+=size+UINT64_C(65536); /* unmapped gap catches overruns; allocation granularity */
    }
    write_unlock();
    return p;
#else
    void *p=MAP_FAILED;
    while (low_next+size<=USER_MIN) {
        p=mmap((void *)low_next,size,prot,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        low_next+=size+PAGE; /* unmapped gap catches overruns */
        if (p!=MAP_FAILED) break;
    }
    write_unlock();
    return p==MAP_FAILED ? NULL : p;
#endif
}
/* ---- GPU library interface (gpu/shim/gow3gpu.cpp) ---- */
/* Optimizations switched off at run time (diagnostics): the number in the file named by
 * GOW3_TOGGLE_FILE, re-read every 250 ms. Bits: 1 region cache, 2 fetch shader cache,
 * 4 page tracking early exit, 8 pending-op poll limit, 16 threaded Vulkan recording,
 * 32 texture descriptor cache, 64 lock-free upload check, 128 image lookup cache,
 * 256 buffer uploads on the recording thread, 512 barrier tracker insert memo,
 * 1024 texture binding memo, 2048 page-granular read tracking for barriers,
 * 4096 resource sharps read by the draw-preparation workers,
 * 8192 prepared draws from the draw-preparation workers,
 * 16384 small read-only buffer copies on the recording thread,
 * 32768 hot pages (opt-in with GOW3_HOT_PAGES=1),
 * 65536 unprotect the 256 KiB window around a guest write fault (GOW3_FAULT_WINDOW KiB),
 * 131072 large guest memory copies split across copy threads (GOW3_COPY_THREADS),
 * 262144 with GOW3_ASYNC_FENCES=1: wait for guest copies at fences again,
 * 524288 small guest copies batched for the copy threads instead of the recording thread,
 * 1073741824 the lock-free UpdateImage path for clean, tracked images,
 * 2147483648 (Windows) pool views kept across a decommit (pool_decommit). */
/* Bits 32 and up: the draw pipeline and related GPU thread work (gpu/shim/gow3_toggles.h);
 * 288230376151711744 (bit 58) volumes rendered slice by slice through 2D array views.
 * An optional second number in the file is a second word of bits (all 64 of the first are
 * taken): 1 bulk descriptor copy per push, 2 sampler handles reused within a GC period,
 * 4 constant ring reuse (Gow3Toggle2 in gow3_toggles.h). */
uint64_t runtime_disabled_optimizations, runtime_disabled_optimizations2;
/* Speculative readers of guest memory (GPU draw-preparation workers) register a recovery
 * point: a fault on that thread jumps back to it instead of terminating (probe.c). */
__thread sigjmp_buf *runtime_fault_recover;
static void *toggle_watcher(void *path) {
    for (unsigned long long last=ULLONG_MAX,last2=ULLONG_MAX;;) {
        FILE *f=fopen(path,"r");
        unsigned long long value=0,value2=0;
        if (f) {
            const int read=fscanf(f,"%llu %llu",&value,&value2);
            if (read<1) value=0;
            if (read<2) value2=0;
            fclose(f);
        }
        if (value!=last) {
            __atomic_store_n(&runtime_disabled_optimizations,(uint64_t)value,__ATOMIC_RELEASE);
            printf("Runtime: disabled optimizations mask=%llu\n",value);
            last=value;
        }
        if (value2!=last2) {
            __atomic_store_n(&runtime_disabled_optimizations2,(uint64_t)value2,__ATOMIC_RELEASE);
            if (value2 || last2!=ULLONG_MAX) printf("Runtime: disabled optimizations second word=%llu\n",value2);
            last2=value2;
        }
#ifdef _WIN32
        compat_sleep_ns(250000000);
#else
        struct timespec t={0,250000000}; nanosleep(&t,NULL);
#endif
    }
    return NULL;
}
/* gow3: pages a GPU shader wrote (the buffer cache's GPU-modified ranges), one bit per 4 KiB page
 * below 2^40, set and cleared by the GPU side. The guest's libc writes into them go to the GPU side
 * (it writes exactly those bytes into its copy too) instead of faulting: a write fault there reads
 * the GPU's data back and waits for the whole GPU (~27 a second, ~3 ms each in MediEvil, T-D1b).
 * A stale bit only costs that path. */
#define GPU_WRITTEN_LIMIT (1ull<<40)
typedef int (*PreciseWrite)(uintptr_t address, const void *src, int value, uint64_t size);
static uint64_t *gpu_written; /* atomic words */
static PreciseWrite hook_precise_write;
void runtime_memory_set_precise_write_hook(PreciseWrite hook) {
    if (!gpu_written) {
        gpu_written=VirtualAlloc(NULL,GPU_WRITTEN_LIMIT>>15,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
        if (!gpu_written) return;
    }
    __atomic_store_n(&hook_precise_write,hook,__ATOMIC_RELEASE);
}
void runtime_memory_mark_gpu_written(uintptr_t address, uint64_t size, int written) {
    if (!gpu_written || !size || address>=GPU_WRITTEN_LIMIT) return;
    uint64_t first=address>>12, last=(address+size-1)>>12;
    if (last>=(GPU_WRITTEN_LIMIT>>12)) last=(GPU_WRITTEN_LIMIT>>12)-1;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (written) __atomic_fetch_or(&gpu_written[word],mask,__ATOMIC_RELEASE);
        else __atomic_fetch_and(&gpu_written[word],~mask,__ATOMIC_RELEASE);
    }
}
int runtime_memory_precise_write(void *dst, const void *src, int value, uint64_t size) {
    const uintptr_t address=(uintptr_t)dst;
    const PreciseWrite hook=__atomic_load_n(&hook_precise_write,__ATOMIC_ACQUIRE);
    if (!hook || !size || address>=GPU_WRITTEN_LIMIT || size>GPU_WRITTEN_LIMIT-address) return 0;
    const uint64_t first=address>>12, last=(address+size-1)>>12;
    for (uint64_t word=first>>6; word<=last>>6; ++word) {
        const uint64_t lo=word==first>>6 ? first&63 : 0, hi=word==last>>6 ? last&63 : 63;
        const uint64_t mask=(hi==63 ? ~0ull : (2ull<<hi)-1) & ~((1ull<<lo)-1);
        if (__atomic_load_n(&gpu_written[word],__ATOMIC_ACQUIRE) & mask) return hook(address,src,value,size);
    }
    return 0;
}
void runtime_memory_set_gpu_hooks(GpuRange map, GpuRange unmap, GpuRange invalidate) {
    const char *toggles=getenv("GOW3_TOGGLE_FILE");
    static pthread_t watcher;
    if (toggles && !watcher) pthread_create(&watcher,NULL,toggle_watcher,(void *)toggles);
    write_lock();
    hook_map=map; hook_unmap=unmap; hook_invalidate=invalidate;
    for (size_t i=0;i<vma_count;++i) if (vmas[i].kind!=KIND_RESERVED) queue_hook(HOOK_MAP,vmas[i].start,vmas[i].end-vmas[i].start);
    write_unlock();
    flush_hooks();
}
/* Writes through the backing view; 0 when part of the range has no backing. */
int runtime_memory_write_backing(uintptr_t address, const void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy(backing_base+vmas[i].phys+(at-vmas[i].start),(const unsigned char *)data+(at-address),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* gow3: reads through the backing view (past the page protection the GPU side keeps);
 * 0 when part of the range has no backing. */
int runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size) {
    read_lock();
    int ok=1;
    for (uintptr_t at=address, end=address+size; at<end && ok;) {
        size_t i=vma_index(at);
        if (i==vma_count || vmas[i].start>at || vmas[i].kind==KIND_RESERVED) { ok=0; break; }
        uint64_t n=(vmas[i].end<end ? vmas[i].end : end)-at;
        memcpy((unsigned char *)data+(at-address),backing_base+vmas[i].phys+(at-vmas[i].start),n);
        at+=n;
    }
    read_unlock();
    return ok;
}
/* Per-thread cache of recently found regions for the GPU's per-draw queries:
 * entries hold for as long as the table generation they were read at. */
typedef struct { uint64_t generation, stamp; uintptr_t start, end; int kind; } CachedRegion;
static __thread CachedRegion region_cache[8];
static __thread unsigned region_cache_next;
/* GOW3_REGION_BUCKETS=0: the table-wide generation, as before L1. */
static int region_buckets_on(void) {
    static int on=-1;
    if (on<0) { const char *env=getenv("GOW3_REGION_BUCKETS"); on=!(env && env[0]=='0'); }
    return on;
}
static const CachedRegion *cached_region(uintptr_t address) {
    const int buckets=region_buckets_on();
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    /* Odd: a writer holds the lock. With buckets the cache still answers (L2: a writer bumps a
     * range's buckets before its host mapping changes); without them it changes host mappings
     * before the table, so no cached answers. */
    if (!buckets && (generation & 1)) return NULL;
    for (unsigned i=0;i<8;++i) {
        const CachedRegion *c=&region_cache[i];
        if (!c->end || c->start>address || address>=c->end) continue;
        if (buckets ? c->stamp && c->stamp==region_stamp(c->start,c->end) : c->generation==generation) return c;
    }
    return NULL;
}
/* Called under the read lock after a lookup of address. */
static void cache_region(size_t i) {
    uint64_t generation=__atomic_load_n(&table_generation,__ATOMIC_ACQUIRE);
    if (i==vma_count) return;
    if (generation & 1) return;
    region_cache[region_cache_next++ % 8]=(CachedRegion){generation,
        region_stamp(vmas[i].start,vmas[i].end),vmas[i].start,vmas[i].end,vmas[i].kind};
}
/* Lock-free runtime_memory_clamp (see vma_seq): 1 with the length in *out when no table entry
 * changed during the search. */
/* A table edit (a memmove of up to ~13,000 entries) takes tens of microseconds; the lock waits for
 * the whole writer section, host calls included. Spin through an edit for up to ~100 us. */
#define OPTIMISTIC_SPIN_CYCLES (UINT64_C(400000))
static int optimistic_enabled(void) {
    int enabled=__atomic_load_n(&perf_optimistic,__ATOMIC_RELAXED);
    if (enabled<0) {
        const char *e=getenv("GOW3_OPTIMISTIC_CLAMP");
        enabled=!(e && e[0]=='0');
        __atomic_store_n(&perf_optimistic,enabled,__ATOMIC_RELAXED);
    }
    return enabled && !exclusive_depth;
}
static int clamp_optimistic(uintptr_t address, uint64_t size, uint64_t *out) {
    if (!optimistic_enabled()) return 0;
    const uint64_t deadline=__builtin_ia32_rdtsc()+OPTIMISTIC_SPIN_CYCLES;
    for (;;) {
        if (!__atomic_load_n(&vma_optimistic,__ATOMIC_ACQUIRE)) return 0;
        const uint64_t s1=__atomic_load_n(&vma_seq,__ATOMIC_ACQUIRE);
        if (s1 & 1) {
            if (__builtin_ia32_rdtsc()>deadline) return 0;
            __builtin_ia32_pause();
            continue;
        }
        const Vma *v=__atomic_load_n(&vmas,__ATOMIC_ACQUIRE);
        size_t n=__atomic_load_n(&vma_count,__ATOMIC_ACQUIRE);
        if (!v) return 0;
        if (n>VMA_FIXED_CAPACITY) n=VMA_FIXED_CAPACITY;
        size_t lo=0, hi=n;
        while (lo<hi) { size_t mid=(lo+hi)/2; if (v[mid].end<=address) lo=mid+1; else hi=mid; }
        uint64_t length=0;
        for (size_t i=lo; i<n && length<size; ++i) {
            if (v[i].start>address+length || v[i].kind==KIND_RESERVED) break;
            length=v[i].end-address;
        }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vma_seq,__ATOMIC_RELAXED)==s1) { *out=length<size ? length : size; return 1; }
        if (__builtin_ia32_rdtsc()>deadline) return 0;
    }
}
/* Mapped length from address, up to size, across adjacent mappings. */
uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return size;
    { uint64_t length; if (clamp_optimistic(address,size,&length)) return length; }
    read_lock();
    { size_t i=vma_index(address); if (i<vma_count && vmas[i].start<=address) cache_region(i); }
    uint64_t length=0;
    for (size_t i=vma_index(address); i<vma_count && length<size; ++i) {
        if (vmas[i].start>address+length || vmas[i].kind==KIND_RESERVED) break;
        length=vmas[i].end-address;
    }
    read_unlock();
    return length<size ? length : size;
}
/* Region around address: [start,end) mapped or a gap; 0 past the last mapping. */
/* Lock-free runtime_memory_region (see vma_seq; T-P3: CopySparseMemory's lookups took the wait
 * over from the clamp): 1 when no table entry changed during the search, *found as below. */
static int region_optimistic(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped, int *found) {
    if (!optimistic_enabled()) return 0;
    const uint64_t deadline=__builtin_ia32_rdtsc()+OPTIMISTIC_SPIN_CYCLES;
    for (;;) {
        if (!__atomic_load_n(&vma_optimistic,__ATOMIC_ACQUIRE)) return 0;
        const uint64_t s1=__atomic_load_n(&vma_seq,__ATOMIC_ACQUIRE);
        if (s1 & 1) {
            if (__builtin_ia32_rdtsc()>deadline) return 0;
            __builtin_ia32_pause();
            continue;
        }
        const Vma *v=__atomic_load_n(&vmas,__ATOMIC_ACQUIRE);
        size_t n=__atomic_load_n(&vma_count,__ATOMIC_ACQUIRE);
        if (!v) return 0;
        if (n>VMA_FIXED_CAPACITY) n=VMA_FIXED_CAPACITY;
        size_t lo=0, hi=n;
        while (lo<hi) { size_t mid=(lo+hi)/2; if (v[mid].end<=address) lo=mid+1; else hi=mid; }
        uintptr_t s=0, e=0; int m=0; const int f=lo<n;
        if (f && v[lo].start<=address) { s=v[lo].start; e=v[lo].end; m=v[lo].kind!=KIND_RESERVED; }
        else if (f) { s=lo ? v[lo-1].end : 0; e=v[lo].start; m=0; }
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&vma_seq,__ATOMIC_RELAXED)==s1) {
            if (f) { *start=s; *end=e; *mapped=m; }
            *found=f;
            return 1;
        }
        if (__builtin_ia32_rdtsc()>deadline) return 0;
    }
}
int runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c) { *start=c->start; *end=c->end; *mapped=c->kind!=KIND_RESERVED; return 1; }
    { int found; if (region_optimistic(address,start,end,mapped,&found)) return found; }
    read_lock();
    size_t i=vma_index(address);
    int found=i<vma_count;
    if (found && vmas[i].start<=address) {
        cache_region(i);
        *start=vmas[i].start; *end=vmas[i].end; *mapped=vmas[i].kind!=KIND_RESERVED;
    } else if (found) {
        *start=i ? vmas[i-1].end : 0; *end=vmas[i].start; *mapped=0;
    }
    read_unlock();
    return found;
}
/* GPU page tracking: restrict host access, never beyond the guest's own protection. */
void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    read_lock();
    for (size_t i=vma_index(address); i<vma_count && vmas[i].start<address+size; ++i) {
        if (vmas[i].kind==KIND_RESERVED) continue;
        uintptr_t a=vmas[i].start>address ? vmas[i].start : address;
        uintptr_t b=vmas[i].end<address+size ? vmas[i].end : address+size;
        int guest=host_prot(vmas[i].prot);
        int want=(read ? PROT_READ : 0)|(write ? PROT_WRITE|PROT_READ : 0)|(guest & PROT_EXEC);
        host_protect(a,b-a,guest & want);
    }
    read_unlock();
}
/* Host view of a guest range, for the GPU backend: the range must be mapped. */
int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    const CachedRegion *c=(runtime_disabled_optimizations & 1) ? NULL : cached_region(address);
    if (c && c->kind!=KIND_RESERVED && size<=c->end-address) return 1;
    read_lock();
    int result=covered(address,address+size,0);
    read_unlock();
    return result;
}
void runtime_memory_report(void) {
    printf("Runtime: direct memory allocations=%zu, maps=%zu, live=%" PRIu64 ", budget=%" PRIu64 "\n",
           allocations, maps, live_bytes, POOL_SIZE);
    printf("Runtime: flexible maps=%zu, in use=%" PRIu64 ", protects=%zu, queries=%zu, regions=%zu\n",
           flexible_maps, flexible_bytes, protects, queries, vma_count);
}

