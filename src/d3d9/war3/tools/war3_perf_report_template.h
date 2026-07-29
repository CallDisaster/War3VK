#pragma once
// War3 性能报告 HTML 模板（schema v9）。
// 由 processExportJob 以 kWar3PerfReportHtmlHead + jsonData + kWar3PerfReportHtmlTail 拼接。
// 约束：模板内容中绝不允许出现 )PERFTPL" 序列（raw string 终止符）。
namespace dxvk::war3 {

// HTML 头部：CSS + 页面骨架 + <script> 中的 `const data = `（JSON 注入点）。
inline constexpr const char* kWar3PerfReportHtmlHead = R"PERFTPL(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>War3 Perf Report</title>
<style>
:root{
  --bg:#0d1117; --panel:#161b22; --border:#30363d;
  --ink:#e6edf3; --ink2:#8b949e;
  --amber:#d29922; --cyan:#39c5cf; --red:#f85149; --green:#3fb950;
}
*{box-sizing:border-box}
html,body{margin:0;padding:0;background:var(--bg);color:var(--ink);
  font:14px/1.45 system-ui,"Segoe UI","Microsoft YaHei",sans-serif}
.num{font-family:"JetBrains Mono",Consolas,monospace;font-variant-numeric:tabular-nums}
.dim{color:var(--ink2)}
a{color:var(--cyan);text-decoration:none}

/* 顶栏（吸顶） */
#topbar{position:sticky;top:0;z-index:20;background:var(--bg);
  border-bottom:1px solid var(--border);padding:8px 16px}
.topbar-row{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.topbar-row.controls{margin-top:8px}
#topbar h1{font-size:16px;margin:0;color:var(--amber);font-weight:600;white-space:nowrap}
.badges{display:flex;gap:6px;flex-wrap:wrap}
.badge{border:1px solid var(--border);border-radius:6px;padding:1px 8px;
  color:var(--ink2);font-size:12px;white-space:nowrap}
.warn-bar{background:rgba(210,153,34,.12);border:1px solid var(--amber);
  color:var(--amber);border-radius:8px;padding:6px 12px;margin-top:8px;font-size:13px}
#globalSearch{flex:1;min-width:260px;background:var(--panel);
  border:1px solid var(--border);border-radius:8px;color:var(--ink);
  padding:6px 12px;font-size:13px;outline:none}
#globalSearch:focus{border-color:var(--cyan)}
#tabs{display:flex;gap:6px}
#tabs button{background:transparent;border:1px solid var(--border);color:var(--ink2);
  border-radius:8px;padding:4px 14px;cursor:pointer;font-size:13px;transition:color .12s ease,border-color .12s ease}
#tabs button.active{color:var(--amber);border-color:var(--amber);background:rgba(210,153,34,.10)}

/* KPI 条 */
#kpiSection{padding:8px 16px 0}
#kpiToggle{margin-bottom:8px}
#kpiBar{display:flex;flex-wrap:wrap;gap:8px}
.kpi{background:var(--panel);border:1px solid var(--border);border-radius:10px;
  padding:8px 14px;min-width:112px}
.kpi .k-label{color:var(--ink2);font-size:12px;white-space:nowrap}
.kpi .k-val{font-size:18px;color:var(--amber);margin-top:2px}
.kpi.danger{border-color:var(--red)}
.kpi.danger .k-val{color:var(--red)}

/* 视图 */
main{padding-bottom:16px}
.view{display:none;padding:8px 16px 0}
.view.active{display:block}
.chips{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:8px}
.chip{border:1px solid var(--border);background:transparent;color:var(--ink2);
  border-radius:999px;padding:2px 10px;cursor:pointer;font-size:12px;transition:color .12s ease,border-color .12s ease}
.chip.on{color:var(--amber);border-color:var(--amber);background:rgba(210,153,34,.12)}
select{background:var(--panel);border:1px solid var(--border);border-radius:8px;
  color:var(--ink);padding:3px 8px;font-size:12px}

/* 表格 */
.table-wrap{overflow-x:auto;border:1px solid var(--border);border-radius:10px;background:var(--panel)}
.tbl{width:100%;border-collapse:collapse;white-space:nowrap}
.tbl th{position:sticky;top:0;background:var(--panel);color:var(--ink2);font-weight:500;
  font-size:12px;text-align:left;height:28px;padding:0 8px;border-bottom:1px solid var(--border)}
.tbl td{height:28px;padding:0 8px;border-bottom:1px solid #21262d;font-size:13px}
.tbl tbody tr:nth-child(even){background:rgba(255,255,255,.02)}
.tbl tbody tr[data-path]{cursor:pointer}
.tbl tbody tr[data-path]:hover{background:rgba(57,197,207,.06)}
.namecell{max-width:360px;overflow:hidden;text-overflow:ellipsis}
.cellbar{display:flex;align-items:center;gap:6px}
.bar{position:relative;flex:0 0 120px;width:120px;height:10px;
  background:#21262d;border-radius:5px;overflow:hidden}
.bar>i{display:block;height:100%;border-radius:5px;transition:width .15s ease}
.tbadge{font-size:11px;border-radius:5px;padding:0 6px;border:1px solid var(--border);white-space:nowrap}
tr.danger-row{background:rgba(248,81,73,.06)}
tr.danger-row .namecell{color:var(--red)}
.split-ok{color:var(--green)}
.split-missing{color:var(--amber)}
.split-note{max-width:520px;color:var(--ink2);font-size:12px;white-space:normal}
.catalog-summary{display:flex;gap:6px;flex-wrap:wrap}
.catalog-state{border:1px solid var(--border);border-radius:6px;padding:1px 7px;
  font-size:11px;white-space:nowrap}
.catalog-state.Installed{color:var(--green);border-color:var(--green)}
.catalog-state.InstallFailed,.catalog-state.AddressUnavailable{
  color:var(--red);border-color:var(--red)}
.catalog-state.SkippedUnsafeABI{color:var(--amber);border-color:var(--amber)}
.gate-text{max-width:430px;white-space:normal;color:var(--ink2);font-size:12px}

/* 调用树（虚拟滚动） */
.tree-scroll{position:relative;overflow:auto;height:68vh;
  border:1px solid var(--border);border-radius:10px;background:var(--panel)}
#treeSpacer{width:1px}
#treeRows{position:absolute;top:0;left:0;right:0}
.tree-row{position:absolute;left:0;right:0;height:28px;display:flex;align-items:center;
  gap:8px;padding-right:8px;white-space:nowrap;border-bottom:1px solid #21262d;
  border-left:2px solid transparent;font-size:13px;cursor:default}
.tree-row:hover{background:rgba(57,197,207,.05)}
.tree-row.match{background:rgba(210,153,34,.10);border-left-color:var(--amber)}
.tree-row.located{background:rgba(57,197,207,.12);border-left-color:var(--cyan)}
.tree-row.danger-row{background:rgba(248,81,73,.06)}
.tree-row.danger-row .tname{color:var(--red)}
.arrow{flex:0 0 14px;text-align:center;color:var(--ink2);cursor:pointer;user-select:none}
.tname{flex:0 0 auto;max-width:420px;overflow:hidden;text-overflow:ellipsis}

/* 时间线 / 泳道 */
.timeline-wrap{position:relative;border:1px solid var(--border);border-radius:10px;
  background:var(--panel);padding:8px;overflow:hidden}
#timelineCanvas{display:block;cursor:crosshair}
.tip{position:absolute;background:#010409;border:1px solid var(--border);
  border-radius:8px;padding:6px 10px;pointer-events:none;font-size:12px;z-index:30;white-space:nowrap}
#timelineInfo{margin:8px 0;font-size:13px}
#timelineHotspots .table-wrap{margin-top:4px}
.stage-panel{margin-top:10px;border:1px solid var(--border);border-radius:10px;
  background:var(--panel);padding:8px}
.stage-controls{display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-bottom:6px}
.stage-controls .stage-title{color:var(--amber);font-weight:600;margin-right:4px}
.stage-slot{max-width:240px}
#stageCanvas{display:block;width:100%}
#stageLegend{display:flex;gap:12px;flex-wrap:wrap;margin:5px 0 2px;font-size:12px}
.stage-legend-item{display:inline-flex;align-items:center;gap:5px;color:var(--ink2)}
.stage-swatch{width:14px;height:3px;border-radius:2px;display:inline-block}
#stageSelectionStats{margin-top:7px}
.lane-row{border:1px solid var(--border);border-radius:10px;background:var(--panel);
  padding:6px 8px;margin-bottom:8px}
.lane-label{font-size:12px;margin-bottom:4px;display:flex;gap:8px;align-items:baseline}
.lane-row canvas{display:block}

/* Legacy 折叠区 */
#legacy{margin:0 16px 16px;border:1px solid var(--border);border-radius:10px;
  background:var(--panel);padding:8px 12px}
#legacy summary{cursor:pointer;color:var(--ink2);font-size:13px}

@media (max-width:1100px){
  .topbar-row{flex-direction:column;align-items:stretch}
  #tabs{flex-wrap:wrap}
  .kpi{min-width:calc(50% - 8px)}
}
</style>
</head>
<body>
<header id="topbar">
  <div class="topbar-row">
    <h1>War3 Perf Report</h1>
    <div id="metaBadges" class="badges"></div>
  </div>
  <div id="coverageWarning" class="warn-bar" hidden></div>
  <div class="topbar-row controls">
    <input id="globalSearch" type="search" spellcheck="false"
      placeholder="搜索：子串匹配 path/name；&gt;1.5 按 ms 阈值；thread:main / thread:cs / thread:worker；空格分隔多条件 AND">
    <nav id="tabs">
      <button type="button" data-view="hotspots" class="active">Scope Hotspots</button>
      <button type="button" data-view="states">快/慢帧相关</button>
      <button type="button" data-view="hooks">Hook Breakdown</button>
      <button type="button" data-view="catalog">Hook Catalog</button>
      <button type="button" data-view="tree">Call Tree</button>
      <button type="button" data-view="timeline">Timeline</button>
      <button type="button" data-view="swimlanes">Swimlanes</button>
    </nav>
  </div>
</header>

<section id="kpiSection">
  <button id="kpiToggle" type="button" class="chip">KPI ▾</button>
  <div id="kpiBar"></div>
  <p class="dim">UncoveredWall = Present 帧墙钟 − 主线程互斥 additive root scope 墙钟；它不是 CPU。Main CPU possible range 是 GetThreadTimes 主线程 CPU 在已覆盖 roots 之外的保守区间；N/A 表示采样或互斥证据不足。Worker CPU 与 Frame wall 并行，禁止相加。</p>
</section>

<main>
  <section id="view-hotspots" class="view active">
    <div class="chips" id="hotspotChips">
      <button type="button" class="chip" data-chip="self">Self&gt;0.1ms</button>
      <button type="button" class="chip" data-chip="idle">Hide idle</button>
      <button type="button" class="chip" data-chip="unattr">显示 UncoveredWall（非 CPU）</button>
      <button type="button" class="chip" data-chip="phase">叠加阶段墙钟</button>
      <span id="hotspotCount" class="dim"></span>
    </div>
    <div class="table-wrap">
      <table class="tbl" id="hotspotTable">
        <thead><tr>
          <th class="num">#</th><th>名称</th><th>Self wall</th><th>Inclusive wall</th>
          <th class="num">calls/帧</th><th class="num">max ms</th><th class="num">p95</th><th>线程</th>
        </tr></thead>
        <tbody id="hotspotBody"></tbody>
      </table>
    </div>
  </section>

  <section id="view-states" class="view">
    <div class="chips" id="stateModeChips">
      <button type="button" class="chip on" data-state-mode="instantaneous">瞬时 cohort</button>
      <button type="button" class="chip" data-state-mode="rollingStable">约 1 秒稳定核心</button>
      <span class="dim">稳定核心排除 threshold±band 死区；Overlay 为正交重复归因，不可与 Self/调用树求和。</span>
    </div>
    <p id="stateSummary" class="dim"></p>
    <div class="table-wrap">
      <table class="tbl">
        <thead><tr>
          <th class="num">#</th><th>区段</th><th>快簇 Self</th>
          <th>慢簇 Self</th><th>Self 增量</th><th class="num">相对变化</th>
          <th class="num">快/慢 calls/帧</th>
          <th class="num">快/慢 GPU incl</th><th class="num">GPU 变化</th>
        </tr></thead>
        <tbody id="statePathBody"></tbody>
      </table>
    </div>
    <p class="dim">Non-additive overlay 关联（仅 Inclusive/calls；与上表及调用树重叠，禁止求和）</p>
    <div class="table-wrap">
      <table class="tbl">
        <thead><tr>
          <th class="num">#</th><th>Overlay 区段</th>
          <th class="num">快簇 Inclusive</th><th class="num">慢簇 Inclusive</th>
          <th>Inclusive 增量</th><th class="num">相对变化</th>
          <th class="num">快/慢 calls/帧</th>
          <th class="num">快/慢 GPU incl</th><th class="num">GPU 变化</th>
        </tr></thead>
        <tbody id="stateOverlayBody"></tbody>
      </table>
    </div>
    <p class="dim">逐帧工作量关联（同一份报告内比较；字节指标保持原始单位）</p>
    <div class="table-wrap">
      <table class="tbl">
        <thead><tr>
          <th>工作量指标</th><th class="num">快簇均值</th>
          <th class="num">慢簇均值</th><th class="num">相对变化</th>
        </tr></thead>
        <tbody id="stateWorkloadBody"></tbody>
      </table>
    </div>
  </section>

  <section id="view-hooks" class="view">
    <div class="chips">
      <select id="hookSort">
        <option value="warvk">按 Hook residual / 自定义逻辑</option>
        <option value="nativeSelf">按原生边界未归因残余</option>
        <option value="nested">按原函数内已归因子树</option>
        <option value="total">按 Hook 总耗时</option>
        <option value="unresolved">未拆分优先</option>
      </select>
      <button type="button" class="chip on" id="hookHotOnly">仅显示 ≥0.01ms</button>
      <span id="hookCount" class="dim"></span>
      <span class="split-note">所有 section 均为墙钟区间，不是线程 CPU。Native inclusive 是原函数指针调用边界；“原生边界残余”已扣除目前可见的 DXVK/WarVK 回调子树，但在覆盖率达到 100% 前不能等同于纯 game.dll。自定义阶段由标签区分 WarVKHookLogic、DXVKFrontendLogic、WarVKCallbackLogic 与纯探针 ObserverOverhead；无原函数的 D3D9 前端和 ShadowCapture callback 的 Native 三列按设计为 0。</span>
    </div>
    <div class="table-wrap">
      <table class="tbl" id="hookTable">
        <thead><tr>
          <th class="num">#</th><th>Hook / 调用上下文</th><th>Hook residual / 自定义阶段</th>
          <th>原生边界未归因残余</th><th>原函数内已归因子树</th><th>Native inclusive</th>
          <th>Hook total</th><th class="num">calls/帧</th><th>拆分状态</th>
        </tr></thead>
        <tbody id="hookBody"></tbody>
      </table>
    </div>
  </section>

  <section id="view-catalog" class="view">
    <div class="chips">
      <select id="catalogStatusFilter">
        <option value="all">全部状态</option>
      </select>
      <select id="catalogDomainFilter">
        <option value="all">全部域</option>
      </select>
      <span id="catalogCoverage" class="dim"></span>
    </div>
    <div id="catalogSummary" class="catalog-summary"></div>
    <p class="dim">Catalog 是安装期静态目录，不是耗时榜。当前仅覆盖 RenderPerf 默认关闭诊断组；禁用、ABI 不安全或地址不可用的条目不会作为 0ms 热点混入 Hook Breakdown。</p>
    <div class="table-wrap">
      <table class="tbl" id="catalogTable">
        <thead><tr>
          <th>ID</th><th>域 / Hook</th><th>当前状态</th><th>最近尝试</th>
          <th>激活门</th><th>计时合同</th><th>安全 / 地址</th>
        </tr></thead>
        <tbody id="catalogBody"></tbody>
      </table>
    </div>
  </section>

  <section id="view-tree" class="view">
    <div class="chips">
      <button type="button" class="chip" id="treeExpandAll">全部展开</button>
      <button type="button" class="chip" id="treeCollapseAll">全部折叠</button>
      <button type="button" class="chip" id="treeCollapse2">折叠到 2 层</button>
      <button type="button" class="chip" id="treeGt5">仅 &gt;5% 分支</button>
      <select id="treeThreadSel"></select>
      <span id="treeCount" class="dim"></span>
    </div>
    <div id="treeScroll" class="tree-scroll">
      <div id="treeSpacer"></div>
      <div id="treeRows"></div>
    </div>
  </section>

  <section id="view-timeline" class="view">
    <div class="timeline-wrap">
      <canvas id="timelineCanvas"></canvas>
      <div id="timelineTip" class="tip" hidden></div>
    </div>
    <div id="timelineInfo" class="dim"></div>
    <div class="stage-panel">
      <div class="stage-controls">
        <span class="stage-title">Stage Trends</span>
        <select id="stageSlot0" class="stage-slot"></select>
        <select id="stageSlot1" class="stage-slot"></select>
        <select id="stageSlot2" class="stage-slot"></select>
        <select id="stageSlot3" class="stage-slot"></select>
        <button type="button" class="chip" data-stage-mode="raw">Raw ms</button>
        <button type="button" class="chip on" data-stage-mode="relative">各自 P50 = 100%</button>
      </div>
      <p class="dim">1–4 条独立 Inclusive/Overlay 曲线，共用帧横轴；禁止相加或堆叠。Present 两列均不包含 PresentImage / SyncFrameLatency。</p>
      <canvas id="stageCanvas"></canvas>
      <div id="stageLegend"></div>
      <div id="stageSelectionStats"></div>
    </div>
    <div id="timelineHotspots"></div>
  </section>

  <section id="view-swimlanes" class="view">
    <p class="dim">MainThread 来自 GetThreadTimes；CS/Other 是按 scope 墙钟形状分配到非主线程 CPU 预算的估算，Worker 是剩余 CPU。三者用于趋势观察，不是逐线程 OS CPU 实测，也不应与 Frame wall 或 GPU 相加。</p>
    <div id="swimlaneRows"></div>
  </section>
</main>

<details id="legacy">
  <summary>Legacy 字段（旧版表格已移除，字段仍保留在 JSON 中）</summary>
  <p id="legacyKeys" class="dim"></p>
</details>

<script>
        const data = )PERFTPL";

