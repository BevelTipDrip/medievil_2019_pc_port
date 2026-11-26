# Study: S1, the start-up crash in `pthread_mutex_lock`

Status: study, 2026-11-25. Read only: nothing built, launched, edited (except this page) or committed. Exe read:
`out/medievil-runtime.exe` (d59f6c1, v2.5.1, 17:26:55, 136,137,728 bytes); `libwinpthread-1.dll` read from
`C:\msys64\clang64\bin` (the copy the exe loads, 73,216 bytes, 2026-10-17). Parent: [plan-baseline-issues.md](plan-baseline-issues.md)
S1, [plan-work-queue.md](plan-work-queue.md) row S1. Intermediate output and scripts: `research-data/s1-20261125/`
(D: junction): `d6-*.out` / `d8-*.out` (cdb on every complete dump), `d1123*.out` (11-23 dump), `ctx.py` (fault context
from a dump without a stream directory), `plt.py` / `s1dis.py` (eboot imports and disassembly).

Words: **found** = read in code or data, **confirmed** = direct evidence in registers or dump memory. Nothing is fixed.

## Answer

1. **The bad value is the winpthread mutex object, not the guest handle and not our `GuestMutex`.** Chain: guest slot
   -> `GuestMutex *G` (8-byte calloc) -> `G->native` = pointer to winpthread's 24-byte mutex object ("impl":
   `{vtable, event HANDLE, state}`) -> `call *0x10(vtable)`. In every case with memory (10 dumps) `G` is intact, in an
   intact run of other `GuestMutex` blocks, and `G->native` still holds the impl address it got at creation. The
   **impl block had been freed and handed to another allocation** while the guest mutex was alive. **Confirmed.**
2. **Our mutex code does not free it.** The only `pthread_mutex_destroy` / impl free in the port is the retire ring in
   `mutex_destroy` (`src/runtime_mutex.c:443-460`), which frees only after 8192 later destroys; a whole boot creates
   about 2,264 mutexes (175204 report), so the ring cannot wrap at boot. winpthread's own lazy-init path frees only
   its private loser copy and is never used for guest mutexes (they are always initialised explicitly). **Found**
   (code + counts), high confidence.
3. **So something else frees (or hands out again) memory that belongs to a live mutex object**: a double free / stale
   free of a small block in the shared process heap, or heap metadata damage that makes the heap hand out a live
   block. Guest mutex objects are the most numerous 16-24-byte host allocations at boot (about 2,264 impls + 1,806
   rwlocks in the same 0x20 LFH bucket) and the only ones reached through an indirect call, so they are where the
   damage shows. **Who frees is not found.** Candidates ranked in "Mechanism".
