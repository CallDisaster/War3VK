"""Version-pinned VVL365 diagnostic. Old VVL335 and failed control stay intact.

The only layer recipe change is the documented newer settings/status contract.
Original workload, layer source isolation, raw preservation and deadlines remain.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

import diagnose_surface_owner_candidate_vvl as candidate
import diagnose_surface_owner_vvl as b2
import render_lab_vvl365 as current
from run_render_host_protocol_gate import ROOT, identity
from run_render_host_transport_gate import require
from render_lab_raw_artifacts import capture_raw_artifacts, raw_manifests, verify_raw_artifacts
from render_lab_layer_source import create_empty_source, empty_directory_witness
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from run_render_host_d3d9_gate import json_lines
from run_render_host_presenter_gate import check_launch

continuation = b2.continuation
init = continuation.init


def run_native(binaries, out, palette, manifest, library, loader, receipt, frozen):
    receipt['implicitSourceBefore'] = create_empty_source(out)
    empty = Path(receipt['implicitSourceBefore']['path'])
    probes = {bits: continuation.copy_binary(binaries[f'probe{bits}'], out/f'desktop-probe-{bits}.exe', receipt, frozen)
              for bits in (32, 64)}
    with IsolatedRenderDesktop(restricted_medium=True) as desktop:
        receipt['desktopSafety'] = desktop.receipt
        base = init.recipe_environment(dict(os.environ), out, binaries['host'])
        env = {k:v for k,v in base.items() if k in init.ENV_ALLOW or k in ('DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE')}
        try:
            receipt['probe'] = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before, '--child', probes[64],
                '--child-log', out/'probe-child.log'], cwd=out, stdout_path=out/'probe.stdout',
                stderr_path=out/'probe.stderr', timeout_s=30, env=env)
        finally:
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log'], True)
        check_launch(receipt['probe'], desktop.desktop_name, desktop.input_before)
        rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
        require(len(rows) == 2, 'actual x86/x64 non-input probes')
        receipt['probeReports'] = rows
        validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
            station='WinSta0', bits=32, child=rows[1], launch=receipt['probe'])
        for case in b2.CASES:
            b2.preserve_now(frozen)
            require(empty_directory_witness(empty) == receipt['implicitSourceBefore'], 'private layer source drift')
            directory = out/case.label
            controls = current.validation_env(directory, manifest, empty)
            row = continuation.run_one_case(case, binaries, out, desktop, palette, receipt, frozen,
                controls=controls, extra_raw_names=list(b2.EXTRA_RAW_NAMES))
            b2.check_result_scope(case, row)
            expected = continuation.environment(case, {}, directory, directory/'warvk-d3d9-host-dev.exe', controls)
            actual = {k:v for k,v in row['actualSafeEnvironment'].items() if k not in init.ENV_ALLOW}
            require(actual == expected and row['requestedDiagnosticControls'] == controls, 'actual versioned environment differs')
            verify_raw_artifacts(row['initialRawArtifacts'], directory,
                init.RAW_NAMES+list(b2.EXTRA_RAW_NAMES)+[p.name for p in directory.glob('exit-snapshot-*.json')], True)
            result = dict(checksPassed=False, caseChecksPassed=False, acceptancePassed=False, errors=[])
            row['vvlDiagnostic'] = result
            try:
                for log in ('host.log', b2.DXVK_LOG, 'validation.raw.log'):
                    require((directory/log).is_file() and 0 < (directory/log).stat().st_size <= 32*1024*1024,
                            'missing/empty/oversized '+log+'; original bytes retained')
                result['loader'] = b2.validation.check_loader((directory/'host.log').read_text('utf-8'),
                    (directory/b2.DXVK_LOG).read_text('utf-8'), library, empty, loader)
                result['validation'] = current.parse_validation((directory/'validation.raw.log').read_text('utf-8'))
                result['checksPassed'] = True
                result['caseChecksPassed'] = row['workloadPassed'] and row['diagnosticCollected']
            except Exception as error:
                result['errors'].append(str(error))
    require(receipt['desktopSafety']['ok'] is True, 'Desktop cleanup failed')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', choices=('previous', 'candidate'), required=True)
    ap.add_argument('--build-receipt', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--execute-native', action='store_true')
    args = ap.parse_args(); out = args.output.resolve()
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh artifact directory')
    require((args.host == 'candidate') == (args.build_receipt is not None), 'exact candidate build required')
    out.mkdir()
    receipt = dict(schema=1, scope='B2_VVL365_VERSIONED_TWO_CASE_NOT_FULL_C2', hostRole=args.host,
        prepared=False, nativeRequested=args.execute_native, copies=[], cases=[], steps=[], stepRawArtifacts=[],
        acceptancePassed=False, completeC2AcceptancePassed=False, productIntegrated=False, gameTouched=False,
        memorySavingsProven=False, originalTransportMilliseconds=5000, originalExitMilliseconds=5000,
        globalEnvironmentModified=False, systemChanged=False, registryModified=False, oldFailurePreserved=True,
        correctedLayerCommit='edeeee0ff5ce6d79d0c4ccfc28aa9c18e2b94144', priority='BelowNormal', parallelism=1)
    frozen = []
    try:
        old, binaries = candidate.bind_fixtures(frozen)
        candidate.preserve_failure_and_deadlines(frozen)
        # Preserve the first new-layer control too. It exposed a real outdated
        # diagnostic setting and remains a failed run, not retroactively green.
        failed_control = ROOT/'AutoTest/artifacts/x64-dispatch-vvl-control-20261023-r1/receipt.json'
        require(identity(failed_control)['sha256'] == '8F82518E8EB56D220DA786231680FED3F5655077AD05D0BE2783BF0C82FEFFC2',
                'initial new-layer control receipt changed')
        failed = b2.validation.strict_json(failed_control.read_text('utf-8'))
        require(failed['vvlChecksPassed'] is False and failed['hostRole'] == 'previous', 'initial control failure must remain')
        frozen.append(identity(failed_control))
        for raw in raw_manifests(failed):
            frozen.extend(verify_raw_artifacts(raw))
        manifest, library, loader = candidate.bind_layer(frozen)
        current.check_manifest(b2.validation.strict_json(manifest.read_text('utf-8')))
        receipt['layer'] = dict(manifest=identity(manifest), library=identity(library), loader=identity(loader),
            requestedSettings=current.SETTINGS, apiVersion='1.4.365', runtimeFilterLimitProven=False)
        palette = b2.FrozenImage.load(); receipt['paletteFrozenImage'] = palette.receipt()
        if args.host == 'previous':
            binaries['host'] = candidate.bind_previous_host(frozen)
        else:
            record, build = candidate.bind_build(args.build_receipt.resolve(strict=True), frozen)
            receipt['candidateBuildReceipt'] = identity(args.build_receipt.resolve())
            require(identity(candidate.HEADER)['sha256'] == candidate.HEADER_SHA
                    and identity(candidate.PREIMAGE)['sha256'] == candidate.PREIMAGE_SHA, 'dispatch revision identity')
            frozen.extend((identity(candidate.PREIMAGE), identity(candidate.surface.HEADER)))
            source, edits, timings = candidate.surface.instrument_source(init.BACKEND.read_text('utf-8'))
            target = out/'d3d9_backend_surface_diagnostic.cpp'
            target.write_text(source, encoding='utf-8', newline='\n')
            receipt.update(diagnosticSource=identity(target), targetEdits=edits, timingEdits=timings)
            frozen.append(identity(target))
            if args.execute_native:
                env = dict(os.environ)
                env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;'+env['PATH']
                binaries['host'] = init.build_diagnostic(old, build, target, out, env, receipt, frozen)
                frozen.append(identity(binaries['host']))
        helpers = {Path(__file__).resolve(), ROOT/'AutoTest/test_render_lab_vvl365.py', b2.PE_PATH, b2.PDB_PATH}
        helpers.update(Path(m.__file__).resolve() for m in list(sys.modules.values()) if getattr(m, '__file__', None)
            and Path(m.__file__).suffix == '.py' and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen.extend(identity(p) for p in sorted(helpers)); receipt['prepared'] = True
        receipt['binaries'] = {k: identity(v) for k,v in binaries.items()}
        if args.execute_native:
            run_native(binaries, out, palette, manifest, library, loader, receipt, frozen)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            receipt['frozenInputs'] = continuation.unique_inputs(frozen)
            b2.preserve_now(frozen); receipt['frozenInputsPreserved'] = bool(frozen)
            if 'implicitSourceBefore' in receipt:
                receipt['implicitSourceAfter'] = empty_directory_witness(Path(receipt['implicitSourceBefore']['path']))
                require(receipt['implicitSourceAfter'] == receipt['implicitSourceBefore'], 'layer source drift')
                receipt['implicitSourcePreserved'] = True
            for raw in raw_manifests(receipt):
                verify_raw_artifacts(raw)
            receipt['rawArtifactsPreserved'] = bool(list(raw_manifests(receipt)))
        except Exception as error:
            receipt['preservationError'] = str(error)
        b2.summarize(receipt)
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('hostRole', 'preparationComplete', 'diagnosticComplete',
        'vvlChecksPassed', 'error', 'preservationError')}))
    return 0 if (receipt['vvlChecksPassed'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
