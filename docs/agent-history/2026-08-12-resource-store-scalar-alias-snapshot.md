# ResourceStore scalar alias snapshot candidate

`CaptureContract/ResourceStoreBuild` only needs a runtime-model pointer, a
model-resource pointer and a geoset count when it binds model aliases.  It
previously obtained those fields by copying complete model records, including
their `geosetPtrs` and `geosetDataPtrs` vectors, from both cache maps.

This candidate adds trivially-copyable scalar alias snapshots and uses them in
ResourceStore construction and the cold-start runtime sweep.  The full legacy
snapshot APIs remain available for consumers that actually require the pointer
arrays.  Geoset payload ownership, immutable generation, map epoch, alias bind
order and consumer coverage checks are unchanged.

This is an offline CPU-production candidate.  It does not enable experimental
culling or change shadow output, and its frame-time benefit still requires a
fixed-scene physical A/B.
