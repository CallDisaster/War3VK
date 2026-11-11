"""Isolated, actual-production-object D3D9 device-owned resource acceptance.

The product is built separately. This runner borrows its production compile
flags and actual Ninja link command, changes only link output locations, and
adds a test-only scalar probe object. It never rebuilds the product, changes
its exports, deploys a DLL, launches Warcraft, or sends global input.

Successful GPU readback is not player-visual acceptance. A disabled production
losable-resource check is reported NOT_ENABLED, not a successful Reset refusal.
Watchdog termination is failure, never evidence of normal COM teardown.
"""

import argparse
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity


DEFAULT_BUILD = ROOT / 'build32_device_owner_20261022'
PROBE_EXPORT = 'WarVKTestDeviceOwned'
FIELD_COUNT = 39
PROBE_PLAN = ((0, 'primary'), (1, 'primary'), (2, 'primary'), (7, 'primary'),
              (3, 'primary'), (4, 'primary'), (6, 'primary'), (8, 'primary'),
              (8, 'primary'), (1, 'primary'), (0, 'secondary'), (5, 'secondary'),
              (8, 'secondary'), (0, 'primary'), (1, 'primary'), (9, 'secondary'),
              (0, 'primary'), (1, 'primary'), (9, 'primary'), (0, 'ended'))
STATIC_MODULES = (
    'test_device_owned_lifetime_contract', 'test_warvk_imgui_dx9_adapter',
    'test_render_stats_panel_static', 'test_frame_history_shortcut_static',
    'test_frame_recorder_console_static', 'test_frame_recorder_self_contained_static',
)
SUMMARY_COUNTS = dict(cycles=1, devicesCreated=2, devicesDestroyed=2,
                      probes=20, uiDraws=7, cursorUpdates=3, postFxRuns=2,
                      partialRetries=1, resets=2, ownerChecks=1,
                      heldDeviceChecks=1, postFxPixels=2048,
                      cursorTexturePixels=417, instanceBufferRuns=3, instanceArms=2,
                      instanceOtherOwnerChecks=1, instanceTlsIsolationChecks=1,
                      instanceBytesWritten=864, instanceTexturePixels=48)
SUMMARY_ZEROES = ('pixelFailures', 'cursorPixelFailures', 'deviceFinalRefs', 'factoryFinalRefs',
                  'instanceTextureFailures')
SUMMARY_TRUE = ('ok', 'moduleUnloaded', 'windowsClosed', 'normalTeardown')
SUMMARY_FALSE = ('gameLaunched', 'globalInput', 'allocatorFailureInjected', 'visualGameAcceptance',
                 'instanceDrawAcceptance')
SUMMARY_KEYS = (set(SUMMARY_COUNTS) | set(SUMMARY_ZEROES) | set(SUMMARY_TRUE)
                | set(SUMMARY_FALSE) | {'schema', 'stage', 'error', 'requestedCycles',
                                       'resetRejectChecks', 'resetRejectNotEnabled', 'actualGpuDraws'})


def require(condition, message):
    if not condition:
        raise ValueError(message)


def strict_json(text):
    def pairs_unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'duplicate JSON key: ' + key)
            result[key] = value
        return result

    def invalid_constant(value):
        raise ValueError('non-finite JSON value: ' + value)

    return json.loads(text, object_pairs_hook=pairs_unique, parse_constant=invalid_constant)


def native_uint(value, bits=64):
    return type(value) is int and 0 <= value < (1 << bits)


def expected_sequence(cycles):
    """Exact protocol interleaving, not just reassuring final totals."""
    result = [('stage', 'hidden-windows'), ('stage', 'load-companion-dll')]
    for _ in range(cycles):
        result += [('stage', 'create-primary'), ('probe', 0),
                   ('stage', 'ui-and-cursor-resources')]
        result += [('probe', op) for op in (1, 2, 7, 3)]
        result += [('stage', 'postfx-and-partial-retry'), ('probe', 4), ('probe', 6),
                   ('stage', 'instance-storage-before-reset'), ('probe', 8),
                   ('stage', 'reset-resource-contract'), ('reset', None),
                   ('stage', 'instance-storage-after-reset'), ('probe', 8), ('probe', 1),
                   ('stage', 'second-device-owner-isolation'), ('probe', 0), ('probe', 5),
                   ('probe', 8), ('probe', 0), ('probe', 1), ('probe', 9), ('probe', 0), ('probe', 1),
                   ('stage', 'external-resource-retains-device'),
                   ('stage', 'normal-device-destruction'), ('probe', 9), ('probe', 0)]
    return result + [('stage', 'normal-factory-destruction'), ('stage', 'normal-dll-unload'),
                     ('stage', 'normal-window-close'), ('stage', 'complete'), ('result', None)]


