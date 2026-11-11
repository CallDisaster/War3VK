"""Fresh two-bitness build for native-game census and shared FF key compilation.

No game/desktop launch or deployment. Serial builds, BelowNormal, Ninja -j2.
This is a diagnostic local DLL plus CPU compiler, not a full remote renderer.
"""
import argparse
import json
import os
from pathlib import Path
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_core_gate import execute, source_paths, check_host_compile_boundary
from run_device_owned_build import inputs


def require(value, message):
    if not value:
        raise ValueError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build32', type=Path, required=True)
    parser.add_argument('--build64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    b32, b64, out = args.build32.resolve(), args.build64.resolve(), args.output.resolve()
    require(b32.parent == ROOT and b32.name.startswith('build32_device_owner_'), 'x86 build root')
    require(b64.parent == ROOT and b64.name.startswith('build64_render_host_'), 'x64 build root')
    require(not b32.exists() and not b64.exists(), 'fresh build roots required')
    require(out.parent == ROOT/'AutoTest/artifacts', 'fresh evidence child required')
    out.mkdir(exist_ok=False)
    paths = set(inputs()) | set(source_paths()) | {
        ROOT/'AutoTest/run_d3d9_game_input_build.py'}
    before = [identity(p) for p in sorted(paths)]
    protected = [identity(Path('E:/Work/Warcraft III/d3d9.dll')),
                 identity(ROOT/'build32_device_owner_shader_20261022/src/d3d9/d3d9.dll'),
                 identity(ROOT/'build64_render_host_surface_20261023/tools/render_host/dxvk_core/warvk-d3d9-host-dev.exe')]
    receipt = dict(schema='warvk-d3d9-game-input-build/v1', ok=False,
        diagnosticOnly=True, source=before, protectedBefore=protected,
        gameLaunched=False, deployed=False, remoteGameRenderer=False, steps=[])
    env = os.environ.copy()
    env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;' + env['PATH']
    try:
        receipt['steps'].append(execute([sys.executable, '-B',
            ROOT/'AutoTest/run_device_owned_build.py', '--build', b32,
            '--output', out.parent/(out.name+'-x86'), '--migration-census'],
            ROOT, out/'x86-build', 2100, env))
        x86_receipt = out.parent/(out.name+'-x86')/'receipt.json'
        receipt['x86BuildReceipt'] = identity(x86_receipt)
        require(json.loads(x86_receipt.read_text('utf-8'))['ok'] is True, 'x86 receipt failed')
        targets32 = ['src/d3d9/warvk-ff-key-compile.exe',
                     'src/d3d9/warvk-d3d9-migration-census-test.exe']
        receipt['steps'].append(execute(['ninja', '-C', b32, '-j2', *targets32],
            ROOT, out/'x86-tools', 600, env))
        receipt['steps'].append(execute([sys.executable, '-m', 'mesonbuild.mesonmain',
            'setup', b64, ROOT, '--cross-file', ROOT/'build-win64-render-host-llvm.txt',
            '-Denable_d3d8=false', '-Denable_d3d9=false', '-Denable_d3d10=false',
            '-Denable_d3d11=false', '-Denable_dxgi=false',
            '-Dwarvk_render_host_core_dev=true', '-Dbuildtype=release'],
            ROOT, out/'x64-configure', 180, env))
        targets64 = ['tools/render_host/dxvk_core/warvk-ff-key-compile.exe']
        receipt['steps'].append(execute(['ninja', '-C', b64, '-j2', *targets64],
            ROOT, out/'x64-tool', 1200, env))
        receipt['x64CompileBoundary'] = check_host_compile_boundary(b64)
        for name, build, targets in [('x86', b32, ['src/d3d9/d3d9.dll', *targets32]),
                                     ('x64', b64, targets64)]:
            receipt['steps'].append(execute(['ninja', '-C', build, '-n', *targets],
                ROOT, out/(name+'-no-work'), 30, env))
            require('no work to do' in (out/(name+'-no-work.stdout')).read_text('utf-8'), 'build not settled')
        products = [b32/'src/d3d9/d3d9.dll', *(b32/t for t in targets32), *(b64/t for t in targets64)]
        for i, product in enumerate(products):
            require(pe_identity(product) == ((0x14c, 0x10b) if i < 3 else (0x8664, 0x20b)), 'PE architecture mismatch')
        receipt['products'] = [identity(p) for p in products]
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = f'{type(error).__name__}: {error}'
    finally:
        receipt['sourceUnchanged'] = before == [identity(p) for p in sorted(paths)]
        receipt['protectedAfter'] = [identity(Path(p['path'])) for p in protected]
        receipt['protectedUnchanged'] = protected == receipt['protectedAfter']
        receipt['ok'] &= receipt['sourceUnchanged'] and receipt['protectedUnchanged']
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in
        ('ok','error','sourceUnchanged','protectedUnchanged')} | dict(receipt=str(out/'receipt.json'))))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
