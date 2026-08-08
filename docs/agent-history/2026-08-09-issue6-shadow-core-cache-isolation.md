# Issue #6 shadow-core cache isolation — 2026-08-09

## Investigation result

The Present-owned transition already reset semantic registries and retired GPU
resources behind a completion fence, but three CPU lookup paths could still
accept map-A pointer aliases after Warcraft reused the same addresses in map B:

- `StaticMeshDataResourceCache` was process-global and keyed by `meshData`,
  primary stream, stride, mesh index and resource-record count. A hit returned a
  complete copied `ShadowModelResourceRecord`, including map-A geometry vectors.
- the runtime-model/geoset-data shape probes cached a boolean by pointer only;
- the legacy `(runtimeModel, runtimeGeosetData) -> geoset/index` TLS cache was
  also pointer-only.

These are correctness and retained-CPU-state defects, not GPU-fence defects.
The first path can directly substitute old geometry after an address collision;
the latter two can misclassify or resolve a reused pointer before new-map
registries have warmed up.

## Implemented boundary

- Static mesh-data entries carry the current model-resource map epoch and reject
  cross-epoch hits.
- `ShadowValidationRuntime::reset()`, which is called only from the Present map
  transition, clears the process-global static mesh-data table after releasing
  the validation-runtime lock.
- pointer-shape and legacy geoset TLS entries carry the current model-resource
  map epoch and reject old entries lazily.
- No caster policy, CSM resource, Arena capacity, GPU retirement order or
  performance Consume path changed.

## Verification

- 70/70 static test modules passed.
- 18/18 Win32 Meson runnable tests passed.
- Win32 DLL build completed with `-j2`.
- `ninja -C build32 -n` reported no work and `git diff --check` passed.
- Candidate DLL: 34,311,195 bytes.
- SHA-256: `3DDA379EDC6F1AF42B262712BDA3A72AFD25B8246223986F6232606F9A79BADE`.

## Remaining physical gate

This candidate is not deployed and Issue #6 is not declared fixed. A cold-B /
A-to-B / A-to-B-to-A run must still prove zero old-epoch acceptance, bounded
retired-session residency, no first-publication delay and no persistent CPU/GPU
p95 regression.
