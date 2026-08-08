# Issue #6 map-scoped CPU hot caches — 2026-08-09

## Investigation result

The existing map transition already reset the primary semantic registries and
retired GPU-backed D3D9 containers behind the Arena completion fence. Four
smaller hot-path owners still survived the transition:

- the process-global Direct geoset snapshot lookup;
- the TLS unit-flags and material-signature caches;
- the TLS renderable-part palette-slot cache;
- the TLS terrain-bounds and generation index-slice caches.

Most consumers eventually rejected a stale immutable generation, but those
lookups could still retain map-A CPU payloads and, after Warcraft reused an
address, provide map-A classification, material or palette-slot data while
map B was rebuilding its authoritative records. The fixed-size index cache also
kept shared index vectors alive until its slots happened to be replaced.

## Implemented boundary

- The Direct geoset lookup is cleared by the Present-owned shadow-session reset.
  Published packets retain independent `shared_ptr` ownership, so this does not
  shorten command-list or GPU resource lifetime.
- Unit flags, material signatures, palette slots and terrain bounds now include
  the current `ShadowModelResourceCache` map epoch in both key and hit checks.
- The generation index-slice TLS cache lazily releases all old shared CPU vectors
  on the first Populate constructed for a different non-zero map epoch.
- GPU-backed allocations remain in the existing retired-session queue. No Arena,
  fence, CSM publication or producer admission policy changed.

## Verification

- 70/70 static test modules passed.
- 18/18 Win32 Meson runnable tests passed.
- Win32 DLL build completed with `-j2`.
- `ninja -C build32 -n` reported no work and `git diff --check` passed.
- Candidate DLL: 34,175,707 bytes.
- SHA-256: `EA549D1E5C7E211785CF21F7461C09CCCF49AFEC548D6B19CB93105597C89894`.

## Remaining physical gate

This candidate is not deployed and Issue #6 is not declared fixed. A physical
cold-B / A-to-B / A-to-B-to-A comparison must still prove that retired-session
gauges return to zero, CPU/GPU p95 does not remain elevated, the first complete
CSM appears within 60 render frames, and no old material/palette identity is
accepted in map B.
