"""Serial full offline regression of the dedicated device-owner build.

Uses existing Meson tests without modifying acceptance criteria. This is not
Warcraft/gameplay/WSI acceptance, and never installs the product DLL.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import sys

from run_render_host_protocol_gate import ROOT, identity
from run_render_host_core_gate import execute
from run_render_host_transport_gate import require, strict_json
from run_device_owned_build import inputs
from run_render_host_d3d9_gate import json_lines
from run_render_host_presenter_gate import check_launch
from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_raw_artifacts import capture_raw_artifacts, verify_raw_artifacts, raw_manifests


def regression_sources():
    paths = set(inputs())
    paths.update(p for p in (ROOT / 'AutoTest').iterdir()
                 if p.is_file() and p.suffix in ('.py', '.cpp', '.h'))
    return sorted(paths)


def check_test_population(configured, entries):
    """Pure reader: no empty-set success and no coercion of test identities."""
    require(type(configured) is list and configured and type(entries) is list and entries,
            'configured and executed test populations must both be nonempty lists')
    for rows, label in ((configured, 'configured'), (entries, 'executed')):
        require(all(type(row) is dict and type(row.get('name')) is str and row['name']
                    and row['name'].strip() == row['name'] for row in rows), label+' test identity types')
    names = [row['name'] for row in configured]
    actual = [row['name'].removeprefix('warvk:') for row in entries]
    require(all(actual) and len(set(names)) == len(names) and len(set(actual)) == len(actual)
            and len(actual) == len(names) and set(actual) == set(names),
            'configured/executed nonempty unique test population mismatch')
    for row in entries:
        require(type(row.get('result')) is str and row['result'] == 'OK'
                and type(row.get('returncode')) is int and row['returncode'] == 0,
                'a test failed, skipped, timed out or did not exit zero')
        if 'duration' in row:
            require(type(row['duration']) in (int, float) and math.isfinite(row['duration'])
                    and row['duration'] >= 0, 'invalid test duration')
        for name in ('stdout', 'stderr'):
            if name in row: require(type(row[name]) is str, 'invalid test '+name)
    return [{key: row[key] for key in ('name', 'result', 'returncode', 'duration', 'stdout', 'stderr')
             if key in row} for row in entries]


def check_isolated_launch(row, desktop, initial):
    check_launch(row, desktop, initial)
    require(not row.get('error'), 'isolated regression launch exception')
    token = row['restrictedToken']
    require(type(token) is dict and type(token['schema']) is int and token['schema'] == 1
            and token['mode'] == 'restricted-medium-per-child/v1', 'restricted-Medium receipt required')
    for name in ('ready', 'parentTokenUnchanged', 'derivedTokenClosed', 'childTokenVerifiedBeforeResume',
                 'noHighIntegrityFallback', 'resumedAfterVerification'):
        require(token[name] is True, 'restricted-Medium boundary '+name)
    require(token['globalSecurityChanged'] is False and token['cleanupErrors'] == []
            and type(token['resumePreviousCount']) is int and token['resumePreviousCount'] == 1,
            'restricted-Medium cleanup or resume failed')


def copy_raw_exact(source, destination):
    """Exclusive-create a historical copy before reading its JSON content."""
    before = identity(source)
    require(source.resolve(strict=True) == source and not source.is_symlink(), 'redirected Meson evidence')
    with source.open('rb') as original, destination.open('xb') as copied:
        shutil.copyfileobj(original, copied)
    after, frozen = identity(source), identity(destination)
    require(before == after and all(before[key] == frozen[key] for key in ('bytes', 'sha256')),
            'Meson raw evidence changed while being copied')
    return dict(source=before, copy=frozen)


def capture_step(receipt, output, required, key=None):
    # Capture available failed-run bytes too. Never replace an initial manifest
    # in finalization, or silently hide the original launch failure.
    try:
        manifest = capture_raw_artifacts(output, required)
        if key is None: receipt['stepRawArtifacts'].append(manifest)
        else:
            require(key not in receipt, 'initial raw manifest already exists')
            receipt[key] = manifest
        return manifest
    except Exception as exc:
        receipt.setdefault('rawCaptureErrors', []).append(str(exc))
        return None


def execute_step(receipt, argv, output, label, timeout, env):
    try:
        result = execute(argv, ROOT, output/label, timeout, env)
        receipt['steps'].append(result)
        return result
    finally:
        capture_step(receipt, output, [label+'.stdout', label+'.stderr'])


def fresh_logbase(build, output):
    require(re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', output.name), 'safe unique output-derived Meson logbase')
    base = 'warvk-owned-'+output.name
    require(not list((build/'meson-logs').glob(base+'.*')), 'unique Meson logbase already exists')
    return base


def freeze_meson_log(receipt, build, output):
    # --logbase is unique and absent before this launch; a stale default log
    # cannot satisfy the gate. Exclusive-created copies preserve these bytes
    # even after the build directory is later reused or removed.
    receipt['mesonLogCopies'] = []
    for suffix in ('.json', '.txt', '.junit.xml'):
        try:
            source = build/'meson-logs'/(receipt['mesonLogbase']+suffix)
            if source.is_file():
                pair = copy_raw_exact(source, output/('testlog'+suffix))
                receipt['mesonLogCopies'].append(pair)
                if suffix == '.json': receipt['testLogCopy'] = pair
        except Exception as exc:
            receipt.setdefault('rawCaptureErrors', []).append(str(exc))
    capture_step(receipt, output, ['testlog.json', 'testlog.txt', 'testlog.junit.xml'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--isolated-desktop', action='store_true',
                        help='run all configured tests on a verified restricted-Medium non-input Desktop')
    parser.add_argument('--probe32', type=Path)
    parser.add_argument('--probe64', type=Path)
    args = parser.parse_args()
    if args.isolated_desktop != bool(args.probe32 and args.probe64) or bool(args.probe32) != bool(args.probe64):
        raise ValueError('isolated execution requires both exact prebuilt CPU Desktop probes')
    build, output = args.build.resolve(strict=True), args.output.resolve()
    if build.parent != ROOT or not build.name.startswith('build32_device_owner_'):
        raise ValueError('dedicated device-owner build required')
    if output.parent != ROOT / 'AutoTest/artifacts':
        raise ValueError('fresh direct artifact child required')
    output.mkdir(exist_ok=False)
    sources = before = None
    protected = [build / 'src/d3d9/d3d9.dll',
                 ROOT / 'build32_weather_20261010/src/d3d9/d3d9.dll',
                 Path('E:/Work/Warcraft III/d3d9.dll')]
    receipt = dict(schema='warvk-device-owned-regressions/v1', ok=False,
                   scope='FULL_CONFIGURED_MESON_OFFLINE_TESTS', steps=[], stepRawArtifacts=[], gameLaunched=False,
                   deployed=False, releaseReady=False, parallelism=2,
                   priority='BelowNormal', isolatedDesktopRequested=args.isolated_desktop)
    # Meson writes its environment into testlog.json. Do not propagate service
    # tokens or agent/session credentials to a regression artifact.
    safe_env = {'PATH', 'PATHEXT', 'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC',
                'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA',
                'PROGRAMDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)', 'PROGRAMW6432',
                'ALLUSERSPROFILE', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE'}
    env = {k: v for k, v in os.environ.items() if k.upper() in safe_env}
    env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;' + env['PATH']
    env['WARVK_CPU_TEST_BUILD_DIR'] = str(build)
    probes = None
    try:
        sources = [identity(p) for p in regression_sources()]; receipt['sources'] = sources
        before = [identity(p) for p in protected]; receipt['protectedBefore'] = before
        execute_step(receipt, ['ninja', '-C', build, '-j2'], output, 'build-tests', 1800, env)
        receipt['configuredCopy'] = copy_raw_exact(build/'meson-info/intro-tests.json', output/'intro-tests.json')
        configured_manifest = capture_step(receipt, output, ['intro-tests.json'])
        verify_raw_artifacts(configured_manifest, output, ['intro-tests.json'], False)
        receipt['mesonLogbase'] = fresh_logbase(build, output)
        test_command = [sys.executable, '-m', 'mesonbuild.mesonmain',
            'test', '-C', build, '--no-rebuild', '--print-errorlogs', '-j2', '--logbase', receipt['mesonLogbase']]
        if args.isolated_desktop:
            probes = {bits: path.resolve(strict=True) for bits, path in ((32, args.probe32), (64, args.probe64))}
            receipt['probesBefore'] = [identity(probes[bits]) for bits in (32, 64)]
            desktop = IsolatedRenderDesktop(restricted_medium=True)
            receipt['desktopSafety'] = desktop.receipt
            with desktop:
                try:
                    receipt['desktopProbe'] = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                        '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                        '--child', probes[64], '--child-log', output/'desktop-child.log'], cwd=output,
                        stdout_path=output/'probe.stdout', stderr_path=output/'probe.stderr', timeout_s=30, env=env)
                finally:
                    capture_step(receipt, output, ['probe.stdout', 'probe.stderr', 'desktop-child.log'], 'probeRawArtifacts')
                verify_raw_artifacts(receipt['probeRawArtifacts'], output,
                    ['probe.stdout', 'probe.stderr', 'desktop-child.log'], False)
                check_isolated_launch(receipt['desktopProbe'], desktop.desktop_name, desktop.input_before)
                rows = json_lines(output/'probe.stdout') + json_lines(output/'desktop-child.log')
                if len(rows) != 2:
                    raise ValueError('both actual CPU Desktop witnesses required')
                validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                    station='WinSta0', bits=32, child=rows[1], launch=receipt['desktopProbe'])
                require(rows[0]['main']['threadId'] == receipt['desktopProbe']['initialThreadId'],
                        'CPU probe initial thread identity differs')
                receipt['probeReports'] = rows
                # Preserve the launcher's existing 300 s maximum; do not enlarge
                # it to the old default-desktop 900 s offline wrapper deadline.
                try:
                    receipt['testProcess'] = desktop.run(test_command, cwd=ROOT,
                        stdout_path=output/'meson-tests.stdout', stderr_path=output/'meson-tests.stderr',
                        timeout_s=300, env=env)
                finally:
                    capture_step(receipt, output, ['meson-tests.stdout', 'meson-tests.stderr'])
                    freeze_meson_log(receipt, build, output)
                check_isolated_launch(receipt['testProcess'], desktop.desktop_name, desktop.input_before)
                require((receipt['desktopProbe']['pid'], receipt['desktopProbe']['creation100ns']) !=
                        (receipt['testProcess']['pid'], receipt['testProcess']['creation100ns']),
                        'CPU probe and Meson must be independently launched processes')
            if receipt['desktopSafety']['ok'] is not True:
                raise ValueError('offline regression Desktop cleanup failed')
        else:
            try: execute_step(receipt, test_command, output, 'meson-tests', 900, env)
            finally: freeze_meson_log(receipt, build, output)
        entries = [strict_json(line) for line in (output/'testlog.json').read_text('utf-8').splitlines() if line]
        configured = strict_json((output/'intro-tests.json').read_text('utf-8'))
        receipt['results'] = check_test_population(configured, entries)
        receipt['testCount'] = len(entries)
        receipt['testLog'] = receipt['testLogCopy']['copy']
        execute_step(receipt, ['ninja', '-C', build, '-n'], output, 'no-work', 60, env)
        if 'no work to do' not in (output / 'no-work.stdout').read_text('utf-8'):
            raise ValueError('build did not settle')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        errors = list(receipt.get('rawCaptureErrors', []))
        try:
            if before is not None:
                receipt['protectedAfter'] = [identity(p) for p in protected]
                receipt['productAndPlayerPreserved'] = before == receipt['protectedAfter']
                require(receipt['productAndPlayerPreserved'], 'product/player identity changed')
            if sources is not None:
                receipt['sourcePreserved'] = sources == [identity(p) for p in regression_sources()]
                require(receipt['sourcePreserved'], 'source identity changed')
        except Exception as exc: errors.append(str(exc))
        try:
            if 'probesBefore' in receipt:
                receipt['probesAfter'] = [identity(probes[bits]) for bits in (32, 64)]
                receipt['probesPreserved'] = receipt['probesBefore'] == receipt['probesAfter']
                require(receipt['probesPreserved'], 'offline regression CPU probes changed')
        except Exception as exc: errors.append(str(exc))
        for manifest in raw_manifests(receipt):
            try: verify_raw_artifacts(manifest)
            except Exception as exc: errors.append(str(exc))
        copies = ([receipt['configuredCopy']] if 'configuredCopy' in receipt else []) + receipt.get('mesonLogCopies', [])
        for pair in copies:
            try:
                require(pair['source'] == identity(Path(pair['source']['path'])),
                        'Meson source changed during gate: '+pair['source']['path'])
            except Exception as exc: errors.append(str(exc))
        receipt['rawArtifactsPreserved'] = not errors
        if errors:
            receipt['ok'] = False; receipt['preservationErrors'] = errors
        (output / 'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(ok=receipt['ok'], error=receipt.get('error'), receipt=str(output / 'receipt.json'))))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
