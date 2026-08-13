# Direct packet single palette owner candidate

## Problem

For each successful skinned CurrentDraw packet, BuildEligible decoded one
palette/group payload and then kept two owning copies: one in
`ShadowDrawPacket`, and one in `CurrentDrawAuthoritativeSample`. The packet copy
is the authoritative input used by append and lease paths; the sample copy
duplicated matrix and group arrays for every eligible part.

## Change

- A successfully built packet is now the sole owner of decoded palette and
  vertex-group payload.
- The accompanying sample retains its immutable contract, counts, hashes,
  resolve status and stable group identity, which are the fields used for
  selection, exact-owner checks and diagnostics.
- Append already prefers packet-owned palette/group data whenever the packet
  proves an authoritative skinned contract; this behavior remains unchanged.
- Palette-root diagnostics now read the exact effective palette selected by
  append instead of requiring a duplicate sample vector.
- Unresolved/non-authoritative fallback branches retain their historical sample
  behavior; the change only applies once payload ownership has moved into the
  packet.

## Boundary

No producer eligibility, geometry, pose, alpha, blocker, map/device epoch,
lease lifetime or replay rule changes. Submit-permutation verification still
compares the complete packet-owned payload. The candidate is not deployed and
needs a fixed-scene A/B before a frame-time gain can be claimed.
