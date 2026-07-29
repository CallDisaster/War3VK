#pragma once

#include "war3_gpu_skin_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace dxvk::war3::gpu_skin {

/**
 * \brief 多线程 CPU 蒙皮拒绝原因
 *
 * 这些原因只描述 producer 本身的保守拒绝，不授权 native kernel bypass。
 */
enum class CpuSkinMtRejectReason : uint8_t {
  None = 0,
  WrongThread,
  Stopping,
  InvalidEpoch,
  InvalidStaticSnapshot,
  InvalidPalette,
  InvalidOutputFormat,
  InvalidJob,
  DuplicateToken,
  JobOutsideRoute,
  BatchTooSmall,
  QueueBackpressure,
  MemoryBackpressure,
  FloatingPointEnvironmentMismatch,
  Cancelled,
  KernelFailure,
};

/**
 * \brief 单个异步 job 的终态
 */
enum class CpuSkinMtJobState : uint8_t {
  Invalid = 0,
  Queued,
  Running,
  Ready,
  CpuFallback,
  Cancelled,
  Failed,
};

/**
 * \brief 异步 batch 的聚合状态
 */
enum class CpuSkinMtBatchState : uint8_t {
  Invalid = 0,
  Queued,
  Running,
  Ready,
  Partial,
  Cancelled,
  Failed,
};

/**
 * \brief 同步 fork/join 的返回状态
 */
enum class CpuSkinMtSynchronousResult : uint8_t {
  Completed = 0,
  Rejected,
  Cancelled,
  Failed,
};

/**
 * \brief 只读、带 stride 的输入字节流
 *
 * 该视图只在创建 immutable static snapshot 时使用。创建成功后 worker
 * 不再保存或访问这里的裸指针。
 */
struct CpuSkinMtStreamView {
  const void* data = nullptr;
  size_t byteSize = 0;
  uint32_t stride = 0;
};

/**
 * \brief immutable static snapshot 创建参数
 *
 * positions/normals/groupSlots/UV 属于模型静态数据，只能在资源创建或
 * generation 变化时复制一次；禁止每帧重新复制这些大流。diffuse 为空时，
 * odd FVF format 使用原生普通 geoset 的 0xffffffff 默认值。
 */
struct CpuSkinMtStaticSnapshotDesc {
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t contentHash = 0;
  uintptr_t geosetData = 0;
  uint32_t layoutGeneration = 0;
  uint32_t vertexCount = 0;
  uint32_t matrixGroupCount = 0;
  uint32_t uvLayerCount = 0;
  CpuSkinMtStreamView positions;
  CpuSkinMtStreamView normals;
  CpuSkinMtStreamView groupSlots;
  CpuSkinMtStreamView texcoord0;
  CpuSkinMtStreamView texcoord1;
  CpuSkinMtStreamView diffuse;
};

/**
 * \brief 可跨 worker 固定的静态蒙皮输入
 *
 * 对象拥有紧密打包后的 position/normal/group/UV 字节。资源 owner 必须把
 * shared_ptr 与 map/device/layout generation 一起缓存；worker 只读取本对象，
 * 不访问 Game.dll、D3D9 或 Vulkan 对象。
 */
class CpuSkinMtStaticSnapshot {
public:
  /**
   * \brief 创建并一次性复制静态输入
   *
   * \param desc 已由 render/resource owner 证明可读的源视图
   * \param reject 可选的精确拒绝原因
   * \returns 成功时返回 immutable snapshot，失败时返回空
   */
  static std::shared_ptr<const CpuSkinMtStaticSnapshot> Create(
      const CpuSkinMtStaticSnapshotDesc& desc,
      CpuSkinMtRejectReason* reject = nullptr) noexcept;

  /** \brief 析构静态快照 */
  ~CpuSkinMtStaticSnapshot();

  CpuSkinMtStaticSnapshot(const CpuSkinMtStaticSnapshot&) = delete;
  CpuSkinMtStaticSnapshot& operator=(const CpuSkinMtStaticSnapshot&) = delete;

  /** \brief 返回 map generation */
  uint64_t mapEpoch() const noexcept;

  /** \brief 返回 device generation */
  uint64_t deviceEpoch() const noexcept;

  /** \brief 返回静态内容 hash */
  uint64_t contentHash() const noexcept;

