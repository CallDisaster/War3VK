"""E1 actual CPU components on both widths, no product DLL/GPU/game/IPC.

Fresh output only. Serialize compiler and native children BelowNormal; record
actual source identities and output. --part ingress permits the independently
owned first component to be checked while wire/store sources are being authored.
"""
import argparse
import hashlib
import importlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import unittest

from run_render_host_protocol_gate import ROOT, identity, pe_identity


def invoke(command, timeout=180):
    p = subprocess.run(list(map(str, command)), cwd=ROOT, capture_output=True,
        text=True, encoding='utf-8', errors='replace', timeout=timeout,
        creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
    if p.returncode:
        raise RuntimeError(f'exit={p.returncode}: {command}\n{p.stdout}\n{p.stderr}')
    return p.stdout, p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compiler32', type=Path, required=True)
    ap.add_argument('--compiler64', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--part', choices=('ingress', 'wire', 'all'), default='all')
    a = ap.parse_args()
    output = a.output.resolve()
    output.mkdir(parents=False, exist_ok=False)
    receipt = dict(scope='RECORDER_OFFLOAD_CPU_CORES_ONLY', part=a.part, ok=False,
        productDllBuilt=False, ipcTested=False, gameLaunched=False, gpuTested=False,
        gameMemoryBenefitMeasured=False, builds=[], runs=[], source=[])
    cases = [('ingress', ['AutoTest/test_recorder_ingress.cpp'])]
    if a.part == 'wire':
        cases = [('wire', ['AutoTest/test_recorder_event_wire.cpp', 'tools/render_host/recorder_event_wire.cpp'])]
    if a.part == 'all':
        cases += [('wire', ['AutoTest/test_recorder_event_wire.cpp', 'tools/render_host/recorder_event_wire.cpp']),
                  ('history', ['AutoTest/test_recorder_history_store.cpp', 'tools/render_host/recorder_history_store.cpp',
                               'tools/render_host/recorder_event_wire.cpp']),
                  ('handoff', ['AutoTest/test_recorder_offload_handoff.cpp', 'tools/render_host/recorder_history_store.cpp',
                               'tools/render_host/recorder_event_wire.cpp'])]
    sources = {ROOT / p for _, paths in cases for p in paths}
    sources |= {Path(__file__).resolve(), ROOT/'tools/render_host/recorder_event_wire.h',
        ROOT/'tools/render_host/recorder_ingress.h', ROOT/'src/d3d9/war3/tools/war3_frame_evidence_core.h'}
    if a.part == 'all':
        sources.add(ROOT/'tools/render_host/recorder_history_store.h')
    if a.part != 'ingress':
        sources.add(ROOT/'AutoTest/test_recorder_event_wire_golden.py')
    regressions = ('test_frame_evidence_control_static', 'test_frame_recorder_self_contained_static',
                   'test_frame_recorder_memory_static', 'test_analyze_frame_evidence') if a.part == 'all' else ()
    sources.update(ROOT/'AutoTest'/f'{name}.py' for name in regressions)
    sources = sorted(sources)
    try:
        receipt['source'] = [identity(p) for p in sources]
        for name in regressions:
            log = io.StringIO()
            module = importlib.import_module(name)
            result = unittest.TextTestRunner(stream=log).run(unittest.defaultTestLoader.loadTestsFromModule(module))
            receipt.setdefault('pythonRegressions',[]).append(dict(name=name,tests=result.testsRun,
                ok=result.wasSuccessful(),log=log.getvalue()))
            if not result.wasSuccessful() or not result.testsRun:
                raise ValueError('regression failed: '+name+' '+log.getvalue())
        golden = None
        if a.part != 'ingress':
            import test_recorder_event_wire_golden as independent
            log = io.StringIO()
            result = unittest.TextTestRunner(stream=log).run(unittest.defaultTestLoader.loadTestsFromModule(independent))
            receipt['pythonGolden'] = dict(tests=result.testsRun, ok=result.wasSuccessful(), log=log.getvalue())
            if not result.wasSuccessful() or not result.testsRun:
                raise ValueError('independent Python golden tests failed: '+log.getvalue())
            h, event = independent.make_golden()
            golden = independent.pack_packet(h,[event])
            if golden.hex() != independent.GOLDEN_HEX:
                raise ValueError('independent golden construction mismatch')
            receipt['pythonGolden']['sha256'] = hashlib.sha256(golden).hexdigest()
        for bits, compiler in ((32,a.compiler32.resolve(strict=True)),(64,a.compiler64.resolve(strict=True))):
            for name, files in cases:
                exe = output/f'{name}-{bits}.exe'
                command = [compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-pthread',*files,'-o',exe]
                stdout,stderr = invoke(command)
                if pe_identity(exe) != ((0x14c,0x10b) if bits==32 else (0x8664,0x20b)):
                    raise ValueError('wrong executable width')
                receipt['builds'].append(dict(name=name,bits=bits,command=list(map(str,command)),
                    compiler=identity(compiler),exe=identity(exe),stdout=stdout,stderr=stderr))
                stdout,stderr = invoke([exe],timeout=60)
                (output/f'{name}-{bits}.stdout.txt').write_text(stdout,encoding='utf-8')
                (output/f'{name}-{bits}.stderr.txt').write_text(stderr,encoding='utf-8')
                match = re.search(r'checks=(\d+)\s+PASS\s*$',stdout)
                if not match or int(match[1])<=0 or stderr:
                    raise ValueError('unexpected test output: '+stdout+' '+stderr)
                if name == 'history' and ('skipped' in stdout.lower() or 'storageBytes cap=262144: 102760448' not in stdout):
                    raise ValueError('maximum history allocation/visit was not actually covered')
                if name == 'wire':
                    lines = re.findall(r'^GOLDEN=([0-9a-fA-F]+)\s*$',stdout,re.MULTILINE)
                    if len(lines)!=1 or bytes.fromhex(lines[0])!=golden:
                        raise ValueError('native bytes differ from independent Python golden')
                    receipt.setdefault('crossWidthGolden',[]).append(dict(bits=bits,
                        bytes=len(golden),sha256=hashlib.sha256(golden).hexdigest(),matches=True))
                receipt['runs'].append(dict(name=name,bits=bits,checks=int(match[1]),stdout=stdout))
                print(f'PASS {name} bits={bits} checks={match[1]}',flush=True)
        if receipt['source'] != [identity(p) for p in sources]:
            raise ValueError('source changed during verification')
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        with (output/'receipt.json').open('x',encoding='utf-8') as stream:
            json.dump(receipt,stream,ensure_ascii=False,indent=2)
            stream.write('\n')
    print(json.dumps({k:receipt[k] for k in ('ok','part')} | dict(receipt=str(output/'receipt.json'),error=receipt.get('error')),ensure_ascii=False))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':
    raise SystemExit(main())
