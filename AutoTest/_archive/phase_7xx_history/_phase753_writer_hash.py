#!/usr/bin/env python3
"""Phase 7.53: 看 0x12E600 / 0x12FDC0 producer hook 的 LastMatrixHash 在 frozen 段是否冻结。
如果它们冻结 = War3 引擎本身骨骼 logic 不更新；如果它们变化但 CombinedHash 冻结 = 我们读取链路 bug。
"""
import json
import sys

def main(path):
    rows = []
    for line in open(path,'r',encoding='utf-8'):
        line=line.strip()
        if not line: continue
        try: obj=json.loads(line)
        except: continue
        if obj.get('type')!='shadowPoseFullTraceFrame': continue
        cad = obj.get('cadence',{})
        ks = obj.get('keyStats',{})
        rows.append({
            'idx': len(rows),
            'ds': str(cad.get('dynamicPoseSignature', '')),
            'ph': str(cad.get('lastSubmittedPaletteHash', '')),
            'ch': str(ks.get('semanticSceneSubmittedSkinnedPaletteCombinedHash', '')),
            'mwH': str(ks.get('runtimeMatrixWriteLastMatrixHash', '')),
            'rcH': str(ks.get('runtimeMatrixRangeCopyLastMatrixHash', '')),
            'mwC': ks.get('runtimeMatrixWriteCount', 0),
            'gpwC': ks.get('runtimeGroupPaletteWrapperCallCount', 0),
            'sub': cad.get('submittedSkinnedCount', 0),
            'smr': cad.get('shadowMapRenderSerial', 0),
            'rebuildHit': ks.get('submitLiveRebuildHitCount', 0),
        })

    print(f'Total trace frames: {len(rows)}')
    # 找 CombinedHash frozen groups
    freeze_groups = []
    cur_start = 0
    cur_ch = rows[0]['ch'] if rows else ''
    for i in range(1, len(rows)):
        if rows[i]['ch'] == cur_ch and cur_ch != '':
            continue
        length = i - cur_start
        if length >= 3 and cur_ch:
            freeze_groups.append((cur_start, i-1, length))
        cur_start = i
        cur_ch = rows[i]['ch']
    length = len(rows) - cur_start
    if length >= 3 and cur_ch:
        freeze_groups.append((cur_start, len(rows)-1, length))

    print(f'CombinedHash frozen groups (>=3): {len(freeze_groups)}')

    # 输出前 3 段对照
    for k, (s, e, l) in enumerate(freeze_groups[:3]):
        print(f'\n=== Group {k+1} trace[{s}..{e}] len={l} ===')
        # 边界前一帧 (active) + 段内 4 帧
        ai = max(0, s-1)
        print(f'  Frame  CH                 mwH                rcH                mwCount delta')
        for i in [ai] + list(range(s, e+1)):
            r = rows[i]
            mw_delta = r['mwC'] - rows[i-1]['mwC'] if i > 0 else 0
            tag = 'A' if i < s else 'F'
            print(f'  t{i:3d}{tag} {r["ch"][:18]:<18} {r["mwH"][:18]:<18} {r["rcH"][:18]:<18} mw+={mw_delta}')

    # 检查 mwH/rcH 是否在 frozen 段也冻结
    print('\n=== Producer LastMatrixHash freeze analysis ===')
    for k, (s, e, l) in enumerate(freeze_groups[:5]):
        mwH_set = set(rows[i]['mwH'] for i in range(s, e+1))
        rcH_set = set(rows[i]['rcH'] for i in range(s, e+1))
        ds_set = set(rows[i]['ds'] for i in range(s, e+1))
        print(f'Group {k+1} trace[{s}..{e}] len={l}: distinct mwH={len(mwH_set)} rcH={len(rcH_set)} ds={len(ds_set)}')

if __name__ == '__main__':
    main(sys.argv[1])
