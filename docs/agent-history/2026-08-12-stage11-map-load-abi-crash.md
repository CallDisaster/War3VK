# Stage11 map-load ABI crash

## Incident

The incremental Stage11 candidate with SHA-256
`4FD9E93651AFC6E6AA1657E2BBFDDDBDDDD7E5823B10AE02064DD360799E055E`
raised `0xC0000005` while entering a map. The crash dump recorded
`d3d9.dll+0x1D10CF`; symbol and disassembly inspection placed the read in
`war3shader::internal::UpdateRenderContext(const War3PipelineInput&)` while it
was walking scene data.

This was not a Vulkan device loss. Ninja reported zero dependencies for the
incremental `war3_shader_api.cpp.obj`. That object predated the change to
`War3ShadowReplayBufferBinding`, while the producer translation units had been
rebuilt with the new layout. The stale consumer therefore used an old
`War3ShadowCasterDraw` element stride and read beyond a valid element after the
map published a non-empty scene.

## Correction

- The broken candidate was removed from the game directory and the stable DLL
  `79CA8DB4...B2A4` was restored.
- A separate Meson directory, `build32_fresh_stage11_20260812`, rebuilt the
  complete `d3d9.dll` dependency closure from scratch with two compiler jobs.
- A link-time `War3ShaderContextAbi` tag now encodes the size and alignment of
  `War3PipelineInput`, `War3FrameScene`, `War3ShadowCasterDraw`, and
  `War3ShadowReplayBufferBinding`. `d3d9_war3_pipeline.cpp` must resolve that
  exact symbol from the translation unit that implements `UpdateRenderContext`.
  A future stale object therefore fails to link instead of producing a mixed
  ABI DLL.

## Verification boundary

The fresh ABI-guarded DLL has SHA-256
`0EB6964A7F3CD1311C27596B83174BE6E2C4C3EC4E80D3BAF5E3A69D4F639D45`.
Targeted static contracts, the replay validation runnable, clean-target
no-work, and the non-interactive isolated map-load smoke passed. The smoke
entered `isInGame=true`, reached 345 report frames, and recorded no device
loss, incomplete frame, budget overflow, or new crash dump.

This only closes the map-load crash and mixed-object build failure. It does not
constitute physical validation of the reported building shadow tearing, which
still requires a foreground visual test of the fresh candidate.
