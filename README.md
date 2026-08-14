# WarVK 1.21.00

![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)
![Vulkan](https://img.shields.io/badge/Vulkan-1.3+-red)
![Warcraft III](https://img.shields.io/badge/Warcraft%20III-1.27a-gold)

[简体中文](README_CN.md) · [Changelog](CHANGELOG.md) · [WarVK JAPI](WarVK/README.md)

WarVK is a graphics enhancement runtime for Warcraft III 1.27a. It uses a DXVK-derived D3D9-to-Vulkan backend and adds modern directional and point shadows, volumetric lighting and local fog, post-processing, diagnostics, and an author-facing JASS API.

Version 1.21.00 is a new feature release in the 1.x series. It focuses on removing duplicate shadow-producer work, fixing Stage11 static-caster flicker, enforcing terminal behavior after Vulkan device loss, and integrating the new bounded Froxel volumetric renderer.

> [!IMPORTANT]
> Update to the latest official NVIDIA, AMD, or Intel graphics driver before installing. The runtime requests **Vulkan 1.3**, which includes Vulkan 1.2; a driver that exposes only Vulkan 1.2 is not sufficient for this build.

> [!NOTE]
> WarVK prioritizes image quality and resource-lifetime correctness. 4096 CSM, point shadows, Froxel volumetrics, and full post-processing all have a real CPU/GPU cost.

## Version numbering

Starting with 1.21.00, releases use `1.minor.fix`:

- `1.x` identifies the current major architecture series;
- `1.21` identifies feature release 21;
- the final `00` is the first stable build in that feature line; fixes follow as `1.21.01`, `1.21.02`, and so on;
- the next grouped feature/performance release will become `1.22.00`.

The previous `1.2003` release used the old numbering scheme. The external Shader API numeric version remains `1.2.0`, and the JASS wire protocol remains `warvk:v1`; existing map integrations do not need to migrate for this product-version change.

## Highlights in 1.21.00

### Shadow correctness and image quality

- Directional CSM now uses compare-first PCF with a fixed symmetric kernel, per-tap receiver-plane depth correction, and cascade-parity alpha cutoffs. This removes known periodic sampling patterns and silhouette changes at cascade transitions.
- The Stage11 exact producer protects the current active static working set and publishes an explicit producer-completeness contract. High-pressure scenes no longer lose an entire set of casters because the cache evicted its own visible working set.
- Warcraft Transparent Type0 construction attachments use the child model's current draw, exact VB/IB/UV data, and complete matrix palette. Player testing confirmed continuous Undead and Night Elf construction-animation shadows.
- Point shadows now use a consistent radial receiver-depth domain and corrected bias. Player testing confirmed the former ground/unit moiré and continuous banding no longer reproduce.
- Replay resolves logical buffer bindings immediately before Vulkan command recording. Defrag or relocation cannot leave a draw bound to a capture-time physical `VkBuffer`.
- CSM depth, matrices, resource generation, and receiver parameters are published as one complete bundle; an incomplete candidate never replaces a complete shadow map.

### Volumetric lighting and local fog

- Adds bounded local volumetric fog with up to eight Sphere, Box, or Cylinder regions, independently composable with the global medium.
- Adds Froxel Medium and Froxel High. High is the default volumetric backend and uses full-view logarithmic Z slicing, scene-depth termination, and bounded optical integration.
- Directional volumetric shadows use separate base/refined guides and full-resolution scene-depth reconstruction to preserve small-caster shafts without treating every low-resolution effect edge as geometry.
- Froxel image creation and layout publication are transactional. Unsupported formats, missing proof, or excessive work cause a complete fallback to Legacy rather than unbounded GPU submission. 1080p defaults are admitted; worst-case 1440p/4K requests may safely fall back.
- Map authors can control volumetric lighting, global height fog, and local fog regions through the existing bounded JAPI contract.

### Stability and diagnostics

- Once a Vulkan logical device reaches `VK_ERROR_DEVICE_LOST`, D3D9 Reset/ResetEx, submission, Present, frame workers, and pipeline compilation stop issuing new GPU work on that device. CPU-side retirement completes with device-removed semantics.
- Direct driver results are distinguished from synthetic fail-stop state. When supported, one bounded `VK_EXT_device_fault` text snapshot is captured outside submission drain; vendor binaries are not collected.
- Shadow Arena remains capped at 64 MiB pages, 384 MiB per generation, and 1.125 GiB total. Transactional reservation, GPU fences, map/device epochs, and final replay validation remain fail-closed.
- Performance history is capped at 4000 frames to protect Warcraft III's 32-bit address space while cumulative workload/error counters still cover the full run.
- Release builds compile out legacy `warvk:cmd` and unapproved Consume/development-observer routes; environment variables cannot bypass the release freeze.

### CPU performance

- Exact-owner publication removes records already completed by an earlier producer before DirectGrouped performs expensive packet construction. Resolved replay tables are no longer copied again for each consumer.
- In a matched A-B-B-A scene, main-thread CPU time improved from `6.135 ms` to `5.778 ms` (`-0.357 ms / -5.82%`); Populate improved by `76.27%`, DirectGrouped by `87.42%`, and BuildEligible by `96.38%`.
- These measurements establish a producer-side CPU improvement, not a universal FPS guarantee. Volumetrics, GPU workload, map content, and foreground state still determine final frame rate.
- Early union culling, Persistent Package, ReBAR, CPU-MT skinning, and Canonical Queue Takeover remain below their release gates and are disabled in 1.21.00.

See [CHANGELOG.md](CHANGELOG.md) for the full change history and [WarVK/README.md](WarVK/README.md) for the map-author API.

## Requirements

- Windows 10 or Windows 11
- A GPU and current official driver supporting Vulkan 1.3 or newer
- Warcraft III 1.27a (32-bit)

WarVK targets the classic 1.27a executable and verified `Game.dll` layouts only. Unknown signatures are rejected safely rather than scanned heuristically.

## Installation

1. Back up the Warcraft III directory, especially any existing `d3d9.dll`.
2. Copy `d3d9.dll` from the player package next to `war3.exe`.
3. Start the game and confirm that `d3d9.log` reports `DXVK: 1.21.00`.
4. Press `Ctrl + F1` to show or hide the WarVK settings panel.

WarVK is not an in-map DLL loader. Only files explicitly included in the player package belong in the game directory; the JASS/YDWE author package is for map development.

## Common controls

- Frame-rate unlock and post-processing
- Directional CSM, filtering, and TAA mode
- Point lights and point shadows
- Legacy / Froxel Medium / Froxel High volumetric backend
- Global height fog and local fog regions
- Anti-aliasing, bloom, exposure, and outlines

## Known boundaries in 1.21.00

- Leaving a map and loading another in the same Warcraft III process has not completed release validation and may still cause persistent performance loss, shadow corruption, or resource-lifetime issues. Fully exit and restart Warcraft III before loading another map. Tracking remains open in [#6](https://github.com/CallDisaster/War3VK/issues/6).
- Issue #5 early union culling remains a disabled development-observer path. Version 1.21.00 removes confirmed duplicate producer work and active-cache thrashing, but does not claim that terrain/static/skinned front-end culling is complete.
- Worst-case 1440p/4K Froxel requests may fall back to Legacy through bounded admission. This is a stability policy, not a GPU or driver failure.
- Extremely fine foliage, grass, or distant alpha silhouettes may retain minor subpixel motion. This release removes known periodic rotation, incorrect depth filtering, and cross-cascade alpha differences without hiding residual motion behind unconditional TAA history.
- Isolated-desktop results are used only for stability and relative A/B measurements. Foreground FPS should be compared at the same map, camera, and settings.

## Troubleshooting and reports

- A black screen at startup usually indicates an outdated Vulkan driver or a conflicting third-party `d3d9.dll`.
- For poor performance, reduce or disable volumetrics first, then reduce point-shadow count and post-processing. Directional CSM stays at 4096 unless allocation safely latches the fallback.
- Do not copy multiple D3D9 proxy DLLs into the same game directory.
- Useful reports include `d3d9.log`, `runtime_status.json`, GPU incident JSON, and WarVK crash dumps. Remove personal paths or private map data before publishing them.
- Cross-map reports should include the map order. Reproduce ordinary single-map issues from a fresh Warcraft III process.

## Uninstall

Remove WarVK's `d3d9.dll` and restore the previous file if one existed. Logs and diagnostic captures can be deleted separately.

## For developers

- `src/d3d9/`: D3D9 runtime, settings, shadow/lighting pipeline, and integration
- `src/d3d9/war3/`: Warcraft hooks, semantic bridge, resource lifetime, GPU skinning, JAPI, math, and diagnostics
- `subprojects/war3fx/`: WarVK shaders
- `WarVK/`: JASS library, YDWE catalog, icon, and author documentation
- `AutoTest/`: static contracts, Win32 runnable tests, performance gates, and diagnostics

Build the 32-bit runtime with:

```powershell
.\build32_safe.cmd src/d3d9/d3d9.dll -j2
ninja -C build32 -n src/d3d9/d3d9.dll
```

The output is `build32/src/d3d9/d3d9.dll`. Release contents and exclusions are defined in [docs/RELEASE_1.21.00.md](docs/RELEASE_1.21.00.md).

## License and acknowledgements

WarVK is distributed under GPLv3 at the project level. See [LICENSE](LICENSE), [COPYING](COPYING), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Upstream and third-party components retain their original licences and notices.

WarVK builds on DXVK, Dear ImGui, MinHook, the Vulkan ecosystem, and Warcraft III community research. It is an unofficial third-party project; keep backups before testing new releases.
