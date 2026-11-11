"""CPU-only acceptance of an explicitly restricted non-input Desktop launch.

Takes already-built, frozen x86/x64 desktop probes. Does not build, load a GPU
module, start the game, change global security or fall back to a normal token.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys

from isolated_render_desktop import IsolatedRenderDesktop, validate_probe_receipt
from render_lab_token import validate_restricted
from run_render_host_protocol_gate import identity, pe_identity
from run_render_host_transport_gate import require
from run_render_host_d3d9_gate import json_lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ('probe32', 'probe64', 'output'):
        ap.add_argument('--' + name, type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='CPU_ONLY_RESTRICTED_MEDIUM_NONINPUT_DESKTOP', ok=False,
        gpuRun=False, gameTouched=False, globalSecurityChanged=False,
        x64GrandchildTokenDirectlyObserved=False, copies=[])
    inputs = []
    try:
        for path in (Path(__file__), Path(__file__).with_name('render_lab_token.py'),
                     Path(__file__).with_name('render_lab_default_dacl.py'),
                     Path(__file__).with_name('isolated_render_desktop.py')):
            inputs.append(identity(path.resolve()))
        probes = {}
        for bits, path, pe in ((32, args.probe32, (0x14c, 0x10b)),
                              (64, args.probe64, (0x8664, 0x20b))):
            source = path.resolve(strict=True); require(pe_identity(source) == pe, 'exact probe architecture')
            before = identity(source); inputs.append(before)
            target = out / f'desktop-probe-{bits}.exe'; shutil.copy2(source, target)
            copied = identity(target)
            require(all(before[key] == copied[key] for key in ('bytes', 'sha256')), 'probe copy changed')
            receipt['copies'].append(copied); probes[bits] = target
        receipt['inputs'] = inputs
        env = {key: value for key, value in os.environ.items() if key.upper() in {
            'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATH', 'TEMP', 'TMP', 'USERPROFILE',
            'APPDATA', 'LOCALAPPDATA', 'PROGRAMFILES', 'PROGRAMFILES(X86)', 'PROGRAMDATA'}}
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            launch = desktop.run([probes[32], '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', probes[64], '--child-log', out / 'child.log'], cwd=out,
                stdout_path=out / 'probe.stdout', stderr_path=out / 'probe.stderr', timeout_s=30, env=env)
            receipt['launch'] = launch
            require(launch['ok'] is True,
                    f"CPU probe process failed: exit={launch.get('exitCode')} timeout={launch.get('timedOut')}")
            rows = json_lines(out / 'probe.stdout') + json_lines(out / 'child.log')
            receipt['probeReports'] = rows
            require(len(rows) == 2, 'both actual CPU probes required')
            validate_probe_receipt(rows[0], desktop=desktop.desktop_name, input_desktop=desktop.input_before,
                station='WinSta0', bits=32, child=rows[1], launch=launch)
            token = launch['restrictedToken']
            validate_restricted(token['sourceBefore'], token['childBeforeResume'])
            require(token['ready'] is True and token['parentTokenUnchanged'] is True
                and token['childTokenVerifiedBeforeResume'] is True and token['resumedAfterVerification'] is True
                and token['resumePreviousCount'] == 1 and token['derivedTokenClosed'] is True
                and not token['cleanupErrors'], 'derived token lifecycle incomplete')
        require(receipt['desktopSafety']['ok'] is True, 'Desktop cleanup failed')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        try:
            receipt['inputsPreserved'] = inputs == [identity(Path(row['path'])) for row in inputs]
            receipt['copiesPreserved'] = all(row == identity(Path(row['path'])) for row in receipt['copies'])
            require(receipt['inputsPreserved'] and receipt['copiesPreserved'], 'frozen probe/source drift')
        except Exception as error:
            receipt['ok'] = False; receipt['preservationError'] = str(error)
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'error', 'scope', 'inputsPreserved')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
