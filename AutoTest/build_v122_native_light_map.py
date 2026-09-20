"""CreateNew isolated stock-light fixture; original author map remains immutable."""
import argparse
import json
import re
import subprocess
from pathlib import Path
from build_v122_jass_vm_map import ROOT, MAP, PS, YDWE, identity, split_globals

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--name', required=True)
    ap.add_argument('--visual', action='store_true')
    ap.add_argument('--model', type=Path)
    args = ap.parse_args()
    if not args.name.replace('_', '').isalnum():
        raise ValueError('invalid name')
    out = ROOT/'AutoTest/artifacts/v122_visual_api_20260914'/args.name
    before = identity(MAP)
    assert before['sha256'] == '11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF'
    out.mkdir()
    original = ROOT/'AutoTest/artifacts/v122_visual_api_20260914/original-war3map.j'
    assert not args.model or args.visual
    model_before=identity(args.model) if args.model else None
    harness = ROOT/('AutoTest/v122_native_model_light_japi_scenario.j' if args.model else
        'AutoTest/v122_native_light_visual_scenario.j' if args.visual else 'AutoTest/v122_native_light_scenario.j')
    mg, mb = split_globals(original.read_text(encoding='utf-8-sig'))
    hg, hb = split_globals(harness.read_text(encoding='utf-8-sig'))
    if args.visual:
        # The visual fixture uses the candidate's real public JASS wrappers.
        ag, ab = split_globals((ROOT/'WarVK/jass/warvk_api.j').read_text(encoding='utf-8-sig'))
        for name in re.findall(r'^function (\w+) takes', ab, re.M):
            pattern = rf'^[ \t]*function {re.escape(name)} takes[^\n]*\n.*?^[ \t]*endfunction[^\n]*\n'
            mb, count = re.subn(pattern, '', mb, flags=re.M | re.S)
            assert count == (0 if name in ('WarVKSetModelPointLightsEnabled',
                'WarVKGetModelPointLightCount','WarVKIsModelPointLightRegistered') else 1), name
        for line in ag.splitlines():
            match = re.match(r'\s*(?:constant\s+)?\w+\s+(\w+)\s*=', line)
            if match:
                name = match.group(1)
                mg, count = re.subn(rf'^[ \t]*(?:constant[ \t]+)?\w+[ \t]+{name}[ \t]*=.*\n', '', mg, flags=re.M)
                assert count == 1, name
        mg += ag
        hb = ab + hb
    assert hb.count('@NATIVE_RECEIPT@') == 1
    hb = hb.replace('@NATIVE_RECEIPT@', args.name)
    needle = 'function main takes nothing returns nothing\n'
    assert mb.count(needle) == 1
    mb = mb.replace(needle, needle+'    call TimerStart(CreateTimer(), 25.0, false, function V122NativeStart)\n')
    script = out/'war3map.j'
    with script.open('x', encoding='utf-8', newline='\n') as f:
        f.write('globals\n'+mg+hg+'endglobals\n'+hb+mb)
    checked = subprocess.run([str(YDWE/'compiler/pjass/pjass-latest.exe'),
        str(YDWE/'compiler/jass/24/common.j'), str(YDWE/'compiler/jass/24/blizzard.j'), str(script)],
        capture_output=True, text=True)
    with (out/'pjass.txt').open('x') as f:
        f.write(checked.stdout+checked.stderr)
    if checked.returncode: raise RuntimeError('pjass failed')
    target = out/'native-lights.w3x'
    model_args=['-ImportModel',str(args.model),'-ImportName','war3mapImported\\WarVKReview\\TorchHumanUser32059406.mdx'] if args.model else []
    subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT/'AutoTest/v122_mpq_script_copy.ps1'), '-InputMap', str(MAP),
        '-ExtractedScript', str(out/'original.j'), '-OutputMap', str(target),
        '-ReplacementScript', str(script)]+model_args, check=True)
    subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT/'AutoTest/v122_mpq_script_copy.ps1'), '-InputMap', str(target),
        '-ExtractedScript', str(out/'readback.j')], check=True)
    assert (out/'readback.j').read_bytes() == script.read_bytes()
    assert identity(MAP) == before
    receipt = dict(source=before, map=identity(target), harness=identity(harness),
        script=identity(script), receiptPrefix=args.name, pjassExit=0,
        exactReadback=True, authorMapUnchanged=True, runtimeExecuted=False)
    receipt['visual'] = args.visual
    if args.model:
        assert identity(args.model)==model_before
        receipt['importedModel']=model_before
        receipt['importedModelPath']='war3mapImported\\WarVKReview\\TorchHumanUser32059406.mdx'
    with (out/'manifest.json').open('x') as f:
        json.dump(receipt, f, indent=2)
    print(json.dumps(receipt))

if __name__ == '__main__': main()
