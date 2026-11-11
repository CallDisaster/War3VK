"""Compile five shared-boundary TUs with a preserved production x86 configuration.

The existing compile database is read, never regenerated. Only the source of
the new shared-helper TU is substituted, using the production DXSO options
TU's flags. Object/depfile/diagnostic outputs go to one fresh artifact directory.
No Ninja/Meson invocation, link, shipping DLL replacement, game or GPU run.
This is compilation evidence, not full-DLL link or gameplay acceptance.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import mesonbuild.mesonlib
from mesonbuild.mesonlib import split_args
from run_render_host_protocol_gate import ROOT, identity


DEFAULT_BUILD = ROOT / 'build32_weather_20261010'
# source, production command donor, production target directory
UNITS = (
    ('src/d3d9/d3d9_device.cpp', 'src/d3d9/d3d9_device.cpp', 'src/d3d9/d3d9.dll.p'),
    ('src/d3d9/d3d9_fixed_function.cpp', 'src/d3d9/d3d9_fixed_function.cpp', 'src/d3d9/d3d9.dll.p'),
    ('src/dxso/dxso_options.cpp', 'src/dxso/dxso_options.cpp', 'src/dxso/libdxso.a.p'),
    ('src/d3d9/d3d9_shader_helpers.cpp', 'src/dxso/dxso_options.cpp', 'src/dxso/libdxso.a.p'),
    ('src/dxvk/dxvk_context.cpp', 'src/dxvk/dxvk_context.cpp', 'src/dxvk/libdxvk.a.p'),
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def input_paths(build):
    """Freeze local source/header/config inputs, including generated headers.

