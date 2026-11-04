# Study: what the per-frame EOP wait at guest+0x68af47 protects, and whether early signalling stays safe with in-place reads

Status: study, 2026-11-04, on `deck-lab` (`64d998f`). Read only: nothing built, launched or committed. Parent:
[plan-linux-deck.md](plan-linux-deck.md) ("Pinned GPU clock" direction and "Wait sites and per-thread CPU": static
analysis of what UE4 recycles after the EOP wait, for a hybrid U5). Inputs: [study-fence-waits.md](study-fence-waits.md)
(G1, G2), [design-u5-deck-memory.md](design-u5-deck-memory.md) (sections 8-9),
[study-bloodborne-07-memory.md](study-bloodborne-07-memory.md) (1.6-1.7), `research/docs/data/deck-waitsites-1.log`.

Marks: **[code]** read in the eboot disassembly or in this tree. **[log]** a number from a log in `research/docs/data/`.
**[inferred]** reasoning or naming, not checked. Nothing on this page is "verified" in the project's sense.

Address space: every `guest+0x...` is an ELF virtual address of the 01.02 eboot (`out/eboot.elf`, written by
`scripts/prepare.py:128-227` from `E:\MediEvil\CUSA11227-0102\eboot.bin`, source sha256 `fe2f88ba...`,
`out/analysis.json`). The runtime maps that image at `runtime_image_base()` (`src/probe.c:42`) and the wait-site report
prints `at - image` (`src/runtime_waitsites.c:5`, `:59-70`), so log offsets and these addresses are the same numbers.
Text segment: file offset 0x4000 = guest 0; add 0x400000 for the Ghidra project.

## Answer

1. **What the wait is.** guest+0x68af47 is the return address of `sceKernelWaitEqueue` in guest 0x68acc0, the work
   function of UE4's PS4 submission task (`SubmitGPUCommandsAsyncTaskThreadPS4`). The equeue is
   `"SubmitEndOfFrameQueue"`, registered once with `sceGnmAddEqEvent(eq, 64, 0)` [code]. The function:
   - holds the RHI manager's mutex;
   - takes the queued graphics and compute submissions;
   - **waits until the label of the previous frame equals `frame - 1`**;
   - calls `sceGnmSubmitDone` once per frame;
   - then submits the queued command buffers (`sceGnmSubmitCommandBuffers`; compute through a DingDong wrapper).

   The label is written by an `EVENT_WRITE_EOP` with interrupt and 64-bit data = frame number. EndFrame appends it to the
   frame's last command buffer, just before the final submit (`sceGnmSubmitAndFlipCommandBuffers`). Nothing is freed
   or recycled inside the wait function after the wait. **The wait gates submission, not memory.** [code]
2. **Frames ahead.** The fence pair is double-buffered by frame parity (`G+0x1f8` flips each frame). The wait is for
   **frame N-1, the newest frame that has been submitted**: frame N's own EOP sits in its not-yet-submitted final
   buffer. On the PS4 the GPU therefore runs at most one frame (N-1) while the CPU records N. The first submission of
   frame N, and EndFrame(N) (which blocks on the same mutex), wait for N-1's GPU completion. [code]
3. **What is recycled and how.** Every recycling path in this RHI is counted in frames, not keyed on the label:
   - a **transient chunk pool** (guest 0x5805e70) with **4 frame slots** and chunks of 64 KiB, 512 KiB and 8 MiB. Its
     slot advances once per EndFrame; a slot's chunks are reused 4 frames later and freed after 6 idle frames;
   - a **3-frame deferred free list for 8-byte GPU labels**;
   - two **flush-then-rewind** paths (guest 0x679190 and 0x68baf0): the CPU spins on an EOP label written into the
     command buffer, then rewinds the context's command and resource-table chunks to their start in the same frame.

   The GPU reads the transient pool's Garlic chunks directly:
   - per-draw shader constant buffers (the packed `$Globals` commit, 0x6822a0);
   - LCUE resource tables;
   - texture-update staging for `UpdateTexture2D`/3D.

   The command-buffer chunks (Onion) hold PM4, which stage A decodes on the CPU, plus embedded 4/8-byte labels and a
   0xC0-byte global resource table. [code; Garlic/Onion per kind inferred]
