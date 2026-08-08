# Issue #6 semantic registry reset — 2026-08-09

## Proven regression source

`model::Shutdown()` only disabled the model hooks. It did not clear any of the
semantic registries. Production also deliberately disables their end-frame
sweeps. Consequently the following process-global maps retained map A raw
pointers, handles and pose palettes indefinitely:

- `ModelRegistry` and `ModelInstanceRegistry`;
- `PoseRegistry` and `AttachmentRigidRegistry`;
- `ShadowObjectRegistry`;
- the last published `ShadowRuntimeContractCache` bundle.

The old transition explicitly called `War3Renderer::EndFrame()`, which could
publish one last map-A contract, followed by `BeginFrame()`, which only advanced
the registries and did not erase them. This was both an address-reuse
correctness risk and a source of monotonically growing CPU lookup state.

## Implemented reset

- `War3Renderer::ResetMapSession()` replaces the old transition-time
  `EndFrame()/BeginFrame()` pair.
- Frame-local RenderObject and VisibleRenderable double buffers publish an
  empty inactive slot. The previously published slot is not mutated while an
  asynchronous reader may still be using it.
- All persistent semantic registries clear and release their raw-pointer maps
  under their existing exclusive locks. Aggregate counters and same-frame
  dedup state reset with the maps, while frame and mutation generations advance
  to invalidate cached value projections.
- The runtime contract atomically publishes a new empty manifest/resource/
  pose/attachment bundle. Readers already holding the old bundle retain it via
  `shared_ptr`; new readers cannot acquire map-A contents.
- GPU resource retirement remains in the existing Arena/session fence queues.
  This change only destroys CPU semantic records and does not clear in-flight
  GPU backing.

## Verification

- Cross-map static lifecycle suite: 12/12 passed.
- Full static suite: 68/68 modules passed.
- Win32 Meson runnable suite: 17/17 passed.
- Win32 DLL build passed; `ninja -C build32 -n` reported no work.
- `git diff --check` passed.
- Candidate DLL: 33,874,721 bytes.
- Candidate SHA-256:
  `7BED6088560282F284E31630260EBA1BCDA7DBF7236EA2CBE91998C8DA04FBFD`.

The DLL was not deployed and no physical A-to-B comparison is claimed. The
remaining Issue #6 work is the runtime resource census and the cold-B versus
A-to-B/A-to-B-to-A stability and performance gate.
