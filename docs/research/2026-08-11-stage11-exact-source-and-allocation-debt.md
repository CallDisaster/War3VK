# Stage11 exact source and allocation debt

## Scope

This note records a development-only source-ownership experiment. It does not
change the Release defaults and it is not a physical-screen correctness claim.

The experiment retains the exact `DxvkResourceAllocation` behind a proven
source slice and tracks that allocation through the command list. Every range
is validated against the allocation's real byte size before a raw buffer view
is published. Unknown or mutable source generations keep the existing ordered
copy path.

Primary references used for the ownership boundary:

- [Vulkan object lifetime](https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-objectmodel-lifetime)
- [Khronos synchronization examples: CPU upload to vertex buffer](https://docs.vulkan.org/guide/latest/synchronization_examples.html#_upload_data_from_the_cpu_to_a_vertex_buffer)
- [Vulkan synchronization chapter](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)

These references authorize retaining the exact allocation and preserving
command order; they do not authorize reusing mutable Warcraft vertex contents
without a new copy.

## Hidden A/B evidence

All runs used the non-interactive isolated-desktop conductor. The input desktop
was `Default` before and after every run, the AutoTest-owned process exited, and
the pre-existing deployed DLL was restored to SHA-256
`79CA8DB4C73E47357E586CA3B6BE74F267F378AC13E2272F1D4F4722CDD8B2A4`.
OBS' process hook was present, so FPS is not a release-performance result.

The generation-proven static source route removed static Stage11 allocations
and cache evictions in the sampled life-and-death scene. It reduced, but did not
eliminate, producer-incomplete frames. Arena still reached roughly 384 MiB.

The follow-up exact `UploadPerDrawData` allocation route had zero candidates
and zero binds. Source classification then proved that all 4373 remaining
position-allocation requests were direct mutable REAL-buffer sources:

- dynamic position allocation requests: 4373;
- direct mutable requests: 4373;
- UploadPerDraw candidates: 0;
- producer-incomplete frames: 24;
- Arena peak: 383.771 MiB;
- new GPU events/incidents: 0.

Therefore the dominant debt cannot be fixed by retaining the UP ring. Directly
binding the mutable REAL buffer would be unsafe because later uploads may alter
the same contents before the shadow replay. The next candidate must preserve
the ordered exact copy while replacing per-caster destination-buffer creation
with a bounded page/slice allocator.

## Remaining gates

- A page allocator must not reuse a slice before all earlier GPU reads are
  ordered complete.
- Map reset must retire all page owners at the Present safe point.
- Capacity refusal must fall back to the existing exact path or fail closed;
  it must not increase Arena limits or the 32-create budget.
- Hidden runs can establish relative counters and stability only. A visible
  physical-screen pass remains required before any Release default changes.
