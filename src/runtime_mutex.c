/* Linux pthread-backed opaque PS4 mutex handles. No guest threads yet. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif
typedef struct { int type; } GuestAttr;
typedef struct {
    pthread_mutex_t native;
#if GOW3_DEV_TOOLS
    /* GOW3_MUTEX_HOLDERS (card tooling/mutex-holders): written by the holder while it holds the lock, read by
     * waiters without the lock (relaxed). depth: the holder's recursion depth as this code saw it. */
    unsigned long owner_tid;
    const char *owner_name;
    uintptr_t owner_site;
    uint64_t owner_since;
    unsigned depth;
#endif
} GuestMutex;
/* locks / unlocks / waits / wakeups: GOW3_FRAME_STATS only (2026-11-16). Every thread wrote them on
 * every call, and they shared cache lines with the region table's vma_seq / vma_count / vmas, which the
 * GPU threads read on every lookup (study-tooling-off-cost.md). */
int runtime_fault_stats_enabled(void);
#define COUNT(counter) do { if (runtime_fault_stats_enabled()) ++(counter); } while (0)
static size_t created, locks, unlocks;
static int32_t orbis_error(int e) {
    if (!e) return 0;
    unsigned code;
    switch (e) {
    case EPERM: code=1; break;
    case ENOMEM: code=12; break;
    case EBUSY: code=16; break;
    case EINVAL: code=22; break;
    case EAGAIN: code=35; break;
    case EDEADLK: code=11; break;
    default: fprintf(stderr, "STOP: unmapped host pthread error %d\n", e); exit(21);
    }
    return (int32_t)(UINT32_C(0x80020000) | code);
}
static ABI int32_t attr_init(GuestAttr **out) {
    if (!out) return orbis_error(EINVAL);
    GuestAttr *attr = malloc(sizeof(*attr));
    if (!attr) return orbis_error(ENOMEM);
    attr->type = 1; *out = attr; return 0;
}
static ABI int32_t attr_type(GuestAttr **attr, int type) {
    if (!attr || !*attr || type < 1 || type > 4) return orbis_error(EINVAL);
    (*attr)->type = type; return 0;
}
/* Priority protocols need a PS4 scheduler; the value is validated and recorded only. */
static ABI int32_t attr_protocol(GuestAttr **attr, int protocol) {
    if (!attr || !*attr || protocol < 0 || protocol > 2) return orbis_error(EINVAL);
    return 0;
}
static ABI int32_t attr_destroy(GuestAttr **attr) {
    if (!attr || !*attr) return orbis_error(EINVAL);
    free(*attr); *attr = NULL; return 0;
}
static ABI int32_t mutex_init(GuestMutex **out, GuestAttr **attr, const char *name) {
    (void)name;
    if (!out || (attr && !*attr)) return orbis_error(EINVAL);
    int type = attr ? (*attr)->type : 1;
    if (type < 1 || type > 4) return orbis_error(EINVAL);
    GuestMutex *mutex = calloc(1, sizeof(*mutex));
    if (!mutex) return orbis_error(ENOMEM);
    pthread_mutexattr_t native_attr;
    int e = pthread_mutexattr_init(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    int native_type = type == 2 ? PTHREAD_MUTEX_RECURSIVE : type == 3 ? PTHREAD_MUTEX_NORMAL : PTHREAD_MUTEX_ERRORCHECK;
    e = pthread_mutexattr_settype(&native_attr, native_type);
    if (!e) e = pthread_mutex_init(&mutex->native, &native_attr);
    pthread_mutexattr_destroy(&native_attr);
    if (e) { free(mutex); return orbis_error(e); }
    *out = mutex; ++created; return 0;
}
static pthread_mutex_t static_init = PTHREAD_MUTEX_INITIALIZER;
/* Statically initialized handles (0/1) are created once, even under contention. */
static int32_t ensure_mutex(GuestMutex **mutex) {
    if (!mutex) return orbis_error(EINVAL);
    uintptr_t value = __atomic_load_n((uintptr_t *)mutex, __ATOMIC_ACQUIRE);
    if (value == 2) return orbis_error(EINVAL);
    if (value >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*mutex < 2) {
        GuestMutex *created_mutex = NULL;
        e = mutex_init(&created_mutex, NULL, NULL);
        if (!e) __atomic_store_n(mutex, created_mutex, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
/* ---- GOW3_MUTEX_HOLDERS=1 with GOW3_WAIT_TRACE (dev build; card tooling/mutex-holders) ----
 * Who holds a guest mutex while another thread waits on it, and what the holder does meanwhile. Rows in the
 * wait trace (after its w rows; research/tools/mutex_holders.py):
 *   m,<end ns>,<waiter tid>,<waiter thread>,<waiter site>,<holder tid>,<holder thread>,<holder lock site>,
 *     <held when the wait began ns>,<waited ns>        a mutex wait of GOW3_WAIT_TRACE_MIN_US or more
 *   h,<end ns>,<tid>,<thread>,<lock site>,<unlock site>,<held ns>      a hold of that long or more
 *   s,<ns>,<holder tid>,<pc>,<return 1>,...<return 7>   the holder sampled while a thread waits on it (Windows:
 *     every ~0.5 ms, the timer's floor; suspended for a moment to read the registers and copy 16 KB of stack,
 *     nothing locked meanwhile; after resuming: rip, then the rbp chain through that copy, else return addresses
 *     found by scanning it, marked ~ because they can be stale)
 * Sites: guest+0x... (eboot), cond_wait (released / taken again inside a condition wait), 0x... host (the exe
 * row's base). Release build: compiled out. Off: one cached load per lock. */
#define COND_SITE ((uintptr_t)1)
#if GOW3_DEV_TOOLS
static void holder_site(uintptr_t site, char *out, size_t size) {
    if (site == COND_SITE) snprintf(out, size, "cond_wait");
    else runtime_wait_format_site(site, out, size);
}
static void holder_taken(GuestMutex *m, uintptr_t site) {
    if (m->depth++) return;
    __atomic_store_n(&m->owner_name, runtime_thread_current_name(), __ATOMIC_RELAXED);
    __atomic_store_n(&m->owner_site, site, __ATOMIC_RELAXED);
    __atomic_store_n(&m->owner_since, runtime_wait_now(), __ATOMIC_RELAXED);
    __atomic_store_n(&m->owner_tid, runtime_wait_thread_id(), __ATOMIC_RELEASE);
}
/* Before the native unlock, while still held. Not the holder (an EPERM unlock): nothing. */
static void holder_releasing(GuestMutex *m, uintptr_t site) {
    if (__atomic_load_n(&m->owner_tid, __ATOMIC_RELAXED) != runtime_wait_thread_id() || !m->depth || --m->depth) return;
    const uint64_t now = runtime_wait_now(), held = now - m->owner_since;
    __atomic_store_n(&m->owner_tid, 0, __ATOMIC_RELEASE);
    if (held < runtime_wait_trace_min_ns()) return;
    char thread[40], taken[40], left[40], row[200];
    runtime_wait_thread_name(m->owner_name, thread, sizeof(thread));
    holder_site(m->owner_site, taken, sizeof(taken));
    holder_site(site, left, sizeof(left));
    snprintf(row, sizeof(row), "h,%llu,%lu,%s,%s,%s,%llu\n", (unsigned long long)now, runtime_wait_thread_id(), thread,
             taken, left, (unsigned long long)held);
    runtime_wait_trace_line(row);
}
#ifdef _WIN32
/* Threads waiting on a held mutex, by holder thread id, for the sampler. */
#define WAIT_SLOTS 64
static volatile LONG wait_claim[WAIT_SLOTS], wait_holder[WAIT_SLOTS];
/* The exe's image and the game's (eboot) range, for return addresses found on a holder's stack. */
static uintptr_t exe_lo, exe_hi;
static int return_target(uint64_t v) {
    const uintptr_t image = (uintptr_t)runtime_image_base();
    return (v >= exe_lo && v < exe_hi) || (image && v - image < 0x20000000);
}
/* A return address: in the exe or the game, directly after a call instruction (E8 rel32, FF /2). */
static int return_after_call(uint64_t v) {
    if (!return_target(v)) return 0;
    unsigned char b[7]; SIZE_T n = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)(v - 7), b, sizeof(b), &n) || n != sizeof(b)) return 0;
    return b[2] == 0xE8 ||                              /* call rel32 */
           (b[5] == 0xFF && (b[6] & 0x38) == 0x10) ||   /* call reg */
           (b[4] == 0xFF && (b[5] & 0x38) == 0x10) ||   /* call [reg+d8] */
           (b[1] == 0xFF && (b[2] & 0x38) == 0x10) ||   /* call [rip+d32] / [reg+d32] */
           (b[3] == 0xFF && (b[4] & 0x38) == 0x10);     /* call [reg+index+d8] */
}
/* GOW3_SAMPLE_THREADS=<name>[,<name>...] with GOW3_WAIT_TRACE (dev build): host threads named that way
 * (SetThreadDescription, e.g. shadPS4:GpuCommandProcessor = stage A, gow3:DrawRec = stage B) are sampled on every
 * tick, not only while someone waits on them. A scanner thread of its own looks for them every ~2 s until each name
 * is found (a Toolhelp list of every thread on the system takes tens of ms: in the sampler's loop it left gaps);
 * each one found writes t,<tid>,<name>. */
#define PINNED 16
static DWORD pinned[PINNED];
static int pinned_count;
static const char *sample_threads(void) {
    static const char *list; static int read;
    if (!read) { const char *value = getenv("GOW3_SAMPLE_THREADS"); list = value && value[0] ? value : NULL; read = 1; }
    return list;
}
static int name_listed(const char *list, const char *name) {
    const size_t n = strlen(name);
    for (const char *p = list;;) {
        const char *end = strchr(p, ',');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && !strncmp(p, name, n)) return 1;
        if (!end) return 0;
        p = end + 1;
    }
}
static void find_named_threads(void) {
    const char *list = sample_threads();
    typedef HRESULT (WINAPI *GetDescription)(HANDLE, PWSTR *);
    static GetDescription get_description;
    if (!get_description)
        get_description = (GetDescription)(void (*)(void))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!list || !get_description || pinned_count == PINNED) return;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = { .dwSize = sizeof(entry) };
    const DWORD pid = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more && pinned_count < PINNED; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) continue;
        int known = 0;
        for (int i = 0; i < pinned_count && !known; ++i) known = pinned[i] == entry.th32ThreadID;
        if (known) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        PWSTR wide = NULL;
        char name[64] = "";
        if (SUCCEEDED(get_description(thread, &wide)) && wide) {
            WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, sizeof(name), NULL, NULL);
            LocalFree(wide);
        }
        CloseHandle(thread);
        if (!name[0] || !name_listed(list, name)) continue;
        pinned[pinned_count] = entry.th32ThreadID;
        __atomic_store_n(&pinned_count, pinned_count + 1, __ATOMIC_RELEASE); /* the sampler reads it */
        char row[120];
        for (char *c = name; *c; ++c) if (*c == ',' || *c == '\n') *c = ';';
        snprintf(row, sizeof(row), "t,%lu,%s\n", (unsigned long)entry.th32ThreadID, name);
        runtime_wait_trace_line(row);
    }
    CloseHandle(snapshot);
}
static DWORD WINAPI name_scanner(LPVOID unused) {
    (void)unused;
    const char *list = sample_threads();
    int names = 1;
    for (const char *c = list; *c; ++c) names += *c == ',';
    while (pinned_count < names && pinned_count < PINNED) {
        find_named_threads();
        Sleep(2000);
    }
    return 0;
}
static DWORD WINAPI holder_sampler(LPVOID unused) {
    (void)unused;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    struct { DWORD tid; HANDLE handle; } cache[128];
    int cached = 0;
    const DWORD self = GetCurrentThreadId();
    for (;;) {
        LARGE_INTEGER due = { .QuadPart = -2000 }; /* 200 us, relative */
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep(1);
        DWORD holders[WAIT_SLOTS + PINNED];
        int count = 0;
        const int pins = __atomic_load_n(&pinned_count, __ATOMIC_ACQUIRE);
        for (int i = 0; i < WAIT_SLOTS + pins; ++i) {
            const DWORD tid = i < WAIT_SLOTS ? (DWORD)wait_holder[i] : pinned[i - WAIT_SLOTS];
            int seen = !tid || tid == self;
            for (int j = 0; j < count && !seen; ++j) seen = holders[j] == tid;
            if (!seen) holders[count++] = tid;
        }
        for (int i = 0; i < count; ++i) {
            HANDLE handle = NULL;
            for (int j = 0; j < cached; ++j) if (cache[j].tid == holders[i]) handle = cache[j].handle;
            if (!handle) {
                handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, holders[i]);
                if (!handle) continue;
                if (cached == 128) { for (int j = 0; j < cached; ++j) CloseHandle(cache[j].handle); cached = 0; }
                cache[cached].tid = holders[i]; cache[cached].handle = handle; ++cached;
            }
            /* Suspended: the registers and a copy of the top of the stack, through syscalls only (no lock of ours or
             * the loader's: the holder might hold it). Resumed: the copy is read. */
            /* 16 KB copied, then zeros: the unwinder below may read past what was copied. */
            static uint64_t stack[2048 + 8192];
            SIZE_T got = 0;
            CONTEXT context;
            memset(&context, 0, sizeof(context));
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (SuspendThread(handle) == (DWORD)-1) continue;
            const BOOL have = GetThreadContext(handle, &context);
            MEMORY_BASIC_INFORMATION region;
            if (have && VirtualQuery((LPCVOID)(uintptr_t)context.Rsp, &region, sizeof(region))) {
                const uint64_t left = (uint64_t)(uintptr_t)region.BaseAddress + region.RegionSize - context.Rsp;
                const SIZE_T size = left < 16384 ? (SIZE_T)left : 16384; /* the stack's committed region ends there */
                if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)context.Rsp, stack, size, &got)) got = 0;
            }
            ResumeThread(handle);
            if (!have) continue;
            memset((char *)stack + got, 0, sizeof(stack) - got);
            /* pc, then the callers: Windows' unwinder (RtlVirtualUnwind; the exe's and the DLLs' unwind tables) on the
             * copy, with rsp / rbp moved into it, so it reads the copy, not the live stack; game code has no unwind
             * tables: its rbp chain, each return address after a call. If that gives fewer than 3 frames: return
             * addresses found by scanning the copy, marked ~ (they can be stale, left by earlier calls). */
            uint64_t pcs[12] = { context.Rip };
            int frames = 1, scanned = 12;
            const uint64_t base = context.Rsp, copy = (uint64_t)(uintptr_t)stack, delta = copy - base;
