"""Pinned corrected-VVL control/candidate diagnostic, not full C2 acceptance.

An explicit previous-host control retains the old failure. A fresh candidate
build receives only the existing copy-TU target/timing instrumentation. Shared
strict readers, original timeouts, non-input Desktop and independent raw VVL
logging are unchanged. No VUID filters, settings overrides or layer install.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

import diagnose_surface_owner_vvl as b2
import replay_surface_owner_continuation as continuation
import diagnose_render_host_surface_owner as surface
import diagnose_render_host_initialization as init
import run_render_host_validation_gate as validation
from run_render_host_candidate_build import sources as build_sources, PROTECTED, TARGET
from run_render_host_core_gate import check_host_compile_boundary
from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json
from render_lab_palette_watcher import FrozenImage, PE_PATH, PDB_PATH
from render_lab_raw_artifacts import raw_manifests, verify_raw_artifacts

LAYER_ROOT = ROOT/'AutoTest/artifacts/vvl-official-edeeee0-20261022'
LAYER_PINS = {
    'Validation_Layers_Windows_64.zip': (6309913, 'CF02EC73059E48669C3424CBA9B221FAC81CD05B3C987866DEFE4F9C2E31A170'),
    'unpacked/VkLayer_khronos_validation.json': (132861, '4DD2415B7AC11E4288DEACAC0E349AF379D66C52F4132D0C2CDD5507E2EF4D92'),
    'unpacked/VkLayer_khronos_validation.dll': (20856320, '2046514E10B45FCAD82F24F32ACA670E6CE3CA121D577BA0E9E69E0BF5D1F5DD'),
}
HEADER = ROOT/'src/vulkan/vulkan_loader.h'
HEADER_SHA = '60C10887318399F739B1CFFE2F04D9E0EA114DCD8F99768A412FC2653D5464CB'
PREIMAGE = ROOT/'AutoTest/artifacts/vulkan-dispatch-preimage-20261022/vulkan_loader.h'
PREIMAGE_SHA = '6C82092B89F0D97FCCE88B6A8637B3ABDDF8994DD8A32B8A640B4CCD51FDBEB5'
FAILED_VVL = ROOT/'AutoTest/artifacts/x64-surface-owner-vvl-20261022-r1/receipt.json'
FAILED_VVL_SHA = '850F39FC0C95A7C65AC83AEB070D3712ADC025D3CA3BAEEEF7751BCB6392DF1D'


def check_layer_manifest(document):
    layer = document['layer']
    require(layer['name'] == validation.LAYER and layer['api_version'] == '1.4.365'
            and layer['type'] == 'GLOBAL' and layer['library_path'] == '.\\VkLayer_khronos_validation.dll',
            'only the exact frozen official layer recipe is permitted')
    keys = set()
    for node in layer['features']['settings']:
        keys.update(validation.setting_keys(node))
    require(set(validation.VVL_SETTINGS) | {'log_filename'} <= keys, 'requested controls not supported')


def bind_layer(frozen):
    for relative, (size, digest) in LAYER_PINS.items():
        row = identity(LAYER_ROOT/relative)
        require((row['bytes'], row['sha256']) == (size, digest), 'official layer identity differs: '+relative)
        frozen.append(row)
    manifest = LAYER_ROOT/'unpacked/VkLayer_khronos_validation.json'
    library = manifest.with_suffix('.dll')
    check_layer_manifest(strict_json(manifest.read_text('utf-8')))
    require(pe_identity(library) == (0x8664, 0x20b), 'exact x64 layer required')
    loader = (Path(os.environ['SYSTEMROOT'])/'System32/vulkan-1.dll').resolve(strict=True)
    require(pe_identity(loader) == (0x8664, 0x20b), 'system x64 loader required')
    frozen.append(identity(loader))
    return manifest, library, loader


def bind_fixtures(frozen):
    require(identity(init.BASELINE)['sha256'] == init.BASELINE_SHA, 'old r10 receipt changed')
    old = strict_json(init.BASELINE.read_text('utf-8'))
    frozen.append(identity(init.BASELINE))
    require(type(old['compilers']) is list and len(old['compilers']) == 2, 'original x86/x64 compiler identities required')
    for compiler in old['compilers']:
        init.copied.same_file(compiler)
        frozen.append(compiler)
    binaries = {}
    for role, (name, size, digest) in continuation.BINARY_PINS.items():
        rows = [r for r in old['binaries'] if Path(r['path']).name == name]
        require(len(rows) == 1 and (rows[0]['bytes'], rows[0]['sha256']) == (size, digest),
                'unchanged exact fixture required: '+role)
        init.copied.same_file(rows[0]); frozen.append(rows[0])
        binaries[role] = Path(rows[0]['path'])
        require(pe_identity(binaries[role]) == ((0x8664, 0x20b) if role == 'probe64' else (0x14c, 0x10b)),
                'fixture bitness changed')
    for name in ('playerBefore', 'referenceBefore'):
        init.copied.same_file(old[name]); frozen.append(old[name])
    return old, binaries


def bind_previous_host(frozen):
    require(identity(continuation.BASELINE)['sha256'] == continuation.BASELINE_SHA, 'old B2 receipt changed')
    old = strict_json(continuation.BASELINE.read_text('utf-8'))
    row = old['diagnosticHost']
    require((row['bytes'], row['sha256']) == (continuation.HOST_BYTES, continuation.HOST_SHA), 'old control host changed')
    init.copied.same_file(row)
    frozen.extend((identity(continuation.BASELINE), row))
    for raw in raw_manifests(old):
        frozen.extend(verify_raw_artifacts(raw))
    return Path(row['path'])


def check_build_record(record, path):
    require(record['schema'] == 'warvk-x64-candidate-build/v1' and record['ok'] is True
            and record['sourcesPreserved'] is True and record['protectedUnchanged'] is True
            and record['nativeExecuted'] is False, 'complete fresh build receipt required')
    require(record['productIntegrated'] is False and record['releaseReady'] is False
            and record['priority'] == 'BelowNormal' and type(record['parallelism']) is int
            and record['parallelism'] == 2, 'build scope/lease changed')
    build = Path(record['build']).resolve(strict=True)
    require(build.parent == ROOT and build.name.startswith('build64_render_host_'), 'dedicated candidate build')
    expected_paths = dict(host=build/TARGET, compileCommands=build/'compile_commands.json', buildNinja=build/'build.ninja')
    for key, expected in expected_paths.items():
        require(Path(record[key]['path']) == expected and record[key] == identity(expected), 'wrong candidate '+key)
    require(pe_identity(expected_paths['host']) == (0x8664, 0x20b), 'actual host must be build-owned x64 PE32+')
    require(record['sources'] == [identity(p) for p in build_sources()], 'candidate source identity drift')
    protected = [identity(p) for p in PROTECTED]
    require(record['protectedBefore'] == record['protectedAfter'] == protected, 'complete protected file set required')
    require(record['allowedSharedD3D9Inputs'] == check_host_compile_boundary(build)
            == sorted(str(Path(p)) for p in ('src/d3d9/d3d9_options.cpp', 'src/d3d9/d3d9_shader_helpers.cpp')),
            'actual compile boundary changed')
    options = ['-D'+k+'=false' for k in ('enable_dxgi', 'enable_d3d8', 'enable_d3d9', 'enable_d3d10', 'enable_d3d11')]
    expected_commands = [list(map(str, [Path(sys.executable), '-m', 'mesonbuild.mesonmain', 'setup', build, ROOT,
        '--buildtype=release', '--cross-file', ROOT/'build-win64-render-host-llvm.txt',
        '-Dwarvk_render_host_core_dev=true', *options])),
        list(map(str, ['ninja', '-C', build, '-j2', TARGET.as_posix()])),
        list(map(str, ['ninja', '-C', build, '-n', TARGET.as_posix()]))]
    require(type(record['steps']) is list and len(record['steps']) == 3
            and type(record['stepRawArtifacts']) is list and len(record['stepRawArtifacts']) == 3,
            'complete configure/build/no-work commands and first raw seals required')
    for row, command, raw, name in zip(record['steps'], expected_commands, record['stepRawArtifacts'],
                                      ('configure', 'build', 'no-work')):
        require(row['command'] == command and type(row['exit']) is int and row['exit'] == 0
                and type(row['seconds']) in (int, float) and 0 <= row['seconds'] < 1801, 'build step failed/changed')
        verify_raw_artifacts(raw, path.parent, [name+'.stdout', name+'.stderr'], False)
    require('no work to do' in (path.parent/'no-work.stdout').read_text('utf-8'), 'actual build not settled')
    return build


def bind_build(path, frozen):
    record = strict_json(path.read_text('utf-8'))
    build = check_build_record(record, path)
    rows = record['sources'] + record['protectedBefore']
    rows += [record[k] for k in ('host', 'compileCommands', 'buildNinja')] + [identity(path)]
    for raw in raw_manifests(record):
        rows.extend(verify_raw_artifacts(raw))
    for row in rows:
        init.copied.same_file(row)
    frozen.extend(rows)
    return record, build


def preserve_failure_and_deadlines(frozen):
    require(identity(FAILED_VVL)['sha256'] == FAILED_VVL_SHA, 'original failed VVL receipt changed')
    failed = strict_json(FAILED_VVL.read_text('utf-8'))
    require(failed['diagnosticComplete'] is True and failed['vvlChecksPassed'] is False
            and failed['acceptancePassed'] is False, 'old failure must remain failed')
    frozen.append(identity(FAILED_VVL))
    for raw in raw_manifests(failed):
        frozen.extend(verify_raw_artifacts(raw))
    transport = ROOT/'tools/render_host/win32_transport.h'
    connection = ROOT/'tools/render_host/d3d9_connection.h'
    require('inline constexpr DWORD DeadlineMs=5000;' in transport.read_text('utf-8')
            and 'childExit=child.wait(5000);' in connection.read_text('utf-8'), 'original 5s transport/exit changed')
    frozen.extend(identity(p) for p in (transport, connection))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--host', choices=('previous', 'candidate'), required=True)
    ap.add_argument('--build-receipt', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--execute-native', action='store_true')
    args = ap.parse_args(); out = args.output.resolve()
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh artifact child')
    require((args.host == 'candidate') == (args.build_receipt is not None), 'candidate build identity required only for candidate')
    out.mkdir()
    receipt = dict(schema=1, scope='B2_PINNED_CORRECTED_VVL_CONTROL_OR_CANDIDATE_NOT_C2', hostRole=args.host,
        prepared=False, nativeRequested=args.execute_native, copies=[], cases=[], steps=[], stepRawArtifacts=[],
        acceptancePassed=False, completeC2AcceptancePassed=False, productIntegrated=False, gameTouched=False,
        memorySavingsProven=False, originalTransportMilliseconds=5000, originalExitMilliseconds=5000,
        globalEnvironmentModified=False, systemChanged=False, registryModified=False,
        originalC2GateUnmodified=True, oldFailurePreserved=True, correctedLayerCommit='edeeee0ff5ce6d79d0c4ccfc28aa9c18e2b94144',
        correctedLayerOfficialRelease=420859907, correctedLayerAsset=666190310,
        priority='BelowNormal', parallelism=1)
    frozen = []
    try:
        old, binaries = bind_fixtures(frozen)
        preserve_failure_and_deadlines(frozen)
        manifest, library, loader = bind_layer(frozen)
        receipt['layer'] = dict(manifest=identity(manifest), library=identity(library), loader=identity(loader),
            apiVersion='1.4.365', requestedSettings=validation.VVL_SETTINGS, noFiltersAndNoLimitRequested=True,
            runtimeFilterLimitProven=False)
        palette = FrozenImage.load(); receipt['paletteFrozenImage'] = palette.receipt()
        if args.host == 'previous':
            binaries['host'] = bind_previous_host(frozen)
        else:
            record, build = bind_build(args.build_receipt.resolve(strict=True), frozen)
            receipt['candidateBuildReceipt'] = identity(args.build_receipt.resolve())
            require(identity(HEADER)['sha256'] == HEADER_SHA and identity(PREIMAGE)['sha256'] == PREIMAGE_SHA,
                    'exact dispatch source/preimage required')
            frozen.extend((identity(HEADER), identity(PREIMAGE), identity(surface.HEADER)))
            source, edits, timing = surface.instrument_source(init.BACKEND.read_text('utf-8'))
            target = out/'d3d9_backend_surface_diagnostic.cpp'
            # Mechanical copy-TU transformation; formal C++ source is not changed.
            target.write_text(source, encoding='utf-8', newline='\n')
            receipt.update(diagnosticSource=identity(target), targetEdits=edits, timingEdits=timing)
            frozen.append(identity(target))
            if args.execute_native:
                env = dict(os.environ)
                env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;'+env['PATH']
                binaries['host'] = init.build_diagnostic(old, build, target, out, env, receipt, frozen)
                frozen.append(identity(binaries['host']))
        helpers = {Path(__file__).resolve(), Path(__file__).with_name('test_surface_owner_candidate_vvl.py'), PE_PATH, PDB_PATH}
        helpers.update(Path(m.__file__).resolve() for m in list(sys.modules.values()) if getattr(m, '__file__', None)
            and Path(m.__file__).suffix == '.py' and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen.extend(identity(p) for p in sorted(helpers)); receipt['prepared'] = True
        receipt['binaries'] = {k: identity(v) for k, v in binaries.items()}
        if args.execute_native:
            b2.run_native(binaries, out, palette, manifest, library, loader, receipt, frozen)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            receipt['frozenInputs'] = continuation.unique_inputs(frozen)
            b2.preserve_now(frozen); receipt['frozenInputsPreserved'] = bool(frozen)
            if 'implicitSourceBefore' in receipt:
                receipt['implicitSourceAfter'] = b2.empty_directory_witness(Path(receipt['implicitSourceBefore']['path']))
                require(receipt['implicitSourceBefore'] == receipt['implicitSourceAfter'], 'private layer discovery changed')
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
        b2.summarize(receipt)
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('hostRole', 'preparationComplete', 'diagnosticComplete',
        'vvlChecksPassed', 'error', 'preservationError', 'rawPreservationError')}))
    return 0 if (receipt['vvlChecksPassed'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
