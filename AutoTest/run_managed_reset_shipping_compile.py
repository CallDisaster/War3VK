"""Compile the actual shipping JAPI TU using a preserved build's exact flags.

No protocol-test macro, no link, no Ninja regeneration and no modification of
the existing build outputs/DLL. Object and diagnostics go to a fresh artifact.
This is a compile check, never a shipping DLL or backend runtime acceptance.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from mesonbuild.mesonlib import split_args
from run_render_host_protocol_gate import ROOT, identity
from run_render_host_transport_gate import require


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'scope': 'ACTUAL_SHIPPING_JAPI_TRANSLATION_UNIT_ONLY', 'ok': False,
               'protocolTestMacro': False, 'dllLinked': False, 'gameLaunched': False}
    try:
        source = ROOT/'src/d3d9/war3/japi/war3_japi_v1.cpp'
        inputs = [source, source.with_name('war3_japi_v1.h'), source.with_name('war3_managed_object_reset.h'),
                  build/'compile_commands.json', Path(__file__).resolve()]
        before = [identity(p) for p in inputs]
        dll = build/'src/d3d9/d3d9.dll'
        dll_before = identity(dll)
        entries = json.loads((build/'compile_commands.json').read_text(encoding='utf-8'))
        matches = [e for e in entries if Path(e['directory'], e['file']).resolve() == source
                   and 'd3d9.dll.p' in e.get('command', '')]
        require(len(matches) == 1, 'must find exactly the production DLL JAPI translation unit')
        command = split_args(matches[0]['command'])
        require(not any('WARVK_JAPI_PROTOCOL_TEST' in a for a in command), 'test macro must be absent')
        filtered = []
        it = iter(command)
        for item in it:
            if item in ('-o', '-MF', '-MQ', '-MT'):
                next(it)
            elif item not in ('-MD', '-MMD'):
                filtered.append(item)
        obj = output/'shipping-war3-japi-v1.obj'
        filtered += ['-o', str(obj)]
        receipt['command'] = filtered
        receipt['inputs'] = before
        receipt['preservedDll'] = dll_before
        receipt['compiler'] = identity(Path(filtered[0]))
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        with (output/'compile.stdout').open('xb') as out, (output/'compile.stderr').open('xb') as err:
            result = subprocess.run(filtered, cwd=build, stdout=out, stderr=err,
                                    creationflags=flags, timeout=180)
        require(result.returncode == 0, f'production translation unit exit {result.returncode}')
        require(identity(dll) == dll_before, 'preserved DLL changed')
        require([identity(p) for p in inputs] == before, 'source/config changed during compile')
        receipt['object'] = identity(obj)
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2, ensure_ascii=False)
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output/'receipt.json'),
                      'error': receipt.get('error')}, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