#define TO_COPY(x) ((x) >= base && (x) < base + got ? (x) + delta : (x))
            CONTEXT c = context;
            c.Rsp = copy; c.Rbp = TO_COPY(c.Rbp);
            while (frames < 12 && got) {
                DWORD64 image = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(c.Rip, &image, NULL);
                const uintptr_t game = (uintptr_t)runtime_image_base();
                if (entry) {
                    PVOID handler; DWORD64 frame;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, entry, &c, &handler, &frame, NULL);
                    c.Rbp = TO_COPY(c.Rbp);
                } else if (game && c.Rip - game < 0x20000000) {      /* game code: rbp chain */
                    if (c.Rbp < copy || c.Rbp + 16 > copy + got || (c.Rbp & 7)) break;
                    const uint64_t *f = (const uint64_t *)(uintptr_t)c.Rbp;
                    if (!return_after_call(f[1])) break;
                    c.Rip = f[1]; c.Rsp = c.Rbp + 16; c.Rbp = TO_COPY(f[0]);
                } else {                                               /* a leaf without unwind data */
                    if (c.Rsp + 8 > copy + got) break;
                    c.Rip = *(const uint64_t *)(uintptr_t)c.Rsp; c.Rsp += 8;
                }
                if (!c.Rip || c.Rsp < copy || c.Rsp >= copy + got) break;
                pcs[frames++] = c.Rip;
            }
