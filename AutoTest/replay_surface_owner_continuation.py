"""Frozen-binary, fixed twelve-case surface-owner continuation; diagnostic only.

Default prepares a receipt and verifies old evidence; it never builds or launches.
Only the root native-lease owner may use --execute-native. No original failure is
retried/replaced. B renders to the host-owned target, NOT the x86 peer window.
No PRESETS/global timeout mutation, source generation, compilation or deployment.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import shutil
import sys

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/diagnose_render_host_surface_owner.py').is_file()
            and (p/'tools/render_host/dxvk_core/d3d9_backend.cpp').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))

import diagnose_render_host_surface_owner as surface
import diagnose_render_host_initialization as init
from diagnose_render_host_presenter_exit import ExitMonitor, prove_host
from run_render_host_protocol_gate import identity, pe_identity
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_d3d9_gate import json_lines, native_uint
from run_render_host_presenter_gate import check_case_records, check_launch, check_soak, check_fault, check_wsi
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests
from render_lab_palette_watcher import FrozenImage, PE_PATH, PDB_PATH

BASELINE = ROOT/'AutoTest/artifacts/x64-surface-owner-20261022-r1/receipt.json'
BASELINE_SHA = 'B1C633744FF59EB7382AD9603DD32F27033D8A768511CB09EEAECBFC8ECAD237'
HOST_SHA = '339F8C6B718B0880AC43D2F1675F792ADBB58DCC8560224E9821AFA266C8C721'
HOST_BYTES = 5537792
BINARY_PINS = {
    'parent': ('d3d9-api-parent32.exe', 815104, '3C813A3F2C85E65D500679E7A7D19D995CFB87D4228AB5218B5EE8D51E9C2FFA'),
    'proxy': ('warvk-d3d9-proxy-dev.dll', 975872, '10A5229C6725AA81B29E3ED80B5536825A79DBF1D0A55DF92C3D57FA75D35C85'),
    'fault': ('d3d9-presenter-fault32.exe', 1470976, '6AC813982FA7DCAA5ACB407819B6DFF4EA23798C00A01082DD8C82A31AE18B08'),
    'probe32': ('desktop-probe-32.exe', 1503232, 'E6091DFC6B3BDCB3D299558DD6040DF7821B4AA461CCDB0A718145D6BC70DF3C'),
    'probe64': ('desktop-probe-64.exe', 1504768, '93C31701CFB78D4579293613EDFACE8A5727573F03E243A4CF3D9B87F68EA5D9'),
}
SETTLED = frozenset(('process-object-absent', 'pid-reused-original-object-gone', 'exact-process-handle-signaled'))
READER_ARCHIVE = ROOT/'AutoTest/artifacts/surface-owner-reader-revision-20261022-r1'
# Only these reviewed Python reader revisions supersede historical input paths.
# Their byte-exact preimages remain hashed, not silently refreshed to current.
# No executable, C++ source, build flag, raw log or prior receipt is exempted.
READER_REVISIONS = {
    'diagnose_render_host_initialization.py': (
        '381323FC1A382D1E1868CD8CE61443BF4252A5301620A695B0A19D8121BA36A5',
        '0756F074173480650D66E5B5EA837145ADCB3A901D0FA0ED2545C4478062800F'),
    'test_render_host_initialization.py': (
        '2A4D57A17DE66D1D01A00DD08087D33AC26E8EC2F06FEFAD22AD19208DB8D712',
        'E0AF3E93A5E06DD0C01FA68C199B8F3F90F34DFF5749C42CE43B3456E9217A5F'),
}


def reader_revisions():
    rows = []
    for name, (old_sha, new_sha) in READER_REVISIONS.items():
        previous = identity(READER_ARCHIVE/(Path(name).stem+'.before.py'))
        current = identity(ROOT/'AutoTest'/name)
        require(previous['sha256'] == old_sha and current['sha256'] == new_sha,
                'reviewed reader preimage/current identity changed: '+name)
        rows.append(dict(originalPath=str(ROOT/'AutoTest'/name), archivedPreimage=previous,
                         current=current, reason='explicit-readback-profile-only-no-native-change'))
    return rows


def revised_inputs(inputs):
    revisions = {Path(r['originalPath']).resolve(): r for r in reader_revisions()}
    rows = []
    for row in inputs:
        revision = revisions.get(Path(row['path']).resolve())
        if revision is None:
            rows.append(row)
            continue
        previous = revision['archivedPreimage']
        require(all(row[k] == previous[k] for k in ('bytes', 'sha256')),
                'historical reader does not match archived preimage')
        rows.extend((previous, revision['current']))
    return rows


@dataclass(frozen=True)
class Case:
    index: int
    kind: str
    mode: str
    scenario: str
    seconds: int

    @property
    def label(self):
        return f'{self.index:02d}-{self.kind}-{self.scenario}-{self.mode}'

    @property
    def monitor_preset(self):
        # Existing monitor presets select 45/90s observation windows only.
        # They do not select the executable; our copied frozen paths do that.
        return 'r9-readback' if self.kind == 'shader' else 'r8-clear'


CASES = tuple(
    [Case(i+1, 'soak', mode, 'clear-soak', 45)
     for i, mode in enumerate(('peer', 'host', 'host', 'peer'))]
    + [Case(5+i*2+j, 'fault', mode, scenario, 45)
       for i, scenario in enumerate(('marker', 'extent', 'destroy'))
       for j, mode in enumerate(('peer', 'host'))]
    + [Case(11+i, 'shader', mode, 'normal', 90) for i, mode in enumerate(('peer', 'host'))])


def checked_case(case):
    require(type(case) is Case and type(case.index) is int and 1 <= case.index <= 12
            and case == CASES[case.index-1], 'only the fixed twelve-case plan is allowed')
    return case


def arguments(case, parent, proxy, desktop, input_desktop):
    checked_case(case)
    argv = [str(parent), '--dll', str(proxy), '--scenario', case.scenario]
    if case.kind == 'shader':
        return argv+['--wsi-readback']
    if case.kind == 'soak':
        argv += ['--diagnostic-readback', '1']
    return argv+['--expect-desktop', desktop, '--input-desktop', input_desktop]


def environment(case, current, directory, host, controls=None):
    checked_case(case)
    env = init.recipe_environment(current, directory, host)
    if case.kind == 'fault':
        env['WARVK_D3D9_PROFILE'] = 'presenter-isolated-v1'
    env['WARVK_DIAGNOSTIC_SURFACE_OWNER'] = case.mode
    if controls is not None:
        require(type(controls) is dict and all(type(k) is str and type(v) is str
                and k and k == k.upper() and '\x00' not in k+v and '=' not in k
                for k, v in controls.items()), 'explicit string-map diagnostic controls only')
        for key, value in controls.items():
            require((key in env and env[key] == value) or
                    (key not in env and (key.startswith('VK_') or key == 'DXVK_DEBUG')),
                    'diagnostic controls may not change frozen host/profile/recipe: '+key)
        env.update(controls)
    return env


def checked_extra_raw_names(values):
    if values is None:
        return []
    require(type(values) is list and all(type(n) is str and n and Path(n).name == n
            and n not in ('.', '..') and '/' not in n and '\\' not in n and ':' not in n
            and '\x00' not in n for n in values), 'extra raw artifacts must be plain file names')
    require(len(set(values)) == len(values) and not set(values).intersection(init.RAW_NAMES)
            and not any(n.startswith('exit-snapshot-') for n in values),
            'duplicate extra raw artifact')
    return list(values)


def check_settlement(case, launch, desktop, input_desktop):
    """Actual launcher receipt stays intact; no 90->45 or failed->ok rewriting.

    init's existing reader handles its original 45s cases. The 90s shader case
    repeats the same settlement policy at its original frozen 90s outer limit.
    This is cleanup proof only; expected workload failures are still failures.
    """
    checked_case(case)
    if case.seconds == 45:
        init.check_diagnostic_settlement(launch, desktop, input_desktop)
        return
    require(type(launch['exitCode']) is int and launch['exitCode'] in (0, 2), 'unexpected shader fixture exit')
    require(type(launch['jobActiveProcesses']) is int and launch['jobActiveProcesses'] == 0,
            'shader diagnostic Job not empty')
    for key in ('processSettled', 'jobEmpty', 'jobAtomic', 'handleListApplied'):
        require(launch[key] is True, 'missing shader settlement '+key)
    for key in ('timedOut', 'forcedTermination', 'desktopHandleInherited'):
        require(launch[key] is False, 'failed shader settlement '+key)
    require(launch['cleanupErrors'] == [] and launch['desktop'] == desktop
            and launch['inputBefore'] == launch['inputAfter'] == input_desktop
            and desktop != input_desktop, 'shader cleanup/Desktop')
    require(all(native_uint(launch[key], bits) and launch[key] > 0 for key, bits in
        (('pid', 32), ('initialThreadId', 32), ('creation100ns', 64))), 'exact shader parent identity')
    require(type(launch['argv']) is list and launch['argv'] and type(launch['argv'][0]) is str
            and Path(launch['actualExecutable']).resolve() == Path(launch['argv'][0]).resolve()
            and type(launch['timeoutSeconds']) in (int, float) and launch['timeoutSeconds'] == 90,
            'shader fixture image/original deadline changed')
    token = launch['restrictedToken']
    require(token['mode'] == 'restricted-medium-per-child/v1' and token['childTokenVerifiedBeforeResume'] is True
            and token['derivedTokenClosed'] is True and token['cleanupErrors'] == [], 'shader Medium token witness')
    require(token['resumedAfterVerification'] is True and type(token['resumePreviousCount']) is int
            and token['resumePreviousCount'] == 1 and token['parentTokenUnchanged'] is True
            and token['noHighIntegrityFallback'] is True and token['globalSecurityChanged'] is False,
            'shader restricted child verification/resume')
    observed = token['childBeforeResume']
    require(type(observed['integrityRid']) is int and observed['integrityRid'] == 8192
            and observed['integritySid'] == 'S-1-16-8192' and observed['hasRestrictions'] is True,
            'shader suspended child not restricted Medium')


def record_population(case, reports, hosts):
    checked_case(case)
    fixture_schema = {'soak': 'warvk-d3d9-presenter-clear-soak/v1',
        'fault': 'warvk-d3d9-presenter-fault-fixture/v1', 'shader': 'warvk-d3d9-wsi-fixture/v1'}[case.kind]
    host_schema = 'warvk-d3d9-host-fault/v1' if case.kind == 'fault' else 'warvk-d3d9-host/v1'
    a, b = check_case_records(reports, hosts, fixture_schema, host_schema, True)
    return a[fixture_schema], a['warvk-d3d9-proxy/v1'], b[host_schema], b['warvk-d3d9-desktop/v1']


def check_workload(case, records, launch, desktop, input_desktop):
    checked_case(case)
    f, proxy, host, witness = records
    require(native_uint(proxy['primaryExitWaitMs']) and proxy['primaryExitWaitMs'] <= 5000,
            'original primary exit gate exceeded; late cleanup cannot pass')
    if case.kind == 'soak':
        check_soak(f, proxy, host, witness, launch, desktop, input_desktop)
    elif case.kind == 'fault':
        check_fault(f, proxy, host, witness, launch, case.scenario, desktop, input_desktop,
                    readback=False, live_resource=False)
    else:
        check_wsi(f, proxy, host, witness, desktop, True, launch, input_desktop)


def bind_monitor(result, launch, parent, host, desktop, input_desktop):
    require(type(result) is dict and result['errors'] == [], 'read-only monitor error')
    binding = result['binding']
    prove_host(launch, binding['nativeDesktopWitness'], binding['parent'], binding['host'],
               parent, host, desktop, input_desktop, binding['actualParentPid'])
    return binding


def validate_baseline(old):
    require(old['scope'] == 'COPY_TU_SURFACE_OWNERSHIP_ABBA_DIAGNOSTIC'
            and old['acceptancePassed'] is False and old['diagnosticComplete'] is True
            and old['preparationComplete'] is True and old['frozenInputsPreserved'] is True
            and old['rawArtifactsPreserved'] is True and old['sameExeRequired'] is True
            and old['arms'] == ['peer', 'host', 'host', 'peer'], 'exact frozen r1 diagnostic required')
    host = old['diagnosticHost']
    require((host['bytes'], host['sha256']) == (HOST_BYTES, HOST_SHA)
            and Path(host['path']) == BASELINE.parent/'warvk-d3d9-host-dev.exe', 'not the frozen r1 executable')
    require(len(old['cases']) == 4, 'r1 four-arm population')
    for case, row in zip(CASES[:4], old['cases']):
        require(row['index'] == case.index and type(row['index']) is int and row['mode'] == case.mode
                and row['acceptancePassed'] is False and row['workloadPassed'] is True
                and row['diagnosticCollected'] is True and row['childSettlement'] in SETTLED,
                'r1 outcome cannot be replaced or inferred')
        launch = row['process']; directory = Path(launch['argv'][0]).parent
        require(directory == BASELINE.parent/f'{case.index}-{case.mode}', 'r1 case path')
        require(launch['argv'] == arguments(case, directory/'d3d9-presenter-fault32.exe', directory/'proxy.dll',
                launch['desktop'], launch['inputBefore']), 'r1 original case recipe')
        check_settlement(case, launch, launch['desktop'], launch['inputBefore'])
        binding = bind_monitor(row['monitor'], launch, directory/'d3d9-presenter-fault32.exe',
            directory/'warvk-d3d9-host-dev.exe', launch['desktop'], launch['inputBefore'])
        reports, hosts = json_lines(directory/'case.stdout'), json_lines(directory/'host.log')
        require(reports == row['reports'] and hosts == row['hostReports'], 'r1 embedded/raw records disagree')
        records = record_population(case, reports, hosts)
        check_workload(case, records, launch, launch['desktop'], launch['inputBefore'])
        surface.check_monitor_proxy(binding, records[1], records[3])
        text = (directory/'host.log').read_text('utf-8')
        require(surface.read_target(text, case.mode, launch, records[3], records[1]) == row['target'],
                'r1 target receipt differs from strict reader')
        require(init.read_bound_stages(text, hosts, reports, launch, launch['desktop'], launch['inputBefore'])
                == row['stages'], 'r1 initialization receipt differs from strict reader')


def unique_inputs(rows):
    by_path = {}
    for row in rows:
        key = str(Path(row['path']).resolve()).casefold()
        require(key not in by_path or by_path[key] == row, 'conflicting frozen file identities')
        by_path[key] = row
    return list(by_path.values())


def bind_inputs():
    require(identity(BASELINE)['sha256'] == BASELINE_SHA, 'frozen r1 receipt changed')
    old = strict_json(BASELINE.read_text('utf-8'))
    r10, _, _, r10_frozen = init.bind_baseline()
    frozen = r10_frozen + old['frozenInputs'] + [identity(BASELINE)]
    frozen += [old[key] for key in ('diagnosticHost', 'diagnosticObject', 'diagnosticSource', 'originalSource')]
    for pair in old['copies']:
        require(all(pair['source'][k] == pair['copy'][k] for k in ('bytes', 'sha256')), 'r1 unequal binary copy')
        frozen.extend((pair['source'], pair['copy']))
    for manifest in raw_manifests(old):
        frozen.extend(verify_raw_artifacts(manifest))
    binaries = {}
    for role, (name, size, digest) in BINARY_PINS.items():
        matches = [r for r in r10['binaries'] if Path(r['path']).name == name]
        require(len(matches) == 1 and (matches[0]['bytes'], matches[0]['sha256']) == (size, digest),
                'exact r10 binary required: '+role)
        binaries[role] = Path(matches[0]['path'])
    binaries['host'] = Path(old['diagnosticHost']['path'])
    frozen = unique_inputs(revised_inputs(frozen))
    for row in frozen:
        init.copied.same_file(row)
    for role, path in binaries.items():
        require(pe_identity(path) == ((0x8664, 0x20b) if role in ('host', 'probe64') else (0x14c, 0x10b)),
                'frozen binary bitness: '+role)
    validate_baseline(old)
    require('childExit=child.wait(5000);' in (ROOT/'tools/render_host/d3d9_connection.h').read_text('utf-8'),
            'original child exit deadline changed')
    return old, r10, binaries, frozen


def copy_binary(source, target, receipt, frozen):
    before = identity(source)
    require(not target.exists() and target.parent.resolve() == target.parent, 'fresh nonredirected case copy')
    shutil.copy2(source, target)
    after = identity(target)
    require(all(before[k] == after[k] for k in ('bytes', 'sha256')), 'case copy differs from frozen source')
    receipt['copies'].append(dict(source=before, copy=after)); frozen.extend((before, after))
    return target


def run_one_case(case, binaries, out, desktop, palette, receipt, frozen, *, controls=None, extra_raw_names=None):
    checked_case(case)
    extras = checked_extra_raw_names(extra_raw_names)
    directory = out/case.label; directory.mkdir()
    row = dict(**asdict(case), label=case.label, acceptancePassed=False, workloadPassed=False,
        diagnosticCollected=False, settlementCertified=False, errors=[])
    receipt['cases'].append(row)
    source = binaries['parent' if case.kind == 'shader' else 'fault']
    parent = copy_binary(source, directory/source.name, receipt, frozen)
    proxy = copy_binary(binaries['proxy'], directory/'proxy.dll', receipt, frozen)
    host = copy_binary(binaries['host'], directory/'warvk-d3d9-host-dev.exe', receipt, frozen)
    env = environment(case, dict(os.environ), directory, host, controls)
    require(not (directory/'absent.conf').exists(), 'configuration must remain absent')
    row['actualSafeEnvironment'] = env
    row['requestedDiagnosticControls'] = None if controls is None else dict(controls)
    row['extraRawNames'] = extras
    require(all(not (directory/name).exists() for name in extras), 'extra raw file must be fresh')
    argv = arguments(case, parent, proxy, desktop.desktop_name, desktop.input_before)
    monitor = ExitMonitor(desktop, parent, host, directory, case_name=case.monitor_preset, palette_image=palette)
    require(monitor.launcher_timeout == case.seconds and monitor.dbghelp_identity is None,
            'original monitor timeout/no-dump policy')
    monitor.start()
    try:
        row['process'] = desktop.run(argv, cwd=directory, stdout_path=directory/'case.stdout',
            stderr_path=directory/'case.stderr', timeout_s=case.seconds, env=env)
    finally:
        monitor.stop_event.set(); monitor.join(2)
        row['monitor'] = monitor.result
        row['initialRawArtifacts'] = capture_raw_artifacts(directory,
            init.RAW_NAMES+extras+[p.name for p in directory.glob('exit-snapshot-*.json')], True)
    # These failures escape immediately. A later case must never start while
    # the original owned Job, exact host, or observation thread is uncertain.
    try:
        require(not monitor.is_alive(), 'monitor still active; stop the matrix')
        check_settlement(case, row['process'], desktop.desktop_name, desktop.input_before)
        binding = bind_monitor(monitor.result, row['process'], parent, host, desktop.desktop_name, desktop.input_before)
        row['childSettlement'] = child_settled(binding['host']['pid'], binding['host']['creation100ns'])
        require(row['childSettlement'] in SETTLED, 'exact host settlement unproven')
        row['settlementCertified'] = True
    except Exception as error:
        row['settlementError'] = str(error)
        raise
    monitor.result['identityCertified'] = True
    try:
        text = (directory/'host.log').read_text('utf-8')
        reports, hosts = json_lines(directory/'case.stdout'), json_lines(directory/'host.log')
        row.update(reports=reports, hostReports=hosts)
        records = record_population(case, reports, hosts)
        surface.check_monitor_proxy(binding, records[1], records[3])
        for key, reader in (
            ('target', lambda: surface.read_target(text, case.mode, row['process'], records[3], records[1])),
            ('stages', lambda: init.read_bound_stages(text, hosts, reports, row['process'],
                desktop.desktop_name, desktop.input_before, readback=case.kind != 'fault'))):
            try:
                row[key] = reader()
            except Exception as error:
                row['errors'].append(key+': '+str(error))
        row['diagnosticCollected'] = 'target' in row and 'stages' in row
        try:
            check_workload(case, records, row['process'], desktop.desktop_name, desktop.input_before)
            row['workloadPassed'] = True
        except Exception as error:
            row['errors'].append('workload: '+str(error))
    except Exception as error:
        row['errors'].append('records: '+str(error))
    # Retain a failed arm as-is. Only proven cleanup permits the next planned
    # case; neither new success nor secondary exit can overwrite this result.
    return row


def execute_matrix(run_one):
    rows = []
    for case in CASES:
        row = run_one(case)
        require(type(row) is dict and row.get('index') == case.index and type(row.get('index')) is int
                and row.get('mode') == case.mode and row.get('kind') == case.kind
                and row.get('scenario') == case.scenario and row.get('acceptancePassed') is False,
                'case result scope changed')
        require(row.get('settlementCertified') is True, 'unsettled case; stop without starting the next case')
        rows.append(row)
    return rows


def run_native(binaries, out, palette, receipt, frozen):
    require(os.name == 'nt', 'Windows root native lease only')
    probes = {bits: copy_binary(binaries[f'probe{bits}'], out/f'desktop-probe-{bits}.exe', receipt, frozen)
              for bits in (32, 64)}
    with IsolatedRenderDesktop(restricted_medium=True) as desktop:
        receipt['desktopSafety'] = desktop.receipt
        env = init.recipe_environment(dict(os.environ), out, binaries['host'])
        probe_env = {k: v for k, v in env.items() if k in init.ENV_ALLOW or k in ('DXVK_LOG_LEVEL', 'DXVK_SHADER_CACHE')}
        try:
            receipt['probe'] = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before, '--child', probes[64],
                '--child-log', out/'probe-child.log'], cwd=out, stdout_path=out/'probe.stdout',
                stderr_path=out/'probe.stderr', timeout_s=30, env=probe_env)
        finally:
            receipt['probeRawArtifacts'] = capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log'], True)
        check_launch(receipt['probe'], desktop.desktop_name, desktop.input_before)
        rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
        require(len(rows) == 2, 'both bitness probe records required')
        validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
            station='WinSta0', bits=32, child=rows[1], launch=receipt['probe'])
        execute_matrix(lambda case: run_one_case(case, binaries, out, desktop, palette, receipt, frozen))
    safety = receipt['desktopSafety']
    require(safety['desktopClosed'] is True and safety['cleanupErrors'] == []
            and safety['inputAfterClose'] == safety['inputBefore'], 'Desktop cleanup incomplete')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--execute-native', action='store_true', help='sole root native lease required; never builds')
    args = parser.parse_args(); out = args.output.resolve()
    require(out.is_relative_to(ROOT/'AutoTest/artifacts') and out != ROOT/'AutoTest/artifacts', 'fresh artifact subdirectory')
    out.mkdir(exist_ok=False)
    receipt = dict(schema=1, scope='FROZEN_SURFACE_OWNER_TWELVE_CASE_CONTINUATION_DIAGNOSTIC',
        acceptancePassed=False, gameTouched=False, productIntegrated=False, memorySavingsProven=False,
        peerWindowAcceptancePassed=False, systemChanged=False, productionInputsWritten=False,
        nativeRequested=args.execute_native, rootNativeLeaseRequired=True, buildsAllowed=False,
        sameExeRequired=True, originalTransportMilliseconds=5000, originalExitMilliseconds=5000,
        historicalEnvironmentExactlyReproduced=False, recipeReproducedNotSameEnvironment=True,
        priority='BelowNormal', parallelism=1, prepared=False, diagnosticComplete=False, workloadAllPassed=False,
        plan=[dict(**asdict(c), label=c.label) for c in CASES], copies=[], cases=[])
    frozen = []
    try:
        baseline, r10, binaries, inputs = bind_inputs(); frozen.extend(inputs)
        receipt['readerRevisions'] = reader_revisions()
        receipt['faultExitWatcherCoverage'] = 'not-covered-owner-cleanup-return-intent-not-monitored'
        receipt.update(baselineReceipt=identity(BASELINE), r10Receipt=identity(init.BASELINE),
            diagnosticHost=baseline['diagnosticHost'], binaries={k: identity(v) for k, v in binaries.items()})
        palette = FrozenImage.load(); receipt['paletteFrozenImage'] = palette.receipt()
        helpers = {Path(__file__).resolve(), Path(__file__).with_name('test_surface_owner_continuation.py').resolve(), PE_PATH, PDB_PATH}
        helpers.update(Path(m.__file__).resolve() for m in list(sys.modules.values())
            if getattr(m, '__file__', None) and Path(m.__file__).suffix == '.py'
            and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest'))
        frozen.extend(identity(p) for p in sorted(helpers)); receipt['prepared'] = True
        if args.execute_native:
            run_native(binaries, out, palette, receipt, frozen)
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['frozenInputs'] = frozen
        try:
            receipt['frozenInputs'] = unique_inputs(frozen)
            for row in receipt['frozenInputs']:
                init.copied.same_file(row)
            receipt['frozenInputsPreserved'] = bool(frozen)
        except Exception as error:
            receipt['preservationError'] = str(error)
        try:
            manifests = list(raw_manifests(receipt))
            for manifest in manifests:
                verify_raw_artifacts(manifest)
            receipt['rawArtifactsPreserved'] = bool(manifests)
        except Exception as error:
            receipt['rawPreservationError'] = str(error)
        failures = any(k in receipt for k in ('error', 'preservationError', 'rawPreservationError'))
        receipt['preparationComplete'] = bool(receipt['prepared'] and receipt.get('frozenInputsPreserved') and not failures)
        receipt['diagnosticComplete'] = bool(args.execute_native and receipt['preparationComplete']
            and receipt.get('rawArtifactsPreserved') and len(receipt['cases']) == 12
            and all(c['settlementCertified'] and c['diagnosticCollected'] for c in receipt['cases']))
        receipt['workloadAllPassed'] = bool(receipt['diagnosticComplete'] and all(c['workloadPassed'] for c in receipt['cases']))
        receipt['acceptancePassed'] = False
        with (out/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('acceptancePassed', 'preparationComplete', 'diagnosticComplete',
        'workloadAllPassed', 'error', 'preservationError', 'rawPreservationError')}))
    return 0 if (receipt['workloadAllPassed'] if args.execute_native else receipt['preparationComplete']) else 1


if __name__ == '__main__':
    sys.exit(main())