This deliberately includes a superset of the five TUs' local dependencies.
External toolchain/SDK installations are not claimed to be fully frozen; the
compiler and parser identities are recorded separately. The depfiles preserve
the actual include lists for follow-up inspection.
"""
    suffixes = {'.cpp', '.c', '.h', '.hpp', '.hxx', '.in', '.inc', '.def',
                '.vert', '.frag', '.comp', '.glsl', '.wrap'}
    names = {'meson.build', 'meson_options.txt', 'CMakeLists.txt'}
    paths = set()
    for domain in ('src', 'include', 'subprojects', 'tools/render_host'):
        for path in (ROOT / domain).rglob('*'):
            if path.is_file() and (path.suffix in suffixes or path.name in names):
                paths.add(path.resolve())
    for path in build.rglob('*'):
        if path.is_file() and path.suffix in {'.h', '.hpp', '.hxx', '.inc'}:
            paths.add(path.resolve())
    for path in ROOT.glob('build-*.txt'):
        paths.add(path.resolve())
    for relative in ('meson.build', 'meson_options.txt', 'version.h', 'buildenv.h.in',
                     'AutoTest/run_d3d9_shared_shipping_compile.py',
                     'AutoTest/test_d3d9_shared_boundary.py',
                     'AutoTest/run_render_host_protocol_gate.py'):
        paths.add((ROOT / relative).resolve(strict=True))
    for relative in ('compile_commands.json', 'build.ninja', 'buildenv.h',
                     'meson-private/cmd_line.txt', 'meson-private/coredata.dat',
                     'meson-private/build.dat'):
        paths.add((build / relative).resolve(strict=True))
    return sorted(paths)


def select_command(entries, build, donor, target):
    matches = []
    for entry in entries:
        directory = Path(entry['directory']).resolve(strict=True)
        source = (directory / entry['file']).resolve()
        output = (directory / entry.get('output', '')).resolve()
        if source == donor and output.parent == (build / target).resolve():
            matches.append(entry)
    require(len(matches) == 1, f'exactly one production command required: {donor.name}')
    entry = matches[0]
    require(Path(entry['directory']).resolve() == build, 'unexpected compile working directory')
    argv = entry.get('arguments')
    if argv is None:
        argv = split_args(entry['command'])
    require(isinstance(argv, list) and argv and all(isinstance(a, str) for a in argv),
            'invalid compiler argument list')
    return entry, argv


def isolated_command(argv, build, donor, source, obj, depfile):
    """Preserve production semantic flags; redirect only compile I/O metadata."""
    require(argv.count('-c') == 1, 'compile-only command required')
    require(not any(a.startswith('@') for a in argv), 'unexpanded response file is not audited')
    require(not any('WARVK_JAPI_PROTOCOL_TEST' in a or 'WARVK_RENDER_HOST_TEST' in a for a in argv),
            'test-only definitions are forbidden')
    forbidden = ('-save-temps', '--save-temps', '-ftime-trace', '-fprofile',
                 '--coverage', '-coverage', '-MJ', '-serialize-diagnostics')
    require(not any(a.startswith(forbidden) for a in argv), 'extra-output compiler option is not audited')
    compiler_arg = Path(argv[0])
    if compiler_arg.is_absolute():
        compiler = compiler_arg.resolve(strict=True)
    else:
        located = shutil.which(argv[0])
        require(located is not None, 'compiler executable not found')
        compiler = Path(located).resolve(strict=True)
    require('i686' in compiler.name.lower() or '-m32' in argv
            or any(a.startswith(('--target=i686', '-target=i686')) for a in argv),
            'preserved command must explicitly select x86')
    filtered = [str(compiler)]
    index = 1
    source_count = 0
    output_count = 0
    while index < len(argv):
        item = argv[index]
        if item in ('-o', '-MF', '-MQ', '-MT'):
            require(index + 1 < len(argv), f'missing value for {item}')
            output_count += int(item == '-o')
            index += 2
            continue
        if item in ('-MD', '-MMD'):
            index += 1
            continue
        require(not any(item.startswith(prefix) and item != prefix
                        for prefix in ('-o', '-MF', '-MQ', '-MT')),
                f'joined output option is not audited: {item}')
        if item == '-c':
            require(index + 1 < len(argv) and (build / argv[index + 1]).resolve() == donor,
                    'production -c input does not match the selected TU')
            filtered.extend(('-c', str(source)))
            source_count += 1
            index += 2
            continue
        filtered.append(item)
        index += 1
    require(source_count == 1 and output_count == 1, 'unexpected compile input/output count')
    filtered.extend(('-MD', '-MQ', str(obj), '-MF', str(depfile), '-o', str(obj)))
    return filtered, compiler


def coff_machine(path):
    with path.open('rb') as stream:
        header = stream.read(20)
    require(len(header) == 20, 'missing COFF object header')
    first, second = struct.unpack_from('<HH', header)
    # Ordinary COFF and Microsoft's anonymous/bigobj COFF header layouts.
    return struct.unpack_from('<H', header, 6)[0] if (first, second) == (0, 0xffff) else first


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=600, help='per-TU compiler watchdog in seconds')
    args = parser.parse_args()
    require(os.name == 'nt', 'preserved Windows production configuration required')
    require(1 <= args.timeout <= 1800, 'bounded per-TU timeout required')
    build = args.build.resolve(strict=True)
    output = args.output.resolve()
    artifacts = (ROOT / 'AutoTest/artifacts').resolve(strict=True)
    require(output.is_relative_to(artifacts) and output != artifacts, 'fresh AutoTest artifact child required')
    require(not output.is_relative_to(build), 'outputs must not enter the preserved build')
    output.mkdir(parents=False, exist_ok=False)
    receipt = {
        'schema': 'warvk-shared-shipping-compile/v1', 'ok': False,
        'scope': 'FIVE_PRODUCTION_X86_TRANSLATION_UNITS_ONLY',
        'build': str(build), 'priority': 'BelowNormal', 'parallelCompilers': 1,
        'dllLinked': False, 'buildRegenerated': False, 'gameLaunched': False,
        'gpuTested': False, 'productIntegrated': False, 'memorySavingsProven': False,
        'fullToolchainHeadersFrozen': False, 'sourceDriftChecked': False,
        'preservedDllUnchanged': False, 'preservedObjectsUnchanged': False,
        'steps': [],
    }
    paths = []
    before = []
    preserved = []
    preserved_before = []
    try:
        dll = (build / 'src/d3d9/d3d9.dll').resolve(strict=True)
        receipt['preservedDll'] = identity(dll)
        entries = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
        require(isinstance(entries, list), 'compile database must be an array')
        plans = []
        compilers = set()
        for relative, donor_relative, target in UNITS:
            source = (ROOT / relative).resolve(strict=True)
            donor = (ROOT / donor_relative).resolve(strict=True)
            entry, original = select_command(entries, build, donor, target)
            obj = output / (source.stem + '.obj')
            depfile = output / (source.stem + '.d')
            command, compiler = isolated_command(original, build, donor, source, obj, depfile)
            compilers.add(compiler)
            preserved.append((build / entry['output']).resolve(strict=True))
            plans.append((source, donor, original, command, obj, depfile))
        preserved = sorted(set(preserved))
        preserved_before = [identity(path) for path in preserved]
        receipt['preservedObjects'] = preserved_before
        paths = input_paths(build)
        before = [identity(path) for path in paths]
        receipt['inputs'] = before
        tool_paths = sorted(compilers | {Path(sys.executable).resolve(strict=True),
                                        Path(mesonbuild.mesonlib.__file__).resolve(strict=True)})
        receipt['tools'] = [identity(path) for path in tool_paths]
        for source, donor, original, command, obj, depfile in plans:
            step = {'source': str(source), 'flagsFrom': str(donor),
                    'sourceSubstituted': source != donor, 'originalCommand': original,
                    'command': command, 'exit': None, 'timedOut': False}
            receipt['steps'].append(step)
            stdout = output / (source.stem + '.stdout')
            stderr = output / (source.stem + '.stderr')
            started = time.monotonic()
            try:
                flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
                with stdout.open('xb') as out, stderr.open('xb') as err:
                    result = subprocess.run(command, cwd=build, stdout=out, stderr=err,
                                            creationflags=flags, timeout=args.timeout)
                step['exit'] = result.returncode
                require(result.returncode == 0, f'{source.name}: compile exit {result.returncode}')
                require(coff_machine(obj) == 0x14c, f'{source.name}: output is not x86 COFF')
                require(depfile.is_file(), f'{source.name}: missing include dependency list')
                step['machine'] = 'I386'
                require(identity(dll) == receipt['preservedDll'], 'preserved reference DLL changed')
            except subprocess.TimeoutExpired:
                step['timedOut'] = True
                raise RuntimeError(f'{source.name}: compiler watchdog failure')
            finally:
                step['seconds'] = time.monotonic() - started
                step['outputs'] = [identity(path) for path in (obj, depfile, stdout, stderr) if path.is_file()]
        require([identity(path) for path in tool_paths] == receipt['tools'], 'compiler/parser changed during compile')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        # Run preservation checks even after compile failure/timeout. A changed
        # reference is recorded as failure; never silently restore/overwrite it.
        checks = {}
        try:
            if 'preservedDll' in receipt:
                receipt['preservedDllAfter'] = identity(Path(receipt['preservedDll']['path']))
                checks['preservedDllUnchanged'] = receipt['preservedDllAfter'] == receipt['preservedDll']
            if preserved_before:
                checks['preservedObjectsUnchanged'] = [identity(path) for path in preserved] == preserved_before
            if before:
                checks['sourceDriftChecked'] = paths == input_paths(build) and [identity(path) for path in paths] == before
            receipt.update(checks)
            require(checks and all(checks.values()) and all(receipt[name] for name in
                    ('preservedDllUnchanged', 'preservedObjectsUnchanged', 'sourceDriftChecked')),
                    'source/config/reference preservation was not fully verified')
        except Exception as exc:
            receipt['ok'] = False
            receipt['preservationError'] = str(exc)
        with (output / 'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, indent=2, ensure_ascii=False)
    print(json.dumps({'ok': receipt['ok'], 'receipt': str(output / 'receipt.json'),
                      'error': receipt.get('error'), 'preservationError': receipt.get('preservationError')},
                     ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
