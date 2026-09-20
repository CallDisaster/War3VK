# culling-b02: Observer proof contract

- taskId: `culling-b02`
- parentThreadId: `01a02e0b-1d1e-7762-b40b-63a00bbb3449`
- status: design-only. No Consume, no capture skip, no CSM reorder, no budget change.
- scope: union Observe proof tightening plus future early-reduction admission contract.

## 1. Orthographic contract

The union policy must validate the homogeneous w row directly, not through a
caller-supplied boolean:

```
matrix.columns[0][3] == 0.0f
matrix.columns[1][3] == 0.0f
matrix.columns[2][3] == 0.0f
matrix.columns[3][3] == 1.0f
```

The check runs after the finite-matrix check and before any projection math.
Exact equality is deliberate: perspective, negative-w, near-perspective and
NaN w rows must fail visible with `NonOrthographicProjection` (or
`NonFiniteMatrix` when the matrix is non-finite). The row-length radius formula
is unchanged. Old reject values stay in place; new reasons are appended.

## 2. Map/device proof

`War3UnionGenerationProof` now separates map/device identity from frame and
resource domains:

```
currentMapGeneration, candidateMapGeneration, consumerMapGeneration
currentDeviceGeneration, candidateDeviceGeneration, consumerDeviceGeneration
```

All three sources must be non-zero and equal. A caller must not copy
`current` into `candidate` or `consumer`. If no independent source exists,
pass 0 and treat the Observe result as unauthenticated/fail-visible.

Production call-site mapping (read-only review; parent owns the shared TU):

| proof source | field | reliable source |
| --- | --- | --- |
| current map/device | input.mapEpoch / input.deviceEpoch | `War3PipelineInput` |
| candidate map/device | draw.mapEpoch / draw.deviceEpoch | `War3ShadowCasterDraw` |
| consumer map/device | m_shadowMapEpoch / m_shadowDeviceEpoch | `War3ShadowReceiverPass`, set by `InvalidateMapEpoch` |

Proposed small patch for the parent, not applied in this batch:

```diff
@@ -4609,6 +4609,12 @@
  query.generations.currentFrameGeneration = input.frameSerial;
  query.generations.candidateFrameGeneration = input.frameSerial;
  query.generations.boundsFrameGeneration = draw.boundsFrameSerial;
  query.generations.cameraFrameGeneration = input.frameSerial;
  query.generations.consumerStateFrameGeneration = input.frameSerial;
+ query.generations.currentMapGeneration = input.mapEpoch;
+ query.generations.candidateMapGeneration = draw.mapEpoch;
+ query.generations.consumerMapGeneration = m_shadowMapEpoch;
+ query.generations.currentDeviceGeneration = input.deviceEpoch;
+ query.generations.candidateDeviceGeneration = draw.deviceEpoch;
+ query.generations.consumerDeviceGeneration = m_shadowDeviceEpoch;
  query.generations.resourceGeneration = m_shadowMapResourceGeneration;
  query.generations.expectedResourceGeneration =
      m_shadowMapResourceGeneration;
```

Until that patch is reviewed/applied, production Observe receives zeros for
the new domains and therefore remains fail-visible. This is intentional.

## 3. Off/Observe/Consume semantics

`Off` behavior, `effectiveVisibleMask`, and `consumeAdmissionGranted=false`
stay unchanged. No default Consume, no capture bypass, no shadow-budget change.

## 4. False-negative counter reference

`unionCullFalseNegativeCount` currently compares the union prediction with
`cascadeVisible`, the canonical conservative replay mask. That is not
geometric ground truth. The batch does not rename existing JSON fields and does
not claim that zero proves correctness. Future evidence must label this as
"disagreement with the conservative baseline"; a real missed-shadow
counterexample needs image/geometry evidence.

## 5. Early `wouldSkipIfEarly` contract

`wouldSkipIfEarly` may not be derived from C2/C3 alone. It must require closure
across every actual consumer of the caster: main, CSM0-3, volume-sun,
point-shadow, outline, and any future consumer registered in the joint mask.
It also requires exact same-frame bounds, current map/device/consumer/resource
generations, and the complete caster light volume. Camera-frustum-only culling
is not admissible.

The future observation should be counter-only:

- bytes that would be skipped per independently allocated slice;
- all-consumer outside bits and proof reject reasons;
- page create/used/reclaim counters already present.

No new log ring, image ring or render admission change is proposed here.

## 6. Snapshot size correction

Snapshot slices are allocated independently and may cross or create pages.
The planning estimate is:

```
B_i_est = sum_s align256(bytes_s)
```

not a single `align256(pos + blend + uv + index)`. Actual resident/suballocation
counters remain authoritative.

## 7. Validation

- C++ counterexamples cover exact w row, perspective/negative/near-perspective
  w, map/device 0/mismatch, same-frame stale map, boundary contact, negative
  scale and unproven bounds.
- Python static tests pin the new reject reasons, proof fields and source
  order. Compilation is left to the validation lane.