4. **Why it varies:** LFH picks a free slot at random and the threads race at boot, so whether the stale free lands on
   a live mutex object (and on one that is locked again later) changes per run. The underlying bad free may well happen
   on most boots. Today's cluster (3 in 12 v2.5.1 launches) would be ~1% for a fixed window at ~1 in 25, but it is a
   window picked after the fact; v2.5.1 changed only the launcher and the pipeline-binary calls (`bf990d9`), nothing on
   this path. extra-env.txt is not a factor (the 11-17..11-23 S1s had the four switches set, today's had none). **S1 was never absent**: 5 more
   happened between 11-15 and today (11-17 x2, 11-18, 11-19, 11-23; table below); 8 S1 in 200 run logs since 11-16
   (~1 in 25; the "27 clean launches" of the 11-16 note ended at 11-17 06:53).
5. **Today's three dumps are unusable** (no stream directory): the crash handler's first dump attempt (type 0x1a65,
   1.6-2.4 GB) never finished; the process ended meanwhile. Only the fault context could be recovered by hand.

## Evidence

### The fault (winpthread `pthread_mutex_lock`, read in the DLL)

```
+0x356b  mov (%rcx),%rcx        ; rcx = *pthread_mutex_t = impl pointer
+0x3573  cmp $-3,%rcx ; jae lazy_init   ; -1..-3 = static initializers (never ours)
+0x3579  test %rcx,%rcx ; je EINVAL
+0x357e  mov (%rcx),%rax        ; rax = impl->vtable
+0x3583  call *0x10(%rax)       ; vtable[2] = lock   <- S1
```

At the fault: `rsi` = the `pthread_mutex_t *` = our `GuestMutex *G`; `rcx` = impl; `rax` = what is now in the impl's
first qword. A valid impl's first qword is `libwinpthread-1.dll+0xb1a0` (errorcheck/normal) or `+0xb170` / `+0xb1d0` /
`+0xb200`; impls are `malloc(0x18)` (`pthread_mutex_init` -> vtable[0] at `+0x3f40`: `{vtable, CreateEventW handle,
0xffffffff00000000}`). "read ffffffffffffffff" is how Windows reports a non-canonical address (`rax+0x10` with garbage
`rax`), not winpthread's `-1` static initializer.

### Every S1 with data

| Run (build) | Fault | `rax` = impl's first qword at the fault | Impl block in the dump | Guest mutex (slot) |
|---|---|---|---|---|
| 11-08 01:12 (a75f7a0) | read 0x10 | 0 | dbghelp DIA object (allocated while the dump was written: the block was free) | NpToolkit2 static `0x5ff0f78`, via libc `+0x59d47` |
| 11-14 01:52 | exec 0 | `GuestCond->native` (cond_t, magic `0xc0bab1fd`) | live `GuestCond` {cond_t*, seq 1} | eboot BSS `0x5c68158` |
| 11-15 02:22 | read 0x10 | 0 | live 24-byte object `{0, &self, 0}` | eboot BSS `0x581cf98` |
| 11-15 08:41 | exec 0 | cond_t | `GuestCond` seq 1 | eboot BSS `0x581cf98` |
| 11-15 10:52 | exec 0 | cond_t | `GuestCond` seq 0 | NpToolkit2 static `0x5ff0f78` |
| 11-15 15:58 | exec 0 | cond_t | `GuestCond` seq 0 | eboot, return `0x26d5443` |
| 11-17 06:53 (v2.4.0) | exec 0 | cond_t `0x542ffa30` | `GuestCond` seq 12 | eboot BSS `0x5b145e0` |
| 11-17 08:06 (v2.4.0) | read `ffffffff00000010` | `0xffffffff00000000` | dump incomplete | libSceFios2 `+0x3c58e` |
| 11-18 19:57 (v2.4.0) | non-canonical | `0x706172476b736154` = ASCII **"TaskGrap"** | dump incomplete | ICU `umtx_lock` (`0x367a5de`) |
| 11-19 22:52 (v2.4.0) | exec 0 | cond_t | `GuestCond` seq 1 | eboot BSS `0x5b145e0` |
| 11-23 20:21 (v2.4.0) | read 0x10 | 0 | zeroed span + an nvwgf2umx.dll function table over live impl blocks | online task manager `0x1002fe00e8` (eboot `0x9f9af`) |
| 11-25 17:48 (d59f6c1) | read 0x10 | 0 | dump incomplete | UE4 critical section `this+0x218` (eboot `0x7559dd`) |
| 11-25 17:49 | read 0x10 | 0 | dump incomplete | online task manager `0x1002fe00e8` again (`0x9f9af`, thread OnlineAsyncTask) |
| 11-25 17:54 | non-canonical | `0x12f25f1000000000` | dump incomplete | ICU `umtx_lock` again (`0x367a5de`, `posix_mutex_lock`) |

Read with cdb from `D:\MediEvil-archive\crashdumps` (`d6-*.out`, `d8-*.out`, `d1123*.out`). The "execute 0" form
(11-14..11-19) is the same fault: `[rsp]` = `libwinpthread-1.dll+0x3586`, the impl block now holds a live
`GuestCond {pthread_cond_t native, int seq}` (16 bytes), so `vtable` = the winpthread `cond_t` and `cond_t+0x10` = 0.

What the dumps show (**confirmed**):
- `G` is a normal `GuestMutex`: 0x10-stride LFH blocks, each `{impl pointer, encoded header 0x8800xx00...}`, every
  neighbour pointing at a valid impl in the same impl subsegment (e.g. 11-23: `G=0x1626ea3f0` -> `0x162515f10`,
  neighbours -> `0x162515b50..dd0`; `0x162515e50` is a valid impl `{libwinpthread+0xb1a0, 0x4550, ffffffff00000000}`).
- The impl's 0x20 LFH block has an intact header and a new owner (GuestCond: request 16 bytes, header `0x90..`; a
  24-byte object: `0x88..`), i.e. it went through `free` and `malloc` normally. In 11-23 a zeroed span followed by an
  NVIDIA DirectX UMD (`nvwgf2umx.dll`) table covers the impl and its neighbours' headers: there the heap handed out a
  larger block over a live LFH subsegment (heap metadata damage or a stale free of a large block).