#undef TO_COPY
            if (frames < 3) {
                scanned = frames;
                for (SIZE_T k = 0; k < got / 8 && frames < 12; ++k)
                    if (stack[k] != pcs[frames - 1] && return_after_call(stack[k])) pcs[frames++] = stack[k];
            }
            char row[1200];
            int at = snprintf(row, sizeof(row), "s,%llu,%lu", (unsigned long long)runtime_wait_now(), (unsigned long)holders[i]);
            for (int f = 0; f < frames; ++f) {
                char site[80];
                const uintptr_t pc = (uintptr_t)pcs[f];
                HMODULE module = NULL;
                char path[MAX_PATH];
                if (pc >= exe_lo && pc < exe_hi) snprintf(site, sizeof(site), "exe+0x%llx", (unsigned long long)(pc - exe_lo));
                else if (return_target(pc)) runtime_wait_format_site(pc, site, sizeof(site));
                else if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                            (LPCSTR)pc, &module) && GetModuleFileNameA(module, path, sizeof(path))) {
                    const char *name = strrchr(path, '\\');
                    snprintf(site, sizeof(site), "%s+0x%llx", name ? name + 1 : path, (unsigned long long)(pc - (uintptr_t)module));
                } else snprintf(site, sizeof(site), "0x%llx", (unsigned long long)pc);
                for (char *c = site; *c; ++c) if (*c == ',') *c = ';';
                at += snprintf(row + at, sizeof(row) - (size_t)at, ",%s%s", f >= scanned ? "~" : "", site);
            }
            snprintf(row + at, sizeof(row) - (size_t)at, "\n");
            runtime_wait_trace_line(row);
        }
    }
    return 0;
}
static INIT_ONCE sampler_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK sampler_start(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    const unsigned char *exe = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    exe_lo = (uintptr_t)exe; exe_hi = exe_lo + nt->OptionalHeader.SizeOfImage;
    HANDLE thread = CreateThread(NULL, 0, holder_sampler, NULL, 0, NULL);
    if (thread) CloseHandle(thread);
    if (sample_threads() && (thread = CreateThread(NULL, 0, name_scanner, NULL, 0, NULL))) {
        SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(thread);
    }
    return TRUE;
}
static int wait_register(unsigned long holder) {
    InitOnceExecuteOnce(&sampler_once, sampler_start, NULL, NULL);
    for (int i = 0; i < WAIT_SLOTS; ++i)
        if (!InterlockedCompareExchange(&wait_claim[i], 1, 0)) { InterlockedExchange(&wait_holder[i], (LONG)holder); return i; }
    return -1;
}
static void wait_unregister(int slot) {
    if (slot < 0) return;
    InterlockedExchange(&wait_holder[slot], 0);
    InterlockedExchange(&wait_claim[slot], 0);
}
/* GOW3_SAMPLE_THREADS: starts the sampler (the wait trace calls this on its waits; one load once started). */
void runtime_thread_sampler_start(void) {
    if (sample_threads()) InitOnceExecuteOnce(&sampler_once, sampler_start, NULL, NULL);
}
#else
static int wait_register(unsigned long holder) { (void)holder; return -1; }
static void wait_unregister(int slot) { (void)slot; }
void runtime_thread_sampler_start(void) {}
#endif
/* A lock with the holder recorded: trylock first; when that fails, note the holder, wait, and write an m row if the
 * wait was long. */
