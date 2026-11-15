---
name: hang-crash-capture
description: Capture and read where a hung or crashed MediEvil runtime is stuck - cdb non-invasive stacks and minidumps of the live process, symbolizing runtime frames with llvm-symbolizer, the fault address from an exception record, and the runtime's own crash dumps. Use whenever the game hangs (black screen, frozen) or crashes, before the user closes it.
---

# Hang and crash capture

## A hang: capture before anyone closes it

UE4's watchdog turns a render-side hang into a crash after 120 s ("GameThread timed out waiting for RenderThread").
Ask the user to leave the game open; capturing is read-only and safe.

1. The game's PID: `tasklist //FI "IMAGENAME eq medievil-runtime.exe"`.
2. Non-invasive attach (suspends threads briefly, then detaches and leaves the game as it was), from PowerShell:
   ```
   $sp = '<scratchpad>'; $cdb = 'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
   & $cdb -pv -p <pid> -lines -c ".dump /o /mhtu $sp\hang.dmp; ~*k 40; qd" > "$sp\hang-stacks.txt"
   ```
   ~1,700 lines; thread names are in the `Id:` lines (`"gow3:VkRecorder"`, `"shadPS4:GpuCommandProcessor"`,
   `"gow3:DrawRec"`, `"RHIThread"`, `"RenderThread 1"` ...). Keep the stacks of a real finding in
   `research/docs/data/` (as `hang-200pct-stacks-20261115.txt`).
3. Symbolize the runtime frames (`medievil_runtime+0x...`; the exe is clang/DWARF, cdb cannot):
   ```
   /c/msys64/clang64/bin/llvm-symbolizer.exe --obj=out/medievil-runtime.exe -C --no-inlines --pretty-print 0x<0x140000000 + offset>
   ```
   (image base 0x140000000: `llvm-objdump -p out/medievil-runtime.exe | grep ImageBase`). Use the exe that ran;
   if `out\` was rebuilt since, use the saved copy (`C:\Users\Ty\MediEvil\baselines\...`). Frames at `0x00000008`
   + offset are guest (eboot) code: offset = address - 0x800000000.
4. Read it as a cycle: for each key thread, what it waits on and who must release it. Stage A =
   `Liverpool::ProcessCommands` / `DrawPipe::Drain`; stage B = `DrawPipe::Run` / `WaitDeferredSignals`;
   recorder = `Scheduler::RecorderThread`; `SendCommand<true>` = a thread waiting for stage A to run its lambda;
   `SyncRecording` = waiting for the recorder.

## The fault behind a fault-handler wait

A thread whose stack runs `ntdll!KiUserExceptionDispatch` -> `vectored_fault` -> `GuestFaultSignalHandler` took a
page fault. The CONTEXT is at that frame's Child-SP and the EXCEPTION_RECORD at Child-SP + 0x4f0:
```
& $cdb -z "$sp\hang.dmp" -c ".exr <childsp>+0x4f0; .cxr <childsp>; r rsi; r rdi; r rcx; r r8; .cxr; q"
```
`Parameter[1]` is the faulting address; for `memcpy_repmovs`, rsi/rdi/rcx are source / destination / bytes left.

## Crashes

- The runtime writes `user\crashdumps\medievil-<time>-<pid>.dmp` (`GOW3_CRASH_DUMP`: off `0`, `full`) and a
  `Fault:` block with guest frames in the run log (native-diagnostics skill).
- A player's report: their `last_run.log` is overwritten by the next launch; ask for `last_run.1.log` too, and
  check the log's `Render resolution:` line matches what they describe (two "wrong logs" on 2026-11-15).
- Third-party launchers (e.g. Quiver) wrap the port: their paths differ, the log format is the same.

## Pitfalls

- Never capture with a full-memory dump by default (gigabytes); `/mhtu` (handles, threads, unloaded modules,
  stacks) was enough for every hang so far.
- A crash can hide a different hang: on 2026-11-15 the same 200% setting hung (a deadlock) in one run and ran out
  of framebuffer memory in the next. Read each run's own log before assuming.