  /** \brief 返回诊断用 geoset identity；worker 不解引用它 */
  uintptr_t geosetData() const noexcept;

  /** \brief 返回 packing/layout generation */
  uint32_t layoutGeneration() const noexcept;

  /** \brief 返回顶点数 */
  uint32_t vertexCount() const noexcept;

  /** \brief 返回 matrix group 数 */
  uint32_t matrixGroupCount() const noexcept;

  /** \brief 返回静态 UV 层数 */
  uint32_t uvLayerCount() const noexcept;

  /** \brief 返回本对象实际拥有的静态字节数 */
  size_t ownedByteSize() const noexcept;

private:
  class Impl;

  explicit CpuSkinMtStaticSnapshot(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> m_impl;

  friend class CpuSkinMtFrozenKernel;
};

/**
 * \brief 一条已经冻结 palette 的纯 CPU kernel
 *
 * 创建发生在 render thread 的安全窗口内，仅复制最多 256*48 bytes 的
 * palette 和小 descriptor。静态大流通过 shared immutable snapshot 固定。
 */
class CpuSkinMtFrozenKernel {
public:
  /**
   * \brief 创建一条可供 persistent worker 使用的冻结 kernel
   *
   * \param staticSnapshot generation-pinned 静态源
   * \param paletteData 当前帧 3x4 palette 字节
   * \param paletteByteSize palette 可读字节数
   * \param outputFormat 原生 FVF format 0..5
   * \param reject 可选的精确拒绝原因
   */
  static std::shared_ptr<const CpuSkinMtFrozenKernel> Create(
      std::shared_ptr<const CpuSkinMtStaticSnapshot> staticSnapshot,
      const void* paletteData,
      size_t paletteByteSize,
      uint32_t outputFormat,
      CpuSkinMtRejectReason* reject = nullptr) noexcept;

  /** \brief 析构冻结 kernel */
  ~CpuSkinMtFrozenKernel();

  CpuSkinMtFrozenKernel(const CpuSkinMtFrozenKernel&) = delete;
  CpuSkinMtFrozenKernel& operator=(const CpuSkinMtFrozenKernel&) = delete;

  /** \brief 返回静态 snapshot */
  const std::shared_ptr<const CpuSkinMtStaticSnapshot>&
  staticSnapshot() const noexcept;

  /** \brief 返回顶点数 */
  uint32_t vertexCount() const noexcept;

  /** \brief 返回 output format */
  uint32_t outputFormat() const noexcept;

  /** \brief 返回精确原生 FVF stride */
  uint32_t outputStride() const noexcept;

  /** \brief 返回完整输出字节数 */
  uint32_t outputByteSize() const noexcept;

  /**
   * \brief 返回冻结的 MXCSR 控制位
   *
   * 只包含 DAZ、异常 mask、rounding 与 FTZ；sticky exception status 不属于
   * 蒙皮输入，也不能从 render thread 传播到 persistent worker。
   */
  uint32_t floatingPointControl() const noexcept;

  /** \brief 返回本对象拥有的小 palette 字节数 */
  size_t ownedPaletteByteSize() const noexcept;

  /**
   * \brief 在调用线程执行一段纯蒙皮
   *
   * 该入口不排队、不访问图形 API，供离线 parity 与 producer-owned staging
   * 计算复用。不同调用可并行写不重叠范围，但本 helper 不提供 Win32 SEH；
   * persistent worker 禁止把 caller/native mapped VB 直接作为 outputBase。
   */
  bool runRange(uint32_t firstVertex,
                uint32_t vertexCount,
                void* outputBase,
                size_t outputByteSize) const noexcept;

private:
  class Impl;

  bool runRangeWithInstalledFloatingPointControl(
      uint32_t firstVertex,
      uint32_t vertexCount,
      void* outputBase,
      size_t outputByteSize,
      uint32_t* cachedGroupSlot,
      float* cachedMatrix) const noexcept;