4. **Early signalling with in-place reads is not safe unconditionally.**
   - Today's early signal plus `frames_queued=1` (`vk_presenter.cpp:674-691`) lets the GPU lag **3 frames** behind the
     game's RHI frame. Native lag is 1.
   - That still covers the 4-slot pool for data the RHI thread allocated, with no frame to spare.
   - It misses by one frame for data the render thread allocates into the current slot for the next frame (texture
     staging) [inferred].
   - It breaks outright with `frames_queued` 0 or ≥ 2.
   - The flush-then-rewind paths are unsafe whenever they run, because their spin label is early too.

   Two fixes, either sufficient for the pool:
   - **(A)** keep the transient pool's chunks on the copy path (snapshot at stage B, as today) and read everything
     else in place;
   - **(B)** hold the frame EOP (ident 64) until the frames-ahead wait of the flip that follows it. This holds the GPU
     lag at 2. Its cost is likely small, because stage B pays that wait anyway [inferred].

   The rewind paths need (A) for their chunks, or their spin label GPU-ordered. Confidence: medium-high for the
   structure, medium for the hazard margins (render-thread lead and kind-to-memory-type mapping not checked).
5. **Runtime checks** (section 6): log the pool's chunks and kinds, the GPU lag at slot reuse, the count of
   flush-and-rewind calls, the EOP-with-IRQ targets, and the four cvars.

## 1. The wait and its callers

### 1.1 Function map

| Guest | What it is | Evidence | UE4 4.21 PS4 RHI name |
|---|---|---|---|
| 0x58053c0 (`G`) | RHI manager global. Members: mutex `+0xb8`; frame number `+0x2d8`; parity `+0x1f8`; display index `+0x1f0`; frame labels `+0x210/+0x218`; start / end timestamps `+0x228/+0x230`, `+0x238/+0x240`; submit task `+0x348`; SubmitDone task `+0x338`; equeue `+0x5d0`; graphics state `+0x200`; context `+0x208` | init at 0x688790 (labels 0x6889b3-0x688afa, equeue 0x688de0, tasks 0x689076/0x6890a2) [code] | `GGnmManager` [inferred] |
| 0x679560 | EndFrame (lock G, end the frame, flip the parity, start the next command buffer, rotate the label free list) | reached from FDynamicRHI vtable slot 0x4ed70e0 (thunk 0x679540) and RHI-command thunks 0x67ef80 / 0x67efa0; reads `u'r.VSync'` [code] | `RHIEndDrawingViewport` → `FGnmManager::EndFrame` [inferred] |
| 0x68a5a0 | writes the frame's end-of-pipe timestamp and **EOP+IRQ label = frame**, enqueues async compute, calls 0x68a9b0, then `G[0x2d8]++` | [code] | — |
| 0x68a9b0 | calls 0x68acc0 on the submit task, then the final submit of the frame (`sceGnmSubmitAndFlipCommandBuffers` or `SubmitCommandBuffers`) | string `u'Final submission failed with: 0x%x, %p'` [code] | — |
| **0x68acc0** | **the wait** (guest+0x68af47), then SubmitDone, then the queued graphics and compute submissions | strings `u'async submitCommandBuffers failed: 0x%x, %i, %p'`, `u'async compute submit failed...'` [code] | submit task "ProcessQueue" [inferred] |
| 0x6a74d0 | `SubmitGPUCommandsAsyncTaskThreadPS4` loop: `FEvent::Wait(-1)` (vtable +0x20) on `task+0x90`, then 0x68acc0 while `r.PS4ContinuousSubmits` | vtable slot 0x4ed7cd0; thread name at 0x68907f [code] | — |
| 0x687de0 | enqueue a graphics submission and `FEvent::Trigger` (vtable +0x10); call sites include the 32 KiB command-buffer split at 0x672141 (`r.PS4CommandBufferSplitSize`) | [code] | — |
| 0x68ba00 | `sceGnmSubmitDone` once per frame (`G[0x2e0]` = last frame done) | [code] | — |
| 0x6a7300 | `SubmitDoneAsyncTaskThreadPS4`: calls 0x68ba00(force) when no SubmitDone came for a while | [code] | GPU-watchdog keep-alive [inferred] |
| 0x68a490 | the same wait as a standalone function; no direct, tail-jump or vtable reference found | [code] | unused or called through data [inferred] |

