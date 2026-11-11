"""Two frozen host-target B2 cases under VVL; NOT the original C2 gate.

Default: verify inputs and prepare only. --execute-native is solely for the root
native-lease owner. No build, layer installation, arbitrary Vulkan controls, ICD
redirect, source/system edit, old receipt rewrite, or product acceptance occurs.
The original C2 gate still requires its complete passing input receipts.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import os
from pathlib import Path
import sys

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/replay_surface_owner_continuation.py').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))
import replay_surface_owner_continuation as continuation
import run_render_host_validation_gate as validation
from render_lab_layer_source import create_empty_source, empty_directory_witness
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests
from run_render_host_protocol_gate import identity, pe_identity
from run_render_host_transport_gate import require
from run_render_host_d3d9_gate import json_lines
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_palette_watcher import FrozenImage, PE_PATH, PDB_PATH

LAYER_MANIFEST = Path('E:/Dev/Vulkan SDK/Bin/VkLayer_khronos_validation.json')
CASES = (continuation.CASES[1], continuation.CASES[11])
DXVK_LOG = 'warvk-d3d9-host-dev_warvk-d3d9-host.log'
EXTRA_RAW_NAMES = ('validation.raw.log', DXVK_LOG)
FORBIDDEN_CONTROLS = frozenset(('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES',
    'VK_ADD_IMPLICIT_LAYER_PATH', 'VK_INSTANCE_LAYERS', 'VK_LOADER_DRIVERS_SELECT', 'VK_LOADER_DRIVERS_DISABLE'))
SCOPE = 'B2_FROZEN_HOST_TARGET_TWO_CASE_VVL_DIAGNOSTIC_NOT_C2_ACCEPTANCE'


def checked_case(case):
    continuation.checked_case(case)
    require(case in CASES and case.mode == 'host' and
            (case.index, case.kind, case.seconds) in ((2, 'soak', 45), (12, 'shader', 90)),
            'only host-target soak256 and shader8 are in this diagnostic')
    return case


def check_controls(controls, directory, manifest, empty_source):
    """Exact existing recipe, not generic VK_* permission or user passthrough."""
    require(type(controls) is dict and all(type(k) is str and type(v) is str for k, v in controls.items()),
            'exact validation string-map required')
    expected = validation.validation_env(directory, manifest, empty_source)
    require(controls == expected and not FORBIDDEN_CONTROLS.intersection(controls),
            'only the unchanged validation_env recipe is permitted; no ICD/extra layer override')
    require(controls['VK_IMPLICIT_LAYER_PATH'] == str(empty_source)
            and controls['VK_LAYER_PATH'] == str(manifest.parent)
            and controls['VK_KHRONOS_VALIDATION_LOG_FILENAME'] == str(directory/'validation.raw.log'),
            'validation source/log paths must be explicit per-run paths')
    return controls


def controls_for(case, directory, manifest, empty_source):
    checked_case(case)
    return check_controls(validation.validation_env(directory, manifest, empty_source), directory, manifest, empty_source)


def check_actual_environment(case, row, directory, manifest, empty_source):
    checked_case(case)
    controls = controls_for(case, directory, manifest, empty_source)
    check_controls(row['requestedDiagnosticControls'], directory, manifest, empty_source)
    require(type(row['actualSafeEnvironment']) is dict and all(type(k) is str and type(v) is str
            and k == k.upper() for k, v in row['actualSafeEnvironment'].items()), 'actual environment string-map')
    actual = {k: v for k, v in row['actualSafeEnvironment'].items() if k not in continuation.init.ENV_ALLOW}
    expected = continuation.environment(case, {}, directory, directory/'warvk-d3d9-host-dev.exe', controls)
    require(actual == expected and not FORBIDDEN_CONTROLS.intersection(actual),
            'actual child controls differ from exact B2 validation recipe')
    require(row['extraRawNames'] == list(EXTRA_RAW_NAMES), 'independent validation/DXVK logs not initially required')


def inspect_frozen_layer():
    manifest = LAYER_MANIFEST.resolve(strict=True)
    before = identity(manifest)
    library = validation.inspect_layer(manifest)
    require(identity(manifest) == before, 'manifest changed during inspection')
    loader = (Path(os.environ['SYSTEMROOT'])/'System32/vulkan-1.dll').resolve(strict=True)
    require(pe_identity(loader) == (0x8664, 0x20b), 'frozen system loader must be x64')
    return manifest, library, loader, [before, identity(library), identity(loader)]


def check_result_scope(case, row):
    checked_case(case)
    require(type(row) is dict and all(row.get(k) == v and type(row.get(k)) is type(v)
            for k, v in asdict(case).items()) and row.get('acceptancePassed') is False,
            'single-case result does not match the planned diagnostic')
    require(row.get('settlementCertified') is True and row.get('childSettlement') in continuation.SETTLED,
            'unsettled case: do not advance to another native case')


def parse_evidence(host_text, dxvk_text, raw_text, library, empty_source, loader):
    """Pure actual evidence readers; all messages remain in the frozen raw files."""
    checks = {}; errors = []
    for name, call in (
        ('loader', lambda: validation.check_loader(host_text, dxvk_text, library, empty_source, loader)),
        ('validation', lambda: validation.parse_validation(raw_text))):
        try:
            checks[name] = call()
        except Exception as error:
            errors.append(name+': '+str(error))
    checks.update(errors=errors, checksPassed=not errors, acceptancePassed=False,
        runtimeFilterLimitProven=False, completeC2AcceptancePassed=False)
    return checks


def analyze_case(case, row, directory, manifest, library, loader, empty_source):
    check_result_scope(case, row)
    result = dict(checksPassed=False, acceptancePassed=False, completeC2AcceptancePassed=False,
                  runtimeFilterLimitProven=False, errors=[])
    row['vvlDiagnostic'] = result
    try:
        check_actual_environment(case, row, directory, manifest, empty_source)
        first = row['initialRawArtifacts']
        require(set(continuation.init.RAW_NAMES+list(EXTRA_RAW_NAMES)) <= set(first['requiredNames']),
                'validation files were not required at initial launch return')
        verify_raw_artifacts(first, directory=directory, include_logs=True)
        paths = {'raw': directory/'validation.raw.log', 'host': directory/'host.log', 'dxvk': directory/DXVK_LOG}
        for kind, path in paths.items():
            require(path.is_file() and 0 < path.stat().st_size <= 32*1024*1024,
                    'missing/empty/oversized '+kind+' log; original bytes retained')
        result['rawFiles'] = {kind: identity(path) for kind, path in paths.items()}
        result.update(parse_evidence(paths['host'].read_text('utf-8'), paths['dxvk'].read_text('utf-8'),
            paths['raw'].read_text('utf-8'), library, empty_source, loader))
    except Exception as error:
        result['errors'].append(str(error))
    result['caseChecksPassed'] = bool(result['checksPassed'] and row['workloadPassed']
                                     and row['diagnosticCollected'])
    # Never rewrite continuation's native failure or turn VVL activation alone
    # into success; an expected failed case stays present in the two-case record.
    return row


def execute_two_cases(run_one):
    rows = []
    for case in CASES:
        row = run_one(case)
        check_result_scope(case, row)
        rows.append(row)
    return rows


def preserve_now(frozen):
    for row in continuation.unique_inputs(frozen):
        continuation.init.copied.same_file(row)


def run_native(binaries, out, palette, manifest, library, loader, receipt, frozen):
    require(os.name == 'nt', 'root-owned Windows native lease only')
    receipt['implicitSourceBefore'] = create_empty_source(out)
    empty_source = Path(receipt['implicitSourceBefore']['path'])
    probes = {bits: continuation.copy_binary(binaries[f'probe{bits}'], out/f'desktop-probe-{bits}.exe', receipt, frozen)
              for bits in (32, 64)}
    with IsolatedRenderDesktop(restricted_medium=True) as desktop:
        receipt['desktopSafety'] = desktop.receipt
        base = continuation.init.recipe_environment(dict(os.environ), out, binaries['host'])
        probe_env = {k: v for k, v in base.items() if k in continuation.init.ENV_ALLOW
                     or k in ('DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE')}
        try:
            receipt['probe'] = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before, '--child', probes[64],
                '--child-log', out/'probe-child.log'], cwd=out, stdout_path=out/'probe.stdout',
                stderr_path=out/'probe.stderr', timeout_s=30, env=probe_env)
        finally:
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log'], True)
        check_launch(receipt['probe'], desktop.desktop_name, desktop.input_before)
        rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
        require(len(rows) == 2, 'actual x86/x64 non-input Desktop probe population')
        receipt['probeReports'] = rows
        validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
            station='WinSta0', bits=32, child=rows[1], launch=receipt['probe'])
        def run(case):
            preserve_now(frozen)
            require(empty_directory_witness(empty_source) == receipt['implicitSourceBefore'],
                    'private implicit source changed before case')
            directory = out/case.label
            controls = controls_for(case, directory, manifest, empty_source)
            row = continuation.run_one_case(case, binaries, out, desktop, palette, receipt, frozen,
                controls=controls, extra_raw_names=list(EXTRA_RAW_NAMES))
            return analyze_case(case, row, directory, manifest, library, loader, empty_source)
        execute_two_cases(run)
    safety = receipt['desktopSafety']
    require(safety['desktopClosed'] is True and safety['cleanupErrors'] == []
            and safety['inputAfterClose'] == safety['inputBefore'], 'isolated Desktop cleanup incomplete')


def summarize(receipt):
    rows = receipt['cases']
    cases_match = len(rows) == 2
    if cases_match:
        try:
            for case, row in zip(CASES, rows):
                check_result_scope(case, row)
        except (ValueError, KeyError, TypeError):
            cases_match = False
    clean = not any(k in receipt for k in ('error', 'preservationError', 'rawPreservationError'))
    receipt['preparationComplete'] = bool(receipt['prepared'] and receipt.get('frozenInputsPreserved') and clean)
    receipt['diagnosticComplete'] = bool(receipt['nativeRequested'] and receipt['preparationComplete']
        and receipt.get('rawArtifactsPreserved') and cases_match
        and all(r.get('settlementCertified') is True and r.get('diagnosticCollected') is True for r in rows)
        and receipt.get('implicitSourcePreserved') is True)
    receipt['vvlChecksPassed'] = bool(receipt['diagnosticComplete']
        and all(r.get('workloadPassed') is True
                and r.get('vvlDiagnostic', {}).get('checksPassed') is True
                and r.get('vvlDiagnostic', {}).get('caseChecksPassed') is True for r in rows))
    receipt['acceptancePassed'] = False
    receipt['completeC2AcceptancePassed'] = False
    receipt['peerWindowAcceptancePassed'] = False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--execute-native', action='store_true', help='root native lease only; no compilation')
    args = parser.parse_args(); out = args.output.resolve()
    require(out.is_relative_to(ROOT/'AutoTest/artifacts') and out != ROOT/'AutoTest/artifacts', 'fresh artifact subdirectory')
    out.mkdir(exist_ok=False)
    receipt = dict(schema=1, scope=SCOPE, acceptancePassed=False, completeC2AcceptancePassed=False,
        peerWindowAcceptancePassed=False, originalC2GateUnmodified=True, prepared=False,
        nativeRequested=args.execute_native, rootNativeLeaseRequired=True, diagnosticComplete=False,
        gameTouched=False, productIntegrated=False, memorySavingsProven=False, performanceMeasured=False,
        windowPixelsVerified=False, playerVisualAccepted=False, runtimeFilterLimitProven=False,
        globalEnvironmentModified=False, systemChanged=False, registryModified=False, buildsAllowed=False,
        restrictedMediumRequested=True, sameExeRequired=True, originalTransportMilliseconds=5000,
        originalExitMilliseconds=5000, priority='BelowNormal', parallelism=1,
        plan=[dict(**asdict(c), label=c.label) for c in CASES], copies=[], cases=[])
    frozen = []
    try:
        baseline, _, binaries, inputs = continuation.bind_inputs(); frozen.extend(inputs)
        receipt.update(baselineReceipt=identity(continuation.BASELINE), r10Receipt=identity(continuation.init.BASELINE),
            diagnosticHost=baseline['diagnosticHost'], readerRevisions=continuation.reader_revisions(),
            binaries={k: identity(v) for k, v in binaries.items()})
        manifest, library, loader, layer_inputs = inspect_frozen_layer(); frozen.extend(layer_inputs)
        receipt['layer'] = dict(manifest=layer_inputs[0], library=layer_inputs[1], loader=layer_inputs[2],
            apiVersion='1.4.335', requestedSettings=validation.VVL_SETTINGS,
            requestedSettingsSha256=validation.digest_json(validation.VVL_SETTINGS),
            requestedNoFiltersAndNoLimit=True, activationIsNotInstallation=True,
            runtimeFilterLimitProven=False, shaderAccessHeuristic=False, gpuAssistedValidation=False)
        palette = FrozenImage.load(); receipt['paletteFrozenImage'] = palette.receipt()
        helpers = {Path(__file__).resolve(), Path(__file__).with_name('test_surface_owner_vvl.py').resolve(), PE_PATH, PDB_PATH}
        helpers.update(Path(m.__file__).resolve() for m in list(sys.modules.values())
            if getattr(m, '__file__', None) and Path(m.__file__).suffix == '.py'
            and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen.extend(identity(p) for p in sorted(helpers)); receipt['prepared'] = True
        if args.execute_native:
            run_native(binaries, out, palette, manifest, library, loader, receipt, frozen)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['frozenInputs'] = frozen
        try:
            receipt['frozenInputs'] = continuation.unique_inputs(frozen)
            preserve_now(frozen); receipt['frozenInputsPreserved'] = bool(frozen)
            if 'implicitSourceBefore' in receipt:
                receipt['implicitSourceAfter'] = empty_directory_witness(Path(receipt['implicitSourceBefore']['path']))
                require(receipt['implicitSourceBefore'] == receipt['implicitSourceAfter'], 'private implicit source changed')
                receipt['implicitSourcePreserved'] = True
        except Exception as error:
            receipt['preservationError'] = str(error)
        try:
            manifests = list(raw_manifests(receipt))
            for item in manifests:
                verify_raw_artifacts(item)
            receipt['rawArtifactsPreserved'] = bool(manifests)
        except Exception as error:
            receipt['rawPreservationError'] = str(error)
        summarize(receipt)
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('acceptancePassed', 'preparationComplete', 'diagnosticComplete',
        'vvlChecksPassed', 'error', 'preservationError', 'rawPreservationError')}))
    return 0 if (receipt['vvlChecksPassed'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    sys.exit(main())