  explicit CpuSkinMtFrozenKernel(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> m_impl;

  friend class War3CpuSkinMtProducer;
};

/**
 * \brief persistent CPU producer 配置
 *
 * 默认 async route 只表达 193..448 的实验区间，但必须同时满足整批最小顶点
 * 和有界队列门。pure kernel 可生成 0..5 六种 FVF；producer 的 async/sync
 * production route 固定只接受已获接管白名单的 0/2/4。任何阈值都不是生产授权；
 * 正式值必须由 Test Conductor 的 crossover 数据决定。
 */
struct CpuSkinMtConfig {
  uint32_t workerCount = 0;
  uint32_t maxWorkerCount = 4;
  uint32_t maxQueuedTasks = 64;
  uint32_t maxPendingBatches = 4;
  uint32_t maxJobsPerBatch = 512;
  uint32_t targetVerticesPerTask = 2048;
  uint32_t cancelCheckPeriodVertices = 64;
  uint32_t minAsyncVerticesPerJob = 193;
  uint32_t maxAsyncVerticesPerJob = 448;
  uint32_t minAsyncVerticesPerBatch = 4096;
  uint32_t synchronousChunkVertices = 1024;
  uint32_t minSynchronousVertices = 4096;
  size_t maxOwnedBytes = 32u << 20;
  size_t maxPinnedStaticBytesPerBatch = 64u << 20;
  size_t maxOutputBytesPerBatch = 8u << 20;
};

/**
 * \brief 异步 batch 公共 epoch
 */
struct CpuSkinMtBatchDesc {
  uint64_t batchId = 0;
  uint64_t mapEpoch = 0;
  uint64_t deviceEpoch = 0;
  uint64_t frameTag = 0;
  uint64_t flushEpoch = 0;
};

/**
 * \brief 异步 job descriptor
 *
 * kernel 已拥有 palette/static 输入；其余字段映射到现有 OutputLeaseDesc，
 * 但只有 render thread 完成 GPU upload 和现有 manager consumer plan 后才能授权。
 */
struct CpuSkinMtJobDesc {
  std::shared_ptr<const CpuSkinMtFrozenKernel> kernel;
  uint64_t dispatchEpoch = 0;
  uint64_t uploadEpoch = 0;
  uint32_t token = 0;
  uint32_t dipOrdinal = 0;
  uint32_t consumerBits = 0;
};

class CpuSkinMtBatchStateData;
class CpuSkinMtSynchronousOutputState;

/**
 * \brief 异步 batch 的拥有型句柄
 */
class CpuSkinMtBatchHandle {
public:
  /** \brief 构造空句柄 */
  CpuSkinMtBatchHandle() = default;

  /** \brief 判断句柄是否引用 batch */
  explicit operator bool() const noexcept;

  /** \brief 返回 batch id */
  uint64_t batchId() const noexcept;

  /** \brief 返回 producer generation */
  uint64_t generation() const noexcept;

  /** \brief 返回当前聚合状态 */
  CpuSkinMtBatchState state() const noexcept;

  /** \brief 释放调用者对 batch 的所有权 */
  void reset() noexcept;

private:
  explicit CpuSkinMtBatchHandle(
      std::shared_ptr<CpuSkinMtBatchStateData> state) noexcept;

  std::shared_ptr<CpuSkinMtBatchStateData> m_state;

  friend class War3CpuSkinMtProducer;
};

/**
 * \brief 已完成 CPU staging slice 的拥有型 lease
 *
 * 该 lease 只证明 CPU bytes ready，不是 GPU OutputLease。render thread 必须批量
 * 上传到现有 output arena，再用 makeOutputLeaseDesc 生成 descriptor 并走原 consumer
 * ledger/retirement 合同。
 */
class CpuSkinMtOutputLease {
public:
  /** \brief 构造空 lease */
  CpuSkinMtOutputLease() = default;

  /** \brief 判断 lease 在当前 generation 下是否仍然有效 */
  explicit operator bool() const noexcept;

  /** \brief 返回只读 CPU staging 首地址；失效时返回空 */
  const uint8_t* data() const noexcept;

  /** \brief 返回 slice 字节数 */
  uint32_t byteSize() const noexcept;

  /** \brief 返回 job token */
  uint32_t token() const noexcept;

  /** \brief 返回 output format */
  uint32_t outputFormat() const noexcept;

  /**
   * \brief 生成现有 GPU output allocator 使用的 descriptor
   *
   * \param gpuByteOffset render thread 实际分配的 GPU output offset
   * \returns generation 失效时返回全零 descriptor
   */
  OutputLeaseDesc makeOutputLeaseDesc(
      uint32_t gpuByteOffset) const noexcept;

