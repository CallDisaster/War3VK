# v1.22 native capture integration checkpoint

Candidate-only semantic port from the preserved performance tree; no Water
changes, no DXVK core synchronization replacements, no player DLL promotion.

## Exact native boundary

Warcraft 1.27a E04D Game.dll: ED080 (whole-function SHA1
9900e819c11894e87d225417a9f79a262d129793) and E65A0
(61922bf50f949c4b2a1b9e85a228653f97fe8f56), normalized only at the previously
audited HIGHLOW relocations. The wrapper changes request=1 to 0 only when
capture=0, nonnested, and the same successful Vulkan Present/world-thread owner
is proven. It still calls the native function exactly once and returns its real
result. Reset/failure revokes readiness; a second owner permanently faults it.
No LockRect, DXVK fence, frame latency, or screenshot pixel consumer is bypassed.

The event callback 175530 has an exact 27-byte digest and accepts event 0x212.
Handled means requested, not saved. Unsupported/unavailable service preserves
native behavior. Full queues explicitly reject rather than blocking a burst.

## Readback and teardown

Three bounded coherent/cached staging slots. Copy-before-signal on the same
DXVK command list, HOST_READ visibility through existing resource barriers,
worker requires a successful exact timeline counter >= the slot's nonzero token
and a healthy device before reading. Elapsed frames never authorize access.
CreateNew .part and non-overwriting rename; actual close/publication precede
saved log. Reset cancels only unsubmitted work. OS module detachment deliberately
abandons the worker owner instead of joining under the loader lock.

Primary specifications:
- https://docs.vulkan.org/guide/latest/synchronization_examples.html#cpu-read-back-of-data-written-by-a-compute-shader
- https://docs.vulkan.org/refpages/latest/refpages/source/vkGetSemaphoreCounterValue.html
- https://docs.vulkan.org/spec/latest/chapters/memory.html

This port preserves that algorithm. Every failed/skipped Present additionally
cancels unsubmitted requests via an owner-thread scope guard; no later-frame
substitution. Ordinary destruction still joins only its own worker; a blocked
filesystem/driver call is not proven impossible by a bounded queue.

## Candidate defaults / test boundary

Unlike the old player launcher experiment, absent ASYNC_SCREENSHOT and
NATIVE_FRAME_SYNC_PERSISTENT/ELIDE variables request the candidate behavior.
Explicit value 0 (and unknown non-1 values) disables the respective service.
There is no recording-owner fallback. This is not a stable-default acceptance.
The candidate must pass fresh isolated 2560x1440 recording-off screenshot,
successful-Present counters, native exit and GPU/backup restoration gates.
Native-model point-light ingestion is separate and NOT provided by this port.

## Console / ReShade distinction

Print used to AllocConsole unconditionally. It is now opt-in only with exact
DXVK_WAR3_DEBUG_CONSOLE=1, C++ thread-safe initialization; debugger/DBWIN remains.
An inherited console is not reconfigured. This removes automatic console
creation, not proof that all game-exit hangs are fixed.

User's War3 test ran A570, whereas overnight B1FCC was in Warcraft III. No
comparison of their raw FPS is accepted as an isolated ReShade effect.
51 local ReShade/config/shader/addon files were moved with hash verification to
E:/Work/WarVK-Backups/war3-reshade-20260914; WarVK A570 preserved. Global DLLs and
registry untouched. ReShadeApps.ini edit was denied by filesystem access and
remains byte-identical. v6.8.0 upstream dll_main.cpp rejects implicit-layer
initialization without a game-local config unless RESHADE_DISABLE_LOADING_CHECK
is set. A future launcher also explicitly disables VK_LAYER_reshade per-process.
No global uninstall is claimed; fresh runtime module absence must be checked.
Source: https://github.com/crosire/reshade/blob/v6.8.0/source/dll_main.cpp

## Runtime checkpoint and exit corrections

All six transactions used one fresh isolated 2560x1440 process, no global input;
restored A570 in War3 and preserved B1FCC in Warcraft III. Artifacts live under
AutoTest/artifacts/v122_native_capture_20260914. R1 failed module inventory, not
evidence of ReShade loading. R2 onward used the retained-HANDLE module snapshot
and verified ReShade absent. Local removal succeeded; global files stay intact.

