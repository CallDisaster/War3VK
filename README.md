# WarVK

[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
![Vulkan](https://img.shields.io/badge/Vulkan-Backend-red)

[简体中文](README_CN.md)

WarVK is a Warcraft III 1.27a graphics enhancement project built on top of a DXVK-derived D3D9-to-Vulkan path. It focuses on visual fidelity and rendering experimentation for the classic client, including modern shadows, post-processing, runtime diagnostics, and engine-facing rendering research.

The latest experimental branch has intentionally moved away from the old rendering approach. Instead of treating Warcraft III as a black-box D3D9 draw stream and trying to replay whatever the fixed-function renderer submitted, WarVK now tries to read higher-level Warcraft III runtime data directly: visible renderables, object identity, model resources, pose/palette facts, material information, and draw-time geometry. Those semantic records are then used to build WarVK's own shadow map and receiver passes.

> [!NOTE]
> WarVK targets visual quality first. High-quality effects usually increase GPU load, and enabling more features does not guarantee higher FPS.

## For Players

### What it does

- Replaces the legacy D3D9 backend with a Vulkan-backed rendering path
- Adds in-game graphics controls for shadows, anti-aliasing, bloom, and exposure
- Improves visual presentation without modifying map data or gameplay rules

### Static Shadow Fixed Milestone

- Fixed Warcraft III's built-in static building shadow residue by hooking `CUnitUIManager_RecordSetStructureShadow` early and blocking `buildingShadow(+0x50)` writes into UnitUI type records
- Removed the legacy `TerrainShadow_RenderListB` black blob unit shadow by default so it no longer stacks with WarVK shadows
- Retired the old `WriteMaskRegion / StaticStampPath / RegisterImage / DoodadStamp` static-shadow experiments from the default production path
- Kept the research trail documented so the older paths remain available as evidence and diagnostic references

### 1.1.0 Highlights

- Added a logic-to-render bridge so object identity can be tracked more reliably across the game pipeline
- Reworked the experimental shadow path around upper-level Warcraft III runtime facts instead of relying only on legacy D3D9 draw replay
- Integrated StormBreaker directly into the project as part of the runtime foundation
- Upgraded GPU Arena to avoid driver overload caused by freeze-and-snapshot shadow capture on dynamic VB/IB workloads
- Added the first stage of static model caching to reduce GPU Arena pressure

### Current known issues

- Looking at bridge, ramp, and similar decorative/static geometry can still cause a short stall. The current static VB cache and LRU work reduce repeat stalls after the first sighting, but they are a mitigation rather than a complete fix.
- Path blocker CSM shadows can still leak into WarVK's shadow map. The project can identify some blocker render objects by FourCC, but one or more instances still survive into the final CSM caster list.

### Audience and limitations

- Competitive players may prefer lower visual clarity noise; bloom and soft shadows are not ideal for PVP
- Third-party platforms may block or remove custom `d3d9.dll` files, so WarVK is mainly intended for local, LAN, and single-player environments
- Map authors should not assume client-side DLL loading is available on all user platforms

### Requirements

- Windows 10 or Windows 11
- A Vulkan-capable GPU and current graphics drivers
- Warcraft III 1.27a

### Installation

1. Back up your Warcraft III directory, especially any existing `d3d9.dll`
2. Copy the release files into the game root directory where `war3.exe` is located
3. Start the game and confirm that `d3d9.log` is generated

Typical release contents:

- `d3d9.dll`
- `shaderpacks/` if a shader package is included
- Any extra runtime files bundled with the release

### In-game controls

- Press `Ctrl + F1` to toggle the WarVK configuration panel
- Common controls include:
  - `Unlock FPS`
  - `Enable Post-Processing`
  - `Enable Shadows`
  - `Shadow Quality`
  - `Shadow Intensity`
  - `Anti-Aliasing`
  - `Bloom`
  - `Exposure`

### Troubleshooting

- If the game crashes or shows a black screen, check `d3d9.log` and verify there is no conflicting third-party `d3d9.dll`
- If performance is poor, reduce shadow quality, bloom, and anti-aliasing first
- If specific maps show flicker or shadow instability, disable shadows or post-processing to isolate the issue before reporting it

### Uninstall

1. Remove `d3d9.dll` from the game root
2. Optionally remove `d3d9.log` and `shaderpacks/`

## For Developers

### Project status

Current WarVK milestone: `静态阴影解决版 (2026-07-08)`

WarVK is in a stage where the gameplay-facing bridge, GPU Arena, StormBreaker integration, and first-stage static model cache are already in place. Dynamic pose takeover is still under active research and has not fully replaced the fallback shadow path.

The active experimental shadow branch is no longer a simple "capture D3D9 draw calls and replay them later" renderer. The intended path is semantic: hook Warcraft III's upper render/runtime layers, reconstruct stable object and renderable identity, pull model/pose/material facts near their producer, and submit a WarVK-owned shadow scene. Legacy draw capture still exists as a compatibility and diagnostic fallback, but the long-term direction is to render the shadow map from Warcraft III runtime semantics rather than from the old fixed-function stream alone.

### Core architecture

- `src/d3d9/`
  The WarVK-specific D3D9 runtime, game hooks, shadow pipeline, post-processing, and graphics settings
- `src/dxvk/`
  Vulkan abstraction and DXVK-derived runtime layer
- `src/dxso/`
  Shader compiler and translation support
- `src/util/`
  Shared utilities, logging, threading, configuration, and helpers
- `src/spirv/`
  SPIR-V handling and shader infrastructure
- `src/vulkan/`
  Vulkan loader and common helpers
- `src/wsi/`
  Window system integration
- `src/minhook/`
  Hooking dependency used for game-side interception

### Rendering subsystems added in WarVK

- Logic-to-render bridge for object identity propagation
- Native render hook domains for UI, render, lifecycle, JASS, and shadow control
- GPU Arena for shadow capture and transient runtime geometry handling
- Static model cache for reducing runtime geometry duplication
- Runtime diagnostics, performance reporting, and automated regression hooks

### Build requirements

- MinGW-w64 GCC 15.2.0 or newer
- Meson 0.58 or newer
- Ninja
- Vulkan SDK
- glslang

### Build

```powershell
# Initial setup
meson setup --cross-file build-win32.txt build32

# Recommended incremental build
.\build32_safe.cmd src/d3d9/d3d9.dll -j8
```

Main output:

- `build32/src/d3d9/d3d9.dll`

### Development notes

- `build32_safe.cmd` exists to avoid the Windows extended-path shell issue that can break MinGW incremental builds
- `build32_safe.cmd` also restores the local Meson shim used to build the upstream `imgui` submodule without maintaining a separate fork
- `CHANGELOG.md` tracks user-facing version changes
- `AGENTS.md` tracks the current engineering status and handoff notes
- AutoTest and performance tooling live under `AutoTest/` and `src/d3d9/war3/tools/`

### Source release and licensing note

WarVK is now distributed under GPLv3 at the project level. The full GPLv3 text is provided in [LICENSE](LICENSE) and [COPYING](COPYING).

This repository also contains third-party components and upstream-derived code that remain available under their original licenses. Those components are documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and their original notices should be preserved when redistributing the project.

## Roadmap

- Complete dynamic pose-driven shadow reconstruction using cached model resources
- Continue reducing hot-path overhead in shadow and projector interception
- Move more culling and batching work from CPU-side heuristics toward GPU-friendly execution
- Expose a cleaner external shader workflow for community-authored shader packs

## Acknowledgments

- [DXVK](https://github.com/doitsujin/dxvk)
- Dear ImGui
- MinHook
- MemHack research references and reverse-engineering inspiration
- Asphodelus and prior JASS engine research

## Disclaimer

WarVK is an unofficial third-party rendering plugin. Use it at your own risk, and keep backups of your game directory and saves before testing new builds.
