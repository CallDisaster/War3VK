#!/usr/bin/env python3
"""Phase 7.52 bindings 刷新证据分析：对齐 CombinedHash 冻结窗口 vs snapshot capture 增量。"""
import json
import sys

def phex(s):
    if isinstance(s, int): return s
    if isinstance(s, str) and s.startswith('0x'):
        try: return int(s, 16)
        except: return 0
    return 0

def analyze(path):
    rows = []
    with open(path,'r',encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line: continue
            try: obj = json.loads(line)
            except: continue
            if obj.get('type') != 'shadowPoseFullTraceFrame': continue
            ks = obj.get('keyStats', {}) or {}
            rows.append({
                'ch': phex(ks.get('semanticSceneSubmittedSkinnedPaletteCombinedHash', 0)),
                'lh': phex(ks.get('semanticSceneDirectLastSubmittedPaletteHash', 0)),
                'distinct': ks.get('semanticSceneSubmittedSkinnedPaletteDistinctSampleCount', 0),
                'mw': ks.get('runtimeMatrixWriteCount', 0),
                'gpw': ks.get('runtimeGroupPaletteWrapperCallCount', 0),
                'snapCap': ks.get('renderablePartPaletteSnapshotCapturedCount', 0),
                'snapTooL': ks.get('renderablePartPaletteSnapshotTooLargeCount', 0),
                'snapUnr': ks.get('renderablePartPaletteSnapshotUnreadableCount', 0),
                'snapQH': ks.get('renderablePartPaletteSnapshotQueryHitCount', 0),
                'snapQM': ks.get('renderablePartPaletteSnapshotQueryMissCount', 0),
                'bindQH': ks.get('renderablePartPaletteBindingQueryHitCount', 0),
                'bindQM': ks.get('renderablePartPaletteBindingQueryMissCount', 0),
            })
    if not rows:
        print("no frames")
        return

    print(f"TRACE: {path}")
    print(f"  total frames: {len(rows)}")

    final = rows[-1]
    print(f"  final counters:")
    for k in ['snapCap','snapTooL','snapUnr','snapQH','snapQM','bindQH','bindQM','mw','gpw']:
        print(f"    {k} = {final[k]}")

    # active 段（CombinedHash!=0）
    active_start = next((i for i, r in enumerate(rows) if r['ch'] != 0), None)
    if active_start is None:
        print("  no active frames")
        return
    active = rows[active_start:]
    print(f"  active frames: {len(active)} (from frame {active_start})")

    # CombinedHash 冻结窗口
    windows = []
    cur_start = 0
    cur_hash = active[0]['ch']
    for i in range(1, len(active)):
        if active[i]['ch'] == cur_hash:
            continue
        length = i - cur_start
        if length >= 3:
            windows.append((cur_start, i-1, length, cur_hash))
        cur_start = i
        cur_hash = active[i]['ch']
    length = len(active) - cur_start
    if length >= 3:
        windows.append((cur_start, len(active)-1, length, cur_hash))
    print(f"  CombinedHash frozen windows (>=3): {len(windows)}")

    # 对每个冻结窗口，计算 snapCap/mw/gpw 的增量
    if windows:
        print(f"  Per-frozen-window counter deltas:")
        for (s, e, length, ch) in windows[:10]:
            r0 = active[s]
            r1 = active[e]
            dmw = r1['mw'] - r0['mw']
            dgpw = r1['gpw'] - r0['gpw']
            dsnap = r1['snapCap'] - r0['snapCap']
            dqh = r1['snapQH'] - r0['snapQH']
            print(f"    window[{s}..{e}] len={length} ch={ch:#x}  dmw={dmw}  dgpw={dgpw}  dsnapCap={dsnap}  dsnapQH={dqh}")

    # 全局：active 段里 snapCap 每帧平均增量
    total_snap = active[-1]['snapCap'] - active[0]['snapCap']
    total_gpw = active[-1]['gpw'] - active[0]['gpw']
    total_mw = active[-1]['mw'] - active[0]['mw']
    print(f"  Active-segment total delta: snapCap={total_snap} gpw={total_gpw} mw={total_mw}")
    print(f"  Per-frame avg: snapCap={total_snap/len(active):.1f} gpw={total_gpw/len(active):.1f}")

    # 采样几帧头尾
    print(f"  first 3 active frames:")
    for r in active[:3]:
        print(f"    ch={r['ch']:#x} snapCap={r['snapCap']} snapQH={r['snapQH']} gpw={r['gpw']}")
    print(f"  last 3 active frames:")
    for r in active[-3:]:
        print(f"    ch={r['ch']:#x} snapCap={r['snapCap']} snapQH={r['snapQH']} gpw={r['gpw']}")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        analyze(p)
        print()
