# WarVK 1.22.03

[简体中文](README_CN.md) · [Changelog](CHANGELOG.md) · [Map-author API](WarVK/README.md) · [Downloads](https://github.com/CallDisaster/War3VK/releases/latest)

WarVK enhances **Warcraft III 1.27a (32-bit)** using a DXVK-derived D3D9-to-Vulkan backend. It adds directional/point shadows, volumetric lighting, local fog, post-processing, diagnostics and a bounded JASS API, without changing map gameplay.

## Changes in 1.22.03

- **Panel settings are saved.** Anything changed under "built-in render settings" in the Ctrl+F1 panel is written to `WarVK\settings.ini` and restored on the next start. Only touched values are stored; the panel has a "restore defaults" button.
- **Unit portraits are no longer tinted by WarVK lighting.** The sun/ambient rewrite now applies to the world scene only; the portrait model keeps the game's own light instead of getting darker or brighter with the time of day.
- **Shadows and volumetric light no longer draw over the UI while the game is paused or the menu is open.** Paused frames dispatch the world stages through a different path, so the old boundary was never found and the passes ran at frame end. The boundary is now the end of the world scene.
- **Melee games with computer players no longer drop a shadow frame about once a second.** 1.27a re-enters the script runner whenever a JASS thread resumes (melee AI does so several times per second); it was mistaken for a map change and reset the whole shadow session. It now counts as a map change only when the world is not being rendered.
- **Sharper, more visible volumetric light.** Dedicated shadow map 1536→2048 with a tighter near layer (5.5→2.9 world units per texel); the shaft edge guide no longer falls straight to 1/8 at 1440p/4K (now 1/3 and 1/4). Default intensity 1.2→1.5, contrast 2.1→2.5, occluded-shaft darkening cap 24%→36%. Still off by default; about 14 MiB more VRAM when enabled.
- Shader API 1.2.0 and JASS wire `warvk:v1` are unchanged. See the [changelog](CHANGELOG.md) and [1.22.03 notes](docs/RELEASE_1.22.03.md).

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

## Why performance improves

The old wait was **not a fixed 5 ms sleep calculated by Warcraft**. Reading the backbuffer requires prior GPU work to finish; the ordinary native sync path requested that readback without consuming its pixels. Skipping that operation restores CPU/GPU overlap.

A historical same-DLL, matched isolated 2560×1440 A-B-B-A experiment measured mean Present intervals of **13.155 → 7.840 ms**: 40.40% shorter intervals, or 67.78% higher application Present throughput. This is a synchronization-policy experiment, **not a universal v1.21-to-v1.22 FPS benchmark or input-latency measurement**. Actual gains vary by map, camera, GPU and effects.

## Requirements / installation

- Windows 10/11; Warcraft III **1.27a / 32-bit**.
- A GPU and official driver supporting **Vulkan 1.3** plus the extensions this fork uses: NVIDIA 551.61+, AMD Adrenalin 24.2.1+, Intel 32.0.101.6557+ (see `WarVK_GPU_REQUIREMENTS.md`). If start-up fails, WarVK tells you why and which driver to install.
- Only verified executable/Game.dll signatures are supported; unknown layouts are not guessed.

1. Fully exit Warcraft III and the editor; back up any existing `d3d9.dll` separately.
2. Extract **WarVK-1.22.03-win32.zip** and put its DLL beside `war3.exe`.
3. Start normally; `d3d9.log` should report `DXVK: 1.22.03`.
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
- **Excluded:** unfinished Water-branch changes, automatic native-model point-light takeover, the 64-bit renderer and unapproved culling/Consume experiments. Existing explicit author-created point lights remain supported.

## Reports and rollback

If the game does not start, attach `WarVK\Log\startup-error.txt`. Otherwise include map name, fresh-process/map-change sequence, settings, screenshots and performance HTML when available. Logs, runtime status and GPU/crash reports help; remove private data before publishing. Heavy evidence is not required for normal play.

Fully exit before restoring the backed-up DLL. To uninstall, remove WarVK's DLL; logs can be managed separately.

## Development / licensing

Product/JAPI display: **1.22.03**; subsequent fixes retain the 1.22 numbering series. External Shader API remains **1.2.0**, JASS wire remains **warvk:v1**; existing scripts do not need an ABI migration.

Runtime code: `src/d3d9/`; Warcraft integration: `src/d3d9/war3/`; shaders: `subprojects/war3fx/`; author integration: `WarVK/`; tests: `AutoTest/`. Use pinned dependencies and explicit release options, not an old diagnostic build directory. See [release scope](docs/RELEASE_1.22.02.md).

WarVK is GPLv3 at project level: [LICENSE](LICENSE), [COPYING](COPYING), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Dependencies retain their original notices. This is an unofficial project built on DXVK, Dear ImGui, MinHook, Vulkan and Warcraft community research.
