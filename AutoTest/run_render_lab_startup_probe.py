"""Narrow kernel-only -> explicit USER32 CPU startup diagnostic, no GPU.

Builds only two tiny probes; runs each once in the same owned non-input Desktop
with an explicitly derived Medium token. Import tables must contain kernel32
only. No shared ACL, registry, privilege, injection, or default-desktop fallback.
Failure output is preserved, including a normal kernel entry with load failure.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

from isolated_render_desktop import IsolatedRenderDesktop
from run_render_host_core_gate import execute
from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--toolchain', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--reuse-probes', type=Path,
                    help='reuse exact compiled probes from a preserved startup receipt')
    ap.add_argument('--unrestricted-control', action='store_true',
                    help='explicit CPU-only control; never an automatic privilege fallback')
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    source = ROOT/'AutoTest/test_render_lab_startup.c'
    receipt = dict(schema=1, scope='KERNEL_USER32_CPU_STARTUP_DIAGNOSTIC', ok=False,
                   gpuRun=False, gameTouched=False, globalSecurityChanged=False, builds=[], runs=[],
                   restrictedMediumRequested=not args.unrestricted_control, automaticFallback=False)
    inputs = [identity(source), identity(Path(__file__)),
        identity(ROOT/'AutoTest/isolated_render_desktop.py'), identity(ROOT/'AutoTest/render_lab_token.py'),
        identity(ROOT/'AutoTest/render_lab_default_dacl.py')]
    try:
        exes = []
        readobj = args.toolchain.resolve(strict=True)/'llvm-readobj.exe'
        inputs.append(identity(readobj))
        previous = None
        if args.reuse_probes is not None:
            previous_path = args.reuse_probes.resolve(strict=True)
            inputs.append(identity(previous_path))
            previous = json.loads(previous_path.read_text('utf-8'))
            require(previous['scope'] == receipt['scope'] and len(previous['builds']) == 2,
                    'two original CPU probe builds required, even when runtime failed')
            require(any(row == identity(source) for row in previous['inputs']), 'probe source drift')
        for bits, target, entry, pe in ((32, 'i686', '_ProbeEntry', (0x14c, 0x10b)),
                                       (64, 'x86_64', 'ProbeEntry', (0x8664, 0x20b))):
            cc = args.toolchain.resolve(strict=True)/(target+'-w64-mingw32-clang.exe')
            inputs.append(identity(cc)); exe = out/f'startup-{bits}.exe'
            if previous is None:
                build = execute([cc, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-ffreestanding', '-fno-builtin', '-fno-stack-protector', '-nostdlib', source, '-Wl,--subsystem,console',
                    '-Wl,--entry,'+entry, '-lkernel32', '-o', exe], ROOT, out/f'build-{bits}', 60)
                require(build['exit'] == 0, 'CPU-only probe compile')
            else:
                matches = [row for row in previous['builds'] if row['bits'] == bits]
                require(len(matches) == 1, 'unique original probe width')
                original = matches[0]['exe']; original_path = Path(original['path'])
                require(identity(original_path) == original, 'frozen original CPU probe drift')
                inputs.append(original); shutil.copy2(original_path, exe)
                require(all(identity(exe)[key] == original[key] for key in ('bytes', 'sha256')), 'probe copy drift')
                build = dict(reusedFrom=original, rebuilt=False)
            imports = execute([readobj, '--coff-imports', exe], ROOT, out/f'imports-{bits}', 30)
            require(imports['exit'] == 0, 'CPU-only import inspection')
            import_text = (out/f'imports-{bits}.stdout').read_text('utf-8')
            names = re.findall(r'^\s+Name:\s+(\S+)', import_text, re.M)
            require([name.casefold() for name in names] == ['kernel32.dll'], 'kernel32-only import table')
            require(pe_identity(exe) == pe, 'probe architecture')
            receipt['builds'].append(dict(bits=bits, exe=identity(exe), build=build, imports=names))
            exes.append(exe)
        env = {k: v for k, v in os.environ.items() if k.upper() in {
            'SYSTEMROOT', 'WINDIR', 'PATH', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA'}}
        with IsolatedRenderDesktop(restricted_medium=not args.unrestricted_control) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            for bits, exe in zip((32, 64), exes):
                launch = desktop.run([exe], cwd=out, stdout_path=out/f'run-{bits}.stdout',
                                    stderr_path=out/f'run-{bits}.stderr', timeout_s=15, env=env)
                text = (out/f'run-{bits}.stdout').read_text('utf-8')
                receipt['runs'].append(dict(bits=bits, launch=launch, stdout=text))
                require(launch['processSettled'] and launch['jobEmpty'] and not launch['forcedTermination'],
                        'prior CPU probe must settle before diagnosing other width')
                receipt['runs'][-1]['startupWitness'] = bool(launch['ok'] is True
                    and 'KERNEL_ENTRY\n' in text and 'USER32_LOADED=1\n' in text
                    and 'STATION=WinSta0\n' in text and 'DESKTOP='+desktop.desktop_name+'\n' in text
                    and text.endswith('CPU_PROBE_COMPLETE\n'))
        require(all(row['startupWitness'] for row in receipt['runs']), 'CPU startup witness failed')
        require(receipt['desktopSafety']['ok'] is True, 'CPU diagnostic Desktop cleanup')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['inputs'] = inputs
        receipt['inputsPreserved'] = all(row == identity(Path(row['path'])) for row in inputs)
        if not receipt['inputsPreserved']: receipt['ok'] = False
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({key: receipt.get(key) for key in ('ok', 'error', 'scope', 'inputsPreserved')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
