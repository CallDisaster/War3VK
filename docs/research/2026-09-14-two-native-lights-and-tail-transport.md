# Two-native-light test candidate and post-world color transport

Implemented as a tested candidate; no product acceptance yet. User requested
fixing registration selection / disappearing shadows and raising test capacity.
The automatic native takeover/shadow capacity becomes two (was one); authored
JAPI budgets remain 16 lights / 4 cubes. This is not an unlimited-light release.

## Four endpoints, not two subtractions

Let C00 be the original scene, C01 remove A, C10 remove B, C11 remove both.
All endpoints execute the same admitted FFP geometry, texture operations, fog,
alpha/coverage and per-attachment blend, with only exact selected direct/specular
terms removed. Original ambient/native calls/primary output remain untouched.

For visibility a,b in [0,1], independently derived transport is
`mix(mix(C11,C10,a), mix(C01,C00,a), b)`.
It equals the correct stored counterfactual at all four binary corners and is
a convex interpolation between them for filtered visibility. It is NOT claimed
to reproduce applying fractional lighting before arbitrary nonlinear operations.
Summing `(C00-C01)` and `(C00-C10)` fails under saturation; the both-removed
endpoint is mandatory. Three private same-format MRTs plus original are required.

The test shader reserves four color outputs and six private color varyings;
single-light scenes use the same four-endpoint path. This adds bandwidth and
shader work. No performance gain is claimed; dynamic/lower-cost variants need
separate measurement. No changes to public Shader API or JASS signatures.

## Tail proof and retained exclusions

Native WorldRenderScene ends before the established BeforeUi receiver boundary.
After it ends, admitted fixed-function draws use a zero removal mask for ALL
endpoints. They have identical incoming source color, original alpha, geometry,
depth and coverage while retaining each destination's independent blend state.
This is exact propagation of the counterfactual endpoints, not a new light claim
outside the native world. No new instance or light ownership can be seeded there.

All original shader/RT identity/format/feedback/application-MRT/alpha-dependency,
external-writer and final generation/frame/complete-cube guards remain. Unknown
or unsupported draws still withdraw the transaction; they are not silently skipped.
The previous unconditional lighting-bit rejection was stricter than the necessary
color-state proof. Tail counts must be observed together with actual commits in
the player's mixed scene, including selected-unit/portrait activity.

Explicit model rules rank ahead of built-ins, then by projected center distance.
Previous census is only a ranking hint; exact current draw claims and final lease
validation remain authoritative. More rules than capacity still compete; it does
not promise an arbitrary number of simultaneously shadowed models.

## Verification checkpoint

74CC676B / 35,486,415-byte PE32/i386 candidate: 58/58 exact DLL build edges,
BelowNormal / -j2 and exact no-work; 148/148 targeted Python/static, three-file
py_compile and Win32 synthetic path/parser tests pass. Receiver SPIR-V and all
10 dumped runtime SPIR-V modules pass Vulkan 1.3 / scalar-block-layout validation.
This is offline SPIR-V validation, not an installed Vulkan validation-layer run.

The original 3B0B candidate reproduces 78 sampled color fallbacks in the mixed
player-map fixture. The new R23 run has zero color/receiver fallback records,
86 sampled two-light / 12-face receiver commit records and both selected lights
still present after the final asynchronous screenshot. Tail draws remain nonzero.
ED122 imported TorchHuman is selected at (-58.1924,163.395,150.935), together with
the policy-2 stock TorchHumanOmni at (295.873,177.994,76.2476). Turning their JASS
shadow flags off deselects these two from takeover; unrelated native lamps may
then use the two slots. It does not turn off all lights in the scene.

Inspected R23 stage-1 (these lamps' shadows OFF) and stage-3 (ON) at 2560x1440:
the imported-lamp area has sustained projected caster silhouettes when ON.
The stock lamp still has a strongly darkened angular footprint. This does not
establish exact offending geometry, fix every stock emitter's self-occlusion,
or certify global shadow visual quality. The narrow inherited policy-8 emitter
exclusion remains restricted to its audited owner/first automatic slot; it is not
silently generalized to arbitrary imported models or whole walls.

Both actual runs were noninteractive isolated desktop / zero global input,
four JASS stages and native asynchronous screenshots, exact graceful exit 0,
zero new GPU events/dumps, and conditional restoration to the actual 3B0B test
baseline. Player DLL/model/map are unchanged. These are functional observations,
not an FPS A/B benchmark or all-effects/lifecycle acceptance. Full identities and
failed preparation boundaries are in
`../agent-history/2026-09-14-two-native-lights-runtime-checkpoint.md`.

## Primary references

- [Microsoft D3D9 diffuse lighting](https://learn.microsoft.com/en-us/windows/win32/direct3d9/diffuse-lighting):
  sums/materials and clamping explain why independent counterfactuals are needed.
- [Khronos framebuffer blending](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html):
  blending is attachment-local, with independent controls and destinations.
- [Khronos limits](https://docs.vulkan.org/spec/latest/chapters/limits.html):
  four color attachments form the conservative MRT budget; no eight-target assumption.

The interpolation algebra is derived here, not attributed to these references.
Tests must cover corners, saturation, filtered boundedness, primary alpha,
three private MRTs, priority, no post-world ownership expansion, fresh leases,
and mixed-light actual runtime/visuals. Test success is not product acceptance.
