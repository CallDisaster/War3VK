// war3_perf_monitor.h - War3 性能监控器（增强版）
// 支持分层计时、帧历史记录、HTML 报告导出

#pragma once

#include "d3d9_war3_debug.h"
#include "../../d3d9_war3_scene.h"
#include "../gpu_skin/war3_gpu_skin_native_bridge.h"
#include "../render/war3_shadow_runtime_bridge.h"
#include "war3_resource_residency_census.h"

#include "../../dxvk/dxvk_cmdlist.h"
#include "../../dxvk/dxvk_device.h"
#include "../../dxvk/dxvk_gpu_query.h"

#include <chrono>
#include <array>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <deque>
#include <atomic>
#include <thread>
#include <cstdint>

namespace dxvk::war3 {

/**
 * @brief 单个计时区段的数据
 */
struct SectionTiming {
    uint32_t id = 0;
    uint32_t threadId = 0;
    double cpuMs = 0.0;
    double gpuMs = 0.0;
    double maxCpuMs = 0.0;
    double maxGpuMs = 0.0;
    uint32_t callCount = 0;
    // Number of completed timestamp pairs contributing to gpuMs. This keeps
    // per-path export correlation from treating an absent query as 0 ms.
    uint32_t gpuCount = 0;
};

/**
 * @brief Fixed-cost, once-per-receiver-run Shadow TAA contract telemetry.
 *
 * No strings or containers are allowed here. The receiver publishes this from
 * a scope-exit path so early returns remain visible in the report.
 */
struct ShadowTaaFrameTelemetry {
    uint32_t runtimeModuleEnabled = 0;
    uint32_t requestedMode = 0;
    uint32_t effectiveMode = 0;
    uint32_t shaderMode = 0;
    uint32_t blockedSemanticDynamic = 0;
    uint32_t blockedSunMotion = 0;
    uint32_t blockedCsmFallback = 0;
    uint32_t visibilityExecuted = 0;
    uint32_t motionVectorExecuted = 0;
    uint32_t receiverExecuted = 0;
    uint32_t historyWriteExecuted = 0;
    uint32_t historyAdvanced = 0;
    uint32_t historyAdvanceSkippedIncomplete = 0;
    uint32_t historyValidBefore = 0;
    uint32_t historyValidAfter = 0;
    uint32_t historyInvalidationMask = 0;
};

/**
 * @brief Numeric snapshot of the opt-in point-shadow persistent prepare path.
 *
 * Values are cumulative receiver-instance counters. The perf report stores the
 * latest snapshot and never translates reject reasons on the render hot path.
 */
struct PointShadowPersistentFrameTelemetry {
    uint32_t configuredMode = 0;
    uint32_t effectiveMode = 0;
    uint32_t lastBeginRejectReason = 0;
    uint32_t workerCreated = 0;
    uint32_t workerAvailable = 0;
    uint64_t beginAttempts = 0;
    uint64_t beginEligible = 0;
    uint64_t workerCreateCount = 0;
    uint64_t workerThreadStarts = 0;
    uint64_t accepted = 0;
    uint64_t ready = 0;
    uint64_t deadlineFallback = 0;
    uint64_t rejectedFallback = 0;
    uint64_t observeMatch = 0;
    uint64_t mismatch = 0;
    uint64_t consumed = 0;
    uint64_t failed = 0;
    uint64_t busy = 0;
};

/**
 * @brief Per-frame workload gauges used only by offline report correlation.
 *
 * This is populated once at the Present boundary. It deliberately contains no
 * strings or containers so retaining 3600 frames does not affect Hook hot
 * paths or materially grow the history ring.
 */
struct FrameWorkloadSnapshot {
    uint64_t businessFrameSerial = 0;
    bool hasShadowBudget = false;
    uint64_t capturedDrawCount = 0;
    // Sampled metadata-only draw hook cost for this Present interval. This is
    // kept in the per-frame series so p50/p95+MAD can reject foreground
    // pre-emption instead of relying on a polluted aggregate wall average.
    uint64_t shadowMetadataCaptureCalls = 0;
    uint64_t shadowMetadataCaptureUs = 0;
    uint64_t terrainDoodadCaptureAttemptCount = 0;
    uint64_t terrainDoodadCaptureAcceptedCount = 0;
    uint64_t terrainDoodadDynamicSourceCount = 0;
    uint64_t terrainDoodadWorldIdentityLikeCount = 0;
    uint64_t terrainDoodadWorldNonIdentityCount = 0;
    uint64_t terrainS1CaptureAttemptCount = 0;
    uint64_t terrainS1CaptureAcceptedCount = 0;
    uint64_t terrainS1WorldIdentityLikeCount = 0;
    uint64_t terrainS1WorldNonIdentityCount = 0;
    uint64_t terrainS1WorldNonFiniteCount = 0;
    uint64_t terrainS1ForceIdentityWorldCount = 0;
    uint64_t terrainS1WorldMatrixHash = 0;
    uint64_t terrainS1WorldTranslationMilliMax = 0;
    uint64_t stage13CaptureAttemptCount = 0;
    uint64_t stage13CaptureRejectedNoDemandCount = 0;
    uint64_t stage13CaptureRejectedAfterBeforeUiCount = 0;
    uint64_t stage13CaptureConsideredCount = 0;
    uint64_t stage13FreezeCopyBytes = 0;
    uint64_t stage13CpuSnapshotCopyBytes = 0;
    uint64_t stage13RetentionSnapshotBytes = 0;
    uint64_t stage13ReplayDrawCount = 0;
    uint64_t receiverCameraDeltaNano = 0;
    uint64_t receiverSunDeltaNano = 0;
    uint64_t receiverCsmDeltaNano = 0;
    uint64_t receiverSnappedCenterDeltaTexelsNano = 0;
    uint64_t receiverTexelSizeDeltaNano = 0;
    uint64_t shadowMapRenderSerial = 0;
    uint64_t uniqueGeometryCount = 0;
    uint64_t staticPersistentCount = 0;
    uint64_t dynamicPoseCount = 0;
    uint64_t dynamicSkinnedOutputCount = 0;
    uint64_t fallbackDrawCount = 0;
    uint64_t drawTimeVBCacheCaptureCount = 0;
    uint64_t drawTimeVBCacheConsumeHitCount = 0;
    uint64_t drawTimeVBCacheConsumeMissCount = 0;
    uint64_t drawTimeVBCacheRejectNoLayerContext = 0;
    uint64_t drawTimeVBCacheRejectContractLookup = 0;
    uint64_t drawTimeVBCacheRejectContractFreshness = 0;
    uint64_t drawTimeVBCacheRejectContractStage = 0;
    uint64_t drawTimeVBCacheRejectContractRenderFrame = 0;
    uint64_t drawTimeVBCacheRejectContractInstance = 0;
    uint64_t drawTimeVBCacheRejectContractSlice = 0;
    uint64_t drawTimeVBCacheSameFrameDedupMiss = 0;
    uint64_t drawTimeGenerationBackedPositionReuseCount = 0;
    uint64_t drawTimeGenerationBackedUvReuseCount = 0;
    uint64_t drawTimeGenerationBackedIndexReuseCount = 0;
    uint64_t drawTimeGenerationBackedCopyBytesSaved = 0;
    uint64_t semanticSceneSubmitted = 0;
    uint64_t semanticSceneSubmittedSkinned = 0;
    uint64_t skippedCasterCap = 0;
    uint64_t skippedDistanceCull = 0;
    bool hasShadowReceiver = false;
    uint64_t replayCasterCount = 0;
    uint64_t replayGeometryWork = 0;
    bool hasShadowTaaTelemetry = false;
    uint32_t shadowTaaRuntimeModuleEnabled = 0;
    uint32_t shadowTaaRequestedMode = 0;
    uint32_t shadowTaaEffectiveMode = 0;
    uint32_t shadowTaaShaderMode = 0;
    uint32_t shadowTaaBlockedSemanticDynamic = 0;
    uint32_t shadowTaaBlockedSunMotion = 0;
    uint32_t shadowTaaBlockedCsmFallback = 0;
    uint32_t shadowTaaVisibilityExecuted = 0;
    uint32_t shadowTaaMotionVectorExecuted = 0;
    uint32_t shadowTaaReceiverExecuted = 0;
    uint32_t shadowTaaHistoryWriteExecuted = 0;
    uint32_t shadowTaaHistoryAdvanced = 0;
    uint32_t shadowTaaHistoryAdvanceSkippedIncomplete = 0;
    uint32_t shadowTaaHistoryValidBefore = 0;
    uint32_t shadowTaaHistoryValidAfter = 0;
    uint32_t shadowTaaHistoryInvalidationMask = 0;
    uint64_t terrainDoodadPreparedCount = 0;
    uint64_t terrainS1PreparedCount = 0;
    uint64_t terrainDoodadCascade0DrawnCount = 0;
    uint64_t terrainDoodadCascade1DrawnCount = 0;
    uint64_t terrainDoodadCascade2DrawnCount = 0;
    uint64_t terrainDoodadCascade3DrawnCount = 0;
    uint64_t terrainS1Cascade0DrawnCount = 0;
    uint64_t terrainS1Cascade1DrawnCount = 0;
    uint64_t terrainS1Cascade2DrawnCount = 0;
    uint64_t terrainS1Cascade3DrawnCount = 0;
    uint32_t requestedShadowResolution = 0;
    uint32_t effectiveShadowResolution = 0;
    bool reusedLastCompleteShadowMap = false;
    bool renderedCurrentPartialShadowMap = false;
    bool hasPersistentGeometry = false;
    uint64_t rejectCapacity = 0;
    uint64_t rejectPositionBufferCreate = 0;
    uint64_t rejectIndexBufferCreate = 0;
    uint64_t rejectBlendBufferCreate = 0;
    uint64_t rejectUvBufferCreate = 0;
    uint64_t rejectRegistryInsert = 0;
    uint64_t rejectOther = 0;
    uint64_t createAttempts = 0;
    uint64_t bytesNeededTotal = 0;
    uint64_t bytesNeededMax = 0;
    uint64_t bytesNeededLast = 0;
    uint64_t forceGcRequests = 0;
    uint64_t forceGcNoBytesFreed = 0;
    uint64_t forceGcStillInsufficient = 0;
    uint64_t forceGcBytesFreed = 0;
    uint64_t capacityRejectAllCallers = 0;
    uint64_t capacityFastReject = 0;
    uint64_t expiryTokensPopped = 0;
    uint64_t expiryTokensRequeued = 0;
    uint64_t expiryStaleTokens = 0;
    uint64_t expiryAgeEvictions = 0;
    uint64_t expiryQueueSize = 0;
    uint64_t bytesCap = 0;
    uint64_t bytesUsed = 0;
    uint64_t bytesEvicted = 0;
    uint64_t bytesEvictedDelta = 0;
    uint64_t liveGeometryCount = 0;
    uint64_t s1EarlyEntryCount = 0;
    uint64_t s1EarlyPersistentBackedCount = 0;
    uint64_t s1EarlyPersistentReverseIndexCount = 0;
    uint64_t s1EarlyFallbackBackedCount = 0;
    uint64_t s1EarlyLogicalReferencedBytes = 0;
    uint64_t s1EarlyAcceptedHitCount = 0;
    uint64_t s1EarlyReplayPublishedCount = 0;
    uint64_t s1EarlyReplayInstanceCount = 0;
    uint64_t s1EarlyReplayFallbackCount = 0;
    uint64_t s1EarlySourceMismatchEvictCount = 0;
    bool s1EarlyReplayClosureMismatch = false;
};

/**
 * @brief 单帧的性能快照
 */
struct FrameSnapshot {
    uint64_t frameIndex = 0;
    // Monotonic capture epoch. Unlike frameIndex, this is never reset and is
    // therefore safe for delayed worker/GPU samples across report resets.
    uint64_t frameEpoch = 0;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    double processCpuMs = 0.0;
    double mainThreadCpuMs = 0.0;
    double workerThreadsCpuMs = 0.0;
    // GPU timestamp queries are asynchronous. A numeric zero is meaningful
    // only after at least one query completed for this Present frame; without
    // this bit, a missing sample was indistinguishable from measured 0 ms.
    bool hasGpuTiming = false;
    bool hasProcessCpu = false;
    bool hasMainThreadCpu = false;
    bool mainThreadCpuClampedToProcess = false;
    std::chrono::steady_clock::time_point timestamp;
    std::vector<SectionTiming> sections;
    // Raw device timestamp intervals for every completed GPU scope assigned
    // to this Present epoch. Section timings intentionally retain inclusive
    // scope costs, while totalGpuMs is the union of these intervals so nested
    // scopes cannot count the same GPU work more than once.
    std::vector<std::array<uint64_t, 2>> gpuTimestampIntervals;
    FrameWorkloadSnapshot workload;
};

/**
 * @brief War3 性能监控器（增强版）
 * 
 * 功能：
 * - CPU/GPU 分段计时
 * - 分层 Scope（父子关系）
 * - 帧历史记录（环形缓冲区）
 * - HTML 报告导出
 */
class War3PerfMonitor final {
public:
    ~War3PerfMonitor();