static int traced_lock(GuestMutex *m, uintptr_t site) {
    if (!pthread_mutex_trylock(&m->native)) { holder_taken(m, site); return 0; }
    const unsigned long holder = __atomic_load_n(&m->owner_tid, __ATOMIC_ACQUIRE);
    const char *holder_name = __atomic_load_n(&m->owner_name, __ATOMIC_RELAXED);
    const uintptr_t holder_lock = __atomic_load_n(&m->owner_site, __ATOMIC_RELAXED);
    const uint64_t since = __atomic_load_n(&m->owner_since, __ATOMIC_RELAXED), t0 = runtime_wait_now();
    const int slot = holder ? wait_register(holder) : -1;
    const int e = pthread_mutex_lock(&m->native);
    wait_unregister(slot);
    const uint64_t t1 = runtime_wait_now();
    if (!e) holder_taken(m, site);
    if (t1 - t0 >= runtime_wait_trace_min_ns()) {
        char me[40], mine[40], them[40], theirs[40], row[300];
        runtime_wait_thread_name(runtime_thread_current_name(), me, sizeof(me));
        holder_site(site, mine, sizeof(mine));
        runtime_wait_thread_name(holder ? holder_name : NULL, them, sizeof(them));
        holder_site(holder ? holder_lock : 0, theirs, sizeof(theirs));
        snprintf(row, sizeof(row), "m,%llu,%lu,%s,%s,%lu,%s,%s,%llu,%llu\n", (unsigned long long)t1, runtime_wait_thread_id(),
                 me, mine, holder, them, theirs, (unsigned long long)(holder && t0 > since ? t0 - since : 0),
                 (unsigned long long)(t1 - t0));
        runtime_wait_trace_line(row);
    }
    return e;
}
#define HOLDERS_ON() runtime_mutex_holders_enabled()
#else
#define HOLDERS_ON() 0
static void holder_taken(GuestMutex *m, uintptr_t site) { (void)m; (void)site; }
static void holder_releasing(GuestMutex *m, uintptr_t site) { (void)m; (void)site; }
static int traced_lock(GuestMutex *m, uintptr_t site) { (void)site; return pthread_mutex_lock(&m->native); }
void runtime_thread_sampler_start(void) {}
#endif

static int32_t mutex_lock_impl(GuestMutex **mutex, uintptr_t site) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(HOLDERS_ON() ? traced_lock(*mutex, site) : pthread_mutex_lock(&(*mutex)->native));
    if (!e) COUNT(locks);
    return e;
}
/* GOW3_WAIT_SITES: each export the guest calls measures, so its return address is the call site. */
static ABI int32_t mutex_lock(GuestMutex **mutex) {
    RUNTIME_WAIT_BEGIN();
    const int32_t e = mutex_lock_impl(mutex, (uintptr_t)__builtin_return_address(0));
    RUNTIME_WAIT_END(9, 0);
    return e;
}
static int32_t mutex_trylock_impl(GuestMutex **mutex, uintptr_t site) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    e = orbis_error(pthread_mutex_trylock(&(*mutex)->native));
    if (!e) { COUNT(locks); if (HOLDERS_ON()) holder_taken(*mutex, site); }
    return e;
}
static ABI int32_t mutex_trylock(GuestMutex **mutex) { return mutex_trylock_impl(mutex, (uintptr_t)__builtin_return_address(0)); }
static int32_t mutex_unlock_impl(GuestMutex **mutex, uintptr_t site) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return orbis_error(EPERM);
    if (HOLDERS_ON()) holder_releasing(*mutex, site);
    int32_t e = orbis_error(pthread_mutex_unlock(&(*mutex)->native));
    if (!e) COUNT(unlocks);
    return e;
}
static ABI int32_t mutex_unlock(GuestMutex **mutex) { return mutex_unlock_impl(mutex, (uintptr_t)__builtin_return_address(0)); }
/* Destroyed mutexes are retired through a ring and only freed after many later destroys: a game
 * may destroy a mutex while another thread is still leaving an unlock or a condition wait on it
 * (the PS4 kernel tolerates it; freeing at once crashed inside winpthread with NpToolkit2). */
