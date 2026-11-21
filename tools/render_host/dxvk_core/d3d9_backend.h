#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d9.h>
#include "../../../src/d3d9/war3/effects/war3_shadow_scene.h"
#include "shadow_outcome.h"
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include "../d3d9_memory_probe.h"
#include "../d3d9_clear_wire.h"
#include "../d3d9_ff_state.h"
#include "../d3d9_texture_wire.h"
#include "../d3d9_sampler_wire.h"
#include "../d3d9_effects_wire.h"
#include "../d3d9_texture_stages_wire.h"
#include "../d3d9_resource_registry.h"
#include "host_surface_target_policy.h"

namespace warvk::host::dxvkcore {
class D3D9Factory;

// A deliberately finite development profile backed by the REAL DXSO compiler
// and DXVK allocator/context/submission/retirement. Not a shipping D3D9 device.
// One dedicated x64 owner thread; no Game.dll, hooks or x86 C++ pointers.
class D3D9Backend final {
public:
  // Only pure input/profile preflight may throw this recoverable rejection.
  // All messages are static literals; rejecting a profile input allocates no
  // exception-owned string and never consumes/quarantines the GPU owner.
  class ProfileRejected final : public std::exception {
  public:
    explicit ProfileRejected(const char* reason) noexcept : m_reason(reason) { }
    const char* what() const noexcept override { return m_reason; }
  private:
    const char* m_reason;
  };

  static constexpr uint32_t MaxDimension = 256;
  static constexpr uint32_t MaxBufferBytes = d9::MaxBuffer;
  // Texture slots are game-sized (phase 5.1); the other typed tables keep the
  // laboratory bounds, which already cover the game (two vertex buffers, one
  // index buffer, no shaders). 64 resource ids always remain for them.
  static constexpr uint32_t BufferSlots = d9::BufferSlots, TextureSlots = d9::TextureSlots,
    ShaderSlots = d9::ShaderSlots, DeclarationSlots = d9::DeclarationSlots;
  static constexpr uint32_t FloatRegisters = 32, MaxDeclarationElements = 16;
  static constexpr uint32_t MaxDrawBatch = 32;
  // Bounded by one wire packet of complete fixed-function draws (3,180 B each).
  static constexpr uint32_t MaxFixedDrawBatch = 20;
  static constexpr uint32_t MaxDrawFlights = 4;
  // Opt-in stream profile: one open command list per sync interval, bounded
  // per list and by unretired stream submissions (native frame-latency shape).
  static constexpr uint32_t MaxStreamDraws = 4096;
  static constexpr uint32_t MaxStreamLatency = 3;
  // Same per-storage size as native DefaultConstantBufferSize.
  static constexpr uint32_t StreamRingBytes = 1u << 20;
  enum class ShaderStage : uint32_t { Vertex = 0, Pixel = 1 };
  enum class ResourceType : uint32_t { VertexBuffer, IndexBuffer, Texture, VertexShader, PixelShader, Declaration };

