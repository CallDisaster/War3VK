"""Build a fresh x64 existing-DXVK candidate without executing any native test.

The sole build owner runs this BelowNormal/-j2. Old build trees, binaries,
receipts and player files remain read-only. Runtime gates must bind this receipt
and run separately on a verified non-input Desktop.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from run_render_host_core_gate import ROOT, source_paths, check_host_compile_boundary, execute
from run_render_host_protocol_gate import identity, pe_identity
from run_render_host_transport_gate import require
from render_lab_raw_artifacts import capture_raw_artifacts, raw_manifests, verify_raw_artifacts

TARGET = Path('tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe')
PROTECTED = (
    Path('E:/Work/Warcraft III/d3d9.dll'),
    ROOT/'build32_weather_20261010/src/d3d9/d3d9.dll',
    ROOT/'build32_device_owner_presenter_20261022/src/d3d9/d3d9.dll',
    ROOT/'build32_device_owner_shader_20261022/src/d3d9/d3d9.dll',
    ROOT/'build64_render_host_20261011'/TARGET,
)


def sources():
    result = set(source_paths())
    result.update(p for p in (ROOT/'tools/render_host').rglob('*')
                  if p.is_file() and (p.suffix in ('.h', '.cpp', '.def') or p.name == 'meson.build'))
    result.add(Path(__file__).resolve())
    return sorted(result)


def check_paths(build, output):
    require(build.parent == ROOT and build.name.startswith('build64_render_host_'), 'dedicated x64 build path')
    require(not build.exists(), 'fresh independent build only; old products are not overwritten')
    require(output.parent == ROOT/'AutoTest/artifacts' and not output.exists(), 'fresh artifact child')


def run_step(argv, output, stem, timeout, env, receipt):
    try:
        receipt['steps'].append(execute(argv, ROOT, output/stem, timeout, env))
    finally:
        receipt['stepRawArtifacts'].append(capture_raw_artifacts(output, [stem+'.stdout', stem+'.stderr']))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--configure-timeout', type=int, choices=(180, 600), default=180,
                        help='build-tool configuration watchdog only; no native runtime gate changes')
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    check_paths(build, output)
    output.mkdir()
    receipt = dict(schema='warvk-x64-candidate-build/v1', ok=False, nativeExecuted=False,
                   productIntegrated=False, releaseReady=False, priority='BelowNormal', parallelism=2,
                   build=str(build), steps=[], stepRawArtifacts=[])
    receipt['configureTimeoutSeconds'] = args.configure_timeout
    env = os.environ.copy()
    env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;' + env['PATH']
    before = [identity(p) for p in sources()]
    receipt['sources'] = before
    receipt['protectedBefore'] = [identity(p) for p in PROTECTED]
    try:
        require(os.name == 'nt', 'Windows build owner only')
        options = ['-D'+k+'=false' for k in
                   ('enable_dxgi', 'enable_d3d8', 'enable_d3d9', 'enable_d3d10', 'enable_d3d11')]
        run_step([sys.executable, '-m', 'mesonbuild.mesonmain', 'setup', build, ROOT,
                  '--buildtype=release', '--cross-file', ROOT/'build-win64-render-host-llvm.txt',
                  '-Dwarvk_render_host_core_dev=true', *options], output, 'configure', args.configure_timeout, env, receipt)
        receipt['allowedSharedD3D9Inputs'] = check_host_compile_boundary(build)
        run_step(['ninja', '-C', build, '-j2', TARGET.as_posix()], output, 'build', 1800, env, receipt)
        run_step(['ninja', '-C', build, '-n', TARGET.as_posix()], output, 'no-work', 60, env, receipt)
        require('no work to do' in (output/'no-work.stdout').read_text('utf-8'), 'build did not settle')
        require(pe_identity(build/TARGET) == (0x8664, 0x20b), 'x64 PE32+ host required')
        receipt['host'] = identity(build/TARGET)
        receipt['compileCommands'] = identity(build/'compile_commands.json')
        receipt['buildNinja'] = identity(build/'build.ninja')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            receipt['sourcesPreserved'] = before == [identity(p) for p in sources()]
            require(receipt['sourcesPreserved'], 'source drift during build')
            receipt['protectedAfter'] = [identity(p) for p in PROTECTED]
            receipt['protectedUnchanged'] = receipt['protectedBefore'] == receipt['protectedAfter']
            require(receipt['protectedUnchanged'], 'protected product changed')
            for item in raw_manifests(receipt):
                verify_raw_artifacts(item)
        except Exception as error:
            receipt['ok'] = False
            receipt['preservationError'] = str(error)
        with (output/'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
    print(json.dumps({k: receipt.get(k) for k in ('ok', 'error', 'preservationError', 'protectedUnchanged')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