    /**
     * @brief One completed Present-to-Present interval of persistent geometry
     * diagnostics.
     *
     * Reject counters refine the legacy persistentRejectCreateOrBudget
     * ShadowCapture bucket. Creation counters include both shared-helper calls
     * and the equivalent ShadowCapture capacity preflight. Pool and S1 early
     * values are end-of-interval gauges, not additive byte traffic.
     */
    struct PersistentGeometryFrameStats {
        uint64_t rejectCapacity = 0;
        uint64_t rejectPositionBufferCreate = 0;
        uint64_t rejectIndexBufferCreate = 0;
        uint64_t rejectBlendBufferCreate = 0;
        uint64_t rejectUvBufferCreate = 0;
        uint64_t rejectRegistryInsert = 0;
        uint64_t rejectOther = 0;
        uint64_t createAttempts = 0;
        uint64_t bytesNeededTotal = 0;
        uint64_t bytesNeededMax = 0;
        uint64_t bytesNeededLast = 0;
        uint64_t forceGcRequests = 0;
        uint64_t forceGcNoBytesFreed = 0;
        uint64_t forceGcStillInsufficient = 0;
        uint64_t forceGcBytesFreed = 0;
        uint64_t capacityRejectAllCallers = 0;
        uint64_t capacityFastReject = 0;
        uint64_t expiryTokensPopped = 0;
        uint64_t expiryTokensRequeued = 0;
        uint64_t expiryStaleTokens = 0;
        uint64_t expiryAgeEvictions = 0;
        uint64_t expiryQueueSize = 0;
        uint64_t bytesCap = 0;
        uint64_t bytesUsed = 0;
        uint64_t bytesEvicted = 0;
        uint64_t liveGeometryCount = 0;
        uint64_t s1EarlyEntryCount = 0;
        uint64_t s1EarlyPersistentBackedCount = 0;
        uint64_t s1EarlyPersistentReverseIndexCount = 0;
        uint64_t s1EarlyFallbackBackedCount = 0;
        uint64_t s1EarlyLogicalReferencedBytes = 0;
        uint64_t s1EarlyAcceptedHitCount = 0;
        uint64_t s1EarlyReplayPublishedCount = 0;
        uint64_t s1EarlyReplayInstanceCount = 0;
        uint64_t s1EarlyReplayFallbackCount = 0;
        uint64_t s1EarlySourceMismatchEvictCount = 0;
        bool s1EarlyReplayClosureMismatch = false;
        uint64_t s1GenerationProofEntryCount = 0;
        uint64_t s1GenerationProofEligibleCount = 0;
        uint64_t s1GenerationProofFirstCount = 0;
        uint64_t s1GenerationProofSameFrameCount = 0;
        uint64_t s1GenerationProofAdvancedCount = 0;
        uint64_t s1GenerationProofChangedCount = 0;
        uint64_t s1GenerationProofStaleRestartCount = 0;
        uint64_t s1GenerationProofPromotionReadyCount = 0;
        uint64_t s1GenerationProofCapacityRejectCount = 0;
    };

    /**
     * @brief RAII 风格的分段计时器
     */
    class ScopedSection final {
    public:
        ScopedSection() = default;
        ScopedSection(War3PerfMonitor* monitor,
                      uint32_t id,
                      const Rc<DxvkCommandList>& ctx);
        ~ScopedSection();

        ScopedSection(const ScopedSection&) = delete;
        ScopedSection& operator=(const ScopedSection&) = delete;
        ScopedSection(ScopedSection&& other) noexcept;
        ScopedSection& operator=(ScopedSection&& other) noexcept;

    private:
        War3PerfMonitor* m_monitor = nullptr;
        uint32_t m_id = 0;
        uint64_t m_frameEpoch = 0;
        std::chrono::steady_clock::time_point m_cpuStart;
        Rc<DxvkGpuQuery> m_gpuStart;
        Rc<DxvkCommandList> m_ctx;
    };

    /**
     * @brief 帧边界守卫
     */
    class FrameGuard final {
    public:
        explicit FrameGuard(War3PerfMonitor* monitor);
        ~FrameGuard();

        FrameGuard(const FrameGuard&) = delete;
        FrameGuard& operator=(const FrameGuard&) = delete;

    private:
        War3PerfMonitor* m_monitor = nullptr;
    };

    /**
     * @brief 纯 CPU 的分层 Scope（RAII）
     */
    class ScopedCpuScope final {
    public:
        ScopedCpuScope() = default;
        ScopedCpuScope(War3PerfMonitor* monitor, const char* name,
                       uint32_t sampleWeight = 1,
                       bool detailSample = false);
        ~ScopedCpuScope();

        ScopedCpuScope(const ScopedCpuScope&) = delete;
        ScopedCpuScope& operator=(const ScopedCpuScope&) = delete;
        ScopedCpuScope(ScopedCpuScope&& other) noexcept;
        ScopedCpuScope& operator=(ScopedCpuScope&& other) noexcept;

    private:
        War3PerfMonitor* m_monitor = nullptr;
        uint32_t m_sampleWeight = 1;
        bool m_detailSample = false;
    };

    static War3PerfMonitor& instance();

    void setDevice(DxvkDevice* device);
    // 仅为资源普查提供 allocator 数值快照；monitor 不拥有该指针。
    void setResourceCensusAllocator(D3D9MemoryAllocator* allocator);
    void clearResourceCensusAllocator(D3D9MemoryAllocator* allocator);
    void shutdown();

    // 帧管理
    void beginFrame();
    void endFrame();
    FrameGuard makeFrameGuard();

    // 分段计时
    ScopedSection scope(const char* name, const Rc<DxvkCommandList>& ctx);
    ScopedCpuScope cpuScope(const char* name);
    // 带采样权重的变体：popScope 时按 sampleWeight 加权（Horvitz-Thompson）。
    ScopedCpuScope cpuScope(const char* name, uint32_t sampleWeight);
    // 详细 trace 专用：默认是空 scope。仅 DXVK_WAR3_PERF_TRACE=detail 时
    // 按 DXVK_WAR3_PERF_TRACE_SAMPLE_PERIOD 采样，适合 per-draw 热路径。
    ScopedCpuScope cpuDetailScope(const char* name);
    /**
     * @brief 手工写入 CPU 采样（用于跨函数聚合结果上报）
     * @param name 节点名
     * @param cpuMs 累计 CPU 时间（毫秒）
     * @param parentPath 父节点路径（可选）
     * @param calls 调用次数增量（默认 1）
     */
    void addCpuSample(const char* name,
                      double cpuMs,
                      const char* parentPath = nullptr,
                      uint32_t calls = 1);

    /**
     * @brief 将已在调用端聚合的 CPU 采样挂到当前 TLS scope 下
     *
     * 用于微秒级热循环：循环内只累加裸计时，循环结束后一次写入，避免
     * 每条记录 push/pop scope 的字符串、哈希和队列成本污染被测代码。
     */
    void addCpuSampleToCurrentScope(const char* name,
                                    double cpuMs,
                                    uint32_t calls = 1);

