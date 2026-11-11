"""Measure the x86 data-layer cut of an existing shipping d3d9.dll build.

The x64 split keeps Game.dll hooks, game-memory readers and JASS natives in the
x86 process and moves GPU work to the host. This gate reads the object files of
a finished x86 build (llvm-nm only; no compile, game, desktop or deployment) and
reports two surfaces:

* forward: symbols the data layer still takes from the in-process D3D9 device,
  DXVK, effects or ImGui. Zero means the data layer links without them.
* reverse: symbols device/effects code takes from the data layer at runtime.
  These become the annotations and payloads sent to the host.

Each object is classified by an explicit rule; an unknown object fails the gate
so new files are placed deliberately. The data set starts from the data layer
and grows by every non-core object it needs, so the forward cut lists only the
edges where core code is reached. With --baseline the gate compares symbol sets,
and --ratchet fails when the forward cut gains a symbol.
"""
import argparse
import collections
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_render_host_protocol_gate import identity  # noqa: E402

SCHEMA = 'warvk-x86-data-layer-cut/v1'
LIBRARIES = {
    'src/d3d9/d3d9.dll.p': None,
    'src/dxvk/libdxvk.a.p': 'dxvk',
    'src/dxso/libdxso.a.p': 'dxvk',
    'src/wsi/libwsi.a.p': 'dxvk',
    'src/spirv/libspirv.a.p': 'dxvk',
    'subprojects/dxbc-spirv/libdxbc_spv.a.p': 'dxvk',
    'subprojects/libdisplay-info/libdisplay-info.a.p': 'dxvk',
    'src/util/libutil.a.p': 'util',
    'subprojects/imgui/libimgui.a.p': 'imgui',
    'src/d3d9/libwarvk_minhook.a.p': 'minhook',
}
DATA_DIRS = {'bridge', 'core', 'debug', 'diagnostics', 'game', 'handle', 'hooks', 'japi',
             'math', 'memory', 'model', 'native', 'platform', 'reimpl', 'semantic', 'state'}
MIXED_DIRS = {'render', 'gpu_skin', 'shadow', 'shader'}
CORE = frozenset({'device', 'effects', 'dxvk', 'imgui'})
PORTABLE = frozenset({'util', 'minhook'})
GROWABLE = frozenset({'mixed', 'tools', 'runtime', 'ui'})
BELOW_NORMAL = 0x00004000


def classify(library, name):
    """Layer of one object; None for a resource object; ValueError when unknown."""
    if library:
        return library
    if name == 'version':  # the version resource, version.o
        return None
    if name == 'meson-generated_.._war3_imgui_dx9_backend.cpp':
        return 'imgui'
    if name == 'd3d9_war3_hook.cpp':
        return 'data'
    if name.startswith('d3d9_war3_'):
        return 'effects'
    if name.startswith('d3d9_'):
        return 'device'
    if name.startswith('jass_'):
        return 'data'
    if name.startswith('war3_shaderpack'):
        return 'effects'
    if name == 'war3_shader_api.cpp':
        return 'mixed'
    if name == 'war3_war3.cpp':
        return 'runtime'
    if name == 'war3_war3_user_example.cpp':
        return 'data'
    if name.startswith('war3_'):
        sub = name[len('war3_'):].split('_', 1)[0]
        if sub in DATA_DIRS:
            return 'data'
        if sub in MIXED_DIRS or name.startswith('war3_gpu_skin_'):
            return 'mixed'
        if sub == 'tools':
            return 'tools'
        if sub == 'ui':
            return 'ui'
    raise ValueError(f'unclassified d3d9 object: {name}')


def run_nm(nm, path, *flags):
    result = subprocess.run([str(nm), '--format=just-symbols', *flags, str(path)],
        capture_output=True, text=True, timeout=120,
        creationflags=BELOW_NORMAL if os.name == 'nt' else 0)
    if result.returncode:
        raise RuntimeError(f'llvm-nm failed for {path}: {result.stderr.strip()}')
    return set(result.stdout.split())


def collect(build, nm):
    """{key: {'layer', 'defined', 'undefined'}} for every object of the DLL closure."""
    objects = {}
    for relative, library in LIBRARIES.items():
        directory = build/relative
        if not directory.is_dir():
            if library is None:
                raise ValueError(f'missing {directory}')
            continue
        for path in sorted(directory.iterdir()):
            if not path.is_file() or path.suffix not in ('.obj', '.o'):
                continue
            name = path.name[:-len(path.suffix)]
            layer = classify(library, name)
            if layer is None:
                continue
            key = f'{library or "d3d9"}:{name}'
            objects[key] = {'layer': layer,
                'defined': run_nm(nm, path, '--defined-only', '--extern-only'),
                'undefined': run_nm(nm, path, '--undefined-only')}
    return objects


def analyze(objects):
    """Forward cut and reverse surface over symbol tables (mangled names)."""
    definers = collections.defaultdict(list)
    for key in sorted(objects):
        for symbol in objects[key]['defined']:
            definers[symbol].append(key)
    data = {k for k, o in objects.items() if o['layer'] == 'data'}
    portable = {k for k, o in objects.items() if o['layer'] in PORTABLE}

    def satisfied(symbol, members):
        return any(d in members or d in portable for d in definers[symbol])

    grown = True
    while grown:
        grown = False
        for key in sorted(data):
            for symbol in sorted(objects[key]['undefined']):
                if symbol not in definers or satisfied(symbol, data):
                    continue
                pull = [d for d in definers[symbol] if objects[d]['layer'] in GROWABLE]
                if pull:
                    data.add(pull[0])
                    grown = True

    forward = collections.defaultdict(set)
    for key in sorted(data):
        for symbol in objects[key]['undefined']:
            if symbol in definers and not satisfied(symbol, data):
                forward[(key, definers[symbol][0])].add(symbol)

    reverse = collections.defaultdict(set)
    for key, info in objects.items():
        if info['layer'] not in ('device', 'effects'):
            continue
        for symbol in info['undefined']:
            ds = definers.get(symbol, [])
            if ds and all(d in data for d in ds):
                reverse[(key, ds[0])].add(symbol)
    return data, forward, reverse


