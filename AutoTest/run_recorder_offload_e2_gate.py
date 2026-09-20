"""Real x86 -> x64 CPU laboratory. CreateNew outputs; no product/game/GPU."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke
from run_render_host_transport_gate import require, strict_json, child_settled
from run_render_host_shared_slots_gate import containers_gone

CASES = ('normal', 'small', 'slow', 'disconnect', 'missing-seal', 'late-data',
         'wrong-ordinal', 'wrong-session', 'wrong-scope', 'bad-kind', 'seal-totals',
         'p2-to-e2', 'p3-to-e2', 'e2-to-p2', 'e2-to-p3')


def run_client(command, directory, timeout):
    """Preserve raw output even for nonzero exit / watchdog failure."""
    with (directory / 'client.stdout').open('xb') as stdout, (directory / 'client.stderr').open('xb') as stderr:
        result = subprocess.run(list(map(str, command)), cwd=ROOT, stdout=stdout, stderr=stderr,
                                timeout=timeout, creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    require(result.returncode == 0, f'client exit {result.returncode}; preserved raw files in {directory}')
    return ((directory / 'client.stdout').read_text(encoding='utf-8').strip(),
            (directory / 'client.stderr').read_text(encoding='utf-8').strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compiler32', type=Path, required=True)
    parser.add_argument('--compiler64', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cases', nargs='+', choices=CASES, default=CASES)
    parser.add_argument('--build-only', choices=('client', 'all'))
    args = parser.parse_args()
    require(os.name == 'nt', 'Windows only')
    output = args.output.resolve(); output.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='RECORDER_OFFLOAD_E2_CPU_LAB', ok=False,
                   gameMemoryBenefitMeasured=False, gpuTested=False, gameLaunched=False,
                   shippingDllBuilt=False, productIntegration=False, builds=[], cases=[])
    common = ['protocol', 'win32_transport', 'win32_security', 'slot_ledger', 'slot_wire', 'shared_slots']
    specs = [
        ('client', 32, args.compiler32, ['owned_child', 'process_memory_probe', 'recorder_event_wire'], 'AutoTest/test_recorder_offload_worker.cpp'),
        ('host', 64, args.compiler64, ['slot_host_runtime', 'process_memory_probe', 'recorder_event_wire', 'recorder_history_store'], 'tools/render_host/recorder_host_main.cpp'),
        ('p2', 64, args.compiler64, ['slot_host_runtime'], 'tools/render_host/slot_host_main.cpp'),
        ('p3', 64, args.compiler64, ['slot_host_runtime', 'sample_envelope'], 'tools/render_host/sample_host_main.cpp')]
    if args.build_only == 'client':
        specs = specs[:1]
    source_paths = set(ROOT.glob('tools/render_host/*.[hc]*'))
    source_paths.add(ROOT / 'src/d3d9/war3/tools/war3_frame_evidence_core.h')
    source_paths |= {ROOT / 'AutoTest' / name for name in (
        'run_recorder_offload_e2_gate.py', 'test_recorder_offload_worker.cpp',
        'render_host_handle_probe.h',
        'run_render_host_protocol_gate.py', 'run_render_host_transport_gate.py', 'run_render_host_shared_slots_gate.py')}
    if not args.build_only:
        source_paths.add(ROOT / 'AutoTest/analyze_recorder_offload_e2.py')
    sources = sorted(p for p in source_paths if p.is_file())
    try:
        receipt['sources'] = [identity(p) for p in sources]
        binaries = {}
        for name, bits, compiler, extra, main_path in specs:
            compiler = compiler.resolve(strict=True); exe = output / f'e2-{name}-{bits}.exe'
            command = [compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread',
                       '-municode', '-D_WIN32_WINNT=0x0a00', '-DNOMINMAX',
                       *[f'tools/render_host/{n}.cpp' for n in common + extra], main_path,
                       '-lbcrypt', '-ladvapi32', '-lpsapi', '-o', exe]
            stdout, stderr = invoke(command, 120)
            require(not stderr and pe_identity(exe) == ((0x14c, 0x10b) if bits == 32 else (0x8664, 0x20b)), 'build/PE')
            binaries[name] = exe
            receipt['builds'].append(dict(name=name, bits=bits, compiler=identity(compiler), exe=identity(exe), stdout=stdout, stderr=stderr))
        if not args.build_only:
            from analyze_recorder_offload_e2 import analyze_case
            for index, mode in enumerate(args.cases):
                directory = output / f'{index:02d}-{mode}'; directory.mkdir(exist_ok=False)
                log = directory / 'host.ndjson'; start = time.monotonic()
                host_exe = binaries['p2' if mode == 'e2-to-p2' else 'p3' if mode == 'e2-to-p3' else 'host']
                stdout, stderr = run_client([binaries['client'], host_exe, mode, log], directory, 130 if mode == 'normal' else 65)
                require(not stderr, 'client stderr')
                rows = [strict_json(line) for line in stdout.splitlines()]
                require(len(rows) == 4 and all(r.get('initialization') is True for r in rows[:2]), 'parent row contract')
                env = rows[2]
                require(env.get('environmentWindow') is True and env['milliseconds'] == (55000 if mode == 'normal' else 0)
                        and env['frequency'] > 0 and env['endQpc'] >= env['startQpc']
                        and (env['endQpc'] - env['startQpc']) * 1000 >= env['milliseconds'] * env['frequency'], 'startup observation window')
                p = rows[-1]; require(p['mode'] == mode, 'actual mode')
                inits = []
                for number, row in enumerate(rows[:2]):
                    require(row['exit'] == (2 if mode == 'e2-to-p2' else 3), 'init exit')
                    inits.append(dict(identity=identity(Path(str(log) + f'.init-{number}')),
                                      settlement=child_settled(row['pid'], row['creation'])))
                host_rows = [strict_json(line) for line in log.read_text(encoding='utf-8').splitlines()]
                transports = [r for r in host_rows if r.get('summary') is True]
                hosts = [r for r in host_rows if r.get('recorderHost') is True]
                require(len(transports) == 1 and len(hosts) == (0 if mode.startswith('e2-to-') else 1), 'host terminal rows')
                t = transports[0]; h = hosts[0] if hosts else None
                verdict = analyze_case(p, h, t)
                # Actual host receipts must match each control-packet ordinal,
                # not merely a trusting aggregate. Independent digest oracle is
                # provided by the analyzer; no C++ parsing or native oracle.
                samples = [r for r in host_rows if r.get('sample') is True]
                require(len(samples) == p['ackPackets'], 'actual ACK sample count')
                good = mode in ('normal', 'small', 'slow')
                mismatch = mode in ('p2-to-e2', 'p3-to-e2', 'e2-to-p2', 'e2-to-p3')
                expected_controls = (4 + 2 * p['ackPackets'] if good else 0 if mismatch else
                                     2 + 2 * p['ackPackets'] if mode == 'missing-seal' else 3 + 2 * p['ackPackets'])
                require(t['accepted'] == expected_controls, 'actual accepted control-message population')
                require(p['sentPackets'] == p['ackPackets'] + (not good and not mismatch and mode != 'missing-seal'),
                        'actual rejected packet population')
                for ordinal, sample in enumerate(samples, 1):
                    require(sample['frame'] == ordinal and sample['slot'] == (ordinal - 1) % 4 and
                            sample['generation'] == (ordinal - 1) // 4 + 1 and
                            sample['map'] == p['map'] and sample['device'] == p['device'], 'exact slot receipt')
                case = dict(parent=p, host=h, transport=t, analysis=verdict, environmentWindow=env, seconds=time.monotonic()-start,
                            hostRaw=identity(log), parentRaw=identity(directory / 'client.stdout'), initialization=inits,
                            settlement=child_settled(p['hostPid'], p['hostCreation']))
                containers_gone(p['nonce']); case['containersIndependentlyGone'] = True
                receipt['cases'].append(case)
                print(json.dumps({'case': mode, 'ok': verdict['ok'], 'failedChecks': verdict['failedChecks']}), flush=True)
                require(verdict['ok'], f'analyzer rejected {mode}: {verdict["failedChecks"]}')
            require(len({c['parent']['nonce'] for c in receipt['cases']}) == len(receipt['cases']), 'connection nonce reused')
        require(receipt['sources'] == [identity(p) for p in sources], 'source changed during gate')
        receipt['buildOnly'] = bool(args.build_only)
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output / 'receipt.json').open('x', encoding='utf-8') as stream:
            json.dump(receipt, stream, ensure_ascii=False, indent=2); stream.write('\n')
    print(json.dumps(dict(ok=receipt['ok'], passed=len(receipt['cases']), error=receipt.get('error'), receipt=str(output / 'receipt.json'))))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
