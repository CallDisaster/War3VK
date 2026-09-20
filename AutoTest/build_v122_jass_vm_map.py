"""Prepare an isolated COPY, replacing known API wrappers with exact current JASS.

Does not save the author's map or claim YDWE GUI/editor-save acceptance. Requires
prior read-only MPQ extraction. Each output is CreateNew; hashes pin all inputs.
"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'AutoTest/artifacts/v122_visual_api_20260914/jass-vm-map-r3'
RECEIPT_PREFIX = 'v122_jass_vm_r3_20260914'
MAP = Path('E:/Work/Warcraft III/Maps/Test/WorldEditTestMap.w3x')
PS = 'C:/Windows/SysWOW64/WindowsPowerShell/v1.0/powershell.exe'
YDWE = Path('E:/Work/War3/YDWE1.32.13 - MemoryHack')


def identity(path):
    return dict(path=str(path), size=path.stat().st_size,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest().upper())


def split_globals(text):
    match = re.match(r'globals\n(.*?)^endglobals\n', text, re.M | re.S)
    if not match:
        raise ValueError('one initial globals block required')
    return match.group(1), text[match.end():]


def main():
    before = identity(MAP)
    if before['sha256'] != '11376DE62E38EE1B76111FA48C3AB86122079748205CA13F433EFB047A85E7CF':
        raise ValueError('source map changed')
    OUT.mkdir()  # Never replace a prior test map or evidence.
    original = ROOT/'AutoTest/artifacts/v122_visual_api_20260914/original-war3map.j'
    api = ROOT/'WarVK/jass/warvk_api.j'
    harness = ROOT/'AutoTest/v122_jass_vm_scenario.j'
    mg, mb = split_globals(original.read_text(encoding='utf-8-sig'))
    ag, ab = split_globals(api.read_text(encoding='utf-8-sig'))
    hg, hb = split_globals(harness.read_text(encoding='utf-8-sig'))
    if hb.count('@V122_RECEIPT_PREFIX@') != 1:
        raise ValueError('receipt prefix substitution must be unique')
    hb = hb.replace('@V122_RECEIPT_PREFIX@', RECEIPT_PREFIX)
    names = re.findall(r'^function (\w+) takes', ab, re.M)
    for name in names:
        pattern = rf'^[ \t]*function {re.escape(name)} takes[^\n]*\n.*?^[ \t]*endfunction[^\n]*\n'
        mb, count = re.subn(pattern, '', mb, flags=re.M | re.S)
        if count != 1:
            raise ValueError(f'expected one old wrapper: {name}: {count}')
    # Replace only matching declarations in the copied map, not arbitrary globals.
    for line in ag.splitlines():
        match = re.match(r'\s*(?:constant\s+)?\w+\s+(\w+)\s*=', line)
        if match:
            name = match.group(1)
            mg, count = re.subn(rf'^[ \t]*(?:constant[ \t]+)?\w+[ \t]+{name}[ \t]*=.*\n', '', mg, flags=re.M)
            if count != 1:
                raise ValueError(f'expected one old global {name}: {count}')
    needle = 'function main takes nothing returns nothing\n'
    if mb.count(needle) != 1:
        raise ValueError('main ambiguity')
    mb = mb.replace(needle, needle+'    call TimerStart(CreateTimer(), 30.0, false, function V122Start)\n')
    merged = 'globals\n'+mg+ag+hg+'endglobals\n'+ab+hb+mb
    script = OUT/'war3map.j'
    with script.open('x', encoding='utf-8', newline='\n') as f:
        f.write(merged)
    checked = subprocess.run([str(YDWE/'compiler/pjass/pjass-latest.exe'),
        str(YDWE/'compiler/jass/24/common.j'), str(YDWE/'compiler/jass/24/blizzard.j'), str(script)],
        capture_output=True, text=True)
    with (OUT/'pjass.txt').open('x') as f:
        f.write(checked.stdout+checked.stderr)
    if checked.returncode:
        raise RuntimeError('pjass failed; inspect pjass.txt')
    target = OUT/'v122-jass-vm.w3x'
    subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT/'AutoTest/v122_mpq_script_copy.ps1'), '-InputMap', str(MAP),
        '-ExtractedScript', str(OUT/'original-again.j'), '-OutputMap', str(target),
        '-ReplacementScript', str(script)], check=True)
    subprocess.run([PS, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
        str(ROOT/'AutoTest/v122_mpq_script_copy.ps1'), '-InputMap', str(target),
        '-ExtractedScript', str(OUT/'readback.j')], check=True)
    if (OUT/'readback.j').read_bytes() != script.read_bytes() or identity(MAP) != before:
        raise RuntimeError('readback/source identity failure')
    receipt = dict(source=before, map=identity(target), api=identity(api),
        harness=identity(harness), script=identity(script), pjassExit=0,
        exactMpqScriptReadback=True, sourceMapUnchanged=True,
        wrapperCount=len(names), receiptPrefix=RECEIPT_PREFIX,
        jassVmExecuted=False, editorSaveAccepted=False)
    with (OUT/'manifest.json').open('x') as f:
        json.dump(receipt, f, indent=2)
    print(json.dumps(receipt))


if __name__ == '__main__':
    main()
