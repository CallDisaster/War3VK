#!/usr/bin/env python3
"""Phase 7.54: 不再用 dynamicPoseSignature 聚合 hash，改看每个 ShadowObject 的 matrixHash。
在 active 段和 frozen 段，分别看：
  - 哪些对象的 matrixHash 在变
  - 哪些对象的 matrixHash 不在变
如果 frozen 段所有对象都不变 → 是引擎层面的 keyframe 离散
如果 frozen 段只有部分对象不变 → 是其他原因（比如 idle）
"""
import json
import sys
from collections import defaultdict

def main(path):
    # frame index -> { runtimeModelPtr -> matrixHash }
    object_hashes_per_frame = []
    cadence_per_frame = []
    pose_per_frame = []

    cur_frame_objs = None
    cur_frame_pose = None
    cur_cadence = None
    cur_serial = None

    for line in open(path,'r',encoding='utf-8'):
        line=line.strip()
        if not line: continue
        try: obj=json.loads(line)
        except: continue
        t = obj.get('type', '')
        if t == 'shadowPoseFullTraceFrame':
            if cur_frame_objs is not None:
                object_hashes_per_frame.append(cur_frame_objs)
                cadence_per_frame.append(cur_cadence)
                pose_per_frame.append(cur_frame_pose)
            cur_frame_objs = {}
            cur_frame_pose = {}
            cur_cadence = obj.get('cadence', {})
            cur_serial = obj.get('cadence', {}).get('sceneFrameSerial')
        elif t == 'shadowPoseFullTraceObject':
            rmp = obj.get('runtimeModelPtr', '')
            mhash = obj.get('matrixHash', '')
            if rmp and cur_frame_objs is not None:
                cur_frame_objs[rmp] = mhash
        elif t == 'shadowPoseFullTracePose':
            rmp = obj.get('runtimeModelPtr', '')
            mhash = obj.get('matrixHash', '')
            if rmp and cur_frame_pose is not None:
                cur_frame_pose[rmp] = mhash

    if cur_frame_objs is not None:
        object_hashes_per_frame.append(cur_frame_objs)
        cadence_per_frame.append(cur_cadence)
        pose_per_frame.append(cur_frame_pose)

    n = len(cadence_per_frame)
    print(f'Total trace frames: {n}')
    if n < 2:
        return

    # 找 dynamicPoseSignature frozen groups
    freeze_groups = []
    cur_start = 0
    cur_ds = cadence_per_frame[0].get('dynamicPoseSignature', '')
    for i in range(1, n):
        ds = cadence_per_frame[i].get('dynamicPoseSignature', '')
        if ds == cur_ds and cur_ds:
            continue
        if (i - cur_start) >= 3 and cur_ds:
            freeze_groups.append((cur_start, i-1, i-cur_start))
        cur_start = i
        cur_ds = ds
    if (n - cur_start) >= 3 and cur_ds:
        freeze_groups.append((cur_start, n-1, n-cur_start))

    print(f'dynamicPoseSignature frozen groups: {len(freeze_groups)}')

    # 选 2 个 group 做 per-object 分析
    for k, (s, e, l) in enumerate(freeze_groups[:2]):
        print(f'\n=== Group {k+1}: trace[{s}..{e}] len={l} ===')

        # PoseRecord 视角
        # 收集这段 frozen 段所有对象的 hash 序列
        all_rmps = set()
        for i in range(max(0, s-1), min(n, e+1)):
            for rmp in pose_per_frame[i].keys():
                all_rmps.add(rmp)
        print(f'  Total distinct runtimeModelPtr in PoseRegistry across active+frozen window: {len(all_rmps)}')

        # 每个对象在 frozen 段是否冻结
        active_idx = max(0, s-1)
        frozen_static = 0   # frozen 段全冻结
        frozen_changed = 0  # frozen 段有变化
        active_changed = 0  # 在 active->frozen 边界变化
        active_static = 0
        examples_static = []
        examples_changed = []
        for rmp in all_rmps:
            hashes_in_frozen = []
            for i in range(s, e+1):
                if rmp in pose_per_frame[i]:
                    hashes_in_frozen.append(pose_per_frame[i][rmp])
            if len(hashes_in_frozen) < 2:
                continue
            distinct_in_frozen = len(set(hashes_in_frozen))
            if distinct_in_frozen == 1:
                frozen_static += 1
                if len(examples_static) < 3:
                    examples_static.append((rmp, hashes_in_frozen[0]))
            else:
                frozen_changed += 1
                if len(examples_changed) < 3:
                    examples_changed.append((rmp, hashes_in_frozen))

            # active 段是否变化
            active_h = pose_per_frame[active_idx].get(rmp, '')
            if active_h and hashes_in_frozen and active_h != hashes_in_frozen[0]:
                active_changed += 1
            elif active_h:
                active_static += 1

        total_with_data = frozen_static + frozen_changed
        print(f'  PoseRecord per-object analysis:')
        print(f'    objects with data: {total_with_data}')
        print(f'    in frozen segment ALL frames same hash: {frozen_static} ({100*frozen_static/max(1,total_with_data):.1f}%)')
        print(f'    in frozen segment changed: {frozen_changed}')
        print(f'    active->frozen first frame changed (i.e. animation moved that one frame): {active_changed}')
        print(f'    active->frozen first frame same: {active_static}')
        for rmp, h in examples_static[:2]:
            print(f'    static example: {rmp[:18]} hash={h[:18]}')
        for rmp, hs in examples_changed[:2]:
            uniq = len(set(hs))
            print(f'    changed example: {rmp[:18]} {uniq} distinct in frozen')

if __name__ == '__main__':
    main(sys.argv[1])