#define RETIRED 8192
static GuestMutex *retired[RETIRED];
static size_t retired_next;
static pthread_mutex_t retired_lock = PTHREAD_MUTEX_INITIALIZER;
static ABI int32_t mutex_destroy(GuestMutex **mutex) {
    if (!mutex || (uintptr_t)*mutex == 2) return orbis_error(EINVAL);
    if ((uintptr_t)*mutex < 2) return 0;
    GuestMutex *dead = *mutex;
    if (pthread_mutex_trylock(&dead->native)) return orbis_error(EBUSY); /* still held */
    pthread_mutex_unlock(&dead->native);
    *mutex = (GuestMutex *)(uintptr_t)2;
    pthread_mutex_lock(&retired_lock);
    GuestMutex *oldest = retired[retired_next];
    retired[retired_next] = dead;
    retired_next = (retired_next + 1) % RETIRED;
    pthread_mutex_unlock(&retired_lock);
    if (oldest) {
        pthread_mutex_destroy(&oldest->native);
        free(oldest);
    }
    return 0;
}
static int deadline_after(struct timespec *end, uint64_t usec) {
    if (clock_gettime(CLOCK_REALTIME, end)) return errno;
    uint64_t ns = (uint64_t)end->tv_nsec + (usec % 1000000) * 1000;
    end->tv_sec += (time_t)(usec / 1000000 + ns / 1000000000);
    end->tv_nsec = (long)(ns % 1000000000);
    return 0;
}
static int32_t timed_error(int e) { return e == ETIMEDOUT ? (int32_t)UINT32_C(0x8002003c) : orbis_error(e); }
static ABI int32_t mutex_timedlock(GuestMutex **mutex, uint32_t usec) {
    int32_t e = ensure_mutex(mutex);
    if (e) return e;
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    e = timed_error(pthread_mutex_timedlock(&(*mutex)->native, &end));
    if (!e) { COUNT(locks); if (HOLDERS_ON()) holder_taken(*mutex, (uintptr_t)__builtin_return_address(0)); }
    return e;
}
/* Condition variables: opaque handles over host pthread_cond (CLOCK_REALTIME,
 * like FreeBSD's default), sharing the host mutex behind GuestMutex. */