    /**
     * @brief 将聚合样本挂到“当前 scope 下的一个合成父节点”
     *
     * 调用端应先用 addCpuSampleToCurrentScope(parentName, ...) 写入父样本，
     * 再用本接口写子阶段。这样低频裸计时仍能形成真实 parentPath 调用树，
     * 无需为了建立层级而在热循环中 push/pop scope。
     */
    void addCpuSampleToCurrentScopeChild(const char* parentName,
                                         const char* childName,
                                         double cpuMs,
                                         uint32_t calls = 1);

    /**
     * @brief 将聚合样本挂到当前 scope 下的任意相对父路径
     *
     * 只允许在低频 flush/frame 回填点调用；热循环不得构造 relativeParentPath。
     * 该接口用于把固定 ID 的高频 Hook 聚合桶恢复成真实嵌套调用树。
     */
    void addCpuSampleToCurrentScopeRelative(const char* relativeParentPath,
                                            const char* name,
                                            double cpuMs,
                                            uint32_t calls = 1);
    
    // 分层 Scope（用于无 CommandList 的纯 CPU 计时）
    void pushScope(const char* name, uint32_t sampleWeight = 1,
                   bool detailSample = false);
    void popScope(uint32_t sampleWeight = 1);

    // 开关
    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    void setRecording(bool recording) { m_recording.store(recording, std::memory_order_relaxed); }
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    bool isRecording() const { return m_recording.load(std::memory_order_relaxed); }
    void resetHistory();
    bool waitForPendingExports(uint32_t timeoutMs = 15000);
    uint32_t getPendingExportCount() const;
    void noteShadowBudgetFrame(const War3ShadowCaptureStats& stats);
    void noteShadowMetadataFrame(uint64_t captureUs, uint64_t captureCalls);
    void notePersistentGeometryFrame(
        const PersistentGeometryFrameStats& stats);
    // 登记业务帧主序号（m_war3ShadowPersistentFrameSerial），用于报告里
    // 与 perf frameEpoch 建立对齐点。仅原子写，热路径无锁。
    void noteBusinessFrameSerial(uint64_t serial);
    void noteShadowMapFallback(bool reusedLastComplete, bool renderedCurrentPartial);
    void noteShadowReceiverFrame(uint32_t replayCasterCount,
                                 uint64_t replayGeometryWork,
                                 uint32_t requestedShadowResolution,
                                 uint32_t effectiveShadowResolution);
    void noteShadowTaaFrame(const ShadowTaaFrameTelemetry& telemetry);
    void notePointShadowPersistentFrame(
        const PointShadowPersistentFrameTelemetry& telemetry);

    // 报告导出
    void exportHtmlReport(const std::string& outputPath);
    void cleanupTempFolder();
    std::string generateJsonData(double windowSec = 0.0) const;

    // 获取统计
    const std::deque<FrameSnapshot>& getFrameHistory() const { return m_frameHistory; }
    size_t getMaxHistorySize() const { return m_maxHistorySize; }
    void setMaxHistorySize(size_t size) { m_maxHistorySize = size; }

private:
    War3PerfMonitor();

    struct SectionStats {
        std::string name;
        std::string path;
        std::string parentPath;
        uint64_t cpuCount = 0;
        double cpuSumMs = 0.0;
        uint64_t gpuCount = 0;
        double gpuSumMs = 0.0;
        double cpuMaxMs = 0.0;
        double gpuMaxMs = 0.0;
    };

    struct PendingSample {
        uint32_t id = 0;
        uint32_t threadId = 0;
        uint64_t frameEpoch = 0;
        double cpuMs = 0.0;
        Rc<DxvkGpuQuery> gpuBegin;
        Rc<DxvkGpuQuery> gpuEnd;
    };

    struct CpuOnlyScope {
        std::string name;
        std::string path;
        std::string parentPath;
        // 动态父路径：push 时 TLS 栈顶（可能为空）。绝对路径 name 在嵌套
        // 上下文中，报表的 self/覆盖归因应以真实调用栈父节点为准。
        std::string dynamicParentPath;
        uint32_t sampleWeight = 1;
        uint64_t frameEpoch = 0;
        std::chrono::steady_clock::time_point start;
    };

    struct CpuProbeSnapshot {
        uint64_t processCpu100ns = 0;
        uint64_t mainThreadCpu100ns = 0;
        DWORD mainThreadId = 0;
        bool hasProcess = false;
        bool hasMainThread = false;
    };

    struct CpuDelta {
        uint32_t id = 0;
        uint32_t threadId = 0;
        uint64_t frameEpoch = 0;
        double cpuMs = 0.0;
        double maxCpuMs = 0.0;
        uint32_t calls = 0;
    };

    struct ProfilerCounters {
        uint64_t coarseScopeAttempts = 0;
        uint64_t detailScopeAttempts = 0;
        uint64_t detailScopeSampled = 0;
        uint64_t lateCpuDeltasRecovered = 0;
        uint64_t lateGpuSamplesRecovered = 0;
        uint64_t gpuSamplesDropped = 0;
        uint64_t lateCpuDeltasDropped = 0;
    };

    struct SectionIdCacheEntry {
        // 必须拥有字符串。Scope 的 name/parentPath 常来自栈上 std::string；
        // 保存裸指针不仅永远命不中下一次同名 Scope，还会留下悬空地址。
        std::string name;
        std::string parentPath;
        size_t nameHash = 0;
        size_t parentHash = 0;
        uint32_t id = 0;
        bool occupied = false;
    };

    struct ThreadCpuState {
        // 一个 detail Hook 报告通常有 200-300 个动态 section。旧 64 项
        // FIFO + 全表线性扫描会每帧抖出并反复进入全局 mutex。512 项
        // power-of-two 表配合小范围哈希探测可让稳定路径留在 TLS。
        std::array<SectionIdCacheEntry, 512> sectionIdCache = {};
        uint32_t nextCacheSlot = 0;
        std::vector<CpuDelta> pendingDeltas;
        uint64_t coarseScopeAttempts = 0;
        uint64_t detailScopeAttempts = 0;
        uint64_t detailScopeSampled = 0;
        uint64_t lateCpuDeltasDropped = 0;
    };

    struct ShadowBudgetAggregate {
        struct PhaseAggregate {
            const char* name = "";
            uint64_t requestedBytes = 0;
            uint64_t acceptedBytes = 0;
            uint64_t rejectedBytes = 0;
            uint64_t requests = 0;
            uint64_t rejects = 0;
        };