Cvars (registration in 0x6b8cf0, defaults in the image) [code]:

| Address | Cvar | Default |
|---|---|---|
| 0x5600618 | `r.PS4CommandBufferSplitSize` | 32768 |
| 0x5600620 | `r.PS4ContinuousSubmits` (async submit thread) | 1 |
| 0x5600624 | `r.PS4EventBasedFrameEnd` ("Event based submission thread. 0 will go back to polling") | 1 |
| 0x5600628 | `r.PS4TimeSubmitWaits` | 1 |

The ini may override them (not checked).

### 1.2 The wait (0x68acc0, excerpt)

```
68aeab: lea  r14, [rip+...]            ; G = 0x58053c0
68aeb2: mov  r13d, 1
68aee6: movsxd rax, dword [r14+0x1f8]  ; parity i
68aef3: sub  r13, rax                  ; r13 = 1 - i  (the other slot = previous frame)
68aef6: mov  eax, dword [r14+0x2d8]    ; frame number F
68aefd: mov  rcx, qword [r14+r13*8+0x210]
68af05: dec  eax                       ; F - 1
68af07: mov  rcx, qword [rcx]          ; *label[1-i]
68af0d: cmp  rcx, rbx                  ; == F-1 ?
68af20: cmp  dword [rip+...], 0        ; r.PS4EventBasedFrameEnd (0x5600624)
68af29: mov  rdi, qword [r14+0x5d0]    ; "SubmitEndOfFrameQueue"
68aed6: (timeout 0xea60 = 60000 us)
68af42: call sceKernelWaitEqueue
68af47: mov  rax, qword [r14+r13*8+0x210]   ; <- wait site; loop until *label == F-1
68af79: lea  rdi, G ; xor esi,esi ; xor edx,edx
68af84: call 0x68ba00                  ; sceGnmSubmitDone (once per frame)
68b011: call sceGnmSubmitCommandBuffers (per queued entry, 0x1b0 bytes each)
68b203: call 0x2e8b8f0                 ; compute submit, retried on 0x80000001
```

When `r.PS4EventBasedFrameEnd` is 0, the loop polls with `Sleep(0)` (0x8e7d90) instead.

The mutex `G+0xb8` is taken at 0x68acf1 and held across the wait. EndFrame takes the same mutex first
(0x67959c, return guest+0x6795a1). The Deck log shows `mutex_lock guest+0x6795a1` at 66/s, 1.5 ms average, next to
`equeue_wait guest+0x68af47` at 63/s, 1.8 ms (`deck-waitsites-1.log:337-339`) [log]. While the submit thread waits for
the GPU, the RHI thread's EndFrame waits on the submit thread.

### 1.3 Before the wait: the frame-end EOP (0x68a5a0)

```
68a69d: movsxd rax, dword [r14+0x1f8]         ; i
68a6ba: mov  rcx, qword [r14+rax*8+0x238]     ; end-of-frame timestamp slot
68a6c6: call 0x3bedb40   ; EOP, event 4, data_sel 4 (GPU counter), no IRQ
68a6d6: mov  r9d, dword [r14+0x2d8]           ; data = F
68a6f0: mov  rcx, qword [r14+rax*8+0x210]     ; label[i]
68a6fc: call 0x3bedc20   ; EOP, event 4, data_sel 2 (64-bit imm), DW3 |= 0x2000000 (int_sel 2: IRQ)
68a932: call 0x68a9b0    ; -> 0x68acc0 (wait for F-1, submit queued), then final submit (+flip)
68a937: inc  dword [r14+0x2d8]                ; F -> F+1
```