def check_probe(row, position, losable):
    require(type(row) is dict and row.keys() == {'op', 'hr', 'values'}, 'probe record keys')
    op, role = PROBE_PLAN[position]
    require(native_uint(row['op'], 32) and row['op'] == op, 'probe operation order')
    require(native_uint(row['hr'], 32) and row['hr'] == 0, 'probe failed HRESULT')
    v = row['values']
    require(type(v) is list and len(v) == FIELD_COUNT and all(native_uint(x) for x in v),
            '39 actual uint64 probe values required')
    require(v[0] == 1 and v[1] == op and v[38] == 0, 'probe ABI or reserved field')
    for index in (4, 5, 6, 13, 14, 15, 16, 17, 18, 27, 30, 31, 34, 35, 36, 37):
        require(v[index] in (0, 1), 'probe boolean is not 0/1')
    ended = role == 'ended'
    require(v[2] == v[3] == (0 if ended else 1), 'public device refs changed or leaked')
    if ended:
        require(all(x == 0 for x in v[4:]), 'UI/resource state survived device destruction')
        return v
    require(v[4] == v[6] == 1, 'active UI initialization/context missing')
    require(v[5] == int(role == 'primary'), 'two-device UI ownership mismatch')
    require(v[18] == losable, 'losable-resource configuration changed within cycle')
    require((v[10] == 0) == (v[11] == 0), 'live atlas count/pixels disagree')
    # The first frame after legal Reset may recreate the dynamic font atlas;
    # the intervening instance-buffer probe must not masquerade as a UI draw.
    if position not in (0, 8):
        require(v[10] > 0 and v[11] > 0, 'actual live atlas evidence missing')

    require(v[36] == int(op == 9), 'instance TLS arm/teardown/isolation evidence mismatch')
    used = {0, 1, 2, 3, 4, 5, 6, 10, 11, 18, 36}
    if op in (1, 2, 3, 7):
        used.update((7, 8, 9, 12, 13))
        require(all(v[i] > 0 for i in (7, 8, 9, 12)) and v[13] == 1,
                'actual UI geometry, DXVK draws, and GPU completion are required')
        if op in (2, 3, 7):
            used.update((19, 20, 23, 24, 25, 26))
            extent, color = {2: (8, 0xff80c040), 7: (8, 0xffe05060), 3: (17, 0xff40a0e0)}[op]
            require(v[19] == v[20] == extent and v[23] == 1 and v[24] == extent * extent
                    and v[25] == 0 and v[26] == color, 'actual cursor texture/draw payload')
    elif op in (4, 6):
        used.update((12, 13, 14, 15, 16, 21, 22))
        require(v[12] >= 12 and v[13] == v[14] == v[15] == 1,
                'actual PostFX initialization/draw/GPU completion missing')
        require(v[16] == int(op == 6), 'partial initialization failure/retry not exercised')
        require(v[21] == 1024 and v[22] == 0, 'PostFX 1024-pixel independent oracle failed')
    elif op == 5:
        used.add(17)
        require(v[17] == 1, 'non-owner cleanup changed the owner resources')
    elif op == 8:
        used.update(range(27, 38))
        require(v[27] == v[30] == v[31] == v[37] == 1 and v[28] == 288 and v[29] == 96,
                'actual device-owned instance allocation/binding/stable-owner evidence missing')
        require(v[32] == 16 and v[33] == 0, 'privately retained instance texture pixels failed')
        require(v[34] == v[35] == int(role == 'secondary'), 'cross-device instance/TLS isolation missing')
    require(all(v[i] == 0 for i in set(range(FIELD_COUNT)) - used),
            'unexpected evidence in fields unused by this operation')
    return v


