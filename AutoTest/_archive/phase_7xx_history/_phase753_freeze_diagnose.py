#!/usr/bin/env python3
"""Phase 7.53: 深度对比 active vs frozen 段所有 cadence 字段。"""
import json
import sys

def main(path):
    trace_idx = 0
    last_ds = None
    freeze_run = 0
    freeze_groups = []
    all_records = []
    for line in open(path,'r',encoding='utf-8'):
        line=line.strip()
        if not line: continue
        try: obj=json.loads(line)
        except: continue
        if obj.get('type')!='shadowPoseFullTraceFrame': continue
        cad = obj.get('cadence',{})
        ds = str(cad.get('dynamicPoseSignature', ''))
        all_records.append((trace_idx, cad, obj.get('keyStats',{})))
        if ds == last_ds:
            freeze_run += 1
        else:
            if freeze_run >= 2:
                freeze_groups.append((trace_idx-1-freeze_run, trace_idx-1, freeze_run+1))
            freeze_run = 0
        last_ds = ds
        trace_idx += 1

    print(f'Total trace frames: {trace_idx}')
    print(f'Total dynamicPoseSignature freeze groups (>=3 frames): {len(freeze_groups)}')
    print(f'First 5 groups: {freeze_groups[:5]}')

    if not freeze_groups:
        return

    s, e, l = freeze_groups[1]
    print(f'\nGroup #2 covers trace[{s}..{e}] len={l}')

    ai = max(0, s-1)
    fi = (s+e)//2
    print(f'\n=== ACTIVE frame trace[{ai}] ===')
    cad_a = all_records[ai][1]
    ks_a = all_records[ai][2]
    print(f'  ds={cad_a.get("dynamicPoseSignature")}')
    print(f'  ph={cad_a.get("lastSubmittedPaletteHash")}')
    print(f'  smr={cad_a.get("shadowMapRenderSerial")}')
    print(f'  matKey={cad_a.get("shadowMatrixSceneKey")}')
    print(f'  uploadSer={cad_a.get("shadowMatrixUploadSerial")}')
    print(f'  ssub={cad_a.get("submittedSkinnedCount")}')
    print(f'  shadowMapDrawn={cad_a.get("shadowMapDrawnCasters")}')
    print(f'  recvDraw={cad_a.get("receiverDrawExecutedThisFrame")}')
    print(f'  shadowExec={cad_a.get("shadowMapExecutedThisFrame")}')
    print(f'  receiverReuse={cad_a.get("receiverReuseShadowMap")}')
    # keyStats
    print(f'  combinedHash={ks_a.get("semanticSceneSubmittedSkinnedPaletteCombinedHash")}')
    print(f'  distinct={ks_a.get("semanticSceneSubmittedSkinnedPaletteDistinctSampleCount")}')
    print(f'  consecMax={ks_a.get("semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax")}')
    print(f'  rebuildAttempt={ks_a.get("submitLiveRebuildAttemptCount")}')
    print(f'  rebuildHit={ks_a.get("submitLiveRebuildHitCount")}')
    print(f'  rebuildApplied={ks_a.get("submitLiveRebuildAppliedCount")}')
    print(f'  snapCap={ks_a.get("renderablePartPaletteSnapshotCapturedCount")}')
    print(f'  snapQH={ks_a.get("renderablePartPaletteSnapshotQueryHitCount")}')
    print(f'  bindQH={ks_a.get("renderablePartPaletteBindingQueryHitCount")}')
    print(f'  mwCount={ks_a.get("runtimeMatrixWriteCount")}')
    print(f'  gpwCount={ks_a.get("runtimeGroupPaletteWrapperCallCount")}')

    print(f'\n=== FROZEN frame trace[{fi}] ===')
    cad_f = all_records[fi][1]
    ks_f = all_records[fi][2]
    print(f'  ds={cad_f.get("dynamicPoseSignature")}')
    print(f'  ph={cad_f.get("lastSubmittedPaletteHash")}')
    print(f'  smr={cad_f.get("shadowMapRenderSerial")}')
    print(f'  matKey={cad_f.get("shadowMatrixSceneKey")}')
    print(f'  uploadSer={cad_f.get("shadowMatrixUploadSerial")}')
    print(f'  ssub={cad_f.get("submittedSkinnedCount")}')
    print(f'  shadowMapDrawn={cad_f.get("shadowMapDrawnCasters")}')
    print(f'  recvDraw={cad_f.get("receiverDrawExecutedThisFrame")}')
    print(f'  shadowExec={cad_f.get("shadowMapExecutedThisFrame")}')
    print(f'  receiverReuse={cad_f.get("receiverReuseShadowMap")}')
    print(f'  combinedHash={ks_f.get("semanticSceneSubmittedSkinnedPaletteCombinedHash")}')
    print(f'  distinct={ks_f.get("semanticSceneSubmittedSkinnedPaletteDistinctSampleCount")}')
    print(f'  consecMax={ks_f.get("semanticSceneSubmittedSkinnedPaletteConsecutiveSameHashCountMax")}')
    print(f'  rebuildAttempt={ks_f.get("submitLiveRebuildAttemptCount")}')
    print(f'  rebuildHit={ks_f.get("submitLiveRebuildHitCount")}')
    print(f'  rebuildApplied={ks_f.get("submitLiveRebuildAppliedCount")}')
    print(f'  snapCap={ks_f.get("renderablePartPaletteSnapshotCapturedCount")}')
    print(f'  snapQH={ks_f.get("renderablePartPaletteSnapshotQueryHitCount")}')
    print(f'  bindQH={ks_f.get("renderablePartPaletteBindingQueryHitCount")}')
    print(f'  mwCount={ks_f.get("runtimeMatrixWriteCount")}')
    print(f'  gpwCount={ks_f.get("runtimeGroupPaletteWrapperCallCount")}')

    # 跨整组冻结期间的 counter 增量
    if l >= 2:
        first_in = all_records[s][2]
        last_in = all_records[e][2]
        print(f'\n=== Counter delta over frozen group trace[{s}..{e}] ===')
        for k in ['runtimeMatrixWriteCount', 'runtimeGroupPaletteWrapperCallCount',
                  'renderablePartPaletteSnapshotCapturedCount',
                  'renderablePartPaletteSnapshotQueryHitCount',
                  'renderablePartPaletteBindingQueryHitCount',
                  'submitLiveRebuildHitCount', 'submitLiveRebuildAppliedCount',
                  'currentDrawContractPublishReadyCount',
                  'currentDrawContractPublishMissInvalidPaletteSlot',
                  'paletteCaptureTrustedSourceHitCount',
                  'paletteCaptureFrameTagMismatchMissCount']:
            v0 = first_in.get(k, 0)
            v1 = last_in.get(k, 0)
            print(f'  {k}: {v0} -> {v1} delta={v1-v0}')

if __name__ == '__main__':
    main(sys.argv[1])