  struct RenderStates {
    uint32_t zEnable = D3DZB_TRUE, zWriteEnable = TRUE, zFunc = D3DCMP_LESSEQUAL;
    uint32_t cullMode = D3DCULL_NONE;
    uint32_t alphaBlendEnable = FALSE, srcBlend = D3DBLEND_ONE, destBlend = D3DBLEND_ZERO;
    uint32_t blendOp = D3DBLENDOP_ADD, colorWriteMask = 15;
  };
  struct DrawState {
    uint32_t vertexBuffer = 0, indexBuffer = 0, texture = 0;
    // Host-resolved typed slots. The legacy draw bytes never serialize these;
    // each draw revalidates the connection's generation-bearing stage table.
    std::array<uint32_t,d9::AdditionalTextureStages> additionalTextures{};
    uint32_t vertexShader = 0, pixelShader = 0, declaration = 0;
    uint32_t streamOffset = 0, stride = 0;
    D3DVIEWPORT9 viewport{};
    RenderStates render;
    std::array<float, FloatRegisters * 4> vertexFloatConstants{};
    std::array<float, FloatRegisters * 4> pixelFloatConstants{};
    int32_t baseVertexIndex = 0;
    uint32_t minVertexIndex = 0, numVertices = 0, startIndex = 0, primitiveCount = 0;
    // This profile only accepts a triangle list, stream 0, 16-bit indices,
    // pixel texture/sampler 0..7, no instancing or stencil. Alpha/fog/scissor facts are
    // ordered connection state; each recorded draw owns its derived GPU state.
    D3DPRIMITIVETYPE primitiveType = D3DPT_TRIANGLELIST;
  };
  struct ReuseWaitCounters {
    uint64_t episodes = 0, gpuCalls = 0;
    uint64_t totalNs = 0, cpuSubmissionNs = 0, gpuWaitNs = 0, retirementNs = 0;
  };
  struct ReuseWaitSample {
    uint64_t query = 0, draws = 0;
    ReuseWaitCounters cumulative;
  };
  // P0 measurement only (warvk-d3d9-host-profile/v1): owner-thread CPU wall
  // time spent blocked, by the call site that asked for it (WaitSite) and the
  // primitive that blocked (WaitKind), and submissions by call site.
  // QueryPerformanceCounter ticks in fixed tables; nothing here changes when
  // or whether anything waits or is submitted.
  enum class WaitSite : uint8_t {
    Other, Initialize, Reset, BufferDynamic, BufferStaticBefore, BufferStaticAfter, CreateTexture,
    TextureUploadBefore, TextureUploadAfter, TextureLod, ClearFullBefore, ClearFullAfter, ClearRectsBefore,
    ClearRectsAfter, DrawFixed, DrawBatch, DrawStream, DrawFlight, FlushDraws, Effects, Readback, IssueEvent,
    WaitEvent, PresentBefore, Present, PresentAfter, Destroy, Finish, ShadowCaster, TextureUploadSlice, Count };
  // SettleIdle/SettleGpu: settle() with nothing / something submitted and not
  // yet complete (its submission check and timeline wait); Retire: DXVK
  // resource retirement after a settle; SubmitCpu: the submission thread's
  // result for one command list; the rest are the named waits. DiscardCarry,
  // UploadCopy and CasterCopy are not waits but owner-thread CPU copies
  // (Discard rename plus carried prefix, the upload memcpy into host-visible
  // memory, one shadow caster's geometry), kept in the same table.
  enum class WaitKind : uint8_t {
    SettleIdle, SettleGpu, Retire, SubmitCpu, PresentRetire, Acquire, PresentQueue, StreamLatency, FlightReuse,
    Event, DiscardCarry, UploadCopy, CasterCopy, Count };
  static const char* WaitSiteName(size_t site) noexcept {
    static constexpr const char* names[size_t(WaitSite::Count)] = { "Other", "Initialize", "Reset", "BufferDynamic",
      "BufferStaticBefore", "BufferStaticAfter", "CreateTexture", "TextureUploadBefore", "TextureUploadAfter",
      "TextureLod", "ClearFullBefore", "ClearFullAfter", "ClearRectsBefore", "ClearRectsAfter", "DrawFixed",
      "DrawBatch", "DrawStream", "DrawFlight", "FlushDraws", "Effects", "Readback", "IssueEvent", "WaitEvent",
      "PresentBefore", "Present", "PresentAfter", "Destroy", "Finish", "ShadowCaster", "TextureUploadSlice" };
    return site < size_t(WaitSite::Count) ? names[site] : "unknown";
  }
  static const char* WaitKindName(size_t kind) noexcept {
    static constexpr const char* names[size_t(WaitKind::Count)] = { "SettleIdle", "SettleGpu", "Retire",
      "SubmitCpu", "PresentRetire", "Acquire", "PresentQueue", "StreamLatency", "FlightReuse", "Event",
      "DiscardCarry", "UploadCopy", "CasterCopy" };
    return kind < size_t(WaitKind::Count) ? names[kind] : "unknown";
  }
  // polls: Sleep(1) calls inside the wait (PresentRetire, PresentQueue).
  // slow: waits of at least 1 ms, 5 ms and 20 ms.
  struct WaitCell { uint64_t count = 0, ticks = 0, maxTicks = 0, polls = 0; std::array<uint64_t, 3> slow{}; };
  struct WaitProfile {
    WaitSite site = WaitSite::Other;
    std::array<std::array<WaitCell, size_t(WaitKind::Count)>, size_t(WaitSite::Count)> waits{};
    std::array<uint64_t, size_t(WaitSite::Count)> submits{}, gpuSubmits{};
  };
  // E0 (WARVK_D3D9_PRESENT_WAIT; block by default since the 2026-11-18
  // review): how Present's two waits (queue result, frame retirement) sleep.
  // block: sleep on the completion's own notification in bounded slices, with
  // a 1 ms timer-resolution request. poll: Sleep(1), the earlier wait.
  // alternate:N: poll and block in turns of N presents in one run (same-run
  // A/B). Same deadlines, checks and errors in every mode.
  struct PresentWaitProfile {
    uint32_t setting = 0, every = 0;
    // Per mode (0 poll, 1 block): presents, queueTicks, queueSleeps,
    // retireTicks, retireSleeps, intervalTicks, intervals (host Present to Present).
    std::array<std::array<uint64_t, 7>, 2> modes{};
    // Per mode: Present-to-Present intervals in 1 ms buckets; the last is >= 100 ms.
    std::array<std::array<uint32_t, 101>, 2> intervalMs{};
  };
  // Dynamic buffers' host-visible storage and the two owner-thread paths that
  // READ it on the CPU: the DISCARD carry and shadow-caster geometry. A
  // dynamic VB keeps a cached CPU copy updated with every upload (IBs already
  // have indexBytes), and those reads take it instead of the mapping, which
  // is uncached write-combined memory on some adapters. Same bytes, same
  // moments. WARVK_D3D9_DYNAMIC_CPU_COPY: on by default since the 2026-11-18
  // review; "0" reads the mapping, "alternate:N" in turns of N presents.
  struct DynamicReadProfile {
    uint32_t setting = 0, every = 0, flagsSeen = 0;
    uint64_t created = 0, createdDeviceLocal = 0, createdCached = 0;
    uint64_t liveBytes = 0, peakBytes = 0, copyBytes = 0, peakCopyBytes = 0;
    // Per source (0 mapping, 1 CPU copy): carries, carryBytes, carryTicks,
    // casters, casterTicks, castersFromDynamicVertex.
    std::array<std::array<uint64_t, 6>, 2> modes{};
  };
  // P1 batch 1 (WARVK_D3D9_UPLOAD_SLICES; on by default since the 2026-11-18
  // review, stream profile only): each texture upload goes into a DXVK
  // staging slice and its copy is recorded in the open stream list, in order
  // with the draws around it; the list owns the slice's buffer until GPU
  // retirement. In-flight staging is bounded in bytes and slices, recycled by
  // the completion timeline. "0": settle + copy + submit(true) per upload, the
  // earlier path (and the only one without a stream list); "alternate:N" in
  // turns of N presents (same-run A/B). Begin/Chunk/Commit and the managed
  // CPU copy are the same; only the host's GPU upload lifetime differs.
  struct UploadSliceProfile {
    uint32_t setting = 0, every = 0;
    uint64_t recorded = 0, recordedBytes = 0, listFlushes = 0, creditWaits = 0, creditWaitTicks = 0;
    uint64_t peakInFlightBytes = 0, peakInFlightSlices = 0;
    // Per mode (0 settle/submit, 1 slices): uploads, ticks inside uploadTextureRegion.
    std::array<std::array<uint64_t, 2>, 2> modes{};
  };
  enum class WaterFallback { None, Msaa, Format, Snapshot, DrawState, Projection, Resources, Pipeline, Count };
  struct WaterStats {
    uint64_t draws = 0, snapshots = 0, skippedOldPasses = 0;
    std::array<uint64_t, size_t(WaterFallback::Count)> fallbacks{};
    uint64_t gpuSamples = 0;
    double gpuTotalUs = 0, gpuMaxUs = 0;
  };
  struct Stats {
    // Development-only, opt-in CPU wall time. Samples delimit EVENT Issue
    // intervals, not presents. Nested stage times are contained in totalNs;
    // vkWaitSemaphores call duration is not pure GPU execution time.
    bool reuseWaitProfile = false;
    ReuseWaitCounters reuseWait;
    std::array<ReuseWaitSample, 128> reuseWaitSamples{};
    uint32_t reuseWaitSampleCount = 0, reuseWaitSamplesDropped = 0;
    uint64_t submitted = 0, gpuCompleted = 0, retired = 0;
    // Frozen legacy wire: shadersCompiled counts successful shader CREATE
    // requests, including reuse. Actual CPU compiles live in ShaderCacheStats.
    uint64_t buffersCreated = 0, texturesCreated = 0, shadersCompiled = 0, declarationsCreated = 0;
    uint64_t uploads = 0, uploadedBytes = 0, draws = 0, clears = 0, readbacks = 0, queries = 0;
    uint64_t drawBatches = 0, batchedDraws = 0, batchConstantBytes = 0;
    uint32_t largestDrawBatch = 0;
    uint64_t flightSubmissions = 0, flightReuses = 0, flightRetirements = 0;
    uint32_t flightCapacity = 1, maxOccupiedFlightSlots = 0;
    uint32_t submissionGroupCapacity = 1;
    uint64_t groupedSubmissions = 0, nonIndexedDraws = 0;
    uint32_t batchStateReuse = 0;
    uint64_t redundantStateBindingsSkipped = 0;
    // Stream profile only. Renames hand the old storage to the recording
    // command list; latency waits are the only CPU waits stream draws add.
    bool streamMode = false;
    uint64_t streamSubmissions = 0, streamCapSubmissions = 0, streamDraws = 0, streamFixedDraws = 0;
    // WARVK_D3D9_STREAM_FLUSH_DRAWS (0: off): the open stream list is submitted
    // once it holds this many draws; how often that happened.
    uint32_t streamFlushDraws = 0;
    uint64_t streamCadenceSubmissions = 0;
    uint64_t streamConstantRenames = 0, streamLatencyWaits = 0;
    uint32_t largestStreamList = 0;
    // Opt-in admission profile, CPU wall time inside drawBatch/drawFixedBatch
    // only. The constant fill (SM3 and fixed-function UBOs) is nested inside
    // recording; recording includes any flight, group or stream submission work
    // that the batch itself triggers.
    // Inside recording: state binding (sampler/view loop nested) and the DXVK
    // draw call that commits descriptors/pipeline. Bind excludes the constants.
    bool admissionProfile = false;
    uint64_t admissionPrepareNs = 0, admissionRecordNs = 0, admissionConstantNs = 0;
    uint64_t admissionBindNs = 0, admissionSamplerNs = 0, admissionDrawNs = 0;
    uint64_t liveLogicalBytes = 0, peakLogicalBytes = 0;
    // BeforeUi effect executors (before_ui_effects.h): runs, CPU wall time
    // inside runBeforeUi, and the bytes of the images the executors own.
    uint64_t beforeUiRuns = 0, beforeUiNs = 0, effectResourceBytes = 0;
    // Shadow pass (shadow_before_ui.h): runs, and caster draws handed to it.
    uint64_t shadowRuns = 0, shadowCasterDraws = 0;
    WaterStats water;
    // Separate milestones: a CPU Present request/queue result is not GPU
    // completion, DXVK resource retirement, or proof of displayed pixels.
    uint64_t presentAttempts = 0, acquiredImages = 0, presentQueued = 0;
    uint64_t presentApiSucceeded = 0, presentGpuCompleted = 0, presentRetired = 0;
    // WARVK_D3D9_PRESENT_PIPELINE (on unless "0"; stream mode only): Presents
    // that returned without waiting for their own frame on the GPU, and the
    // times a window fault made the next Present take that wait after all.
    uint64_t presentsPipelined = 0, presentPipelineDrains = 0, clearsPipelined = 0;
    uint32_t presentPipelineSetting = 0;
    // Diagnostic reads of the last successful actual swapchain-image copy,
    // not source-target reads or proof of HWND pixels/scanout.
    uint64_t presentedReadbacks = 0, presentedFrame = 0;
    uint32_t liveResources = 0, width = 0, height = 0, vendorId = 0, deviceId = 0;
    uint32_t windowProcessId = 0, windowThreadId = 0;
    // VK_NOT_READY initially; kept as integers so this API need not expose
    // Vulkan headers to its D3D9/IPC caller.
    int32_t lastAcquireVkResult = 1, lastPresentVkResult = 1;
    bool initialized = false, normalTeardown = false, quarantined = false;
    // A pre-acquire window rejection is still permanent DEVICELOST. Only a
    // proven quiescent owner with no author resources may be cleaned normally;
    // this does not turn the failed frame into successful/normal rendering.
    bool windowPreflightFailed = false, faultCleanupCompleted = false;
    bool shutdownQueueDrained = false;
    uint64_t faultCleanupMs = 0;
    bool presenterImplemented = false;
    // D3D9 Reset of the implicit swap chain (window resets: those of the
    // window's own resize, which kept every resource), and Presents skipped
    // because the window client no longer matched the back buffer.
    uint64_t resets = 0, windowResets = 0, presentsSkipped = 0;
    // Dynamic buffers: uploads by Lock mode, storage renames on DISCARD and
    // the bytes carried into the new storage, and uploads that settled first.
    uint64_t dynamicBuffers = 0, orderedDynamicUploads = 0, noOverwriteUploads = 0, discardUploads = 0;
    uint64_t discardRenames = 0, discardCarriedBytes = 0, uploadSettles = 0;
    bool opaqueTarget = false;
    uint32_t syncInterval = 0;
    std::string adapter;
    // P0 measurement only (host-profile/v1), see WaitSite.
    WaitProfile waitProfile;
    PresentWaitProfile presentWait;
    DynamicReadProfile dynamicRead;
    UploadSliceProfile uploadSlices;
    // Present-to-Present intervals by the phase of whichever alternate:N
    // switch is set (E0, item 4 or P1), 1 ms buckets, the last >= 100 ms.
    uint32_t abPhaseSwitch = 0;
    std::array<std::array<uint32_t, 101>, 2> abIntervalMs{};
    std::array<uint64_t, 2> abFrames{}, abIntervalTicks{};
  };
  struct ShaderCacheStats {
    uint64_t requests = 0, hits = 0, compileAttempts = 0, compilesSucceeded = 0;
    uint64_t registrations = 0, raceLosers = 0, validationFailures = 0;
    uint64_t registrationFailures = 0, readyEntries = 0, failedEntries = 0;
    uint64_t keyBytes = 0, keyCapacityBytes = 0;
    uint64_t constantBytes = 0, constantCapacityBytes = 0;
    std::array<uint64_t, ShaderSlots> vertexCookies{}, pixelCookies{};
  };