// HTML 尾部：全部前端 JS + </script></body></html>。
// 注意首字符必须是 `;`，与 head 末尾的 `const data = ` 拼成完整语句。
inline constexpr const char* kWar3PerfReportHtmlTail = R"PERFTPL(;
// ===== War3 Perf Report 前端逻辑（schema v9，单文件零依赖） =====
(function () {
'use strict';

var sections = data.sections || [];
var threadSections = data.threadSections || [];
var hookInventory = Array.isArray(data.hookInventory) ? data.hookInventory : [];
var hookCatalog = Array.isArray(data.hookCatalog) ? data.hookCatalog : [];
var hookCatalogCoverage = data.hookCatalogCoverage || {};
var frameSeries = data.frameSeries || [];
var stageSeries = data.stageSeries || [];
var stageColumns = data.stageSeriesColumns || [];
var stageMetadata = data.stageSeriesMetadata || [];
var frameState = data.frameStateAnalysis || {};
var workloadSeries = data.workloadSeries || [];
var workloadColumns = data.workloadSeriesColumns || [];
var pcts = data.sectionPercentiles || {};
var meta = data.meta || {};
var frameMs = data.avgFrameTimeMs || 0;
var ROW_H = 28;
var searchText = '';
var stateCohortMode = 'instantaneous';
var stageMode = 'relative';

var LANE_KEYS = ['main', 'cs', 'worker', 'other'];
var LANE_COLORS = { main: '#39c5cf', cs: '#d29922', worker: '#3fb950', other: '#8b949e' };
var LANE_LABEL = { main: 'Main', cs: 'CS', worker: 'Worker', other: 'Other' };
var STAGE_COLORS = ['#39c5cf', '#d29922', '#3fb950', '#bc8cff'];
var stageColumnIndex = {};
var stageMetadataByColumn = {};
var stageRowsByEpoch = {};
for (var sci = 0; sci < stageColumns.length; sci++) {
  stageColumnIndex[stageColumns[sci]] = sci;
}
for (var smi = 0; smi < stageMetadata.length; smi++) {
  stageMetadataByColumn[stageMetadata[smi].column] = stageMetadata[smi];
}
for (var sri = 0; sri < stageSeries.length; sri++) {
  if (stageSeries[sri] && stageSeries[sri].length)
    stageRowsByEpoch[String(stageSeries[sri][0])] = stageSeries[sri];
}

// ---------- 基础工具 ----------
function $(id) { return document.getElementById(id); }
function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
  });
}
function fmtMs(v) {
  v = +v || 0;
  if (v >= 100) return v.toFixed(0);
  if (v >= 10) return v.toFixed(1);
  if (v >= 0.05) return v.toFixed(2);
  if (v >= 0.005) return v.toFixed(3);
  return '0';
}
function fmtOptionalMs(v) {
  return v == null || v === '' || !Number.isFinite(+v) ? '—' : fmtMs(v);
}
function fmtCalls(v) {
  v = +v || 0;
  return v >= 100 ? v.toFixed(0) : v.toFixed(1);
}
function pctOf(v, total) { return total > 0 ? (100 * v / total) : 0; }
function lastSeg(p) {
  if (!p) return '';
  var i = p.lastIndexOf('/');
  return i >= 0 ? p.slice(i + 1) : p;
}
function stripHookPrefix(s) {
  return String(s || '').replace(/^Hook_/, '');
}
function hookContextLabel(path) {
  var parts = String(path || '').split('/');
  var hooks = [];
  for (var i = 0; i < parts.length; i++) {
    if (/^Hook_/.test(parts[i])) hooks.push(stripHookPrefix(parts[i]));
  }
  if (!hooks.length) {
    var last = parts.length ? parts[parts.length - 1] : '';
    if (/^(Orig|Original|NativeOriginal|NativeOriginalInclusive)$/.test(last) &&
        parts.length > 1) {
      return stripHookPrefix(parts[parts.length - 2]);
    }
    return stripHookPrefix(last);
  }
  return hooks.slice(Math.max(0, hooks.length - 3)).join(' › ');
}
var leafNameCounts = {};
sections.forEach(function (s) {
  var leaf = s.name || lastSeg(s.path);
  leafNameCounts[leaf] = (leafNameCounts[leaf] || 0) + 1;
});
function hotspotDisplayName(s) {
  var path = (s && s.path) || '';
  var leaf = (s && s.name) || lastSeg(path);
  if (path.indexOf('FramePipeline/DetachedPhaseWall/') === 0) {
    return stripHookPrefix(leaf) +
      ' · detached phase wall (non-additive; NOT CPU)';
  }
  if (/^(Orig|Original|NativeOriginal|NativeOriginalInclusive)$/.test(leaf)) {
    return hookContextLabel(path) + ' · native original call boundary' +
      (leaf === 'NativeOriginalInclusive' ? ' (inclusive aggregate)' : '');
  }
  if (/^(WarVKPreHook|CustomPre|Pre)$/.test(leaf))
    return hookContextLabel(path) + ' · WarVK pre-hook';
  if (/^(WarVKPostHook|CustomPost|Post)$/.test(leaf))
    return hookContextLabel(path) + ' · WarVK post-hook';
  if (/^Stage\d+$/.test(leaf))
    return hookContextLabel(path) + ' · ' + leaf;
  if ((leafNameCounts[leaf] || 0) > 1) {
    var segs = path.split('/');
    return segs.slice(Math.max(0, segs.length - 2)).map(stripHookPrefix).join(' › ');
  }
  return stripHookPrefix(leaf);
}
function isUncoveredWall(s) {
  if (s && s.isWallGap) return true;
  var text = ((s && s.path) || '') + ' ' + ((s && s.name) || '');
  return /uncoveredframewall|uncovered frame wall|unattributedactivewall/i.test(text);
}
function isDetachedPhaseWall(s) {
  return ((s && s.path) || '').indexOf(
    'FramePipeline/DetachedPhaseWall/') === 0;
}
// 热度渐变：青 -> 琥珀 -> 红
function heatColor(t) {
  t = Math.max(0, Math.min(1, t));
  var c1 = [57, 197, 207], c2 = [210, 153, 34], c3 = [248, 81, 73], a, b, u;
  if (t < 0.5) { a = c1; b = c2; u = t * 2; } else { a = c2; b = c3; u = (t - 0.5) * 2; }
  var r = Math.round(a[0] + (b[0] - a[0]) * u);
  var g = Math.round(a[1] + (b[1] - a[1]) * u);
  var bl = Math.round(a[2] + (b[2] - a[2]) * u);
  return 'rgb(' + r + ',' + g + ',' + bl + ')';
}
function barHtml(v, max, color) {
  var w = max > 0 ? Math.min(100, 100 * v / max) : 0;
  return '<span class="bar"><i style="width:' + w.toFixed(1) + '%;background:' + color + '"></i></span>' +
    '<span class="num">' + fmtMs(v) + '</span>';
}

