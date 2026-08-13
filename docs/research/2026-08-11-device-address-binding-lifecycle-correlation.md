# Device-address binding lifecycle correlation

The hidden `life_and_death_tdr` gate reproduced `VK_ERROR_DEVICE_LOST` while
directional replay remained complete and queue progress was current. The
bounded `VK_EXT_device_fault` record reported an invalid GPU read inside a
16 MiB virtual-address range whose recent binding-report history contained
only unbind events for multiple aliased images, memory objects, and a buffer.

`VK_EXT_device_address_binding_report` is intended to associate a faulting GPU
address with Vulkan objects during crash postmortem. The extension explicitly
allows aliasing, split or unmatched binding events, and reports reallocation as
an unbind/bind pair. Consequently, a historical overlapping unbind is evidence
but is not sufficient by itself to identify the current resource owner.

The development-only tracker now copies the callback object's debug name into
fixed atomic storage and correlates the newest event for each exact
`fault/object type/object handle/base/size` identity. It also attaches the
previous event and most recent prior bind sequence. The callback remains
bounded, allocation-free, lock-free, and logging-free. Release builds still do
not enable the address-binding feature.

Primary references:

- https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_address_binding_report.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceAddressBindingCallbackDataEXT.html
- https://docs.vulkan.org/refpages/latest/refpages/source/VkDebugUtilsMessengerCallbackDataEXT.html

This is a diagnostic candidate, not a TDR fix. One controlled hidden
reproduction is still required to identify the exact resource class and then
repair its ownership/lifetime contract.

The first lifecycle-enabled reproduction showed that CPU-side terminal
retirement appended many unbind reports before the delayed D3D fault query.
The tracker therefore now freezes the binding sequence at the first direct
driver `VK_ERROR_DEVICE_LOST` result and excludes later events from lifecycle
state. A later exact-key event may contribute only its bounded debug name. The
development build also preserves DXVK buffer/image/memory names even when the
normal capture debug flag is off. These changes prevent teardown activity from
being mistaken for the fault-time ownership state.

A subsequent run proved that the driver may emit the teardown unbind callbacks
inside the Vulkan call that ultimately returns `VK_ERROR_DEVICE_LOST`. The
post-call marker is therefore still too late. Queue submit/timeline wait and
WSI acquire/present/wait/fence paths now snapshot the binding sequence before
entering the driver and pass that token to the terminal notifier. The tracker
keeps the earliest direct-loss token, so later terminal reports cannot move the
cutoff forward into driver teardown.