  /** \brief 释放 lease */
  void reset() noexcept;

private:
  CpuSkinMtOutputLease(
      std::shared_ptr<const CpuSkinMtBatchStateData> state,
      uint32_t jobIndex) noexcept;

  std::shared_ptr<const CpuSkinMtBatchStateData> m_state;
  uint32_t m_jobIndex = 0;

  friend class War3CpuSkinMtProducer;
};

/**
 * \brief 同步 fork/join 完成后的 producer-owned staging lease
 *
 * producer 核心永不触碰 caller/Game.dll mapped output。接入层只能在本
 * lease 有效时，由创建 producer 的 owner thread 在原生 caller-owned
 * SEH transaction 中复制 data()/byteSize()，并由该 transaction 显式
 * 结算 normal/exception/reset。lease 只证明 CPU bytes ready，不授权
 * native kernel bypass、GPU consumer 或资源退休。
 */
class CpuSkinMtSynchronousOutput {
public:
  /** \brief 构造空 lease */
  CpuSkinMtSynchronousOutput() = default;

  /** \brief 判断 staging 在当前 generation/MXCSR 下是否仍有效 */
  explicit operator bool() const noexcept;

  /** \brief 返回只读 staging 首地址；失效时返回空 */
  const uint8_t* data() const noexcept;

  /** \brief 返回 staging 字节数；失效时返回 0 */
  uint32_t byteSize() const noexcept;

  /** \brief 返回创建 staging 的 producer generation */
  uint64_t generation() const noexcept;

  /** \brief 返回冻结的 MXCSR 控制位 */
  uint32_t floatingPointControl() const noexcept;

  /** \brief 释放 lease 及其 tracked owned-byte reservation */
  void reset() noexcept;

private:
  explicit CpuSkinMtSynchronousOutput(
      std::shared_ptr<const CpuSkinMtSynchronousOutputState> state) noexcept;

  std::shared_ptr<const CpuSkinMtSynchronousOutputState> m_state;

  friend class War3CpuSkinMtProducer;
};

/**
 * \brief producer 的无锁快照诊断
 */
struct CpuSkinMtDiagnostics {
  uint64_t generation = 0;
  uint64_t submittedBatches = 0;
  uint64_t rejectedBatches = 0;
  uint64_t submittedJobs = 0;
  uint64_t readyJobs = 0;
  uint64_t cpuFallbackJobs = 0;
  uint64_t cancelledJobs = 0;
  uint64_t failedJobs = 0;
  uint64_t workerTasks = 0;
  uint64_t ownerAssistTasks = 0;
  uint64_t synchronousCalls = 0;
  uint64_t synchronousCompleted = 0;
  uint64_t queuedTaskHighWater = 0;
  uint64_t paletteBytesPinned = 0;
  uint64_t staticBytesPinned = 0;
  uint64_t outputBytesOwned = 0;
  uint64_t verticesCompleted = 0;
  uint64_t queueBackpressure = 0;
  uint64_t memoryBackpressure = 0;
  uint64_t staleLeaseRejects = 0;
  uint64_t floatingPointEnvironmentRejects = 0;
  uint64_t synchronousReadyBytes = 0;
  uint64_t resets = 0;
  uint64_t joinedWorkers = 0;
  uint32_t workerCount = 0;
  uint32_t pendingTasks = 0;
};

/**
 * \brief DXVK-owned persistent 多线程 CPU 蒙皮 producer
 *
 * 该类必须由 D3D9 owner 显式持有和析构，禁止 process-static singleton。
 * worker 永不 detach；shutdown 会 cancel、唤醒并 join 全部线程。
 */
class War3CpuSkinMtProducer {
public:
  /**
   * \brief 在 render thread 上创建 producer
   */
  explicit War3CpuSkinMtProducer(const CpuSkinMtConfig& config = {});

  /**
   * \brief cancel 并 join 全部 persistent worker
   *
   * 析构必须发生在创建 producer 的 owner thread；违反时 fail-hard，
   * 不能在对象 lifetime 已结束后留下仍引用 this 的 worker。
   */
  ~War3CpuSkinMtProducer();

  War3CpuSkinMtProducer(const War3CpuSkinMtProducer&) = delete;
  War3CpuSkinMtProducer& operator=(const War3CpuSkinMtProducer&) = delete;