// ---------- 线程推断 ----------
// threadId 的具体语义未知：按聚合耗时排序依次映射为 main/cs/worker/other（近似）。
var threadTotals = {};
var pathBestThread = {};
threadSections.forEach(function (ts) {
  var t = String(ts.threadId);
  threadTotals[t] = (threadTotals[t] || 0) + (ts.totalCpuMs || 0);
  var cur = pathBestThread[ts.path];
  if (!cur || (ts.totalCpuMs || 0) > cur.v) pathBestThread[ts.path] = { t: t, v: ts.totalCpuMs || 0 };
});
var threadOrder = Object.keys(threadTotals).sort(function (a, b) { return threadTotals[b] - threadTotals[a]; });
function laneOfThread(t) {
  var i = threadOrder.indexOf(String(t));
  return i >= 0 && i < LANE_KEYS.length ? LANE_KEYS[i] : 'other';
}
function bestThreadOf(path) {
  var e = pathBestThread[path];
  return e ? e.t : '';
}

// ---------- 全局搜索解析 ----------
// 支持：普通子串（path/name，AND）；>1.5 阈值（ms）；thread:main/cs/worker/other。
function parseSearch(q) {
  var res = { subs: [], ms: null, thread: null };
  (q || '').trim().split(/\s+/).forEach(function (tok) {
    if (!tok) return;
    if (/^>\s*[\d.]+$/.test(tok)) res.ms = parseFloat(tok.slice(1));
    else if (/^thread:/i.test(tok)) res.thread = tok.slice(7).toLowerCase();
    else res.subs.push(tok.toLowerCase());
  });
  return res;
}
function searchActive(q) { return q.subs.length > 0 || q.ms != null || !!q.thread; }
function matchSearch(sec, q) {
  if (!searchActive(q)) return true;
  var path = (sec.path || '').toLowerCase();
  var name = (sec.name || '').toLowerCase();
  for (var i = 0; i < q.subs.length; i++) {
    if (path.indexOf(q.subs[i]) < 0 && name.indexOf(q.subs[i]) < 0) return false;
  }
  if (q.ms != null && !((sec.avgSelfCpuMs || 0) >= q.ms || (sec.avgCpuMs || 0) >= q.ms)) return false;
  if (q.thread) {
    var bt = bestThreadOf(sec.path);
    if (laneOfThread(bt) !== q.thread && String(bt).toLowerCase().indexOf(q.thread) < 0) return false;
  }
  return true;
}

// ---------- 顶栏 / KPI ----------
function colMean(idx) {
  if (!frameSeries.length) return 0;
  var s = 0;
  for (var i = 0; i < frameSeries.length; i++) s += frameSeries[i][idx] || 0;
  return s / frameSeries.length;
}
function renderHeader() {
  var b = [];
  b.push('schema v' + (data.schemaVersion || '?'));
  if (meta.dllSha256) b.push('DLL ' + String(meta.dllSha256).slice(0, 8));
  if (meta.dllFileSize) b.push(fmtInt0(meta.dllFileSize) + ' B');
  if (meta.buildTimestamp) b.push(meta.buildTimestamp);
  if (meta.runtimeProfile) b.push('profile: ' + meta.runtimeProfile);
  if (meta.disabledModules && meta.disabledModules.length)
    b.push('disabled: ' + meta.disabledModules.join(','));
  b.push('frames ' + (data.frameCount || 0) + ' / ' + (data.windowSec || 0) + 's');
  $('metaBadges').innerHTML = b.map(function (x) { return '<span class="badge">' + esc(x) + '</span>'; }).join('');
  var warnings = [];
  var mainDiag = data.mainThreadProbeDiagnostics;
  if (mainDiag && !mainDiag.match) {
    warnings.push('主线程身份不可信：probe TID ' +
      (mainDiag.trackedThreadId || 0) + ' != WorldRender TID ' +
      (mainDiag.worldRenderThreadId || 0));
  }
  var uncoveredWall = data.avgUncoveredFrameWallMs != null
    ? data.avgUncoveredFrameWallMs : data.avgUnattributedCpuMs;
  if (data.frameWallCoverageWarning || data.coverageWarning) {
    warnings.push('主线程 additive root 未覆盖的帧墙钟超过 2ms/帧（' +
      fmtMs(uncoveredWall) + ' ms）。这是 Present 墙钟区间，不是 CPU 热点，也不能与 worker CPU 相加');
  }
  if (warnings.length) {
    var w = $('coverageWarning');
    w.hidden = false;
    w.textContent = '⚠ ' + warnings.join('；') + '。';
  }
}
function fmtInt0(v) { return String(Math.round(+v || 0)); }
function renderKpis() {
  var wallAttr = data.frameWallAttribution || {};
  var uncoveredWall = data.avgUncoveredFrameWallMs != null
    ? data.avgUncoveredFrameWallMs : data.avgUnattributedCpuMs;
  var wallCoverage = data.frameWallScopeCoveragePct != null
    ? data.frameWallScopeCoveragePct : data.cpuCoveragePct;
  var mainCpuBounds = wallAttr.mainThreadCpuBoundsAvailable
    ? (wallAttr.mainThreadCpuUpperBoundAvailable
      ? (fmtMs(wallAttr.unattributedMainThreadCpuLowerBoundMs) + '–' +
         fmtMs(wallAttr.unattributedMainThreadCpuUpperBoundMs))
      : ('≥' + fmtMs(wallAttr.unattributedMainThreadCpuLowerBoundMs)))
    : 'N/A';
  var kpis = [
    ['AvgFPS', (data.avgFps || 0).toFixed(1), false],
    ['Frame ms', fmtMs(frameMs), false],
    ['Process CPU ms', fmtMs(data.avgProcessCpuMs), false],
    ['MainThread ms', fmtMs(data.avgMainThreadCpuMs), false],
    ['Main CPU possible range ms', mainCpuBounds, false],
    ['CS est. CPU ms', fmtMs(colMean(4)), false],
    ['Worker residual CPU ms', fmtMs(colMean(5)), false],
    ['Other est. CPU ms', fmtMs(colMean(6)), false],
    ['GPU ms', fmtOptionalMs(data.avgGpuTimeMs), false],
    ['UncoveredWall ms (NOT CPU)', fmtMs(uncoveredWall), (uncoveredWall || 0) > 2],
    ['Main-root wall coverage %', (wallCoverage || 0).toFixed(1), false]
  ];
  $('kpiBar').innerHTML = kpis.map(function (k) {
    return '<div class="kpi' + (k[2] ? ' danger' : '') + '">' +
      '<div class="k-label">' + esc(k[0]) + '</div>' +
      '<div class="k-val num">' + esc(k[1]) + '</div></div>';
  }).join('');
}

// ---------- 视图 1：Hotspots ----------
var hsChips = { self: false, idle: false, unattr: false, phase: false };
function renderHotspots() {
  var q = parseSearch(searchText);
  var rows = [];
  for (var i = 0; i < sections.length; i++) {
    var s = sections[i];
    // 未覆盖墙钟是预算差值，不是可优化函数。默认从 CPU/函数热点榜移除；
    // 只有显式点击“显示未覆盖墙钟”时才单独查看。
    if (isUncoveredWall(s) && !hsChips.unattr) continue;
    // Detached 阶段墙钟用于框定 Prepare/Render 前后的预算窗口，但不是
    // 函数 self CPU。默认不让它们挤占热点榜；需要时显式叠加比较。
    if (isDetachedPhaseWall(s) && !hsChips.phase) continue;
    if (hsChips.self && (s.avgSelfCpuMs || 0) < 0.1) continue;
    if (hsChips.idle && s.isIdleWait) continue;
    if (hsChips.unattr && !isUncoveredWall(s)) continue;
    if (!matchSearch(s, q)) continue;
    rows.push(s);
  }
  rows.sort(function (a, b) { return (b.avgSelfCpuMs || 0) - (a.avgSelfCpuMs || 0); });
  var maxSelf = 0, maxIncl = 0;
  rows.forEach(function (s) {
    maxSelf = Math.max(maxSelf, s.avgSelfCpuMs || 0);
    maxIncl = Math.max(maxIncl, s.avgCpuMs || 0);
  });
  var html = '';
  for (var r = 0; r < rows.length; r++) {
    var s2 = rows[r];
    var lane = laneOfThread(bestThreadOf(s2.path));
    var hasThread = !!bestThreadOf(s2.path);
    var p95 = pcts[s2.path] ? pcts[s2.path].p95 : null;
    var selfT = maxSelf > 0 ? (s2.avgSelfCpuMs || 0) / maxSelf : 0;
    html += '<tr data-path="' + esc(s2.path || '') + '"' + (isUncoveredWall(s2) ? ' class="danger-row"' : '') + '>' +
      '<td class="num dim">' + (r + 1) + '</td>' +
      '<td class="namecell" title="' + esc(s2.path || '') + '">' + esc(hotspotDisplayName(s2)) + '</td>' +
      '<td><div class="cellbar">' + barHtml(s2.avgSelfCpuMs || 0, maxSelf, heatColor(selfT)) + '</div></td>' +
      '<td><div class="cellbar">' + barHtml(s2.avgCpuMs || 0, maxIncl, 'var(--cyan)') + '</div></td>' +
      '<td class="num">' + fmtCalls(s2.callsPerFrame) + '</td>' +
      '<td class="num">' + fmtMs(s2.maxCpuMs) + '</td>' +
      '<td class="num">' + (p95 != null ? fmtMs(p95) : '-') + '</td>' +
      '<td>' + (hasThread
        ? '<span class="tbadge" style="color:' + LANE_COLORS[lane] + ';border-color:' + LANE_COLORS[lane] + '">' + LANE_LABEL[lane] + '</span>'
        : '<span class="tbadge dim" title="threadSections 无此 path，可能为多线程聚合">多线程</span>') + '</td>' +
      '</tr>';
  }
  $('hotspotBody').innerHTML = html;
  $('hotspotCount').textContent = rows.length + ' / ' + sections.length + ' sections';
}

