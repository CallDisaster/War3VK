"""Freeze the validated diagnostic DLL + launcher/readers + matching research source.
Local CreateNew package only. Does not deploy, upload, commit or publish.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]
SHA='6D6C7F3C3C834CE56692881A5C46DE6086D9AF2B7F93C2C6848E39A5D5F594B1'
SIZE=35805448
def require(ok,message):
    if not ok:raise RuntimeError(message)
def identity(path):
    with path.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest().upper()
    return dict(size=path.stat().st_size,sha256=digest)
def main():
    runs=ROOT/'AutoTest/artifacts/frame_evidence_runs'
    r17=json.loads((runs/'input_gather_r17_20260915/receipt.json').read_text(encoding='utf-8'))
    r18=json.loads((runs/'input_full_watch_r18_20260915/receipt.json').read_text(encoding='utf-8'))
    r19=json.loads((runs/'input_watch_r19_20260915/receipt.json').read_text(encoding='utf-8'))
    require(r17['ok'] and r17['inputs']['focusReconstructed']==r17['inputs']['focusDraws'] and r17['inputs']['focusDraws']>0,'R17 focus coverage')
    require(r18['restoreOk'] and r18['historyStatus']['durationSatisfied'] and r18['historyFrames']==112,'R18 image subgate')
    require(r19['ok'] and r19['watcherCompleted'] and r19['restoreOk'] and r19['playerUntouched'],'R19 watcher/restore')
    for name in ('input_gather_r17_20260915','input_full_watch_r18_20260915','input_watch_r19_20260915'):
        pin=json.loads((runs/name/'preflight.json').read_text(encoding='utf-8'))
        require(pin['candidate']['sha256']==SHA and pin['candidate']['size']==SIZE,'runtime candidate pin')
    dll=ROOT/'build32/src/d3d9/d3d9.dll';require(identity(dll)==dict(size=SIZE,sha256=SHA),'DLL drift')
    out=Path('C:/Users/Administrator/Desktop/WarVK-v1.22-FrameInputs-20260915')
    archive_path=out.parent/(out.name+'.zip');require(not out.exists() and not archive_path.exists(),'CreateNew package path')
    out.mkdir()
    selected={'d3d9.dll':dll,'README.md':ROOT/'docs/FRAME_INPUTS_CANDIDATE_README.md'}
    for name in ('frame_history_watch.py','frame_evidence_control.py','shadow_pose_full_trace_control.py',
                 'analyze_frame_evidence.py','analyze_frame_history.py','analyze_frame_inputs.py',
                 'launch_frame_history_player.ps1','export_frame_history_video.py','package_frame_input_research.py'):
        selected['AutoTest/'+name]=ROOT/'AutoTest'/name
    # Freeze exactly the source subset used by the offline research packer. No
    # later checkout, asset edits or newer DLL may silently replace this snapshot.
    source_paths=(
        'src/d3d9/d3d9_device.cpp','src/d3d9/d3d9_war3_shadow.cpp','src/d3d9/d3d9_war3_shadow_resources.cpp',
        'src/d3d9/d3d9_war3_scene.h','src/d3d9/d3d9_war3_pipeline.cpp','src/util/util_matrix.h',
        'src/d3d9/war3/tools/war3_frame_inputs.cpp','src/d3d9/war3/tools/war3_frame_inputs_core.h',
        'src/d3d9/shaders/war3_frame_input_gather.comp','src/dxvk/dxvk_memory.cpp','src/dxvk/dxvk_memory.h','src/dxvk/dxvk_buffer.cpp',
        'src/d3d9/war3/render/war3_shadow_drawtime_cache_policy.h','src/d3d9/war3/render/war3_canonical_draw.h',
        'src/d3d9/war3/shadow/war3_shadow_renderer_core.cpp','src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp',
        'subprojects/war3fx/shaders/war3_shadow_caster_vert.vert','subprojects/war3fx/shaders/war3_shadow_caster_frag.frag',
        'AutoTest/test_frame_inputs.py','docs/research/2026-09-15-frame-input-evidence.md',
        'docs/research/2026-09-15-fissure-localized-route-evidence.md','docs/agent-history/DEVELOPMENT_CHANGELOG.md')
    for rel in source_paths:selected[rel]=ROOT/rel
    for name in ('input_gather_r17_20260915','input_full_watch_r18_20260915','input_watch_r19_20260915'):
        for rel in ('receipt.json','preflight.json','resolution.json','env.json'):
            selected['validation/'+name+'/'+rel]=runs/name/rel
    files={}
    for rel,src in selected.items():
        require(src.is_file(),'missing package source: '+str(src));pin=identity(src)
        dst=out/rel;dst.parent.mkdir(parents=True,exist_ok=True)
        with src.open('rb') as f,dst.open('xb') as g:shutil.copyfileobj(f,g)
        require(identity(src)==pin and identity(dst)==pin,'copy drift');files[rel]=pin
    meta=dict(candidate=True,releaseReady=False,fissureFixed=False,rootCauseReady=False,dllSha256=SHA,
              gitHead=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),files=files)
    with (out/'package-info.json').open('x',encoding='utf-8') as f:json.dump(meta,f,indent=2)
    with zipfile.ZipFile(archive_path,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for file in sorted(out.rglob('*')):
            if file.is_file():z.write(file,file.relative_to(out).as_posix())
    with zipfile.ZipFile(archive_path) as z:
        require(z.testzip() is None,'CRC failed')
        for rel,pin in files.items():require(hashlib.sha256(z.read(rel)).hexdigest().upper()==pin['sha256'],'zip hash')
    print(json.dumps(dict(directory=str(out),zip=str(archive_path),identity=identity(archive_path),files=len(files)),indent=2))
if __name__=='__main__':main()
