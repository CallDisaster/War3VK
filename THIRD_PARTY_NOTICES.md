# Third-Party Notices

WarVK is distributed as a GPLv3-licensed project at the repository level, but it includes upstream-derived code, submodules, headers, tools, and libraries that remain available under their own original licenses.

This file is a practical notice file for redistribution. It is not a complete legal inventory of every single dependency header in the tree, but it records the primary third-party components that should not be mistaken for WarVK-original code.

## Primary upstream and third-party components

- DXVK
  - Location: repository base, especially `src/dxvk/`, `src/d3d9/`, `src/dxso/`, `src/util/`, `src/vulkan/`, `src/wsi/`
  - Upstream: [DXVK](https://github.com/doitsujin/dxvk)
  - Original license: zlib/libpng
  - Preserved notice copy: [LICENSES/DXVK-zlib-libpng.txt](LICENSES/DXVK-zlib-libpng.txt)

- Dear ImGui
  - Location: `subprojects/imgui/`
  - Upstream: [ocornut/imgui](https://github.com/ocornut/imgui)
  - Original license: MIT

- MinHook
  - Location: `src/minhook/`
  - Upstream: [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook)
  - Original license: 2-clause BSD

- SMAA
  - Location: `smaa/`
  - Upstream: [iryoku/smaa](https://github.com/iryoku/smaa)
  - Original license: MIT

- dxbc-spirv
  - Location: `subprojects/dxbc-spirv/`
  - Upstream: [doitsujin/dxbc-spirv](https://github.com/doitsujin/dxbc-spirv)
  - Original license: follow upstream repository notices

- libdisplay-info
  - Location: `subprojects/libdisplay-info/`
  - Upstream: [doitsujin/libdisplay-info](https://github.com/doitsujin/libdisplay-info)
  - Original license: follow upstream repository notices

- mingw-directx-headers / native DirectX headers
  - Location: `include/native/directx/`
  - Upstream: [Joshua-Ashton/mingw-directx-headers](https://github.com/Joshua-Ashton/mingw-directx-headers)
  - Original license: see files in that directory, including [include/native/directx/COPYING.MinGW-w64.txt](include/native/directx/COPYING.MinGW-w64.txt)

- Vulkan Headers
  - Location: `include/vulkan/`
  - Upstream: [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)
  - Original license: follow upstream repository notices

- SPIR-V Headers
  - Location: `include/spirv/`
  - Upstream: [KhronosGroup/SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers)
  - Original license: follow upstream repository notices

## Project-adjacent research references

- MemHack
  - Status: not bundled as a runtime dependency of WarVK
  - Usage: research reference during Warcraft III memory and engine analysis
  - Note: the current repository does not ship the MemHack library as an integrated runtime component

## Redistribution guidance

- Keep [LICENSE](LICENSE) and [COPYING](COPYING) with source distributions of WarVK.
- Keep original third-party notices and per-directory license texts when redistributing source or substantial code excerpts.
- Do not remove upstream copyright notices from DXVK-derived or third-party files.