// ---------- 快/慢帧状态归因 ----------
function signedPct(v) {
  if (v == null || v === '' || !Number.isFinite(+v)) return '—';
  v = +v;
  return (v >= 0 ? '+' : '') + v.toFixed(1) + '%';
}
function renderStateAttribution() {
  var selected = stateCohortMode === 'rollingStable'
    ? (frameState.rollingStable || {}) : frameState;
  var isStable = stateCohortMode === 'rollingStable';
  if (!frameState.valid || !selected.valid) {
    $('stateSummary').textContent =
      (!frameState.valid
        ? '当前窗口没有形成样本量和数值分离都足够的快/慢帧描述簇。'
        : '约 1 秒稳定核心样本不足：threshold±band 死区已排除，快/慢两侧各至少需要 8 帧。');
    $('statePathBody').innerHTML = '';
    $('stateOverlayBody').innerHTML = '';
    $('stateWorkloadBody').innerHTML = '';
    return;
  }

  var fast = selected.fast || {}, slow = selected.slow || {};
  var temporal = frameState.temporalPersistence || {};
  var instantRuns = temporal.instantaneous || {};
  var rollingRuns = temporal.rolling || {};
  var persistenceText =
    '；瞬时标签切换 ' + (instantRuns.transitionCount || 0) + ' 次' +
    '，约 ' + (((temporal.rollingWindowTargetMs || 1000) / 1000).toFixed(1)) +
    's 滚动+迟滞后切换 ' + (rollingRuns.transitionCount || 0) + ' 次' +
    '，最长快/慢段 ' +
    (((rollingRuns.maxFastRunWallMs || 0) / 1000).toFixed(2)) + 's / ' +
    (((rollingRuns.maxSlowRunWallMs || 0) / 1000).toFixed(2)) + 's';
  var modeText = isStable
    ? ('约 1 秒滚动稳定核心（死区 ' +
       fmtMs(selected.deadbandLowMs) + '–' +
       fmtMs(selected.deadbandHighMs) + ' ms）：')
    : '导出端瞬时离线分簇：';
  $('stateSummary').textContent =
    modeText + '快簇 ' + fmtMs(fast.avgFrameWallMs) +
    ' ms（' + (fast.frameCount || 0) + ' 帧），慢簇 ' +
    fmtMs(slow.avgFrameWallMs) + ' ms（' + (slow.frameCount || 0) +
    ' 帧），帧墙钟 ' + signedPct(selected.relativeFrameWallDeltaPct) +
    '；GPU ' + signedPct(selected.relativeGpuDeltaPct) +
    '（n=' + (fast.gpuSamples || 0) + '/' + (slow.gpuSamples || 0) + '）' +
    '，Main CPU ' + signedPct(selected.relativeMainCpuDeltaPct) +
    '（n=' + (fast.mainCpuSamples || 0) + '/' +
      (slow.mainCpuSamples || 0) + '）' +
    '。这是描述性快/慢 cohort 切分，不证明存在离散引擎状态或因果关系；' +
    '下表用于定位相关区段，不把 OS CPU、GPU 与墙钟相加' +
    persistenceText + '。';

  var q = parseSearch(searchText);
  var rows = (selected.paths || []).filter(function (r) {
    return matchSearch({
      path: r.path || '', name: r.name || '',
      avgSelfCpuMs: Math.abs(r.selfDeltaMs || 0),
      avgCpuMs: Math.abs(r.inclusiveDeltaMs || 0)
    }, q);
  });
  rows.sort(function (a, b) {
    return Math.abs(b.selfDeltaMs || 0) - Math.abs(a.selfDeltaMs || 0);
  });
  var maxDelta = 0;
  rows.forEach(function (r) {
    maxDelta = Math.max(maxDelta, Math.abs(r.selfDeltaMs || 0));
  });
  var html = '';
  for (var i = 0; i < rows.length; i++) {
    var r = rows[i], delta = r.selfDeltaMs || 0;
    html += '<tr data-path="' + esc(r.path || '') + '">' +
      '<td class="num dim">' + (i + 1) + '</td>' +
      '<td class="namecell" title="' + esc(r.path || '') + '">' +
        esc(hotspotDisplayName(r)) + '</td>' +
      '<td class="num">' + fmtMs(r.fastSelfMs) + '</td>' +
      '<td class="num">' + fmtMs(r.slowSelfMs) + '</td>' +
      '<td><div class="cellbar">' +
        barHtml(Math.abs(delta), maxDelta,
          delta >= 0 ? 'var(--red)' : 'var(--green)') +
        '<span class="dim">' + (delta >= 0 ? '慢簇增加' : '慢簇减少') +
        '</span></div></td>' +
      '<td class="num">' + signedPct(r.selfDeltaPct) + '</td>' +
      '<td class="num">' + fmtCalls(r.fastCallsPerFrame) + ' / ' +
        fmtCalls(r.slowCallsPerFrame) + '</td>' +
      '<td class="num">' + fmtOptionalMs(r.fastGpuMs) + ' / ' +
        fmtOptionalMs(r.slowGpuMs) + ' <span class="dim">n=' +
        (r.fastGpuSamples || 0) + '/' + (r.slowGpuSamples || 0) +
        '</span></td>' +
      '<td class="num">' + signedPct(r.gpuDeltaPct) + '</td></tr>';
  }
  $('statePathBody').innerHTML = html;

  var overlayRows = (selected.overlayPaths || []).filter(function (r) {
    return matchSearch({
      path: r.path || '', name: r.name || '',
      avgSelfCpuMs: 0,
      avgCpuMs: Math.abs(r.inclusiveDeltaMs || 0)
    }, q);
  });
  overlayRows.sort(function (a, b) {
    return Math.abs(b.inclusiveDeltaMs || 0) -
      Math.abs(a.inclusiveDeltaMs || 0);
  });
  var maxOverlayDelta = 0;
  overlayRows.forEach(function (r) {
    maxOverlayDelta = Math.max(
      maxOverlayDelta, Math.abs(r.inclusiveDeltaMs || 0));
  });
  var overlayHtml = '';
  for (var oi = 0; oi < overlayRows.length; oi++) {
    var orow = overlayRows[oi], odelta = orow.inclusiveDeltaMs || 0;
    overlayHtml += '<tr data-path="' + esc(orow.path || '') + '">' +
      '<td class="num dim">' + (oi + 1) + '</td>' +
      '<td class="namecell" title="' + esc(orow.path || '') + '">' +
        esc(hotspotDisplayName(orow)) + '</td>' +
      '<td class="num">' + fmtMs(orow.fastInclusiveMs) + '</td>' +
      '<td class="num">' + fmtMs(orow.slowInclusiveMs) + '</td>' +
      '<td><div class="cellbar">' +
        barHtml(Math.abs(odelta), maxOverlayDelta,
          odelta >= 0 ? 'var(--red)' : 'var(--green)') +
        '<span class="dim">' +
          (odelta >= 0 ? '慢簇增加' : '慢簇减少') +
        '</span></div></td>' +
      '<td class="num">' + signedPct(orow.inclusiveDeltaPct) + '</td>' +
      '<td class="num">' + fmtCalls(orow.fastCallsPerFrame) + ' / ' +
        fmtCalls(orow.slowCallsPerFrame) + '</td>' +
      '<td class="num">' + fmtOptionalMs(orow.fastGpuMs) + ' / ' +
        fmtOptionalMs(orow.slowGpuMs) + ' <span class="dim">n=' +
        (orow.fastGpuSamples || 0) + '/' +
        (orow.slowGpuSamples || 0) + '</span></td>' +
      '<td class="num">' + signedPct(orow.gpuDeltaPct) + '</td></tr>';
  }
  $('stateOverlayBody').innerHTML = overlayHtml;

  var stateByEpoch = {};
  (selected.stateSeries || []).forEach(function (row) {
    stateByEpoch[String(row[0])] = row[1];
  });
  var columnIndex = {};
  workloadColumns.forEach(function (name, idx) { columnIndex[name] = idx; });
  var metrics = [
    ['capturedDrawCount', 'Shadow captured draws', 'hasShadowBudget'],
    ['uniqueGeometryCount', 'Unique geometry', 'hasShadowBudget'],
    ['dynamicSkinnedOutputCount', 'Dynamic skinned output', 'hasShadowBudget'],
    ['fallbackDrawCount', 'Fallback draws', 'hasShadowBudget'],
    ['semanticSceneSubmitted', 'Semantic submitted', 'hasShadowBudget'],
    ['replayCasterCount', 'Replay casters', 'hasShadowReceiver'],
    ['replayGeometryWork', 'Replay geometry work', 'hasShadowReceiver'],
    ['createAttempts', 'Persistent create attempts', 'hasPersistentGeometry'],
    ['capacityFastReject', 'Persistent fast-capacity reject subset',
      'hasPersistentGeometry'],
    ['expiryTokensPopped', 'Persistent expiry tokens popped',
      'hasPersistentGeometry'],
    ['expiryTokensRequeued', 'Persistent expiry tokens requeued',
      'hasPersistentGeometry'],
    ['expiryStaleTokens', 'Persistent stale expiry tokens',
      'hasPersistentGeometry'],
    ['expiryAgeEvictions', 'Persistent age evictions',
      'hasPersistentGeometry'],
    ['expiryQueueSize', 'Persistent expiry queue size',
      'hasPersistentGeometry'],
    ['poolBytesUsed', 'Persistent pool used bytes', 'hasPersistentGeometry'],
    ['poolBytesEvictedDelta', 'Persistent evicted bytes/frame',
      'hasPersistentGeometry'],
    ['liveGeometryCount', 'Persistent live geometry', 'hasPersistentGeometry']
  ];
  var workloadHtml = '';
  metrics.forEach(function (metric) {
    var idx = columnIndex[metric[0]];
    if (idx == null) return;
    var availabilityIdx = columnIndex[metric[2]];
    var sums = [0, 0], counts = [0, 0];
    workloadSeries.forEach(function (row) {
      var state = stateByEpoch[String(row[0])];
      if (state !== 0 && state !== 1) return;
      if (availabilityIdx != null && !row[availabilityIdx]) return;
      var value = +row[idx] || 0;
      sums[state] += value;
      counts[state]++;
    });
    var a = counts[0] ? sums[0] / counts[0] : 0;
    var b = counts[1] ? sums[1] / counts[1] : 0;
    var deltaPct = a > 1e-9 ? (b - a) / a * 100 : null;
    workloadHtml += '<tr><td>' + esc(metric[1]) + '</td>' +
      '<td class="num">' + (Math.abs(a) >= 1000 ? a.toFixed(0) : fmtMs(a)) +
      ' <span class="dim">n=' + counts[0] + '</span></td>' +
      '<td class="num">' + (Math.abs(b) >= 1000 ? b.toFixed(0) : fmtMs(b)) +
      ' <span class="dim">n=' + counts[1] + '</span></td>' +
      '<td class="num">' + signedPct(deltaPct) +
      '</td></tr>';
  });
  $('stateWorkloadBody').innerHTML = workloadHtml;
}

// ---------- Hook 原生 / 自定义分账 ----------
var hookHotOnly = true;
function normalizeHookIdentity(s) {
  return String(s || '').toLowerCase()
    .replace(/^hook_/, '')
    .replace(/^(cworldframewar3|cworldframe|cworld|war3)/, '')
    .replace(/(updateandpreparepasses)/g, 'prepare')
    .replace(/[^a-z0-9]/g, '');
}
function hookInventoryTimingAlias(inv) {
  var key = String((inv && inv.domain) || '') + '/' +
    String((inv && inv.hookName) || '');
  var aliases = {
    'Storm/SMemAlloc': 'Hook_Storm_Alloc',
    'Storm/SMemFree': 'Hook_Storm_Free',
    'Storm/SMemGetSize': 'Hook_Storm_GetSize',
    'Storm/SMemReAlloc': 'Hook_Storm_ReAlloc',
    'WidgetIdentity/CWidget_RegisterFootprintAndShadowMask':
      'Hook_Widget_RegisterFootprintAndShadowMask',
    'Shadow/Terrain_RenderShadowLayer': 'Hook_Shadow_TerrainRenderLayer',
    'Shadow/TerrainShadow_RenderListB': 'Hook_Shadow_TerrainListB',
    'Shadow/CUnitUIManager_RecordSetUnitShadow':
      'Hook_Shadow_CUnitUI_RecordSetUnitShadow',
    'Shadow/CUnitUIManager_RecordSetStructureShadow':
      'Hook_Shadow_CUnitUI_RecordSetStructureShadow',
    'Shadow/ShadowProjector_Add_FromObject': 'Hook_ShadowProjector_FromObject',
    'Shadow/ShadowProjector_Add_Simple': 'Hook_ShadowProjector_Simple',
    'Jass/ExecuteJassFunction': 'Hook_ExecuteJassFunction',
    'Lifecycle/EventMainCallback': 'Callback',
    'Lifecycle/EventMessagePump': 'Pump',
    'Lifecycle/EventDispatch': 'Hook_EventDispatch',
    'Lifecycle/EngineTlsPump': 'Hook_EngineTlsPump',
    'Lifecycle/EngineSelectWorker': 'Hook_EngineSelectWorker',
    'Lifecycle/EngineRunCallbacks': 'Hook_EngineRunCallbacks',
    'Lifecycle/EngineQueueFlush': 'Hook_EngineQueueFlush',
    'Lifecycle/EngineFinalizeTick': 'Hook_EngineFinalizeTick',
    'Lifecycle/EngineReschedule': 'Hook_EngineReschedule',
    'Lifecycle/EnginePrepareDispatch': 'Hook_EnginePrepareDispatch',
    'Lifecycle/EngineFinalizeDispatch': 'Hook_EngineFinalizeDispatch',
    'Lifecycle/EngineTickUpdate': 'Hook_EngineTickUpdate',
    'Lifecycle/EngineFinalizeWorker': 'Hook_EngineFinalizeWorker',
    'Lifecycle/EngineComputeWakeDelta': 'Hook_EngineComputeWakeDelta',
    'Lifecycle/EnginePrepareWait': 'PrepareWait',
    'Lifecycle/EngineWaitGate': 'WaitGate',
    'Render/RenderDispatcher': 'Hook_RenderDispatcher',
    'Render/SceneSubmitBatch': 'Hook_SceneSubmitBatch',
    'Model/PromoteRuntimeModel': 'Hook_Model_PromoteRuntimeModel',
    'Model/CreateSpriteAndBindSourceObject':
      'Hook_Model_CreateSpriteAndBindSourceObject',
    'Model/CreateSpriteRuntime': 'Hook_Model_CreateSpriteRuntime',
    'Model/CreateGeosetFromRawArrays': 'Hook_Model_CreateGeosetFromRawArrays',
    'CurrentDraw/RenderQueueUpdateItemWorldMatrix':
      'Hook_CurrentDraw_UpdateWorldMatrix'
  };
  return aliases[key] || (inv && inv.hookName) || '';
}

