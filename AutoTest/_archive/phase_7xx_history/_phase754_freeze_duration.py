#!/usr/bin/env python3
"""看 frozen 段的真实时间长度（毫秒）。"""
import json
import sys

def main(path):
    last_ds = None
    freeze_start_el = None
    freeze_groups_ms = []
    last_el = 0
    intervals_between = []
    last_freeze_end = None
    for line in open(path,'r',encoding='utf-8'):
        line=line.strip()
        if not line: continue
        try: obj=json.loads(line)
        except: continue
        if obj.get('type')!='shadowPoseFullTraceFrame': continue
        cad = obj.get('cadence',{})
        ds = str(cad.get('dynamicPoseSignature', ''))
        el = obj.get('elapsedMs', 0)
        if ds == last_ds and ds:
            if freeze_start_el is None:
                freeze_start_el = last_el
        else:
            if freeze_start_el is not None:
                duration = last_el - freeze_start_el
                if duration > 0:
                    freeze_groups_ms.append(duration)
                    if last_freeze_end is not None:
                        intervals_between.append(freeze_start_el - last_freeze_end)
                    last_freeze_end = last_el
                freeze_start_el = None
        last_ds = ds
        last_el = el

    print(f'Total frozen group durations (ms): {[f"{x:.0f}" for x in freeze_groups_ms]}')
    if freeze_groups_ms:
        avg = sum(freeze_groups_ms) / len(freeze_groups_ms)
        print(f'Average frozen duration: {avg:.0f}ms')
        print(f'Min: {min(freeze_groups_ms):.0f}ms, Max: {max(freeze_groups_ms):.0f}ms')
    print()
    print(f'Intervals between frozen groups (= active duration): {[f"{x:.0f}" for x in intervals_between]}')
    if intervals_between:
        avg = sum(intervals_between) / len(intervals_between)
        print(f'Average active duration: {avg:.0f}ms')
        print(f'Min: {min(intervals_between):.0f}ms, Max: {max(intervals_between):.0f}ms')

if __name__ == '__main__':
    main(sys.argv[1])
