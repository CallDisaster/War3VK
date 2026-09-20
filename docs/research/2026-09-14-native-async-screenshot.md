# Native asynchronous screenshot — source contract (2026-09-14)

Status: opt-in candidate with limited isolated runtime evidence below, not a
stable/default or long-term deployment. Does not resolve the player's 00:26
nvlddmkm incident. The legacy recording-owner route remains intact; the separately
opted-in persistent route no longer depends on recording. Actual readback/LockRect
semantics and DXVK fences stay unchanged.

## Proven native boundary

Game.dll E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A,
preferred base 0x6F000000. IDA read-only disassembly identifies Game+175530 as
the registered screenshot-event callback: if the first event DWORD is 0x212,
set global Game+BE3A38 to one; otherwise retain it; return one in either case.
Its complete 27-byte SHA1 is f2a84946771dc3853231d4a4116fb582cbe96853.
Only HIGHLOW operands +2/+22 may be normalized in a local byte copy for ASLR.

FrameRender+175550 consumes that flag after rendering the scene/UI, invokes
the device capture path and TrySaveScreenshot+174FD0, then clears the flag.
TrySaveScreenshot synchronously writes a 32-bit TGA under the executable's
Screenshots directory (not Documents). It returns actual save success. We
intercept the earlier event, not the saver: return one means event handled,
never a fabricated successful save. Other events and unavailable/unsupported
async owners retain the original trampoline. No keyboard remapping/global hook.

## GPU/CPU ownership and visibility

Primary references:

