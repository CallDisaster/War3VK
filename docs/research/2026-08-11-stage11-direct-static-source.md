# Stage11 direct static-source development candidate

This candidate is restricted to the development shadow-observer build. It
binds the exact D3D9 REAL vertex/index buffer slice only when the source owner,
identity generation, allocation generation, content generation, range,
map epoch and device epoch are all valid and the semantic caster has no
dynamic-pose evidence. UP/ring sources remain on the existing exact-copy path.

The source `Rc<DxvkBuffer>` is retained by the cache entry, while DXVK command
recording retains resources used by pending command lists. Rebinding the same
buffer in a later shadow draw relies on the existing DXVK context hazard
tracking; this patch does not insert independent raw Vulkan commands or bypass
the normal resource-state machinery.

Primary references:

- Vulkan object lifetime requires objects referenced by recorded/pending
  commands to remain valid:
  <https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#fundamentals-objectmodel-lifetime>
- Khronos synchronization examples describe transfer/host writes becoming
  vertex-input reads through an execution and memory dependency:
  <https://docs.vulkan.org/guide/latest/synchronization_examples.html#_upload_data_from_the_cpu_to_a_vertex_buffer>
- `VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT` is the access type for buffers bound
  through vertex input:
  <https://docs.vulkan.org/spec/latest/chapters/synchronization.html>

This is an A/B candidate, not a Release default. It does not authorize direct
binding for dynamic sysmem uploads, skinned output, unknown generations, or a
different map/device epoch. Physical correctness and allocation-debt metrics
must improve before any broader admission is considered.