- 0x3bedb40 and 0x3bedc20 are the statically linked Gnm `writeAtEndOfPipe` helpers. Both emit 0xC0044700
  (`EVENT_WRITE_EOP`); the second adds the interrupt bit [code].
- Labels: 8 bytes, allocated at init with `0x8e4440(size 8, kind 1)`, initial value −1 (0x688a17, 0x688afa) [code].
- Fingerprint the port can match in stage A:
  - `EVENT_WRITE_EOP`, `event_type` 4, `data_sel` 2, `int_sel` 2, cache action 0;
  - target is one of the two addresses stored at guest `0x58053c0+0x210` and `+0x218`;
  - data is the incrementing frame number.

### 1.4 After the wait, in EndFrame (0x679560)

In order [code]:
1. Computes GPU frame time from the other slot's start/end timestamps (`G+0x228/0x238 + (1-i)*8`) into 0x5c28340.
2. Ticks the transient pool, 0x68b360 (section 3.1).
3. Empties the RHI thread's list of large transient chunks (`G+0x2f8 + G[0x2f4]*0x20`).
4. Display index `G[0x1f0] = (G[0x1f0]+1) % 2`; parity `G[0x1f8] = 1 - G[0x1f8]` (0x6798c9).
5. Starts the next command buffer (0x684de0 → 0x66d000).
6. Writes the next frame's start timestamp EOP (0x679a22).
7. Under mutex 0x5805a70, rotates the 3-slot label free list (section 3.2).

## 2. Frames ahead

| | PS4 (native) | Port today (frame EOP at stage B, `frames_queued=1`) |
|---|---|---|
| The game sees label F-1 when | the GPU finished F-1 | stage B recorded F-1's EOP. Stage B passed flip F-2 before that, and at that flip it waited for the GPU tick of F-3 (`vk_presenter.cpp:680-691`) |
| GPU known done when the game starts recording F+1 (right after EndFrame(F)) | F-1 | **F-3** |
| GPU lag behind the RHI frame being recorded (G = F+1) | ≤ 1 frame (done ≥ G-2) | ≤ 3 frames (done ≥ G-4) |
| GPU-ordered EOP (U5-2 B0/B2) | — | done ≥ G-2, as native; but stage A/B add ~12-15 ms between submit and GPU start, so CPU and GPU take turns (−36%) [log] |

The double-buffered labels allow exactly one older frame to be outstanding. No modulo counter deeper than 2 guards
this wait [code]. The counters that are deeper (4-slot pool, 3-slot label list) are frame counts and never consult the
label (section 3). The two places that read `label[...]` are 0x68acc0 and 0x68a490: a scan of every `+0x210/+0x218`
access in 0x660000-0x6c0000 [code].

## 3. What is recycled, when, and who reads it

### 3.1 The transient chunk pool (guest 0x5805e70)

```
697ca0 (allocate, rdi=pool, esi=kind, edx=size class):
  697d12: mov  edx, [rbx+0x264]          ; current frame slot (0..3)
  697d18: mov  r8d, [G+0x2d8]            ; frame number
  697d48: cmp  edx(cursor), [list+8]     ; reuse entry [cursor] if it exists:
  697d5b: mov  [rsi+rcx*8], r8d          ;   entry.frame = F, return entry.block
  697da0: call 0x8e4440                  ; else allocate size_table[class] (0x3c5f5bc: 0x10000, 0x80000, 0x800000), align 0x400, kind
68b360 (EndFrame tick):
  68b3c3: mov  eax, [r15+0x264] ; inc eax ; and eax, 3   ; next slot
  68b3ba: vmovups [r15+0x240], ymm0                    ; cursors of the new slot = 0
  68b42c: sub  edx(F), [entry.frame] ; cmp edx, 6 ; jb keep
  68b43f: call 0x8e4530                                ; free blocks idle >= 6 frames
```

