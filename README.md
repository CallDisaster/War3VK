# WarVK 1.2.0 Release

[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
![Vulkan](https://img.shields.io/badge/Vulkan-1.3+-red)
![Warcraft III](https://img.shields.io/badge/Warcraft%20III-1.27a-gold)

[简体中文](README_CN.md) · [Changelog](CHANGELOG.md) · [WarVK JAPI](WarVK/README.md)

WarVK is a graphics enhancement runtime for Warcraft III 1.27a. It uses a DXVK-derived D3D9-to-Vulkan backend and adds modern directional and point shadows, volumetric lighting, post-processing, diagnostics, and an author-facing JASS API.

Version 1.2.0 is the first public release of the newer semantic rendering architecture. WarVK now combines Warcraft runtime identity, model, pose, material, and current-draw evidence to construct its own shadow scene. Legacy D3D9 capture remains a guarded compatibility path rather than the only source of rendering truth.

> [!IMPORTANT]
> Update to the latest official NVIDIA, AMD, or Intel graphics driver before installing. The current runtime requests **Vulkan 1.3**, which includes Vulkan 1.2; a driver that exposes only Vulkan 1.2 is not sufficient for this build.

> [!NOTE]
> WarVK prioritizes rendering quality and correctness. 4096-resolution cascaded shadows, point shadows, volumetric effects, and post-processing all have a real performance cost.

## Highlights in 1.2.0

### Shadows and lighting

- Four-cascade directional shadows now default to a stable 4096 resolution, with a latched 2048 fallback only when allocation cannot be completed safely.
- Sharper PCF tuning reduces overly soft CSM edges without changing point-shadow filtering.
- Alpha-tested foliage, animated/skinned units, rigid geometry, buildings, and terrain use stricter current-frame source ownership.
- Path blockers and native static/blob shadow residue are excluded from the WarVK shadow scene.
- Point-light shadows use radial depth, receiver-plane bias, texel-centred sampling, and explicit depth synchronization to substantially reduce the previous severe moiré/banding artifacts. Some surfaces and viewing angles remain affected; see Known issues below.
- TAA v2 is available as an optional temporal mode with variance clipping, reactive feedback, history diagnostics, and one-shot history invalidation. DirectInline remains the release default.
- Volumetric sunlight, volumetric point lights, and independently controlled global height fog are available through the runtime and author API.

### Stability and performance

- Shadow Arena capture is transactional: vertex, blend, UV, and index data are reserved as one bundle and fail closed as one caster.
- Arena generations are retired by GPU completion fences instead of frame-index reuse, preventing in-flight data from being overwritten.
- Map/device epochs isolate manifests, cached geometry, GPU skinning, point-shadow work, TAA history, and receiver publications across map changes.
- Final CSM and point-shadow replay validates buffer ranges, index domains, formats, generations, matrices, and skinning inputs before issuing Vulkan draws.
- Incomplete CSM candidates are never published caster by caster. A complete same-map shadow is published atomically, or the receiver safely shows no directional shadow.
- Bounded bulk reads for write-combined index buffers remove a major CPU regression without restoring unsafe cross-frame VB/IB caches.
- Compact work tables, conservative union culling, persistent GPU packages, a persistent point-shadow planner, and CPU multi-threaded skinning contracts are included as guarded infrastructure. Experimental Consume paths remain disabled until their correctness and performance gates pass.

### Map-author API

- WarVK JAPI is built into the proxy `d3d9.dll`, which must be installed before Warcraft III starts. The author package contains no in-map DLL loader and does not require a separate `war3map.dll`.
- The public wire protocol remains `warvk:v1`. High-frequency numeric calls can use a verified typed Hashtable transport while text commands retain the compatible string route.
- Authors can create and update point lights, enable point shadows, control volumetric lighting and global height fog, and manage the WarVK lighting clock independently from Warcraft gameplay time.
- Lightning templates support textures, colours, widths, animation, branching, formula curves, and uploaded polyline curves.
- The bounded MathProgram/Curve runtime can evaluate scalar, `vec2`, and `vec3` expressions, return real or integer results to JASS, query derivatives and arc length, and drive continuous lightning ribbons.
- YDWE metadata is grouped into system, diagnostics, sun/CSM, lighting clock, point light, volumetric light, volumetric fog, lightning, template, math, and curve categories. Mode arguments use selectable trigger types instead of raw integer entry.

See [CHANGELOG.md](CHANGELOG.md) for the full release summary and [WarVK/README.md](WarVK/README.md) for the author API.

## Requirements

- Windows 10 or Windows 11
- A GPU and official driver exposing Vulkan 1.3 or newer
- Warcraft III 1.27a (32-bit)

WarVK targets the classic 1.27a executable and verified `Game.dll` layouts. Unknown executable signatures fail closed rather than guessing addresses.

## Installation

1. Back up the Warcraft III directory, especially any existing `d3d9.dll`.
2. Copy the release `d3d9.dll` next to `war3.exe`.
3. Start the game and confirm that `d3d9.log` reports `DXVK: 1.2.0 Release`.
4. Press `Ctrl + F1` to open or close the WarVK settings panel.

The player package needs only the files explicitly listed in the release archive. Source folders, test tools, research data, and the YDWE author package do not belong in the game directory.

## Common controls

- Unlock FPS
- Enable post-processing
- Enable shadows and select shadow/TAA modes
- Adjust shadow strength and filtering
- Configure point lights and point shadows
- Configure volumetric lighting and fog
- Adjust anti-aliasing, bloom, exposure, and outlines

## Known issues in 1.2.0 Release

- Point lights with point shadows enabled can still produce moire or banding artifacts on some ground surfaces and viewing angles. This is not fully fixed in 1.2.0; if the artifact is distracting, keep the point light enabled but disable its point shadow.
- Leaving a map and then loading another map in the same Warcraft III process can cause persistent performance loss, shadow corruption, or other resource-lifetime problems. Reliable cross-map sessions are not supported in 1.2.0. Fully exit Warcraft III and restart it before loading another map.
- Both issues are deferred to a later release. The common platform workflow of launching the game, playing one map, and then exiting remains the recommended usage for 1.2.0.

## Troubleshooting and reports

- A black screen at startup usually indicates an outdated Vulkan driver or a conflicting third-party `d3d9.dll`.
- For poor performance, disable volumetric effects first, then reduce point-shadow count and post-processing. Directional CSM remains 4096 unless the runtime safely latches the allocation fallback.
- Do not copy multiple D3D9 proxy DLLs into the same game directory.
- Useful report files include `d3d9.log`, `runtime_status.json`, GPU incident JSON, and WarVK crash dumps. Remove personal paths or map data before publishing logs.
- If a map has already been left in the current process, fully exit Warcraft III, restart it, and load only the target map. Cross-map reports should include the exact transition order and the first incident snapshot.

## Uninstall

Remove WarVK's `d3d9.dll` from the game directory and restore the previous file if one existed. Logs and diagnostic captures can be deleted separately.

## For developers

The primary code areas are:

- `src/d3d9/`: D3D9 runtime, settings, shadow/lighting pipeline, and integration
- `src/d3d9/war3/`: Warcraft hooks, semantic bridge, resource lifetime, GPU skinning, JAPI, math, and diagnostics
- `subprojects/war3fx/`: WarVK shaders
- `WarVK/`: JASS library, YDWE catalog, icon, and author documentation
- `AutoTest/`: static contracts, runnable tests, performance gates, and attach-only diagnostics

Build the 32-bit runtime with:

```powershell
.\build32_safe.cmd src/d3d9/d3d9.dll -j8
ninja -C build32 -n
```

The output is `build32/src/d3d9/d3d9.dll`. Release contents and exclusions are defined in [docs/RELEASE_1.2.0.md](docs/RELEASE_1.2.0.md).

## License and acknowledgements

WarVK is distributed under GPLv3 at the project level. See [LICENSE](LICENSE), [COPYING](COPYING), and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Upstream and third-party components retain their original licences and notices.

WarVK builds on work from DXVK, Dear ImGui, MinHook, Vulkan ecosystem projects, and Warcraft III community research. It is an unofficial third-party project; keep backups before testing new releases.