def demangler(cxxfilt):
    def demangle(symbols):
        symbols = sorted(symbols)
        if not symbols:
            return {}
        result = subprocess.run([str(cxxfilt)], input='\n'.join(symbols),
            capture_output=True, text=True, timeout=120)
        lines = result.stdout.splitlines()
        if result.returncode or len(lines) != len(symbols):
            raise RuntimeError('llvm-cxxfilt output does not match its input')
        return dict(zip(symbols, lines))
    return demangle


def surface(edges, objects, demangle):
    names = demangle(set().union(*edges.values())) if edges else {}
    rows = []
    for (source, target), symbols in sorted(edges.items()):
        rows.append({'from': source, 'fromLayer': objects[source]['layer'],
            'to': target, 'toLayer': objects[target]['layer'],
            'symbols': sorted(symbols), 'demangled': [names[s] for s in sorted(symbols)]})
    return {'symbolCount': len(set().union(*edges.values())) if edges else 0,
            'edgeCount': len(edges), 'objects': sorted({r['from'] for r in rows}),
            'edges': rows}


def compare(current, baseline):
    """Added/removed forward and reverse symbols against an earlier receipt."""
    def symbols(receipt, side):
        return {s for edge in receipt[side]['edges'] for s in edge['symbols']}
    if baseline.get('schema') != SCHEMA:
        raise ValueError('baseline schema mismatch')
    out = {}
    for side in ('forward', 'reverse'):
        now, before = symbols(current, side), symbols(baseline, side)
        out[side] = {'added': sorted(now - before), 'removed': sorted(before - now),
                     'before': len(before), 'after': len(now)}
    return out


def toolchain(build):
    compilers = json.loads((build/'meson-info/intro-compilers.json').read_text('utf-8'))
    exe = Path(compilers['host']['cpp']['exelist'][0])
    nm, cxxfilt = exe.parent/'llvm-nm.exe', exe.parent/'llvm-cxxfilt.exe'
    if not nm.is_file() or not cxxfilt.is_file():
        raise ValueError(f'llvm-nm/llvm-cxxfilt not found beside {exe}')
    return nm, cxxfilt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--ratchet', action='store_true')
    args = parser.parse_args()
    build, out = args.build.resolve(), args.output.resolve()
    if build.parent != ROOT or not build.name.startswith('build32'):
        raise ValueError('x86 build root under the repository required')
    if out.parent != ROOT/'AutoTest/artifacts':
        raise ValueError('fresh evidence child required')
    if args.ratchet and not args.baseline:
        raise ValueError('--ratchet needs --baseline')
    out.mkdir(exist_ok=False)
    receipt = {'schema': SCHEMA, 'ok': False, 'error': None, 'compiled': False,
               'gameLaunched': False, 'deployed': False}
    try:
        dll = build/'src/d3d9/d3d9.dll'
        nm, cxxfilt = toolchain(build)
        receipt['dll'] = identity(dll)
        receipt['tools'] = [identity(nm), identity(cxxfilt)]
        options = json.loads((build/'meson-info/intro-buildoptions.json').read_text('utf-8'))
        receipt['options'] = {o['name']: o['value'] for o in options
                              if o['name'].startswith(('warvk_', 'enable_')) or o['name'] == 'buildtype'}
        objects = collect(build, nm)
        receipt['objectsByLayer'] = dict(sorted(collections.Counter(
            o['layer'] for o in objects.values()).items()))
        data, forward, reverse = analyze(objects)
        receipt['dataSet'] = {'count': len(data),
            'data': sorted(k for k in data if objects[k]['layer'] == 'data'),
            'pulledIn': sorted(k for k in data if objects[k]['layer'] != 'data')}
        demangle = demangler(cxxfilt)
        receipt['forward'] = surface(forward, objects, demangle)
        receipt['reverse'] = surface(reverse, objects, demangle)
        if args.baseline:
            baseline = json.loads(args.baseline.read_text('utf-8'))
            receipt['baseline'] = identity(args.baseline.resolve())
            receipt['delta'] = compare(receipt, baseline)
            if args.ratchet and receipt['delta']['forward']['added']:
                raise ValueError('forward cut gained symbols')
        receipt['ok'] = True
    except Exception as error:  # the receipt records every failure
        receipt['error'] = f'{type(error).__name__}: {error}'
    (out/'receipt.json').write_text(json.dumps(receipt, ensure_ascii=False, indent=1), 'utf-8')
    summary = {'ok': receipt['ok'], 'error': receipt['error']}
    for side in ('forward', 'reverse'):
        if side in receipt:
            summary[side] = {k: receipt[side][k] for k in ('symbolCount', 'edgeCount')}
    if 'delta' in receipt:
        summary['delta'] = {s: {k: v for k, v in d.items() if k in ('before', 'after')}
                            for s, d in receipt['delta'].items()}
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