// 报告导出期的静态语义表；不进入 Hook 热路径，也不增加 QPC、字符串 scope
// 或锁。nativeMode='none' 表示该入口本身就是 DXVK/WarVK 回调边界，没有
// game.dll trampoline；ObserverOverhead 表示 detour 只负责测量并调用原函数。
var HOOK_ROOT_SEMANTICS = {
  'DXVK_D3D9_DrawPrimitive':
    { customPhase: 'DXVKFrontendLogic', nativeMode: 'none' },
  'DXVK_D3D9_DrawIndexedPrimitive':
    { customPhase: 'DXVKFrontendLogic', nativeMode: 'none' },
  'DXVK_D3D9_DrawPrimitiveUP':
    { customPhase: 'DXVKFrontendLogic', nativeMode: 'none' },
  'DXVK_D3D9_DrawIndexedPrimitiveUP':
    { customPhase: 'DXVKFrontendLogic', nativeMode: 'none' },
  'WarVKCallback_ShadowCapture':
    { customPhase: 'WarVKCallbackLogic', nativeMode: 'none' },
  'Hook_RenderQueue_StageUpdate':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_RenderQueue_FlushTransparent':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_TransparentDispatch_Type0_RenderBatch':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_TransparentDispatch_Type1_ParticleEmitter':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_TransparentDispatch_Type2_ImageLike':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_TransparentDispatch_Type3_RibbonEmitter':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_TransparentDispatch_Type4_CallbackWrapper':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineTlsPump':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineSelectWorker':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineRunCallbacks':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineQueueFlush':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineFinalizeTick':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineReschedule':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EnginePrepareDispatch':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineFinalizeDispatch':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineTickUpdate':
    { customPhase: 'WarVKHookLogic', nativeMode: 'trampoline' },
  'Hook_EngineFinalizeWorker':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_EngineComputeWakeDelta':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_CameraBuildFrustum':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_TerrainShadowFlush':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_TerrainExtraPass':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_ShadowProjectorFlush':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_TargetIndicatorRingAdvance':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_CinematicFilterTimeAdvance':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' },
  'Hook_WorldPrepare_RuntimeFlagClockAdvance3B8760':
    { customPhase: 'ObserverOverhead', nativeMode: 'trampoline' }
};
function hookRootSemantics(leaf) {
  var exact = HOOK_ROOT_SEMANTICS[leaf];
  if (exact) return exact;
  // D3D9Device/SwapChain API entry scopes are DXVK frontend boundaries, not
  // MinHook detours into game.dll. Keep this prefix rule generic so new API
  // phase roots such as PresentEx cannot silently fall back to "unresolved".
  if (/^DXVK_D3D9_/.test(leaf))
    return { customPhase: 'DXVKFrontendLogic', nativeMode: 'none' };
  return null;
}
function isHookCustomPhaseName(name) {
  return /^(WarVKHookLogic|DXVKFrontendLogic|WarVKCallbackLogic|ObserverOverhead)$/.test(name || '');
}

function collectHookBreakdownRows() {
  var children = {};
  sections.forEach(function (s) {
    var p = s.parentPath || '';
    if (!children[p]) children[p] = [];
    children[p].push(s);
  });
  var rows = [];
  sections.forEach(function (s) {
    var leaf = s.name || lastSeg(s.path);
    var direct = children[s.path] || [];
    var semantics = hookRootSemantics(leaf);
    var nativeChildren = direct.filter(function (c) {
      return /^(Orig|Original|NativeOriginal|NativeOriginalInclusive)$/.test(c.name || lastSeg(c.path));
    });
    var customChildren = direct.filter(function (c) {
      return isHookCustomPhaseName(c.name || lastSeg(c.path));
    });
    var looksHook = /^Hook_/.test(leaf) || /^Native\//.test(s.path || '') ||
      /^DXVK_D3D9_/.test(leaf) || /^WarVKCallback_/.test(leaf) || !!semantics;
    if (!looksHook && !nativeChildren.length) return;
    var nativeIncl = 0, nativeSelf = 0, nativeCalls = 0;
    var inclusiveOnly = false, exactNativeCount = 0;
    nativeChildren.forEach(function (c) {
      var nativeLeaf = c.name || lastSeg(c.path);
      nativeIncl += c.avgCpuMs || 0;
      if (nativeLeaf === 'NativeOriginalInclusive') inclusiveOnly = true;
      nativeSelf += c.avgSelfCpuMs || 0;
      exactNativeCount++;
      nativeCalls += c.callsPerFrame || 0;
    });
    var total = s.avgCpuMs || 0;
    var noNativeByDesign = !!semantics && semantics.nativeMode === 'none';
    var split = nativeChildren.length > 0 || noNativeByDesign;
    var customPhase = semantics ? semantics.customPhase :
      (customChildren.length
        ? (customChildren[0].name || lastSeg(customChildren[0].path))
        : 'WarVKHookLogic');
    var customMs = noNativeByDesign
      ? total
      : (nativeChildren.length ? Math.max(0, total - nativeIncl) : 0);
    rows.push({
      sec: s, path: s.path || '', label: hookContextLabel(s.path),
      total: total,
      nativeIncl: nativeIncl,
      nativeSelf: nativeSelf,
      nested: exactNativeCount ? Math.max(0, nativeIncl - nativeSelf) : 0,
      warvk: customMs,
      unresolved: split ? 0 : total,
      calls: s.callsPerFrame || nativeCalls || 0,
      split: split,
      exclusiveKnown: split && exactNativeCount > 0,
      inclusiveOnly: inclusiveOnly,
      customPhase: customPhase,
      observerOnly: customPhase === 'ObserverOverhead',
      noNativeByDesign: noNativeByDesign
    });
  });

  // 安装清单补齐“本采样窗口没有调用”的入口。它们总量为 0，因此默认
  // 热点过滤会隐藏；关闭“仅显示 >=0.01ms”即可审阅全部已安装 Hook。
  var observedKeys = rows.map(function (r) {
    return normalizeHookIdentity((r.sec && r.sec.name) || r.label || r.path);
  }).filter(Boolean);
  hookInventory.forEach(function (inv) {
    var key = normalizeHookIdentity(hookInventoryTimingAlias(inv));
    var observed = observedKeys.some(function (known) {
      return key && known && (known.indexOf(key) >= 0 || key.indexOf(known) >= 0);
    });
    if (observed) return;
    var domain = inv.domain || 'Unknown';
    var name = inv.hookName || 'UnnamedHook';
    var path = 'HookInventory/' + domain + '/' + name;
    rows.push({
      sec: { path: path, name: name, parentPath: 'HookInventory/' + domain },
      path: path, label: domain + ' › ' + name,
      total: 0, nativeIncl: 0, nativeSelf: 0, nested: 0,
      warvk: 0, unresolved: 0, calls: 0,
      split: false, exclusiveKnown: false, inclusiveOnly: false,
      customPhase: 'Unresolved',
      observerOnly: false, noNativeByDesign: false,
      inventoryOnly: true, installed: !!inv.installed,
      installState: inv.state || 'unknown'
    });
  });
  return rows;
}
function hookSplitStatusText(r) {
  if (r.inventoryOnly) {
    return r.installed
      ? '已安装；本窗口未调用，或需 PERF_LEVEL=2 才记录热调用树'
      : '未激活：' + r.installState;
  }
  if (r.noNativeByDesign)
    return '无原生 trampoline，闭合；全部归入 ' + r.customPhase;
  if (r.observerOnly)
    return '纯观察器；Hook residual 归入 ObserverOverhead';
  if (r.inclusiveOnly)
    return '总量/原生/自定义已闭合；残余会随回调覆盖增加而继续收窄';
  if (r.split)
    return '动态子树闭合；残余不是纯 game.dll 声明';
  return '按设计未调用原函数，或尚未埋点';
}
function renderHookBreakdown() {
  var q = parseSearch(searchText);
  var rows = collectHookBreakdownRows().filter(function (r) {
    if (hookHotOnly && r.total < 0.01) return false;
    return matchSearch(r.sec, q);
  });
  var sortKey = $('hookSort') ? $('hookSort').value : 'warvk';
  rows.sort(function (a, b) {
    if (sortKey === 'unresolved' && a.split !== b.split) return a.split ? 1 : -1;
    var key = sortKey === 'unresolved' ? 'total' : sortKey;
    return (b[key] || 0) - (a[key] || 0);
  });
  var maxima = { warvk: 0, nativeSelf: 0, nested: 0, nativeIncl: 0, total: 0 };
  rows.forEach(function (r) {
    Object.keys(maxima).forEach(function (k) { maxima[k] = Math.max(maxima[k], r[k] || 0); });
  });
  var html = '';
  rows.forEach(function (r, i) {
    html += '<tr data-path="' + esc(r.path) + '">' +
      '<td class="num dim">' + (i + 1) + '</td>' +
      '<td class="namecell" title="' + esc(r.path) + '">' + esc(r.label) + '</td>' +
      '<td><div class="cellbar"><span class="tbadge">' +
        esc(r.customPhase || 'WarVKHookLogic') + '</span>' +
        barHtml(r.warvk, maxima.warvk, r.observerOnly ? '#8b949e' : 'var(--amber)') +
        '</div></td>' +
      '<td>' + (r.exclusiveKnown || r.noNativeByDesign
        ? '<div class="cellbar">' + barHtml(r.nativeSelf, maxima.nativeSelf, 'var(--cyan)') + '</div>'
        : '<span class="dim">—（聚合边界）</span>') + '</td>' +
      '<td>' + (r.exclusiveKnown || r.noNativeByDesign
        ? '<div class="cellbar">' + barHtml(r.nested, maxima.nested, 'var(--green)') + '</div>'
        : '<span class="dim">—</span>') + '</td>' +
      '<td><div class="cellbar">' + barHtml(r.nativeIncl, maxima.nativeIncl, '#8b949e') + '</div></td>' +
      '<td><div class="cellbar">' + barHtml(r.total, maxima.total, 'var(--red)') + '</div></td>' +
      '<td class="num">' + fmtCalls(r.calls) + '</td>' +
      '<td class="' + (r.split ? 'split-ok' : 'split-missing') + '">' +
        esc(hookSplitStatusText(r)) + '</td></tr>';
  });
  $('hookBody').innerHTML = html;
  $('hookCount').textContent = rows.length + ' contexts / ' +
    hookInventory.length + ' installed records';
}

// ---------- Static Hook Catalog（安装状态，不参与热点排序） ----------
function initializeHookCatalogControls() {
  var statuses = {}, domains = {};
  hookCatalog.forEach(function (row) {
    statuses[row.status || 'NotEvaluated'] = true;
    domains[row.domain || 'Unknown'] = true;
  });
  var statusSel = $('catalogStatusFilter');
  Object.keys(statuses).sort().forEach(function (state) {
    var option = document.createElement('option');
    option.value = state;
    option.textContent = state;
    statusSel.appendChild(option);
  });
  var domainSel = $('catalogDomainFilter');
  Object.keys(domains).sort().forEach(function (domain) {
    var option = document.createElement('option');
    option.value = domain;
    option.textContent = domain;
    domainSel.appendChild(option);
  });
}
function catalogGateText(row) {
  var gate = row.activationGate || {};
  if (gate.displayExpression) return gate.displayExpression;
  var parts = [];
  if (gate.compileConfig) parts.push(gate.compileConfig);
  if (+gate.minPerfLevel > 0) parts.push('PERF_LEVEL>=' + gate.minPerfLevel);
  if (gate.environment) parts.push(gate.environment);
  if (gate.runtimeModules) parts.push(gate.runtimeModules);
  return parts.join(' && ') || 'always/catalog-only';
}
function renderHookCatalog() {
  var statusFilter = $('catalogStatusFilter').value;
  var domainFilter = $('catalogDomainFilter').value;
  var query = String(searchText || '').toLowerCase().trim();
  var queryTokens = query.split(/\s+/).filter(function (token) {
    return token && token.charAt(0) !== '>' && token.indexOf('thread:') !== 0;
  });
  var rows = hookCatalog.filter(function (row) {
    if (statusFilter !== 'all' && row.status !== statusFilter) return false;
    if (domainFilter !== 'all' && row.domain !== domainFilter) return false;
    var haystack = [
      row.id, row.domain, row.hookName, row.status, row.lastAttemptState,
      row.kind, row.timingRoot, row.nativeMode, row.customPhase,
      row.safetyClass, row.reason, catalogGateText(row)
    ].join(' ').toLowerCase();
    return queryTokens.every(function (token) {
      return haystack.indexOf(token) >= 0;
    });
  });
  rows.sort(function (a, b) {
    var domainOrder = String(a.domain || '').localeCompare(String(b.domain || ''));
    return domainOrder || String(a.id || '').localeCompare(String(b.id || ''));
  });

  var statusCounts = {};
  hookCatalog.forEach(function (row) {
    var state = row.status || 'NotEvaluated';
    statusCounts[state] = (statusCounts[state] || 0) + 1;
  });
  $('catalogSummary').innerHTML = Object.keys(statusCounts).sort()
    .map(function (state) {
      return '<span class="catalog-state ' + esc(state) + '">' +
        esc(state) + ' ' + statusCounts[state] + '</span>';
    }).join('');
  var mode = hookCatalogCoverage.mode || 'partial';
  $('catalogCoverage').textContent =
    rows.length + '/' + hookCatalog.length + ' visible · ' + mode +
    ' · complete=' + (!!hookCatalogCoverage.complete);

  var html = '';
  rows.forEach(function (row) {
    var last = row.lastAttemptState || 'NotEvaluated';
    if (+row.attemptCount > 0) last += ' · attempts=' + row.attemptCount;
    if (+row.minHookStatus !== 0) last += ' · MH=' + row.minHookStatus;
    var timing = (row.timingRoot || '—') + ' · native=' +
      (row.nativeMode || 'unresolved') + ' · custom=' +
      (row.customPhase || 'Unresolved');
    var address = 'RVA ' + (row.targetRva || '0x0');
    if (row.target && row.target !== '0x0') address += ' · target ' + row.target;
    html += '<tr>' +
      '<td class="num">' + esc(row.id || '—') + '</td>' +
      '<td><div>' + esc((row.domain || 'Unknown') + ' › ' +
        (row.hookName || 'UnnamedHook')) + '</div><span class="dim">' +
        esc(row.kind || '') + '</span></td>' +
      '<td><span class="catalog-state ' + esc(row.status || '') + '">' +
        esc(row.status || 'NotEvaluated') + '</span>' +
        (row.reason ? '<div class="dim">' + esc(row.reason) + '</div>' : '') +
        '</td>' +
      '<td class="num">' + esc(last) + '</td>' +
      '<td class="gate-text">' + esc(catalogGateText(row)) + '</td>' +
      '<td class="gate-text">' + esc(timing) + '</td>' +
      '<td class="gate-text">' + esc((row.safetyClass || 'Unclassified') +
        ' · ' + address) + '</td></tr>';
  });
  $('catalogBody').innerHTML = html;
}

