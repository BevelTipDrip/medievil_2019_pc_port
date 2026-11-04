// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 GOW3_DECODE_PACING=1 (research/docs/impl-decode-pacing.md): Bloodborne 0.7's `decode`
// submission pacing (bloodborne_pc 0fc2921, gpu/platform/gnm/gnm_driver.cpp; first ported for
// GOW3_GPU_FENCES in gow3_gpu_fences.h) adapted to the copy model, where a fence is done once
// the draw recording thread (stage B) and the label writes it deferred have passed it, not the
// GPU.
//
// The console's lock (sceGnmSubmitDone arms it, the next submission waits for GPU idle) and the
// GPU command thread's drain at every idle make the game wait for stage B twice a frame. With
// the switch:
// - GPU idle is a step stage B runs in stream order after the label writes before it (stage A
//   does not drain); it raises the interrupt only if nothing was submitted meanwhile.
// - sceGnmSubmitDone arms no lock. A frame (one sceGnmSubmitDone to the next) lets its first
//   GOW3_SUBMIT_AHEAD submissions through; later ones, the flip submission and
//   sceGnmSubmitDone wait until the game is fewer than GOW3_DECODE_PACING_LEAD frames ahead of
//   the newest retired frame. Retired: all its submissions decoded, then stage B and the
//   deferred label writes passed its end (a deferred signal, Scheduler::SignalAfterHostCopies).
// Off by default; ignored with GOW3_GPU_FENCES=1 (which has its own GPU-ordered pacing).
#pragma once

#include <cstdint>
#include <string>

namespace Gow3DecodePacing {

/// GOW3_DECODE_PACING=1 and not GOW3_GPU_FENCES=1 (read once; logs the setting once).
bool Enabled();
/// GOW3_DECODE_PACING_LEAD (default 1, at most 2): frames the game may be ahead of the newest
/// retired frame.
unsigned Lead();
/// GOW3_SUBMIT_AHEAD (default 2, as GOW3_GPU_FENCES): a frame's submissions that do not wait.
unsigned SubmitAhead();

// ---- Pacing (gnmdriver.cpp; the guest's submitting threads, under its submission lock) ----

/// sceGnmSubmitDone: waits for the lead, then returns the new frame's number (1, 2, ...) for
/// Liverpool::SubmitDone.
std::uint64_t BeginFrame();
/// A submission (graphics or compute) was handed to the command processor. `flip`: it carries
/// the frame's flip (sceGnmSubmitAndFlipCommandBuffers); it waits for the lead even among the
/// first SubmitAhead() (the guest rotates its label free list and pool slot right after it).
void AfterSubmit(bool flip);

// ---- The GPU command thread (stage A) and the steps it hands to stage B ----

/// Stage A handed a frame's retire step to stage B (frame `frame`'s submissions all decoded).
/// `decoded_ns`: when (NowNs()), for the report.
void NoteFramesRetired(std::uint64_t frame, std::uint64_t decoded_ns);
/// Stage A handed a GPU idle step to stage B instead of draining it.
void NoteIdleDeferred();
/// That step ran (after stage B and the label writes before it): `raised` GPU idle, or skipped
/// it because something was submitted meanwhile. `handed_ns`: when stage A handed it over.
void NoteIdleRun(bool raised, std::uint64_t handed_ns);

std::uint64_t NowNs();

/// What the GPU command thread is at (set by Liverpool; for the watchdog).
inline std::string (*describe_gpu)(void* context) = nullptr;
inline void* describe_context = nullptr;

/// The report lines for the 5 s block (videoout/driver.cpp, GOW3_FRAME_STATS=1); empty with the
/// switch off. Resets the counters. `frames`: frames presented in the window.
std::string Report(double window_s, unsigned frames);

} // namespace Gow3DecodePacing