320F3AC0815BB98C83B2764F83397623BF3E988889C98EAF85AB42F562D73FD1,
35,336,854 bytes: R2 verified no recording, no console, 2658/2658 ordinary sync
calls elided and five 2560x1440 TGA files decoded (single + three-slot burst +
reuse, fourth simultaneous request rejected). These are functional observations,
not native-model lighting, keyboard, whole-frame FPS or product acceptance.

R1/R2/R3 WM_CLOSE alone did NOT prove an exit deadlock: game health continued,
so the in-map confirmation path remained possible. GDB attach and minidump
attempts returned access denied; no usable diagnostic stacks obtained there.
Do not repeat those attempts or infer a destructor stack from them.

R4 added exact stock EndGame(false) via the existing owner-thread internal API,
separately enabled by DXVK_WAR3_INTERNAL_EXIT_TEST=1; no public API/native-table
patch, no keyboard simulation, no forced exit. B306555F...841B3 (35,336,455)
then exited with C0000409. **Its historical receipt ok=true is invalid for normal
exit**: the old runner checked termination but not the exit code. Evidence is
preserved unchanged; current runner explicitly requires exitCode=0.

Control-plane global std::thread was still joinable at process detach, where its
destructor calls terminate. It now has a process-detach-only deleter that abandons
the already OS-terminated thread wrapper; normal explicit shutdown still wakes
and joins it. 42C093629AB351D91E7D0D21EAF642B9334A790B73851FFE34FA9942236854B2,
35,336,455 bytes: R5 recording-off EndGame/WM_CLOSE exited with code 0, no force,
no console/ReShade/GPU event/new dump, exact restore and zero processes. Five
TGA files decoded. Viewed only R5 single-preview.png: real map, coherent UI and
scene; not a claim of native point shadows or a random-flicker visual gate.

R6 recording-on rejected: screenshot/gate passed but exit raised C0000005.
Exact fatal dump under War3/WarVK/Crash, timestamp 2026_09_14_14_45_27_053,
pid11768/tid17836. D3D9 RVA30DA97 is flushThreadCpuDeltas+4E7, reading [EDI+20]
from retired TLS; return RVA30E9CE is shutdown, RVA30F43E is ~War3PerfMonitor.
Independent module preferred-base 62440000 / nm / disassembly and EBP chain
bind that observation to the frozen 42C DLL. Not a GPU event.

Fix candidate: skip the entire process-owned monitor destructor at process
detach (not merely return inside shutdown and still destroy thread/resources),
and place the existing D3D9 detach guard BEFORE WarVK TLS/export/callback work.
Ordinary device destruction retains report/export, shutdown, CS sync, GPU idle
and resource cleanup. Rebuilding/retesting; no normal-exit acceptance yet for it.

Primary lifetime sources:
- https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess
- https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices
- Toolchain lib/gcc/i686-w64-mingw32/15.2.0/include/c++/bits/std_thread.h,
  thread destructor invokes std::__terminate when joinable.

War3/LockDLL.ps1 also deliberately holds d3d9.dll open until Read-Host. Its mere
presence does not prove it was the player's remaining console; left untouched.

## Final limited combination (R7 / R8)

5B705B6B97577C66D8AC96621FAFA1DFFAE216C7CB876A82BB023E32E2684116,
35,336,455 bytes, PE32/i386, exact DLL no-work after the three-edge build.
R7 recording-on / R8 recording-off both passed the corrected exitCode=0 gate,
no forced stop, no new dump/GPU event, exact baseline/player/map preservation.
Both had no ReShade module or attached console, default sync elision and five
decoded TGA outputs. R7's 2553 and R8's 2663 observed ordinary calls were all
elided. Their query-window rates are NOT a whole-frame or foreground benchmark.
121 targeted Python/static tests and three standalone Win32 CPU tests passed;
five Python files compiled. No native model point-light acceptance or full API
regression is implied. The DLL is frozen in each run's candidate.dll but is NOT
delivered as the user's requested native-model-point-shadow candidate.
