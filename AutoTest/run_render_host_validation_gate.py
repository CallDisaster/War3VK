"""Replay five frozen C2 cases with the installed x64 VVL, without rebuilding.

Only the main agent may grant the native/GPU lease. The two input receipts must
be complete passing Presenter and R1/Q1 gates, not successful fragments of a
failed run. All Vulkan controls are child-process environment entries; no SDK,
registry, system, game, source, driver or global environment changes are made.

VVL's own LOG_MSG callback is explicitly non-default and therefore independent
of DXVK's nine ignored message IDs. Its activation message proves activation
and synchronization validation, NOT whether every requested reporting control
was obeyed: runtimeFilterLimitProven remains false. This is not a timing gate.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_d3d9_gate import json_lines
from run_render_host_presenter_gate import (
    check_case_records, check_case_record_mutations, check_soak, check_wsi,
    check_reader_mutations, check_launch)
from run_render_host_resource_lifetime_gate import (
    check_case as check_lifetime, mutations as lifetime_mutations,
    read_records as lifetime_records)
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_layer_source import (create_empty_source, empty_directory_witness,
    check_empty_source_trace, loader_records, check_loader_module_trace)
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests


LAYER = 'VK_LAYER_KHRONOS_validation'
STATUS_ID = 'WARNING-CreateInstance-status-message'
SYNC_ENABLE = 'VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT'
CACHE_DISABLE = 'VK_VALIDATION_FEATURE_DISABLE_SHADER_VALIDATION_CACHING_EXT'
ENV_ALLOW = frozenset(('SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'TEMP', 'TMP',
    'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)',
    'PROGRAMDATA', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'))
# These names and values are in the installed SDK Config/vk_layer_settings.sh
# and VkLayer_khronos_validation.json. Do not use deprecated ENABLES/DISABLES.
VVL_SETTINGS = {
    'validate_core': 'true', 'check_image_layout': 'true',
    'check_command_buffer': 'true', 'check_object_in_use': 'true',
    'check_query': 'true', 'check_shaders': 'true',
    'check_shaders_caching': 'false', 'unique_handles': 'true',
    'object_lifetime': 'true', 'stateless_param': 'true', 'thread_safety': 'true',
    'validate_sync': 'true', 'syncval_submit_time_validation': 'true',
    'syncval_shader_accesses_heuristic': 'false',
    'gpuav_enable': 'false', 'printf_enable': 'false',
    'validate_best_practices': 'false', 'legacy_detection': 'false',
    # DEFAULT would allow DXVK's application callback to suppress this logger.
    'debug_action': 'VK_DBG_LAYER_ACTION_LOG_MSG',
    'report_flags': 'info,warn,error,perf', 'enable_message_limit': 'false',
    'message_id_filter': '', 'message_format_json': 'false',
    'message_format_display_application_name': 'true',
}
CASE_SPECS = (
    ('clear-soak', 'presenter', 45),
    ('presenter-eight-frames', 'presenter', 90),
    ('presenter-readback-eight-frames', 'presenter', 90),
    ('com', 'lifetime', 45), ('raw', 'lifetime', 45),
)
VVL_HEADER = re.compile(
    r'^Validation (Error|Warning|Performance Warning|Information): '
    r'\[ ([^\]\r\n]+) \] \| MessageID = 0x([0-9a-fA-F]+)\r?$', re.M)


def digest_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True,
                                    separators=(',', ':')).encode()).hexdigest().upper()


def require_identity(record):
    require(type(record) is dict and set(record) == {'path', 'bytes', 'sha256'},
            'complete frozen file identity required')
    require(type(record['path']) is str and Path(record['path']).is_absolute(),
            'absolute frozen path required')
    require(type(record['bytes']) is int and record['bytes'] >= 0
            and type(record['sha256']) is str and re.fullmatch(r'[0-9A-F]{64}', record['sha256']),
            'exact frozen size/hash required')
    require(identity(Path(record['path'])) == record, 'frozen file changed: ' + record['path'])
    return Path(record['path'])


def one(rows, field, value):
    found = [row for row in rows if type(row) is dict and row.get(field) == value]
    require(len(found) == 1, 'missing/duplicate frozen ' + str(value))
    return found[0]


def case_command(fixture, proxy, label, desktop, input_desktop):
    if label in ('com', 'raw'):
        return [fixture, '--mode', label, '--dll', proxy,
                '--expect-desktop', desktop, '--input-desktop', input_desktop]
    if label == 'clear-soak':
        return [fixture, '--dll', proxy, '--scenario', 'clear-soak',
                '--diagnostic-readback', '1', '--expect-desktop', desktop,
                '--input-desktop', input_desktop]
    require(label in ('presenter-eight-frames', 'presenter-readback-eight-frames'), 'known validation case required')
    return [fixture, '--dll', proxy, '--scenario', 'normal',
            '--wsi-readback' if label == 'presenter-readback-eight-frames' else '--wsi']


def verify_native(reports, hosts, launch, label, desktop, input_desktop,
                  with_mutations=False):
    if label in ('com', 'raw'):
        check_lifetime(reports, hosts, launch, label, desktop, input_desktop, True)
        _, proxy, _, _ = lifetime_records(reports, hosts, label, True)
        mutations = lifetime_mutations(reports, hosts, launch, label, desktop,
            input_desktop, True) if with_mutations else None
    else:
        schema = ('warvk-d3d9-presenter-clear-soak/v1' if label == 'clear-soak'
                  else 'warvk-d3d9-wsi-fixture/v1')
        rows, host_rows = check_case_records(reports, hosts, schema,
                                            'warvk-d3d9-host/v1', host_desktop=True)
        fixture, proxy = rows[schema], rows['warvk-d3d9-proxy/v1']
        host, owner = host_rows['warvk-d3d9-host/v1'], host_rows['warvk-d3d9-desktop/v1']
        if label == 'clear-soak':
            check_soak(fixture, proxy, host, owner, launch, desktop, input_desktop)
        else:
            diagnostic = label == 'presenter-readback-eight-frames'
            check_wsi(fixture, proxy, host, owner, desktop, diagnostic, launch, input_desktop)
            require(owner['peerPid'] == launch['pid'], 'actual WSI launch owner')
        mutations = None
        if with_mutations:
            mutations = dict(records=check_case_record_mutations(reports, hosts,
                schema, 'warvk-d3d9-host/v1', host_desktop=True))
            if label != 'clear-soak':
                mutations['reader'] = check_reader_mutations(fixture, proxy, host,
                    owner, desktop, diagnostic, launch, input_desktop)
    return proxy, mutations


def verify_baseline_raw(receipt, path, kind):
    """Require producer-time hashes, never backfill old successful summaries."""
    require(receipt.get('rawArtifactsPreserved') is True, 'baseline raw artifacts were not frozen and rechecked')
    root = path.parent.resolve(strict=True)
    verify_raw_artifacts(receipt.get('probeRawArtifacts'), root,
                         ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False)
    require(type(receipt.get('stepRawArtifacts')) is list and receipt['stepRawArtifacts'], 'baseline CPU raw manifests missing')
    for manifest in receipt['stepRawArtifacts']:
        verify_raw_artifacts(manifest)
        require(Path(manifest['directory']) == root and manifest['includeLogs'] is False,
                'baseline CPU raw artifact scope')
    for case in receipt['cases']:
        directory = Path(case['process']['stdout']).parent
        require(directory.resolve(strict=True).is_relative_to(root), 'baseline raw case escaped artifact root')
        reference = kind == 'presenter' and case.get('label') in ('reference-normal', 'reference-invalid')
        verify_raw_artifacts(case.get('initialRawArtifacts'), directory,
                             ['case.stdout', 'case.stderr'] + ([] if reference else ['host.log']), True)


def load_baseline(path, kind):
    require(kind in ('presenter', 'lifetime'), 'baseline kind')
    raw = path.read_text('utf-8')
    receipt = strict_json(raw)
    expected_scope = ('D3D9_DXVK_PRESENTER_NONINPUT_DESKTOP' if kind == 'presenter'
                      else 'R1_Q1_ACTUAL_X86_COM_RAW_TO_X64_DXVK')
    require(type(receipt) is dict and receipt.get('ok') is True
            and receipt.get('scope') == expected_scope
            and receipt.get('sourceAndReferencePreserved') is True
            and receipt.get('playerDllChanged') is False, 'complete passing ' + kind + ' receipt required')
    require(receipt['desktopSafety']['ok'] is True, 'baseline desktop did not close')
    require(type(receipt['cases']) is list and receipt['cases']
            and all(case.get('ok') is True for case in receipt['cases']), 'baseline contains failed case')
    for key in ('sources', 'binaries', 'caseCopies'):
        require(type(receipt[key]) is list and receipt[key], 'missing baseline population: ' + key)
    for record in receipt['sources'] + receipt['binaries']:
        require_identity(record)
    for record in receipt['caseCopies']:
        require_identity(record['copy'])
        require(all(record['source'][key] == record['copy'][key]
                    for key in ('bytes', 'sha256')), 'baseline executed copy identity mismatch')
    require_identity(receipt['referenceBefore']); require_identity(receipt['playerBefore'])
    require_identity(receipt['compileCommands'])
    verify_baseline_raw(receipt, path, kind)
    return receipt


def baseline_case(receipt, receipt_path, kind, label):
    case = one(receipt['cases'], 'label' if kind == 'presenter' else 'mode', label)
    desktop, input_desktop = receipt['desktopName'], receipt['desktopSafety']['inputBefore']
    verify_native(case['reports'], case['hostReports'], case['process'], label, desktop, input_desktop)
    # Reload the original native output as well: a JSON summary alone is not
    # permission to relabel a different process/binary as the successful case.
    argv = case['process']['argv']
    fixture = Path(argv[0]); directory = fixture.parent
    require(directory.resolve().is_relative_to(receipt_path.parent.resolve()), 'baseline case escaped its artifact root')
    verify_raw_artifacts(case.get('initialRawArtifacts'), directory,
                         ['case.stdout', 'case.stderr', 'host.log'], True)
    require(json_lines(directory/'case.stdout') == case['reports']
            and json_lines(directory/'host.log') == case['hostReports'], 'baseline raw/native receipt differs')
    require(argv.count('--dll') == 1, 'baseline exact DLL argument')
    dll = Path(argv[argv.index('--dll') + 1]); host = directory/'warvk-d3d9-host-dev.exe'
    expected_fixture = ('d3d9-resource-lifetime32.exe' if kind == 'lifetime'
        else 'd3d9-presenter-fault32.exe' if label == 'clear-soak' else 'd3d9-api-parent32.exe')
    require(fixture.name == expected_fixture and dll == directory/'proxy.dll', 'unexpected native fixture/proxy')
    require(argv == list(map(str, case_command(fixture, dll, label, desktop, input_desktop))),
            'baseline case command differs from the replay contract')
    require(Path(case['process']['stdout']) == directory/'case.stdout'
            and Path(case['process']['stderr']) == directory/'case.stderr', 'baseline redirected native output identity')
    for source in (fixture, dll, host):
        frozen = [row['copy'] for row in receipt['caseCopies'] if Path(row['copy']['path']) == source]
        require(len(frozen) == 1, 'native input was not a uniquely frozen executed copy')
        require_identity(frozen[0])
    require(pe_identity(fixture) == pe_identity(dll) == (0x14c, 0x10b)
            and pe_identity(host) == (0x8664, 0x20b), 'native binary widths')
    return fixture, dll, host


def baseline_probes(receipt, receipt_path):
    """Both current native gates freeze their actual x86/x64 CPU probes."""
    desktop, input_desktop = receipt['desktopName'], receipt['desktopSafety']['inputBefore']
    launch = receipt['desktopProbe']
    check_launch(launch, desktop, input_desktop)
    probes = {}
    for bits, width in ((32, (0x14c, 0x10b)), (64, (0x8664, 0x20b))):
        rows = [row for row in receipt['binaries'] if Path(row['path']).name == f'desktop-probe-{bits}.exe']
        require(len(rows) == 1, 'frozen CPU desktop probe missing/duplicated')
        path = require_identity(rows[0])
        require(path.parent.resolve() == receipt_path.parent.resolve(), 'probe escaped baseline artifact root')
        require(pe_identity(path) == width, 'frozen probe architecture')
        probes[bits] = path
    argv = launch['argv']
    root = receipt_path.parent
    verify_raw_artifacts(receipt.get('probeRawArtifacts'), root,
                         ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False)
    require(argv == list(map(str, [probes[32], '--expect-desktop', desktop,
            '--expect-station', 'WinSta0', '--input-desktop', input_desktop,
            '--child', probes[64], '--child-log', root/'desktop-child.log'])), 'probe executed command identity')
    require(Path(launch['stdout']) == root/'probe.stdout' and Path(launch['stderr']) == root/'probe.stderr',
            'probe redirected native output identity')
    rows = json_lines(root/'probe.stdout') + json_lines(root/'desktop-child.log')
    require(rows == receipt['probeReports'] and len(rows) == 2, 'probe raw/native receipt differs')
    validate_probe_receipt(rows[0], desktop=desktop, input_desktop=input_desktop,
                           station='WinSta0', bits=32, child=rows[1], launch=launch)
    return probes


def setting_keys(node):
    found = set()
    if type(node) is dict:
        if type(node.get('key')) is str: found.add(node['key'])
        for key in ('settings', 'flags'):
            for child in node.get(key, []): found.update(setting_keys(child))
    return found


def inspect_layer(path):
    manifest = strict_json(path.read_text('utf-8')); layer = manifest['layer']
    require(layer['name'] == LAYER and layer['api_version'] == '1.4.335'
            and layer['type'] == 'GLOBAL', 'this gate is frozen to installed SDK VVL 1.4.335')
    library = (path.parent/layer['library_path']).resolve(strict=True)
    require(library.parent == path.parent.resolve() and library.name == 'VkLayer_khronos_validation.dll',
            'layer library outside explicit manifest directory')
    require(pe_identity(library) == (0x8664, 0x20b), 'x64 VVL required')
    keys = set()
    for node in layer['features']['settings']: keys.update(setting_keys(node))
    require(set(VVL_SETTINGS) | {'log_filename'} <= keys, 'installed manifest does not support requested controls')
    return library


def validation_env(case_dir, manifest, empty_implicit_source=None):
    controls = {'VK_KHRONOS_VALIDATION_' + key.upper(): value for key, value in VVL_SETTINGS.items()}
    controls.update(VK_KHRONOS_VALIDATION_LOG_FILENAME=str(case_dir/'validation.raw.log'),
        VK_LAYER_PATH=str(manifest.parent), VK_LOADER_LAYERS_DISABLE='~implicit~',
        VK_LOADER_DEBUG='error,warn,info,layer', DXVK_DEBUG='validation', DXVK_LOG_LEVEL='info',
        DXVK_SHADER_CACHE='0', DXVK_LOG_PATH=str(case_dir),
        DXVK_CONFIG_FILE=str(case_dir/'absent.conf'))
    if empty_implicit_source is not None:
        controls['VK_IMPLICIT_LAYER_PATH'] = str(empty_implicit_source)
    return controls


def parse_validation(text):
    require(type(text) is str and bool(text) and text.endswith('\n'), 'empty/truncated raw VVL file')
    matches = list(VVL_HEADER.finditer(text))
    require(matches and not text[:matches[0].start()].strip(), 'raw VVL prefix/header missing')
    records = []
    for index, match in enumerate(matches):
        body = text[match.end():matches[index+1].start() if index+1 < len(matches) else len(text)].strip()
        require(body and not re.search(r'(?m)^\s*(?:Validation|Verbose)(?:[ \t]+[^\r\n:]+:|'
                r'[ \t]+(?:Error|Warning|Performance Warning|Information)\b|[ \t]*$)', body),
                'malformed/unrecognized VVL record boundary')
        records.append(dict(severity=match[1], vuid=match[2], messageId='0x'+match[3].lower(), body=body))
    status = [row for row in records if row['vuid'] == STATUS_ID]
    require(len(status) == 1 and status[0]['severity'] in ('Information', 'Warning'), 'one exact VVL activation record required')
    body = status[0]['body']
    require(body.count('Khronos Validation Layer Active:') == 1, 'VVL active banner missing')
    def states(name):
        values = re.findall(r'^\s*Current ' + name + r':\s*([^\r\n]+)\.\s*$', body, re.M)
        require(len(values) == 1, 'missing/duplicate activation ' + name)
        tokens = [value.strip() for value in values[0].split(',')]
        require(all(tokens) and len(tokens) == len(set(tokens)), 'empty/duplicate activation ' + name)
        return set(tokens)
    require(states('Enables') == {SYNC_ENABLE}, 'requested synchronization validation not independently active')
    require(states('Disables') == {CACHE_DISABLE}, 'validation checks unexpectedly disabled or cache not disabled')
    failures = [row for row in records if row['vuid'] != STATUS_ID
                and row['severity'] in ('Error', 'Warning', 'Performance Warning')]
    require(not failures, 'VVL warning/error: ' + ', '.join(row['vuid'] for row in failures))
    require(not any(re.search(r'not recognized|not a valid option|could not be opened|falling back to stdout|'
                              r'deprecated settings|LAYER SETTING.*error', row['body'], re.I) for row in records),
            'VVL settings/logging failure')
    return dict(records=records, activationProven=True, synchronizationValidationProven=True,
        rawCallbackIndependentOfDxvkIgnoredIds=True, runtimeFilterLimitProven=False,
        statusRecordClassifiedSeparately=True, nonStatusWarnings=0, errors=0)


def normalized_path(text):
    return os.path.normcase(os.path.normpath(text.strip().strip('"')))


def check_loader(host_text, dxvk_text, library, empty_implicit_source=None, loader_path=None):
    wanted = normalized_path(str(library))
    parsed = loader_records(host_text)
    layer_messages = [message for channels, message in parsed if 'LAYER' in channels]
    loads = [match[1] for message in layer_messages
             if (match := re.fullmatch(r'Loading layer library\s+([^\r\n]+)', message))]
    inserted = [(match[1], match[2]) for message in layer_messages
                if (match := re.fullmatch(r'Insert instance layer "([^"]+)" \(([^\r\n]+)\)', message))]
    devices = [(match[1], match[2]) for message in layer_messages
               if (match := re.fullmatch(r'Inserted device layer "([^"]+)" \(([^\r\n]+)\)', message))]
    require(loads and all(normalized_path(path) == wanted for path in loads),
            'loader did not load only exact x64 VVL library')
    require(inserted and all(name == LAYER and normalized_path(path) == wanted for name, path in inserted),
            'actual instance layer chain missing/contains another layer')
    require(devices and all(name == LAYER and normalized_path(path) == wanted for name, path in devices),
            'actual device layer chain missing/contains another layer')
    disabled_pattern = re.compile(r'Implicit layer "[^"]+" forced disabled because name matches filter '
                                  r"of env var 'VK_LOADER_LAYERS_DISABLE'\.")
    disabled = []; loader_lines = []
    for line in host_text.splitlines():
        # The Windows loader callback reaches this host with this prefix. Do
        # not let a real ERROR/WARNING bypass validation merely by its routing.
        match = re.match(r'^\s*(?:\[Vulkan Loader\]\s*)?([A-Z]+(?:\s*\|\s*[A-Z]+)*)\s*:\s*(.*)$', line)
        if not match: continue
        channels = {value.strip() for value in match[1].split('|')}
        if not channels & {'ERROR', 'WARNING', 'WARN', 'LAYER', 'INFO', 'DEBUG'}: continue
        loader_lines.append(line)
        is_disabled = disabled_pattern.fullmatch(match[2].strip()) is not None
        if is_disabled: disabled.append(line)
        require('ERROR' not in channels, 'Vulkan loader error: ' + line)
        if channels & {'WARNING', 'WARN'}:
            require(is_disabled, 'unclassified loader warning: ' + line)
    if empty_implicit_source is None:
        require(disabled, 'no actual loader proof of implicit-layer filtering')
        source_evidence = dict(mode='discovered-layer-filter', actualDiscoveryProven=False)
    else:
        source_evidence = check_empty_source_trace(host_text, empty_implicit_source, (LAYER,))
        require(loader_path is not None, 'private discovery requires actual frozen loader binding')
        source_evidence['loaderModule'] = check_loader_module_trace(host_text, loader_path)
    for text in (host_text, dxvk_text):
        require(not re.search(r'Validation layers not found|Failed to create debug messenger|'
            r'LAYER SETTING.*error|not a valid option for VK_LAYER|could not be opened, falling back|'
            r'not recognized by the Validation Layers|deprecated settings', text, re.I),
            'layer/settings/debug messenger failure')
    require('Enabled instance layers:' in dxvk_text and LAYER in dxvk_text,
            'DXVK did not request validation layer')
    return dict(exactLibrary=str(library), instanceLayers=inserted, deviceLayers=devices,
                disabledImplicitLayerLines=disabled, loaderTraceLines=loader_lines,
                exactX64LayerLoaded=True, implicitLayersDisabledByProcessEnvironment=True,
                implicitDiscoverySource=source_evidence)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('presenter-receipt', 'lifetime-receipt', 'output'):
        ap.add_argument('--'+name, type=Path, required=True)
    ap.add_argument('--layer-manifest', type=Path,
                    default=Path('E:/Dev/Vulkan SDK/Bin/VkLayer_khronos_validation.json'))
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='C2_FROZEN_BINARY_X64_VVL_SYNC_VALIDATION', ok=False,
        rebuilt=False, gameTouched=False, playerDllChanged=None, productIntegrated=False,
        memorySavingsProven=False, performanceMeasured=False, windowPixelsVerified=False,
        playerVisualAccepted=False, globalEnvironmentModified=False, registryModified=False,
        runtimeFilterLimitProven=False, restrictedMediumRequested=True,
        cases=[], caseCopies=[], preservedInputs=[], rawArtifactsPreserved=False)
    frozen = {}
    baseline_raw = []
    def preserve(record):
        require_identity(record)
        key = str(Path(record['path']).resolve())
        if key in frozen: require(frozen[key] == record, 'conflicting frozen identity: '+key)
        frozen[key] = record
    def copy_file(source, target):
        record = identity(source); preserve(record); require(not target.exists(), 'copy target exists')
        shutil.copy2(source, target); copied = identity(target)
        require(all(record[key] == copied[key] for key in ('bytes', 'sha256')), 'executed copy differs')
        receipt['caseCopies'].append(dict(source=record, copy=copied))
        return target
    try:
        require(os.name == 'nt', 'Windows isolated native runner required')
        receipt_paths = {'presenter': args.presenter_receipt.resolve(strict=True),
                         'lifetime': args.lifetime_receipt.resolve(strict=True)}
        baselines = {kind: load_baseline(path, kind) for kind, path in receipt_paths.items()}
        for kind, baseline in baselines.items():
            preserve(identity(receipt_paths[kind]))
            for record in baseline['sources'] + baseline['binaries']:
                preserve(record)
            for record in baseline['caseCopies']: preserve(record['copy'])
            for key in ('referenceBefore', 'playerBefore', 'compileCommands'): preserve(baseline[key])
            for manifest in raw_manifests(baseline):
                baseline_raw.append(manifest)
                for record in verify_raw_artifacts(manifest): preserve(record)
        receipt['baselineReceipts'] = {kind: identity(path) for kind, path in receipt_paths.items()}
        inputs = {label: baseline_case(baselines[kind], receipt_paths[kind], kind, label)
                  for label, kind, _ in CASE_SPECS}
        host_hashes = {identity(paths[2])['sha256'] for paths in inputs.values()}
        require(len(host_hashes) == 1, 'baseline cases do not share one frozen C2 host')
        for script in (Path(__file__).resolve(), Path(__file__).with_name('test_render_host_validation_gate.py').resolve(),
                       Path(__file__).with_name('render_lab_layer_source.py').resolve(),
                       Path(__file__).with_name('test_render_lab_layer_source.py').resolve(),
                       Path(__file__).with_name('render_lab_raw_artifacts.py').resolve(),
                       Path(__file__).with_name('test_render_lab_raw_artifacts.py').resolve()):
            if script.exists(): preserve(identity(script))
        manifest = args.layer_manifest.resolve(strict=True); library = inspect_layer(manifest)
        loader = Path(os.environ['SYSTEMROOT'])/'System32/vulkan-1.dll'
        require(pe_identity(loader) == (0x8664, 0x20b), 'x64 system loader required')
        for path in (manifest, library, loader): preserve(identity(path))
        receipt['layer'] = dict(manifest=identity(manifest), library=identity(library), loader=identity(loader),
            apiVersion='1.4.335', requestedSettings=VVL_SETTINGS,
            requestedSettingsSha256=digest_json(VVL_SETTINGS), requestedNoFiltersAndNoLimit=True,
            activationIsNotInstallation=True, runtimeFilterLimitProven=False,
            shaderAccessHeuristic=False, gpuAssistedValidation=False)
        player = Path(baselines['presenter']['playerBefore']['path'])
        receipt['playerBefore'] = identity(player)
        environment = {key: value for key, value in os.environ.items() if key.upper() in ENV_ALLOW}
        receipt['inheritedEnvironmentNames'] = sorted(environment)
        receipt['implicitSourceBefore'] = create_empty_source(out)
        empty_implicit_source = Path(receipt['implicitSourceBefore']['path'])
        probe_sources = baseline_probes(baselines['lifetime'], receipt_paths['lifetime'])
        probes = {bits: copy_file(path, out/path.name) for bits, path in probe_sources.items()}
        # A high-integrity loader ignores secure environment controls. Run only
        # the derived, restricted child token; do not change global UAC/layers.
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopName'] = desktop.desktop_name; receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', probes[64], '--child-log', out/'desktop-child.log'], cwd=out,
                stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=environment)
            receipt['desktopProbe'] = probe
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'desktop-child.log'])
            check_launch(probe, desktop.desktop_name, desktop.input_before)
            rows = json_lines(out/'probe.stdout') + json_lines(out/'desktop-child.log')
            require(len(rows) == 2, 'actual CPU desktop probe population')
            receipt['probeReports'] = rows
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station='WinSta0', bits=32, child=rows[1], launch=probe)
            for label, kind, timeout in CASE_SPECS:
                directory = out/label; directory.mkdir()
                case = dict(label=label, ok=False, deadlineSeconds=timeout, runtimeFilterLimitProven=False)
                receipt['cases'].append(case)
                fixture, proxy, host = inputs[label]
                local_fixture = copy_file(fixture, directory/fixture.name)
                local_proxy = copy_file(proxy, directory/'proxy.dll')
                local_host = copy_file(host, directory/'warvk-d3d9-host-dev.exe')
                controls = validation_env(directory, manifest, empty_implicit_source) | dict(WARVK_D3D9_HOST_EXE=str(local_host),
                    WARVK_D3D9_HOST_LOG=str(directory/'host.log'))
                if label == 'clear-soak':
                    controls['WARVK_D3D9_PROFILE'] = 'presenter-readback-isolated-v1'
                elif kind == 'presenter':
                    diagnostic = label == 'presenter-readback-eight-frames'
                    controls['WARVK_D3D9_PROFILE'] = ('presenter-readback-isolated-v1' if diagnostic
                                                   else 'presenter-isolated-v1')
                cmd = case_command(local_fixture, local_proxy, label, desktop.desktop_name, desktop.input_before)
                case['processControls'] = controls; case['processControlsSha256'] = digest_json(controls)
                local = environment | controls
                raw_path = directory/'validation.raw.log'
                require(not raw_path.exists(), 'raw validation log must be a new per-case file')
                launch = desktop.run(cmd, cwd=directory, stdout_path=directory/'case.stdout',
                    stderr_path=directory/'case.stderr', timeout_s=timeout, env=local)
                case['process'] = launch
                case['initialRawArtifacts'] = capture_raw_artifacts(directory,
                    ['case.stdout', 'case.stderr', 'host.log', 'validation.raw.log',
                     local_host.stem+'_warvk-d3d9-host.log'], True)
                # Preserve all emitted files even on a failed watchdog/exit.
                case['artifacts'] = [identity(path) for path in sorted(directory.iterdir()) if path.is_file()]
                reports = json_lines(directory/'case.stdout'); hosts = json_lines(directory/'host.log')
                case['reports'] = reports; case['hostReports'] = hosts
                require(launch['ok'] is True, 'native validation case failed: '+label)
                peer, reader_mutations = verify_native(reports, hosts, launch, label,
                    desktop.desktop_name, desktop.input_before, True)
                case['readerMutations'] = reader_mutations
                case['childSettlement'] = child_settled(peer['childPid'], peer['childCreation'])
                require(raw_path.is_file() and raw_path.stat().st_size <= 32*1024*1024,
                        'independent VVL raw file missing or exceeded parse budget; original retained')
                raw_text = raw_path.read_text('utf-8')
                case['rawValidationFile'] = identity(raw_path)
                # Retain the diagnostic text before checking it. No bad record
                # is removed, even when it happens to match a DXVK ignored ID.
                case['rawValidationText'] = raw_text
                require(local_host.name == 'warvk-d3d9-host-dev.exe', 'expected frozen logger executable name')
                # Logger::getFileName prefixes its fixed base with the actual
                # executable stem. Never pick an arbitrary glob/older log.
                dxvk_path = directory/(local_host.stem+'_warvk-d3d9-host.log')
                require(dxvk_path.is_file(), 'separate DXVK diagnostic log missing')
                case['loader'] = check_loader((directory/'host.log').read_text('utf-8'),
                                             dxvk_path.read_text('utf-8'), library, empty_implicit_source, loader)
                case['validation'] = parse_validation(raw_text)
                case['ok'] = True
        require(receipt['desktopSafety']['ok'] is True, 'isolated desktop cleanup failed')
        require(len(receipt['cases']) == 5 and all(case['ok'] for case in receipt['cases']), 'five native cases required')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['preservedInputs'] = list(frozen.values())
        try:
            if 'implicitSourceBefore' in receipt:
                receipt['implicitSourceAfter'] = empty_directory_witness(Path(receipt['implicitSourceBefore']['path']))
                require(receipt['implicitSourceBefore'] == receipt['implicitSourceAfter'], 'private implicit source changed')
            for record in frozen.values(): require_identity(record)
            for record in receipt['caseCopies']: require_identity(record['copy'])
            if 'playerBefore' in receipt:
                receipt['playerAfter'] = identity(Path(receipt['playerBefore']['path']))
                receipt['playerDllChanged'] = receipt['playerBefore'] != receipt['playerAfter']
                require(not receipt['playerDllChanged'], 'player DLL changed')
            captured = list(raw_manifests(receipt))
            for manifest in baseline_raw + captured: verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(captured)
            receipt['sourceAndReferencePreserved'] = bool(frozen) and receipt['playerDllChanged'] is False
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'error', 'scope',
        'sourceAndReferencePreserved', 'runtimeFilterLimitProven')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