- One slot per frame and 4 slots. Chunks taken in RHI frame G are handed out again from G+4 on. The pool's mutex is
  `+0x268`; the slot index is global, shared by every thread [code].
- `0x8e4440` kinds: 0 → allocator pool 1, 1 → pool 2, 2 → pool 3. Read against the stats string `u'CPU %.2f MB, Garlic
  %.2f MB, Onion %.2f MB, FrameBuffer...'` (0x3c8c9dc), this gives **kind 0 = Garlic, kind 1 = Onion**. That matches
  Sony's convention: command buffers and labels in Onion, GPU data in Garlic [inferred].

| User | Kind / size class | What goes in | GPU reads it? |
|---|---|---|---|
| command buffer chunks: `ctx+0x170` list, 0x66d000 / 0x66cb60 | 1 (Onion), 64 KiB | PM4 | **no**: stage A decodes it on the CPU. But see embedded data below |
| LCUE resource buffer: `ctx+0x190` list. `0x2e9ac20(dcb, 64K, rb, 64K, 0)` matches `LightweightGfxContext::init` | 0 (Garlic), 64 KiB | descriptor tables (V#/T#/S#) pointed to by user data | GPU on the PS4. The port reads the tables on the CPU at bind time (shadPS4 SRT walk) [inferred] |
| context transient ≤ 64 KiB: `ctx+0x1c50`, 0x680450 | 0 (Garlic), 64 KiB | **per-draw packed constant buffer** (0x6822a0: allocate, `memcpy` shadow → chunk, V# via LCUE 0x2e96b60; called from the draw entry 0x680c80, vtable slot 0x4ed67e0) | **yes**, shaders |
| small allocations: `ctx+0x1b0`, 0x686b60 / 0x6761a0 | 0 (Garlic), 64 KiB | 16-byte items (descriptors or small buffers) | likely [inferred] |
| global transient ≤ 8 MiB: `G+0x2f8 + idx*0x20`, 0x680570. Two lists: `G[0x2f0]` when not on the RHI thread (0x73e6b0 compares the thread id at 0x581c7b0), `G[0x2f4]` on it; the render thread toggles 0x2f0 and an RHI command (0x6a8610) hands the index over | 0 (Garlic), 8 MiB | staging for `UpdateTexture2D` (0x69b3e0, string `'UpdateTexture2D'`, FDynamicRHI slot 0x4ed6fe8), the next slot 0x69c110 (UpdateTexture3D [inferred]), 0x691d30 (texture region lock, slot 0x4ed7270), 0x682560 (> 64 KiB constant data) | **yes**: GPU copy or detile source |

**Command-buffer embedded data** (`allocateFromCommandBuffer` 0x3becb60, about 35 call sites) [code]:
- 4- and 8-byte labels, for query and wait targets and for the flush spin label;
- a 0xC0-byte block: 12 × 16-byte V#s, the global resource table [inferred] (0x6799d4, 0x684cc6, 0x6794ae);
- one 0x40-byte block in Gnmx (0x2e8df69).

No vertex or constant payloads were found among them.

### 3.2 GPU labels: 3-frame deferred free list

- Allocation (0x6871e0): pop from `G+0x370`; when it is empty, `0x8e4440(0x400, align 8, kind 0)` is split into 127
  slots.
- Release (0x686dc0, 0x686e90, 0x686f30, 0x687020, 0x687100: destructors of label holders, vtables 0x4ed6c48 /
  0x4ed6c78) pushes to `G+0x390`.
- EndFrame rotation (0x679aa2-0x679ae6):
  - appends `G+0x3b0` to the free list `G+0x370`;
  - shifts `0x390 → 0x3a0 → 0x3b0`.
- A label released in frame R is reused from frame R+3 on [code].
- Poll: `0x687360: return *label == 1` (`FGnmGPUFence::Poll` [inferred]). These labels are GPU-written and CPU-read.

### 3.3 Flush-then-rewind (intra-frame reuse)

```
679190 (reached through callback 0x684590, stored at DCB init; trigger not resolved):
  6791e7: call 0x3becb60          ; 8-byte label inside the command buffer
  679217: call 0x3bedb40          ; EOP, imm 1, no IRQ, cache action 0x38 (WB+INV L1/L2)
  6793a6: call 0x68a9b0           ; submit (waits for F-1 first, 1.2)
  6793c0: inc [rbp-0x2c] ; cmp [r13], 1 ; jne   ; CPU busy-spin until the label is 1 (no syscall)
  6793e3: [ctx+0x180] = first chunk of ctx+0x170   ; rewind command buffer chunks
  67941d: [ctx+0x1a0] = first chunk of ctx+0x190   ; rewind LCUE resource chunks
  67944b: call 0x66d350           ; re-init the command buffer in chunk 0
68baf0: the same sequence (68bb47 .. 68bdfa). Called from 0x6904c0 (FDynamicRHI slot 0x4ed70a0;
        RHIBlockUntilGPUIdle / RHISubmitCommandsAndFlushGPU [inferred]) and 4 times from 0x6a0c80.
```

On the PS4 the spin ends at GPU idle, so the rewind is safe there. In the port the spin label is a no-IRQ EOP written
at stage B time (`liverpool.cpp:416-423`). The rewind then overwrites, in the same frame, LCUE tables and command
memory the GPU may not have executed yet. The busy-spin is invisible to `GOW3_WAIT_SITES` (no guest call). How often
it runs: unknown.

### 3.4 Summary table

| Memory | Reuse rule | Distance in RHI frames | GPU reads in place under U5? | Needs, under early frame EOP |
|---|---|---|---|---|
| frame labels and timestamps (`G+0x210..0x260`) | parity | 2 | written by the GPU (EOP) | written at stage B today: fine |
| transient Garlic chunks (constants, LCUE tables, staging) | pool slot | 4; render-thread data is consumed 1 frame after its slot [inferred] | **yes** | GPU lag ≤ 3 (RHI data), ≤ 2 (render-thread data) |
| command buffer chunks (Onion) | pool slot, or rewind after flush | 4, or **0** after a flush | PM4 no; embedded labels written by the CP; global table [inferred] | rewind: copy or GPU-ordered spin label |
| query and fence labels | 3-frame free list | 3 | GPU-written, CPU-read | write at stage B (today) or keep lag < 3 |
| freed pool blocks → general heap | 6 idle frames (checked every 4) | ≥ 6 | anything later | fine |
| persistent RHI resources (textures, static buffers) | RHI deferred deletion | not traced (open) | yes | open |

## 4. Consequences for the hybrid U5

**What the early signal buys.** The game decides "N-1 is done" at stage B time, so the RHI thread records ~2 frames
deeper than on the PS4. That depth is the CPU/GPU overlap that the GPU-ordered EOP (B0/B2) removed. Keeping it is the
point of the hybrid.

**Why the copy model is safe today.** Stage B snapshots buffer contents (stream buffer for ≤ 16 KiB binds, host
copies before the submit: `design-u5-deck-memory.md` §9), so later CPU writes cannot reach the GPU's copy.

**Under in-place reads**, the condition is "the GPU finished every read of a range before the game writes it again".
With the reuse distances above:

| Case | GPU done when RHI frame G starts | Pool data from RHI thread (needs ≥ G-4) | Render-thread data in that slot (needs ≥ G-3) [inferred lead 1] | Flush-and-rewind |
|---|---|---|---|---|
| early frame EOP, `frames_queued=1` (today) | ≥ G-4 | safe, no spare frame | **race possible by 1 frame** | **unsafe** |
| early frame EOP, `frames_queued` 0 or ≥ 2 | unbounded / ≥ G-5 | **unsafe** | **unsafe** | **unsafe** |
| (B) frame EOP released after the frames-ahead wait of the next flip (GPU done F-2 when label F-1 appears) | ≥ G-3 | safe (+1) | safe (0 spare) | **unsafe** |
| GPU-ordered frame EOP (B0/B2) | ≥ G-2 | safe | safe | safe if the spin label is GPU-ordered |

**Options** [inferred]:

- **(A) Copy only the transient pool, read the rest in place.**
  - Copy path: chunks from 0x697ca0, Garlic 64 KiB and 8 MiB plus the Onion command chunks.
  - Fences: the frame EOP stays early; other fences may be GPU-ordered.
  - This is the "dynamic rings stay copied" variant from plan-linux-deck.md. It covers the rewind case too, because
    LCUE and staging reads then come from stage-B snapshots.
  - Detection, in order of preference:
    1. Hook 0x697ca0's return (guest+0x697e54, `mov rax, r14`; size class in `r12b`/`dl` at entry, kind in `esi`) and
       record `[ptr, ptr+size)` as transient for the session.
    2. Walk the pool at guest 0x5805e70 once a frame: 4 slots of 0x90 bytes; per kind 0x30 bytes; per size class a
       TArray at `+0`/`+0x10`/`+0x20` of 0x28-byte entries `{u32 frame; u64 ptr; ...}`.
    3. Watch the dedicated over-8 MiB blocks (0x682560 / 0x691d30 / 0x69c110 fallback through 0x8e4440, freed at once
       with 0x8e4530 at 0x69c495).
  - A range freed to the general heap may later hold persistent data. Copying it then costs time but is still
    correct.
  - Expected size: up to 4 slots × the per-frame transient volume. On the Deck most binds are ≤ 16 KiB stream copies
    already [inferred].
- **(B) Lag bound instead of copies.** Hold the guest-visible write of the frame EOP (int_sel 2, the two `G+0x210/0x218`
  targets) until stage B has passed the following flip's `frames_ahead` wait. Stage B pays that wait anyway, so the
  game should lose little overlap.
  - It makes the pool safe without copies (table above).
  - It does not fix the rewind paths: their spin labels also need GPU order (cheap, since they are rare), or their
    chunks need (A).
  - It needs `frames_queued` fixed at 1 while U5 is on.
- **Labels (3.2)** keep stage-B writes (today) or GPU markers with lag < 3. Readbacks that a polled label guards
  (GPU write → CPU read) stay on today's copy / remap path, or need GPU-ordered labels. Early signalling is safe only
  for the frame fence.
- **`sceGnmSubmitDone` sits right after the wait** (0x68af84), before the frame's submits. With an honest submission
  lock and no overlap, it serialises the same point again (G1 in study-fence-waits.md). Keep K ≥ 2 or the decode mode.

**Confidence.**

| Claim | Confidence | Basis |
|---|---|---|
| Wait structure: N-1, parity labels, SubmitDone and submits after it, no memory recycled in it | high | [code] |
| Pool structure (4 slots, 3 size classes, reuse and 6-frame free) | high | [code] |
| Rewind paths exist | high | [code] |
| Pool users: constants, LCUE, staging | medium-high | [code] shape + strings; vtable names [inferred] |
| Kind 0 = Garlic, kind 1 = Onion | medium | [inferred] |
| Render-thread lead of 1 frame into the current slot | medium-low | [inferred] |
| Port lag arithmetic | medium | `vk_presenter.cpp:674-691` [code] + reasoning |

## 5. Cross-checks

- PC T-W3's `equeue_wait guest+0x6885e7` and `guest+0x69c1ac` (plan-frame-pacing.md:710-711) are this wait and the
  VideoOut wait in an image shifted by 0x2960. 0x68af47 − 0x6885e7 = 0x69eb0c − 0x69c1ac = 0x2960. Neither PC offset
  is a call return in the 01.02 image; 0x69eb0c is (0x69eb07 `call sceKernelWaitEqueue`) [code, inferred same build
  family]. So the PC's 3.8-4.1 ms per frame was this wait too.
- The Deck's "submits / frame 10.8" with GPU fences [log] fits the 32 KiB split (0x672141) feeding 0x687de0 [inferred].

## 6. Runtime checks (one Deck run, stats only, before any U5 change)

1. **Pool inventory.** Hook guest+0x697e54 (or walk 0x5805e70 every 5 s). Print:
   - chunk count and MiB per kind and size class, and how many are new per 5 s;
   - the fault-map PS4 type of each chunk's first page.

   Expected: kind 0 in Garlic-WC (type 3), kind 1 in Onion (type 0), a stable set after the first seconds.
2. **Lag at slot reuse.**
   - At 0x697d53 (entry reuse) record `F - entry.frame` (always 4 if the slot logic holds).
   - Record the port's last GPU-completed guest frame (flip ticks) at the same moment.
   - Print the minimum and p99 of `F - gpu_done_frame`. Safe under (A): any value. Under in place: ≤ 3, or ≤ 2 if
     render-thread allocations (0x680570 with `G[0x2f0]`) are present.
3. **Render-thread lead.** Count 0x680570 calls by list (`G[0x2f0]` vs `G[0x2f4]`) and the bytes in each, to see
   whether texture staging comes from the render thread.
4. **Flush and rewind.** Count, per 5 s, EOPs with `data_sel` 2, data 1, `int_sel` 0, cache action 0x38 whose target
   lies inside the command buffer being decoded (the 0x679190 / 0x68baf0 fingerprint). Optionally also count entries
   to 0x6904c0. If the count is ~0 in gameplay, the rewind hazard is load-time only.
5. **Frame EOP identity.** Per 5 s, log EOPs with IRQ by target address and data.
   - Expect two alternating addresses, read from guest `0x58053c0+0x210/+0x218`, with data = frame number; no other
     IRQ EOP.
   - Read the four cvars at 0x5600618..0x5600628 to rule out ini overrides.
6. **Then the A/B** (`plan-linux-deck.md` pass rule): the copy model against hybrid (A).
   - Hybrid (A): GPU-ordered fences except the ident-64 frame EOP, the transient pool copied, the rest in place.
   - Variant: (B) instead of (A) for the pool.
   - Watch for flicker in constants (wrong per-draw values), texture-upload corruption after streaming, and guest
     heap faults.

## Open questions

- What triggers callback 0x684590 → 0x679190 (no direct read of the stored pointer at `dcb+0xb5b0` was found)?
- The deferral of RHI resource deletion (`FRHIResource` with extra deletion latency, or the PS4 RHI's own): not traced.
  It decides when persistent buffer and texture memory can be rewritten.
- `r.PS4.AllowDefrag` (0x5600644, default 1) and `r.PS4DefragPoolSize` (0x560062c, 2300): GPU defragmentation moves
  textures by GPU copies and frees the old range. How it waits for those copies under early fences is unknown.
- 0x69c110 frees its dedicated over-8 MiB staging block right after issuing the copy (0x69c46a → 0x69c495). Is 0x69bb20
  synchronous?

## Method

- Tools: capstone 5.0.7 under `py -3.14 -I` over the image rebuilt from `out/eboot.elf` program headers.
- Import stubs: `jmp [rip+x]` near guest 0x3bf0000, matched to `DT_SCE_JMPREL` and the RELA table, named through
  `src/import_names.inc`. Trampolines `jmp rel32` (e.g. 0x3bf2270 → `sceGnmSubmitCommandBuffers`) were followed.
- Vtable slots: `R_X86_64_RELATIVE` addends equal to a function address.
- Code references: rip-relative displacement scan.
- Strings: UTF-16 literals at the referencing sites.
- Scripts: kept in the session scratchpad only, not in the repo.
- No game run, no build.