        uint64_t framesObserved = 0;
        uint64_t framesIncomplete = 0;
        uint64_t framesProducerIncomplete = 0;
        uint64_t producerRequiredCasterOmissionCount = 0;
        uint64_t producerExactBudgetDeferredUniqueCasterCount = 0;
        uint64_t producerPositionAllocBudgetCount = 0;
        uint64_t producerUvAllocBudgetCount = 0;
        uint64_t producerIndexAllocBudgetCount = 0;
        uint64_t producerAllocationFailureCount = 0;
        uint64_t producerFallbackByteBudgetCount = 0;
        uint64_t producerArenaAdmissionCount = 0;
        uint64_t producerFreezeFailureCount = 0;
        uint64_t producerSoftPriorityBudgetCount = 0;
        uint64_t producerCompletenessCounterOverflowFrames = 0;
        uint64_t producerSealFrameSerialLast = 0;
        uint64_t producerSealMapEpochLast = 0;
        uint64_t producerSealDeviceEpochLast = 0;
        uint64_t drawTimeVBCacheStaticLiveBytesLast = 0;
        uint64_t drawTimeVBCacheStaticProtectedBytesLast = 0;
        uint64_t drawTimeVBCacheStaticOverCapBytesLast = 0;
        uint64_t drawTimeVBCacheStaticOverCapFrameCountLast = 0;
        uint64_t drawTimeVBCacheStaticEvictedBytes = 0;
        uint64_t drawTimeVBCacheStaticEvictedEntryCount = 0;
        uint64_t drawTimeVBCacheIndexedUnknownRangeFallbackCount = 0;
        uint64_t framesBudgetExceeded = 0;
        uint64_t framesReuseLastComplete = 0;
        uint64_t framesRenderCurrentPartial = 0;
        uint64_t shadowReceiverFrames = 0;
        uint64_t shadowReceiverReplayCasterCountTotal = 0;
        uint64_t shadowReceiverReplayCasterCountMax = 0;
        uint64_t shadowReceiverReplayGeometryWorkTotal = 0;
        uint64_t shadowReceiverReplayGeometryWorkMax = 0;
        uint64_t shadowReceiverAdaptiveResolutionFrames = 0;
        uint64_t shadowReceiverRequestedResolutionLast = 0;
        uint64_t shadowReceiverEffectiveResolutionLast = 0;
        uint64_t shadowTaaFramesObserved = 0;
        uint64_t shadowTaaRuntimeModuleDisabledFrames = 0;
        uint64_t shadowTaaRequestedDirectFrames = 0;
        uint64_t shadowTaaRequestedPrepassFrames = 0;
        uint64_t shadowTaaRequestedTemporalFrames = 0;
        uint64_t shadowTaaEffectiveDirectFrames = 0;
        uint64_t shadowTaaEffectivePrepassFrames = 0;
        uint64_t shadowTaaEffectiveTemporalFrames = 0;
        uint64_t shadowTaaBlockedSemanticDynamicFrames = 0;
        uint64_t shadowTaaBlockedSunMotionFrames = 0;
        uint64_t shadowTaaBlockedCsmFallbackFrames = 0;
        uint64_t shadowTaaVisibilityExecutedFrames = 0;
        uint64_t shadowTaaMotionVectorExecutedFrames = 0;
        uint64_t shadowTaaReceiverExecutedFrames = 0;
        uint64_t shadowTaaHistoryWriteExecutedFrames = 0;
        uint64_t shadowTaaHistoryAdvancedFrames = 0;
        uint64_t shadowTaaHistoryAdvanceSkippedIncompleteFrames = 0;
        uint64_t shadowTaaHistoryValidBeforeFrames = 0;
        uint64_t shadowTaaHistoryValidAfterFrames = 0;
        uint64_t shadowTaaHistoryInvalidatedFrames = 0;
        std::array<uint64_t, 11> shadowTaaHistoryInvalidationReasonFrames = {};
        uint32_t shadowTaaHistoryInvalidationMaskLast = 0;
        uint32_t shadowTaaRequestedModeLast = 0;
        uint32_t shadowTaaEffectiveModeLast = 0;
        uint32_t shadowTaaShaderModeLast = 0;
        uint64_t pointShadowPersistentFramesObserved = 0;
        PointShadowPersistentFrameTelemetry
            pointShadowPersistentLast = {};
        uint64_t stage13CaptureAttemptCount = 0;
        uint64_t stage13CaptureRejectedNoDemandCount = 0;
        uint64_t stage13CaptureRejectedAfterBeforeUiCount = 0;
        uint64_t stage13CaptureConsideredCount = 0;
        uint64_t stage13FreezeCopyBytes = 0;
        uint64_t stage13CpuSnapshotCopyBytes = 0;
        uint64_t stage13RetentionSnapshotBytes = 0;
        uint64_t stage13ReplayDrawCount = 0;
        uint64_t totalBudgetBytes = 0;
        uint64_t totalUsedBytes = 0;
        uint64_t maxBudgetBytes = 0;
        uint64_t maxUsedBytes = 0;
        uint64_t totalArenaBytes = 0;
        uint64_t maxArenaBytes = 0;
        uint64_t arenaClaimCount = 0;
        uint64_t arenaDispatchClaims = 0;
        uint64_t arenaDispatchClaimMisses = 0;
        uint64_t arenaCaptured = 0;
        uint64_t legacyCaptured = 0;
        uint64_t directRefCaptured = 0;
        uint64_t arenaFallbackToFreeze = 0;
        uint64_t arenaDedup = 0;
        uint64_t vbLevelCacheHits = 0;
        uint64_t arenaOverflowCount = 0;
        uint64_t arenaEmitRejects = 0;
        uint64_t directRefRejectedUnstable = 0;
        uint64_t arenaRebasedIndexed = 0;
        uint64_t arenaRebasedNonIndexed = 0;
        uint64_t arenaRejectUnsupported = 0;
        uint64_t arenaRejectValidation = 0;
        uint64_t arenaRejectUninitialized = 0;
        uint64_t arenaRejectOverflow = 0;
        uint64_t skippedFreezeBudget = 0;
        uint64_t skippedPriorityBudget = 0;
        uint64_t skippedUpload = 0;
        uint64_t skippedCasterCap = 0;
        uint64_t skippedDistanceCull = 0;
        uint64_t degradedAlphaBudget = 0;
        uint64_t reusedFreezeHits = 0;
        uint64_t actualFreezeReuseHits = 0;
        uint64_t uniqueGeometryCount = 0;
        uint64_t uniqueInstanceableGeometryCount = 0;
        uint64_t duplicateGeometryInstances = 0;
        uint64_t reuseEligibleDuplicates = 0;
        uint64_t potentialFreezeReuseHits = 0;
        uint64_t staticPersistentCount = 0;
        uint64_t dynamicPoseCount = 0;
        uint64_t dynamicSkinnedOutputCount = 0;
        uint64_t fallbackDrawCount = 0;
        uint64_t fallbackDrawCountTerrain = 0;
        uint64_t fallbackDrawCountWorldObject = 0;
        uint64_t fallbackDrawCountUnitObject = 0;
        uint64_t objectFallbackDrawCount = 0;
        uint64_t drawTimeVBCacheCaptureCount = 0;
        uint64_t drawTimeVBCacheConsumeHitCount = 0;
        uint64_t drawTimeVBCacheConsumeMissCount = 0;
        uint64_t drawTimeAllocObserverFrames = 0;
        uint64_t drawTimePositionAllocRequestCount = 0;
        uint64_t drawTimePositionAllocNewEntryCount = 0;
        uint64_t drawTimePositionAllocMissingBackingCount = 0;
        uint64_t drawTimePositionAllocCapacityGrowthCount = 0;
        uint64_t drawTimePositionAllocLeaseDetachCount = 0;
        uint64_t drawTimePositionAllocStaticRequestCount = 0;
        uint64_t drawTimePositionAllocDynamicRequestCount = 0;
        uint64_t drawTimePositionDeferredNewEntryCount = 0;
        uint64_t drawTimePositionDeferredMissingBackingCount = 0;
        uint64_t drawTimePositionDeferredCapacityGrowthCount = 0;
        uint64_t drawTimePositionDeferredLeaseDetachCount = 0;
        uint64_t drawTimePositionProofUniqueCount = 0;
        uint64_t drawTimePositionProofDuplicateCount = 0;
        uint64_t drawTimePositionProofInvalidCount = 0;
        uint64_t drawTimePositionProofSetOverflowCount = 0;
        uint64_t drawTimePositionProofUniqueBytes = 0;
        uint64_t drawTimePositionProofDuplicateBytes = 0;
        uint64_t drawTimeDirectStaticPositionBindCount = 0;
        uint64_t drawTimeDirectStaticPositionBytes = 0;
        uint64_t drawTimeDirectStaticIndexBindCount = 0;
        uint64_t drawTimeDirectStaticIndexBytes = 0;
        uint64_t drawTimeDirectUploadPositionBindCount = 0;
        uint64_t drawTimeDirectUploadPositionBytes = 0;
        uint64_t drawTimeDirectUploadUvBindCount = 0;
        uint64_t drawTimeDirectUploadUvBytes = 0;
        uint64_t drawTimeDirectUploadIndexBindCount = 0;
        uint64_t drawTimeDirectUploadIndexBytes = 0;
        uint64_t drawTimeDirectUploadCandidateCount = 0;
        uint64_t drawTimeDirectUploadRejectNoProofCount = 0;
        uint64_t drawTimeDirectUploadRejectNoStorageCount = 0;
        uint64_t drawTimeDirectUploadRejectRangeCount = 0;
        uint64_t drawTimePositionAllocDirectMutableRequestCount = 0;
        uint64_t drawTimeVBCacheRejectNoLayerContext = 0;
        uint64_t drawTimeVBCacheSameFrameDedupMiss = 0;
        uint64_t drawTimeGenerationBackedPositionReuseCount = 0;
        uint64_t drawTimeGenerationBackedUvReuseCount = 0;
        uint64_t drawTimeGenerationBackedIndexReuseCount = 0;
        uint64_t drawTimeGenerationBackedCopyBytesSaved = 0;
        uint64_t semanticBridgeHit = 0;
        uint64_t semanticBridgeMiss = 0;
        uint64_t semanticBridgeBypassed = 0;
        uint64_t semanticSceneSubmitted = 0;
        uint64_t semanticSceneSubmittedUnit = 0;
        uint64_t semanticSceneSubmittedSkinned = 0;
        uint64_t semanticSceneSubmittedSkinnedNonUnitResolvedCount = 0;
        uint64_t semanticSceneSubmittedSkinnedUnknownPacketKindCount = 0;
        uint64_t semanticSceneSubmittedSkinnedUnitPtrNonUnitResolvedCount = 0;
        uint64_t semanticSceneSubmittedSkinnedGroupNonZeroCount = 0;
        uint64_t semanticSceneSubmittedSkinnedTransparentQueueCount = 0;
        uint64_t semanticSceneSubmittedSkinnedMissingUnitPtrCount = 0;
        uint64_t semanticSceneSubmittedSkinnedDynamicUnitEvidenceCount = 0;
        uint64_t semanticSceneSubmittedBuilding = 0;
        uint64_t semanticSceneSubmittedDestructible = 0;
        uint64_t semanticSceneSubmittedCutout = 0;
        uint64_t semanticSceneSubmittedAlphaBlend = 0;
        uint64_t semanticSceneMaterialObservedCutoutCount = 0;
        uint64_t semanticSceneMaterialObservedAlphaBlendCount = 0;
        uint64_t semanticSceneRejectedCutoutSkinnedContract = 0;
        uint64_t semanticSceneRejectedAlphaBlendSkinnedContract = 0;
        uint64_t semanticSceneRejectedCutoutGeometry = 0;
        uint64_t semanticSceneRejectedAlphaBlendGeometry = 0;
        uint64_t semanticSceneRejectedCutoutVisualPolicy = 0;
        uint64_t semanticSceneRejectedAlphaBlendVisualPolicy = 0;
        uint64_t semanticSceneMaterialLayerContractResolvedCount = 0;
        uint64_t semanticSceneMaterialLayerContractFailedCount = 0;
        uint64_t semanticSceneMaterialBlendMode0Count = 0;
        uint64_t semanticSceneMaterialBlendMode1Count = 0;
        uint64_t semanticSceneMaterialBlendMode2PlusCount = 0;
        uint64_t semanticSceneDirectCurrentDrawLayerIndexNonZeroCount = 0;
        uint64_t semanticSceneFastAppendBoundsPoseAvailableCount = 0;
        uint64_t semanticSceneFastAppendBoundsSceneReadSuccessCount = 0;
        uint64_t semanticSceneFastAppendBoundsPoseDeltaLe1Count = 0;
        uint64_t semanticSceneFastAppendBoundsPoseDeltaLe4Count = 0;
        uint64_t semanticSceneFastAppendBoundsPoseDeltaLe16Count = 0;
        uint64_t semanticSceneFastAppendBoundsPoseDeltaGt16Count = 0;
        uint32_t semanticSceneFastAppendBoundsPoseDeltaMaxMilli = 0;
        uint64_t semanticSceneLivePaletteRefreshAttemptCount = 0;
        uint64_t semanticSceneLivePaletteRefreshHitCount = 0;
        uint64_t semanticSceneLivePaletteRefreshMissCount = 0;
        uint64_t semanticSceneAuthoritativePaletteLiveSlotFallbackBlockedCount = 0;
        uint64_t semanticScenePaletteOverrideNoComposeCount = 0;
        uint64_t semanticScenePaletteOverrideWouldComposeCount = 0;
        uint64_t semanticScenePalettePacketWorldComposeCount = 0;
        uint64_t semanticSceneLivePaletteMotionSampleCount = 0;
        uint64_t semanticSceneLivePaletteMotionNewRuntimeCount = 0;
        uint64_t semanticSceneLivePaletteMotionRawChangedCount = 0;
        uint64_t semanticSceneLivePaletteMotionRawStableCount = 0;
        uint64_t semanticSceneLivePaletteMotionGroupChangedCount = 0;
        uint64_t semanticSceneLivePaletteMotionGroupStableCount = 0;
        uint64_t semanticSceneDrawTimePoseAttemptCount = 0;
        uint64_t semanticSceneDrawTimePosePublishedCount = 0;
        uint64_t semanticSceneDrawTimePoseRejectUiOrEffectCount = 0;
        uint64_t semanticSceneDrawTimePoseRejectVertexShaderCount = 0;
        uint64_t semanticSceneDrawTimePoseRejectNoVertexBlendCount = 0;
        uint64_t semanticSceneDrawTimePoseRejectNoContextCount = 0;
        uint64_t semanticSceneDrawTimePoseRejectNoRuntimeModelCount = 0;
        uint64_t semanticSceneDrawTimePoseDedupedCount = 0;
        uint64_t semanticSceneDrawTimePoseChangedCount = 0;
        uint64_t semanticSceneDrawTimePoseStableCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionSampleCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionNewRuntimeCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionChangedCount = 0;
        uint64_t semanticSceneSubmittedPaletteMotionStableCount = 0;
        uint64_t semanticSceneSkinnedDynamicIndexSliceCount = 0;
        uint64_t semanticSceneSubmittedOwnedGroupSlots = 0;
        uint64_t semanticSceneSubmittedExplicitBlendContract = 0;
        uint64_t semanticSceneSubmittedSingleMatrixGroupSkinning = 0;
        uint64_t semanticSceneSubmittedMultiGroupSlotSkinning = 0;
        uint64_t semanticSceneSkinnedMinUniqueGroupSlots = 0;
        uint64_t semanticSceneSkinnedMaxUniqueGroupSlots = 0;
        uint64_t semanticSceneSkinnedGroupSlotsUnique1Count = 0;
        uint64_t semanticSceneSkinnedGroupSlotsUnique2To4Count = 0;
        uint64_t semanticSceneSkinnedGroupSlotsUnique5To8Count = 0;
        uint64_t semanticSceneSkinnedGroupSlotsUnique9To16Count = 0;
        uint64_t semanticSceneSkinnedGroupSlotsUnique17PlusCount = 0;
        uint64_t semanticSceneExplicitBlendUnavailableCurrentDraw = 0;
        uint64_t semanticSceneCurrentDrawContractKnownCount = 0;
        uint64_t semanticSceneCurrentDrawPaletteReadyCount = 0;
        uint64_t semanticSceneCurrentDrawGroupSlotReadyCount = 0;
        uint64_t semanticSceneCurrentDrawResolveReadyCount = 0;
        uint64_t semanticSceneCurrentDrawMissNoContract = 0;
        uint64_t semanticSceneCurrentDrawMissNoPalette = 0;
        uint64_t semanticSceneCurrentDrawMissNoGroupSlots = 0;
        uint64_t semanticSceneCurrentDrawMissStaleVisibleFrame = 0;
        uint64_t semanticSceneCurrentDrawResolveReadyRejectedCount = 0;
        uint64_t semanticSceneCanonicalReadyCount = 0;
        uint64_t semanticSceneCanonicalReadyCutoutCount = 0;
        uint64_t semanticSceneCanonicalReadyAlphaBlendCount = 0;
        uint64_t semanticSceneCanonicalRejectNoStableIdentity = 0;
        uint64_t semanticSceneCanonicalRejectNoMesh = 0;
        uint64_t semanticSceneCanonicalRejectNoWorldTransform = 0;
        uint64_t semanticSceneCanonicalRejectNoPalette = 0;
        uint64_t semanticSceneCanonicalRejectNoSlotContract = 0;
        uint64_t semanticSceneCanonicalRejectStaleProducer = 0;
        uint64_t semanticSceneCanonicalRejectInvalidVertexIndex = 0;
        uint64_t semanticSceneCanonicalRejectExplicitBlendIncomplete = 0;
        uint64_t semanticSceneCanonicalRejectAfterReadyCount = 0;
        uint64_t semanticSceneSkinnedFullIndexFallbackCount = 0;
        uint64_t semanticSceneSkinnedMissingVisibleIndexSliceRejectCount = 0;
        uint64_t semanticSceneSubmittedFrameLocal = 0;
        uint64_t semanticSceneSubmittedPersistent = 0;
        uint64_t semanticScenePopulateAttemptCount = 0;
        uint64_t semanticScenePopulateUnitsOnlyCount = 0;
        uint64_t semanticScenePopulateLastReturnReason = 0;
        uint64_t semanticScenePopulateLastProducerPublishAttemptDelta = 0;
        uint64_t semanticScenePopulateLastProducerPublishReadyDelta = 0;
        uint64_t semanticScenePopulateLastProducerQueryAttemptDelta = 0;
        uint64_t semanticScenePopulateLastProducerQueryHitDelta = 0;
        uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryAttemptDelta = 0;
        uint64_t semanticScenePopulateLastProducerCapturedPaletteQueryHitDelta = 0;
        uint64_t semanticScenePopulateLastProducerGroupDecodeAttemptDelta = 0;
        uint64_t semanticScenePopulateLastProducerGroupDecodeHitDelta = 0;
        uint64_t semanticSceneDirectCurrentDrawRecordCount = 0;
        uint64_t semanticSceneDirectCurrentDrawBuiltPacketCount = 0;
        uint64_t semanticSceneDirectCurrentDrawBuiltSkinnedPacketCount = 0;
        uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNonSkinnedCount = 0;
        uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoIdentityCount = 0;
        uint64_t semanticSceneDirectCurrentDrawUnitsFilterRejectNoStableResourceCount = 0;
        // Close the DirectGrouped admission ledger in exported perf reports.
        // These counters already exist in the authoritative per-frame shadow
        // stats, but omitting them here made an exact-owner suppression look
        // indistinguishable from an expensive late append failure.
        uint64_t drawTimeSemanticProducerVisibleCandidateCount = 0;
        uint64_t drawTimeSemanticProducerFreshEntryCount = 0;
        uint64_t drawTimeSemanticProducerClaimedCount = 0;
        uint64_t drawTimeSemanticProducerSubmittedCount = 0;
        uint64_t drawTimeSemanticProducerMissNoFreshEntryCount = 0;
        uint64_t drawTimeSemanticProducerFallbackCurrentDrawCount = 0;
        uint64_t drawTimeSemanticProducerOwnedDirectGroupedSkipCount = 0;
        uint64_t drawTimeSemanticProducerLifecycleMergedCount = 0;
        uint64_t semanticSceneProducerClaimObserveMode = 0;
        uint64_t semanticSceneProducerClaimExactKeyCount = 0;
        uint64_t semanticSceneProducerClaimCandidateCount = 0;
        uint64_t semanticSceneProducerClaimCanonicalOwnedCount = 0;
        uint64_t semanticSceneProducerClaimMissingKeyCount = 0;
        uint64_t semanticSceneProducerClaimUnresolvedCount = 0;
        uint64_t semanticSceneProducerClaimStrictPredictedCount = 0;
        uint64_t semanticSceneProducerClaimStrictMatchCount = 0;
        uint64_t semanticSceneProducerClaimStrictFalsePositiveCount = 0;
        uint64_t semanticSceneProducerClaimStrictFalseNegativeCount = 0;
        uint64_t semanticSceneProducerClaimLogicalPredictedCount = 0;
        uint64_t semanticSceneProducerClaimLogicalMatchCount = 0;
        uint64_t semanticSceneProducerClaimLogicalFalsePositiveCount = 0;
        uint64_t semanticSceneProducerClaimLogicalFalseNegativeCount = 0;
        uint64_t semanticSceneProducerClaimConsumeDeniedCount = 0;
        // === Phase 7.2: single-caster flicker + reconciliation ===
        uint64_t semanticSceneDirectLastRawRecordCount = 0;
        uint64_t semanticSceneDirectLastEligibleRecordCount = 0;
        uint64_t semanticSceneCompactWorkTableMode = 0;
        uint64_t semanticSceneCompactWorkTableCandidateCount = 0;
        uint64_t semanticSceneCompactWorkTableSealedCount = 0;
        uint64_t semanticSceneCompactWorkTableConsumedCount = 0;
        uint64_t semanticSceneCompactWorkTableFallbackCount = 0;
        uint64_t semanticSceneCompactWorkTableRejectStageCount = 0;
        uint64_t semanticSceneCompactWorkTableRejectFreshnessCount = 0;
        uint64_t semanticSceneCompactWorkTableRejectPolicyCount = 0;
        uint64_t semanticSceneCompactWorkTableRejectFrameCount = 0;
        uint64_t semanticSceneCompactWorkTableRejectIdentityCount = 0;
        uint64_t semanticSceneCompactWorkTableMismatchCount = 0;
        uint64_t semanticSceneDirectLastSubmittedRecordCount = 0;
        uint64_t semanticSceneDirectLastUniqueObjectCount = 0;
        uint64_t semanticSceneDirectLastSubmittedObjectCount = 0;
        uint64_t semanticSceneDirectLastRecordCapPartialObjectCount = 0;
        uint64_t semanticSceneDirectLastScanCapPartialObjectCount = 0;
        uint64_t semanticSceneDirectLastMinGeosetsPerObject = 0;
        uint64_t semanticSceneDirectLastMaxGeosetsPerObject = 0;
        uint64_t semanticSceneDirectLastSubmittedIdentityHash = 0;
        uint64_t semanticSceneDirectIdentityChurnCount = 0;
        uint64_t semanticSceneDirectRecordCapHitCount = 0;
        uint64_t semanticSceneDirectRecordCapTruncatedRecordCount = 0;
        uint64_t semanticSceneDirectScanCapHitCount = 0;
        uint64_t semanticSceneDirectObjectGroupedSubmitCount = 0;
        uint64_t semanticSceneDirectObjectGroupedSkipCount = 0;
        uint64_t semanticSceneDirectRecordCapSkipObjectCount = 0;
        uint64_t semanticSceneDirectRecordCapAppendFailCount = 0;
        uint64_t semanticSceneDirectSelectionLeaseActiveKeyCount = 0;
        uint64_t semanticSceneDirectSelectionLeasePrunedKeyCount = 0;
        uint64_t semanticSceneDirectSelectionLeaseSubmittedKeyCount = 0;
        uint64_t semanticSceneDirectStickyFillBudgetRecordCount = 0;
        uint64_t semanticSceneDirectStickyFillAppendedCount = 0;
        uint64_t semanticSceneDirectStickyFillSubmittedCount = 0;
        uint64_t semanticSceneDirectStickyFillMissedCount = 0;
        uint64_t semanticSceneDirectPartLeaseRestoredCount = 0;
        uint64_t semanticSceneDirectPartLeaseUpdatedCount = 0;
        uint64_t semanticSceneDirectPartLeaseExpiredCount = 0;
        uint64_t semanticSceneDirectPartLeaseRejectedDynamicMeshCount = 0;
        uint64_t semanticSceneDirectPartLeaseRejectedNotSelfContainedCount = 0;
        uint64_t semanticSceneDirectPartLeaseRejectedUnsafeBackingCount = 0;
        uint64_t semanticSceneDirectPartLeaseRejectedSelfRenewCount = 0;
        uint64_t semanticSceneDirectPartLeaseBudgetLimitCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRestoredCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseUpdatedFromLiveCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseExpiredCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRejectedPoseStaleCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRejectedSliceStaleCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRejectedUnsafeBackingCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRejectedNotSelfContainedCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRejectedSelfRenewCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseBudgetLimitCount = 0;
        uint64_t semanticSceneShadowManifestPartLeaseRestoredPoseStaleCoreCount = 0;
        uint64_t semanticSceneShadowManifestPartLeasePoseFreshenedFromCModelCount = 0;
        uint64_t semanticSceneShadowManifestPartLeasePoseCModelRefreshMissCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreCompleteCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreIncompleteSkipCount = 0;
        uint64_t semanticSceneShadowManifestPartOmittedIncompleteCoreCount = 0;
        // Phase 7.25 core epoch planner 专属聚合计数器。
        uint64_t semanticSceneShadowManifestObjectCoreEpochUpdatedFromLiveCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreEpochRestoredCompleteCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreEpochSkippedIncompleteCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreEpochMissingPartCount = 0;
        uint64_t semanticSceneShadowManifestObjectCoreEpochSelfRenewRejectCount = 0;
        uint64_t semanticSceneShadowManifestCorePartPrunedOnLeaseExpiryCount = 0;
        uint64_t semanticSceneShadowManifestCoreObjectEmptiedOnLeaseExpiryCount = 0;
        uint64_t semanticSceneShadowManifestLeaseExpiredBackingOnlyCount = 0;
        uint64_t semanticSceneShadowManifestRetiredAfterAuthoritativeAbsenceCount = 0;
        uint64_t semanticSceneShadowManifestMissingRequiredPartCount = 0;
        uint64_t semanticSceneShadowManifestGraceUsedCount = 0;
        uint64_t semanticSceneShadowManifestTombstoneRetiredCount = 0;
        // Phase 7.28：skinned palette content stability probe。
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceNoneCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceDrawTimeCapturedCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeGlobalSlotCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeBlendedCacheCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimePublishedRegistryCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceSubmitTimeCModelFallbackCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStablePartSampleCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteHashChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSourceChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSlotIndexChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteHashUniqueInWindowMax = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteSlotIndexUniqueInWindowMax = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixSmallDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixMediumDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteCountChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPayload11CMultiValueCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteLeaseKeyPaletteCountMultiValueCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceSampleCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceHashChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceCountChurnCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixSmallDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixMediumDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteStrictSliceFirstMatrixLargeDeltaCount = 0;
        uint64_t semanticSceneDirectPaletteAttributionSnapshotHitCount = 0;
        uint64_t semanticSceneDirectPaletteCaptureTrustedSourceHitCount = 0;
        uint64_t semanticSceneDirectPaletteCaptureTrustedSourceMissCount = 0;
        // Phase 7.30 Step A：stale→live 过渡归因。
        uint64_t semanticSceneSubmittedSkinnedPaletteStaleRestoreSubmittedCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteAfterStaleRestoreLargeDeltaCount = 0;
        uint64_t semanticSceneSubmittedSkinnedPaletteLiveToLiveLargeDeltaCount = 0;
        uint64_t semanticSceneDirectManifestObjectCount = 0;
        uint64_t semanticSceneDirectManifestObservedPartCount = 0;
        uint64_t semanticSceneDirectManifestShadowEligiblePartCount = 0;
        uint64_t semanticSceneDirectObjectCompleteEligibleCount = 0;
        uint64_t semanticSceneDirectObjectIncompleteByScanCapCount = 0;
        uint64_t semanticSceneDirectObjectIncompleteByAlphaPolicyCount = 0;
        uint64_t semanticSceneDirectObjectIncompleteBySliceUnresolvedCount = 0;
        uint64_t semanticSceneDirectObjectIncompleteByPacketBuildFailCount = 0;
        uint64_t semanticSceneDirectObjectIncompleteByAppendFailCount = 0;
        uint64_t semanticSceneDirectSubmittedCompleteObjectCount = 0;
        uint64_t semanticSceneDirectSubmittedPartialObjectCount = 0;
        uint64_t semanticSceneDirectPreparedSliceAuthoritativeCount = 0;
        uint64_t semanticSceneDirectPreparedSliceFallbackLayerIndexCount = 0;
        uint64_t semanticSceneDirectPreparedSliceMissingCount = 0;
        uint64_t semanticScenePreparedProbeAttemptCount = 0;
        uint64_t semanticScenePreparedProbeContextReadyCount = 0;
        uint64_t semanticScenePreparedProbeBackingReadableCount = 0;
        uint64_t semanticScenePreparedSliceRecordedCount = 0;
        uint64_t semanticScenePreparedSliceQueryAttemptCount = 0;
        uint64_t semanticScenePreparedSliceQueryHitCount = 0;
        uint64_t semanticScenePreparedSliceQueryMissCount = 0;
        uint64_t semanticSceneShadowManifestObjectCount = 0;
        uint64_t semanticSceneShadowManifestPartCount = 0;
        uint64_t semanticSceneShadowManifestStableObjectCount = 0;
        uint64_t semanticSceneShadowManifestNewObjectCount = 0;
        uint64_t semanticSceneShadowManifestExpiredObjectCount = 0;
        uint64_t semanticSceneShadowManifestFreshPartCount = 0;
        uint64_t semanticSceneShadowManifestLeaseablePartCount = 0;
        uint64_t semanticSceneShadowManifestPoseStalePartCount = 0;
        uint64_t semanticSceneShadowManifestSliceStalePartCount = 0;
        uint64_t semanticSceneShadowManifestExpiredPartCount = 0;
        uint64_t semanticSceneShadowManifestMultiSlicePartCount = 0;
        uint64_t semanticSceneShadowManifestPayload11CChurnCount = 0;
        uint64_t semanticSceneShadowManifestRenderablePartChurnCount = 0;
        uint64_t semanticSceneShadowManifestCModelPoseHitCount = 0;
        uint64_t semanticSceneShadowManifestCModelPoseMissCount = 0;
        uint64_t semanticSceneShadowManifestCModelPoseNoRuntimeCount = 0;
        uint64_t
            semanticSceneShadowManifestPoseFreshGenerationVerifierMismatchCount =
                0;
        uint64_t semanticSceneShadowManifestCModelPoseLastRuntimeModelPtr = 0;
        uint64_t semanticSceneShadowManifestCModelPoseLastMatrixCount = 0;
        uint64_t semanticSceneShadowManifestCModelPoseLastMatrixHash = 0;
        uint64_t semanticSceneSubmittedObjectJaccardMilli = 0;
        uint64_t semanticSceneSubmittedPartJaccardMilli = 0;
        uint64_t semanticSceneVisibleLookupPartLayerHitCount = 0;
        uint64_t semanticSceneVisibleLookupSingleFallbackCount = 0;
        uint64_t semanticSceneVisibleLookupMissCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingNotCheckedCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingPassCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailNoRenderablePartCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailLookupMissCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailNonMainQueueCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailNonWorldGroupCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailIdentityMismatchCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailSceneNodeMismatchCount = 0;
        uint64_t semanticSceneDirectMainWorldBackingFailMeshDataMismatchCount = 0;
        uint64_t semanticSceneDirectPaletteHashChurnCount = 0;
        uint64_t semanticSceneDirectGroupHashChurnCount = 0;
        uint64_t semanticSceneDirectStableGroupHashChurnCount = 0;
        uint64_t semanticSceneDirectStream1PtrChurnCount = 0;
        uint64_t semanticSceneDirectGeometrySourceHashChurnCount = 0;
        uint64_t semanticSceneDirectSameCasterComparisonCount = 0;
        uint64_t semanticSceneDirectIdentitySkippedChurnCount = 0;
        uint64_t semanticSceneDirectPaletteRootDeltaSampleCount = 0;
        uint64_t semanticSceneDirectPaletteRootHashChangedTinyDeltaCount = 0;
        uint64_t semanticSceneDirectPaletteRootHashChangedSmallDeltaCount = 0;
        uint64_t semanticSceneDirectPaletteRootHashChangedMediumDeltaCount = 0;
        uint64_t semanticSceneDirectPaletteRootHashChangedLargeDeltaCount = 0;
        uint64_t semanticSceneDirectPaletteRootMaxDeltaMilli = 0;
        uint64_t semanticSceneDirectSelectionKeyUnitPtrCount = 0;
        uint64_t semanticSceneDirectSelectionKeyJHandleCount = 0;
        uint64_t semanticSceneDirectSelectionKeyRuntimeModelCount = 0;
        uint64_t semanticSceneDirectSelectionKeyWorldObjectCount = 0;
        uint64_t semanticSceneDirectSelectionKeySceneNodeCount = 0;
        uint64_t semanticSceneDirectSelectionKeyModelMeshCount = 0;
        uint64_t semanticSceneDirectSelectionKeyRenderablePartCount = 0;
        uint64_t semanticSceneLastAppendedGeometrySourceHash = 0;
        uint64_t semanticSceneLastAppendedGeometryId = 0;
        uint64_t semanticSceneShadowCastersCount = 0;
        uint64_t semanticSceneReplayDrawsCount = 0;
        uint64_t semanticSceneShadowMapDrawnCasters = 0;
        uint64_t semanticSceneShadowMapCascadeCulledCount = 0;
        uint64_t semanticSceneTerrainBoundsCullMode = 0;
        uint64_t semanticSceneTerrainBoundsProducerS1AttemptCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerFallbackAttemptCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerExactRangeCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerMissingExactRangeCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerUpSourceAttemptCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerMappedSourceAttemptCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerNoSourceCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanAcceptedCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanRejectedCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanNullBaseRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanNotCpuReadableRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanMissingOwnerRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanMissingGenerationRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanRangeRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerSpanAddressOverflowRejectCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerComputeSuccessCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerComputeFailureCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerValidSphereCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerInvalidSphereCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerPublishedExactCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheLookupCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheHitCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheMissCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheCollisionMissCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheStoreCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerDomainCacheEvictionCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintComparableCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintExactCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintSupersetCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintUnderCoverageCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintInvalidCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintRangeAcceptedCount = 0;
        uint64_t semanticSceneTerrainBoundsProducerHintRangeRejectedCount = 0;
        uint64_t semanticSceneTerrainBoundsCandidateCount = 0;
        uint64_t semanticSceneTerrainBoundsProofAcceptedCount = 0;
        uint64_t semanticSceneTerrainBoundsFailVisibleCount = 0;
        std::array<uint64_t,
            render::kWar3ShadowBoundsCullRejectReasonCount>
            semanticSceneTerrainBoundsRejectReasonHistogram = {};
        uint64_t semanticSceneTerrainBoundsWouldCullCount = 0;
        uint64_t semanticSceneTerrainBoundsAppliedCullCount = 0;
        uint64_t semanticSceneTerrainBoundsC0WouldCullCount = 0;
        uint64_t semanticSceneTerrainBoundsC1WouldCullCount = 0;
        uint64_t semanticSceneTerrainBoundsC2WouldCullCount = 0;
        uint64_t semanticSceneTerrainBoundsC3WouldCullCount = 0;
        uint64_t semanticSceneObjectBoundsCandidateCount = 0;
        uint64_t semanticSceneObjectBoundsProofAcceptedCount = 0;
        uint64_t semanticSceneObjectBoundsFailVisibleCount = 0;
        std::array<uint64_t,
            render::kWar3ShadowBoundsCullRejectReasonCount>
            semanticSceneObjectBoundsRejectReasonHistogram = {};
        uint64_t semanticSceneObjectBoundsWouldCullCount = 0;
        uint64_t semanticSceneObjectBoundsAppliedCullCount = 0;
        uint64_t semanticSceneUnionCullMode = 0;
        uint64_t semanticSceneUnionCullObserveFrameCount = 0;
        uint64_t semanticSceneUnionCullCandidateCount = 0;
        uint64_t semanticSceneUnionCullProofAcceptedCount = 0;
        uint64_t semanticSceneUnionCullFailVisibleCount = 0;
        uint64_t semanticSceneUnionCullDynamicConservativeCount = 0;
        uint64_t semanticSceneUnionCullUnknownOrStaleCount = 0;
        uint64_t semanticSceneUnionCullC2WouldCullCount = 0;
        uint64_t semanticSceneUnionCullC3WouldCullCount = 0;
        uint64_t semanticSceneUnionCullBothFarWouldCullCount = 0;
        uint64_t semanticSceneUnionCullFalseNegativeCount = 0;
        uint64_t semanticSceneUnionCullFalsePositiveCount = 0;
        uint64_t semanticSceneShadowMapSkinnedCasterCount = 0;
        uint64_t semanticSceneShadowMapSkinnedPreparedCount = 0;
        uint64_t semanticSceneShadowMapSkinnedInvalidBufferCount = 0;
        uint64_t semanticSceneShadowMapSkinnedInvalidPipelineCount = 0;
        uint64_t semanticSceneShadowMapSkinnedDrawnCount = 0;
        uint64_t semanticSceneShadowTaaActive = 0;
        uint64_t semanticSceneReceiverReuseShadowMap = 0;
        uint64_t semanticSceneReceiverInputValid = 0;
        uint64_t semanticSceneReceiverInputRejectReason = 0;
        uint64_t semanticSceneReceiverNeedPass = 0;
        uint64_t semanticSceneReceiverNeedShadowMap = 0;
        uint64_t semanticSceneReceiverHasCompleteShadowMap = 0;
        uint64_t semanticSceneReceiverHasUsableDirectionalShadow = 0;
        uint64_t semanticSceneReceiverActiveStrengthMilli = 0;
        uint64_t semanticSceneReceiverUboStrengthMilli = 0;
        uint64_t semanticSceneReceiverDebugMode = 0;
        uint64_t semanticSceneReceiverCsmCascadeCount = 0;
        uint64_t semanticSceneReceiverZeroStrengthFrameCount = 0;
        uint64_t semanticSceneReceiverDrawnWithZeroStrengthCount = 0;
        uint64_t semanticSceneReceiverNoCompleteShadowMapCount = 0;
        uint64_t semanticSceneReceiverNoShadowMapImageCount = 0;
        uint64_t semanticSceneReceiverNoShadowMapSampleViewCount = 0;
        uint64_t semanticSceneReceiverNoCandidateCsmCount = 0;
        uint64_t semanticSceneReceiverCsmFallbackToLastGoodCount = 0;
        uint64_t semanticSceneReceiverHoldInvalidCsmCount = 0;
        uint64_t semanticSceneReceiverHoldEmptyReplayCount = 0;
        uint64_t semanticSceneReceiverHoldIdentityChurnCount = 0;
        uint64_t semanticSceneReceiverReuseInvalidatedAfterEnsureCount = 0;
        uint64_t semanticSceneShadowMapRenderSkippedNoResourcesCount = 0;
        uint64_t semanticSceneShadowMapRenderSkippedNoMatrixBufferCount = 0;
        uint64_t semanticSceneReceiverViewportX = 0;
        uint64_t semanticSceneReceiverViewportY = 0;
        uint64_t semanticSceneReceiverViewportWidth = 0;
        uint64_t semanticSceneReceiverViewportHeight = 0;
        uint64_t semanticSceneSkippedUnitsOnlyFilter = 0;
        uint64_t semanticSceneAcceptedExplicitResourceOwnerRigid = 0;
        uint64_t semanticSceneRejectedNoVertex = 0;
        uint64_t semanticSceneRejectedSkinnedContract = 0;
        uint64_t semanticSceneRejectedGeometry = 0;
        uint64_t semanticSceneRejectedGeometryFrameLocal = 0;
        uint64_t semanticSceneRejectedGeometryPersistent = 0;
        uint64_t persistentRejectNoIdentity = 0;
        uint64_t persistentRejectUnsupportedMode = 0;
        uint64_t persistentRejectDynamicSource = 0;
        uint64_t persistentRejectAlphaBlend = 0;
        uint64_t persistentRejectMissingStorage = 0;
        uint64_t persistentRejectCreateOrBudget = 0;
        // Detailed refinement of persistentRejectCreateOrBudget. These are
        // additive event counters over the report recording window.
        uint64_t persistentRejectCapacity = 0;
        uint64_t persistentRejectPositionBufferCreate = 0;
        uint64_t persistentRejectIndexBufferCreate = 0;
        uint64_t persistentRejectBlendBufferCreate = 0;
        uint64_t persistentRejectUvBufferCreate = 0;
        uint64_t persistentRejectRegistryInsert = 0;
        uint64_t persistentRejectOther = 0;
        // Persistent pool observation. *Last/*Max are gauges. NeededTotal is
        // additive requested byte traffic; UsedGaugeSum exists only to derive
        // an average of end-of-Present gauge samples.
        uint64_t persistentDiagnosticsFramesObserved = 0;
        uint64_t persistentCreateAttempts = 0;
        uint64_t persistentPoolBytesCapLast = 0;
        uint64_t persistentPoolBytesUsedLast = 0;
        uint64_t persistentPoolBytesUsedMax = 0;
        uint64_t persistentPoolBytesUsedGaugeSum = 0;
        uint64_t persistentPoolBytesNeededLast = 0;
        uint64_t persistentPoolBytesNeededMax = 0;
        uint64_t persistentPoolBytesNeededTotal = 0;
        uint64_t persistentPoolBytesEvictedLast = 0;
        uint64_t persistentPoolBytesEvictedDelta = 0;
        uint64_t persistentPoolLiveGeometryCountLast = 0;
        uint64_t persistentPoolLiveGeometryCountMax = 0;
        uint64_t persistentForceGcRequests = 0;
        uint64_t persistentForceGcNoBytesFreed = 0;
        uint64_t persistentForceGcStillInsufficient = 0;
        uint64_t persistentForceGcBytesFreed = 0;
        uint64_t persistentCapacityRejectAllCallers = 0;
        uint64_t persistentCapacityFastReject = 0;
        uint64_t persistentS1EarlyEntryCountLast = 0;
        uint64_t persistentS1EarlyEntryCountMax = 0;
        uint64_t persistentS1EarlyPersistentBackedCountLast = 0;
        uint64_t persistentS1EarlyPersistentBackedCountMax = 0;
        uint64_t persistentS1EarlyPersistentReverseIndexCountLast = 0;
        uint64_t persistentS1EarlyPersistentReverseIndexCountMax = 0;
        uint64_t persistentS1EarlyPersistentReverseIndexMismatchFrames = 0;
        uint64_t persistentS1EarlyFallbackBackedCountLast = 0;
        uint64_t persistentS1EarlyFallbackBackedCountMax = 0;
        uint64_t persistentS1EarlyLogicalReferencedBytesLast = 0;
        uint64_t persistentS1EarlyLogicalReferencedBytesMax = 0;
        uint64_t persistentS1EarlyAcceptedHitCount = 0;
        uint64_t persistentS1EarlyReplayPublishedCount = 0;
        uint64_t persistentS1EarlyReplayInstanceCount = 0;
        uint64_t persistentS1EarlyReplayFallbackCount = 0;
        uint64_t persistentS1EarlySourceMismatchEvictCount = 0;
        uint64_t persistentS1EarlyReplayClosureMismatchFrames = 0;
        uint64_t persistentS1GenerationProofEntryCountLast = 0;
        uint64_t persistentS1GenerationProofEntryCountMax = 0;
        uint64_t persistentS1GenerationProofEligibleCount = 0;
        uint64_t persistentS1GenerationProofFirstCount = 0;
        uint64_t persistentS1GenerationProofSameFrameCount = 0;
        uint64_t persistentS1GenerationProofAdvancedCount = 0;
        uint64_t persistentS1GenerationProofChangedCount = 0;
        uint64_t persistentS1GenerationProofStaleRestartCount = 0;
        uint64_t persistentS1GenerationProofPromotionReadyCount = 0;
        uint64_t persistentS1GenerationProofCapacityRejectCount = 0;
        uint64_t semanticFallbackPruned = 0;
        uint64_t semanticFallbackPrunedByHandle = 0;
        uint64_t semanticFallbackPrunedByWorldObjectEntry = 0;
        uint64_t semanticFallbackPrunedBySceneNode = 0;
        uint64_t semanticFallbackPrunedByRuntimeModel = 0;
        uint64_t instancedGeometryGroups = 0;
        uint64_t instancedGeometryInstances = 0;
        uint64_t instancedGeometryDrawsSaved = 0;
        uint64_t reusedFreezeBytes = 0;
        uint64_t actualFreezeReuseBytes = 0;
        uint64_t uniqueFreezeAcceptedBytes = 0;
        uint64_t duplicateFreezeBypassBytes = 0;
        uint64_t potentialFreezeReuseBytes = 0;
        std::array<PhaseAggregate, 4> phases = {{
            {"POS"}, {"BLEND"}, {"UV"}, {"INDEX"}
        }};
    };