def check_output(stdout, stderr, exit_code, cycles):
    """Pure strict reader for the actual C++ fixture; does not launch anything."""
    require(native_uint(cycles, 32) and 1 <= cycles <= 8, 'cycles must be 1..8')
    require(type(exit_code) is int and exit_code == 0, 'fixture did not exit normally with zero')
    require(type(stdout) is str and type(stderr) is str, 'text fixture output required')
    require('DEVICE_OWNER_' not in stderr, 'probe/fixture failure or misplaced protocol on stderr')
    rows, order = [], []
    for line in stdout.splitlines():
        if not line.startswith('DEVICE_OWNER_'):
            continue  # Native DLL/driver diagnostics remain preserved in the raw log.
        if line.startswith('DEVICE_OWNER_STAGE stage='):
            stage = line[len('DEVICE_OWNER_STAGE stage='):]
            require(re.fullmatch('[a-z-]+', stage) is not None, 'invalid stage record')
            order.append(('stage', stage))
            continue
        matches = [(kind, prefix) for kind, prefix in (
            ('probe', 'DEVICE_OWNER_PROBE '), ('reset', 'DEVICE_OWNER_RESET '),
            ('result', 'DEVICE_OWNER_RESULT ')) if line.startswith(prefix)]
        require(len(matches) == 1, 'unknown or failed device-owner protocol record')
        kind, prefix = matches[0]
        row = strict_json(line[len(prefix):])
        require(type(row) is dict, 'native record must be an object')
        order.append((kind, row.get('op') if kind == 'probe' else None))
        rows.append((kind, row))
    require(order == expected_sequence(cycles), 'missing/reordered/duplicate native protocol records')
    probes = [row for kind, row in rows if kind == 'probe']
    resets = [row for kind, row in rows if kind == 'reset']
    summaries = [row for kind, row in rows if kind == 'result']
    require(len(probes) == 20 * cycles and len(resets) == cycles and len(summaries) == 1,
            'actual cycle population mismatch')
    totals = dict(actualGpuDraws=0, postFxPixels=0, pixelFailures=0,
                  cursorTexturePixels=0, cursorPixelFailures=0, instanceBytesWritten=0,
                  instanceTexturePixels=0, instanceTextureFailures=0)
    reset_checks = 0
    for cycle in range(cycles):
        group = probes[cycle * 20:(cycle + 1) * 20]
        initial = group[0].get('values')
        require(type(initial) is list and len(initial) == FIELD_COUNT
                and native_uint(initial[18]) and initial[18] in (0, 1), 'initial Reset configuration')
        losable = initial[18]
        for position, row in enumerate(group):
            values = check_probe(row, position, losable)
            for key, index in (('actualGpuDraws', 12), ('postFxPixels', 21), ('pixelFailures', 22),
                               ('cursorTexturePixels', 24), ('cursorPixelFailures', 25),
                               ('instanceBytesWritten', 28), ('instanceTexturePixels', 32),
                               ('instanceTextureFailures', 33)):
                totals[key] += values[index]
        reset = resets[cycle]
        require(reset.keys() == {'enabled', 'hr'} and type(reset['enabled']) is bool
                and reset['enabled'] == bool(losable) and native_uint(reset['hr'], 32),
                'Reset event/configuration mismatch')
        require(reset['hr'] == (0x8876086c if losable else 0), 'wrong Reset contract HRESULT')
        reset_checks += losable

    summary = summaries[0]
    require(summary.keys() == SUMMARY_KEYS, 'summary schema keys changed')
    for key, multiplier in SUMMARY_COUNTS.items():
        require(native_uint(summary[key]) and summary[key] == multiplier * cycles,
                'summary population mismatch: ' + key)
    for key in SUMMARY_ZEROES:
        require(native_uint(summary[key]) and summary[key] == 0, 'summary failure/ref leak: ' + key)
    for key in SUMMARY_TRUE:
        require(summary[key] is True, 'normal completion evidence missing: ' + key)
    for key in SUMMARY_FALSE:
        require(summary[key] is False, 'unsupported acceptance claim: ' + key)
    for key, value in dict(schema=1, requestedCycles=cycles, resetRejectChecks=reset_checks,
                           resetRejectNotEnabled=cycles - reset_checks).items():
        require(native_uint(summary[key], 32) and summary[key] == value, 'summary contract: ' + key)
    require(summary['stage'] == 'complete' and summary['error'] == '', 'fixture incomplete')
    require(all(native_uint(summary[key]) and summary[key] == value for key, value in totals.items()),
            'summary does not equal the actual probe record totals')
    require(totals['actualGpuDraws'] >= 31 * cycles, 'actual GPU draw population too small')
    return dict(summary=summary, probeRecords=len(probes), resetRecords=len(resets),
                resetResourceRejection=('PROVEN' if reset_checks == cycles else
                                        'NOT_ENABLED' if reset_checks == 0 else 'PARTIAL'),
                publicDeviceReferences='UNCHANGED_BY_INTERNAL_RESOURCES',
                gpuReadback='POSTFX_UNIFORM_IMAGE_ORACLE',
                cursorEvidence='ACTUAL_BOUND_MANAGED_TEXTURE_PIXELS',
                instanceEvidence='ACTUAL_ALLOCATION_BIND_RETAINED_TEXTURE_AND_OWNER_TLS',
                instanceDrawAcceptance=False, normalTeardown=True, gameVisualAcceptance=False)