// ---------- 视图 3：Call Tree（虚拟滚动） ----------
var treeExpanded = {};
var treeOnly5 = false;
var treeThread = 'all';
var treeRoot = null;
var flatRows = [];
var treeMatchSet = null;
var treeHighlightPath = null;

function nodeKey(n) { return n.path || '@root'; }
function buildTree(list) {
  var nodes = {};
  var root = { path: '', name: 'Frame', sec: null, children: [], parent: null,
    incl: frameMs, self: 0, calls: data.frameCount || 0, p95: null };
  list.forEach(function (s) {
    nodes[s.path] = {
      path: s.path, name: s.name || lastSeg(s.path), sec: s, children: [], parent: null,
      incl: s.avgCpuMs || 0,
      self: (s.avgSelfCpuMs != null ? s.avgSelfCpuMs : null),
      calls: s.callsPerFrame || 0,
      p95: (pcts[s.path] ? pcts[s.path].p95 : null)
    };
  });
  list.forEach(function (s) {
    var n = nodes[s.path];
    var p = s.parentPath;
    if (p && nodes[p]) { n.parent = nodes[p]; nodes[p].children.push(n); }
    else { n.parent = root; root.children.push(n); }
  });
  // threadSections 无 self 字段：inclusive 减子节点 inclusive 估算
  (function fixSelf(n) {
    var sum = 0;
    n.children.forEach(function (c) { fixSelf(c); sum += c.incl; });
    if (n.self == null) n.self = Math.max(0, n.incl - sum);
  })(root);
  (function sortRec(n) {
    n.children.sort(function (a, b) { return b.incl - a.incl; });
    n.children.forEach(sortRec);
  })(root);
  return root;
}
function rebuildTree() {
  var list = sections;
  if (treeThread !== 'all') {
    list = threadSections.filter(function (ts) { return String(ts.threadId) === treeThread; });
  }
  treeRoot = buildTree(list);
}
function flattenTree() {
  flatRows = [];
  if (!treeRoot) return;
  var fm = frameMs || 1;
  (function walk(n, depth) {
    if (depth > 0 && treeOnly5 && (n.incl / fm) < 0.05) return;
    flatRows.push({ n: n, depth: depth });
    if (depth === 0 || treeExpanded[nodeKey(n)]) {
      for (var i = 0; i < n.children.length; i++) walk(n.children[i], depth + 1);
    }
  })(treeRoot, 0);
}
function renderTreeWindow() {
  var scrollEl = $('treeScroll');
  var total = flatRows.length;
  $('treeSpacer').style.height = (total * ROW_H) + 'px';
  var start = Math.max(0, Math.floor(scrollEl.scrollTop / ROW_H) - 8);
  var end = Math.min(total, Math.ceil((scrollEl.scrollTop + scrollEl.clientHeight) / ROW_H) + 8);
  var fm = frameMs || 1;
  var html = '';
  for (var i = start; i < end; i++) {
    var r = flatRows[i], n = r.n;
    var open = (r.depth === 0) || !!treeExpanded[nodeKey(n)];
    var arrow = n.children.length ? (open ? '▾' : '▸') : '';
    var cls = 'tree-row';
    if (treeMatchSet && n.path && treeMatchSet[n.path]) cls += ' match';
    if (treeHighlightPath && n.path === treeHighlightPath) cls += ' located';
    if (n.sec && isUncoveredWall(n.sec)) cls += ' danger-row';
    html += '<div class="' + cls + '" data-path="' + esc(n.path) + '" data-idx="' + i + '"' +
      ' style="top:' + (i * ROW_H) + 'px;padding-left:' + (8 + r.depth * 16) + 'px">' +
      '<span class="arrow" data-arrow="1">' + arrow + '</span>' +
      '<span class="tname" title="' + esc(n.path) + '">' + esc(n.name) + '</span>' +
      barHtml(n.incl, fm, 'var(--cyan)') +
      '<span class="num dim">' + pctOf(n.incl, fm).toFixed(1) + '%</span>' +
      barHtml(n.self, fm, heatColor(n.self / fm)) +
      '<span class="num dim">' + pctOf(n.self, fm).toFixed(1) + '%</span>' +
      '<span class="num dim">' + fmtCalls(n.calls) + '</span>' +
      '<span class="num dim">' + (n.p95 != null ? fmtMs(n.p95) : '-') + '</span>' +
      '</div>';
  }
  $('treeRows').innerHTML = html;
  $('treeCount').textContent = total + ' 行';
}
function refreshTree() { flattenTree(); renderTreeWindow(); }
// 搜索时自动展开命中路径的祖先并高亮命中节点
function applyTreeSearch() {
  treeMatchSet = null;
  var q = parseSearch(searchText);
  if (searchActive(q) && treeRoot) {
    treeMatchSet = {};
    (function walk(n) {
      var matched = false;
      if (n.sec && matchSearch(n.sec, q)) { matched = true; treeMatchSet[n.path] = true; }
      var childMatched = false;
      n.children.forEach(function (c) { if (walk(c)) childMatched = true; });
      if (childMatched && n.path) treeExpanded[nodeKey(n)] = true;
      return matched || childMatched;
    })(treeRoot);
  }
  refreshTree();
}
function setTreeExpandMode(mode) {
  treeExpanded = {};
  if (mode === 'all') {
    (function walk(n) { if (n.path) treeExpanded[nodeKey(n)] = true; n.children.forEach(walk); })(treeRoot);
  } else if (mode === '2') {
    (function walk(n, depth) {
      if (n.path && depth <= 1) treeExpanded[nodeKey(n)] = true;
      n.children.forEach(function (c) { walk(c, depth + 1); });
    })(treeRoot, 0);
  }
  applyTreeSearch();
}
// 从 Hotspots 跳转：切到树视图、展开祖先链、滚动定位并高亮
function locateInTree(path) {
  switchView('tree');
  if (!treeRoot) return;
  var target = null;
  (function find(n) {
    if (target) return;
    if (n.path === path) { target = n; return; }
    n.children.forEach(find);
  })(treeRoot);
  if (target) {
    var p = target.parent;
    while (p && p.path) { treeExpanded[nodeKey(p)] = true; p = p.parent; }
  }
  treeHighlightPath = path;
  refreshTree();
  var idx = -1;
  for (var i = 0; i < flatRows.length; i++) {
    if (flatRows[i].n.path === path) { idx = i; break; }
  }
  if (idx >= 0) {
    $('treeScroll').scrollTop = Math.max(0, (idx - 4) * ROW_H);
    renderTreeWindow();
  }
}

