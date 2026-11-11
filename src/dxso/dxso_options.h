#pragma once

#include "../dxvk/dxvk_device.h"
#include "../d3d9/d3d9_options.h"

namespace dxvk {

  struct D3D9Options;

  struct DxsoOptions {
    DxsoOptions();
    // Shared by the in-process D3D9 device and the isolated x64 D3D9 host.
    // Pass the actual constant layout size; no Game.dll/device-wrapper ABI is
    // needed to derive shader compiler options from the same DXVK device.
    DxsoOptions(const Rc<DxvkDevice>& device, const D3D9Options& options,
                uint32_t vertexFloatConstantBytes);

    /// True:  Copy our constant set into UBO if we are relative indexing ever.
    /// False: Copy our constant set into UBO if we are relative indexing at the start of a defined constant
    /// Why?:  In theory, FXC should never generate code where this would be an issue.
    bool strictConstantCopies = false;

    /// Whether to emulate d3d9 float behaviour using clampps
    /// True:  Perform emulation to emulate behaviour (ie. anything * 0 = 0)
    /// False: Don't do anything.
    D3D9FloatEmulation d3d9FloatEmulation = D3D9FloatEmulation::Disabled;

    /// Whether or not we should care about pow(0, 0) = 1
    bool strictPow = false;

    /// Work around a NV driver quirk
    /// Fixes flickering/z-fighting in some games.
    bool invariantPosition = false;

    /// Always use a spec constant to determine sampler type (instead of just in PS 1.x)
    /// Works around a game bug in Halo CE where it gives cube textures to 2d/volume samplers
    bool forceSamplerTypeSpecConstants = false;

    /// Interpolate pixel shader inputs at the sample location rather than pixel center
    bool forceSampleRateShading = false;

    /// Should the SWVP float constant buffer be a SSBO (because of the size on NV)
    bool vertexFloatConstantBufferAsSSBO = false;

    /// Whether or not we can rely on robustness2 to handle oob constant access
    bool robustness2Supported = false;

    /// Whether or not we need to use custom sincos
    bool sincosEmulation = false;
  };

}