    struct ExportSnapshot {
        struct IdentitySameFrameDedupStats {
            uint64_t attempts = 0;
            uint64_t hits = 0;
            uint64_t missMissingAlias = 0;
            uint64_t missCrossFrame = 0;
            uint64_t missNoBatchProof = 0;
            uint64_t missIncomplete = 0;
            uint64_t missInputMismatch = 0;
            uint64_t missAliasConflict = 0;
            uint64_t missRuntimeOwner = 0;
            uint64_t batchMarked = 0;
        };

        std::deque<FrameSnapshot> frameHistory;
        std::vector<SectionStats> sections;
        ShadowBudgetAggregate shadowBudgetAggregate;
        uint32_t processId = 0;
        DWORD mainThreadId = 0;
        uint64_t processStartFileTime100ns = 0;
        bool gpuSkinHooksEnabled = false;
        gpu_skin::GpuSkinRuntimeConfig gpuSkinRuntimeConfig;
        gpu_skin::NativeBridgeFingerprint gpuSkinNativeFingerprint;
        gpu_skin::NativeBridgeCounters gpuSkinNativeCounters;
        resource_census::ResourceResidencySnapshot resourceResidency;
        render::WorldObjectsPhase1TelemetrySummary
            worldObjectsMaintenanceTiming;
        IdentitySameFrameDedupStats modelIdentitySameFrameDedup;
        IdentitySameFrameDedupStats shadowIdentitySameFrameDedup;
        ProfilerCounters profilerCounters;
        bool detailTraceEnabled = false;
        uint32_t detailTraceSamplePeriod = 0;
        // 报告元数据：构建/配置身份与帧序号对齐（schema v9）。
        struct ReportMeta {
            std::string schemaVersion = "9";
            std::string dllSha256Hex;
            uint64_t dllFileSize = 0;
            std::string buildTimestamp;
            std::string runtimeProfile;
            std::string enabledModulesCsv;
            std::string disabledModulesCsv;
            std::string perfEnvJson;
            uint64_t perfFrameEpoch = 0;
            uint64_t businessFrameSerial = 0;
            bool monitorDisabledByEnv = false;
        };
        ReportMeta meta;
    };

