# Direct group-slot single-pass candidate

The authoritative Stage11 current-draw path previously walked the decoded
vertex group-slot payload three times: validation/diagnostic hashing, stable
content hashing, and the final maximum palette-slot scan in BuildEligible.

This candidate derives both hashes and the maximum slot from the validating
decode pass. The diagnostic hash still includes the stream address, while the
stable geometry hash deliberately does not. Live-rebuild and non-authoritative
fallback paths retain their historical scan because they do not have a proven
decoded current-draw payload.

This is a CPU-production candidate only. It does not change caster selection,
palette contents, shader or descriptor ABI, CSM filtering, experimental
culling, or any Release Consume default. Offline tests cannot establish an FPS
gain or player-visible correctness; a fixed-scene A/B remains required.
