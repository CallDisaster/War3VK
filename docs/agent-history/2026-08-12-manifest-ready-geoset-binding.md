# Manifest ready-geoset binding candidate

Visible-unit manifest hydration only consumes geoset/model pointer identity,
model key and geoset index.  Its cache lookup previously materialized a full
`ShadowGeosetResourceRecord`, deep-copying immutable positions, indices, UVs,
group slots and matrix-group arrays for every lookup.

This candidate adds a trivially-copyable ready-binding projection.  Pointer
lookups still require both a ready alias and its ready canonical data
publication, and apply the same alias metadata precedence.  Data lookups still
require the canonical publication to be ready.  No geometry bytes, generation
or readiness rule is changed.

The legacy copying APIs remain intact for callers that consume geometry.  This
candidate only removes unneeded payload copies from ManifestCopy/demand-fill;
its frame-time benefit and visual behavior still require a fixed-scene A/B.