    struct ExportJob {
        std::string finalPath;
        double windowSec = 0.0;
        bool isAuto = false;
        ExportSnapshot snapshot;
    };

    uint32_t getSectionId(const char* name, const char* parentPath = nullptr);
    uint32_t getSectionIdLocked(const char* name, const char* parentPath);
    uint32_t resolveSectionIdWithTlsCache(ThreadCpuState& state,
                                          const char* name,
                                          const char* parentPath);
    void queueCpuDelta(ThreadCpuState& state,
                       uint32_t id,
                       double cpuMs,
                       uint32_t calls);
    void flushThreadCpuDeltas(ThreadCpuState& state);
    void flushCurrentThreadCpuDeltas();
    static ThreadCpuState& threadCpuState();
    bool shouldSampleDetailScope(ThreadCpuState& state) const;
    static bool detailTraceEnabled();
    static uint32_t detailTraceSamplePeriod();
    Rc<DxvkGpuQuery> writeTimestamp(const Rc<DxvkCommandList>& ctx);
    void submitSample(uint32_t id,
                      uint64_t frameEpoch,
                      double cpuMs,
                      Rc<DxvkGpuQuery> gpuBegin,
                      Rc<DxvkGpuQuery> gpuEnd);
    void tick();
    void archiveFrame();
    void report(std::chrono::steady_clock::time_point now);
    bool readProcessCpu100ns(uint64_t& out100ns) const;
    bool readThreadCpu100ns(HANDLE threadHandle, uint64_t& out100ns) const;
    HANDLE ensureMainThreadHandleLocked(DWORD tid);
    void closeMainThreadHandleLocked();
    CpuProbeSnapshot captureCpuProbeLocked(bool frameEnd);
    ExportSnapshot captureExportSnapshotLocked() const;
    std::string resolveExportPath(const std::string& outputPath) const;
    double resolveReportWindowSec() const;
    void ensureExportWorkerLocked();
    void enqueueExportJob(ExportJob&& job);
    void exportWorkerLoop();
    void processExportJob(const ExportJob& job);
    std::string generateJsonDataFromSnapshot(const ExportSnapshot& snapshot,
                                             double windowSec) const;