- [Khronos host readback synchronization example](https://docs.vulkan.org/guide/latest/synchronization_examples.html#cpu-read-back-of-data-written-by-a-compute-shader).
- [Khronos timeline counter query](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetSemaphoreCounterValue.html).

Mapping to this checkout: copyImageToBufferHw calls syncResources, whose
releaseResources records TRANSFER_WRITE -> buffer.info().access. Staging info
includes HOST_READ/HOST stage. DxvkBarrierBatch::finalize emits the deferred
host barrier before submission; copyImageToBuffer restores/tracks image access
through the existing DXVK hazard machinery. The private timeline semaphore is
signalled by that command list. The worker requires VK_SUCCESS, healthy device
and counter >= the exact nonzero slot target before reading mapped bytes.
Two or three elapsed frames are never proof. HOST_VISIBLE|HOST_COHERENT memory
is mandatory; no missing noncoherent invalidate is silently tolerated. Mapped
DxvkBuffer storage is non-relocatable (canRelocate checks mapPtr).

Three private slots, at most 32 MiB each; width/height <=8192. 2560x1440 uses
42.1875 MiB for three staging images. Prewarm/reuse buffers at Present, no
resource creation for the steady-state request. One reusable worker performs
counter queries, row conversion and file IO. No LockRect/GetRenderTargetData,
GPU wait, device-idle, forced flush or image encoding on the render thread.
The normal Present submission remains responsible for scheduling the copy.

Slots: Retired -> Preparing -> Free -> Requested -> Preparing -> Submitted -> Retired.
Present revalidates dimensions before publishing Free again. Acquire/release
publication separates native request, render ownership and worker ownership.
Any ambiguous submission failure quarantines the slot, never early-recycles.
Busy/unsupported requests are not deferred to an arbitrary later rendered
frame. Full queues are explicitly rejected, not turned into a synchronous path.
Format admission is single-sample BGRA8 UNORM/SRGB, full image, mip/layer zero;
other formats/MSAA retain the original screenshot path. No DXVK core changes.

## Compatibility and lifecycle

Opt-in DXVK_WAR3_ASYNC_SCREENSHOT=1, independent of profiler recording.
Copy the first Present's backbuffer after the native request, before diagnostic
capture, fallback ImGui and swapchain rotation. This preserves the native
pre-Present pixel boundary, not a promise to include third-party post-Present
effects, a hardware cursor, or monitor gamma. Multiple accepted requests in
that render interval refer to the same image; none use a later frame as a
substitute. TGA keeps the native directory/prefix, opaque alpha and explicit
top-left orientation. UTF-16 file APIs, CreateNew temp and non-overwriting
rename protect existing screenshots. Saved is logged only after successful
close/publication; failures and queue rejection are distinct.

Reset cancels unsubmitted requests. Already submitted snapshots retain their
own buffer, dimensions and completion token and may finish saving the old,
correctly selected frame; they never read a new backbuffer. Device loss blocks
read/publication and quarantines uncertain slots. Ordinary teardown unregisters
the owner, stops/joins its worker without a GPU wait; queued CS commands and
command lists retain their resources. During OS process termination the owner
is intentionally abandoned, matching DXVK's no-wait termination boundary.

## Acceptance still required

Synthetic tests: exact relocated hook bytes, queue bounds, slow completion,
query/device failure, reset/shutdown ownership, row order/channel/alpha,
dimensions/overflow, file collision/write failure and exact copy-before-signal.
Compile and exact DLL no-work, source SHA inventory. Then separately validate
native PrintScreen -> decodable current-frame TGA, 2560x1440 isolated desktop,
profiler on/off, burst/full queue, fullscreen/windowed, resize/reset, shutdown,
GPU events/incidents and live restore. CPU/static/compile proof is not proof
that native event dispatch and real GPU readback have worked in the player game.

## 02:03 isolated checkpoint

Candidate 3D1F55E511CC57E9ABE8B135FF75D0A679C13576F3AE7F522B48AD61FD029576
(34,548,768 bytes) passed two fresh-process 2560x1440 isolated transactions:
native_async_on_r1_20260914 / native_async_off_r2_20260914. Each exercised the
actual installed native callback through the render-owner internal command:
one image, four-event burst (three identical images and one explicit rejection),
then a recycled-slot image. Ten real TGA images decoded correctly. The on-run
single-preview view was visually checked: correct scene/UI orientation and
color, no obvious black patch; one image is not a flicker absence proof.
No keyboard events were injected: physical PrintScreen dispatch remains a
player-acceptance item. Both exact retained processes stopped, desktops closed,
video restored to original1280x720/144Hz and live EE90 restored; zero new GPU
events/dumps and zero residual relevant processes. Native sync elision remains
recording-dependent; screenshot itself does not.

Worker save times: on79.8932..90.5688ms, off99.6521..115.729ms. These are background
encode/file work, NOT main-thread stalls; enqueue-to-observed-ready includes
the worker's own prior saves and polling, NOT pure GPU-copy duration.

Follow-up narrows one parameter: request HOST_CACHED readback memory, preserving
HOST_COHERENT. [Khronos memory properties](https://docs.vulkan.org/spec/latest/chapters/memory.html)
explicitly distinguish CPU caching from coherence. DXVK's property-mask table
may fall back from cached+coherent to coherent, never to noncoherent. Verify and
log storage()->getMemoryProperties(), since buffer.memFlags() reports requested
flags rather than actual allocation flags. This candidate needs a fresh run.

State clarification: worker completion publishes Retired, not directly Free.
Present must rewarm/revalidate the slot for current dimensions before Free.
This is essential when a pre-reset snapshot completes after the new resolution.

## Cached-memory checkpoint and independent-owner follow-up

05FAE7F0E344F3F315562938BFB2B31B7BA7491BAB762F2883241825EC5417A9 /
34,548,768 bytes passed cached-on-r3 and cached-off-r4 fresh isolated transactions.
Both preserve ten decoded images, zero GPU events/dumps, exact EE90 restoration.
Actual staging memory flags=14 (HOST_VISIBLE|HOST_COHERENT|HOST_CACHED).
On-r3 worker-save15.1117..18.1319ms versus prior on-r1 79.8932..90.5688ms;
this is a local background-save comparison, not a foreground FPS claim.

The next opt-in `DXVK_WAR3_NATIVE_FRAME_SYNC_PERSISTENT=1` additionally requires
`DXVK_WAR3_NATIVE_FRAME_SYNC_ELIDE=1`. It leaves the legacy recording-owner route
unchanged when absent. Persistent owner is established only by a successful
real Vulkan Present on the same swapchain/thread, also matching the authoritative
WorldFrame thread. Present entry, Reset and failure revoke readiness; a different
swapchain/thread or destruction permanently faults the process-local lease.
No recording/QPC dependency grants the permission. Native full-function identity,
request==1, readable capture==0 and non-nesting remain mandatory. All pixel
consumers and DXVK's GPU fences, frame-latency wait, Present and return values stay
canonical. Unknown/unavailable async capture still falls back to the native saver.

Internal-test-only counters and owner-command QPC queries allow recording=off,
timeline=off verification without inventing report frames. Two query boundaries
enclose N completed Presents; elapsed/N is a long-window throughput estimate,
not exact per-frame intervals or p99. It excludes our requested screenshot burst.
All gates remain opt-in, not stable/default/incident-fix acceptance.

## Recording-off / timeline-off runtime result

3D7931F621AB9FB2B9C9A1C1A656876D4B39BAAA18FB2BA59637318B2478DEE8 /
34,553,982 bytes completed B1/A1/B2/A2 (B-A-B-A, NOT ABBA), each fresh isolated
2560x1440/full_default, thirty-second no-screenshot query window, actual recording
off and timeline off. B enables persistent; A disables it with the same DLL.

| Run | Presents | Ordinary sync calls elided | Query-window elapsed/N ms |
| --- | ---: | ---: | ---: |
| B1 | 5420 | 5420/5420 | 5.541840 |
| A1 | 3411 | hook not installed | 8.804407 |
| B2 | 5631 | 5631/5631 | 5.332853 |
| A2 | 3196 | hook not installed | 9.398102 |

Mean of two runs: A9.101254/B5.437346ms, -40.257% elapsed/N, +67.384% inverse
throughput. This repeated isolated result is NOT foreground FPS, exact per-frame
latency, p99, or formal ABBA/workload-equivalence/product acceptance. The A route
does not install the sync Hook (timeline off), so its zero counters are missing
instrumentation, NOT zero native work. B hook elided11051/11051 ordinary calls
with retained0. Each run subsequently saved five independently decoded images;
all four had zero GPU events/dumps/residual processes and restored EE90. No
recording flag is needed for candidate behavior. Physical key/fullscreen/long-run
and the earlier incident remain unclosed.