// ---------- 视图 3：Timeline ----------
var tlSel = null; // [i0, i1]（帧下标，含端点）
var tlDrag = null;
function frameIndexFromX(x, W) {
  var n = frameSeries.length;
  if (!n) return 0;
  var i = Math.floor(x / (W / n));
  return Math.max(0, Math.min(n - 1, i));
}
function stageRowAt(frameIndex) {
  var frame = frameSeries[frameIndex];
  if (!frame) return null;
  var direct = stageSeries[frameIndex];
  if (direct && String(direct[0]) === String(frame[0])) return direct;
  return stageRowsByEpoch[String(frame[0])] || null;
}
function stageValueAt(frameIndex, column) {
  var columnIndex = stageColumnIndex[column];
  var row = stageRowAt(frameIndex);
  if (!row || columnIndex == null || columnIndex <= 0 ||
      columnIndex >= row.length || row[columnIndex] == null) {
    return null;
  }
  var value = +row[columnIndex];
  return Number.isFinite(value) ? value : null;
}
function selectedStageColumns() {
  var result = [], seen = {};
  for (var i = 0; i < 4; i++) {
    var select = $('stageSlot' + i);
    var column = select ? select.value : '';
    if (!column || seen[column]) continue;
    seen[column] = true;
    result.push(column);
  }
  return result.slice(0, 4);
}
function sortedStageValues(column, firstIndex, lastIndex) {
  var result = [];
  var first = Math.max(0, firstIndex == null ? 0 : firstIndex);
  var last = Math.min(
    frameSeries.length - 1,
    lastIndex == null ? frameSeries.length - 1 : lastIndex);
  for (var i = first; i <= last; i++) {
    var value = stageValueAt(i, column);
    if (value != null) result.push(value);
  }
  result.sort(function (a, b) { return a - b; });
  return result;
}
function quantileSorted(values, q) {
  if (!values.length) return null;
  var pos = Math.max(0, Math.min(1, q)) * (values.length - 1);
  var lo = Math.floor(pos), hi = Math.min(values.length - 1, lo + 1);
  var frac = pos - lo;
  return values[lo] * (1 - frac) + values[hi] * frac;
}
var stageGlobalP50Cache = {};
function stageGlobalP50(column) {
  if (Object.prototype.hasOwnProperty.call(stageGlobalP50Cache, column))
    return stageGlobalP50Cache[column];
  var result = quantileSorted(sortedStageValues(column), 0.5);
  stageGlobalP50Cache[column] = result;
  return result;
}
function initializeStageControls() {
  var available = [];
  for (var i = 1; i < stageColumns.length; i++) {
    var column = stageColumns[i];
    var metaRow = stageMetadataByColumn[column];
    if (!metaRow || metaRow.available !== false) available.push(column);
  }
  var preferred = [
    'worldPrepareMainInclMs',
    'renderSceneMainInclMs',
    'populateMainInclMs',
    'shadowCaptureMainOverlayMs'
  ];
  var defaults = [];
  preferred.forEach(function (column) {
    if (available.indexOf(column) >= 0 && defaults.length < 4)
      defaults.push(column);
  });
  available.forEach(function (column) {
    if (defaults.indexOf(column) < 0 && defaults.length < 4)
      defaults.push(column);
  });
  for (var slot = 0; slot < 4; slot++) {
    var select = $('stageSlot' + slot);
    if (!select) continue;
    var html = '<option value="">— 不显示 —</option>';
    available.forEach(function (column) {
      var metaRow = stageMetadataByColumn[column] || {};
      var suffix = metaRow.isOverlay ? ' · Overlay' : '';
      html += '<option value="' + esc(column) + '">' +
        esc(metaRow.label || column) + suffix + '</option>';
    });
    select.innerHTML = html;
    select.value = defaults[slot] || '';
  }
}
function renderStageTrends() {
  var cv = $('stageCanvas');
  if (!cv) return;
  var W = Math.max(200, cv.parentElement.clientWidth - 16), H = 210;
  var dpr = window.devicePixelRatio || 1;
  cv.width = W * dpr; cv.height = H * dpr;
  cv.style.width = W + 'px'; cv.style.height = H + 'px';
  var ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);

  var selected = selectedStageColumns();
  var n = frameSeries.length;
  if (!selected.length || !n || !stageSeries.length) {
    ctx.fillStyle = '#8b949e';
    ctx.fillText('无可用 stageSeries，或尚未选择曲线', 10, 20);
    $('stageLegend').innerHTML = '';
    return;
  }

  var plotted = [], maxY = stageMode === 'relative' ? 100 : 0;
  for (var s = 0; s < selected.length; s++) {
    var column = selected[s];
    var baseline = stageGlobalP50(column);
    var values = new Array(n);
    for (var i = 0; i < n; i++) {
      var raw = stageValueAt(i, column);
      var value = raw;
      if (stageMode === 'relative') {
        value = raw != null && baseline != null && baseline > 1e-9
          ? raw / baseline * 100 : null;
      }
      values[i] = value;
      if (value != null && value > maxY) maxY = value;
    }
    plotted.push({ column: column, baseline: baseline, values: values });
  }
  if (!(maxY > 0)) maxY = 1;
  var plotTop = 8, plotBottom = H - 18, plotH = plotBottom - plotTop;
  var bw = W / n;

  if (stageMode === 'relative') {
    var baselineY = plotBottom - Math.min(1, 100 / maxY) * plotH;
    ctx.strokeStyle = '#30363d';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, baselineY);
    ctx.lineTo(W, baselineY);
    ctx.stroke();
    ctx.fillStyle = '#8b949e';
    ctx.fillText('P50 100%', 6, Math.max(11, baselineY - 3));
  }

  for (var p = 0; p < plotted.length; p++) {
    var series = plotted[p].values;
    ctx.strokeStyle = STAGE_COLORS[p % STAGE_COLORS.length];
    ctx.lineWidth = 1.25;
    ctx.beginPath();
    var open = false;
    for (var j = 0; j < n; j++) {
      var sample = series[j];
      if (sample == null) {
        open = false;
        continue;
      }
      var x = j * bw + bw / 2;
      var y = plotBottom - Math.max(0, sample) / maxY * plotH;
      if (!open) {
        ctx.moveTo(x, y);
        open = true;
      } else {
        ctx.lineTo(x, y);
      }
    }
    ctx.stroke();
  }

  if (tlSel) {
    var i0 = Math.min(tlSel[0], tlSel[1]);
    var i1 = Math.max(tlSel[0], tlSel[1]);
    ctx.fillStyle = 'rgba(57,197,207,.12)';
    ctx.fillRect(i0 * bw, 0, (i1 - i0 + 1) * bw, H);
  }

  var legend = '';
  for (var l = 0; l < plotted.length; l++) {
    var metaRow = stageMetadataByColumn[plotted[l].column] || {};
    legend += '<span class="stage-legend-item"><i class="stage-swatch" style="background:' +
      STAGE_COLORS[l % STAGE_COLORS.length] + '"></i>' +
      esc(metaRow.label || plotted[l].column) +
      (metaRow.isOverlay ? ' · Overlay' : '') +
      ' · P50 ' + fmtOptionalMs(plotted[l].baseline) + ' ms</span>';
  }
  $('stageLegend').innerHTML = legend;
}
function renderStageSelectionStats() {
  var host = $('stageSelectionStats');
  if (!host) return;
  var selected = selectedStageColumns();
  if (!tlSel || !selected.length) {
    host.innerHTML =
      '<span class="dim">在上方帧时间线框选窗口后，这里会用逐帧 stageSeries 精确计算均值、P50 与 P95。</span>';
    return;
  }
  var i0 = Math.min(tlSel[0], tlSel[1]);
  var i1 = Math.max(tlSel[0], tlSel[1]);
  var html = '<div class="table-wrap"><table class="tbl"><thead><tr>' +
    '<th>Stage</th><th class="num">样本</th><th class="num">均值 ms</th>' +
    '<th class="num">P50 ms</th><th class="num">P95 ms</th>' +
    '<th class="num">均值 / 全局 P50</th></tr></thead><tbody>';
  for (var i = 0; i < selected.length; i++) {
    var column = selected[i];
    var values = sortedStageValues(column, i0, i1);
    var sum = 0;
    for (var j = 0; j < values.length; j++) sum += values[j];
    var mean = values.length ? sum / values.length : null;
    var p50 = quantileSorted(values, 0.5);
    var p95 = quantileSorted(values, 0.95);
    var globalP50 = stageGlobalP50(column);
    var relative = mean != null && globalP50 != null && globalP50 > 1e-9
      ? mean / globalP50 * 100 : null;
    var metaRow = stageMetadataByColumn[column] || {};
    html += '<tr><td><span class="stage-swatch" style="background:' +
      STAGE_COLORS[i % STAGE_COLORS.length] + '"></span> ' +
      esc(metaRow.label || column) +
      (metaRow.isOverlay ? ' <span class="dim">Overlay</span>' : '') +
      '</td><td class="num">' + values.length +
      '</td><td class="num">' + fmtOptionalMs(mean) +
      '</td><td class="num">' + fmtOptionalMs(p50) +
      '</td><td class="num">' + fmtOptionalMs(p95) +
      '</td><td class="num">' +
      (relative == null ? '—' : relative.toFixed(1) + '%') +
      '</td></tr>';
  }
  host.innerHTML = html + '</tbody></table></div>';
}
function renderTimeline() {
  var cv = $('timelineCanvas');
  var wrap = cv.parentElement;
  var W = Math.max(200, wrap.clientWidth - 16), H = 260;
  var dpr = window.devicePixelRatio || 1;
  cv.width = W * dpr; cv.height = H * dpr;
  cv.style.width = W + 'px'; cv.style.height = H + 'px';
  var ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, W, H);
  var n = frameSeries.length;
  if (!n) {
    ctx.fillStyle = '#8b949e';
    ctx.fillText('无 frameSeries 数据', 12, 20);
    renderStageTrends();
    return;
  }
  var maxV = 1, maxGpu = 0, i, k;
  var totals = new Array(n);
  for (i = 0; i < n; i++) {
    totals[i] = frameSeries[i][1] || 0;
    if (totals[i] > maxV) maxV = totals[i];
    var laneTotal = frameSeries[i][7] ||
      ((frameSeries[i][3] || 0) + (frameSeries[i][4] || 0) +
       (frameSeries[i][5] || 0) + (frameSeries[i][6] || 0));
    if (laneTotal > maxV) maxV = laneTotal;
    if ((frameSeries[i][2] || 0) > maxGpu) maxGpu = frameSeries[i][2] || 0;
  }
  var sorted = totals.slice().sort(function (a, b) { return a - b; });
  var p95v = sorted[Math.min(n - 1, Math.floor(n * 0.95))];
  var bw = W / n, plotH = H - 24;
  var segDef = [[3, LANE_COLORS.main], [4, LANE_COLORS.cs], [5, LANE_COLORS.worker], [6, LANE_COLORS.other]];
  for (i = 0; i < n; i++) {
    var r = frameSeries[i], x = i * bw, y = H;
    for (k = 0; k < 4; k++) {
      var v = r[segDef[k][0]] || 0;
      if (v <= 0) continue;
      var h = v / maxV * plotH;
      y -= h;
      ctx.fillStyle = segDef[k][1];
      ctx.fillRect(x, y, Math.max(0.6, bw - 0.4), h);
    }
    if (totals[i] > p95v) {
      ctx.fillStyle = '#f85149';
      ctx.fillRect(x, 0, Math.max(0.6, bw - 0.4), 3);
    }
  }
  // GPU 折线（独立刻度）
  if (maxGpu > 0) {
    ctx.strokeStyle = '#e6edf3';
    ctx.lineWidth = 1;
    ctx.beginPath();
    var gpuSegmentOpen = false;
    for (i = 0; i < n; i++) {
      if (frameSeries[i][2] == null) {
        gpuSegmentOpen = false;
        continue;
      }
      var gy = H - (+frameSeries[i][2] || 0) / maxGpu * plotH;
      if (!gpuSegmentOpen) {
        ctx.moveTo(i * bw + bw / 2, gy);
        gpuSegmentOpen = true;
      } else {
        ctx.lineTo(i * bw + bw / 2, gy);
      }
    }
    ctx.stroke();
  }
  // 框选覆盖层
  if (tlSel) {
    var i0 = Math.min(tlSel[0], tlSel[1]), i1 = Math.max(tlSel[0], tlSel[1]);
    ctx.fillStyle = 'rgba(57,197,207,.15)';
    ctx.fillRect(i0 * bw, 0, (i1 - i0 + 1) * bw, H);
  }
  // 拖拽中的临时选区
  if (tlDrag) {
    var dx0 = Math.min(tlDrag.x0, tlDrag.x1), dx1 = Math.max(tlDrag.x0, tlDrag.x1);
    ctx.fillStyle = 'rgba(210,153,34,.18)';
    ctx.fillRect(dx0, 0, Math.max(1, dx1 - dx0), H);
  }
  renderStageTrends();
}
function showTimelineSelection() {
  if (!tlSel) {
    $('timelineInfo').textContent =
      '框选一段帧查看聚合；点击帧查看数值。堆叠条是 Process CPU 预算拆分（Main 实测，其余为估算/余量），不是 Frame wall 的组成。';
    $('timelineHotspots').innerHTML = '';
    renderStageSelectionStats();
    return;
  }
  var i0 = Math.min(tlSel[0], tlSel[1]), i1 = Math.max(tlSel[0], tlSel[1]);
  var sum = 0, gpu = 0, gpuCount = 0;
  for (var i = i0; i <= i1 && i < frameSeries.length; i++) {
    sum += frameSeries[i][1] || 0;
    if (frameSeries[i][2] != null) {
      gpu += +frameSeries[i][2] || 0;
      gpuCount++;
    }
  }
  var cnt = i1 - i0 + 1;
  $('timelineInfo').textContent = '选中帧窗口：#' + i0 + ' ~ #' + i1 + '（' + cnt + ' 帧，均值 Frame wall ' +
    fmtMs(sum / cnt) + ' ms / GPU ' +
    (gpuCount ? fmtMs(gpu / gpuCount) + ' ms（n=' + gpuCount + '）' : '—') +
    '）—— Stage Trends 使用逐帧归档精确重算；Scope hotspots 下表仍为全局聚合。';
  renderStageSelectionStats();
  var rows = sections.filter(function (s) {
    return !isUncoveredWall(s) && !isDetachedPhaseWall(s);
  }).sort(function (a, b) {
    return (b.avgSelfCpuMs || 0) - (a.avgSelfCpuMs || 0);
  }).slice(0, 10);
  var html = '<div class="table-wrap"><table class="tbl"><thead><tr>' +
    '<th class="num">#</th><th>名称</th><th class="num">Self wall ms</th><th class="num">Incl wall ms</th>' +
    '</tr></thead><tbody>';
  rows.forEach(function (s, idx) {
    html += '<tr data-path="' + esc(s.path || '') + '">' +
      '<td class="num dim">' + (idx + 1) + '</td>' +
      '<td class="namecell" title="' + esc(s.path || '') + '">' + esc(hotspotDisplayName(s)) + '</td>' +
      '<td class="num">' + fmtMs(s.avgSelfCpuMs) + '</td>' +
      '<td class="num">' + fmtMs(s.avgCpuMs) + '</td></tr>';
  });
  $('timelineHotspots').innerHTML = html + '</tbody></table></div>';
}
function bindTimeline() {
  var cv = $('timelineCanvas');
  var tip = $('timelineTip');
  cv.addEventListener('mousedown', function (e) {
    tlDrag = { x0: e.offsetX, x1: e.offsetX };
    tlSel = null;
    tip.hidden = true;
    showTimelineSelection();
    renderTimeline();
  });
  window.addEventListener('mousemove', function (e) {
    if (!tlDrag) return;
    var rect = cv.getBoundingClientRect();
    tlDrag.x1 = Math.max(0, Math.min(rect.width, e.clientX - rect.left));
    renderTimeline();
  });
  window.addEventListener('mouseup', function (e) {
    if (!tlDrag) return;
    var rect = cv.getBoundingClientRect();
    var x0 = tlDrag.x0, x1 = Math.max(0, Math.min(rect.width, e.clientX - rect.left));
    tlDrag = null;
    if (Math.abs(x1 - x0) >= 4) {
      tlSel = [frameIndexFromX(x0, rect.width), frameIndexFromX(x1, rect.width)];
      renderTimeline();
      showTimelineSelection();
    } else {
      var idx = frameIndexFromX(x0, rect.width);
      var r = frameSeries[idx];
      if (r) {
        var processCpu = r[7] ||
          ((r[3] || 0) + (r[4] || 0) + (r[5] || 0) + (r[6] || 0));
        var stageTip = '';
        var selectedStages = selectedStageColumns();
        for (var si = 0; si < selectedStages.length; si++) {
          var stageColumn = selectedStages[si];
          var stageMeta = stageMetadataByColumn[stageColumn] || {};
          stageTip += '<br><span style="color:' +
            STAGE_COLORS[si % STAGE_COLORS.length] + '">' +
            esc(stageMeta.label || stageColumn) + ' ' +
            fmtOptionalMs(stageValueAt(idx, stageColumn)) + ' ms</span>';
        }
        tip.hidden = false;
        tip.style.left = Math.min(rect.width - 10, x0 + 16) + 'px';
        tip.style.top = '24px';
        tip.innerHTML = '帧 #' + idx + '（epoch ' + (r[0] || 0) + '）<br>' +
          'Frame wall ' + fmtMs(r[1]) + ' ms / Process CPU ' +
          fmtMs(processCpu) + ' ms / GPU ' + fmtOptionalMs(r[2]) +
          (r[2] == null ? '' : ' ms') + '<br>' +
          '<span style="color:' + LANE_COLORS.main + '">Main ' + fmtMs(r[3]) + '</span> / ' +
          '<span style="color:' + LANE_COLORS.cs + '">CS est. ' + fmtMs(r[4]) + '</span> / ' +
          '<span style="color:' + LANE_COLORS.worker + '">Worker residual ' + fmtMs(r[5]) + '</span> / ' +
          '<span style="color:' + LANE_COLORS.other + '">Other est. ' + fmtMs(r[6]) + '</span>' +
          stageTip;
      }
      renderTimeline();
    }
  });
  $('timelineHotspots').addEventListener('click', function (e) {
    var tr = e.target.closest('tr');
    if (tr && tr.getAttribute('data-path')) locateInTree(tr.getAttribute('data-path'));
  });
  for (var slot = 0; slot < 4; slot++) {
    (function (select) {
      if (!select) return;
      select.addEventListener('change', function () {
        stageGlobalP50Cache = {};
        renderStageTrends();
        renderStageSelectionStats();
      });
    })($('stageSlot' + slot));
  }
  var stageModeButtons =
    document.querySelectorAll('[data-stage-mode]');
  for (var sm = 0; sm < stageModeButtons.length; sm++) {
    (function (btn) {
      btn.addEventListener('click', function () {
        stageMode = btn.getAttribute('data-stage-mode') || 'relative';
        for (var j = 0; j < stageModeButtons.length; j++) {
          stageModeButtons[j].classList.toggle(
            'on',
            stageModeButtons[j].getAttribute('data-stage-mode') ===
              stageMode);
        }
        renderStageTrends();
      });
    })(stageModeButtons[sm]);
  }
}

