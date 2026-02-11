# War3VK (Warcraft III Graphics Enhancement Plugin)

[![Platform](https://img.shields.io/badge/platform-Windows-lightgrey.svg)]()
![Vulkan](https://img.shields.io/badge/Vulkan-Backend-red)

[简体中文](README_CN.md)

**War3VK** is a graphics enhancement plugin for **Warcraft III** (Classic Client). It hooks the Direct3D 9 rendering pipeline and utilizes a custom **Vulkan** backend to introduce modern visual features, including Cascaded Shadow Maps (CSM), Bloom, and advanced post-processing effects.

> [!NOTE]
> **Target:** Visual Fidelity > Performance.
> Enabling high-quality effects will increase GPU load. This project aims to make the game look better, not run faster.

## ⚠️ Target Audience & Limitations (MUST READ)

Please be aware of the following target audience mismatches and limitations before use:

- **Competitive Players (PVP)**: Competitive gameplay often requires maximum clarity. Visual effects like Soft Shadows and Bloom may distract from gameplay. It is recommended to disable these effects or the plugin entirely for competitive matches.
- **Platform Compatibility**: Many third-party matchmaking platforms (e.g., Netease, W3Champions) may block or delete unauthorized DLLs (like `d3d9.dll`) upon startup. This plugin is primarily designed for **Local / LAN / Single Player** environments.
- **Map Authors**: Due to the platform restrictions mentioned above, map authors cannot rely on this plugin to distribute custom visual effects, as client-side loading cannot be guaranteed.

## 1. Introduction

### What is this? What does it change?

- **Vulkan Backend**: Translates the legacy D3D9 rendering calls to Vulkan (reducing reliance on legacy drivers).
- **Visual Enhancement**: Adds an in-game configuration panel for **Shadow Quality**, **Anti-Aliasing**, **Bloom**, **Exposure**, and more.
- **Pure Visuals**: Does not modify game mechanics or map data; focuses solely on rendering presentation.

## 2. Prerequisites

### 2.1 System & Drivers
- **OS**: Windows 10 / 11 (Recommended).
- **GPU Driver**: Must support **Vulkan 1.1+**. Please update to the latest drivers.

### 2.2 Game Version
- **Target**: Warcraft III **1.27a**.
  - *Note: Other versions may crash due to `Game.dll` offset differences.*

### 2.3 Compatibility Warning
> [!WARNING]
> Please remove or rename any existing third-party `d3d9.dll` (e.g., other graphical patches or wrappers) in your game directory to avoid conflicts.

## 3. Installation

### 3.1 Backup
It is highly recommended to backup your game root directory (especially the original `d3d9.dll`).

### 3.2 Copy Files
Extract the release package into your Warcraft III root directory (where `war3.exe` is located).

Files usually include:
- `d3d9.dll` (Core Plugin)
- `shaderpacks/` (Optional: For post-processing shaders)

### 3.3 Verify
Launch the game. A `d3d9.log` file should appear in the root directory. If the game starts and the log is generated, War3VK is loaded successfully.

## 4. In-Game Settings

### 4.1 Toggle Menu
Press **`Ctrl + F1`** to toggle the configuration overlay.

### 4.2 Key Settings
In the "War3VK Configuration" panel, you can adjust:

- **Unlock FPS**: Uncaps the frame rate limit.
- **Enable Post-Processing**: Master switch for effects like Bloom and AA.
- **Enable Shadows**: Master switch for the custom shadow engine.
- **Shadow Quality**: Low / Medium / High / Ultra.
- **Shadow Intensity**: Adjusts the darkness of shadows.
- **Anti-Aliasing**: Off / FXAA / SMAA.
- **Bloom**: Enables the glow effect.
- **Exposure**: Adjusts global brightness/exposure.

## 5. ShaderPack (Optional)
*Coming Soon (Not yet available)*

## 6. Uninstallation
1. Delete (or move) `d3d9.dll` from your game directory.
2. You can also remove `d3d9.log` and the `shaderpacks/` folder.

Once removed, the game will revert to its original rendering engine.

## 7. FAQ

### 7.1 Game crashes on startup / Black screen?
- **Cause**: Usually related to incompatible GPU drivers, Vulkan environment issues, or conflicts with other `d3d9.dll` files.
- **Solution**: Update GPU drivers; ensure no other DLLs conflict; check `d3d9.log` for errors.

### 7.2 Low FPS after installing?
- **Note**: This project focuses on visual quality, not performance optimization.
- **Solution**: Lower Shadow Quality, disable Bloom, or reduce Anti-Aliasing settings.

### 7.3 Visual glitches (flickering) on specific maps?
- **Cause**: Some maps use unique rendering techniques or extreme camera angles.
- **Solution**: Try disabling Shadows or Post-Processing; submit your `d3d9.log` for analysis.

### 7.4 Antivirus / Platform interference?
- **Note**: Since this plugin uses DLL injection, some platforms may flag it.
- **Solution**: Use in a permitted environment (Local/LAN).

## 8. Technical Overview

- **Backend**: D3D9 to Vulkan translation based on DXVK concepts.
- **Hooking**: MinHook is used to intercept War3 rendering stages.
- **Shadows**: 4-Level CSM + Poisson Disk Sampling + Temporal Accumulation.
- **Post-Processing**: FXAA / SMAA, Bloom, Exposure Control.
- **UI**: Integrated **Dear ImGui**.

## 9. Disclaimer
War3VK is an unofficial third-party plugin. Use it at your own risk. Always backup your game and save files.

## 10. Roadmap (Future Plans)
- **Rendering Optimization**: Rewrite culling logic to migrate from CPU Culling to **GPU Culling**.
- **Batching**: Refactor the rendering layer to support **Draw Call Batching**, leveraging Vulkan's efficiency.
- **External Shaders**: The backend supports external shader packs (similar to Minecraft's Optifine/Iris). Documentation for community shaders will be released in the future.

## 11. Acknowledgments
- **DXVK**
- **Dear ImGui**
- **MinHook**
- **MemHack**
