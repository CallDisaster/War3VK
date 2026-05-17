"""Analyze Phase 7.45 real-scene trace for shadow pose stutter root cause."""
import json, sys, csv
from collections import defaultdict

TRACE = r"E:\Work\War3\WarVK\Log\shadow_pose_full_trace_2026_05_12_21_53_54.jsonl"

frames = []
pose_records = defaultdict(list)  # frameSerial -> list of pose records

with open(TRACE, 'r') as f:
    for line in f:
        obj = json.loads(line)
        t = obj.get('type')
        if t == 'shadowPoseFullTraceFrame':
            frames.append(obj)
        elif t == 'shadowPoseFullTracePose':
            pose_records[obj['frameSerial']].append(obj)

print(f"=== Trace Summary: {len(frames)} frame events ===\n")

# Extract key fields per frame
rows = []
for i, fr in enumerate(frames):
    c = fr['cadence']
    ks = fr['keyStats']
    rows.append({
        'idx': i,
        'elapsedMs': fr['elapsedMs'],
        'sceneSerial': c['sceneFrameSerial'],
        'uploadSerial': c['shadowMatrixUploadSerial'],
        'renderSerial': c['shadowMapRenderSerial'],
        'poseSig': c['dynamicPoseSignature'],
        'matrixKey': c['shadowMatrixSceneKey'],
        'paletteHash': c['lastSubmittedPaletteHash'],
        'groupHash': c['lastSubmittedGroupHash'],
        'submitted': ks['semanticSceneSubmittedSkinned'],
        'leaseUpdated': ks.get('semanticSceneDirectPartLeaseUpdatedCount', 0),
        'leaseRestored': ks.get('semanticSceneDirectPartLeaseRestoredCount', 0),
        'poseStale': ks.get('semanticSceneShadowManifestPoseStalePartCount', 0),
        'sliceStale': ks.get('semanticSceneShadowManifestSliceStalePartCount', 0),
        'freshPart': ks.get('semanticSceneShadowManifestFreshPartCount', 0),
        'contentAge3to5': ks.get('submitPaletteContentAgeLag3To5Count', 0),
        'contentAge6plus': ks.get('submitPaletteContentAgeLag6PlusCount', 0),
        'contentAgeMax': ks.get('submitPaletteContentAgeMax', 0),
        'paletteChurn': ks.get('semanticSceneSubmittedSkinnedPaletteHashChurnCount', 0),
        'largeDelta': ks.get('semanticSceneSubmittedSkinnedPaletteFirstMatrixLargeDeltaCount', 0),
        'executed': c.get('shadowMapExecutedThisFrame', 0),
        'reuse': c.get('receiverReuseShadowMap', 0),
        'replayDraws': c.get('replayDrawsCount', 0),
    })

# Find repeated runs of dynamicPoseSignature
print("=== dynamicPoseSignature repeated runs (len>=3) ===")
print(f"{'start':>5} {'end':>5} {'len':>4} {'ms':>8} | {'poseSig':>18} | {'paletteHash':>18} | {'leaseUpd':>8} {'leaseRst':>8} {'poseStale':>9} {'freshPart':>9} {'contentAge3+':>12} {'paletteChurn':>12}")
runs = []
i = 0
while i < len(rows):
    j = i
    while j < len(rows) and rows[j]['poseSig'] == rows[i]['poseSig']:
        j += 1
    run_len = j - i
    if run_len >= 3:
        elapsed = rows[j-1]['elapsedMs'] - rows[i]['elapsedMs']
        r = rows[i]
        # Check if palette also frozen during this run
        palette_unique = len(set(rows[k]['paletteHash'] for k in range(i, j)))
        runs.append((i, j-1, run_len, elapsed, r['poseSig'], palette_unique))
        print(f"{i:5d} {j-1:5d} {run_len:4d} {elapsed:8.1f} | {r['poseSig'][2:18]:>16} | pal_uniq={palette_unique} | {r['leaseUpdated']:8d} {r['leaseRestored']:8d} {r['poseStale']:9d} {r['freshPart']:9d} {r['contentAge3to5']+r['contentAge6plus']:12d} {r['paletteChurn']:12d}")
    i = j

print(f"\nTotal runs >= 3 frames: {len(runs)}")
print()

