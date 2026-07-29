'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const repoRoot = path.resolve(__dirname, '..');
const templatePath = path.join(
  repoRoot, 'src', 'd3d9', 'war3', 'tools',
  'war3_perf_report_template.h');
const source = fs.readFileSync(templatePath, 'utf8');
const beginMarker = 'var HOOK_ROOT_SEMANTICS = {';
const endMarker = 'function renderHookBreakdown() {';
const begin = source.indexOf(beginMarker);
const end = source.indexOf(endMarker, begin);
assert(begin >= 0 && end > begin, 'Hook Breakdown source block not found');

const context = {
  sections: [],
  hookInventory: [],
  lastSeg(value) {
    const parts = String(value || '').split('/');
    return parts[parts.length - 1] || '';
  },
  hookContextLabel(value) {
    return String(value || '');
  },
  normalizeHookIdentity(value) {
    return String(value || '').toLowerCase().replace(/[^a-z0-9]/g, '');
  },
  hookInventoryTimingAlias(inv) {
    return inv && inv.hookName ? inv.hookName : '';
  },
};
vm.createContext(context);
vm.runInContext(source.slice(begin, end), context, {
  filename: 'war3_perf_report_template.h#HookBreakdown',
});

function section(name, parentPath, avgCpuMs, avgSelfCpuMs, callsPerFrame) {
  const pathValue = parentPath ? parentPath + '/' + name : name;
  return {
    name,
    path: pathValue,
    parentPath: parentPath || '',
    avgCpuMs,
    avgSelfCpuMs,
    callsPerFrame,
  };
}

const present = section('DXVK_D3D9_PresentEx', 'Frame', 0.18, 0.18, 1);
const shadow = section(
  'WarVKCallback_ShadowCapture', 'Frame/Hook_WorldRenderScene',
  0.75, 0, 55);
const shadowCustom = section(
  'WarVKCallbackLogic', shadow.path, 0.75, 0.75, 55);
const normal = section('Hook_WorldRenderScene', 'Frame', 3.0, 0.6, 1);
const normalNative = section(
  'NativeOriginalInclusive', normal.path, 2.4, 1.6, 1);
const normalCustom = section('WarVKHookLogic', normal.path, 0.6, 0.6, 1);
const observer = section(
  'Hook_RenderQueue_StageUpdate', 'Frame/Hook_WorldRenderScene',
  1.1, 0.15, 8);
const observerNative = section(
  'NativeOriginalInclusive', observer.path, 0.95, 0.9, 8);
const observerCustom = section(
  'ObserverOverhead', observer.path, 0.15, 0.15, 8);

context.sections = [
  present,
  shadow,
  shadowCustom,
  normal,
  normalNative,
  normalCustom,
  observer,
  observerNative,
  observerCustom,
];
const rows = context.collectHookBreakdownRows();
function row(name) {
  const found = rows.find((item) => item.sec.name === name);
  assert(found, `missing Hook Breakdown row: ${name}`);
  return found;
}
function close(actual, expected, label) {
  assert(Math.abs(actual - expected) < 1e-9,
    `${label}: expected ${expected}, got ${actual}`);
}

const presentRow = row('DXVK_D3D9_PresentEx');
assert.strictEqual(presentRow.customPhase, 'DXVKFrontendLogic');
assert.strictEqual(presentRow.noNativeByDesign, true);
assert.strictEqual(presentRow.split, true);
close(presentRow.warvk, 0.18, 'PresentEx custom');
close(presentRow.nativeIncl, 0, 'PresentEx native inclusive');
close(presentRow.nativeSelf, 0, 'PresentEx native self');
close(presentRow.nested, 0, 'PresentEx native nested');
assert.match(context.hookSplitStatusText(presentRow),
  /无原生 trampoline，闭合/);

const shadowRow = row('WarVKCallback_ShadowCapture');
assert.strictEqual(shadowRow.customPhase, 'WarVKCallbackLogic');
assert.strictEqual(shadowRow.noNativeByDesign, true);
close(shadowRow.warvk, 0.75, 'ShadowCapture custom');
close(shadowRow.nativeIncl, 0, 'ShadowCapture native inclusive');

const normalRow = row('Hook_WorldRenderScene');
assert.strictEqual(normalRow.customPhase, 'WarVKHookLogic');
assert.strictEqual(normalRow.noNativeByDesign, false);
close(normalRow.warvk, 0.6, 'normal Hook custom');
close(normalRow.nativeIncl, 2.4, 'normal Hook native inclusive');
close(normalRow.nativeSelf, 1.6, 'normal Hook native self');
close(normalRow.nested, 0.8, 'normal Hook native nested');

const observerRow = row('Hook_RenderQueue_StageUpdate');
assert.strictEqual(observerRow.customPhase, 'ObserverOverhead');
assert.strictEqual(observerRow.observerOnly, true);
close(observerRow.warvk, 0.15, 'Observer overhead');
close(observerRow.nativeIncl, 0.95, 'Observer native inclusive');
close(observerRow.nativeSelf, 0.9, 'Observer native self');
close(observerRow.nested, 0.05, 'Observer native nested');

console.log('Hook Breakdown mock smoke: 4 cases passed');
