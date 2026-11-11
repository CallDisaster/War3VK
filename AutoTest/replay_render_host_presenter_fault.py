"""Replay ONE preserved standalone fault fixture on an actual non-input Desktop.

Diagnostic only: not the aggregate acceptance gate. Source and executable hashes
must match the named build receipt before and after execution; never rebuilds,
deploys, changes a timeout, or substitutes binaries from a newer source tree.
"""
import argparse
import json
import os
from pathlib import Path
import shutil

from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from run_render_host_protocol_gate import ROOT, identity
from run_render_host_presenter_gate import json_lines, require, check_fault


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--receipt', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    original = json.loads(args.receipt.read_text('utf-8'))
    out = args.output.resolve()
    require(out.parent == ROOT / 'AutoTest/artifacts', 'dedicated artifact child required')
    out.mkdir(exist_ok=False)
    receipt = dict(scope='DIAGNOSTIC_SINGLE_PRESERVED_CASE', ok=False, gameTouched=False,
                   aggregateAcceptance=False, originalReceipt=identity(args.receipt.resolve()))
    guarded = original['sources'] + original['binaries'] + [original['referenceBefore'], original['playerBefore']]
    try:
        require(all(row == identity(Path(row['path'])) for row in guarded), 'preserved input drift')
        base = args.receipt.resolve().parent
        receipt['executables'] = []
        def copy(source, name):
            target = out / name
            shutil.copy2(source, target)
            receipt['executables'].append(dict(source=identity(source), copy=identity(target)))
            return target
        parent = copy(base / 'd3d9-presenter-fault32.exe', 'd3d9-presenter-fault32.exe')
        proxy = copy(base / 'warvk-d3d9-proxy-dev.dll', 'proxy.dll')
        host_row = next(row for row in original['binaries'] if Path(row['path']).name == 'warvk-d3d9-host-dev.exe')
        host = copy(Path(host_row['path']), 'warvk-d3d9-host-dev.exe')
        probe32 = copy(base / 'desktop-probe-32.exe', 'desktop-probe-32.exe')
        probe64 = copy(base / 'desktop-probe-64.exe', 'desktop-probe-64.exe')
        env = {k: v for k, v in os.environ.items() if k.upper() in {
            'SYSTEMROOT', 'WINDIR', 'PATH', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA'}}
        env.update(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0', DXVK_LOG_PATH=str(out),
                   DXVK_CONFIG_FILE=str(out/'absent.conf'), WARVK_D3D9_HOST_EXE=str(host),
                   WARVK_D3D9_HOST_LOG=str(out/'host.log'),
                   WARVK_D3D9_PROFILE='presenter-readback-isolated-v1')
        with IsolatedRenderDesktop() as desktop:
            receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([probe32, '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', probe64, '--child-log', out/'probe-child.log'], cwd=out,
                stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['probe'] = probe
            require(probe['ok'], 'native isolation probe failed')
            rows = json_lines(out/'probe.stdout') + json_lines(out/'probe-child.log')
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                                   station='WinSta0', bits=32, child=rows[1], launch=probe)
            launch = desktop.run([parent, '--dll', proxy, '--scenario', 'marker',
                '--expect-desktop', desktop.desktop_name, '--input-desktop', desktop.input_before,
                '--diagnostic-readback', '1'], cwd=out, stdout_path=out/'case.stdout',
                stderr_path=out/'case.stderr', timeout_s=45, env=env)
            receipt['process'] = launch
            receipt['reports'] = json_lines(out/'case.stdout')
            receipt['hostReports'] = json_lines(out/'host.log')
            require(launch['ok'], 'native diagnostic failed; inspect retained stderr')
            f = next(r for r in receipt['reports'] if r['schema']=='warvk-d3d9-presenter-fault-fixture/v1')
            p = next(r for r in receipt['reports'] if r['schema']=='warvk-d3d9-proxy/v1')
            h = next(r for r in receipt['hostReports'] if r['schema']=='warvk-d3d9-host-fault/v1')
            d = next(r for r in receipt['hostReports'] if r['schema']=='warvk-d3d9-desktop/v1')
            check_fault(f,p,h,d,launch,'marker',desktop.desktop_name,desktop.input_before,True)
            receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        receipt['preserved'] = all(row == identity(Path(row['path'])) for row in guarded)
        if not receipt['preserved']:
            receipt['ok'] = False
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps(dict(ok=receipt['ok'], preserved=receipt['preserved'], receipt=str(out/'receipt.json'))))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
