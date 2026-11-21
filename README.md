# WarVK 1.30.00

[简体中文](README_CN.md) · [Changelog](CHANGELOG.md) · [Map-author API](WarVK/README.md) · [Downloads](https://github.com/CallDisaster/War3VK/releases/latest)

WarVK enhances **Warcraft III 1.27a (32-bit)** using a DXVK-derived D3D9-to-Vulkan backend. It adds directional/point shadows, volumetric lighting, local fog, post-processing, diagnostics and a bounded JASS API, without changing map gameplay. Since 1.30 the Vulkan renderer runs in a separate 64-bit process by default, which gives the game's own 32-bit address space about 500 MiB back.

## Changes in 1.30.00

- **64-bit rendering (on by default).** DXVK, the GPU driver, shadows and post-processing moved to a separate 64-bit process, `WarVK\x64\warvk-d3d9-host.exe`; the game process keeps only the game and a thin forwarding layer. Usable space in the game's low 2 GiB after loading, same machine and map: Lost Temple 1033 → 1556 MiB, the large custom map 生与死 481 → 1048 MiB — about 230–240 MiB more than the game's own OpenGL mode; with lighting on it runs 1.6–2.1 times as fast as single-process mode (see the performance report below). It turns on by itself when the player package's `WarVK\x64` folder is present; delete or rename that folder to get the single-process mode of 1.22.03 back. If the 64-bit process cannot start, WarVK falls back by itself.
- **Shadows per category.** Units, buildings, doodads, destructables and others can each have casting turned off or their shadow length capped; parts that touch the ground (infantry feet, low fences) no longer cast; shadows no longer vanish wholesale with a low camera and many units; the shadows Warcraft bakes into the ground are removed.
- **Sun disc and the colour of the day.** Drag the sun or lock the time of day in the console. Light colours were retuned: warm light with cool shadows, twilight around sunrise and sunset, bluish moonlight; "real shadow length" now changes only the length.
- **New water (64-bit rendering, on by default on discrete GPUs).** WarVK takes over at the moment the game draws its water: depth tint, shallow-water refraction and caustics, reflections and sky, long waves travelling with the wind with glints on their crests, shore foam; no more large shadows cast from the shore onto deep water. Water style 0 is the game's own water.
- **Fixes.** The Infernal's green light is no longer lit pale yellow (with 64-bit rendering; the single-process mode still tints it). Found and fixed while 64-bit rendering was in testing, never present in 1.22.03's single-process mode: trees, buildings and water glowing under the fog of war and the black mask; smoke and water mist turning into solid blue or black silhouettes; campaign cutscenes and the first frame of a map turning grey-white or blue-white; a black screen without UI after the intro movie.
- **Console.** Render Stats shows a Warcraft address-space bar and the 64-bit process's memory and VRAM; performance logs can be recorded. Frame evidence for catching rare picture faults is off by default; start with `WarVK_EvidenceStart.cmd` from the player package when you need it.
- Shader API 1.2.0 and JASS wire `warvk:v1` are unchanged. See the [changelog](CHANGELOG.md) and [1.30.00 notes](docs/RELEASE_1.30.00.md).

### Earlier in 1.22.03

- **Panel settings are saved.** Anything changed under "built-in render settings" in the Ctrl+F1 panel is written to `WarVK\settings.ini` and restored on the next start. Only touched values are stored; the panel has a "restore defaults" button.
- **Unit portraits are no longer tinted by WarVK lighting.** The sun/ambient rewrite now applies to the world scene only; the portrait model keeps the game's own light instead of getting darker or brighter with the time of day.
- **Shadows and volumetric light no longer draw over the UI while the game is paused or the menu is open.** Paused frames dispatch the world stages through a different path, so the old boundary was never found and the passes ran at frame end. The boundary is now the end of the world scene.
- **Melee games with computer players no longer drop a shadow frame about once a second.** 1.27a re-enters the script runner whenever a JASS thread resumes (melee AI does so several times per second); it was mistaken for a map change and reset the whole shadow session. It now counts as a map change only when the world is not being rendered.
- **Sharper, more visible volumetric light.** Dedicated shadow map 1536→2048 with a tighter near layer (5.5→2.9 world units per texel); the shaft edge guide no longer falls straight to 1/8 at 1440p/4K (now 1/3 and 1/4). Default intensity 1.2→1.5, contrast 2.1→2.5, occluded-shaft darkening cap 24%→36%. Still off by default; about 14 MiB more VRAM when enabled.
- See the [1.22.03 notes](docs/RELEASE_1.22.03.md).

### Earlier in 1.22.02

- **Startup failures now explain themselves.** Previously a Vulkan start-up failure (or a missing `vulkan-1.dll`) made the game quit silently. WarVK now shows a dialog (Chinese) with the reason, every GPU's model/VRAM/current driver version, the vendor's minimum driver and download page, whether the GPU itself is supported, and "delete d3d9.dll to return to stock". The same text is saved to `WarVK\Log\startup-error.txt`. Warcraft III then shows its own DirectX error and exits; that part is the game's behavior.
- **Documented GPU floor.** NVIDIA driver **551.61+** (Maxwell/GTX 750 and newer; Kepler unsupported), AMD Adrenalin **24.2.1+** (RX 5000/RDNA and newer, Ryzen 6000+ iGPUs; Polaris/Vega unsupported), Intel **32.0.101.6557+** (Arc, Core Ultra; Iris Xe listed but unverified). Floors come from published extension lists; only one NVIDIA GPU was tested. See `WarVK_GPU_REQUIREMENTS.md` in the player ZIP.
- **Automatic VRAM preset.** ≤2.5 GB or integrated → low (sun cascade shadow resolution capped at 2048, ~80 MiB instead of ~320 MiB); ≤5.5 GB → medium; otherwise high (unchanged). Override with `VramPreset=low|medium|high` under `[Graphics]` in `WarVK\warvk.ini`. The choice is logged to `WarVK\Log\gpu.txt`.
- **`WarVK_LockD3D9.cmd` / `WarVK_UnlockD3D9.cmd`** stop game platforms or cleaners from deleting/renaming/replacing `d3d9.dll` (NTFS permissions; loading is unaffected). Unlock before updating.
- **Memory/stability.** Low-address domain for Storm `MDLDATAHash` arenas (an old out-of-address error); if that optional part fails to install, the base Storm hooks still install. Per-thread CurrentDraw/material tables are allocated on first use (~12.9 → 3.2 MiB per thread). NetEvent installs only after its JASS prerequisites. Same-process map change requests a shadow-session reset (isolated `ChangeLevel` A→B→A only; still restart per map). Persistent shadow geometry publication is transactional. 32-bit fixes found during x64 work (descriptor sync, condition-variable deadline, Vulkan dispatch, ImGui ownership) are included.
- **Authors.** Unit Walk-loop switch and movement-animation Override helpers. **Weather preview** (12 calls: snowflakes, snow fog, drifting snow), off unless a map calls it; art and per-GPU performance are not accepted yet. Real results via the LocalizedString carrier no longer use exponent notation, which stock `S2R` misread. The experimental map-module loader is **deny-by-default** (requires a local `WarVK\modules\allowed.txt` with name + SHA-256).
- Not included: the 64-bit render host and its dev options (source stays in the repo), unfinished Water, automatic native-model point lights, unapproved culling experiments. Shader API 1.2.0 and JASS wire `warvk:v1` are unchanged.

### Earlier 1.22.01 maintenance update

- **Adaptive memory budgets and idle retirement.** Pool growth checks the actual heap budget, physical backing commitment and 32-bit address-space headroom. Completed Arena generations can gradually release idle tail pages; live resources are not revoked.
- **Snapshot hole reuse.** Retired intervals can be reused after their final CPU/command/GPU consumer releases them, without waiting for every slice in a page to expire. Metadata exhaustion and fragmentation retain conservative fallbacks.
- **Failure cleanup and screenshot memory.** Complete image allocation/registration unwind, preserve small D3D9 allocator tails and reject overflow. Screenshot readback slots are allocated on demand and returned when idle after completion.
- The matching candidate received one positive player test. This is not all-GPU, cross-map, slow-storage shutdown or long-duration certification. See [1.22.01 scope](docs/RELEASE_1.22.01.md).

### Retained improvements from the 1.22 series

- **Remove unnecessary frame-end readback waits.** On the verified native synchronization path without a pixel consumer, CPU preparation can overlap GPU rendering again. Works with performance recording both off and on. Real fences, Present and unsupported-call fallbacks remain intact.
- **Asynchronous screenshots.** Bounded GPU readback slots and exact completion signals allow pixel processing/file output in the background. Native TGA compatibility is retained; elapsed frames are never used as completion proof.
- **Shadow-memory recovery on large maps.** Stage11 position/UV/index snapshots are grouped by retention lifetime. Failed captures no longer renew successful-cache lifetimes; 1.22.01 adds safe interval reuse. Required casters are not discarded to fake recovery.
- **Stretched-shadow input fixes ([#8](https://github.com/CallDisaster/War3VK/issues/8)).** Repair stale palette-slot reuse, group coverage and matrix source/coordinate-space handoffs. Captured bad triangles were independently reconstructed. Fixes are included, not a claim that every intermittent defect is conclusively eliminated.
- **Author controls.** Correct solar-disable semantics and Warcraft 1.27a typed JASS carrier signatures. Existing explicit point lights, Froxel and local fog remain available; unsupported commands do not silently succeed.
- **Working Render Stats.** Ctrl+F1 shows snapshot pages, Arena, allocator budgets, 32-bit address space and shadow completeness. Pool gauges stay valid on cache-hit-only frames. Arena distinguishes retained capacity from last submitted generation usage.
- **Incremental architecture work.** Owner-thread build advancement, generation-bound progress publication, shared palette calculations and selected shader helpers reduce inconsistent handoffs. Long-term refactoring is not claimed complete.

Froxel High, local fog, directional guides and compare-first PCF were already in 1.21; they are retained/improved, not presented as new again. See [CHANGELOG.md](CHANGELOG.md) for details.

## Performance report

Test machine: AMD Ryzen 5 5600X (6 cores, 12 threads), 32 GB RAM, NVIDIA GeForce RTX 4060 Ti 16 GB (driver 621.01), Windows 11 Pro. Every number below comes from isolated automated tests on this machine: the stock 1.27a `war3.exe` (2 GiB address space), windowed (1902×963). The machine was running other work at the same time (about 3.6 logical cores busy on average during the runs, the game included), so a few percent between repeats is normal. Each cell is the median of two rounds; the four modes take turns on the same map.

### WarVK with its lighting on, against the original game

| Map | Mode | Frame rate (median) | p99 frame time | Usable low 2 GiB of the game |
|---|---|---:|---:|---:|
| Lost Temple | Original (its own 64 fps cap) | 64 | 17.3 ms | 1326 MiB |
| Lost Temple | Original (frame limits lifted) | 701 | 2.9 ms | 1326 MiB |
| Lost Temple | WarVK 1.30 single-process | 227 | 7.9 ms | 1033 MiB |
| Lost Temple | **WarVK 1.30 64-bit** | 443 | 4.5 ms | 1556 MiB |
| EchoIsles | Original (its own 64 fps cap) | 63 | 18.5 ms | 1339 MiB |
| EchoIsles | Original (frame limits lifted) | 547 | 4.1 ms | 1337 MiB |
| EchoIsles | WarVK 1.30 single-process | 153 | 10.5 ms | 1040 MiB |
| EchoIsles | **WarVK 1.30 64-bit** | 317 | 6.5 ms | 1565 MiB |
| 生与死 | Original (its own 64 fps cap) | 64 | 22.3 ms | 808 MiB |
| 生与死 | Original (frame limits lifted) | 116 | 14.2 ms | 807 MiB |
| 生与死 | WarVK 1.30 single-process | 43 | 34.2 ms | 481 MiB |
| 生与死 | **WarVK 1.30 64-bit** | 67 | 22.7 ms | 1048 MiB |

- **Original**: no WarVK file in the game folder, no lighting effects. The original Direct3D mode cannot start on an isolated desktop (the game reports it could not initialize DirectX), so the game's own OpenGL mode (`-opengl`) is used.
- **The original's own frame limits.** In OpenGL mode the original runs at exactly 64 frames a second. That is the game waiting, not the machine running out of breath: its main loop schedules the next frame by `GetTickCount`, which only moves every 15.6 ms (asking for a 1 ms timer does not change it); with that lifted, the NVIDIA OpenGL driver still waits for vertical sync (the game never sets a swap interval, and OpenGL's default waits).
- **Original (frame limits lifted)**: only those two, nothing else — the game process asks for a 1 ms timer, Game.dll's `GetTickCount` reads the 1 ms `timeGetTime` instead, and on its first present the swap interval is set to 0 (no vertical sync). The game's own `maxfps` setting (1000 on this machine) is left as it is. This is done in the test instance only; the WarVK product does none of it.
- **WarVK**: the defaults a player gets. Sun light and real-time shadows, Bloom, post-processing, the day/night light and the occluded-unit outline are on, plus the new water in 64-bit mode; SSAO, volumetric light and point lights are off by default and stayed off. WarVK does not wait for vertical sync by default. "Single-process" is what you get after removing `WarVK\x64` (the same as 1.22.03).
- **One ruler for all four**: every frame rate comes from the presents Windows' own kernel records (DxgKrnl Present events), cut to the same stretch of each run; WarVK's own frame counter is a cross-check.
- **How to read the table**: with lighting on, 64-bit mode runs Lost Temple and EchoIsles at 443 and 317 fps, 5–7 times the original's own 64 fps cap; on the large map 生与死 it runs at 67 fps, a little above the cap, with a p99 frame time (22.7 ms) close to the original's (22.3 ms). Against the original with its frame limits lifted, the lighting costs about 40% of the frame rate (the original draws no real-time shadows, Bloom, post-processing or new water). Single-process mode runs 生与死 at only 43 fps, below the original; 64-bit mode is 1.6–2.1 times as fast as single-process mode. The tool is `AutoTest/run_v130_vanilla_compare.py`; the numbers of every run are in [the 1.30.00 notes](docs/RELEASE_1.30.00.md).

### The 64-bit migration

**Memory.** 32-bit Warcraft III has 2 GiB of address space; large maps and HD asset packs fill it and the game crashes. 1.30 moves DXVK, the GPU driver, shadows and post-processing into a separate 64-bit process, and the game process keeps only the game and a thin forwarding layer:

- The table's last column: 64-bit mode leaves 520–570 MiB more usable space than single-process mode, and 230–240 MiB more than the original game in OpenGL mode. The Vulkan/driver images (about 132 MiB), the driver's and DXVK's memory (about 230 MiB), a 64 MiB shared view and 31 threads that single-process mode puts in the game process are all gone from it in 64-bit mode.
- **Gemini's HD asset pack stress test** (StormTide v0.40 HD, nine packs totalling 754 MiB, 生与死 for 170 s): single-process mode crashed at 78.8 s (461 MiB usable, largest hole 113 MiB just before); the original game in OpenGL mode survived with only 448 MiB left; 64-bit mode ran through and still had 707 MiB at the end. Report: `docs/research/2026-11-18-x64-stormtide-beta-memory-comparison.md`.

**Frame rate.** Forwarding across processes has a cost: the first 64-bit build that ran all three maps (early 11-18) was 5–16% slower than single-process mode — Lost Temple 209 / 198 fps, EchoIsles 152 / 145, 生与死 52.0 / 44.7. Two threads have to run flat out each frame: the game thread encoding draw commands, and the 64-bit process decoding and executing them. This round cut the per-frame CPU on both sides:

| Optimization | What changed | Measured effect |
|---|---|---|
| Present pipeline | The 64-bit process waits only for the previous frame at Present, not for this one to finish on the GPU | Lost Temple about 160 → 250 fps; each Present 3.0 → 0.5 ms |
| Incremental encoding | The game side rewrites only the sections that differ from the previous draw (same wire format) | Game thread CPU per frame 22.8 → 20.2 ms (生与死) |
| Incremental decoding | The 64-bit side decodes only the changed sections | 64-bit process CPU per frame 17.0 → 15.6 ms |
| Delta encoder | Compares four words at a time (SSE2) and touches only the changed ones | 0.98 → 0.26 µs per record |
| System calls off the hot path | Reading its own memory uses `IsBadReadPtr + memcpy`; the blocker query runs once per object per frame | Those two system calls on the game thread 10.8% / 10.5% → 3.0% / 4.3% |
| Transform cache | No 4×4 inverse when world / view did not change | The top hotspot of the 64-bit execute thread (10–13%) is gone |
| Leaner shadow copies, early culling | Copy only the vertex data shadows use; skip objects entirely outside a cascade | 生与死 per-frame copy 6.44 → 2.59 MiB; objects drawn into shadows 2252 → 1899 |
| Readback buffers | CPU-cached memory | Each capture 7–9 ms → 0.2 ms |

After this work (11-18 evening) 64-bit mode was faster than single-process mode: Lost Temple 200 → 350 fps, EchoIsles 144 → 318, 生与死 47 → 72, with p99 frame times 9.4 → 6.5, 12.3 → 6.4 and 34.5 → 22.0 ms (both modes alternating in one test). The lighting-on table above was measured again on the 1.30.00 release build, with the shadow categories, the new water and the new light colours.

**The new water** (64-bit mode, GPU time): Gemini measured 0.29–0.32 ms a frame for a full-screen sea and 0.045–0.062 ms for the pool on the StormTide test map. Merging Gemini's fourth water version made it about 60% more expensive in an alternating comparison (shallow beach 0.30 → 0.51 ms, steep shore 0.22 → 0.36 ms), so the shader's registers were brought from 72 down to 64 (the game's own fixed-function pipeline uses 48) and a batch of special functions removed; the 1.30.00 water averages 0.34 ms (steep shore) and 0.48 ms (beach) a frame at the two test spots. Water style 0 is the game's own water, for anyone who does not want the new one.

### 1.22's synchronization fix (history)

The old path did not "wait a fixed 5 ms per frame"; reading back the back buffer had to wait for the related GPU work, and ordinary frames did that without consuming the pixels. Dropping consumer-less readbacks restored CPU/GPU overlap and pipeline throughput; it does not mean shaders do 40% less work. In a historical same-DLL, same-scene, isolated 2560×1440 A-B-B-A run, the mean Present interval went **13.155→7.840 ms** (40.40% shorter, 67.78% more application Present throughput). This is a synchronization-policy comparison, not a general FPS comparison between versions and not an input-latency measurement.

## Requirements / installation

- **64-bit** Windows 10/11; Warcraft III **1.27a / 32-bit**.
- A GPU and official driver supporting **Vulkan 1.3** plus the extensions this fork uses: NVIDIA 551.61+, AMD Adrenalin 24.2.1+, Intel 32.0.101.6557+ (see `WarVK_GPU_REQUIREMENTS.md`). If start-up fails, WarVK tells you why and which driver to install.
- Only verified executable/Game.dll signatures are supported; unknown layouts are not guessed.

1. Fully exit Warcraft III and the editor; back up any existing `d3d9.dll` separately.
2. Extract all of **WarVK-1.30.00-win32.zip** into the folder of `war3.exe`: `d3d9.dll` and the `WarVK` folder (with `x64` inside) should sit right beside `war3.exe`.
3. Start normally. `warvk-d3d9-host.exe` under Task Manager's Details means 64-bit rendering is on; each start's logs are in `WarVK\logs`.
4. Press **Ctrl+F1** for settings and Render Stats. Normal optimizations do not require recording.

Do not use old experimental launch scripts for normal play. Environment variables are an internal diagnostic surface and can override defaults; supported player/author entry points are the panel and documented JAPI. Heavy frame evidence is **off by default**. Do not install conflicting D3D9 proxy DLLs.

The separate **author-kit** ZIP contains JASS/YDWE integration, not a runtime DLL loader. Players do not need it. No game-executable, map or MPQ change is required.

## Reading memory statistics

- **Snapshot pool:** adaptive quota up to 512 MiB by default with trusted driver budget data; otherwise 384 MiB fallback. Existing explicit caps still constrain it. This is neither preallocation nor the GPU's total VRAM limit.
- **Arena retained capacity:** at most 384 MiB per generation and 1152 MiB total; actual growth quota can be lower. Completed generations gradually shed idle tail pages. DXVK/driver backing caches may release later; a full capacity bar alone is not allocation failure.
- **Last growth quota:** the most recent growth decision, not continuous VRAM sampling. GPU headroom and 32-bit VA can both block growth; extreme working sets can still exhaust available space.
- **Last submitted generation usage:** bytes used before resetting that generation's cursor, not proof of GPU completion or visible geometry size.
- Cache references overlap pool backing; do not add them as independent allocations. Sources may represent different frames. VA means address space, not physical RAM or VRAM.

## Known boundaries

- **Restart the game for every map.** Same-process map changes remain outside release validation ([#6](https://github.com/CallDisaster/War3VK/issues/6)).
- Finite budgets can still reject genuinely excessive active working sets. Incomplete candidates must not replace complete shadow maps.
- Issue #8 fixes have code/input and stage-specific player evidence, not proof that all intermittent shadows or historical crashes are solved.
- Launching with `-opengl` (some map tools/launchers force it) means Warcraft III never loads `d3d9.dll`, so WarVK is inactive.
- Worst-case 1440p/4K Froxel requests may safely fall back to Legacy. Fine foliage and point-shadow surfaces can still need map-specific assessment.
- Not every JASS wrapper is implemented or exhaustively runtime-tested. Check capability/error results and [author notes](WarVK/README.md).
- **64-bit rendering limits.** Keep anti-aliasing (MSAA) off. If the 64-bit process fails while playing, the game exits; it cannot switch back to the single-process mode mid-game. Alt-Tab, resizing by dragging, restoring from minimized and changing display scaling while running have no automated tests. Full screen supports only the primary display's current resolution; windowed mode is the safer start. The picture is slightly brighter than in the single-process mode and shadows tear now and then; neither cause is found yet. Water reflects only what is on screen.
- The first map after installing may stall for a few seconds while the driver compiles the shadow shaders for the first time; later starts do not. When a map fades through white or another colour, shadows float above that colour.
- Isolated automated tests ran on a single NVIDIA RTX 4060 Ti; this formal build has no player acceptance yet.
- **Excluded:** automatic native-model point-light takeover and unapproved culling/Consume experiments. Existing explicit author-created point lights remain supported.

## Reports and rollback

If the game does not start, attach `WarVK\Log\startup-error.txt`. Otherwise include map name, fresh-process/map-change sequence, settings, screenshots and performance HTML when available. Logs, runtime status and GPU/crash reports help; remove private data before publishing. Heavy evidence is not required for normal play.

Fully exit before restoring the backed-up DLL. To stop using 64-bit rendering for now, delete or rename `WarVK\x64`. To uninstall, delete `d3d9.dll` and the `x64`, `logs`, `cache` and `Evidence` folders inside `WarVK` (`Evidence` may hold evidence packages or performance logs you have not sent yet).

## Development / licensing

Product/JAPI display: **1.30.00**; subsequent fixes retain the 1.30 numbering series. External Shader API remains **1.2.0**, JASS wire remains **warvk:v1**; existing scripts do not need an ABI migration.

Runtime code: `src/d3d9/`; Warcraft integration: `src/d3d9/war3/`; shaders: `subprojects/war3fx/`; author integration: `WarVK/`; tests: `AutoTest/`. Use pinned dependencies and explicit release options, not an old diagnostic build directory. See [release scope](docs/RELEASE_1.30.00.md).

WarVK is GPLv3 at project level: [LICENSE](LICENSE), [COPYING](COPYING), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Dependencies retain their original notices. This is an unofficial project built on DXVK, Dear ImGui, MinHook, Vulkan and Warcraft community research.