  /**
   * \brief 提交一个粗粒度多 job batch
   *
   * 此函数只接受创建 producer 的 owner thread。成功后不等待 worker；native
   * preflight 应使用 tryAcquireOutput，未 ready 时立即走原 CPU fallback。
   */
  CpuSkinMtRejectReason submit(
      const CpuSkinMtBatchDesc& batch,
      const CpuSkinMtJobDesc* jobs,
      size_t jobCount,
      CpuSkinMtBatchHandle& output) noexcept;

  /**
   * \brief 非阻塞取得单 job 的 CPU staging lease
   */
  bool tryAcquireOutput(const CpuSkinMtBatchHandle& batch,
                        uint32_t token,
                        CpuSkinMtOutputLease& output) noexcept;

  /**
   * \brief 将尚未发布的单 job 标成原生 CPU fallback
   *
   * running worker 会在有界顶点周期内观察 cancel；即使已经写了部分 staging，
   * 状态也不会再晋级 Ready。
   */
  bool cancelJob(const CpuSkinMtBatchHandle& batch,
                 uint32_t token) noexcept;

  /** \brief cancel 整个 batch；不等待 worker */
  void cancelBatch(const CpuSkinMtBatchHandle& batch) noexcept;

  /**
   * \brief owner thread 主动执行至多 maxTasks 个本 batch 粗任务
   *
   * 这是可控的 main-thread participation，不会等待其他线程，也不会调用图形 API。
   */
  uint32_t assist(const CpuSkinMtBatchHandle& batch,
                  uint32_t maxTasks) noexcept;

  /**
   * \brief 执行同步 staged fork/join 并返回 owned output
   *
   * worker 与 owner assist 只写 producer-owned staging 的不重叠范围。所有任务
   * 已结算且数值环境仍 exact 后，本函数发布一个 owned staging lease；它不接收、
   * 保存或写入 caller/Game.dll mapped output。接入层负责在原生 caller-owned SEH
   * transaction 中复制并显式结算，避免 Win32 exception 绕过本函数的 C++ RAII。
   */
  CpuSkinMtSynchronousResult runSynchronous(
      const std::shared_ptr<const CpuSkinMtFrozenKernel>& kernel,
      CpuSkinMtSynchronousOutput& output,
      CpuSkinMtRejectReason* reject = nullptr) noexcept;

  /**
   * \brief 推进 generation 并非阻塞 cancel 全部旧工作
   *
   * 仅允许创建 producer 的 owner thread 调用；wrong-thread 返回 0。旧
   * static/palette/output 由 shared ownership 保持到 worker 自然结算；旧 lease
   * 因 generation 不匹配立即失效。
   */
  uint64_t reset() noexcept;

  /**
   * \brief 幂等停止并 join 全部 worker
   *
   * 仅允许创建 producer 的 owner thread 调用；wrong-thread 返回 false，调用者
   * 不得把它解释为 workers 已回收。
   */
  bool shutdown() noexcept;

  /** \brief 返回当前 generation */
  uint64_t generation() const noexcept;

  /** \brief 返回轻量诊断快照 */
  CpuSkinMtDiagnostics snapshotDiagnostics() const noexcept;

private:
  static const std::shared_ptr<CpuSkinMtBatchStateData>& batchState(
      const CpuSkinMtBatchHandle& batch) noexcept;

  static CpuSkinMtBatchHandle makeBatchHandle(
      std::shared_ptr<CpuSkinMtBatchStateData> state) noexcept;

  static CpuSkinMtOutputLease makeOutputLease(
      std::shared_ptr<const CpuSkinMtBatchStateData> state,
      uint32_t jobIndex) noexcept;

  static CpuSkinMtSynchronousOutput makeSynchronousOutput(
      std::shared_ptr<const CpuSkinMtSynchronousOutputState> state) noexcept;

  static bool runFrozenRangeWithInstalledFloatingPointControl(
      const CpuSkinMtFrozenKernel& kernel,
      uint32_t firstVertex,
      uint32_t vertexCount,
      void* outputBase,
      size_t outputByteSize,
      uint32_t* cachedGroupSlot,
      float* cachedMatrix) noexcept;

  class Impl;
  std::unique_ptr<Impl> m_impl;
};

}  // namespace dxvk::war3::gpu_skin
