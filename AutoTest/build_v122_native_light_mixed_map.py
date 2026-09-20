"""New isolated fixture from the frozen player map; never edit the author map."""
import argparse
import json
import subprocess
from pathlib import Path
from build_v122_jass_vm_map import ROOT, PS, YDWE, identity, split_globals

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--name',required=True); args=ap.parse_args()
    assert args.name.replace('_','').isalnum()
    original=ROOT/'AutoTest/artifacts/v122_native_lights_20260914/player_report_203145_3b0b/WorldEditTestMap.w3x'
    before=identity(original)
    assert before['sha256']=='DF4482EE2A585CB4FE0814D9D199A4E3ABA59B5C7EA6CEAF0B5158F3718746F4'
    out=ROOT/'AutoTest/artifacts/v122_visual_api_20260914'/args.name
    out.mkdir()
    helper=ROOT/'AutoTest/v122_mpq_script_copy.ps1'
    def extract(src,dst):
        subprocess.run([PS,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(helper),
            '-InputMap',str(src),'-ExtractedScript',str(dst)],check=True)
    extract(original,out/'original.j')
    base=(out/'original.j').read_text(encoding='utf-8-sig')
    assert 'function V122NativeStart ' not in base
    assert 'function WarVKSetModelPointLightsEnabled ' in base
    mg,mb=split_globals(base)
    harness=ROOT/'AutoTest/v122_native_light_mixed_scenario.j'
    hg,hb=split_globals(harness.read_text(encoding='utf-8-sig'))
    assert hb.count('@NATIVE_RECEIPT@')==1
    hb=hb.replace('@NATIVE_RECEIPT@',args.name)
    entry='function main takes nothing returns nothing\n'
    assert mb.count(entry)==1
    # JASS functions must follow callees. Existing API wrappers precede main;
    # place the harness here, not before the map's already-compiled wrappers.
    mb=mb.replace(entry,hb+entry+'    call TimerStart(CreateTimer(),10.0,false,function V122NativeStart)\n')
    script=out/'war3map.j'
    with script.open('x',encoding='utf-8',newline='\n') as f: f.write('globals\n'+mg+hg+'endglobals\n'+mb)
    check=subprocess.run([str(YDWE/'compiler/pjass/pjass-latest.exe'),
        str(YDWE/'compiler/jass/24/common.j'),str(YDWE/'compiler/jass/24/blizzard.j'),str(script)],capture_output=True,text=True)
    with (out/'pjass.txt').open('x') as f: f.write(check.stdout+check.stderr)
    assert check.returncode==0, 'pjass failed'
    target=out/'native-lights.w3x'
    subprocess.run([PS,'-NoProfile','-ExecutionPolicy','Bypass','-File',str(helper),
        '-InputMap',str(original),'-ExtractedScript',str(out/'second-read.j'),
        '-OutputMap',str(target),'-ReplacementScript',str(script)],check=True)
    extract(target,out/'readback.j')
    assert (out/'readback.j').read_bytes()==script.read_bytes()
    assert identity(original)==before
    receipt=dict(source=before,map=identity(target),harness=identity(harness),script=identity(script),
        receiptPrefix=args.name,pjassExit=0,exactReadback=True,authorMapUnchanged=True,
        runtimeExecuted=False,visual=True,mixedPlayerFixture=True,
        importedResource='war3mapImported\\TorchHuman.mdx',
        importedResourceSha256='ED12243B5560E39BD118DF6591738EDF22C2131DA575E8202C62ADB4EF749C3A')
    with (out/'manifest.json').open('x') as f: json.dump(receipt,f,indent=2)
    print(json.dumps(receipt))

if __name__=='__main__': main()