- Contents at the fault (registers, so not changed by the dump writer): GuestConds that had already been signalled
  (seq up to 12), and a winpthread thread-name copy ("TaskGrap": `pthread_setname_np` strdup's our 15-character name,
  `"TaskGraphThread"`, 16 bytes). The impl was freed well before the lock that crashed, and the block lived on as
  something else.
- The same mutexes are hit again and again (BSS `0x5b145e0` x2, `0x581cf98` x2, NpToolkit2 `0x5ff0f78` x2, ICU x2,
  online manager x2): mostly **static (zero-initialised) mutexes**, created lazily by `ensure_mutex` and locked rarely,
  plus objects built early in boot. A rarely locked mutex keeps a freed impl unnoticed until its block is reused.

### Guest call sites (eboot `out/eboot.elf`, image offset = vaddr; PLT read from the dynamic table)

- `0x7559dd` (17:48): function `0x755990`, `scePthreadMutexLock(r14+0x218)` then owner (`scePthreadSelf`) at `+0x220`,
  count at `+0x228`: a recursive critical-section wrapper inside a larger object.
- `0x9f9af` (17:49, 11-23): function `0x9f970`, lock at `this+0x68`, owner `+0x70`, count `+0x78`, then copies the
  queue at `+0x58` (an online async task queue; object at guest `0x1002fe0080` in both runs). Initialised explicitly
  (`0x9f040-0x9f131`: attr init, settype 4, `scePthreadMutexInit`).
- `0x367a5de` (17:54, 11-18): ICU 53 `umtx_lock` (`0x367a5b0`, strings `icu4c-53_1/source/common/umutex.cpp(157)`):
  `pthread_mutex_lock(m ? m : &globalMutex)`, globalMutex at BSS `0x5c66598`. ICU's mutexes are static
  (`U_MUTEX_INITIALIZER`), never destroyed: a mutex that is never destroyed still lost its impl.

### Our code paths (`src/runtime_mutex.c`, **found**)

- `ensure_mutex` (85-99): double-checked creation under `static_init`, acquire / release on the slot. No race found.
- `mutex_init` (66-82): `calloc(G)`, `pthread_mutex_init(&G->native, attr)` (explicit, never a static initializer).
- `mutex_destroy` (443-460): trylock, unlock, `*slot = 2`, retire; frees only the entry 8192 destroys older.
  `cond_destroy` (506-524) the same with its own ring. Nothing else in `src/` calls `pthread_mutex_destroy` or frees a
  `GuestMutex`.
- winpthread `pthread_mutex_lock` lazy init (`+0x359d..+0x35f3`): cmpxchg into the slot, the loser destroys its own
  copy and locks the winner's: correct. `pthread_mutexattr_*` allocate nothing in this winpthread.
- Not S1 at boot, but real: two threads destroying the same slot at once can both retire the same `G` (the slot is
  read before the trylock, written after); after 8192 more destroys that is a double free. Same for `cond_destroy`.

### Today's runs and dumps

