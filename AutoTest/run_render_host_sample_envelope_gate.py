"""P3b1 actual payload envelope core on both widths; not real IPC proof."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from run_render_host_protocol_gate import ROOT, identity, pe_identity, invoke


def golden():
    return struct.pack('<4sHHIIII7Q',b'WVP1',1,0,80,0,81,1,
        0x0807060504030201,0x1817161514131211,3,7,42,15,1)+b'\xa5'


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler32',required=True,type=Path);p.add_argument('--compiler64',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path);args=p.parse_args()
    output=args.output.resolve();output.mkdir(parents=False,exist_ok=False)
    receipt=dict(schema=1,scope='P3B1_ENVELOPE_CODEC_AND_PAYLOAD_ORDER',ok=False,
                 ipcTested=False,gpuTested=False,gameLaunched=False,shippingDllBuilt=False,runs=[])
    paths=[ROOT/name for name in ('tools/render_host/protocol.h','tools/render_host/protocol.cpp',
        'tools/render_host/slot_ledger.h','tools/render_host/slot_ledger.cpp','tools/render_host/producer_inbox.h',
        'tools/render_host/sample_envelope.h','tools/render_host/sample_envelope.cpp',
        'AutoTest/test_render_host_sample_envelope.cpp','AutoTest/run_render_host_sample_envelope_gate.py',
        'AutoTest/run_render_host_protocol_gate.py')]
    try:
        receipt['source']=[identity(path) for path in paths]
        for bits,compiler in ((32,args.compiler32),(64,args.compiler64)):
            compiler=compiler.resolve(strict=True);exe=output/f'sample-envelope-{bits}.exe'
            stdout,stderr=invoke([compiler,'-std=c++17','-O2','-Wall','-Wextra','-Werror',
                '-Wno-misleading-indentation','-static','-pthread','tools/render_host/protocol.cpp',
                'tools/render_host/slot_ledger.cpp','tools/render_host/sample_envelope.cpp',
                'AutoTest/test_render_host_sample_envelope.cpp','-o',exe],120)
            if pe_identity(exe)!=((0x14c,0x10b) if bits==32 else (0x8664,0x20b)):raise ValueError('PE width')
            result,errors=invoke([exe],30)
            match=re.fullmatch(r'P3 envelope checks=(\d+) bits=(32|64) positive=10000 PASS',result)
            if not match or int(match[1])<50000 or int(match[2])!=bits or errors:raise ValueError('native proof: '+result+errors)
            encoded,errors=invoke([exe,'--golden'],10)
            if errors or bytes.fromhex(encoded)!=golden():raise ValueError('independent golden mismatch')
            receipt['runs'].append(dict(bits=bits,checks=int(match[1]),positive=10000,
                goldenBytes=len(golden()),goldenSha256=hashlib.sha256(golden()).hexdigest().upper(),
                compiler=identity(compiler),exe=identity(exe),compilerStdout=stdout,compilerStderr=stderr))
        if receipt['source']!=[identity(path) for path in paths]:raise ValueError('source changed during proof')
        receipt['ok']=True
    except Exception as e:receipt['error']=str(e)
    finally:
        with (output/'receipt.json').open('x',encoding='utf-8') as f:json.dump(receipt,f,indent=2)
    print(json.dumps(dict(ok=receipt['ok'],checks=[r['checks'] for r in receipt['runs']],
                         receipt=str(output/'receipt.json'),error=receipt.get('error'))))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':raise SystemExit(main())
