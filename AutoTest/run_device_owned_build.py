"""Fresh isolated x86 product build for the device-owned COM repair.

No player deployment or game execution. A rebuild uses an explicit existing
build but always a fresh evidence directory. Historical DLLs are hash guarded.
"""
import argparse
import json
import os
from pathlib import Path
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_core_gate import execute


def inputs():
    suffixes = {'.cpp', '.c', '.h', '.hpp', '.in', '.inc', '.def', '.vert',
                '.frag', '.comp', '.glsl', '.rc', '.txt'}
    names = {'meson.build', 'meson_options.txt'}
    result = set()
    # StormBreaker dirty checkout is not a build input. The production tree
    # already has its own integration source, which is included under src.
    for name in ('src', 'include', 'subprojects/imgui', 'subprojects/dxbc-spirv',
                 'subprojects/libdisplay-info', 'subprojects/war3fx', 'src/minhook'):
        for path in (ROOT / name).rglob('*'):
            if path.is_file() and '.git' not in path.parts and (
                    path.suffix in suffixes or path.name in names):
                result.add(path)
    for name in ('meson.build', 'meson_options.txt', 'version.h', 'buildenv.h.in',
                 'build-win32-weather-llvm.txt', 'AutoTest/prepare_warvk_imgui_dx9.py',
                 'AutoTest/test_warvk_imgui_dx9_adapter.py', 'AutoTest/run_device_owned_build.py'):
        result.add(ROOT / name)
    return sorted(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--reuse', action='store_true')
    parser.add_argument('--migration-census', action='store_true',
                        help='diagnostic local-game census; not a performance baseline')
    parser.add_argument('--mdl-allocation-trace', action='store_true',
                        help='bounded developer MDLDATA observer; no allocator policy changes')
    parser.add_argument('--startup-witness', action='store_true')
    args = parser.parse_args()
    build, output = args.build.resolve(), args.output.resolve()
    if build.parent != ROOT or not build.name.startswith('build32_device_owner_'):
        raise ValueError('dedicated build32_device_owner_* child required')
    if output.parent != ROOT / 'AutoTest/artifacts':
        raise ValueError('fresh artifact child required')
    if build.exists() != args.reuse:
        raise ValueError('fresh build required, or explicit --reuse of this dedicated build')
    output.mkdir(exist_ok=False)
    preserved = [ROOT / 'build32_weather_20261010/src/d3d9/d3d9.dll',
                 Path('E:/Work/Warcraft III/d3d9.dll')]
    before = [identity(p) for p in preserved]
    source = [identity(p) for p in inputs()]
    receipt = dict(schema='warvk-device-owned-build/v1', ok=False,
                   gameLaunched=False, deployed=False, releaseReady=False,
                   sources=source, preserved=before, steps=[])
    receipt['migrationCensus'] = args.migration_census
    receipt['mdlAllocationTrace'] = args.mdl_allocation_trace
    receipt['startupWitness'] = args.startup_witness
    env = os.environ.copy()
    env['PATH'] = ('D:/Environment/Python3.13.11/Scripts;'
                   'E:/Dev/Vulkan SDK/Bin;' + env['PATH'])
    try:
        if not (build/'build.ninja').is_file():
            receipt['steps'].append(execute([sys.executable, '-m', 'mesonbuild.mesonmain',
                'setup', build, ROOT, '--cross-file', ROOT / 'build-win32-weather-llvm.txt',
                '-Denable_d3d10=false', '-Denable_d3d11=false', '-Denable_dxgi=false',
                '-Denable_d3d9=true', '-Dbuildtype=release',
                '-Dwarvk_d3d9_migration_census_dev=' + str(args.migration_census).lower(),
                '-Dwarvk_mdl_allocation_trace_dev=' + str(args.mdl_allocation_trace).lower(),
                '-Dwarvk_startup_witness_dev=' + str(args.startup_witness).lower()],
                ROOT, output/'configure', 180, env))
        entries = json.loads((build/'compile_commands.json').read_text('utf-8'))
        device_entries = [e for e in entries if
                          Path(e['file']).name == 'd3d9_device.cpp']
        if not device_entries or any(
                ('-DWARVK_D3D9_MIGRATION_CENSUS_DEV=1' in e.get('command', ''))
                != args.migration_census for e in device_entries):
            raise ValueError('actual device compile flag differs from requested census mode')
        trace_entries = [e for e in entries if Path(e['file']).name == 'war3_mdl_trace.cpp']
        if not trace_entries or any(
                ('-DWARVK_MDLDATA_TRACE_DEV=1' in e.get('command', ''))
                != args.mdl_allocation_trace for e in trace_entries):
            raise ValueError('actual MDLDATA compile flag differs from requested mode')
        if any('subprojects/StormBreaker' in str(e).replace('\\', '/') for e in entries):
            raise ValueError('dirty StormBreaker input entered product build')
        startup_entries = [e for e in entries if Path(e['file']).name == 'war3_startup_witness.cpp'
                           and 'd3d9.dll.p' in e.get('command', '')]
        if not startup_entries or any(('-DWARVK_STARTUP_WITNESS_DEV=1' in e.get('command', ''))
                                      != args.startup_witness for e in startup_entries):
            raise ValueError('actual startup witness compile flag mismatch')
        receipt['steps'].append(execute(['ninja', '-C', build, '-j2', 'src/d3d9/d3d9.dll'],
            ROOT, output/'build', 1800, env))
        receipt['steps'].append(execute(['ninja', '-C', build, '-n', 'src/d3d9/d3d9.dll'],
            ROOT, output/'no-work', 60, env))
        if 'no work to do' not in (output/'no-work.stdout').read_text('utf-8'):
            raise ValueError('build did not settle')
        dll = build/'src/d3d9/d3d9.dll'
        if pe_identity(dll) != (0x14c, 0x10b):
            raise ValueError('expected x86 PE32')
        receipt['dll'] = identity(dll)
        receipt['generatedBackend'] = identity(build/'src/d3d9/war3_imgui_dx9_backend.cpp')
        receipt['compileCommands'] = identity(build/'compile_commands.json')
        if source != [identity(p) for p in inputs()]:
            raise ValueError('source drift during build')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        receipt['preservedAfter'] = [identity(p) for p in preserved]
        receipt['preservedUnchanged'] = before == receipt['preservedAfter']
        if not receipt['preservedUnchanged']:
            receipt['ok'] = False
            receipt['error'] = 'preserved DLL changed'
        (output/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: receipt[k] for k in ('ok', 'preservedUnchanged')} |
                     dict(receipt=str(output/'receipt.json'), error=receipt.get('error')), ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
