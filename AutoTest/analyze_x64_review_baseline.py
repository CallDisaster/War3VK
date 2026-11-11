"""Reproduce T1 partial ownership and T2 wait findings without promoting gates.

Usage: python -B AutoTest/analyze_x64_review_baseline.py --output <fresh.json>
This frozen checkpoint intentionally names its three actual game receipts.
It never adds overlapping private/commit/GPU budgets or infers private owners.
"""
import argparse
import hashlib
import json
from pathlib import Path

from native_game_memory_review import summarize

ROOT = Path(__file__).resolve().parent.parent
NAMES = ('t1-va-off-20261029-r1', 't1-va-cpu-recorder-20261029-r1', 't1-va-off-20261029-r2')


def checked_file(identity):
    path = Path(identity['path'])
    data = path.read_bytes()
    if len(data) != identity['bytes'] or hashlib.sha256(data).hexdigest().upper() != identity['sha256']:
        raise ValueError('artifact identity changed: ' + str(path))
    return json.loads(data)


def game_sample(name):
    path = ROOT/'AutoTest/artifacts'/name/'receipt.json'
    receipt = json.loads(path.read_text('utf-8'))
    if not all(receipt.get(key) is True for key in ('ok', 'normalExitProven', 'protectedUnchanged', 'cleanupPassed')):
        raise ValueError('game/exit/preservation gate failed')
    observed = receipt['observer']['memoryReview']
    sample = checked_file(observed['artifact'])
    if not sample['complete'] or sample['snapshotAtomic'] or sample['totalsArePartial']:
        raise ValueError('incomplete or mislabelled VA scan')
    low = [dict(row, end=min(row['end'], 0x80000000)) for row in sample['regions']
           if row['begin'] < 0x80000000]
    full, lower = summarize(sample['regions']), summarize(low)
    if full != sample['summary'] or full != observed['summary']:
        raise ValueError('region accounting differs from receipt')
    image_names = ('vulkan-1.dll', 'nvoglv32.dll', 'nvgpucomp32.dll', 'nvapi.dll',
                   'd3d9.dll', 'game.dll', 'storm.dll')
    images = {}
    for row in full['mappedFiles']:
        base = row['mappedFile'].split('\\')[-1].lower()
        if row['type'] == 'image' and base in image_names:
            images[base] = row['committed']+row['reserved']
    return dict(receipt=str(path), mapSha256=receipt['map']['sha256'],
                dllSha256=next(item['sha256'] for item in receipt['deployed']
                              if Path(item['path']).name.lower() == 'd3d9.dll'),
                resolution=receipt['observer']['actualResolution'],
                largeAddressAware=sample['addressLimit']['largeAddressAware'],
                privateBytes=observed['memoryAfter']['privateBytes'],
                workingSetBytes=observed['memoryAfter']['workingSetBytes'],
                fullStateBytes=full['stateBytes'], fullTypeBytes=full['typeBytes'],
                largestFullHoleBytes=full['largestAlignedFreeHoleBytes'],
                largestLowHoleBytes=lower['largestAlignedFreeHoleBytes'],
                lowFreeBytes=lower['stateBytes']['free'],
                unclassifiedAllocatedVaBytes=full['unclassifiedAllocatedVaBytes'],
                identifiedImageVaBytes=images,
                cpuRecorder=receipt['cpuRecorderReviewRequested'],
                allocatorOwnershipProven=False)


def analyze():
    samples = [game_sample(name) for name in NAMES]
    for key in ('mapSha256', 'dllSha256', 'resolution', 'largeAddressAware'):
        if any(row[key] != samples[0][key] for row in samples[1:]):
            raise ValueError('comparison mismatch: ' + key)
    if [row['cpuRecorder'] for row in samples] != [False, True, False]:
        raise ValueError('not OFF/CPU-ON/OFF')
    deltas = [samples[1]['privateBytes']-samples[index]['privateBytes'] for index in (0, 2)]
    return dict(schema='warvk-x64-review-baseline/v1', samples=samples,
                cpuRecorderPrivateDifferenceRangeBytes=[min(deltas), max(deltas)],
                cameraNumericEquivalenceProven=False, privateAllocatorAttributionComplete=False,
                netMigrationEstimateBytes=None, migrationSavingsProven=False,
                T1Complete=False, T2GoNoGoPassed=False,
                caveats=['one map, fixed initial camera with no input; no deterministic replay',
                         'process private bytes are a different axis from VirtualQuery MEM_PRIVATE',
                         'mapped filenames do not identify private allocators',
                         'graphics DLL image VA is only a conditional movable candidate',
                         'CPU recorder ON is not image history or input-readback ON'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    result = analyze()
    with args.output.open('x', encoding='utf-8') as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
    print(json.dumps(dict(samples=len(result['samples']), T1Complete=False,
                          netMigrationEstimateBytes=None, output=str(args.output))))