# Now look at what changes BETWEEN frozen runs (the "smooth" intervals)
print("=== Transitions: last frozen frame -> first next frame ===")
print(f"{'fromIdx':>7} {'toIdx':>7} | {'poseSig_changed':>15} {'paletteHash_changed':>19} {'matrixKey_changed':>17} | {'leaseUpd_delta':>14} {'freshPart_delta':>15}")
for ri in range(len(runs) - 1):
    end_idx = runs[ri][1]
    next_start = runs[ri+1][0]
    if next_start > end_idx + 1:
        # There's a gap (smooth interval)
        r_end = rows[end_idx]
        r_next = rows[end_idx + 1]
        print(f"{end_idx:7d} {end_idx+1:7d} | {r_end['poseSig']!=r_next['poseSig']:>15} {r_end['paletteHash']!=r_next['paletteHash']:>19} {r_end['matrixKey']!=r_next['matrixKey']:>17} | {r_next['leaseUpdated']-r_end['leaseUpdated']:>14d} {r_next['freshPart']-r_end['freshPart']:>15d}")

print()

# Key question: during frozen runs, are leaseUpdated/leaseRestored changing?
print("=== Within frozen poseSig runs: do lease counts change? ===")
for start, end, rlen, elapsed, sig, pal_uniq in runs[:5]:
    lease_upd_vals = [rows[k]['leaseUpdated'] for k in range(start, end+1)]
    lease_rst_vals = [rows[k]['leaseRestored'] for k in range(start, end+1)]
    fresh_vals = [rows[k]['freshPart'] for k in range(start, end+1)]
    stale_vals = [rows[k]['poseStale'] for k in range(start, end+1)]
    submitted_vals = [rows[k]['submitted'] for k in range(start, end+1)]
    palette_vals = [rows[k]['paletteHash'] for k in range(start, end+1)]
    print(f"  Run idx {start}-{end} (sig={sig[2:10]}):")
    print(f"    leaseUpdated: {lease_upd_vals}")
    print(f"    leaseRestored: {lease_rst_vals}")
    print(f"    freshPart: {fresh_vals}")
    print(f"    poseStale: {stale_vals}")
    print(f"    submitted: {submitted_vals}")
    print(f"    paletteHash unique: {len(set(palette_vals))}")
    print()

# Check: is the cadence periodic?
print("=== Run length distribution ===")
all_runs = []
i = 0
while i < len(rows):
    j = i
    while j < len(rows) and rows[j]['poseSig'] == rows[i]['poseSig']:
        j += 1
    all_runs.append(j - i)
    i = j
from collections import Counter
dist = Counter(all_runs)
for k in sorted(dist.keys()):
    print(f"  len={k}: {dist[k]} occurrences")

# Check pose record staleness
print("\n=== Pose record freshness in frozen vs smooth frames ===")
# Pick a frozen run and a smooth frame, compare lastMatrixPaletteFrame vs poseRegistryFrame
if runs:
    frozen_idx = runs[0][0]
    fr = frames[frozen_idx]
    serial = fr['cadence']['serial']
    poses = pose_records.get(serial, [])
    if poses:
        reg_frame = fr.get('poseRegistryFrame', 0)
        ages = [reg_frame - p['lastMatrixPaletteFrame'] for p in poses if p['lastMatrixPaletteFrame'] > 0]
        if ages:
            print(f"  Frozen frame idx={frozen_idx}, serial={serial}, poseRegistryFrame={reg_frame}")
            print(f"    Pose ages (registryFrame - lastMatrixPaletteFrame): min={min(ages)}, max={max(ages)}, mean={sum(ages)/len(ages):.1f}, count={len(ages)}")

# Write CSV for external analysis
csv_path = r"E:\Mycode\Source\Repos\War3MapReforge\Core\Base\Graphics\dxvk\AutoTest\artifacts\phase745_trace_analysis_runs.csv"
with open(csv_path, 'w', newline='') as csvf:
    w = csv.writer(csvf)
    w.writerow(['idx','elapsedMs','sceneSerial','uploadSerial','renderSerial','poseSig','matrixKey','paletteHash','groupHash','submitted','leaseUpdated','leaseRestored','poseStale','sliceStale','freshPart','contentAge3to5','contentAge6plus','contentAgeMax','paletteChurn','largeDelta','executed','reuse','replayDraws'])
    for r in rows:
        w.writerow([r['idx'],r['elapsedMs'],r['sceneSerial'],r['uploadSerial'],r['renderSerial'],r['poseSig'],r['matrixKey'],r['paletteHash'],r['groupHash'],r['submitted'],r['leaseUpdated'],r['leaseRestored'],r['poseStale'],r['sliceStale'],r['freshPart'],r['contentAge3to5'],r['contentAge6plus'],r['contentAgeMax'],r['paletteChurn'],r['largeDelta'],r['executed'],r['reuse'],r['replayDraws']])
print(f"\nCSV written to: {csv_path}")
