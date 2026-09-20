"""Read frozen functional receipts; optionally settle the owned short test map."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'AutoTest/artifacts/v122_visual_api_20260914'
sys.path.insert(0, str(ROOT/'AutoTest'))
from run_v122_visual_api_gate import zero_processes, identity


def unique(pairs):
    value = {}
    for k, v in pairs:
        if k in value:
            raise ValueError('duplicate JSON key '+k)
        value[k] = v
    return value


def read(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def require(condition, text):
    if not condition:
        raise RuntimeError(text)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--restore-test-map', action='store_true')
    args = parser.parse_args()
    output = OUT/'validation-summary.json'
    require(not output.exists(), 'summary exists')
    runs = {}
    for name in ('v122_visual_api_first_20260914', 'v122_visual_api_legacy8_20260914',
                 'v122_jass_vm_first_20260914', 'v122_jass_vm_typed_fixed_20260914'):
        path = OUT/name/'receipt.json'
        r = read(path)
        runs[name] = dict(identity=identity(path), ok=r['ok'], errors=r['errors'],
            restored=r['restoreOk'], zero=r['zeroProcesses'], gpu=r['gpuEvents'], newDumps=r['newDumps'])
        require(r['restoreOk'] and r['playerUnchanged'] and r['mapUnchanged'] and
                not r['zeroProcesses'] and not r['gpuEvents'] and not r['newDumps'], 'resource gate '+name)
    vm = OUT/'v122_jass_vm_typed_fixed_20260914'
    r = read(vm/'receipt.json')
    require(r['ok'] and r['jassBytecodeAccepted'] and len(r['vmStages']) == 10, 'VM gate')
    require(r['vmStages'][-1]['checks'] == 83 and all(s['errors'] == 0 for s in r['vmStages']), 'VM assertions')
    require(not r['productAccepted'], 'not a release acceptance')
    candidate = identity(ROOT/'build32/src/d3d9/d3d9.dll')
    require(candidate['size'] == 35258436 and candidate['sha256'] ==
            'D888587E899F9B7EC63C8C1C23F7DB2C5EDDDA4AE8BF9CEA7EAA41CCD5EDE2D4', 'DLL identity')
    pe_data = (ROOT/'build32/src/d3d9/d3d9.dll').read_bytes()
    offset = struct.unpack_from('<I', pe_data, 60)[0]
    require(pe_data[offset:offset+4] == b'PE\0\0' and struct.unpack_from('<H', pe_data, offset+4)[0] == 0x14c
            and struct.unpack_from('<H', pe_data, offset+24)[0] == 0x10b, 'PE32/i386')
    for capture in r['captures']:
        require(identity(Path(capture['path']))['sha256'] == capture['sha256'], 'capture identity')
    for label, backend in [('sphere-high', 2), ('box-high', 2), ('cylinder-high', 2),
                           ('cylinder-medium', 1), ('low-angle-high', 2), ('inside-high', 2)]:
        e = read(vm/('vm-'+label+'.json'))['execution']
        require(e['compositeSubmitted'] and e['effectiveBackend'] == backend and e['fogVolumes'] == 1
                and e['width'] == 2560 and e['height'] == 1440 and e['mapEpoch'] and e['deviceEpoch'], label)
    map_before = identity(Path('E:/Work/War3/Maps/Test/WorldEditTestMap.w3x'))
    source = Path('E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x')
    source_identity = identity(source)
    require(source_identity['sha256'] == '11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF', 'original map')
    zero_processes()
    if args.restore_test_map:
        # AutoTest's audited host copies into this test-only short path. Return
        # it to the known 1137 content used by both pre-VM transactions; the
        # author's separate source map was never opened for writing.
        require(map_before['sha256'] == '5691174EB7A667D9C8553ADD7612C1209626EFF30BA33B8D4B94C4433C511989', 'owned short test map changed')
        backup = OUT/'settled-vm-short-map.w3x'
        with backup.open('xb') as dst, Path(map_before['path']).open('rb') as src:
            shutil.copyfileobj(src, dst)
        require(identity(backup)['sha256'] == map_before['sha256'], 'short map evidence copy')
        shutil.copyfile(source, map_before['path'])
    after = identity(Path(map_before['path']))
    require(after['sha256'] == source_identity['sha256'], 'short map not settled')
    public = set(re.findall(r'^function (WarVK\w+) takes', (ROOT/'WarVK/jass/warvk_api.j').read_text(encoding='utf-8'), re.M))
    direct = set(re.findall(r'\b(WarVK\w+)\(', (ROOT/'AutoTest/v122_jass_vm_scenario.j').read_text()))
    dll_live = identity(Path('E:/Work/War3/d3d9.dll'))
    player = identity(Path('E:/Work/Warcraft III/d3d9.dll'))
    require(dll_live['sha256'] == 'A5701AF3F3724683E559B4F142F7E45DD0EFCA8F6B3DF31C26842EBC10675A7A', 'test DLL restore')
    require(player['sha256'] == 'B1FCC15442DAD980E70947FE7C259B9915CDB3D70DBD946A907F3299C74CB993', 'player DLL changed')
    status = subprocess.check_output(['git', 'status', '--porcelain=v1', '-uall'], cwd=ROOT, text=True)
    value = dict(candidate=candidate, pe='PE32/i386', runs=runs, vmAssertions=83,
        publicFunctionCount=len(public), directlyExercised=sorted(public & direct),
        directlyExercisedCount=len(public & direct), unexercised=sorted(public-direct),
        unsupportedExpectation='WarVKSetBloomEnabled returns 18; not a working bloom claim',
        sourceMap=source_identity, shortMapBefore=map_before, shortMapAfter=after,
        testDllRestored=dll_live, playerDllUnchanged=player, zeroProcesses=zero_processes(),
        dirtyPaths=status.splitlines(), stableAccepted=False, foregroundFpsAccepted=False,
        fullApiMatrixAccepted=False, waterBranchMerged=False)
    with output.open('x', encoding='utf-8') as f:
        json.dump(value, f, indent=2, ensure_ascii=False)
    print(json.dumps(dict(summary=identity(output), vmAssertions=83, apiFunctions=len(public & direct),
        zeroProcesses=value['zeroProcesses'], shortTestMapRestored=True)))


if __name__ == '__main__':
    main()
