# Vulkan release review closure and shadow-edge candidate (2026-08-09)

## Scope

This local candidate closes the release-path Vulkan lifetime findings from the
2026-08-09 review and advances the directional-shadow edge investigation. It is
not deployed and has not passed physical Warcraft III visual or validation-layer
testing.

## Vulkan and fail-stop changes

- SMAA lookup uploads keep staging buffers alive through command-list tracking;
  both lookup images are published as one transaction and failure falls back
  without marking the tables ready.
- `VK_ERROR_DEVICE_LOST` is an irreversible device-level terminal state for
  submission, WSI and resource creation. Presenter recreation is forbidden after
  loss and D3D9 `Present` reports device removal.
- CS shutdown rejects new GPU work but still drains and joins CPU command-stream
  ownership. Single-use commands are destroyed exactly once and failed chunks
  still advance their sequence.
- Outline uses the matching WithCount dynamic states, updates both WarVK and
  DXVK image-layout ownership, and preflights pipelines before rendering begins.
- Final shadow replay validates palette requirement, index/count arithmetic and
  the complete 256-matrix entry before any shader access.
- Shadow Arena has an exact `DxvkDevice` owner and explicit shutdown. Its budget
  now follows the heap selected by the real first allocation rather than the
  largest device-local heap; a later allocation on a different heap is rejected.
- Volumetric resources publish transactionally. Directional, volume-sun and
  point-shadow workload reservations roll back until the first rendering
  command. Volume-sun and directional CSM use independent reservations.
- Fatal device loss has a dedicated incident latch, so an earlier ordinary
  stall cannot suppress the terminal snapshot.

## Directional shadow-edge candidate

The already integrated compare-first PCF path is retained: raw depth is sampled
nearest for blocker/debug paths, CSM visibility compares each texel before
filtering, the periodic world-space rotation is disabled by default, and the
fixed Poisson kernel is paired and zero-centroid.

This candidate adds receiver-plane reference-depth correction per PCF and PCSS
tap in both the receiver and visibility shaders. The gradient is accepted only
when the complete UV Jacobian and normalized depth excursion are finite and
bounded. If the proof fails, the whole kernel uses the previous conservative
centre reference; taps never mix proof domains.

This targets moving/flowing edge aliasing. It does not repair a missing caster,
an incomplete CSM publication or a workload-budget rejection. Those must remain
separate diagnoses.

## Validation

- 86/86 `AutoTest/test_*_static.py` scripts passed.
- 27/27 Win32 Meson runnable tests passed.
- Win32 DLL clean dependency rebuild passed with `-j2`.
- `ninja -C build32 -n src/d3d9/d3d9.dll`: no work to do.
- `git diff --check`: clean apart from line-ending notices.
- Candidate DLL SHA-256 before this documentation-only change:
  `1501EFF6D31A6BABF0E1565A8B1DBA6A85CBAC56A17794EA6E085DB1A7A0E774`.

## Remaining gates

- Physical DirectInline A/B at fixed camera and moving sun, including cascade
  transitions and stable walls.
- Validation-layer run for the reviewed Vulkan paths.
- TAA temporal reprojection defects and far-cascade alpha-cutoff parity remain
  outside this candidate.
- Cross-map correctness remains unclaimed.
