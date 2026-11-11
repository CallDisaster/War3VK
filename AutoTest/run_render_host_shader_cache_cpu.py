"""Build and run the real shared DXSO canonical owner, CPU only, isolated Desktop.

Only the root build owner runs this. No game, deployment, system edits, or GPU
creation. The same owner is compiled into host and shipping, whose integration
and GPU gates remain separate. Failed runs are never overwritten.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import sys

from run_render_host_protocol_gate import ROOT, identity, pe_identity
from run_render_host_core_gate import execute, check_host_compile_boundary
from run_render_host_d3d9_gate import sources
from run_render_host_transport_gate import require
from isolated_render_desktop import IsolatedRenderDesktop


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    out = args.output.resolve(); out.mkdir(parents=False, exist_ok=False)
    build = args.build.resolve(strict=True)
    paths = set(sources()) | {ROOT/'AutoTest'/name for name in (
        'run_render_host_shader_cache_cpu.py','test_dxso_shader_module_cache.cpp',
        'test_dxso_shader_module_cache_contract.py','isolated_render_desktop.py',
        'render_lab_token.py','render_lab_default_dacl.py')}
    before = [identity(p) for p in sorted(paths)]
    protected = [identity(Path('E:/Work/Warcraft III/d3d9.dll')),
        identity(ROOT/'build32_device_owner_presenter_20261022/src/d3d9/d3d9.dll')]
    receipt = dict(schema='warvk-shader-cache-cpu/v1', ok=False, sources=before,
        protectedBefore=protected, gpuRun=False, gameTouched=False, steps=[])
    try:
        env = {k:v for k,v in os.environ.items() if k.upper() in {'SYSTEMROOT','WINDIR',
            'COMSPEC','PATH','TEMP','TMP','USERPROFILE','APPDATA','LOCALAPPDATA',
            'PROGRAMFILES','PROGRAMFILES(X86)','PROGRAMDATA','NUMBER_OF_PROCESSORS','PROCESSOR_ARCHITECTURE'}}
        targets = ['tools/render_host/dxvk_core/'+name for name in (
            'warvk-dxso-shader-cache-test.exe','warvk-d3d9-host-dev.exe')]
        receipt['steps'].append(execute(['ninja','-C',build,'-j2',*targets],ROOT,out/'build',600,env))
        receipt['steps'].append(execute(['ninja','-C',build,'-n',*targets],ROOT,out/'no-work',30,env))
        require('no work to do' in (out/'no-work.stdout').read_text('utf-8'), 'build not settled')
        receipt['compileBoundary'] = check_host_compile_boundary(build)
        test = build/targets[0]; require(pe_identity(test) == (0x8664,0x20b), 'expected x64 CPU test')
        copy = out/test.name; shutil.copy2(test,copy)
        receipt['test'] = identity(test); receipt['copy'] = identity(copy)
        require(all(receipt['test'][k] == receipt['copy'][k] for k in ('bytes','sha256')), 'copied test differs')
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            receipt['launch'] = desktop.run([copy],cwd=out,stdout_path=out/'run.stdout',
                stderr_path=out/'run.stderr',timeout_s=30,env=env)
            require(receipt['launch']['ok'] is True, 'real shared CPU test failed')
            result = (out/'run.stdout').read_text('utf-8')
            match = re.fullmatch(r'DXSO_SHADER_MODULE_CACHE_CPU checks=([1-9][0-9]*) PASS noDeviceCreated=true\n',result)
            require(match is not None, 'exact completed CPU test witness required')
            receipt['checks'] = int(match[1])
        require(receipt['desktopSafety']['ok'] is True, 'CPU isolated Desktop did not cleanly close')
        receipt['host'] = identity(build/targets[1])
        receipt['ok'] = True
    except Exception as exc:
        receipt['error'] = str(exc)
    finally:
        receipt['sourcesPreserved'] = before == [identity(p) for p in sorted(paths)]
        receipt['protectedAfter'] = [identity(Path(r['path'])) for r in protected]
        receipt['protectedPreserved'] = receipt['protectedAfter'] == protected
        receipt['ok'] &= receipt['sourcesPreserved'] and receipt['protectedPreserved']
        (out/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
    print(json.dumps({k:receipt.get(k) for k in ('ok','checks','error','sourcesPreserved','protectedPreserved')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
