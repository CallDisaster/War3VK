"""Fresh x64 build of the real DXVK core and bounded GPU-buffer bootstrap.

No game, shipping DLL, remote deployment or repository cleanup. This establishes
an existing-core host integration surface, not a finished D3D9 remote renderer.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_transport_gate import require, strict_json


def source_paths():
    # Relevant core/build sources. The configured D3D9 dev target now shares
    # DXSO/options/SPIR-V helpers; D3D9 device/game code remains forbidden.
    domains = ('src/dxvk', 'src/util', 'src/wsi', 'src/vulkan', 'src/spirv',
               'include', 'subprojects/dxbc-spirv', 'subprojects/libdisplay-info',
               'tools/render_host/dxvk_core', 'src/dxso')
    paths = set()
    for domain in domains:
        for p in (ROOT / domain).rglob('*'):
            if p.is_file() and (p.suffix in ('.cpp', '.c', '.h', '.hpp', '.vert', '.frag', '.comp', '.glsl')
                                or p.name in ('meson.build', 'meson_options.txt')):
                paths.add(p)
    for relative in ('meson.build', 'meson_options.txt', 'src/meson.build', 'version.h',
                     'buildenv.h.in', 'build-win64-render-host-llvm.txt',
                     'AutoTest/run_render_host_core_gate.py',
                     'AutoTest/run_render_host_protocol_gate.py',
                     'AutoTest/run_render_host_transport_gate.py'):
        paths.add(ROOT / relative)
    paths.update((ROOT/'src/d3d9').rglob('*.h'))
    paths.update(ROOT / relative for relative in
                 ('src/d3d9/d3d9_options.cpp', 'src/d3d9/d3d9_shader_helpers.cpp',
                  'src/d3d9/d3d9_fixed_function.cpp', 'src/d3d9/d3d9_adapter.cpp',
                  'src/d3d9/d3d9_format.cpp', 'src/d3d9/d3d9_monitor.cpp',
                  'src/d3d9/d3d9_names.cpp', 'src/dxso/meson.build'))
    paths.update((ROOT/'src/d3d9/shaders').rglob('*.*'))
    return sorted(paths)


def check_host_compile_boundary(build):
    """Inspect actual compiler inputs, not a substring anywhere in its flags."""
    # One more original compiler TU, not the D3D9 device or game. This explicit
    # expansion belongs to fresh FF receipts; historical gates are unchanged.
    allowed = {ROOT/'src/d3d9/d3d9_options.cpp', ROOT/'src/d3d9/d3d9_shader_helpers.cpp',
               ROOT/'src/d3d9/d3d9_fixed_function.cpp', ROOT/'src/d3d9/d3d9_adapter.cpp',
               ROOT/'src/d3d9/d3d9_format.cpp', ROOT/'src/d3d9/d3d9_monitor.cpp',
               ROOT/'src/d3d9/d3d9_names.cpp'}
    observed = set()
    for entry in strict_json((build/'compile_commands.json').read_text('utf-8')):
        source = (Path(entry['directory']) / entry['file']).resolve()
        require(not source.is_relative_to(ROOT/'subprojects/StormBreaker'), 'StormBreaker entered x64 build')
        if source.is_relative_to(ROOT/'src/d3d9'):
            require(source in allowed, 'D3D9 device/game source entered x64 build: '+str(source))
            observed.add(source)
    return sorted(str(path.relative_to(ROOT)) for path in observed)


def execute(argv, cwd, prefix, timeout, env=None):
    flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
    started = time.monotonic()
    with prefix.with_suffix('.stdout').open('xb') as out, prefix.with_suffix('.stderr').open('xb') as err:
        process = subprocess.Popen(list(map(str, argv)), cwd=cwd, env=env, stdout=out, stderr=err,
                                   creationflags=flags)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            # This gate never starts a game or user-owned process. A watchdog
            # failure is retained, not accepted as normal teardown.
            process.kill()
            process.wait(timeout=10)
            raise RuntimeError(f'{prefix.name}: whole-process watchdog failure')
    require(code == 0, f'{prefix.name}: exit {code}; see {prefix}.stdout/.stderr')
    return {'command': list(map(str, argv)), 'exit': code, 'seconds': time.monotonic()-started}


def check_runtime(result):
    require(result['schema'] == 'warvk-dxvk-core-bootstrap/v1' and result['success'] is True,
            'native bootstrap failed')
    require(result['processBits'] == 64 and result['actualDxvkCore'] is True, 'not actual x64 DXVK')
    require(result['bufferBytesEach'] == 4096 and result['checkedWords'] == 3072, 'readback coverage')
    require(all(result[k] == 3 for k in ('submitted', 'gpuTimelineCompleted', 'resourceTrackingRetired')),
            'distinct submission/GPU/retirement witnesses')
    require(result['observedWordSums'] == [v*1024 for v in (0xA55A13C7, 0x19E27B04, 0xDEAD0123)],
            'independent GPU buffer oracle')
    require(result['sourceMemoryFlags'] & 1 and result['readbackMemoryFlags'] & 6 == 6, 'buffer memory flags')
    require(result['normalOwnerTeardown'] is True, 'normal production owner teardown missing')
    for flag in ('gameTouched', 'd3d9Proxy', 'presentTested', 'warvkPassesMigrated',
                 'rendererMigrated', 'memorySavingsProven'):
        require(result[flag] is False, f'unsupported claim: {flag}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows x64 lab required')
    output = args.output.resolve()
    build = args.build.resolve()
    require(not build.exists(), 'fresh independent build required; never reconfigure a game build')
    require(build.is_relative_to(ROOT) and build != ROOT, 'build must be a new directory below this tree')
    output.mkdir(parents=False, exist_ok=False)
    receipt = {'schema': 1, 'scope': 'X64_ACTUAL_DXVK_CORE_BOOTSTRAP', 'ok': False,
               'gameLaunched': False, 'shippingDllBuilt': False, 'productIntegrated': False,
               'gameMemorySavingsProved': False, 'gpuValidationLayerTested': False,
               'watchdogTerminationIsFailure': True, 'steps': []}
    try:
        paths = source_paths()
        receipt['sources'] = [identity(p) for p in paths]
        meson = shutil.which('meson')
        ninja = shutil.which('ninja')
        require(meson and ninja, 'explicit local Meson/Ninja installation required')
        receipt['buildTools'] = [identity(Path(p)) for p in (meson, ninja)]
        options = ['-D'+key+'=false' for key in
                   ('enable_dxgi', 'enable_d3d8', 'enable_d3d9', 'enable_d3d10', 'enable_d3d11')]
        receipt['steps'].append(execute([meson, 'setup', build, ROOT, '--buildtype=release',
            '--cross-file', ROOT/'build-win64-render-host-llvm.txt',
            '-Dwarvk_render_host_core_dev=true', *options], ROOT, output/'configure', 180))
        target = Path('tools/render_host/dxvk_core/warvk-dxvk-core-host.exe')
        receipt['steps'].append(execute([ninja, '-C', build, '-j2', target.as_posix()],
                                       ROOT, output/'build', 1200))
        receipt['steps'].append(execute([ninja, '-C', build, '-n', target.as_posix()],
                                       ROOT, output/'no-work', 30))
        require('no work to do' in (output/'no-work.stdout').read_text(encoding='utf-8'), 'build still dirty')
        exe = build / target
        require(pe_identity(exe) == (0x8664, 0x20b), 'wrong host executable bitness')
        receipt['exe'] = identity(exe)
        receipt['allowedSharedD3D9Inputs'] = check_host_compile_boundary(build)
        receipt['compileCommands'] = identity(build/'compile_commands.json')
        env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('DXVK_', 'VK_'))}
        env.update(DXVK_LOG_PATH=str(output), DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0',
                   DXVK_CONFIG_FILE=str(output/'intentionally-absent-lab.conf'))
        receipt['runtimeOverrides'] = {k: v for k, v in env.items() if k.startswith('DXVK_')}
        receipt['steps'].append(execute([exe], output, output/'runtime', 60, env))
        result = strict_json((output/'runtime.stdout').read_text(encoding='utf-8'))
        check_runtime(result)
        receipt['runtime'] = result
        require(paths == source_paths() and receipt['sources'] == [identity(p) for p in paths],
                'core source changed during build/runtime')
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