/* seq: the WaitOnAddress word of the futex condition variables (futex_cond_wait). */
typedef struct { pthread_cond_t native; volatile int seq; } GuestCond;
static size_t conds, waits, wakeups;
static ABI int32_t cond_init(GuestCond **out, void **attr, const char *name) {
    (void)attr; (void)name;
    if (!out) return orbis_error(EINVAL);
    GuestCond *c = malloc(sizeof(*c));
    if (!c) return orbis_error(ENOMEM);
    int e = pthread_cond_init(&c->native, NULL);
    if (e) { free(c); return orbis_error(e); }
    c->seq = 0;
    *out = c; ++conds; return 0;
}
static int32_t ensure_cond(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if (__atomic_load_n((uintptr_t *)cond, __ATOMIC_ACQUIRE) >= 2) return 0;
    pthread_mutex_lock(&static_init);
    int32_t e = 0;
    if ((uintptr_t)*cond < 2) {
        GuestCond *c = NULL;
        e = cond_init(&c, NULL, NULL);
        if (!e) __atomic_store_n(cond, c, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&static_init);
    return e;
}
static ABI int32_t cond_destroy(GuestCond **cond) {
    if (!cond) return orbis_error(EINVAL);
    if ((uintptr_t)*cond < 2) return 0;
    /* Retired like mutexes: a woken waiter may still be returning through it. */
    static GuestCond *retired_conds[RETIRED];
    static size_t next;
    GuestCond *dead = *cond;
    *cond = NULL;
    pthread_mutex_lock(&retired_lock);
    GuestCond *oldest = retired_conds[next];
    retired_conds[next] = dead;
    next = (next + 1) % RETIRED;
    pthread_mutex_unlock(&retired_lock);
    if (oldest) {
        pthread_cond_destroy(&oldest->native);
        free(oldest);
    }
    return 0;
}
/* From the Uncharted port (29 Oct 2026, default on there after A/B of 3 games x 3 CPU configs:
 * 1% lows up to +31%): guest condition variables as a sequence word waited on with WaitOnAddress
 * (wake = increment + WakeByAddress) instead of winpthreads' semaphore-based pthread_cond, which
 * is slow for the many signals and broadcasts of a job system. GOW3_FUTEX_COND=0: winpthreads. */
#ifdef _WIN32
static int futex_cond_enabled(void) {
    static int value = -1;
    if (value < 0) { const char *e = getenv("GOW3_FUTEX_COND"); value = !(e && e[0] == '0'); }
    return value;
}
/* Timed waits to their deadline (2026-11-15). WaitOnAddress takes whole ms: a wait was rounded up to the next
 * ms and then to the 1 ms timer tick. UE4's FPThreadEvent::Wait(1) asks for 0-1 ms (its start time cut to whole
 * ms); the game thread polls the render thread with it (guest+0x70df3a, ~450 waits a second, 91% expire, and
 * most frames go on from an expired wait, not a signal): it waited 1.01 ms (median) for 0.50 ms asked.
 *   GOW3_COND_PRECISE=0: the old whole-ms waits. Default 1: ntdll's RtlWaitOnAddress (100 ns timeout units) and
 *     a 0.5 ms timer resolution (NtSetTimerResolution), so a wait ends within ~0.5 ms of its deadline; an early
 *     timeout waits again instead of returning a spurious wakeup.
 *   GOW3_COND_SPIN_US=N (default 0): timed waits with N us or less left spin to the deadline on the sequence
 *     word instead of sleeping (exact, but the waiting thread keeps its core busy). */
typedef LONG (NTAPI *RtlWaitOnAddressFn)(volatile VOID *, PVOID, SIZE_T, PLARGE_INTEGER);
typedef LONG (NTAPI *NtSetTimerResolutionFn)(ULONG, BOOLEAN, PULONG);
static RtlWaitOnAddressFn rtl_wait_on_address;
static int64_t cond_spin_ns, qpc_per_s;
static INIT_ONCE precise_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK precise_init(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    const char *e = getenv("GOW3_COND_PRECISE");
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!(e && e[0] == '0') && ntdll) {
        rtl_wait_on_address = (RtlWaitOnAddressFn)(void (*)(void))GetProcAddress(ntdll, "RtlWaitOnAddress");
        NtSetTimerResolutionFn set = (NtSetTimerResolutionFn)(void (*)(void))GetProcAddress(ntdll, "NtSetTimerResolution");
        ULONG now = 0;
        LONG status = set ? set(5000, TRUE, &now) : -1;
        const char *spin = getenv("GOW3_COND_SPIN_US");
        cond_spin_ns = spin ? (int64_t)strtoll(spin, NULL, 10) * 1000 : 0;
        if (cond_spin_ns < 0) cond_spin_ns = 0;
        LARGE_INTEGER f; QueryPerformanceFrequency(&f); qpc_per_s = f.QuadPart;
        printf("Host scheduling: condition variable timeouts to 100 ns%s, timer resolution %.2f ms%s, spin the last %lld us"
               " (GOW3_COND_PRECISE=0: whole ms)\n", rtl_wait_on_address ? "" : " - RtlWaitOnAddress MISSING",
               now / 10000.0, status ? " - NtSetTimerResolution REFUSED" : "", (long long)(cond_spin_ns / 1000));
    }
    return TRUE;
}
static int cond_precise(void) {
    InitOnceExecuteOnce(&precise_once, precise_init, NULL, NULL);
    return rtl_wait_on_address != NULL;
}
/* Until the sequence word moves (1) or left_ns pass (0), without sleeping. */
static int spin_wait(volatile int *seq, int seen, int64_t left_ns) {
    LARGE_INTEGER t0, t;
    QueryPerformanceCounter(&t0);
    const int64_t ticks = left_ns * qpc_per_s / 1000000000;
    for (unsigned i = 1;; ++i) {
        if (__atomic_load_n(seq, __ATOMIC_ACQUIRE) != seen) return 1;
        YieldProcessor();
        if (!(i & 15)) {
            QueryPerformanceCounter(&t);
            if (t.QuadPart - t0.QuadPart >= ticks) return 0;
        }
    }
}
static int64_t ns_until(const struct timespec *end) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return ((int64_t)end->tv_sec - (int64_t)now.tv_sec) * 1000000000LL + (end->tv_nsec - now.tv_nsec);
}
/* 0 or ETIMEDOUT; the mutex is held again on return. end = CLOCK_REALTIME deadline or NULL. */
static int futex_cond_wait(GuestCond *c, GuestMutex *m, const struct timespec *end) {
    int seen = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);
    DWORD ms = INFINITE;
    if (end) {
        int64_t left = ns_until(end);
        if (left <= 0) return ETIMEDOUT;
        if (cond_precise()) {
            pthread_mutex_unlock(&m->native);
            int woke;
            for (;;) {
                if (left <= cond_spin_ns) { woke = spin_wait(&c->seq, seen, left); break; }
                LARGE_INTEGER timeout = { .QuadPart = -((left + 99) / 100) }; /* relative, 100 ns units */
                woke = (DWORD)rtl_wait_on_address(&c->seq, &seen, sizeof(seen), &timeout) != STATUS_TIMEOUT;
                if (woke || (left = ns_until(end)) <= 0) break;
            }
            pthread_mutex_lock(&m->native);
            return woke ? 0 : ETIMEDOUT;
        }
        ms = (DWORD)((left + 999999) / 1000000);
    }
    pthread_mutex_unlock(&m->native);
    BOOL woke = WaitOnAddress((volatile VOID *)&c->seq, &seen, sizeof(seen), ms);
    DWORD error = woke ? 0 : GetLastError();
    pthread_mutex_lock(&m->native);
    if (!woke && error == ERROR_TIMEOUT && end) {
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > end->tv_sec || (now.tv_sec == end->tv_sec && now.tv_nsec >= end->tv_nsec)) return ETIMEDOUT;
    }
    return 0; /* woken, or early: a spurious wakeup */
}
static void futex_cond_wake(GuestCond *c, int all) {
    __atomic_add_fetch(&c->seq, 1, __ATOMIC_SEQ_CST);
    if (all) WakeByAddressAll((PVOID)&c->seq); else WakeByAddressSingle((PVOID)&c->seq);
}
#else
static int futex_cond_enabled(void) { return 0; }
static int futex_cond_wait(GuestCond *c, GuestMutex *m, const struct timespec *end) { (void)c; (void)m; (void)end; return EINVAL; }
static void futex_cond_wake(GuestCond *c, int all) { (void)c; (void)all; }
#endif
/* GOW3_MUTEX_HOLDERS around a condition wait: the hold ends before it (whatever the recursion depth) and starts
 * again after it. */
