#pragma once

#include <cstdint>

#include "war3_palette_object_evidence.h"

// 2026-09-17 上级裁定（Step 1③）：对象级证据的**生产侧入口**。
// 本头不依赖 nlohmann / d3d9，便于采集点与宿主机测试共用；转换器实现在 sink .cpp 内。
//
// 纪律：子门关闭时，采集点必须在调用任何 Note* 之前用 PaletteObjectEvidenceEnabled() 短路，
// 从而**不产生**查询、矩阵扫描、锁、日志或记录器操作。

namespace dxvk::war3::tools::evidence {

// 进程级唯一记录器实例（定长表、无堆分配）。
PaletteObjectEvidence& PaletteObjectRecorder() noexcept;

// **唯一的转换点**（纯函数，无会话/环依赖）：把一条记录编码成冻结 wire 的 evidence::Event。
// 生产发射器与"生产转换器往返测试"共用这一份实现，避免测试用复制品。
// 前置：session 由调用方给出（生产侧 = ActiveSession()）。
struct Event;
bool EncodePaletteObjectEvent(const PaletteObjectEventRecord& record,
                              uint64_t session, Event& out) noexcept;

// 会话开始（证据 arm）时绑定发射器并重置；Disarm 只解绑，不清子门配置。
void ArmPaletteObjectEvidence(uint64_t sessionGeneration, uint64_t mapEpoch) noexcept;
void DisarmPaletteObjectEvidence() noexcept;
// 换图/换设备：清空观察表与预算（沿用既有采集会话/地图隔离，不新造"原生对象代际"）。
void ResetPaletteObjectEvidence(uint64_t sessionGeneration, uint64_t mapEpoch) noexcept;

// 观测窗口结束：为表内剩余对象结算终态（Recovered 需候选入队 + 绘制同时成立）。
void ClosePaletteObjectWindow() noexcept;

// 证据环自动冻结前的钩子（见 war3_frame_evidence_core.h 的 PreFreezeHook）：
// 让记录器在任何冻结原因发生前先结算终态。幂等，可被重复调用。
void PaletteObjectPreFreezeHook() noexcept;
// 设备代际变化（Reset）：清空观察表与预算，避免把跨 Reset 的记录合并成同一条链。
// 上级口径：未知不是通配符，不得把跨设备/跨 Reset 的事件强行合并。
void ClearPaletteObjectWatchlist() noexcept;

// 导出头块（解析器要求 schema-7 导出必须存在该块）：**子门关闭时填 0**，
// 解析器据此判定 paletteObjectSubGateDisabled，而不是把"没有事件"读成"没有错误矩阵"。
struct PaletteObjectEvidenceHeader {
  uint64_t watchCount = 0u;
  PaletteObjectEvidence::Counters counters{};
};
void QueryPaletteObjectEvidenceHeader(PaletteObjectEvidenceHeader& out) noexcept;

// 2026-09-18 最小诊断（只读、不改行为）：区分"从未 arm"与"入队块未被到达"。
// 生产采集点在对象级入队块入口处调用 Note*；导出头块暴露其结果，一次采集即可定案。
void NotePaletteObjectEnqueueBlockReached() noexcept;
void NotePaletteObjectAppendEntered() noexcept;
void NotePaletteObjectProductionInsertReached() noexcept;
void NotePaletteObjectProductionNoteCalled() noexcept;
void NotePaletteObjectResetOrClear() noexcept;
uint64_t PaletteObjectResetOrClearCount() noexcept;
uint64_t PaletteObjectProductionNoteCalledCount() noexcept;
uint64_t PaletteObjectProductionInsertReachedCount() noexcept;
uint64_t PaletteObjectAppendEnteredCount() noexcept;
bool PaletteObjectEvidenceArmed() noexcept;
uint64_t PaletteObjectEnqueueBlockReachedCount() noexcept;
// 2026-09-18 P0-5（复审 C2）：发射路径两条早退的具名计数（内部，不上 wire）。
uint64_t PaletteObjectDroppedNoSessionCount() noexcept;
uint64_t PaletteObjectEncodeFailedCount() noexcept;

} // namespace dxvk::war3::tools::evidence
