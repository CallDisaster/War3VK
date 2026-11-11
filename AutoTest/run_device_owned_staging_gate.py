"""Repeat the frozen device-owned GPU fixture with direct mapping disabled.

No compilation/relinking, product modification, deployment, game or global
input. Copy the exact parent EXE and diagnostic DLL bytes from a successful
run_device_owned_lifetime_gate receipt into this run's fresh executable
directory. Never execute in the baseline directory: exe-relative crash-handler
status/log writes would mutate its evidence even with an isolated cwd.
The sole configuration override is
d3d9.allowDirectBufferMapping = False, verified in the actual DXVK log.

This is configured staging-path coverage inferred from the frozen production
source, NOT direct instrumentation of map mode, GPU copies, or copy counts.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

from run_device_owned_lifetime_gate import (
    ROOT, check_exports, check_output, identity, native_uint, observe, pe_exports,
    pe_identity, preservation_check, require, source_paths, strict_json, succeeded,
    verify_product_receipt,
)


CONFIG_TEXT = 'd3d9.allowDirectBufferMapping = False\n'
MANIFEST_GROUPS = ('localInputIdentities', 'preservedArtifacts', 'toolIdentities',
                   'toolchainLibraryIdentities', 'originalLinkInputIdentities', 'artifacts')


def identity_record(record):
    require(type(record) is dict and record.keys() == {'path', 'bytes', 'sha256'}, 'identity record shape')
    require(type(record['path']) is str and Path(record['path']).is_absolute()
            and native_uint(record['bytes']) and type(record['sha256']) is str
            and re.fullmatch('[0-9A-F]{64}', record['sha256']) is not None,
            'identity record values')
    return record


def baseline_manifest(baseline, baseline_receipt):
    """Validate and deduplicate every recorded frozen input/output identity."""
    require(type(baseline) is dict and baseline.get('schema') == 'warvk-device-owned-lifetime/v1',
            'device-owned lifetime baseline schema required')
    for key in ('ok', 'preservationVerified', 'productSourceFreezeVerified', 'localInputSetUnchanged'):
        require(baseline.get(key) is True, 'baseline did not certify ' + key)
    require(baseline.get('changedInputs') == [], 'baseline contains input drift')
    for key in ('gameLaunched', 'globalInput', 'deployed', 'releaseReady', 'visualGameAcceptance',
                'allocatorFailureInjected', 'instanceDrawAcceptance', 'memorySavingsProven'):
        require(baseline.get(key) is False, 'baseline scope mismatch: ' + key)
    records = [identity(baseline_receipt)]
    for key in MANIFEST_GROUPS:
        group = baseline.get(key)
        require(type(group) is list and group, 'baseline lacks identity group: ' + key)
        records += [identity_record(record) for record in group]
    for key in ('productBuildReceipt', 'diagnosticDll', 'parentExe'):
        records.append(identity_record(baseline.get(key)))
    merged = {}
    for record in records:
        path = Path(record['path']).resolve()
        require(path not in merged or merged[path] == record, 'conflicting baseline identity: ' + str(path))
        merged[path] = record
    return [merged[path] for path in sorted(merged)]


def check_effective_configuration(text, config):
    """Require the actual logger's loaded path and effective option block."""
    require(type(text) is str, 'DXVK log text required')
    lines = text.splitlines()
    found = []
    blocks = []
    option_occurrences = []
    for index, line in enumerate(lines):
        match = re.fullmatch(r'info:\s+Found config file:\s+(.+)', line)
        if match:
            found.append(Path(match[1]).resolve())
        if 'd3d9.allowDirectBufferMapping' in line:
            option_occurrences.append((index + 1, line))
        if re.fullmatch(r'info:\s+Effective configuration:', line):
            options = {}
            evidence = []
            for following, entry in enumerate(lines[index + 1:], index + 2):
                value = re.fullmatch(r'info:\s+([A-Za-z0-9_.]+)\s+=\s+(.+)', entry)
                if not value:
                    break
                require(value[1] not in options, 'duplicate effective option in DXVK log')
                options[value[1]] = value[2]
                evidence.append(dict(line=following, text=entry))
            blocks.append(dict(headerLine=index + 1, options=options, evidence=evidence))
    # One factory and one loaded DLL produce one configuration. Repeated or
    # competing blocks are not accepted by silently choosing the desired one.
    require(found == [config.resolve()], 'DXVK did not load exactly this isolated config')
    require(len(blocks) == 1 and blocks[0]['options'] == {'d3d9.allowDirectBufferMapping': 'False'},
            'actual effective configuration is not the single requested override')
    require(len(option_occurrences) == 1, 'competing direct-buffer-mapping log observations')
    return dict(loadedConfig=str(found[0]), effectiveOption=False,
                block=blocks[0], evidenceKind='ACTUAL_DXVK_EFFECTIVE_CONFIGURATION_LOG',
                directMapModeInstrumented=False, stagingCopyCountInstrumented=False)


