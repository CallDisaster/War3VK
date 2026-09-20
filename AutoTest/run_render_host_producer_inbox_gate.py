"""P3a actual process-private SPSC core on Win32/Win64; no IPC or game."""
import argparse
import json
from pathlib import Path
from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke


def load_result(raw):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                raise ValueError('duplicate result key')
            out[key] = value
        return out
    def nonfinite(_):
        raise ValueError('nonfinite result')
    value = json.loads(raw, object_pairs_hook=pairs, parse_constant=nonfinite)
    if type(value) is not dict:
        raise ValueError('result must be object')
    return value


def validate(result, bits):
    if result.get('ok') is not True or result.get('bits') != bits:
        raise ValueError('native result/width')
    for key in ('checks', 'queueBytes', 'sampleBytes', 'atomicBytes', 'pausedAttempts',
                'pausedPublished', 'pausedFull', 'concurrentAttempts', 'concurrentPublished',
                'concurrentFull', 'concurrentBytes'):
        if type(result.get(key)) is not int or result[key] < 0:
            raise ValueError('counter type/range ' + key)
    if not (262144 < result['queueBytes'] <= 263168 and 65536 < result['sampleBytes'] <= 65664):
        raise ValueError('fixed footprint')
    if result['atomicBytes'] != 4 or result.get('lockFree') is not True:
        raise ValueError('atomic contract')
    if (result['pausedAttempts'], result['pausedPublished'], result['pausedFull']) != (2000, 4, 1996):
        raise ValueError('paused consumer did not prove full/drop path')
    if result['concurrentAttempts'] != 50000 or not 100 < result['concurrentPublished'] <= 50000:
        raise ValueError('positive concurrent population')
    if result['concurrentPublished'] + result['concurrentFull'] != 50000:
        raise ValueError('population algebra')
    if not result['concurrentPublished'] <= result['concurrentBytes'] <= result['concurrentPublished']*65536:
        raise ValueError('byte population')
    if result['checks'] < 100000 or result.get('cppAllocationForbiddenInQueueCalls') is not True:
        raise ValueError('checks/allocation boundary')
    if result.get('consumerStopDrained') is not True:
        raise ValueError('consumer-requested stop must preserve accepted samples')
    if result.get('freshScopeRecovery') is not True:
        raise ValueError('fresh connection/scope recovery missing')
    if result.get('gpuTested') is not False or result.get('ipcTested') is not False:
        raise ValueError('proof overclaim')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler32', required=True, type=Path)
    p.add_argument('--compiler64', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args();output = args.output.resolve();output.mkdir(parents=False, exist_ok=False)
    receipt = dict(schema=1, scope='P3A_PROCESS_PRIVATE_SPSC_CORE', ok=False,
                   gameLaunched=False, shippingDllBuilt=False, ipcTested=False, gpuTested=False, runs=[])
    paths = [ROOT/name for name in ('tools/render_host/producer_inbox.h', 'tools/render_host/protocol.h',
             'AutoTest/test_render_host_producer_inbox.cpp', 'AutoTest/run_render_host_producer_inbox_gate.py',
             'AutoTest/run_render_host_protocol_gate.py')]
    try:
        receipt['source'] = [identity(path) for path in paths]
        for bits, compiler in ((32,args.compiler32),(64,args.compiler64)):
            compiler=compiler.resolve(strict=True);exe=output/f'producer-inbox-{bits}.exe'
            stdout, stderr=invoke([compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror','-static','-pthread',
                'AutoTest/test_render_host_producer_inbox.cpp','-o',exe],120)
            if pe_identity(exe) != ((0x14c,0x10b) if bits==32 else (0x8664,0x20b)):
                raise ValueError('wrong PE')
            raw, errors=invoke([exe],30)
            if errors:raise ValueError(errors)
            result=load_result(raw);validate(result,bits)
            receipt['runs'].append(dict(result=result,compiler=identity(compiler),exe=identity(exe),
                                         compilerStdout=stdout,compilerStderr=stderr))
        if receipt['source'] != [identity(path) for path in paths]:raise ValueError('source changed')
        receipt['ok']=True
    except Exception as e:receipt['error']=str(e)
    finally:
        with (output/'receipt.json').open('x',encoding='utf-8') as f:json.dump(receipt,f,indent=2)
    print(json.dumps(dict(ok=receipt['ok'],receipt=str(output/'receipt.json'),
                         results=[r['result'] for r in receipt['runs']],error=receipt.get('error'))))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':raise SystemExit(main())