    DxvkDevice* m_device = nullptr;
    // 非 owning；只允许在 m_mutex 下登记、清理或读取。
    D3D9MemoryAllocator* m_resourceCensusAllocator = nullptr;
    double m_timestampPeriodNs = 0.0;
    std::atomic<bool> m_enabled { true };
    std::atomic<bool> m_recording { false };
    std::atomic<bool> m_shutdownStarted { false };

    std::mutex m_mutex;
    std::unordered_map<std::string, uint32_t> m_sectionIds;
    std::vector<SectionStats> m_sections;
    std::vector<PendingSample> m_pending;
    // 键=(threadId<<32)|sectionId；只在 TLS 批量 flush 时触及，不能放入
    // 默认 per-draw 快路径。
    std::unordered_map<uint64_t, SectionStats> m_threadSections;
    ProfilerCounters m_profilerCounters = {};

    // 帧历史
    std::deque<FrameSnapshot> m_frameHistory;
    size_t m_maxHistorySize = 3600; // 约 60 秒 @ 60fps
    size_t m_maxPendingSamples = 8192; // 防止 GPU 计时样本堆积导致内存增长

    // 当前帧数据
    uint64_t m_frameIndex = 0;
    uint64_t m_frameEpochSerial = 0;
    std::chrono::steady_clock::time_point m_frameStart;
    CpuProbeSnapshot m_frameCpuProbeStart;
    FrameWorkloadSnapshot m_currentFrameWorkload = {};
    std::vector<std::array<uint64_t, 2>> m_currentGpuTimestampIntervals;
    bool m_inFrame = false;
    std::atomic<uint64_t> m_activeFrameEpoch { 0 };
    HANDLE m_mainThreadHandle = nullptr;
    DWORD m_mainThreadHandleTid = 0;

    static std::vector<CpuOnlyScope>& scopeStack();

    std::chrono::steady_clock::time_point m_lastReport;
    std::chrono::seconds m_reportInterval = std::chrono::seconds(10);
    std::chrono::steady_clock::time_point m_lastAutoExport;
    std::chrono::seconds m_autoExportInterval = std::chrono::seconds(0);

    std::mutex m_exportMutex;
    std::condition_variable m_exportCv;
    std::deque<ExportJob> m_exportQueue;
    std::thread m_exportThread;
    bool m_exportWorkerStarted = false;
    bool m_exportStopRequested = false;
    bool m_exportAbortRequested = false;
    size_t m_exportInFlight = 0;
    std::atomic<uint32_t> m_pendingExportJobs { 0 };
    std::atomic<uint64_t> m_lastBusinessFrameSerial { 0 };
    ShadowBudgetAggregate m_shadowBudgetAggregate = {};

    friend class ScopedSection;
    friend class FrameGuard;
};

} // namespace dxvk::war3