// ---------- 视图 4：Swimlanes ----------
var SWIM_LANES = [
  { key: 'main', idx: 3, label: 'MainThread OS CPU (measured)' },
  { key: 'cs', idx: 4, label: 'CS CPU (scope-weighted estimate)' },
  { key: 'worker', idx: 5, label: 'Worker CPU (unclassified residual)' },
  { key: 'other', idx: 6, label: 'Other CPU (scope-weighted estimate)' }
];
function renderSwimlanes() {
  var host = $('swimlaneRows');
  host.innerHTML = '';
  var n = frameSeries.length;
  if (!n) {
    host.innerHTML = '<p class="dim">无 frameSeries 数据</p>';
    return;
  }
  SWIM_LANES.forEach(function (lane) {
    var max = 0, i;
    for (i = 0; i < n; i++) max = Math.max(max, frameSeries[i][lane.idx] || 0);
    var row = document.createElement('div');
    row.className = 'lane-row';
    var label = document.createElement('div');
    label.className = 'lane-label';
    label.innerHTML = '<span style="color:' + LANE_COLORS[lane.key] + '">' + lane.label + '</span>' +
      '<span class="dim num">max ' + fmtMs(max) + ' ms</span>';
    var cv = document.createElement('canvas');
    row.appendChild(label);
    row.appendChild(cv);
    host.appendChild(row);
    var W = Math.max(200, row.clientWidth - 16), H = 72;
    var dpr = window.devicePixelRatio || 1;
    cv.width = W * dpr; cv.height = H * dpr;
    cv.style.width = W + 'px'; cv.style.height = H + 'px';
    var ctx = cv.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    var bw = W / n, scale = max > 0 ? (H - 4) / max : 0;
    ctx.fillStyle = LANE_COLORS[lane.key];
    for (i = 0; i < n; i++) {
      var v = frameSeries[i][lane.idx] || 0;
      if (v <= 0) continue;
      var h = Math.max(1, v * scale);
      ctx.fillRect(i * bw, H - h, Math.max(0.6, bw - 0.4), h);
    }
  });
}

// ---------- Legacy 折叠区 ----------
function renderLegacy() {
  var consumed = ['schemaVersion', 'frameCount', 'windowSec', 'meta', 'avgFps', 'avgFrameTimeMs',
    'avgGpuTimeMs', 'gpuSamples', 'avgProcessCpuMs', 'avgMainThreadCpuMs', 'avgWorkerThreadsCpuMs',
    'avgIdleWaitCpuMs', 'avgTrackedActiveCpuMs', 'avgUntrackedActiveCpuMs',
    'avgUnattributedCpuMs', 'avgTrackedAdditiveRootWallMs',
    'avgAllThreadAdditiveRootWallMsDiagnostic', 'allThreadRootWallDiagnostic',
    'avgUncoveredFrameWallMs', 'frameWallScopeCoveragePct',
    'frameWallAttribution', 'legacyWallGapAliases',
    'cpuCoveragePct',
    'coverageWarning', 'frameWallCoverageWarning', 'sections',
    'hookCatalog', 'hookCatalogCoverage', 'hookInventory',
    'threadSections', 'threadLanes', 'threadLaneContract', 'frameSeriesColumns',
    'frameSeries', 'stageSeriesColumns', 'stageSeriesContract',
    'stageSeriesMetadata', 'stageSeries', 'frameStateAnalysis',
    'workloadSeriesColumns',
    'workloadSeries', 'sectionPercentiles', 'mainThreadId', 'mainThreadAuthority',
    'mainThreadCpuClampFrames', 'mainThreadProbeDiagnostics'];
  var legacy = Object.keys(data).filter(function (k) { return consumed.indexOf(k) < 0; });
  $('legacyKeys').textContent = legacy.length ? legacy.join(', ') : '(无)';
}

// ---------- 视图切换与全局绑定 ----------
var currentView = 'hotspots';
var treeBuilt = false;
function switchView(v) {
  currentView = v;
  var btns = document.querySelectorAll('#tabs button');
  for (var i = 0; i < btns.length; i++) {
    btns[i].classList.toggle('active', btns[i].getAttribute('data-view') === v);
  }
  var views = document.querySelectorAll('.view');
  for (var j = 0; j < views.length; j++) {
    views[j].classList.toggle('active', views[j].id === 'view-' + v);
  }
  if (v === 'tree' && !treeBuilt) {
    treeBuilt = true;
    rebuildTree();
    applyTreeSearch();
  }
  if (v === 'states') renderStateAttribution();
  if (v === 'hooks') renderHookBreakdown();
  if (v === 'catalog') renderHookCatalog();
  if (v === 'timeline') { renderTimeline(); showTimelineSelection(); }
  if (v === 'swimlanes') { renderSwimlanes(); }
}
function bindGlobal() {
  var tabBtns = document.querySelectorAll('#tabs button');
  for (var i = 0; i < tabBtns.length; i++) {
    (function (btn) {
      btn.addEventListener('click', function () { switchView(btn.getAttribute('data-view')); });
    })(tabBtns[i]);
  }
  $('globalSearch').addEventListener('input', function () {
    searchText = this.value || '';
    renderHotspots();
    renderStateAttribution();
    renderHookBreakdown();
    renderHookCatalog();
    if (treeBuilt) applyTreeSearch();
  });
  $('kpiToggle').addEventListener('click', function () {
    var bar = $('kpiBar');
    bar.hidden = !bar.hidden;
    this.textContent = bar.hidden ? 'KPI ▸' : 'KPI ▾';
  });
  // Hotspots 行点击 -> 跳转 Call Tree 定位
  $('hotspotBody').addEventListener('click', function (e) {
    var tr = e.target.closest('tr');
    if (tr && tr.getAttribute('data-path')) locateInTree(tr.getAttribute('data-path'));
  });
  $('hookBody').addEventListener('click', function (e) {
    var tr = e.target.closest('tr');
    if (tr && tr.getAttribute('data-path')) locateInTree(tr.getAttribute('data-path'));
  });
  $('statePathBody').addEventListener('click', function (e) {
    var tr = e.target.closest('tr');
    if (tr && tr.getAttribute('data-path'))
      locateInTree(tr.getAttribute('data-path'));
  });
  $('stateOverlayBody').addEventListener('click', function (e) {
    var tr = e.target.closest('tr');
    if (tr && tr.getAttribute('data-path'))
      locateInTree(tr.getAttribute('data-path'));
  });
  var stateModeButtons =
    document.querySelectorAll('#stateModeChips [data-state-mode]');
  for (var sm = 0; sm < stateModeButtons.length; sm++) {
    (function (btn) {
      btn.addEventListener('click', function () {
        stateCohortMode = btn.getAttribute('data-state-mode') ||
          'instantaneous';
        for (var j = 0; j < stateModeButtons.length; j++) {
          stateModeButtons[j].classList.toggle(
            'on',
            stateModeButtons[j].getAttribute('data-state-mode') ===
              stateCohortMode);
        }
        renderStateAttribution();
      });
    })(stateModeButtons[sm]);
  }
  $('hookSort').addEventListener('change', renderHookBreakdown);
  $('hookHotOnly').addEventListener('click', function () {
    hookHotOnly = !hookHotOnly;
    this.classList.toggle('on', hookHotOnly);
    renderHookBreakdown();
  });
  $('catalogStatusFilter').addEventListener('change', renderHookCatalog);
  $('catalogDomainFilter').addEventListener('change', renderHookCatalog);
  // Hotspots 快捷筛选 chips
  var chips = document.querySelectorAll('#hotspotChips .chip[data-chip]');
  for (var c = 0; c < chips.length; c++) {
    (function (btn) {
      btn.addEventListener('click', function () {
        var k = btn.getAttribute('data-chip');
        hsChips[k] = !hsChips[k];
        btn.classList.toggle('on', hsChips[k]);
        renderHotspots();
      });
    })(chips[c]);
  }
  // Call Tree 工具栏
  $('treeExpandAll').addEventListener('click', function () { setTreeExpandMode('all'); });
  $('treeCollapseAll').addEventListener('click', function () { setTreeExpandMode('none'); });
  $('treeCollapse2').addEventListener('click', function () { setTreeExpandMode('2'); });
  $('treeGt5').addEventListener('click', function () {
    treeOnly5 = !treeOnly5;
    this.classList.toggle('on', treeOnly5);
    refreshTree();
  });
  $('treeThreadSel').addEventListener('change', function () {
    treeThread = this.value;
    treeExpanded = {};
    rebuildTree();
    applyTreeSearch();
  });
  $('treeScroll').addEventListener('scroll', renderTreeWindow);
  $('treeRows').addEventListener('click', function (e) {
    var rowEl = e.target.closest('.tree-row');
    if (!rowEl) return;
    var idx = +rowEl.getAttribute('data-idx');
    var r = flatRows[idx];
    if (!r) return;
    if (e.target.hasAttribute('data-arrow')) {
      if (r.depth > 0) {
        var k = nodeKey(r.n);
        treeExpanded[k] = !treeExpanded[k];
        refreshTree();
      }
    } else {
      treeHighlightPath = r.n.path || null;
      renderTreeWindow();
    }
  });
  bindTimeline();
  var resizeTimer = null;
  window.addEventListener('resize', function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      if (currentView === 'timeline') renderTimeline();
      if (currentView === 'swimlanes') renderSwimlanes();
      if (currentView === 'tree') renderTreeWindow();
    }, 120);
  });
}
function renderThreadSel() {
  var sel = $('treeThreadSel');
  var html = '<option value="all">All 线程</option>';
  threadOrder.forEach(function (t) {
    var lane = laneOfThread(t);
    html += '<option value="' + esc(t) + '">' + LANE_LABEL[lane] + ' (tid ' + esc(t) + ')</option>';
  });
  sel.innerHTML = html;
}

renderHeader();
renderKpis();
renderHotspots();
renderStateAttribution();
renderHookBreakdown();
initializeHookCatalogControls();
renderHookCatalog();
renderThreadSel();
initializeStageControls();
renderLegacy();
bindGlobal();
})();
    </script>
</body>
</html>)PERFTPL";

} // namespace dxvk::war3