- 17:48:55, 17:49:23, 17:54:01 runs: no "Crash dump:" line; dumps `-174841-14184`, `-174903-19272`, `-175350-5704` have
  19 directory entries, all zero (MiniDumpWriteDump writes the directory last). Only the exception stream (`0x690`) and
  the fault context (`0x14778` / `0x14888`) were written; no thread list, module list, heap or stack memory (the impl,
  `G` and the faulting stack's values are absent). Every type-0x1a65 file left in `crashdumps` since 11-03 is like
  this (when the fallback runs it overwrites the file); every 0x1065 dump (100-200 MB) opens.
- The process ended during that write (exit code `0xCFFFFFFF` = most likely Windows ending a not-responding process;
  the 17:49 run reported 0). So `TerminateProcess(3)` and the 0x1065 fallback never ran.
- 17:52:04 (`run-20261125-175204`): not S1 by signature. `STOP: guest raised debug exception code=0xa0020007` (the
  guest's own `sceKernelDebugRaiseException`, `runtime_kernel.c:186`, `exit(23)`), then the process ended with
  `0xCFFFFFFF`. Whether it is another effect of the same heap damage is unknown.
- Full fault contexts (from the dumps): 17:48 `rsi(G)=1618e7990 rcx(impl)=792eafb0 rax=0 r14=1002959fc0 (object)
  r15=100295a1d8 (slot)`; 17:49 `G=542a3f30 impl=1619cd360 rbx=1002fe00e8 (slot) r12=1002fe0080`; 17:54 `G=16134b630
  impl=161753be0 rax=12f25f1000000000`.

## Mechanism

A live guest mutex's winpthread object (24 bytes, process heap, LFH 0x20 bucket) is released by a `free` that is not
its owner's, the block is reallocated (GuestCond, thread-name copy, other 16-24-byte objects, or a larger block in
11-23), and the next lock of that mutex calls through what is now someone else's data. Candidates for the bad free,
ranked:

1. **A double free / stale free of a small block somewhere in the process** (host runtime C, the shadPS4 C++ renderer,
   a Vulkan layer or a driver: all use the same process heap). After the first free the block is typically taken by the
   next mutex created on that core (impls dominate the bucket at boot); the stale second free then frees the impl.
   Fits 9 of the 10 dumps (normal LFH reuse, intact headers). Not located.
2. **Heap metadata damage** (an out-of-bounds write into the host heap, e.g. a host buffer overrun, or guest code
   writing through a host object it was handed): the heap hands out memory that is still in use. Fits 11-23 (a big
   zeroed allocation over a live subsegment, NVIDIA UMD data on top). Not located.
3. Our retire rings: ruled out for boot by the count bound (they free only after 8192 later destroys).

Timing: the "TaskGrap" case puts the free before the TaskGraph threads named themselves, i.e. in the first seconds,
while the host's GPU start-up (Vulkan device, DLSS / NGX, Reflex, swapchain, NVIDIA present layering with
`nvwgf2umx`) runs beside the guest's early init. Suggestive only.

## What would confirm it (proposals; each launch needs the user's go)

1. **Page heap run, to catch the bad free itself** (best). As admin: `gflags /p /enable medievil-runtime.exe /full`
   (or Application Verifier, "Heaps"), plus `gflags /i medievil-runtime.exe +ust`. Freed blocks are kept unmapped, so
   a double free stops at the second `free` with its stack, and a lock on a freed impl faults on a decommitted page
   where `!heap -p -a <impl>` shows the stacks of the allocation and the free. Because the double free itself probably
   happens on most boots (the crash needs the LFH coincidence, the bad free does not), one or two launches may be
   enough. Costs: memory and speed at boot (start-up only is needed); `/full /size 0x10 0x100` limits it to small
   blocks if the full mode is too heavy; remove with `gflags /p /disable medievil-runtime.exe` afterwards.
2. **Make the next S1 dump usable** (probe.c `write_crash_dump`, proposal): write type 0x1065 first (seconds, and it
   holds the memory around `G` / impl through indirectly referenced memory), and the large type second into another
   file or only with `GOW3_CRASH_DUMP=full`. Today 0x1a65 runs for 10+ s, fails or is killed, and the 0x1065 that would
   have worked is never written.
3. **Dev-build tripwire**: at `mutex_init` record the impl pointer and its event handle in `GuestMutex` (a dev-only
   field); in `mutex_lock_impl` / unlock check `impl[0]` is a winpthread mutex vtable and `impl[1]` the recorded handle;
   on mismatch print guest site, slot, `G`, impl, and the impl's first qwords, plus the time since creation. Tells which
   mutexes lose their object and when, without a crash.