  D3D9Backend();
  ~D3D9Backend() noexcept;
  D3D9Backend(const D3D9Backend&) = delete;
  D3D9Backend& operator=(const D3D9Backend&) = delete;

  // Null window keeps the original offscreen profile. WSI is restricted to
  // 64x64 and a live HWND belonging to the authenticated peer PID/TID, with a
  // session-unique property whose value is HANDLE(1). The caller supplies the
  // authenticated identity, not a PID asserted by an untrusted command packet.
  // Dev-owned modes retain these fields as the peer/parent identity; the actual
  // surface HWND belongs to a separate host UI owner and is never substituted
  // into the authenticated-peer fields. Peer remains the default mode.
  void initialize(uint32_t width, uint32_t height, HWND window = nullptr,
    uint32_t windowProcessId = 0, uint32_t windowThreadId = 0,
    const wchar_t* windowMarker = nullptr, bool diagnosticPresentedReadback = false,
    HANDLE authenticatedPeerProcess = nullptr, surface::Mode surfaceMode = surface::Mode::Peer,
    const D3D9Factory* factory = nullptr, bool opaqueTarget = false, uint32_t interval = 0);
  // D3D9 Reset: every author buffer is already destroyed, unless keepResources
  // (the window's own resize keeps them). Waits for all GPU, resource and frame
  // retirement, then recreates the color/depth targets (and the swap chain
  // extent and interval) at the new extent and format. Textures, shaders,
  // declarations and queries are kept.
  void reset(uint32_t width, uint32_t height, uint32_t interval, bool opaqueTarget, bool keepResources = false);
  // IDs are 1-based within typed tables. VB/IB share one bounded buffer table.
  // A dynamic buffer lives in host-visible memory and is written in place.
  void createBuffer(uint32_t id, uint32_t size, bool index16, bool dynamic = false);
  void uploadBuffer(uint32_t id, uint32_t offset, const uint8_t* bytes, uint32_t count,
    d9::UploadMode mode = d9::UploadMode::Ordered);
  void createTexture(uint32_t id, uint32_t width, uint32_t height);
  void uploadTexture(uint32_t id, const uint8_t* bgra, uint32_t count);
  void createTexture2D(uint32_t id,const d9::TextureLayout& layout);
  void uploadTextureRegion(uint32_t id,const d9::TextureRegion& region,const uint8_t* bgra);
  void setTextureLod(uint32_t id,uint32_t lod);
  void setSamplerState(const d9::SamplerState& state);
  void setAdditionalSamplers(const std::array<d9::SamplerState,d9::AdditionalTextureStages>& states);
  void setEffects(const d9::EffectsState& state);
  void createShader(uint32_t id, ShaderStage stage, const uint32_t* originalTokens, uint32_t wordCount);
  // Count excludes D3DDECL_END. Uses the shared single-stream declaration
  // contract; packed UDEC3/DEC3N remain unsupported.
  void createDeclaration(uint32_t id, const D3DVERTEXELEMENT9* elements, uint32_t count);
  void clear(uint32_t color, float depth, uint32_t flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER);
  void clearRegions(const d9::ClearRegions& regions);
  using CasterIndexSpan = dxvk::war3::effects::scene::CasterIndexSpan;
  void draw(const DrawState& state, CasterIndexSpan* span = nullptr);
  void drawFixed(const DrawState& state,const d9::FixedFunctionState& fixed, CasterIndexSpan* span = nullptr);
  // Same all-or-nothing validation as drawBatch, one fixed state per draw. The
  // stream profile records the whole batch into its open list; other profiles
  // keep the original per-draw settle/submit because their FF UBO is single.
  void drawFixedBatch(const DrawState* states,const d9::FixedFunctionState* fixed,uint32_t count,
                      CasterIndexSpan* spans = nullptr, uint32_t waterMask = 0);
  // Validate the entire bounded batch before touching GPU state. Each draw's
  // constants occupy a distinct aligned slice until the original DXVK GPU and
  // resource-use retirement completes. One batch produces one submission.
  void drawBatch(const DrawState* states, uint32_t count, CasterIndexSpan* spans = nullptr, uint32_t waterMask = 0);
  void beginWaterScene();
  bool waterSceneEnabled() const;
  void flushDraws();
  // Marks a draw already recorded this frame as a shadow caster: the pass
  // replays its vertex and index buffer at BeforeUi. A draw it cannot use is
  // counted in the frame's outcome, never an error of the draw itself.
  void addShadowCaster(const DrawState& state, const dxvk::war3::effects::scene::ShadowCaster& record,
      const dxvk::war3::effects::scene::ShadowCasterAlpha* alpha,
                       uint64_t frameSerial, uint64_t mapEpoch, const CasterIndexSpan& span);
  // The frame ended without BeforeUi: its casters go with it.
  void endShadowFrame();
  // Runs the shadow pass and then the BeforeUi effect executors over the
  // current targets, after every draw recorded so far. Stage values are
  // fx::SsaoStage / fx::SnowStage.
  struct BeforeUiOutcome {
    uint32_t ssaoStage = 0, snowStage = 0, aaStage = 0, bloomStage = 0;
    bool ssaoRecorded = false, snowRecorded = false, aaRecorded = false, bloomRecorded = false;
    ShadowOutcome shadow;
    // What the device's allocator has handed out once this frame's shadows
    // and effects are recorded, over every heap: the live allocations, and
    // the chunks they sit in. All of the host's rendering, not the pass alone.
    uint64_t deviceUsedBytes = 0, deviceAllocatedBytes = 0;
  };
  BeforeUiOutcome runBeforeUi(const dxvk::war3::effects::scene::FramePacket& packet,
                              const dxvk::war3::effects::scene::ShadowPacket& shadow,
                              uint64_t frameSerial, uint64_t mapEpoch);
  bool hasPendingDraws() const;
  // Tightly packed actual GPU BGRA8 rows [first, first+rows) of the target,
  // size exactly width*rows*4. Callers bound a band to one reply.
  std::vector<uint8_t> readback(uint32_t first, uint32_t rows);
  // Opt-in WSI diagnostic only: target-extent BGRA8 copied from the actual
  // acquired swapchain image BEFORE queuePresent, retained after successful
  // Present and the same GPU/resource/frame retirement; returns rows of that
  // frozen copy. Never reads a presented image afterwards, never substitutes
  // readback(), and does not prove scanout. A read starting at row 0 counts
  // one presentedReadback. Disabled mode or no captured frame is ProfileRejected.
  std::vector<uint8_t> readPresented(uint32_t first, uint32_t rows);
  uint64_t issueEvent();
  bool eventComplete(uint64_t serial);
  void waitEvent(uint64_t serial);
  // Real production Presenter/SwapchainBlitter path for the restricted WSI
  // profile; offscreen remains E_NOTIMPL. A window whose identity holds but
  // whose client no longer matches the back buffer returns d9::PresentSkipped
  // before any acquire. D3D_OK requires acquire, queue result,
  // GPU completion and DXVK frame/resource retirement, not scanout evidence.
  // Driver WSI calls/destructors are not cancellable here: an owned-process
  // watchdog must bound a wedged driver. Uncertain GPU state retains the whole
  // owner; a quiescent, resource-empty pre-acquire window failure may clean it.
  HRESULT present() noexcept;
  void destroy(ResourceType type, uint32_t id);
  // All author-created resources must have been destroyed. Unbinds and tears
  // down only after actual GPU AND production resource-use retirement.
  void finish();
  const Stats& stats() const noexcept { return m_stats; }
  // Cold owner-thread account only: no Vulkan query, wait, flush or allocator
  // walk. CPU vectors are measured; opaque compiler/driver/GPU bytes are not.
  d9::memory::HostSample diagnosticMemory() const;
  // Separate cold device-lifetime cache account. Does not include DXVK
  // pipeline maps, SPIR-V storage or driver allocations; never a wire change.
  ShaderCacheStats diagnosticShaderCache() const;

private:
  struct Owner;
  struct PreparedDraw;
  PreparedDraw prepareDraw(const DrawState& state,const d9::FixedFunctionState* fixed, CasterIndexSpan* outputSpan);
  void recordDraw(const DrawState& state, const PreparedDraw& prepared,
    uint32_t constantSlot, bool batched, const DrawState* previous,const d9::FixedFunctionState* fixed=nullptr,
    bool waterDraw = false);
  enum class Admission { Work, WindowFaultCleanup };
  Owner& active(Admission admission = Admission::Work);
  void quarantine() noexcept;
  std::unique_ptr<Owner> m_owner;
  Stats m_stats;
};

} // namespace warvk::host::dxvkcore
