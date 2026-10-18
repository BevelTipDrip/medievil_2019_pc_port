# GPU threading reference: scheduler, recording, submit, waits, memory

Read it **before changing the GPU threading / wait / memory path and before every test launch** of that path.
**Keep it current:** a new rule, symptom or dead end goes in here the same day, in one line with a pointer to the
findings. Parent: [START-HERE.md](START-HERE.md).

## 1. Directory

| What | Where |
|---|---|
| PM4 decode (stage A) | `gpu/shadps4/video_core/amdgpu/liverpool.cpp` |
| Draw pipe (stage A → B) | `gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h`, `vk_draw_prep.{h,cpp}` |
| Vulkan recorder / submit | `vk_scheduler.{h,cpp}` |
| Rasterizer | `vk_rasterizer.cpp` |
| Buffer cache / readbacks | `video_core/buffer_cache/buffer_cache.cpp`, `memory_tracker.h`, `region_manager.h` |
| Texture cache / collector | `video_core/texture_cache/texture_cache.cpp`, `readback_queue.h`, `image.cpp` |
| Async shader / pipeline compile | `vk_async_compiler.h`, `vk_pipeline_cache.cpp` |
| Toggles | `gpu/shim/gow3_toggles.h` (bit 58 free for parallel recording) |
| Windows shims | `gow3_win_compat.h`, `src/compat_win.c` |
| Bloodborne source for ports | `C:\Users\Ty\MediEvil\bloodborne_pc` (same paths, `bbport`/`BB_` naming) |

## 2. Invariants: must hold after every change

| # | Rule | What broke when it didn't | How to verify |
|---|---|---|---|
| 1 | **Every ported feature sits behind a `GOW3_` switch that defaults to the old path.** | (rule of the work, 2026-10-18) | Launch with the switch off = P1 numbers. |
| 2 | **A command pool is used by one thread only.** | Bloodborne pins pool *i* to worker *i*. | Code review; validation layers. |
| 3 | **No state carries across a command-buffer cut**: rebind pipeline, descriptors, push constants after a segment split. | Bloodborne invalidates dynamic state at each cut. | Validation layers; visual check by the user. |
| 4 | **Guest-visible fences / labels (EOP/EOS) are written only after the work they cover.** | Bloodborne 2026-10-01 guest heap corruption class. | Long run without heap growth / corruption. |
| 5 | **Targeted waits must list every range the GPU reads** (vertex/index too, not only V#). | Bloodborne DoF bug #44. | User checks DoF / post effects. |
| 6 | **With async submit, the presenter waits until the frame's submit is queued.** | Bloodborne 0.4 GPU hang. | No `DEVICE_LOST` over a long run. |
| 7 | **Keep this port's NVIDIA image sub-allocation** (`image.cpp:120-170`); do not take Bloodborne's ≥4 MiB dedicated allocations. | Measured faster for UE4 in this port. | — |
| 8 | **Nothing outside the recorder records into the scheduler** (async compile workers record nothing). | `ad7169e` "workers no longer record into the scheduler". | Code review. |

## 3. Before every test launch

1. The user has said go for this launch.
2. One change since the last launch, behind a switch; say which.
3. `medievil.ini` backed up and diffed against the backup.
4. Build output is new (exe timestamp, a new log string).
5. Same route, settings and affinity as the baseline; `GOW3_FRAME_STATS` on.
6. Say in advance what each outcome would mean, and what the user should look at.

## 4. Symptom → first place to look

| Symptom | Most likely cause | Look at |
|---|---|---|
| 20-30 ms stutters when the game writes GPU-written memory | Readback `Finish()` on the graphics queue | `buffer_cache.cpp:184-222` (candidate #2) |
| ~8 ms spikes while streaming | Per-burst `vkAllocateMemory` | `buffer_cache.cpp:469-523` (candidate #3) |
| VRAM grows, FPS falls over a long session | Texture collector stuck | `texture_cache.cpp:1438-1470` (candidate #4) |
| Stage A waits a lot | `WaitHostCopies` / `DrainDrawPipe(ReasonConstRam)` | `liverpool.cpp:329, 428-436` |

## 5. Things not to retry

(None yet.)
