# WarVK 1.22.00

[简体中文](README_CN.md) · [Changelog](CHANGELOG.md) · [Map-author API](WarVK/README.md) · [Downloads](https://github.com/CallDisaster/War3VK/releases/latest)

WarVK enhances **Warcraft III 1.27a (32-bit)** using a DXVK-derived D3D9-to-Vulkan backend. It adds directional/point shadows, volumetric lighting, local fog, post-processing, diagnostics and a bounded JASS API, without changing map gameplay.

## Highlights in 1.22.00

- **Remove unnecessary frame-end readback waits.** On the verified native synchronization path without a pixel consumer, CPU preparation can overlap GPU rendering again. Works with performance recording both off and on. Real fences, Present and unsupported-call fallbacks remain intact.
- **Asynchronous screenshots.** Bounded GPU readback slots and exact completion signals allow pixel processing/file output in the background. Native TGA compatibility is retained; elapsed frames are never used as completion proof.
- **Shadow-memory recovery on large maps.** Stage11 position/UV/index snapshots are grouped by retention lifetime, reducing small long-lived entries pinning pages of expired short-lived data. Failed captures no longer renew successful-cache lifetimes. The default **384 MiB pool budget is unchanged**; required casters are not discarded to fake recovery.
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
- A GPU and current official driver supporting **Vulkan 1.3**. Vulkan 1.2 alone is insufficient.
- Only verified executable/Game.dll signatures are supported; unknown layouts are not guessed.

1. Fully exit Warcraft III and the editor; back up any existing `d3d9.dll` separately.
2. Extract **WarVK-1.22.00-win32.zip** and put its DLL beside `war3.exe`.
3. Start normally; `d3d9.log` should report `DXVK: 1.22.00`.
4. Press **Ctrl+F1** for settings and Render Stats. Normal optimizations do not require recording.

Do not use old experimental launch scripts for normal play. Environment variables are an internal diagnostic surface and can override defaults; supported player/author entry points are the panel and documented JAPI. Heavy frame evidence is **off by default**. Do not install conflicting D3D9 proxy DLLs.

The separate **author-kit** ZIP contains JASS/YDWE integration, not a runtime DLL loader. Players do not need it. No game-executable, map or MPQ change is required.

## Reading memory statistics

- **Snapshot pool:** default 384 MiB, separate from Arena; not the GPU's total VRAM limit.
- **Arena retained capacity:** reusable GPU buffers, capped at 384 MiB per generation and 1152 MiB total. Pages currently do not shrink automatically when camera pressure falls. A full capacity bar alone is not allocation failure.
- **Last submitted generation usage:** bytes used before resetting that generation's cursor, not proof of GPU completion or visible geometry size.
- Cache references overlap pool backing; do not add them as independent allocations. Sources may represent different frames. VA means address space, not physical RAM or VRAM.

## Known boundaries

- **Restart the game for every map.** Same-process map changes remain outside release validation ([#6](https://github.com/CallDisaster/War3VK/issues/6)).
- Finite budgets can still reject genuinely excessive active working sets. Incomplete candidates must not replace complete shadow maps.
- Issue #8 fixes have code/input and stage-specific player evidence, not proof that all intermittent shadows or historical crashes are solved.
- Worst-case 1440p/4K Froxel requests may safely fall back to Legacy. Fine foliage and point-shadow surfaces can still need map-specific assessment.
- Not every JASS wrapper is implemented or exhaustively runtime-tested. Check capability/error results and [author notes](WarVK/README.md).
- **Excluded:** unfinished Water-branch changes, automatic native-model point-light takeover, the 64-bit renderer and unapproved culling/Consume experiments. Existing explicit author-created point lights remain supported.

## Reports and rollback

Include map name, fresh-process/map-change sequence, settings, screenshots and performance HTML when available. Logs, runtime status and GPU/crash reports help; remove private data before publishing. Heavy evidence is not required for normal play.

Fully exit before restoring the backed-up DLL. To uninstall, remove WarVK's DLL; logs can be managed separately.

## Development / licensing

Product/JAPI display: **1.22.00**, with later fixes numbered **1.22.01** etc. External Shader API remains **1.2.0**, JASS wire remains **warvk:v1**; existing scripts do not need an ABI migration.

Runtime code: `src/d3d9/`; Warcraft integration: `src/d3d9/war3/`; shaders: `subprojects/war3fx/`; author integration: `WarVK/`; tests: `AutoTest/`. Use pinned dependencies and explicit release options, not an old diagnostic build directory. See [release scope](docs/RELEASE_1.22.00.md).

WarVK is GPLv3 at project level: [LICENSE](LICENSE), [COPYING](COPYING), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Dependencies retain their original notices. This is an unofficial project built on DXVK, Dear ImGui, MinHook, Vulkan and Warcraft community research.
