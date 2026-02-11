# War3VK (Warcraft III Graphics Enhancement Plugin)

[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
![Vulkan](https://img.shields.io/badge/Vulkan-Backend-red)

[简体中文](README_CN.md)

**War3VK** is a graphics enhancement plugin for **Warcraft III** (Classic Client). It hooks the Direct3D 9 rendering pipeline and utilizes a custom **Vulkan** backend to introduce modern visual features, including Cascaded Shadow Maps (CSM), Bloom, and advanced post-processing effects.

> [!NOTE]
> **Target:** Visual Fidelity > Performance.
> Enabling high-quality effects will increase GPU load. This project aims to make the game look better, not run faster.

## Features

- **Vulkan Backend**: Replaces the legacy D3D9 rendering path with a modern Vulkan backend via DXVK-based technology.
- **Visual Enhancement**:
  - **Advanced Shadows**: CSM (Cascaded Shadow Maps), PCF/Poisson Sampling.
  - **Post-Processing**: Bloom, Tone Mapping, Exposure Control.
  - **Anti-Aliasing**: FXAA, SMAA.
- **In-Game Overlay**: Built-in **ImGui** panel for real-time configuration.
- **Pure Visuals**: Does not modify game mechanics or map data.

## Prerequisites

### System & Drivers
- **OS**: Windows 10 / 11 (Recommended).
- **GPU Driver**: Must support **Vulkan 1.1+**. Please update to the latest drivers.

### Game Version
- **Target**: Warcraft III **1.27a**.
  - *Note: Other versions may crash due to offset differences in `Game.dll`.*

### Compatibility Warning
> [!WARNING]
> Please remove or rename any existing third-party `d3d9.dll` (e.g., other graphical patches or wrappers) in your game directory to avoid conflicts.

## Installation

1. **Backup**: It is highly recommended to backup your game root directory (especially the original `d3d9.dll` if it exists).
2. **Copy Files**: Extract the release package into your Warcraft III root directory (where `war3.exe` is located).
   - `d3d9.dll` (Core Plugin)
   - `shaderpacks/` (Optional: For post-processing shaders)
3. **Verify**: Launch the game. A `d3d9.log` file should appear in the root directory. If the game starts and the log is generated, War3VK is loaded successfully.

## Usage & Configuration

### Toggle Menu
Press **`Ctrl + F1`** to toggle the configuration overlay.

### Key Settings
- **Unlock FPS**: Uncaps the frame rate limit.
- **Post-Processing**: Master switch for effects like Bloom and AA.
- **Enable Shadows**: Master switch for the custom shadow engine.
- **Shadow Quality**: Low / Medium / High / Ultra.
- **Shadow Intensity**: Adjusts the darkness of shadows.
- **Anti-Aliasing**: Off / FXAA / SMAA.
- **Bloom**: Enables the glow effect.
- **Exposure**: Adjusts global brightness/exposure.

## Uninstallation

Simply delete (or move) `d3d9.dll` from your game directory. You can also remove `d3d9.log` and the `shaderpacks/` folder.

## FAQ

**Q: Game crashes on startup / Black screen?**
A: Check if your GPU driver supports Vulkan. Ensure no other `d3d9.dll` exists in the folder. Check `d3d9.log` for error details.

**Q: Low FPS after installing?**
A: This is expected on older hardware. Try lowering Shadow Quality, disabling Bloom, or reducing AA settings.

**Q: Visual glitches (flickering shadows) on specific maps?**
A: Some custom maps use unique rendering tricks. Try disabling shadows or post-processing for those specific maps. Please report the issue with your `d3d9.log`.

**Q: Antivirus / Battle Platform interference?**
A: Since this plugin uses DLL injection/hooking techniques, some platforms may flag it. Use it in a permitted environment.

## Technical Overview

- **Backend**: D3D9 to Vulkan translation based on DXVK concepts.
- **Hooking**: MinHook is used to intercept War3 rendering stages and state changes.
- **Shadows**: Custom implementation of CSM with Poisson Disk Sampling.
- **UI**: Integrated **Dear ImGui** for the overlay.

## Disclaimer

War3VK is an unofficial third-party plugin. Use it at your own risk. Always backup your game and save files.

## Acknowledgments

- **DXVK**
- **Dear ImGui**
- **MinHook**
- **MemHack**
