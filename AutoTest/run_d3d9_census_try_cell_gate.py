"""Build/run the real bounded census and production v2 writer on an isolated Desktop.

CPU-only. Synthetic roundtrip facts are never labeled game observations. This
does not create a GPU device, touch a player install, or certify performance.
"""
import argparse
import json
import os
from pathlib import Path
import re
import sys

import analyze_d3d9_migration_census as reader
from isolated_render_desktop import IsolatedRenderDesktop
from run_device_owned_build import inputs
from run_fixed_function_key_equivalence import completed
from run_render_host_core_gate import execute
from run_render_host_protocol_gate import ROOT, identity, pe_identity


def require(value, message):
    if not value:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build, out = args.build.resolve(strict=True), args.output.resolve()
    require(build.parent == ROOT and build.name.startswith('build32_device_owner_'), 'dedicated x86 build required')
    require(out.parent == ROOT/'AutoTest/artifacts', 'fresh artifact child required')
    out.mkdir(exist_ok=False)
    sources = set(inputs()) | {Path(__file__), Path(reader.__file__),
        ROOT/'AutoTest/test_d3d9_migration_census.cpp',
        ROOT/'AutoTest/d3d9_migration_census_roundtrip.cpp',
        ROOT/'AutoTest/run_fixed_function_key_equivalence.py',
        ROOT/'AutoTest/isolated_render_desktop.py'}
    sources = sorted(sources)
    before = [identity(path) for path in sources]
    player = Path('E:/Work/Warcraft III/d3d9.dll')
    protected = identity(player)
    receipt = dict(schema='warvk-d3d9-census-try-cell-gate/v1', ok=False,
        cpuOnly=True, syntheticRoundtrip=True, gameLaunched=False, gpuExecuted=False,
        performanceAcceptance=False, source=before, protectedBefore=protected, steps=[], runs=[])
    env = os.environ.copy()
    env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;' + env['PATH']
    targets = ['src/d3d9/warvk-d3d9-migration-census-test.exe',
               'src/d3d9/warvk-d3d9-migration-census-roundtrip.exe']
    try:
        receipt['steps'].append(execute(['ninja', '-C', build, '-j2', *targets], ROOT, out/'build-tests', 180, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-n', *targets, 'src/d3d9/d3d9.dll'],
                                        ROOT, out/'no-work', 30, env))
        require('no work to do' in (out/'no-work.stdout').read_text('utf-8'), 'candidate not settled')
        binaries = [build/target for target in targets]
        require(all(pe_identity(path) == (0x14c, 0x10b) for path in binaries), 'x86 test binaries required')
        receipt['products'] = [identity(path) for path in binaries]
        receipt['candidate'] = identity(build/'src/d3d9/d3d9.dll')
        child_env = {key: value for key, value in env.items() if key.upper() in {
            'SYSTEMROOT','WINDIR','COMSPEC','PATH','TEMP','TMP','USERPROFILE',
            'APPDATA','LOCALAPPDATA','PROGRAMDATA'}}
        child_env['DXVK_LOG_PATH'] = 'none'
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            for label, binary in zip(('component', 'roundtrip'), binaries):
                row = desktop.run([binary], cwd=out, stdout_path=out/(label+'.stdout'),
                    stderr_path=out/(label+'.stderr'), timeout_s=30, env=child_env)
                completed(row, 0)
                require((out/(label+'.stderr')).read_text('utf-8') == '', 'unexpected native stderr')
                receipt['runs'].append(dict(label=label, launch=row))
        safety = receipt['desktopSafety']
        require(safety['ok'] is True and safety['desktopClosed'] is True and
                safety['creatorExited'] is True and not safety['cleanupErrors'] and
                safety['globalInputUsed'] is False and safety['desktopSwitchRequested'] is False,
                'isolated Desktop did not settle cleanly')
        witness = (out/'component.stdout').read_text('utf-8')
        match = re.search(r'D3D9 migration census CPU checks=([1-9][0-9]*) failures=0 bytes=([1-9][0-9]*) allocationWindow=0\n$', witness)
        require(match and int(match[2]) <= 8192, 'component witness missing')
        receipt['componentChecks'] = int(match[1])
        receipt['componentBytes'] = int(match[2])
        sizes = re.search(r'^Try-cell census bytes=([1-9][0-9]*) copyBytes=([1-9][0-9]*) allocationWindow=0$',
                          witness, re.MULTILINE)
        require(sizes and max(map(int, sizes.groups())) <= 8192, 'bounded wrapper/copy witness missing')
        receipt['wrapperBytes'], receipt['copyBytes'] = map(int, sizes.groups())
        concurrency = re.search(r'^Try-cell concurrency attempted=([0-9]+) accepted=([0-9]+) '
            r'recordBusy=([0-9]+) snapshots=([0-9]+) snapshotBusy=([0-9]+) '
            r'halfwaySnapshots=([0-9]+) failures=0$', witness, re.MULTILINE)
        require(concurrency is not None, 'concurrent writer/reader witness missing')
        counts = dict(zip(('attempted', 'accepted', 'recordBusy', 'snapshots',
                           'snapshotBusy', 'halfwaySnapshots'), map(int, concurrency.groups())))
        # This exact equality is test-only: three known API-producing writers,
        # all joined before the counters are read. Production refusal samples
        # have mixed units and are not coherent with accepted call snapshots.
        require(counts['attempted'] == 60003 and counts['accepted'] > 3 and
                counts['attempted'] == counts['accepted'] + counts['recordBusy'] and
                counts['snapshots'] > 0 and counts['halfwaySnapshots'] == 1,
                'concurrent transaction accounting/overlap witness mismatch')
        receipt['concurrency'] = counts
        records = reader.parse_log((out/'roundtrip.stdout').read_text('utf-8'))
        require(len(records) == 3, 'three coherent copies expected')
        lifecycle = [(r['snapshot']['deviceOrdinal'], r['snapshot']['snapshotSequence'],
                      r['snapshot']['reason']) for r in records]
        require(lifecycle == [(1, 1, 'normal-device-destruction'),
                              (2, 1, 'present-milestone'),
                              (2, 2, 'normal-device-destruction')],
                'production-writer fixture lifecycle/sequence mismatch')
        empty = reader.report(records[:1])
        require('no-draw-attempt-evidence' in empty['certificationRefusals'] and
                empty['fullCoverageCertified'] is False, 'empty snapshot promoted')
        require(empty['selectedDeviceOrdinal'] == 1 and
                empty['selectedSnapshot']['reason'] == 'normal-device-destruction',
                'empty device lifetime differs from production destruction path')
        empty_values = empty['selectedSnapshot']
        require(empty_values['observedCalls'] == empty_values['drawAttempts'] ==
                empty_values['observedTextureRequests'] == empty_values['vsKeys']['observed'] ==
                empty_values['fsKeys']['observed'] == 0, 'unused device is not empty')
        first_present = records[1]['snapshot']
        require(first_present['observedCalls'] == 97 and first_present['apiCalls']['Reset'] == 0 and
                first_present['presentAttempts'] == 1, 'first Present fixture transaction mismatch')
        result = reader.report(records, device_ordinal=2)
        require(result['deviceOrdinals'] == [1, 2], 'independent device lifetimes expected')
        final = result['selectedSnapshot']
        require(final['schema'] == reader.SCHEMA_V2 and final['diagnosticForcedDeviceLock'] is False,
                'production serializer policy mismatch')
        require(final['drawKinds'] == [0,12,12,12,12] and final['textureKinds'] == [0,16,16,16], 'grouped observations differ')
        require(final['drawMissingFields'] == [0,0,48,48,24,48,24,36], 'argument-only facts drift')
        require(final['observedCalls'] == 98 and final['apiCalls']['Reset'] == 1 and
                final['presentAttempts'] == 1, 'coherent API arithmetic mismatch')
        require(final['drawSamplesDropped'] == final['textureSamplesDropped'] == 16, 'bounded fact samples')
        require(final['vsKeys']['droppedObservations'] == final['fsKeys']['droppedObservations'] == 16, 'bounded key samples')
        require(final['observationRefusals'] == dict(recordBusy=0,snapshotBusy=0,counterSaturated=0), 'serial fixture refused unexpectedly')
        receipt['roundtripReport'] = result
        receipt['roundtripRecords'] = len(records)
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['sourceUnchanged'] = before == [identity(path) for path in sources]
        receipt['protectedAfter'] = identity(player)
        receipt['protectedUnchanged'] = protected == receipt['protectedAfter']
        receipt['ok'] &= receipt['sourceUnchanged'] and receipt['protectedUnchanged']
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({key:receipt.get(key) for key in ('ok','error','componentChecks','roundtripRecords','sourceUnchanged','protectedUnchanged')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