def copy_frozen_binary(expected, destination):
    """New physical file, same bytes; no hardlink, overwrite, compile or link."""
    source = Path(identity_record(expected)['path']).resolve(strict=True)
    require(identity(source) == expected, 'binary source changed before copy')
    require(source != destination.resolve() and source.parent != destination.parent.resolve(),
            'fixture binary must use a separate fresh executable directory')
    with source.open('rb') as original, destination.open('xb') as copied:
        shutil.copyfileobj(original, copied, length=1 << 20)
    actual = identity(destination)
    require(actual['bytes'] == expected['bytes'] and actual['sha256'] == expected['sha256'],
            'fixture binary copy does not match frozen bytes')
    require(not os.path.samefile(source, destination), 'fixture binary must not alias the baseline file')
    require(pe_identity(destination) == (0x14c, 0x10b), 'fixture binary copy is not x86 PE32')
    return actual


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fixture-timeout', type=int, default=240)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows hidden native fixture required')
    require(1 <= args.fixture_timeout <= 900, 'bounded fixture watchdog required')
    artifacts = (ROOT / 'AutoTest/artifacts').resolve(strict=True)
    baseline_receipt = args.baseline_receipt.resolve(strict=True)
    baseline_dir = baseline_receipt.parent
    output = args.output.resolve()
    require(baseline_receipt.name == 'receipt.json' and baseline_dir.parent == artifacts,
            'baseline must be an existing direct AutoTest artifact child receipt')
    require(output.parent == artifacts and output != baseline_dir, 'fresh direct artifact child required')
    output.mkdir(exist_ok=False)
    receipt = dict(schema='warvk-device-owned-staging/v1', ok=False,
                   scope='SAME_FROZEN_BINARIES_CONFIGURED_STAGING_PATH',
                   baselineReceipt=identity(baseline_receipt), priority='BelowNormal', parallelTools=1,
                   compiled=False, relinked=False, productDllBuilt=False, buildRegenerated=False,
                   baselineExecutableDirectoryUsed=False, exactBinaryCopiesVerified=False,
                   gameLaunched=False, globalInput=False, deployed=False, releaseReady=False,
                   visualGameAcceptance=False, instanceDrawAcceptance=False,
                   allocatorFailureInjected=False, memorySavingsProven=False,
                   directMapModeInstrumented=False, stagingCopyCountInstrumented=False,
                   fullToolchainHeadersFrozen=False, preservationVerified=False,
                   steps=[])
    tracked = [receipt['baselineReceipt'], identity(Path(__file__).resolve()), identity(Path(sys.executable).resolve())]
    receipt['runnerIdentities'] = tracked[1:].copy()
    build = None
    baseline = None
    baseline_files = None
    source_before = None
    try:
        baseline_bytes = baseline_receipt.read_bytes()
        baseline = strict_json(baseline_bytes.decode('utf-8'))
        inherited = baseline_manifest(baseline, baseline_receipt)
        tracked += inherited
        receipt['baselineInputOutputIdentities'] = inherited
        require(not preservation_check(tracked), 'baseline input/tool/object/binary identity drift')
        baseline_parent = Path(baseline['parentExe']['path']).resolve(strict=True)
        baseline_diagnostic = Path(baseline['diagnosticDll']['path']).resolve(strict=True)
        require(baseline_parent == baseline_dir / 'device-owned-lifetime-parent.exe'
                and baseline_diagnostic == baseline_dir / 'diagnostic/d3d9.dll', 'unexpected baseline executable locations')
        require(pe_identity(baseline_parent) == pe_identity(baseline_diagnostic) == (0x14c, 0x10b),
                'exact x86 PE32 binaries required')
        build = Path(baseline['build']).resolve(strict=True)
        require(build.parent == ROOT and build.name.startswith('build32_device_owner_'), 'unexpected production build')
        source_before = source_paths(build)
        require([identity(p) for p in source_before] == baseline['localInputIdentities'],
                'baseline local input set or source identities changed')
        product_receipt = Path(baseline['productBuildReceipt']['path']).resolve(strict=True)
        product = build / 'src/d3d9/d3d9.dll'
        verify_product_receipt(product_receipt, build, product)
        baseline_exports = pe_exports(baseline_diagnostic)
        check_exports(pe_exports(product), baseline_exports)
        require(identity(Path(sys.executable).resolve()) in baseline['toolIdentities'],
                'use the same recorded Python tool as the baseline')
        baseline_files = {Path(record['path']).resolve() for record in baseline['artifacts']}
        require(all(path.is_relative_to(baseline_dir) for path in baseline_files), 'baseline artifact escaped its directory')
        baseline_files.add(baseline_receipt)
        require(baseline_files == {p.resolve() for p in baseline_dir.rglob('*') if p.is_file()},
                'baseline artifact set has changed')
        fixture_steps = [s for s in baseline['steps'] if s.get('command') ==
                         [str(baseline_parent), '--dll', str(baseline_diagnostic), '--cycles', '3']]
        require(len(fixture_steps) == 1, 'baseline must have one exact three-cycle native fixture')
        succeeded(fixture_steps[0])
        baseline_acceptance = check_output((baseline_dir / 'fixture.stdout').read_text('utf-8', errors='replace'),
            (baseline_dir / 'fixture.stderr').read_text('utf-8', errors='replace'), fixture_steps[0]['exit'], 3)
        require(baseline_acceptance == baseline['acceptance'], 'baseline raw records do not reproduce its acceptance')
        receipt['baselineAcceptance'] = baseline_acceptance
        receipt['frozenBinarySources'] = dict(parentExe=baseline['parentExe'], diagnosticDll=baseline['diagnosticDll'])
        # Isolating cwd/config/logs is insufficient: the production crash
        # handler also writes beside the executable. Run new physical copies
        # so those writes cannot alter the immutable baseline evidence.
        fixture_dir = output / 'bin'
        fixture_dir.mkdir(exist_ok=False)
        require(fixture_dir.resolve().parent == output and not fixture_dir.resolve().is_relative_to(baseline_dir),
                'fixture executable directory must be inside the fresh output only')
        parent = fixture_dir / 'device-owned-lifetime-parent.exe'
        diagnostic = fixture_dir / 'd3d9.dll'
        receipt['parentExe'] = copy_frozen_binary(baseline['parentExe'], parent)
        tracked.append(receipt['parentExe'])
        receipt['diagnosticDll'] = copy_frozen_binary(baseline['diagnosticDll'], diagnostic)
        tracked.append(receipt['diagnosticDll'])
        require(pe_exports(diagnostic) == baseline_exports, 'copied diagnostic exports changed')
        receipt['exactBinaryCopiesVerified'] = True
        receipt['fixtureExecutableDirectory'] = str(fixture_dir)
        # Preserve the full source receipt in this new evidence directory.
        snapshot = output / 'baseline-receipt.json'
        snapshot.write_bytes(baseline_bytes)
        receipt['baselineReceiptSnapshot'] = identity(snapshot)
        tracked.append(receipt['baselineReceiptSnapshot'])
        source_notes = [ROOT / 'src/d3d9/d3d9_options.cpp', ROOT / 'src/d3d9/d3d9_common_buffer.cpp',
                        ROOT / 'src/util/config/config.cpp', ROOT / 'src/dxvk/dxvk_instance.cpp']
        receipt['sourceDerivedMode'] = dict(
            kind='FROZEN_SOURCE_PLUS_EFFECTIVE_CONFIG_NOT_COPY_INSTRUMENTATION',
            rule='allowDirectBufferMapping=false selects D3D9_COMMON_BUFFER_MAP_MODE_BUFFER',
            sources=[identity(p) for p in source_notes])
        runtime = output / 'runtime'
        runtime.mkdir()
        config = runtime / 'dxvk.conf'
        config.write_text(CONFIG_TEXT, encoding='utf-8', newline='\n')
        receipt['configuration'] = identity(config)
        receipt['configurationText'] = CONFIG_TEXT
        tracked.append(receipt['configuration'])
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('DXVK_', 'VK_'))}
        receipt['removedEnvironmentKeys'] = sorted(set(os.environ) - set(env))
        env.update(DXVK_CONFIG_FILE=str(config), DXVK_LOG_PATH=str(runtime),
                   DXVK_STATE_CACHE_PATH=str(runtime), DXVK_LOG_LEVEL='info')
        receipt['fixtureEnvironment'] = {k: v for k, v in env.items() if k.startswith('DXVK_')}
        require(not preservation_check(tracked), 'identity drift immediately before configured fixture')
        require(parent.parent.resolve() == diagnostic.parent.resolve() == fixture_dir.resolve()
                and parent.parent.resolve() != baseline_parent.parent
                and not parent.resolve().is_relative_to(baseline_dir)
                and not diagnostic.resolve().is_relative_to(baseline_dir),
                'executing baseline paths or reusing its executable directory is forbidden')
        step = observe([parent, '--dll', diagnostic, '--cycles', '3'], runtime,
                       output / 'fixture', args.fixture_timeout, env, receipt['steps'])
        stdout = (output / 'fixture.stdout').read_text('utf-8', errors='replace')
        stderr = (output / 'fixture.stderr').read_text('utf-8', errors='replace')
        receipt['rawFinalRecords'] = [line for line in stdout.splitlines() if line.startswith('DEVICE_OWNER_RESULT ')]
        succeeded(step)
        receipt['acceptance'] = check_output(stdout, stderr, step['exit'], 3)
        dxvk_log = runtime / 'device-owned-lifetime-parent_d3d9.log'
        require(dxvk_log.is_file() and list(runtime.glob('*_d3d9.log')) == [dxvk_log],
                'one actual fixture DXVK log required')
        receipt['effectiveConfiguration'] = check_effective_configuration(dxvk_log.read_text('utf-8'), config)
        receipt['dxvkLog'] = identity(dxvk_log)
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        # Read-only revalidation even after watchdog/GPU/config failure. Never
        # overwrite/restore an altered baseline or classify a timeout as PASS.
        try:
            receipt['changedInputs'] = preservation_check(tracked)
            source_unchanged = build is not None and source_before is not None and source_before == source_paths(build)
            baseline_unchanged = baseline_files is not None and baseline_files == {
                p.resolve() for p in baseline_dir.rglob('*') if p.is_file()}
            receipt['sourceInputSetUnchanged'] = source_unchanged
            receipt['baselineArtifactSetUnchanged'] = baseline_unchanged
            receipt['preservationVerified'] = source_unchanged and baseline_unchanged and not receipt['changedInputs']
            require(receipt['preservationVerified'], 'baseline/source/tool/config preservation not fully verified')
            verify_product_receipt(Path(baseline['productBuildReceipt']['path']), build,
                                   build / 'src/d3d9/d3d9.dll')
        except Exception as exc:
            receipt['ok'] = False
            receipt['preservationError'] = str(exc)
        receipt['artifacts'] = [identity(p) for p in sorted(output.rglob('*')) if p.is_file()]
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps(dict(ok=receipt['ok'], receipt=str(output / 'receipt.json'),
                          error=receipt.get('error'), preservationError=receipt.get('preservationError')),
                     ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
