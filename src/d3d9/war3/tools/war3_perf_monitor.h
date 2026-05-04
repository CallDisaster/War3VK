// war3_perf_monitor.h - War3 性能监控器（增强版）
// 支持分层计时、帧历史记录、HTML 报告导出

#pragma once

#include "d3d9_war3_debug.h"
#include "../../d3d9_war3_scene.h"

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
    double cpuMs = 0.0;
    double gpuMs = 0.0;
    uint32_t callCount = 0;
};

/**
 * @brief 单帧的性能快照
 */
struct FrameSnapshot {
    uint64_t frameIndex = 0;
    double totalCpuMs = 0.0;
    double totalGpuMs = 0.0;
    double processCpuMs = 0.0;
    double mainThreadCpuMs = 0.0;
    double workerThreadsCpuMs = 0.0;
    bool hasProcessCpu = false;
    bool hasMainThreadCpu = false;
    std::chrono::steady_clock::time_point timestamp;
    std::vector<SectionTiming> sections;
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
        ScopedCpuScope(War3PerfMonitor* monitor, const char* name);
        ~ScopedCpuScope();

        ScopedCpuScope(const ScopedCpuScope&) = delete;
        ScopedCpuScope& operator=(const ScopedCpuScope&) = delete;
        ScopedCpuScope(ScopedCpuScope&& other) noexcept;
        ScopedCpuScope& operator=(ScopedCpuScope&& other) noexcept;

    private:
        War3PerfMonitor* m_monitor = nullptr;
    };

    static War3PerfMonitor& instance();

    void setDevice(DxvkDevice* device);
    void shutdown();

    // 帧管理
    void beginFrame();
    void endFrame();
    FrameGuard makeFrameGuard();

    // 分段计时
    ScopedSection scope(const char* name, const Rc<DxvkCommandList>& ctx);
    ScopedCpuScope cpuScope(const char* name);
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
    
    // 分层 Scope（用于无 CommandList 的纯 CPU 计时）
    void pushScope(const char* name);
    void popScope();

    // 开关
    void setEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    void setRecording(bool recording) { m_recording.store(recording, std::memory_order_relaxed); }
    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    bool isRecording() const { return m_recording.load(std::memory_order_relaxed); }
    void resetHistory();
    bool waitForPendingExports(uint32_t timeoutMs = 15000);
    uint32_t getPendingExportCount() const;
    void noteShadowBudgetFrame(const War3ShadowCaptureStats& stats);
    void noteShadowMapFallback(bool reusedLastComplete, bool renderedCurrentPartial);
    void noteShadowReceiverFrame(uint32_t replayCasterCount,
                                 uint64_t replayGeometryWork,
                                 uint32_t requestedShadowResolution,
                                 uint32_t effectiveShadowResolution);

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
    };

    struct PendingSample {
        uint32_t id = 0;
        double cpuMs = 0.0;
        Rc<DxvkGpuQuery> gpuBegin;
        Rc<DxvkGpuQuery> gpuEnd;
    };

    struct CpuOnlyScope {
        std::string name;
        std::string path;
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
        double cpuMs = 0.0;
        uint32_t calls = 0;
    };

    struct SectionIdCacheEntry {
        const char* name = nullptr;
        const char* parentPath = nullptr;
        size_t nameHash = 0;
        size_t parentHash = 0;
        uint32_t id = 0;
    };

    struct ThreadCpuState {
        std::array<SectionIdCacheEntry, 64> sectionIdCache = {};
        uint32_t nextCacheSlot = 0;
        std::vector<CpuDelta> pendingDeltas;
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
        uint64_t semanticBridgeHit = 0;
        uint64_t semanticBridgeMiss = 0;
        uint64_t semanticBridgeBypassed = 0;
        uint64_t semanticSceneSubmitted = 0;
        uint64_t semanticSceneSubmittedUnit = 0;
        uint64_t semanticSceneSubmittedSkinned = 0;
        uint64_t semanticSceneLivePaletteRefreshAttemptCount = 0;
        uint64_t semanticSceneLivePaletteRefreshHitCount = 0;
        uint64_t semanticSceneLivePaletteRefreshMissCount = 0;
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
        uint64_t semanticSceneSkinnedFullIndexFallbackCount = 0;
        uint64_t semanticSceneSkinnedMissingVisibleIndexSliceRejectCount = 0;
        uint64_t semanticSceneSubmittedFrameLocal = 0;
        uint64_t semanticSceneSubmittedPersistent = 0;
        uint64_t semanticSceneSkippedUnitsOnlyFilter = 0;
        uint64_t semanticSceneAcceptedExplicitResourceOwnerRigid = 0;
        uint64_t semanticSceneRejectedNoVertex = 0;
        uint64_t semanticSceneRejectedSkinnedContract = 0;
        uint64_t semanticSceneRejectedGeometry = 0;
        uint64_t semanticSceneRejectedGeometryFrameLocal = 0;
        uint64_t semanticSceneRejectedGeometryPersistent = 0;
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
        std::deque<FrameSnapshot> frameHistory;
        std::vector<SectionStats> sections;
        ShadowBudgetAggregate shadowBudgetAggregate;
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
    Rc<DxvkGpuQuery> writeTimestamp(const Rc<DxvkCommandList>& ctx);
    void submitSample(uint32_t id,
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
    CpuProbeSnapshot captureCpuProbeLocked();
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
    double m_timestampPeriodNs = 0.0;
    std::atomic<bool> m_enabled { true };
    std::atomic<bool> m_recording { false };
    std::atomic<bool> m_shutdownStarted { false };

    std::mutex m_mutex;
    std::unordered_map<std::string, uint32_t> m_sectionIds;
    std::vector<SectionStats> m_sections;
    std::vector<PendingSample> m_pending;

    // 帧历史
    std::deque<FrameSnapshot> m_frameHistory;
    size_t m_maxHistorySize = 3600; // 约 60 秒 @ 60fps
    size_t m_maxPendingSamples = 8192; // 防止 GPU 计时样本堆积导致内存增长

    // 当前帧数据
    uint64_t m_frameIndex = 0;
    std::chrono::steady_clock::time_point m_frameStart;
    CpuProbeSnapshot m_frameCpuProbeStart;
    bool m_inFrame = false;
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
    ShadowBudgetAggregate m_shadowBudgetAggregate = {};

    friend class ScopedSection;
    friend class FrameGuard;
};

} // namespace dxvk::war3
