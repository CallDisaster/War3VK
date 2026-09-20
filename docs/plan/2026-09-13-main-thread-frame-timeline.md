# Main-thread wall attribution / GameMainLoop diagnostic

User authorizes source, exact DLL build, controlled deployment and AutoTest on
2026-09-13. Goal: locate the unclassified wall time, not assume a 4.515 ms CPU loss.

## Contract

- Preserve existing dirty worktree and all old reports. Build exact DLL BelowNormal/-j2.
- Live baseline is the bytes actually checked at start (139BBE8C...5FD2), not an
  older 055F/Water candidate. CreateNew backup, identity-conditional deployment,
  owned-process stop and hash-conditional restoration after each fresh process.
- Use E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x (SHA
  11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF).
  Do not use the older high-pressure map. Disable AutoTest background throttle
  and automatic game pause explicitly, without altering machine environment.
- No resource/gameplay optimization in the instrumentation patch. Diagnostic
  default off; no trace disk I/O from a hot hook. No added global input.
- One selected swapchain/TID, fixed bounded ledger, QPC intervals split at
  Present entry and legacy begin/archive boundaries. Keep nested native scopes
  across frame epochs. Report exclusive buckets separately from inclusive ones.
- A long-lived Engine scheduler entry is not one frame. Existing verified phase
  hooks provide initial subdivisions; no guessed mid-function detour.
- Unknown outside scopes stays explicit. Wall includes waits/preemption;
  GetThreadTimes is only a window aggregate. ETW CPU Sampled + scheduling is the
  independent check, never sum parallel threads or GPU into the main wall.
- No claims that synthetic closure equals complete semantic attribution, nor
  that the new run reproduces the player's exact camera/workload automatically.
- System trace stays local. Check that no other WPR session is running; stop only
  this task's trace. Preserve failed/partial artifacts and restore before next run.
- Updated source report must parse recursively without duplicate keys. Old
  duplicate reports stay invalid for strict acceptance, usable only for clearly
  labelled unique-field diagnostics.

## Primary references

- https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis
- https://learn.microsoft.com/en-us/windows-hardware/test/wpt/stackwalk
- https://github.com/lucko/spark

First establish co-timed major phases and OS scheduling; then subdivide the
largest measured remainder. No stable promotion or unmeasured FPS claim.

## User correction: isolation and 2560x1440 mandatory

All subsequent runs MUST use a noninteractive isolated desktop, windowed client
2560x1440, verified after ready. No SwitchDesktop/global input/foreground fallback.
Initial default-desktop runs are preliminary attribution evidence, not matching
the requested baseline. Isolated timing remains diagnostic, not player-front FPS.

WPR CPU start failed 0xc5585011: current token lacks SeSystemProfilePrivilege.
No security policy changes. Alternative is a bounded external WOW64 wall-stack
sampler: owned launch HANDLE (OpenProcess VM_READ is denied after game startup),
verified target TID, suspend/context/4KB stack copy/resume in finally, no injected
callbacks or I/O while suspended. This is perturbing wall sampling, NOT ETW CPU
Running/Ready/Waiting; report pause durations and incomplete EBP chains. Selftest
on an owned disposable WOW64 process passes. Original failed sampler runs kept.

## Current checkpoint

R9/R10 now have actual backbuffer (not only client rectangle) 2560x1440 proof.
R8 crash stack implicated optional camera.snapshot native resolution; subsequent
runs preserve the map camera and issue no JASS camera request. R7 interruption
was separately settled, including exact DLL and temporary video-setting restore.
BackBuffer LockRect/readback GPU wait is the measured largest omitted boundary;
do not remove synchronization or falsify LockRect success. See
[measured attribution and run ledger](../research/2026-09-13-main-thread-backbuffer-readback-attribution.md).
