"""Compile real-game-observed FF keys with the same x86/x64 production compiler.

CPU/SPIR-V evidence only. No Vulkan device, game, remote draw, or memory-saving
claim. Syntax-negative cases run the actual binaries, not a Python substitute.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from isolated_render_desktop import IsolatedRenderDesktop
import analyze_d3d9_migration_census as census


def require(value, message):
    if not value:
        raise ValueError(message)


def completed(row, exit_code):
    require(row['exitCode'] == exit_code and row['timedOut'] is False and
            row['forcedTermination'] is False and row['processSettled'] is True and
            row['jobEmpty'] is True and row['jobAtomic'] is True and
            row['handleListApplied'] is True, 'native completion/ownership failure')
    require(row['ok'] is (exit_code == 0), 'launch verdict mismatch')
    require(row['cleanupErrors'] == [] and not row.get('error') and
            row['desktopHandleInherited'] is False, 'launch cleanup/handle failure')
    require(row['inputBefore'] == row['inputAfter'] and
            row['desktop'] != row['inputBefore'], 'input Desktop changed')
    token = row['restrictedToken']
    require(token['mode'] == 'restricted-medium-per-child/v1' and
            token['childTokenVerifiedBeforeResume'] is True and
            token['resumedAfterVerification'] is True and
            token['resumePreviousCount'] == 1 and
            token['noHighIntegrityFallback'] is True and
            token['parentTokenUnchanged'] is True and
            token['derivedTokenClosed'] is True and
            token['globalSecurityChanged'] is False and
            token['cleanupErrors'] == [], 'restricted child contract')


def check_desktop_cleanup(raw, launch_count):
    # The raw aggregate correctly remains false after expected exit-1 parser
    # rejections. Preserve it; accept only separately checked expected exits
    # plus complete Desktop/handle cleanup, never a timeout or unknown failure.
    require(raw['schema'] == 1 and raw['scope'] == 'NON_INPUT_DESKTOP_LAUNCH_CONTEXT',
            'Desktop schema')
    require(raw['ok'] is False and len(raw['launches']) == launch_count,
            'raw negative result/launch inventory changed')
    for name in ('desktopCreated','desktopClosed','creatorExited','restrictedMediumRequested'):
        require(raw[name] is True, 'Desktop completion '+name)
    for name in ('desktopHandleInheritable','desktopSwitchRequested','globalInputUsed','gameTouched'):
        require(raw[name] is False, 'unexpected Desktop operation '+name)
    require(raw['cleanupErrors'] == [] and not raw.get('bodyError') and not raw.get('error'),
            'Desktop cleanup/body error')
    require(raw['windowStation'] == 'WinSta0' and raw['desktop'] != raw['inputBefore']
            and raw['desktopPath'] == 'WinSta0\\'+raw['desktop'], 'Desktop identity')
    for name in ('inputAfterCreate','inputBeforeLaunch','inputAfterLaunch','inputAfterRun','inputAfterClose'):
        require(raw[name] == raw['inputBefore'], 'input Desktop changed at '+name)
    return True


def invalid_fixtures(text):
    # Generated from the real retained sample. Every case is a parser error;
    # no malformed bitfield state reaches the original shader compiler.
    vs_line = next((line for line in text.splitlines() if line.startswith('VS ')), None)
    fs_line = next((line for line in text.splitlines() if line.startswith('FS ')), None)
    require(vs_line or fs_line, 'nonempty real sample required')
    first = vs_line or fs_line
    negative = {
        'bad-marker': text.replace('WARVK_FF_KEYS_V1', 'WARVK_FF_KEYS_V2', 1),
        'bad-bool': re.sub(r'invariantPosition [01]', 'invariantPosition 2', text, count=1),
        'empty': 'WARVK_FF_KEYS_V1\ninvariantPosition 0\nforceSampleRateShading 0\nvsCount 0\nfsCount 0\nEND\n',
        'count-overflow': re.sub(r'vsCount [0-9]+', 'vsCount 33', text, count=1),
        'negative-word': text.replace(first, first.split(' ',1)[0]+' -1 '+' '.join(first.split()[2:]),1),
        'hex-word': text.replace(first, first.split(' ',1)[0]+' 0x1 '+' '.join(first.split()[2:]),1),
        'u32-overflow': text.replace(first, first.split(' ',1)[0]+' 4294967296 '+' '.join(first.split()[2:]),1),
        'missing-word': text.replace(first, ' '.join(first.split()[:-1]),1),
        'extra-word': text.replace(first, first+' 0',1),
        'trailing': text+'unexpected\n',
        'bom': '\ufeff'+text,
        'too-large': text+' ' * (16*1024+1),
    }
    # A small duplicate-only input irrespective of the original sample counts.
    stage = 'VS' if vs_line else 'FS'
    negative['duplicate-key'] = (
        'WARVK_FF_KEYS_V1\ninvariantPosition 0\nforceSampleRateShading 0\n'
        f'vsCount {2 if stage == "VS" else 0}\nfsCount {2 if stage == "FS" else 0}\n'
        f'{first}\n{first}\nEND\n')
    return negative


NEGATIVE_ERRORS = {
    'existing-output': 'output directory must be new and its parent must already exist',
    'bad-marker': 'unexpected fixture marker or field order',
    'bad-bool': 'fixture boolean must be exactly 0 or 1',
    'empty': 'empty key sample is not accepted',
    'count-overflow': 'fixture stage count exceeds limit',
    'negative-word': 'only unsigned decimal words are allowed',
    'hex-word': 'only unsigned decimal words are allowed',
    'u32-overflow': 'fixture uint32 overflow',
    'missing-word': 'only unsigned decimal words are allowed',
    'extra-word': 'unexpected fixture marker or field order',
    'trailing': 'trailing fixture input is forbidden',
    'bom': 'fixture must be ASCII without BOM or control bytes',
    'too-large': 'fixture is empty or exceeds byte limit',
    'duplicate-key': 'duplicate fixture key within one stage',
}


def rejected(stdout, stderr, case):
    require(stdout.read_text('utf-8') == '', 'negative case emitted success output')
    data = json.loads(stderr.read_text('utf-8'))
    require(data == dict(success=False, noGpu=True, error=NEGATIVE_ERRORS[case]),
            'negative failed for a different reason: '+case)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--census-log', type=Path, required=True)
    parser.add_argument('--tool32', type=Path, required=True)
    parser.add_argument('--tool64', type=Path, required=True)
    parser.add_argument('--census-test', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device-ordinal', type=int)
    args = parser.parse_args()
    out = args.output.resolve()
    require(out.parent == ROOT/'AutoTest/artifacts', 'fresh artifact child required')
    out.mkdir(exist_ok=False)
    tools_by_bits = {32:args.tool32.resolve(strict=True), 64:args.tool64.resolve(strict=True)}
    log = args.census_log.resolve(strict=True)
    component = args.census_test.resolve(strict=True)
    sources = [Path(__file__), ROOT/'AutoTest/analyze_d3d9_migration_census.py',
               ROOT/'AutoTest/isolated_render_desktop.py',
               ROOT/'src/d3d9/d3d9_fixed_function.h', ROOT/'src/d3d9/d3d9_fixed_function.cpp',
               ROOT/'tools/render_host/dxvk_core/ff_key_compile_main.cpp', log,
               component, *tools_by_bits.values()]
    before = [identity(p) for p in sources]
    receipt = dict(schema='warvk-native-ff-key-equivalence/v1', ok=False, inputs=before,
        cpuOnly=True, gameRenderedBy64Bit=False, fullSceneCapture=False,
        visualEquivalence=False, gameMemorySavingsProven=False, runs=[], negativeRuns=[])
    try:
        for bits, tool in tools_by_bits.items():
            require(pe_identity(tool) == ((0x14c,0x10b) if bits==32 else (0x8664,0x20b)), 'tool bitness')
        require(pe_identity(component) == (0x14c,0x10b), 'x86 production census test required')
        records = census.parse_lines(log.read_text('utf-8').splitlines())
        report = census.report(records, args.device_ordinal)
        selected = census.select_snapshot(records, args.device_ordinal)['snapshot']
        receipt['censusReport'] = report
        fixture = out/'actual-game-ff-keys.txt'
        census.write_fixture(selected, fixture)
        receipt['fixture'] = identity(fixture)
        source_text = fixture.read_text('ascii')
        env = {k:v for k,v in os.environ.items() if k.upper() in {
            'SYSTEMROOT','WINDIR','COMSPEC','PATH','TEMP','TMP','USERPROFILE',
            'APPDATA','LOCALAPPDATA','PROGRAMDATA'}}
        env['DXVK_LOG_PATH'] = 'none'
        results = {}
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            row = desktop.run([component],cwd=out,stdout_path=out/'census-test.stdout',
                stderr_path=out/'census-test.stderr',timeout_s=30,env=env)
            completed(row,0)
            match = re.fullmatch(r'D3D9 migration census CPU checks=([1-9][0-9]*) failures=0 bytes=([1-9][0-9]*) allocationWindow=0\n',
                                (out/'census-test.stdout').read_text('utf-8'))
            require(match and int(match[2]) <= 8192, 'component CPU witness missing')
            receipt['componentChecks'] = int(match[1])
            receipt['componentBytes'] = int(match[2])
            for bits, tool in tools_by_bits.items():
                label = 'x'+str(bits)
                output = out/(label+'-spirv')
                row = desktop.run([tool,'--input',fixture,'--output',output],
                    cwd=out,stdout_path=out/(label+'.stdout'),stderr_path=out/(label+'.stderr'),
                    timeout_s=30,env=env)
                completed(row,0)
                data = json.loads((out/(label+'.stdout')).read_text('utf-8'))
                require(data['schema']=='warvk-ff-key-compile/v1' and data['success'] is True
                        and data['processBits']==bits and data['noGpu'] is True, 'compiler receipt')
                require(data['gameRender'] is False and data['binaryEquivalenceProven'] is False, 'tool claim drift')
                for field in ('invariantPosition','forceSampleRateShading'):
                    require(data[field] is selected[field], 'compiler option mismatch')
                require(data['vsKeyBytes']==20 and data['fsKeyBytes']==64, 'key ABI changed')
                require(type(data['vsCount']) is int and type(data['fsCount']) is int and
                        data['vsCount'] == len(selected['vsKeys']['keys']) and
                        data['fsCount'] == len(selected['fsKeys']['keys']), 'compiler key counts')
                expected = [f'{stage}-{i:03}.spv' for stage in ('vs','fs')
                            for i in range(len(selected[stage+'Keys']['keys']))]
                require([f['file'] for f in data['files']] == expected and
                        sorted(p.name for p in output.iterdir()) == sorted(expected), 'file coverage mismatch')
                identities = []
                for f in data['files']:
                    ident = identity(output/f['file'])
                    raw = (output/f['file']).read_bytes()
                    require(type(f['bytes']) is int and ident['bytes'] == f['bytes'] and
                            20 <= len(raw) <= 8*1024*1024 and len(raw)%4 == 0 and
                            struct.unpack_from('<I',raw)[0] == 0x07230203, 'SPIR-V output invalid')
                    stage, number = f['file'].split('-')
                    words = selected[stage+'Keys']['keys'][int(number[:3])]
                    expected_name = 'FF_'+stage.upper()+'_'+hashlib.sha1(
                        struct.pack('<'+'I'*len(words), *words)).hexdigest()
                    require(f['debugName'] == expected_name, 'shader key/debug identity mismatch')
                    identities.append(dict(file=f['file'], debugName=f['debugName'],
                                           bytes=ident['bytes'], sha256=ident['sha256']))
                results[bits] = identities
                receipt['runs'].append(dict(bits=bits, launch=row, compiler=data, files=identities))
                # No output-overwrite allowance, even for the identical valid fixture.
                before_output = [identity(p) for p in sorted(output.iterdir())]
                row = desktop.run([tool,'--input',fixture,'--output',output],cwd=out,
                    stdout_path=out/(label+'-reuse.stdout'),stderr_path=out/(label+'-reuse.stderr'),
                    timeout_s=10,env=env)
                completed(row,1)
                rejected(out/(label+'-reuse.stdout'), out/(label+'-reuse.stderr'), 'existing-output')
                require(before_output == [identity(p) for p in sorted(output.iterdir())], 'rejected run overwrote evidence')
                receipt['negativeRuns'].append(dict(bits=bits, case='existing-output', exitCode=row['exitCode']))
                for name, text in invalid_fixtures(source_text).items():
                    case_input = out/(label+'-'+name+'.txt')
                    case_input.write_text(text, encoding='utf-8')
                    case_output = out/(label+'-'+name+'-output')
                    row = desktop.run([tool,'--input',case_input,'--output',case_output],cwd=out,
                        stdout_path=out/(label+'-'+name+'.stdout'),
                        stderr_path=out/(label+'-'+name+'.stderr'),timeout_s=10,env=env)
                    completed(row,1)
                    rejected(out/(label+'-'+name+'.stdout'),out/(label+'-'+name+'.stderr'),name)
                    require(not case_output.exists(), 'syntax rejected only after output mutation')
                    receipt['negativeRuns'].append(dict(bits=bits,case=name,exitCode=row['exitCode']))
            require(results[32] == results[64], 'x86/x64 original compiler output differs')
        receipt['expectedExitAndDesktopCleanupPassed'] = check_desktop_cleanup(
            receipt['desktopSafety'], 1 + len(receipt['runs']) + len(receipt['negativeRuns']))
        receipt['compiledUniqueVsKeys'] = len(selected['vsKeys']['keys'])
        receipt['compiledUniqueFsKeys'] = len(selected['fsKeys']['keys'])
        receipt['spirvFilesByteIdentical'] = len(results[32])
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['inputsUnchanged'] = before == [identity(p) for p in sources]
        receipt['ok'] &= receipt['inputsUnchanged']
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in
        ('ok','error','componentChecks','compiledUniqueVsKeys','compiledUniqueFsKeys',
         'spirvFilesByteIdentical','inputsUnchanged')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