static unsigned cond_release(GuestMutex *m) {
#if GOW3_DEV_TOOLS
    const unsigned depth = m->depth;
    if (depth) { m->depth = 1; holder_releasing(m, COND_SITE); }
    return depth;
#else
    (void)m; return 0;
#endif
}
static void cond_retake(GuestMutex *m, unsigned depth) {
#if GOW3_DEV_TOOLS
    if (!depth) return;
    holder_taken(m, COND_SITE);
    m->depth = depth;
#else
    (void)m; (void)depth;
#endif
}
static int32_t cond_wait_impl(GuestCond **cond, GuestMutex **mutex) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3) return orbis_error(EINVAL);
    COUNT(waits);
    /* GOW3_MUTEX_HOLDERS: the wait releases the mutex and takes it again (site cond_wait). */
    const int holders = HOLDERS_ON();
    const unsigned depth = holders ? cond_release(*mutex) : 0;
    const int r = futex_cond_enabled() ? futex_cond_wait(*cond, *mutex, NULL)
                                       : pthread_cond_wait(&(*cond)->native, &(*mutex)->native);
    if (holders) cond_retake(*mutex, depth);
    return orbis_error(r);
}
static ABI int32_t cond_wait(GuestCond **cond, GuestMutex **mutex) {
    RUNTIME_WAIT_BEGIN();
    const int32_t e = cond_wait_impl(cond, mutex);
    RUNTIME_WAIT_END(5, 0);
    return e;
}
static int32_t cond_wait_until(GuestCond **cond, GuestMutex **mutex, const struct timespec *end) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    if (!mutex || (uintptr_t)*mutex < 3 || !end) return orbis_error(EINVAL);
    COUNT(waits);
    const int holders = HOLDERS_ON();
    const unsigned depth = holders ? cond_release(*mutex) : 0;
    const int r = futex_cond_enabled() ? futex_cond_wait(*cond, *mutex, end)
                                       : pthread_cond_timedwait(&(*cond)->native, &(*mutex)->native, end);
    if (holders) cond_retake(*mutex, depth);
    return timed_error(r);
}
static ABI int32_t cond_timedwait(GuestCond **cond, GuestMutex **mutex, uint32_t usec) {
    struct timespec end;
    if (deadline_after(&end, usec)) return orbis_error(EINVAL);
    RUNTIME_WAIT_BEGIN();
    const int32_t e = cond_wait_until(cond, mutex, &end);
    RUNTIME_WAIT_END(6, (uint64_t)usec * 1000);
    return e;
}
static ABI int32_t cond_signal(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    COUNT(wakeups);
    if (futex_cond_enabled()) { futex_cond_wake(*cond, 0); return 0; }
    return orbis_error(pthread_cond_signal(&(*cond)->native));
}
static ABI int32_t cond_broadcast(GuestCond **cond) {
    int32_t e = ensure_cond(cond);
    if (e) return e;
    COUNT(wakeups);
    if (futex_cond_enabled()) { futex_cond_wake(*cond, 1); return 0; }
    return orbis_error(pthread_cond_broadcast(&(*cond)->native));
}
/* PS4 struct timespec is {int64 sec, int64 nsec}, identical to Linux x86-64. */
typedef struct { int64_t sec, nsec; } GuestTimespec;
static int32_t cond_abs_timedwait_impl(GuestCond **cond, GuestMutex **mutex, const GuestTimespec *abs) {
    if (!abs || abs->nsec < 0 || abs->nsec >= 1000000000) return orbis_error(EINVAL);
    struct timespec end = {.tv_sec = (time_t)abs->sec, .tv_nsec = (long)abs->nsec};
    return cond_wait_until(cond, mutex, &end);
}
/* The time left until an absolute deadline (the clock pthread_cond_timedwait uses), for the report. */
static uint64_t until_deadline_ns(const GuestTimespec *abs) {
    if (!abs || !runtime_wait_sites_enabled()) return 0;
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    const int64_t left = (abs->sec - (int64_t)now.tv_sec) * 1000000000 + (abs->nsec - now.tv_nsec);
    return left > 0 ? (uint64_t)left : 0;
}
/* POSIX pthread APIs return positive guest errno values, unlike scePthread. */
static int32_t posix_result(int32_t result) { return result ? (int32_t)((uint32_t)result & 0xffff) : 0; }
static ABI int32_t posix_attr_init(GuestAttr **out) { return posix_result(attr_init(out)); }
static ABI int32_t posix_attr_type(GuestAttr **attr,int type) { return posix_result(attr_type(attr,type)); }
static ABI int32_t posix_attr_destroy(GuestAttr **attr) { return posix_result(attr_destroy(attr)); }
static ABI int32_t posix_mutex_init(GuestMutex **out,GuestAttr **attr) { return posix_result(mutex_init(out,attr,NULL)); }
static ABI int32_t posix_mutex_destroy(GuestMutex **m) { return posix_result(mutex_destroy(m)); }
static ABI int32_t posix_mutex_lock(GuestMutex **m) {
    RUNTIME_WAIT_BEGIN();
    const int32_t e = mutex_lock_impl(m, (uintptr_t)__builtin_return_address(0));
    RUNTIME_WAIT_END(9, 0);
    return posix_result(e);
}
static ABI int32_t posix_mutex_trylock(GuestMutex **m) { return posix_result(mutex_trylock_impl(m, (uintptr_t)__builtin_return_address(0))); }
static ABI int32_t posix_mutex_unlock(GuestMutex **m) { return posix_result(mutex_unlock_impl(m, (uintptr_t)__builtin_return_address(0))); }
static ABI int32_t posix_cond_init(GuestCond **c, void **a) { return posix_result(cond_init(c, a, NULL)); }
static ABI int32_t posix_cond_destroy(GuestCond **c) { return posix_result(cond_destroy(c)); }
static ABI int32_t posix_cond_wait(GuestCond **c, GuestMutex **m) {
    RUNTIME_WAIT_BEGIN();
    const int32_t e = cond_wait_impl(c, m);
    RUNTIME_WAIT_END(5, 0);
    return posix_result(e);
}
static ABI int32_t posix_cond_timedwait(GuestCond **c, GuestMutex **m, const GuestTimespec *t) {
    const uint64_t requested = until_deadline_ns(t);
    RUNTIME_WAIT_BEGIN();
    const int32_t e = cond_abs_timedwait_impl(c, m, t);
    RUNTIME_WAIT_END(6, requested);
    return posix_result(e);
}
static ABI int32_t posix_cond_signal(GuestCond **c) { return posix_result(cond_signal(c)); }
static ABI int32_t posix_cond_broadcast(GuestCond **c) { return posix_result(cond_broadcast(c)); }
static ABI int32_t condattr_init(void **attr) {
    if (!attr) return (int32_t)0x80020016; /* EINVAL */
    *attr=calloc(1,16);
    return *attr ? 0 : (int32_t)0x8002000C; /* ENOMEM */
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr || !*attr) return (int32_t)0x80020016;
    free(*attr); *attr=NULL; return 0;
}
uintptr_t runtime_mutex_resolve(const char *name) {
    if (!strcmp(name,"m5-2bsNfv7s#libkernel")) return (uintptr_t)condattr_init;
    if (!strcmp(name,"waPcxYiR3WA#libkernel")) return (uintptr_t)condattr_destroy;
    if (!strcmp(name,"0TyVk4MSLt0#libkernel")) return (uintptr_t)posix_cond_init;
    if (!strcmp(name,"RXXqi4CtF8w#libkernel")) return (uintptr_t)posix_cond_destroy;
    if (!strcmp(name,"Op8TBGY5KHg#libkernel")) return (uintptr_t)posix_cond_wait;
    if (!strcmp(name,"27bAgiJmOh0#libkernel")) return (uintptr_t)posix_cond_timedwait;
    if (!strcmp(name,"2MOy+rUfuhQ#libkernel")) return (uintptr_t)posix_cond_signal;
    if (!strcmp(name,"mkx2fVhNMsg#libkernel")) return (uintptr_t)posix_cond_broadcast;
    if (!strcmp(name,"2Tb92quprl0#libkernel")) return (uintptr_t)cond_init;
    if (!strcmp(name,"g+PZd2hiacg#libkernel")) return (uintptr_t)cond_destroy;
    if (!strcmp(name,"WKAXJ4XBPQ4#libkernel")) return (uintptr_t)cond_wait;
    if (!strcmp(name,"BmMjYxmew1w#libkernel")) return (uintptr_t)cond_timedwait;
    if (!strcmp(name,"kDh-NfxgMtE#libkernel")) return (uintptr_t)cond_signal;
    if (!strcmp(name,"JGgj7Uvrl+A#libkernel")) return (uintptr_t)cond_broadcast;
    if (!strcmp(name,"IafI2PxcPnQ#libkernel")) return (uintptr_t)mutex_timedlock;
    if (!strcmp(name,"dQHWEsJtoE4#libkernel")) return (uintptr_t)posix_attr_init;
    if (!strcmp(name,"mDmgMOGVUqg#libkernel")) return (uintptr_t)posix_attr_type;
    if (!strcmp(name,"HF7lK46xzjY#libkernel")) return (uintptr_t)posix_attr_destroy;
    if (!strcmp(name,"ttHNfU+qDBU#libkernel")) return (uintptr_t)posix_mutex_init;
    if (!strcmp(name,"ltCfaGr2JGE#libkernel")) return (uintptr_t)posix_mutex_destroy;
    if (!strcmp(name,"7H0iTOciTLo#libkernel")) return (uintptr_t)posix_mutex_lock;
    if (!strcmp(name,"K-jXhbt2gn4#libkernel")) return (uintptr_t)posix_mutex_trylock;
    if (!strcmp(name,"2Z+PpY6CaJg#libkernel")) return (uintptr_t)posix_mutex_unlock;
    if (!strcmp(name, "F8bUHwAG284#libkernel")) return (uintptr_t)attr_init;
    if (!strcmp(name, "iMp8QpE+XO4#libkernel")) return (uintptr_t)attr_type;
    if (!strcmp(name, "smWEktiyyG0#libkernel")) return (uintptr_t)attr_destroy;
    if (!strcmp(name, "1FGvU0i9saQ#libkernel")) return (uintptr_t)attr_protocol;
    if (!strcmp(name, "cmo1RIYva9o#libkernel")) return (uintptr_t)mutex_init;
    if (!strcmp(name, "9UK1vLZQft4#libkernel")) return (uintptr_t)mutex_lock;
    if (!strcmp(name, "upoVrzMHFeE#libkernel")) return (uintptr_t)mutex_trylock;
    if (!strcmp(name, "tn3VlD0hG60#libkernel")) return (uintptr_t)mutex_unlock;
    if (!strcmp(name, "2Of0f+3mhhE#libkernel")) return (uintptr_t)mutex_destroy;
    return 0;
}
void runtime_mutex_report(void) {
    if (!runtime_fault_stats_enabled()) {
        printf("Runtime: mutexes created=%zu, condition variables created=%zu\n", created, conds);
        return;
    }
    printf("Runtime: mutexes created=%zu, locks=%zu, unlocks=%zu\n", created, locks, unlocks);
    printf("Runtime: condition variables created=%zu, waits=%zu, wakeups=%zu\n", conds, waits, wakeups);
}

