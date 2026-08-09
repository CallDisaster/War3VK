# 2026-08-10 — TDR P4: bounded device-fault text capture

This stage adds an optional, bounded diagnostic record for
`VK_EXT_device_fault`. It is not a TDR fix and does not alter the terminal
fail-stop, queue retirement, map/device epoch, shadow replay, or shader ABI.

When the extension feature and loader entrypoint are both available, the
`DxvkDevice` arms one preallocated capture object. Only a direct Vulkan API
result of `VK_ERROR_DEVICE_LOST` may trigger it. The terminal status is
published first, then a compare-and-swap allows exactly one
`vkGetDeviceFaultInfoEXT` call. Synthetic command-stream failures and queue
results that were merely overwritten by an already-visible terminal latch stay
on the publish-only path.

The capture stores at most 64 address records and 32 vendor text records by
value, together with the bounded standard descriptions and the query result.
It accepts `VK_SUCCESS` and `VK_INCOMPLETE`; oversized counts and the latter
are marked as truncated. Any other query result is also completed and recorded
without retry. The lost-device path does not allocate, lock, wait, log, or
write files.

No vendor binary is enabled, requested, retained, or written. The incident JSON
is created later by the D3D9 diagnostic path from an owning snapshot and always
reports `vendorBinaryEnabled: false`.

The included runnable uses a fake Vulkan dispatch function to verify the
one-shot, fixed-capacity, incomplete, failure, no-allocation, and by-value
snapshot contracts. Static and build validation does not substitute for a real
device-loss injection or a player foreground reproduction.