def pe_exports(path):
    """Read named and ordinal-only exports without loading either DLL."""
    data = path.read_bytes()

    def unpack(fmt, offset):
        require(0 <= offset and offset + struct.calcsize(fmt) <= len(data), 'truncated PE export data')
        return struct.unpack_from(fmt, data, offset)

    require(data[:2] == b'MZ', 'missing DOS header')
    pe = unpack('<I', 60)[0]
    require(data[pe:pe + 4] == b'PE\0\0', 'missing PE signature')
    machine, count, _, _, _, optional_size, _ = unpack('<HHIIIHH', pe + 4)
    optional = pe + 24
    require(machine == 0x14c and unpack('<H', optional)[0] == 0x10b, 'expected x86 PE32 export table')
    require(optional_size >= 104 and unpack('<I', optional + 92)[0] >= 1, 'missing export directory')
    export_rva, export_size = unpack('<II', optional + 96)
    headers_size = unpack('<I', optional + 60)[0]
    sections = []
    for i in range(count):
        offset = optional + optional_size + i * 40
        virtual_size, virtual, raw_size, raw = unpack('<IIII', offset + 8)
        sections.append((virtual, max(virtual_size, raw_size), raw, raw_size))

    def mapped(rva, length=1):
        if rva < headers_size and rva + length <= min(headers_size, len(data)):
            return rva
        for virtual, size, raw, raw_size in sections:
            if virtual <= rva < virtual + size:
                delta = rva - virtual
                require(delta + length <= raw_size and raw + delta + length <= len(data),
                        'export RVA outside backed section')
                return raw + delta
        raise ValueError('unmapped export RVA')

    def string_at(rva):
        start = mapped(rva)
        end = data.find(b'\0', start, min(len(data), start + 4096))
        require(end >= start, 'unterminated export name')
        mapped(rva, end - start + 1)
        return data[start:end].decode('ascii', 'strict')

    require(export_rva and export_size >= 40, 'missing exports')
    directory = unpack('<IIHHIIIIIII', mapped(export_rva, 40))
    base, functions_count, names_count, functions_rva, names_rva, ordinals_rva = directory[5:]
    require(0 < functions_count <= 65536 and names_count <= functions_count, 'invalid export population')
    functions = unpack('<' + 'I' * functions_count, mapped(functions_rva, 4 * functions_count))
    names = unpack('<' + 'I' * names_count, mapped(names_rva, 4 * names_count)) if names_count else ()
    ordinals = unpack('<' + 'H' * names_count, mapped(ordinals_rva, 2 * names_count)) if names_count else ()
    entries = {base + i: dict(ordinal=base + i, names=[], forwarder=(
        string_at(rva) if export_rva <= rva < export_rva + export_size else None))
        for i, rva in enumerate(functions) if rva}
    all_names = set()
    for name_rva, index in zip(names, ordinals):
        name = string_at(name_rva)
        require(index < functions_count and base + index in entries and name not in all_names,
                'invalid/duplicate named export')
        all_names.add(name)
        entries[base + index]['names'].append(name)
    for item in entries.values():
        item['names'].sort()
    return [entries[key] for key in sorted(entries)]


def check_exports(product, diagnostic):
    original = {entry['ordinal']: entry for entry in product}
    result = {entry['ordinal']: entry for entry in diagnostic}
    require(not any(PROBE_EXPORT in e['names'] for e in product), 'probe leaked into product DLL')
    require(all(result.get(ordinal) == entry for ordinal, entry in original.items()),
            'diagnostic DLL changed existing named/ordinal/forwarded exports')
    extra = [entry for ordinal, entry in result.items() if ordinal not in original]
    require(len(extra) == 1 and extra[0]['names'] == [PROBE_EXPORT] and extra[0]['forwarder'] is None,
            'diagnostic DLL must add exactly the scalar probe export')


def link_command_output(argv, build):
    if argv.count('-o') != 1:
        return None
    index = argv.index('-o')
    return (build / argv[index + 1]).resolve() if index + 1 < len(argv) else None


def isolated_link_command(argv, build, product, diagnostic, probe):
    """Keep the actual product link inputs and flags, including the .def file."""
    require('-shared' in argv and '-c' not in argv and not any(a.startswith('@') for a in argv),
            'expanded standalone shared-library link command required')
    require(link_command_output(argv, build) == product, 'link command has wrong product output')
    forbidden = ('-save-temps', '--save-temps', '-ftime-trace', '-fprofile', '--coverage',
                 '-Wl,-Map', '-Wl,--Map', '-Wl,--output-def', '-Wl,--dependency-file')
    require(not any(a.startswith(forbidden) for a in argv), 'unaudited extra-output linker flag')
    require(not any(a in ('&&', '||', '|', '>', '<', ';') for a in argv), 'shell link command forbidden')
    changed = list(argv)
    changed[changed.index('-o') + 1] = str(diagnostic)
    import_options = [i for i, a in enumerate(argv) if a.startswith('-Wl,--out-implib=')]
    require(len(import_options) == 1, 'exactly one import library output required')
    index = import_options[0]
    require((build / argv[index].split('=', 1)[1]).resolve() == Path(str(product) + '.a'),
            'unexpected original import library output')
    changed[index] = '-Wl,--out-implib=' + str(diagnostic) + '.a'
    require(sum(a != b for a, b in zip(argv, changed)) == 2, 'unexpected product link mutation')
    return changed + [str(probe)]