## Proposed fix

The fix for the cause depends on who frees (step 1 above): if it is ours (runtime C or the shadPS4 code), fix that
double free; if a layer or driver, report it and avoid the path. Until then:

- **Mitigation: take guest mutexes off the shared heap.** Allocate `GuestMutex` from a private heap (`HeapCreate`) or a
  slab, with the lock inside it (SRWLOCK plus owner / recursion count for the errorcheck and recursive types, or a
  CRITICAL_SECTION), instead of winpthread's separately `malloc`ed object; add a magic word checked on every call with a
  STOP line naming the guest site. Then no block in the process heap belongs to a guest mutex, and a stray free cannot
  turn into S1. `futex_cond_wait` unlock / relock and the `GOW3_FUTEX_COND=0` path (`pthread_cond_wait` on
  `m->native`) need the same lock. Risk: medium. Every guest lock changes (type semantics, `scePthreadMutexTimedlock`,
  EDEADLK / EPERM codes, lock speed and so frame timing: needs a perf A/B); and it hides the bad free, which will then
  land on another 16-24-byte object (rwlocks fail with EINVAL, GuestConds, thread names, C++ objects): S1 may turn
  into a rarer, different fault. So this is a mitigation to pair with step 1, not a replacement for it.
- **Cheaper stopgap** (release-safe): the tripwire of step 3 that, on mismatch, logs once and re-creates the native
  lock instead of crashing. Low cost (two loads per lock), but it only notices a freed impl once its block is reused;
  locks on the freed block before that still write into freed memory, and if the block went to another mutex the check
  must use the recorded handle to see it. It also grows `GuestMutex` from 8 to 16+ bytes, moving it into the same LFH
  bucket as the impls.

Confidence: the bad value and its form (impl block freed and reused while the mutex lives): confirmed, 10 dumps plus
registers of 14 faults. That our mutex code is not the freer at boot: high (code plus count bound). The freer: not
found; candidate 1 more likely than 2 on the dump evidence.

## Update 2026-11-25 evening: the writer found under the page heap

Run with `gflags /p /enable medievil-runtime.exe /full` + `/i +ust` and `GOW3_CRASH_DUMP=full` (build d59f6c1 +
the dump-order change), first launch (19:36): a guest write fault into the guard page after an 8-byte host block,
`or dword ptr [rax+20h],100h` at libc.prx +0x60aa2 (image offset 0x5cd0aa2), called from `sceLibcMspaceCreate`
(libc.prx +0x62b00) by libSceNpToolkit2.prx (0x5f6b058, 0x5f7374b), from the game at 0x1c47ad. The block held one
pointer to another page-heap block plus the page heap's fill: a `GuestMutex` (8 bytes in the release build).

libc's malloc / mspace setup calls `pthread_mutex_init(&lock, attr)` and then sets a flag in libkernel's own
mutex struct through the handle: `[handle + 0x20] |= 0x100`. Three sites in libc.prx: 0x5ebed, 0x5ee91, 0x60aa2
(scan `research-data/s1-20261125/pageheap/scan_flag_writes.py`; 0 in libSceFios2, libSceNpToolkit2 and the
eboot). On the PS4 the struct is larger than 0x24 bytes; our handle is the 8-byte `GuestMutex`, so every boot set
bit 8 of a dword 24 bytes past one record: a neighbouring block in the same heap bucket. Which block, and whether
the bit was already set, changed per run: S1's ~1 in 25. (Dev builds with GOW3_MUTEX_HOLDERS fields had a 48-byte
record, so the write landed inside it.)

Fix built (2026-11-25): `GuestMutex` reserves the PS4 struct's first 0x40 bytes (`ps4_layout`, never read),
`_Static_assert(sizeof(GuestMutex) >= 0x40)`. Lock path unchanged; ~56 bytes more per mutex. mutex-test and
rwlock-test pass (dev tree; the release tree's mutex-test does not compile: unused dev-only helpers, pre-existing).
Next: one launch with the page heap still on (it must pass the mspace setup), then the page heap off and normal
launches.
