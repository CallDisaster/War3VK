"""CPU/ABI + real journal writer/reader gate; no game or GPU acceptance."""
import argparse
import json
import os
from pathlib import Path
import sys

import analyze_mdl_link_trace as reader
from isolated_render_desktop import IsolatedRenderDesktop
from run_device_owned_build import inputs
from run_fixed_function_key_equivalence import completed
from run_render_host_core_gate import execute
from run_render_host_protocol_gate import ROOT, identity, pe_identity


def require(condition, message):
    if not condition:
        raise ValueError(message)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    build, out = args.build.resolve(strict=True), args.output.resolve()
    require(build.parent == ROOT and build.name.startswith('build32_device_owner_'), 'dedicated build')
    require(out.parent == ROOT/'AutoTest/artifacts', 'fresh artifact child')
    out.mkdir(exist_ok=False)
    paths = set(inputs()) | {Path(__file__), Path(reader.__file__),
        ROOT/'AutoTest/test_mdl_trace_core.cpp', ROOT/'AutoTest/test_mdl_trace_hooks.cpp',
        ROOT/'AutoTest/test_mdl_link_trace.py', ROOT/'AutoTest/test_mdl_trace_integration.py'}
    paths = sorted(paths)
    before = [identity(p) for p in paths]
    player = Path('E:/Work/Warcraft III/d3d9.dll')
    protected = identity(player)
    receipt = dict(schema='warvk-mdl-trace-cpu-gate/v1',ok=False,cpuOnly=True,
                   syntheticRoundtrip=True,gameLaunched=False,gpuExecuted=False,
                   sources=before,preservedBefore=protected,steps=[],runs=[])
    env = os.environ.copy()
    env['PATH'] = 'D:/Environment/Python3.13.11/Scripts;E:/Dev/Vulkan SDK/Bin;' + env['PATH']
    targets = ['src/d3d9/warvk-mdl-trace-core-test.exe','src/d3d9/warvk-mdl-trace-hooks-test.exe']
    try:
        receipt['steps'].append(execute(['ninja','-C',build,'-j2',*targets],ROOT,out/'build',180,env))
        receipt['steps'].append(execute(['ninja','-C',build,'-n',*targets,'src/d3d9/d3d9.dll'],ROOT,out/'no-work',30,env))
        require('no work to do' in (out/'no-work.stdout').read_text('utf-8'), 'candidate drift')
        binaries = [build/t for t in targets]
        require(all(pe_identity(p)==(0x14c,0x10b) for p in binaries),'x86 CPU fixtures required')
        receipt['products'] = [identity(p) for p in binaries]
        receipt['dll'] = identity(build/'src/d3d9/d3d9.dll')
        child = {k:v for k,v in env.items() if k.upper() in {
            'SYSTEMROOT','WINDIR','COMSPEC','PATH','TEMP','TMP','USERPROFILE',
            'APPDATA','LOCALAPPDATA','PROGRAMDATA'}}
        fixture = out/'journal.bin'
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            for label, binary in zip(('core','hooks'), binaries):
                command = [binary,'--emit',fixture] if label=='core' else [binary]
                launch = desktop.run(command,cwd=out,stdout_path=out/(label+'.stdout'),
                    stderr_path=out/(label+'.stderr'),timeout_s=30,env=child)
                receipt['runs'].append(dict(label=label,launch=launch))
                completed(launch,0)
                require((out/(label+'.stderr')).read_bytes()==b'',label+' stderr')
        safety=receipt['desktopSafety']
        require(safety['ok'] and safety['desktopClosed'] and safety['creatorExited'] and
                not safety['cleanupErrors'] and not safety['globalInputUsed'] and
                not safety['desktopSwitchRequested'],'isolation cleanup')
        env['WARVK_MDL_TRACE_CPP_FIXTURE']=str(fixture)
        for name in ('test_mdl_link_trace.py','test_mdl_trace_integration.py',
                     'test_d3d9_migration_census.py','test_d3d9_migration_census_integration.py',
                     'test_native_d3d9_survey_static.py'):
            receipt['steps'].append(execute([sys.executable,'-B',ROOT/'AutoTest'/name],
                ROOT,out/name.removesuffix('.py'),120,env))
        receipt['roundtrip'] = reader.analyze_file(fixture,process_exited=True)
        receipt['fixture'] = identity(fixture)
        receipt['ok']=True
    except Exception as error:
        receipt['error']=f'{type(error).__name__}: {error}'
    finally:
        receipt['sourceUnchanged']=before==[identity(p) for p in paths]
        receipt['preservedAfter']=identity(player)
        receipt['preservedUnchanged']=protected==receipt['preservedAfter']
        receipt['ok'] &= receipt['sourceUnchanged'] and receipt['preservedUnchanged']
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in ('ok','error','sourceUnchanged','preservedUnchanged')}))
    return 0 if receipt['ok'] else 1


if __name__=='__main__':
    sys.exit(main())