def query_inputs(text, build, product):
    lines = text.splitlines()
    require(lines and (build / lines[0].removesuffix(':')).resolve() == product,
            'Ninja query target mismatch')
    require(len(lines) >= 2 and lines[1].startswith('  input: '), 'Ninja query input section missing')
    rule = lines[1][len('  input: '):]
    require(rule in ('cpp_LINKER', 'cpp_LINKER_RSP'), 'unrecognized product link rule')
    inputs = []
    for line in lines[2:]:
        if not line.startswith('    '):
            break
        value = line[4:]
        require(not value.startswith('|| '), 'order-only product link dependency needs explicit audit')
        if value.startswith('| '):
            value = value[2:]
        inputs.append((build / value).resolve(strict=True))
    require(inputs and len(inputs) == len(set(inputs)), 'empty/duplicate final link inputs')
    require(all(p.suffix.lower() in ('.obj', '.o', '.a', '.lib', '.def') for p in inputs),
            'unrecognized final link input')
    return rule, sorted(inputs)


def observe(argv, cwd, prefix, timeout, env, steps):
    """One serial tool, BelowNormal. Kill only its own handle on timeout."""
    argv = list(map(str, argv))
    step = dict(command=argv, cwd=str(cwd), timeoutSeconds=timeout, exit=None,
                timedOut=False, forcedTermination=False, settled=False, priority='BelowNormal')
    steps.append(step)
    started = time.monotonic()
    stdout, stderr = prefix.with_suffix('.stdout'), prefix.with_suffix('.stderr')
    try:
        flags = subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS
        with stdout.open('xb') as out, stderr.open('xb') as err:
            process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=err, creationflags=flags)
            step['ownedPid'] = process.pid  # Never used to find or kill another process.
            try:
                step['exit'] = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                step['timedOut'] = True
                step['forcedTermination'] = True
                process.kill()
                step['exit'] = process.wait(timeout=10)
            except BaseException:
                if process.poll() is None:
                    step['forcedTermination'] = True
                    process.kill()
                    step['exit'] = process.wait(timeout=10)
                raise
            finally:
                step['settled'] = process.poll() is not None
    finally:
        step['seconds'] = time.monotonic() - started
        step['logs'] = [identity(p) for p in (stdout, stderr) if p.is_file()]
    return step


def succeeded(step):
    require(step['exit'] == 0 and step['settled'] and not step['timedOut'] and not step['forcedTermination'],
            'tool failure/timeout: ' + step['command'][0])


def source_paths(build):
    # Share the product build's authoritative local-input scope, then add all
    # generated C/C++ headers/sources, metadata, and this acceptance chain.
    from run_device_owned_build import inputs
    paths = set(inputs())
    for path in build.rglob('*'):
        if path.is_file() and path.suffix.lower() in ('.h', '.hpp', '.hxx', '.inc', '.cpp', '.c'):
            paths.add(path.resolve())
    for name in ('compile_commands.json', 'build.ninja', 'meson-private/cmd_line.txt',
                 'meson-private/coredata.dat', 'meson-private/build.dat', '.ninja_log', '.ninja_deps'):
        path = build / name
        if path.is_file():
            paths.add(path.resolve())
    for name in ('test_device_owned_lifetime_probe.cpp', 'test_device_owned_lifetime_parent.cpp',
                 'run_device_owned_lifetime_gate.py', 'run_d3d9_shared_shipping_compile.py',
                 'run_render_host_protocol_gate.py', 'run_render_host_core_gate.py'):
        paths.add((ROOT / 'AutoTest' / name).resolve(strict=True))
    paths.update((ROOT / 'AutoTest' / (name + '.py')).resolve(strict=True) for name in STATIC_MODULES)
    optional_test = ROOT / 'AutoTest/test_device_owned_lifetime_gate.py'
    if optional_test.is_file():
        paths.add(optional_test.resolve())
    return sorted(paths)


