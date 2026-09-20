# Issue #8: transient black fissure investigation, not a closed fix

## Evidence boundary

User authorizes local implementation/build/isolated1440p tests. GitHub read-only,
no comments posted. [Issue8](https://github.com/CallDisaster/War3VK/issues/8) and
[original report](https://github.com/CallDisaster/War3VK/issues/4#issuecomment-5301627806)
separate random brief black fissures from the earlier building/tree shadow loss.
No reproducible offending framebuffer or pass attribution is available yet.
CPU timing cannot diagnose undefined pixels. A correctly decoded screenshot or
a quiet five-minute run cannot prove this issue absent. An internal framebuffer
artifact would distinguish rendered black pixels from display scanout tearing;
its absence without continuous coverage cannot rule either out.

## Confirmed source defect: AA copy image first-use layout

`War3AAPass::ensureResources` allocates m_colorCopy with `createImage`; that API
constructs DxvkImage without issuing initialization commands. DxvkImageCreateInfo
initialLayout defaults to UNDEFINED. The previous `copyColorToInput` nevertheless
declared the first barrier's oldLayout SHADER_READ_ONLY_OPTIMAL, including after
resize/recreation. Preferred `colorInfo.layout` is not a GPU transition.

[Khronos synchronization specification](https://docs.vulkan.org/spec/latest/chapters/synchronization.html)
requires an image layout transition to start from the current layout or discard
contents with UNDEFINED. The fix uses UNDEFINED/discard for the destination of
the full intermediate-image overwrite, including first use and recreation; it
does not discard the original scene target. ensureResources creates the exact
source extent before the full copy. The post-copy SHADER_READ_ONLY transition
remains. The existing fragment-read -> transfer-write
execution dependency is retained, including repeated frames; no queue wait,
full-pipeline drain, extra image copy or AA disable is introduced.

This is a deterministic first-use contract defect, NOT yet the cause of random
steady-state issue8. It cannot explain arbitrary steady-state frames without
evidence of resource recreation or an additional hazard. Ordinary reuse remains
on the same barrier/copy sequence, now discarding overwritten intermediate contents.
No class-layout/header change is needed. Four source/static checks cover allocation
vs initialization, exact extent, copy/barrier order and unchanged AA calls.

## Other inspected paths / next discriminating evidence

- Shadow temporal history already advances only after the complete receiver /
  visibility / motion-vector write closure in current d3d9_war3_shadow.cpp.
  Do not claim this old correction as a new fix; DirectInline frames do not
  authorize blaming temporal history.
- Volumetric composite uses LOAD for partial scissor and DONT_CARE only for full
  output. FXAA/SMAA final passes use full destination viewport/scissor and no
  discard in their shaders. DONT_CARE alone is not evidence of a black-strip bug.
- Volumetric24-slot uniform ring relies on frameSerial and MaxFrameLatency20.
  Its one-use-per-frame/retirement assumptions still need runtime evidence; no
  speculative barrier removal or rewriting of that unrelated path was done.
- Local Vulkan SDK validation layer is AMD64/0x8664, not Win32. It cannot load in
  Warcraft's PE32 process. [LunarG release notes](https://vulkan.lunarg.com/doc/view/1.4.304.0/windows/release_notes.html)
  confirm removal of prebuilt32-bit layers. No validation-enabled pass is claimed;
  a separately built compatible Win32 layer remains a useful next diagnostic.
- Next collection should keep bounded consecutive full-resolution frames and
  exact sequence/drop counts. Sparse successful screenshots must not become a
  false claim of continuous coverage. GPU completion polling must remain real,
  never a fixed two-frame assumption, and capture must not reinstate LockRect.

Stable acceptance still requires offending-frame evidence, causal pass isolation,
targeted correction and repeat/visual/integrity/GPU regression gates.

## Bounded consecutive-frame collection candidate

Internal-test-only `screenshot.flicker_burst` arms exactly three successive Present
captures through the proven asynchronous slots. It requires all three slots free,
the known WorldFrame thread and the internal API gate. Present schedules one
capture per invocation; queue conflict/Reset cancels instead of waiting or silently
replacing a missed frame. Each TGA filename carries the exact service Present ordinal.
The offline reader requires three adjacent ordinals and recomputes every file SHA.
It detects dark middle pixels whose preceding/following pixels are bright and
similar, reports candidate count/bounding box, and always leaves bugConfirmed=false.
Five tests include a one-pixel line, persistent darkness, camera-neighbor mismatch,
extent/ordinal rejection and the actual C++ one-request-per-Present schedule.

At most24 bursts (72 frames, about1.0GiB TGA output) are allowed per runner call.
They are sparse bursts, not continuous frame coverage or an absence proof; all
thresholds are explicitly heuristic. No new shader, GPU wait, full-rate disk stream,
global input or post to GitHub is added. Runtime validation follows separately.

## 02:55 combined candidate checkpoint

A96B958B56B0BCFB1B09E4F217821B714CE963C65272F1DD72E9C1A6A829B866 /
34,562,451-byte PE32/i386 is the source-consistent combined candidate. Final
build-r8-final.log48/48 and exact-DLL no-work passed. The temporary AA header
field was removed and its original CRLF restored; header is clean/HEAD-equivalent.
The final AA correction is one oldLayout change plus its explanation, no class
layout change. 66 targeted Python tests, two actual Win32 core executables and
seven-file py_compile / diff-check passed. Earlier unittest invocation from the
wrong import root failed module imports; the corrected PYTHONPATH run is the
reported66-test result, not an ignored failing test.

- async_fissure_smaa_bursts_r1_20260914: recording on, AA env5, 41 images including
  twelve exact-adjacent three-frame bursts. Strict report3351frames, duplicate0,
  PostFX/AA GPU total879.198ms/calls3324; AA was actually active. Viewed the new
  single-preview: scene/UI/colors/shadows are present and correctly oriented,
  no obvious black fissure; preview is not a one-pixel absence proof.
- async_fissure_fxaa_bursts_r2_20260914: recording off, timeline off, AA env1,
  same41-image pattern. Persistent sync6444/6444 calls elided, retained0 in the
 30-second query window. Its4.66045ms is a DIFFERENT AA configuration and must
  not be substituted into the previous SMAA B-A-B-A gain comparison.
-72 exact consecutive frames across24 sparse bursts: zero >=4-pixel dark-middle
 recovery candidates. SMAA burst02 had one pixel at[284,341], below heuristic
 threshold; retained in analysis, not silently dropped. No continuous coverage,
 no proof of absence, and no evidence yet tying the source defect to issue8.
- Both runs: GPU event/dump/residual-process0, exact native process settlement,
  desktop and original video restoration, byte-exact EE90 live restore. These
  are isolated functional checks, not foreground/long-term visual acceptance.

Evidence identities (under AutoTest/artifacts/native_async_screenshot_20260914):

| Relative file | Bytes | SHA256 |
| --- | ---: | --- |
| async_fissure_smaa_bursts_r1_20260914/receipt.json | 32895 | 50C8D7D0C0432DA80C518008F7BB477143A7D9F10071107B93378D2967F732BA |
| async_fissure_smaa_bursts_r1_20260914/transient-black-analysis.json | 27564 | 463725D5E1A7D4D8B2F23A36499DEA825401CDD4F50961FFEF2DF59D5606BE89 |
| async_fissure_smaa_bursts_r1_20260914/war3_perf_report_auto_2026_09_14_02_51_53.html | 6273575 | F03FE9169E29C12DE7025380F0EAED733AFA5D600AB5D6095CE56A7B4F349221 |
| async_fissure_fxaa_bursts_r2_20260914/receipt.json | 33230 | 0371EAFD5DC169F4E17B5C652BC541C53F9BE4E4BA77DB2F83B20AFCB8DF4F59 |
| async_fissure_fxaa_bursts_r2_20260914/transient-black-analysis.json | 27474 | 990C7055A70CF64A8CD13553ADF1DC325B698033629C5F1000CB0B07A4187F46 |

Each receipt/analysis pins every image's full path, size and SHA; runner source,
candidate and original-live backup are immutable per-run files. Latest combined
candidate is opt-in and offline after restoration; do not automatically replace
the player's EE90. Subsequent evidence-led continuation is scheduled on this
same task hourly until2026-09-14 23:59 Asia/Taipei (automation warvk-2), with the
same resource/restore/GitHub-read-only boundaries. It is not a stable acceptance.

## 04:00 continuation: volumetric image initialization (offline candidate)

Read-only preflight: HEAD88089cdf on the native-shadow stable-baseline branch,
394 dirty paths; build32 remains A96B/34,562,451 and player live remains
EE90/34,495,769. Game/compiler/owned-runner processes were zero. The protected
player trial is not ended merely by zero processes; no deployment is planned
for this checkpoint.

The same first-use defect is present in the volumetric pass, independently of
the AA finding. `DxvkDevice::createImage` only constructs `DxvkImage`, whose
`VkImageCreateInfo.initialLayout` comes from the default UNDEFINED. Neither
`info.layout`, view creation, descriptor binding nor `ctx->track` executes an
initialization transition. `beginExternalRendering` flushes existing DXVK
barriers but cannot initialize resources subsequently created by this pass.

The source correction is confined to volumetric cpp plus the private helper
signature in its header; no fields/class layout or shaders change:

| Resource | Previous first-use defect | Candidate contract |
| --- | --- | --- |
| private color/depth copies | declared shader-readable before first full copy | UNDEFINED discard, existing shader-read to transfer-write dependency and post-copy transition retained |
| private effect target | declared shader-readable before first write | discard before full legacy render-area CLEAR, or full in-bounds Froxel writes |
| base/refined directional guides | declared shader-readable before first compute write | discard; both compute and fragment readers remain in the source scope; every in-bounds early-out writes output |
| Froxel current + two histories | allocated UNDEFINED, bound GENERAL without a transition | one three-image UNDEFINED-to-GENERAL barrier only after allocation/recreation, retained by the same command list |

Cached Froxel identities return before the new initialization barrier; old
history is never discarded every frame. The existing compute reuse barrier,
map/device/frame/distribution/camera history gates and shader `historyValid=0`
early return remain. Fresh history pixels need no clear because they are not
read before a complete temporal write, but the bound image layouts must still
be valid. The new helper receives the existing CS-owned command list; it creates
no context, host/GPU wait, queue flush or extra copy. Existing source color/depth
targets are not discarded.

Primary basis: [Vulkan image layout transitions and execution dependencies](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions)
and [Khronos synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html).
UNDEFINED permits content discard, not concurrent reuse while an older reader
still runs. The code preserves those dependencies. This is a source/API-contract
finding, not Win32 validation-layer output or proof that issue8 is caused by
volumetrics. It can affect allocation/recreation paths; random steady-state
fissures and the earlier nvlddmkm153 incident remain unexplained.

New `test_volumetric_image_layout_static.py` has twelve source checks, including
mutation rejection of the original copy layouts and removal of a reader scope,
three-image initialization ordering/lifetime, cached history preservation,
full-coverage shader early-outs and no new host wait. Together with the four AA
layout checks:16/16 pass. DLL and larger targeted results are recorded after
their own execution; no runtime acceptance is inferred from these static tests.

### Offline build checkpoint

- Exact DLL dry-run3edges: d3d9_device.cpp, d3d9_war3_volumetric_light.cpp,
  DLL link. Actual3/3, parent PID44604 BelowNormal, -j2; no generators/shaders/
  static libraries/other targets. Only pre-existing JASS OPCode reorder warnings.
- New candidate27B736056F7D9362F29BBFA610D78EBA304B0E812D0250496B1AD9269250DB54,
  34,562,483bytes, objdump pei-i386/i386 and optional-header010b PE32. Exact DLL
  no-work,78 targeted Python tests,8-file py_compile and diff-check pass. The
  initial architecture-tool invocation used a nonexistent prefixed objdump path;
  it was not counted as a pass. The installed E:/Dev/MinGW/bin/objdump.exe then
  provided the architecture and PE32 evidence successfully.
- Ignored immutable candidate and build.log are under
  `AutoTest/artifacts/volumetric_first_use_layout_20260914/`. Candidate was copied
  fail-if-exists, source/target exact size/SHA match. Build.log2279bytes /
  489495F70DC01176FC960FE1A911000DD9E8200521AD12069F5E8A241FD660B1.
- Strictly reread the existing A96B SMAA report (same F03FE9...349221,duplicate0):
  VolumetricLight3333calls but totalGpuMs=0, totalCpuMs=1.067. This does not prove
  a volumetric dispatch; those prior bursts cannot validate today's change or
  establish volumetrics as the cause of the user's default-scene fissure.
- No new game process, deployment, GPU/desktop use or physical input. EE90 live
  unchanged and related processes zero. This candidate remains offline. Next
  useful runtime evidence needs actual admitted volumetric/froxel work with a
  compatible Win32 validation layer or targeted lifecycle capture, and must not
  borrow the earlier AA-only burst acceptance. UBO-ring retirement remains an
  unproven separate audit item, not modified speculatively here.

## 05:00 continuation: default receiver copies and retained CSM contents

Preflight397dirty, all397 prior checkpoint file hashes unchanged; build27B7 and
liveEE90 identities unchanged, related processes0. No new game/desktop or GPU
transaction is started. The installed explicit-layer registry checks found no
compatible32-bit validation registration; no validation run is claimed.

Further source audit found the same missing initial-layout contract in the
ShadowReceiver and SSAO private color/depth copies, plus the newly allocated
directional CSM/caster-mask and volume-sun images. The old A96B SMAA report has
actual ShadowCopy3338calls/387.044GPUms and ShadowMap3339calls/2638.932GPUms.
Unlike volumetrics/SSAO (GPU0 in that report), the receiver/map paths were doing
real GPU work. This only establishes path relevance, not a causal bad frame.

Important distinction discovered in `renderShadowMap`: image attachment
transitions precede replay pipeline/material preparation. If preparation fails,
the code restores read layouts and retains the old complete image. Therefore
changing CSM's per-frame oldLayout to UNDEFINED would destroy that safety
contract. It is deliberately NOT done. Point shadows likewise retain all faces
outside their updateMask, and their existing one-time cube initialization stays
unchanged.

Candidate correction (no shader or class-layout changes):

- Four private copy destinations discard contents immediately before their full
  overwrite, retaining source scene images, fragment/compute reader stages and
  post-copy visibility barriers.
- A local `InitializeNewShadowImageLayout` records UNDEFINED-to-view-layout for
  a new image's full view subresource range, keeps it in the command list, and
  initializes CSM/caster-mask only after complete candidate allocation and before
  transactional publication. Both cache-hit branches return without initialization.
- Volume-sun's two new layers receive the same one-time transition after view
  creation. All callers pass the existing CS-owned command list through private
  helper signatures. No new context, wait, flush, upload, clear or policy is added.
- Existing CSM read-to-attachment transitions, prepare-failure rollback, held-map
  reuse, point-shadow partial updates and scene-copy dimensions are untouched.
  Non-default SSAO MSAA/view-extent admission and other TAA/outline first-use
  paths remain separate audit items; this patch does not claim an exhaustive
  image-layout repair across every renderer.

The [Khronos layout/dependency contract](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-image-layout-transitions)
is the same primary basis as above: declaration is not initialization; discard
must not erase intentionally retained pixels or remove previous-reader ordering.
Ten new source tests include mutation rejection of a per-frame CSM/mask discard,
cache/new-candidate distinction, complete view ranges/lifetime, context forwarding,
and point-face retention. Together with prior AA/volumetric checks:26/26 pass.
This is still source/static evidence, not Vulkan validation or issue8 closure.

### 05:00 build checkpoint

- Header signature dependency fan-out was50edges, not a two-object-only build:
 47 DLL objects + dxso_options.cpp + libdxso.a link + exact DLL link. Actual50/50
  set equals preflight, parent41696 BelowNormal/-j2, exit0 and exact DLL no-work.
  No generators/shader compilation or additional requested targets. Pre-existing
  JASS OPCode reorder and ExecBatch unused-variable warnings remain.
- New combined build/frozen candidate992BFEAE6843E64A2146CFC42E75823F29F38D55B98168D83E9F2096659E85B0,
 34,562,810bytes, PE32/pei-i386/i386. Ignored fail-if-exists snapshot under
 `AutoTest/artifacts/shadow_first_use_layout_20260914/`; preflight.txt and
 build.log pin the entire edge enumeration. build.log26045bytes /
 E5002A7A3D7236DE7107C4B6CEF3F7AB96BAB5F692C253A23199A486829027FE.
-88 targeted Python checks,9-file py_compile and diff-check pass. Extracted
 function bodies of renderShadowMap/renderPointShadow/renderMotionVectors/
 renderShadowVisibility remain text-equivalent to current HEAD; the only
 shadow.cpp edits forward ctx at four allocation call sites. Not a full-tree
 regression, Win32 Vulkan validation, or runtime acceptance.
- No new game/deployment/input/desktop/GPU work. Player EE90 remains protected.

## Higher-priority next hypothesis: external images vs allocator relocation

This is a newly identified SOURCE-REACHABILITY hypothesis, not a runtime cause
claim. It is not fixed by the first-use patches above, including992B. Do not
promote that DLL to a stable baseline on the strength of its successful build.

Verified local chain:

1. `DxvkImage` ordinary allocation constructor starts `m_globalLayout` from
   initialLayout (UNDEFINED). `handle()` only returns the raw VkImage; neither it
   nor the view handle pins storage. `canRelocate()` accepts unmapped, unshared,
   non-sparse images without stableGpuAddress. These private device-local pass
   images are created in that class of allocation and register with the allocator.
2. In the inspected AA/SSAO/volumetric/shadow resource/render files, raw
   `cmdPipelineBarrier`/draw/copy calls have no matching `trackLayout` or stable
   address request. `DxvkCommandList::cmdPipelineBarrier` only records the Vulkan
   command/stat counter; `track(image, access)` retains lifetime/access and does
   NOT change `DxvkImage::queryLayout/isInitialized` metadata. A real GPU layout
   transition alone therefore does not make this software state initialized.
3. `DxvkMemoryAllocator::enableDefrag` defaults to Auto and returns true for the
   normal non-Intel-Mesa path. Fragmentation/pressure, not every frame, selects
   chunks. Resources can be queued by `moveDefragChunk`; `endRecording` calls
   `relocateQueuedResources` after ending current commands. The intermediate
   `DxvkRelocationList::addResource` only keys/enqueues the resource under a mutex;
   it does not filter on initialized layout or add external-image protection.
4. `relocateResources` only builds image copy regions for subresources whose
   `isInitialized` is true, yet replaces backing via `invalidateImageWithUsage`.
   For an externally populated image still recorded as UNDEFINED, this chain
   would omit its populated pixels from the migration. It also forwards the
   resulting regionCount to raw vkCmdCopyImage2; an all-uninitialized selection
   can therefore have additional validation consequences. Selection of such an
   actual WarVK image has NOT been observed in a trace yet.

This could explain an intermittent event after memory activity, and a temporary
recovery when a later frame rewrites the image, more directly than a cold-start
layout omission alone. It does not prove that the user's fissure or nvlddmkm153
incident followed this chain. Hardware scanout, geometry corruption and other
image paths remain unexcluded.

Next discriminating work: correlate exact resource cookie/handle, real-layout
publication, software initialized state and relocated subresource count. Then
use a tightly scoped per-process defrag-off comparison only as a diagnostic,
not a default fix. Do not blindly call trackLayout without a real corresponding
barrier, globally disable defrag, force GPU-idle, or pin every allocation without
a bounded memory/lifetime contract. Preserved CSM, point-cube partial faces and
TAA history need the complete synchronization+software-tracking contract, not
metadata fabricated merely to make migration copy something.

## 06:00 bounded relocation observer (diagnostic only)

Preflight:402 dirty paths match the previous receipt, build992B and liveEE90
unchanged, related processes0. A default-off `DXVK_WAR3_IMAGE_RELOCATION_AUDIT=1`
observer now records three selected external shadow-image sites (new-layout,
color-copy-final, depth-copy-final), the actual copy descriptor immediately
before allocator invalidation, and completed CPU recording checkpoints.
It does not change any GPU command, software layout, storage, allocator policy,
queue wait or fence. Removing its include/two calls from dxvk_context.cpp must
recover HEAD exactly; the static test checks this, not merely string presence.

The TLS observer has64 image slots,96 external records,128 relocation records
and64 checkpoint records per thread. Checkpoints occur at submission1 and every
256 thereafter. Overflow/suppression/log errors are explicit; the final interval
after the last checkpoint remains unclosed. This is not GPU completion evidence,
continuous bad-frame coverage, or proof that the allocator never relocates.
The native async runner gains only a default-off env option, validated in both
requested/effective matrices; deployment/ownership/EE90 restore are unchanged.

The strict offline reader correlates thread key + resource cookie + old handle
and command-recording order. Only an actual full copy label followed by a zero-
region relocation can mark recordingCopyOmissionObserved; image initialization
alone cannot. It always leaves rootCauseConfirmed/productAccepted/GPU completion
false. Empty/partial/duplicate records and regressing counters are rejected.

Primary API check: [VkCopyImageInfo2](https://docs.vulkan.org/refpages/latest/refpages/source/VkCopyImageInfo2.html)
requires matching actual image layouts and a positive regionCount. This observer
does not hide a zero-region descriptor or pretend to be the Vulkan validation
layer. Nine new source/synthetic tests plus42 related tests passed (51 total);
build and real relocation evidence follow separately. No product fix is claimed.

### 06:16 real observation and exact restoration

Diagnostic D55CAD8126AD80561C231FD9555A17F6B9D46DD56271C2E87A91B9B1391D3BAB,
34,568,036bytes, PE32/i386: exact build4/4 (two objects + libdxvk.a + DLL),
parent29896 BelowNormal/-j2, exit0/no-work;97 targeted tests pass.
One fresh process45756, isolated2560x1440/full_default/SMAA, native async1,
persistent sync1, recording on, timeline0, image audit1,60-second query window,
zero global input. No flicker bursts; five existing functional TGA checks passed.
Viewed only the new single-preview: scene/UI/orientation/color/shadows present,
no obvious black patch; this single frame is not a fissure absence proof.

Two separate facts were observed, NOT joined into a cause:

- At recording submission14,25 actual image relocation descriptors were logged.
  Cookie3, old handle146454392 ->164562296, mip1/layer1, allInitialized0,
  regions0. Its originating image/pixel consumer is not identified. The raw
  wrapper forwards this descriptor unchanged. This is a real command-recording
  API violation, not validation-layer output or proof of a GPU crash.
- Only later, submissions206/212/308, eight selected external-image records
  showed declared read layouts4/5 versus software UNDEFINED0, initialized0 and
  canRelocate1. Six records are completed shadow-color/depth copy recording,
  two are fresh CSM/mask layout initialization. No cookie/handle/order match
  joins these copies to the earlier zero-region relocation. Handle reuse across
  different cookies was actually seen, so handle-only matching would be unsafe.

Last counter checkpoint: submission16128, externalCalls6374, externalRecords8,
relocations25, zeroRegion1, suppression/tableOverflow/logErrors0. All64 checkpoint
slots were consumed: the remaining run has no closed final counter interval,
even though relocation/external logs have independent budgets. The analyzer's
v2 result explicitly flags checkpointBudgetReached=true and preserves the v1
result unchanged. Neither analyzer claims whole-run coverage or bad-frame proof.
The observed zero-region command is an anomaly: no retry, replacement or second
run was started after discovering it.

The60.0492022-second window counted12105 Presents/ordinary calls, all12105 elided,
retained0; elapsed/N4.960694ms is isolated diagnostic throughput, NOT a new A/B,
foreground FPS, exact frame latency or gain attributable to image changes.
Four unique-root, duplicate-free reports have3960/4000/4000/4000 frames and pin
the diagnostic DLL. Their framesIncomplete and framesBudgetExceeded are0;
missing producerRequiredCaster fields remain unavailable, not invented zeros.
Screenshot readback and report success do not validate the anomalous migration.

Exact native-handle settlement, desktop closure and original video restoration
passed. EE90/34,495,769 restored byte-exact; GPU event/dump/residual-process0.
The previous00:26 incident remains open. All evidence is under
`AutoTest/artifacts/native_async_screenshot_20260914/image_relocation_observe_r1_20260914/`:

| Evidence | Bytes | SHA256 |
| --- | ---: | --- |
| receipt.json | 7707 | 1224F95F08600D28A9ED8F163351C6D4DAE461022FB17AA46C9DE915482582F5 |
| after-root-war3_d3d9.log | 39846 | 221B395F8A32E7565E7C27F98B058C54B2EAFD1B7AD4F40423967277BE89D790 |
| war3_perf_report_auto_2026_09_14_06_15_28.html | 3441301 | F6C0F47843D51DD15D54A94A08993969B4D84026ABD8EAC2096BEF8D41ACCA4B |
| war3_perf_report_auto_2026_09_14_06_15_48.html | 3406911 | 3D6829DE14747295469D1B6E9215979A8913708A31A57D699DC7A1D831A8085B |
| war3_perf_report_auto_2026_09_14_06_16_08.html | 3497730 | 9AA3F6A9FF5DB488B443F82B1C1203BBAEC2E26153BC1CA420FCC0B76FACD881 |
| war3_perf_report_auto_2026_09_14_06_16_28.html | 3530972 | 1A09877B3F450287587998A85110E92C0EDD0F0776DFD7E9F4572641DF9F4C21 |

### Repair prerequisites (not implemented in D55C)

Do not simply add trackLayout to every external image. Shadow color/depth copies
already have transfer-src/dst usage and transfer access stages, but retained CSM,
caster mask and volume-sun allocations lack transfer usage. DxvkImage allocation
does not add those usage bits automatically. Marking them initialized without
establishing a legal relocation contract can turn missing copies into invalid
copies. Choose a bounded owner/lifetime pin or supported transfer-capable image
creation plus accurate final-layout tracking, with exact retained-content tests.

Likewise, merely skipping vkCmdCopyImage2 when regionCount0 is insufficient:
invalidateImageWithUsage currently marks the new image's full range in the
preferred layout even where the relocation never transitioned/copied that range.
A repair must preserve truly uninitialized subresources, avoid zero-region
commands, and not erase externally written pixels misclassified as uninitialized.
The copy-region loop also mutates subresourceRange.layerCount while using it as
its loop bound on partially initialized arrays; assess this separately before
claiming partial point-shadow/TAA migration correctness. No changes to these
core migration policies or raw render barriers were made by the observer.

## 07:00 conservative automatic-relocation admission candidate

Preflight406 dirty entries matched the previous checkpoint; buildD55C and live
EE90 unchanged, related processes0. The observed cookie3 zero-region descriptor
is sufficient to repair the optional automatic route without guessing its
pixel consumer. It does not identify a black frame.

`DxvkImage::relocateStorage` now keeps the existing base canRelocate gate, then
requires all available subresources initialized and both TRANSFER_SRC/DST usage
before allocating replacement storage. Rejection returns nullptr: the existing
queued-resource loop skips it. No allocation, storage replacement, GPU copy,
layout fabrication, permanent pin or GPU wait occurs on the rejected path.
The safety gate is independent of diagnostic logging. Valid complete images
retain the original allocator call and mode; buffers and direct usage upgrades
remain unchanged. This is not a global defrag disable.

This deliberately defers optional relocation of partially initialized images
instead of editing the complex generic copy/partial-layout algorithm now.
An external image whose software layout is still unknown keeps its old backing
and pixels, rather than being silently migrated without copying them. CSM/mask/
volume-sun images missing transfer usages also cannot enter automatic copies.
This does NOT repair their missing software state or their direct compatibility-
upgrade path, and is NOT an exhaustive proof of every format/aspect copy rule.

Tradeoff: incomplete or unsupported resources may prevent a memory chunk being
compacted/evicted until eligibility changes or the resource is destroyed. No new
memory budget is allocated by rejection, but high-pressure/long-run reclamation
still needs verification; this candidate is not stable merely because it avoids
the observed invalid command. Full initialization can allow later normal
relocation; no sticky fault or permanent resource field is added.

Bounded reject diagnostics add initialized/transfer reasons and64 reject records,
with explicit counters/suppression. The parser accepts the original complete
checkpoint schema OR the complete five-counter extension, rejects partial
extensions/schema changes/counter regression, and keeps legacy missing counters
unavailable. Eight new static/synthetic checks plus nine original audit tests
pass. No old evidence/result is rewritten. The primary Vulkan requirements
remain [positive copy-region count and legal transfer image usage](https://docs.vulkan.org/refpages/latest/refpages/source/VkCopyImageInfo2.html).

Validation contract: exact DLL BelowNormal/-j2 build and no-work, targeted tests;
then one fresh isolated2560x1440 admission check with audit enabled. Require an
actual uninitialized rejection, nonzero normal relocations and zero zero-region
descriptors through the last closed checkpoint, in addition to existing runner/
readback/integrity/GPU/restore gates. If selection does not occur, report it as
untested rather than manufacturing memory pressure or claiming a pass. No
repeat/replacement on an anomaly. Whole-run/fissure/foreground acceptance remain
false, and EE90 must be restored before finishing.

### 07:12 admission run result

FE183B48098EEFC4F49EC6A1B5F1251EE031A018592B0ECC617493D55C7E1B27,
34,572,930bytes PE32/i386, built5/5 (image/context/shadow-resources objects,
libdxvk.a and exact DLL). Parent23632 BelowNormal/-j2, exit0/no-work;
106 targeted tests,4-file py_compile/diff-check pass. A first old-log replay
attempt used the Windows default GBK encoding and failed decoding; explicit
UTF-8 replay passed with every prior analysis field unchanged. Old evidence
was not modified or relabelled as passing.

One fresh process49768, `image_relocation_admission_r1_20260914`, same isolated
2560x1440/full_default/SMAA diagnostic contract, native async1/persistent1/
recording on/timeline0/audit1. Through checkpoint16128:

-24 normal image relocation descriptors, zeroRegion0.
-1 actual rejection at recording submission14: cookie3, handle152090488,
 initialized0, transferCapable0, usage6 (SAMPLED|TRANSFER_DST, no TRANSFER_SRC),
 one mip/layer. Reject reasons overlap: uninitializedRejects1 and usageRejects1
 count the SAME rejected attempt, not two resources. No replacement was allocated.
- externalCalls6372, externalRecords8, suppression/tableOverflow/logErrors0.
 All64 checkpoint slots were used; no whole-run counter coverage is claimed.
 The external layout mismatches remain observable by design: admission guards
 them from automatic relocation, not a fabricated software-state correction.

This proves the rejection path executed while ordinary migration continued;
it did not merely globally disable the allocator. Cookies are process-local:
the repeated ordinal3 across the two runs does NOT identify its creator/pixel
consumer. The previous captured zero-region descriptor remains real failed
evidence, not erased by this successful admission check. Next useful trace is
the selected image's creation identity/extent/format and owner, not another
sparse black-frame hunt or a speculative global resource-policy rewrite.

Five TGA outputs decoded; viewed only this run's new single-preview, with correct
scene/UI/orientation/color/shadows and no obvious patch. This is not a temporal
fissure test. Four strict reports3416/4000/4000/4000frames pin FE183B, duplicate0,
reported incomplete0/budgetExceeded0. No complete physical-memory accounting or
high-pressure proof exists in these reports. Reclamation/residency restoration
also shares the queued route (DxvkResourceRef -> requestResidency ->
requestMakeResident), so high-pressure performance remains an explicit risk gate.

60.0553432seconds /10659 Presents gives5.634238ms elapsed/N; native ordinary sync
calls10659/10659 elided, retained0. This diagnostic value is slower than the
previous single run and is NOT an A/B result or proof of either a regression or
a gain from the guard. No new performance-improvement percentage is claimed.

Native-handle settlement, desktop/video restoration, exact originalEE90 restore,
GPU event/dump/residual-process0 all pass. No second admission run/retry, player
deployment, GitHub reply or stable promotion. Evidence directory:
`AutoTest/artifacts/native_async_screenshot_20260914/image_relocation_admission_r1_20260914/`.

| Evidence | Bytes | SHA256 |
| --- | ---: | --- |
| receipt.json | 7740 | 92523F698C4EADB9E3678442C25C5380598655FAED38C6F85AB38F1C2C0CCAD2 |
| after-root-war3_d3d9.log | 46235 | 7A60B1BB453D76A74C5C9313DCAB43EE5B8EBFA95B4FC6BE15935781D669162E |
| war3_perf_report_auto_2026_09_14_07_11_22.html | 3060513 | A37B0FB551254EE943376E240AED75E78E4128777EC83658F8D974F5681AEF30 |
| war3_perf_report_auto_2026_09_14_07_11_42.html | 3476837 | DA19147CE0A6339F4EC2B13F14199AD5FEE85E98933339E5C9601F44B02D10F3 |
| war3_perf_report_auto_2026_09_14_07_12_02.html | 3485866 | 67BB5CB624D13B95549042E4FBF4E4CDAD91C2A9FA85029EE039E96AADA08029 |
| war3_perf_report_auto_2026_09_14_07_12_22.html | 3526054 | FDEF41E839DBADB87A5D851FD66A3EE1B758D4EF79DACD861C14ECA37668C902 |

## 08:50 bounded creation identity audit (not a rendering change)

Preflight408 paths match the07:00 checkpoint byte-for-byte;31 frozen runtime
files unchanged. BuildFE183B/liveEE90, GameE04D/map11376D and zero related
processes rechecked. No user process stopped or foreground input used.

Source narrows usage6 candidates: normal D3D9CommonTexture primary images
always include both transfer usages, and the HUD font uses usage7. Private
AA copies/lookups, shader-pack textures/fallback and swapchain gamma remain
possible. Cookie3 alone in different runs cannot choose between them.

Add a default-off owned-image creation probe after successful assignStorage,
using the existing exact IMAGE_RELOCATION_AUDIT=1 gate. Record first64 creations
per creating thread, then one explicit limit marker at65; no subsequent logs.
Observe process-global DxvkPagedResource cookie, initial handle, dimensions,
format/type/usages/layouts and original caller debugName (64-byte bounded hex;
cap explicit). Do not persist name pointers or alter debug flags/resource state.
Imported-image constructor and relocations do not masquerade as new origins.
This is CPU construction evidence, not upload/initialization/GPU completion.

Independent origin parser first runs the unchanged strict relocation parser,
then rejects malformed/duplicate/missing origin fields, cookie duplication,
ordinal gaps/regression and logger failures. Only same-log prior construction
with exact cookie+handle+usage+mip/layer may match a rejection; creator and CS
threads need not match because cookies are process-global. Unnamed/capped
labels remain incomplete; unmatched rejection stays unknown. No cross-run
cookie/handle matching, automatic owner guess or flicker acceptance.

Validation plan: related source/synthetic tests, exact DLL BelowNormal/-j2,
no-work/PE identity, then at most one new isolated2560x1440 audit transaction.
Same existing runner, AA5, persistent1/recording on/timeline0/audit1,60seconds,
zero optional flicker bursts. Runtime goal is identify the actually rejected
image in that process, not a second admission retry or sparse-fissure hunt.
Any runtime incident terminates/restores; keep EE90 live on completion. Report
all caps and unclosed tails. No stable/performance/black-fissure verdict.

### 09:03 origin identity result: ShaderPack fallback, not a screen texture

One run `image_origin_r1_20260914`, PID38884, exact isolated2560x1440 with
zero global input, B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993
/34,577,677bytes PE32/i386. Exact build3/3 (image object/libdxvk.a/DLL),
parent50820 BelowNormal/-j2 and no-work.114/114 related checks and6-file
py_compile/diff-check passed before running; no concurrent compile.

Same immutable process log proves the actual image origin:

- Creation line274, creator threadKey419457464, ordinal1, cookie3,
  handle154777464,2D RGBA8_UNORM,1x1x1,mip1/layer1,sample1,usage6,
  initialUNDEFINED/preferredSHADER_READ_ONLY, uncapped exact caller label
  `War3ShaderPackFallback`.
- Rejections at CS threadKey421999032, submissions14 and168, same cookie,
  same handle/usage/mips/layers. These are two attempts on ONE image,
  not two images. Both fail initialized and transfer-src predicates.
-84 observed creations across two threads; main creator reached64 records
  then the explicit65limit. This is not full resource creation coverage.
- Through last closed recording checkpoint16128:36 normal image relocation
  descriptors, zeroRegion0, automaticRejects2, externalCalls6376/records8,
  other suppression/tableOverflow/logErrors0. Checkpoint64-budget reached;
  the run tail remains unclosed. Shadow software-layout mismatch records
  remain unchanged in meaning and are not the fallback image.

Source role is now narrow: `EnsureFallbackTexture` creates this1x1 texture
and its sampled view during `InitShaderPackRuntime`, even before a pack is
loaded. It records no initialization/upload/clear there. The stored fallback
references only feed the ShaderPack descriptor defaults in the pass loop;
`RunShaderPackPasses` gates that loop on pack.loaded/enabled and logs the first
five executing passes. This run logged runtime initialization but no executing
pack pass. That absence plus the source gates is supporting evidence, NOT a
GPU sampling census or proof that arbitrary user packs never sample it.
Uninitialized fallback content is a separate source defect for actual pack
consumption; do not pretend the relocation guard initializes the texel.

This disambiguates the current rejected resource. It does NOT retroactively
identify an old process's ordinal3, and it does NOT explain a one-frame black
fissure. The old zero-region API descriptor remains invalid even if it acted
on an unused dummy. Do not promote an allocator validation defect into a
visual root cause solely because both exist.

Functional checks:5native async TGA files saved/decoded; this run's new
single-preview only was viewed, with scene/UI/colors/orientation/shadows present
and no obvious patch. No temporal/no-fissure claim. Four strict JSON reports
3829/4000/4000/4000frames, duplicate0 and exactDLL identity; reported
framesIncomplete/framesBudgetExceeded0. Producer-specific omission fields
absent from shadowBudgetSummary remain unavailable, not synthesized zeros.
60.0703863seconds/11582Presents=5.186530ms elapsed/N;11582ordinary sync calls
all elided. Diagnostic instrumentation and uncontrolled background mean no
performance gain/regression percentage or playerFPS claim.

Native retained-handle termination/desktop close/video restore/exactEE90 restore
pass;GPU events/new dumps/residual processes0. No second run/retry or GitHub
reply. Sources and frozen evidence remain offline; rootCHANGELOG unchanged.

| Evidence | Bytes | SHA256 |
| --- | ---: | --- |
| receipt.json | 7637 | 8A068DC1A443594D33C5560A4B86F53D5B9F33D2E9A332834F1DE97A20A4C3ED |
| after-root-war3_d3d9.log | 68404 | 6866B1213D0D118FECF70CA536EE37C4C543F40D065B95FE8A017158E81F3579 |
| image-origin-analysis.json | 43830 | 27249B19FC5DF4061388A16CBD257748E2C568874CA33C0A770525AC94A15C3F |
| war3_perf_report_auto_2026_09_14_09_01_53.html | 3349827 | 702277E801363FFA7F02011D8367FA81C6868AD77CD1FDFD21B46CA6DE3C8559 |
| war3_perf_report_auto_2026_09_14_09_02_13.html | 3476321 | 4D8709A710BEC716489FE640C1305B20AF93E61BECFD8E6F231E4AD508F6FFE4 |
| war3_perf_report_auto_2026_09_14_09_02_33.html | 3496621 | 4518E96365D5816C7CC4B32C8AC2F2A878B7D6EFD71DF74EEE636ABE94F44F5A |
| war3_perf_report_auto_2026_09_14_09_02_53.html | 3530395 | 3061894A32B27E8E90EF01DD08FF7604244A99FBEA55FFB3B4DD326336197B3A |

Evidence directory: `AutoTest/artifacts/native_async_screenshot_20260914/image_origin_r1_20260914/`.
Next prioritization: do not spend another sparse run proving the same dummy
identity. Separate optional ShaderPack initialization repair from #8; focus
the latter on retained-content images actually consumed by the active AA/
shadow/presentation paths and their software-state/raw-barrier contracts.
An explicit-upgrade/partial-layer core fix and memory-pressure acceptance are
still outstanding; no global pin, fake layout or GPU-idle workaround is allowed.
