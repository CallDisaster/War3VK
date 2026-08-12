# Visible geoset demand-fill dedup candidate

The demand-fill loop previously queried a missing geoset binding once in the
outer loop, again at the start of the publication helper, once after
publication, and then queried every record again in the final repair sweep.

This candidate records successful lookups for the duration of the function and
skips them in the final sweep.  The publication helper now assumes the caller
has already observed a miss and performs only the required post-publication
proof.  Missing-source dedup is a fixed 64-entry array and stops accepting keys
once the existing 64-capture budget is exhausted, so the helper no longer
allocates an unbounded node set for work it cannot admit.

All readiness, immutable generation and capture-budget behavior remains
unchanged.  The per-call byte vector carries no identity across frames.