def verify_product_receipt(path, build, product):
    from run_device_owned_build import inputs
    receipt = strict_json(path.read_text('utf-8'))
    require(type(receipt) is dict and receipt.get('schema') == 'warvk-device-owned-build/v1'
            and receipt.get('ok') is True and receipt.get('preservedUnchanged') is True,
            'successful product build/preservation receipt required')
    require(receipt.get('dll') == identity(product), 'product DLL differs from successful build receipt')
    require(receipt.get('sources') == [identity(p) for p in inputs()], 'product source freeze has drifted')
    require(receipt.get('compileCommands') == identity(build / 'compile_commands.json'),
            'product compile database has drifted')
    require(receipt.get('generatedBackend') == identity(build / 'src/d3d9/war3_imgui_dx9_backend.cpp'),
            'generated production UI adapter has drifted')


def preservation_check(before):
    changes = []
    for old in before:
        try:
            after = identity(Path(old['path']))
            if after != old:
                changes.append(dict(before=old, after=after))
        except Exception as exc:
            changes.append(dict(before=old, error=str(exc)))
    return changes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=DEFAULT_BUILD)
    parser.add_argument('--build-receipt', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cycles', type=int, default=3, choices=range(1, 9))
    parser.add_argument('--tool-timeout', type=int, default=600)
    parser.add_argument('--fixture-timeout', type=int, default=240)
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows production configuration and hidden native fixture required')
    require(1 <= args.tool_timeout <= 1800 and 1 <= args.fixture_timeout <= 900, 'bounded watchdogs required')
    build, output = args.build.resolve(strict=True), args.output.resolve()
    require(build.parent == ROOT and build.name.startswith('build32_device_owner_'), 'dedicated product build required')
    require(output.parent == (ROOT / 'AutoTest/artifacts').resolve(strict=True), 'fresh direct artifact child required')
    build_receipt = args.build_receipt.resolve(strict=True)
    output.mkdir(exist_ok=False)
    receipt = dict(schema='warvk-device-owned-lifetime/v1', ok=False,
                   scope='ISOLATED_X86_PRODUCTION_OBJECTS_AND_GPU_READBACK',
                   build=str(build), productBuildReceipt=identity(build_receipt),
                   priority='BelowNormal', parallelTools=1, steps=[],
                   productDllBuilt=False, productExportsChanged=False, buildRegenerated=False,
                   gameLaunched=False, globalInput=False, deployed=False, releaseReady=False,
                   visualGameAcceptance=False, allocatorFailureInjected=False,
                   instanceDrawAcceptance=False,
                   memorySavingsProven=False, fullToolchainHeadersFrozen=False,
                   productSourceFreezeVerified=False, preservationVerified=False)
    tracked, source_before = [], []
    preserved_artifacts = []
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith(('DXVK_', 'VK_'))}
    receipt['removedEnvironmentKeys'] = sorted(set(os.environ) - set(env))
    try:
        import mesonbuild.mesonlib
        from mesonbuild.mesonlib import split_args
        from run_d3d9_shared_shipping_compile import select_command, isolated_command, coff_machine
        product = (build / 'src/d3d9/d3d9.dll').resolve(strict=True)
        original_import = Path(str(product) + '.a').resolve(strict=True)
        require(pe_identity(product) == (0x14c, 0x10b), 'product must be x86 PE32')
        verify_product_receipt(build_receipt, build, product)
        receipt['productSourceFreezeVerified'] = True
        paths = source_paths(build)
        source_before = paths
        tracked = [identity(p) for p in paths]
        receipt['localInputIdentities'] = tracked.copy()
        preserved = [product, original_import, build_receipt]
        for historical in (ROOT / 'build32_weather_20261010/src/d3d9/d3d9.dll',
                           Path('E:/Work/Warcraft III/d3d9.dll')):
            if historical.is_file():
                preserved.append(historical.resolve())
        preserved_artifacts = [identity(p) for p in preserved]
        receipt['preservedArtifacts'] = preserved_artifacts
        tracked += preserved_artifacts
        entries = strict_json((build / 'compile_commands.json').read_text('utf-8'))
        require(type(entries) is list, 'compile database array required')
        require(not any('subprojects/StormBreaker' in str(e).replace('\\', '/') for e in entries),
                'dirty StormBreaker checkout is not an accepted product dependency')
        donor = ROOT / 'src/d3d9/d3d9_device.cpp'
        entry, original_compile = select_command(entries, build, donor, 'src/d3d9/d3d9.dll.p')
        obj, depfile = output / 'device-owner-probe.obj', output / 'device-owner-probe.d'
        compile_command, compiler = isolated_command(original_compile, build, donor,
            ROOT / 'AutoTest/test_device_owned_lifetime_probe.cpp', obj, depfile)
        # The production TU uses backend headers indirectly. This probe also
        # inspects the public ImGui draw/texture API, so needs its public include.
        compile_command.append('-I' + str(ROOT / 'subprojects/imgui'))
        located_ninja = shutil.which('ninja', path=env.get('PATH'))
        require(located_ninja, 'existing Ninja executable required')
        ninja = Path(located_ninja).resolve(strict=True)
        tools = {compiler, ninja, Path(sys.executable).resolve(strict=True),
                 Path(mesonbuild.mesonlib.__file__).resolve(strict=True),
                 Path(inspect.getsourcefile(split_args)).resolve(strict=True)}
        for name in ('clang-22.exe', 'clang.exe', 'clang-target-wrapper.exe', 'ld.lld.exe'):
            sibling = compiler.parent / name
            if sibling.is_file():
                tools.add(sibling.resolve())
        tools_before = [identity(p) for p in sorted(tools)]
        receipt['toolIdentities'] = tools_before
        tracked += tools_before
        # Driver-added CRT/C++ libraries are not printed as explicit -l inputs
        # by Meson. Freeze the configured target's library/startup-file superset.
        runtime_paths = set()
        target_lib = compiler.parent.parent / 'i686-w64-mingw32/lib'
        require(target_lib.is_dir(), 'audited llvm-mingw x86 target library directory required')
        for p in target_lib.rglob('*'):
            if p.is_file() and p.suffix.lower() in ('.a', '.o', '.obj', '.lib'):
                runtime_paths.add(p.resolve())
        for p in (compiler.parent.parent / 'lib/clang').glob('*/lib/windows/*i386*'):
            if p.is_file() and p.suffix.lower() in ('.a', '.o', '.lib'):
                runtime_paths.add(p.resolve())
        require(runtime_paths, 'configured x86 runtime libraries unavailable')
        runtime_before = [identity(p) for p in sorted(runtime_paths)]
        receipt['toolchainLibraryIdentities'] = runtime_before
        tracked += runtime_before

        # -t commands/query/compdb are read-only; never invoke Ninja's build mode.
        succeeded(observe([ninja, '-t', 'commands', 'src/d3d9/d3d9.dll'], build,
                          output / 'ninja-commands', 60, env, receipt['steps']))
        succeeded(observe([ninja, '-t', 'query', 'src/d3d9/d3d9.dll'], build,
                          output / 'ninja-query', 60, env, receipt['steps']))
        rule, link_inputs = query_inputs((output / 'ninja-query.stdout').read_text('utf-8'), build, product)
        links_before = [identity(p) for p in link_inputs]
        receipt['originalLinkInputIdentities'] = links_before
        tracked += links_before
        candidates = []
        response = Path(str(product) + '.rsp')
        for line in (output / 'ninja-commands.stdout').read_text('utf-8').splitlines():
            if 'd3d9.dll' not in line:
                continue
            command = split_args(line)
            if link_command_output(command, build) == product or (
                    len(command) == 2 and command[1].startswith('@')
                    and (build / command[1][1:]).resolve() == response):
                candidates.append(command)
        require(len(candidates) == 1, 'exactly one actual final product link command required')
        original_link = candidates[0]
        receipt['ninjaProductLinkCommand'] = original_link
        if any(a.startswith('@') for a in original_link):
            require(rule == 'cpp_LINKER_RSP' and len(original_link) == 2, 'unaudited response-file link shape')
            # Ninja may remove its .rsp after linking. Ask Ninja to expand the
            # same generated edge; never reconstruct its dependency list.
            succeeded(observe([ninja, '-t', 'compdb', '-x', rule], build,
                              output / 'ninja-expanded-link', 60, env, receipt['steps']))
            expanded = strict_json((output / 'ninja-expanded-link.stdout').read_text('utf-8'))
            require(type(expanded) is list, 'expanded Ninja compilation database required')
            matches = []
            for record in expanded:
                command = split_args(record['command'])
                if link_command_output(command, build) == product:
                    require(Path(record['directory']).resolve() == build, 'expanded link directory mismatch')
                    matches.append(command)
            require(len(matches) == 1 and matches[0][0] == original_link[0], 'Ninja link expansion mismatch')
            link = matches[0]
        else:
            link = original_link
        require(Path(link[0]).resolve() == compiler, 'production compile/link compiler identity mismatch')
        explicit_inputs = {(build / item).resolve(strict=True) for item in link[1:]
                           if not item.startswith('-') and Path(item).suffix.lower() in ('.obj', '.o', '.a', '.lib', '.def')}
        require(explicit_inputs and explicit_inputs.issubset(set(link_inputs))
                and (build / entry['output']).resolve() in explicit_inputs,
                'actual link command inputs differ from the generated product edge')
        diagnostic_dir = output / 'diagnostic'
        diagnostic_dir.mkdir()
        diagnostic = diagnostic_dir / 'd3d9.dll'
        isolated_link = isolated_link_command(link, build, product, diagnostic, obj)
        receipt['expandedProductLinkCommand'] = link
        receipt['diagnosticLinkCommand'] = isolated_link
        receipt['probeCompileFlagsFrom'] = entry
        receipt['probeCompileCommand'] = compile_command
        product_exports = pe_exports(product)
        require(not any(PROBE_EXPORT in item['names'] for item in product_exports),
                'test-only export already present in product')
        receipt['productExports'] = product_exports

        succeeded(observe([sys.executable, '-B', '-m', 'unittest', '-v', *STATIC_MODULES],
                          ROOT / 'AutoTest', output / 'static-regressions', 180, env, receipt['steps']))
        succeeded(observe(compile_command, build, output / 'compile-probe', args.tool_timeout, env, receipt['steps']))
        require(coff_machine(obj) == 0x14c and depfile.is_file(), 'x86 probe COFF/dependency evidence missing')
        # Clang response files avoid Windows' command-line length limit. Forward
        # slashes remove Windows-backslash escaping ambiguities in quoted paths.
        response_text = '\n'.join('"' + a.replace('\\', '/').replace('"', '\\"') + '"'
                                  for a in isolated_link[1:]) + '\n'
        response_path = output / 'diagnostic-link.rsp'
        response_path.write_text(response_text, encoding='utf-8', newline='\n')
        succeeded(observe([compiler, '@' + str(response_path)], build, output / 'link-diagnostic',
                          args.tool_timeout, env, receipt['steps']))
        require(pe_identity(diagnostic) == (0x14c, 0x10b), 'diagnostic must be x86 PE32')
        diagnostic_exports = pe_exports(diagnostic)
        check_exports(product_exports, diagnostic_exports)
        receipt['diagnosticExports'] = diagnostic_exports
        receipt['diagnosticDll'] = identity(diagnostic)
        parent = output / 'device-owned-lifetime-parent.exe'
        parent_command = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-static',
                          '-static-libgcc', '-static-libstdc++', '-municode',
                          ROOT / 'AutoTest/test_device_owned_lifetime_parent.cpp', '-o', parent, '-luser32']
        succeeded(observe(parent_command, output, output / 'compile-parent', args.tool_timeout, env, receipt['steps']))
        require(pe_identity(parent) == (0x14c, 0x10b), 'parent fixture must be x86 PE32')
        receipt['parentExe'] = identity(parent)
        # Rehash before loading anything, then again in finally even on failure.
        require(not preservation_check(tracked), 'input/product drift before fixture')
        runtime = output / 'runtime'
        runtime.mkdir()
        config = runtime / 'dxvk.conf'
        config.write_text('# Isolated fixture: no production option overrides.\n', encoding='utf-8')
        local = env | dict(DXVK_CONFIG_FILE=str(config), DXVK_LOG_PATH=str(runtime),
                           DXVK_STATE_CACHE_PATH=str(runtime), DXVK_LOG_LEVEL='info')
        receipt['fixtureEnvironment'] = {k: v for k, v in local.items() if k.startswith('DXVK_')}
        step = observe([parent, '--dll', diagnostic, '--cycles', args.cycles], runtime,
                       output / 'fixture', args.fixture_timeout, local, receipt['steps'])
        # Preserve native failure records even when they cannot be certified.
        stdout = (output / 'fixture.stdout').read_text('utf-8', errors='replace')
        stderr = (output / 'fixture.stderr').read_text('utf-8', errors='replace')
        receipt['rawFinalRecords'] = [line for line in stdout.splitlines() if line.startswith('DEVICE_OWNER_RESULT ')]
        succeeded(step)
        receipt['acceptance'] = check_output(stdout, stderr, step['exit'], args.cycles)
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        # No best-effort restore: a changed product/reference is a failed gate.
        # Every captured input is rehashed even after compilation/GPU failure.
        try:
            changes = preservation_check(tracked)
            receipt['changedInputs'] = changes
            source_set_unchanged = bool(source_before) and source_before == source_paths(build)
            receipt['localInputSetUnchanged'] = source_set_unchanged
            receipt['preservationVerified'] = bool(tracked) and bool(preserved_artifacts) and not changes and source_set_unchanged
            require(receipt['preservationVerified'], 'input/product preservation not fully verified')
            verify_product_receipt(build_receipt, build, build / 'src/d3d9/d3d9.dll')
        except Exception as exc:
            receipt['ok'] = False
            receipt['preservationError'] = str(exc)
        receipt['artifacts'] = [identity(p) for p in sorted(output.rglob('*')) if p.is_file()]
        with (output / 'receipt.json').open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    print(json.dumps(dict(ok=receipt['ok'], receipt=str(output / 'receipt.json'),
                          error=receipt.get('error'), preservationError=receipt.get('preservationError')),
                     ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
