"""CreateNew local candidate + source-review archives. Never commit/tag/upload."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT=Path(__file__).resolve().parents[1]
EXPECTED='5B6D4E4131FED3C940498ABD2DF687CF9C6A2F7317990448B7CDB9DC4D91262F'
def info(p):return dict(bytes=p.stat().st_size,sha256=hashlib.sha256(p.read_bytes()).hexdigest().upper())
def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT).decode('utf-8')
def require(ok,why):
    if not ok:raise RuntimeError(why)
def verified_copy(src,dst):
    before=info(src);dst.parent.mkdir(parents=True,exist_ok=True)
    with src.open('rb') as r,dst.open('xb') as w:shutil.copyfileobj(r,w)
    require(info(src)==before and info(dst)==before,'copy identity changed');return before
def zip_verified(directory,target):
    members={p.relative_to(directory).as_posix():info(p) for p in directory.rglob('*') if p.is_file()}
    with zipfile.ZipFile(target,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for rel in sorted(members):z.write(directory/rel,rel)
    with zipfile.ZipFile(target) as z:
        require(len(z.infolist())==len(members),'archive members')
        for entry in z.infolist():require(hashlib.sha256(z.read(entry)).hexdigest().upper()==members[entry.filename]['sha256'],'archive hash')
    return info(target)
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();candidate_zip=out.parent/(out.name+'.zip')
    require(not out.exists() and not candidate_zip.exists(),'output exists')
    dll=ROOT/'build32/src/d3d9/d3d9.dll';require(info(dll)==dict(bytes=35693151,sha256=EXPECTED),'DLL changed')
    receipt_path=ROOT/'AutoTest/artifacts/frame_evidence_runs/shortcut_final_r15_20260915/receipt.json'
    receipt=json.loads(receipt_path.read_text(encoding='utf-8'))
    require(receipt['ok'] and receipt['historyFrames']==132 and receipt['restoreOk'] and receipt['playerUntouched'] and
            receipt['mapUntouched'] and not receipt['gpuEvents'] and not receipt['newDumps'],'runtime safety closure')
    run_preflight=json.loads(receipt_path.with_name('preflight.json').read_text(encoding='utf-8'))
    require(run_preflight['candidate']['sha256']==EXPECTED,'runtime candidate mismatch')
    history_analysis=json.loads(receipt_path.with_name('history-analysis.json').read_text(encoding='utf-8'))
    require(history_analysis['historyWindowLinked'] and history_analysis['preTriggerSeconds']>=1 and len(history_analysis['frames'])==132 and
            all(f.get('actualDrawStateLinked') for f in history_analysis['frames']),'actual draw window not linked')
    watcher=json.loads((ROOT/'AutoTest/artifacts/frame_evidence_runs/shortcut_hud_r14_20260915/receipt.json').read_text(encoding='utf-8'))
    require(watcher['ok'] and watcher['watcherCompleted'] and watcher['restoreOk'],'watcher not verified')
    watcher_pin=json.loads((ROOT/'AutoTest/artifacts/frame_evidence_runs/shortcut_hud_r14_20260915/preflight.json').read_text(encoding='utf-8'))
    require(watcher_pin['candidate']['sha256']==EXPECTED,'watcher candidate mismatch')
    out.mkdir(parents=True)
    files={'d3d9.dll':dll,'README.md':ROOT/'docs/FRAME_HISTORY_CANDIDATE_README.md'}
    for name in ('frame_history_watch.py','frame_evidence_control.py','shadow_pose_full_trace_control.py',
                 'analyze_frame_evidence.py','analyze_frame_history.py','launch_frame_history_player.ps1'):
        files['AutoTest/'+name]=ROOT/'AutoTest'/name
    for relative in ('docs/agent-history/DEVELOPMENT_CHANGELOG.md','docs/RELEASE_NOTES_1.22.00_DRAFT.md',
                     'docs/plan/2026-09-15-v122-release-preparation-review.md',
                     'docs/plan/2026-09-14-frame-evidence-recorder.md','docs/research/2026-09-15-frame-evidence-schema.md',
                     'docs/agent-history/2026-09-15-frame-history-overnight.md'):
        files[relative]=ROOT/relative
    for path in (ROOT/'WarVK').rglob('*'):
        if path.is_file() and path.suffix.lower() in ('.j','.txt','.md'):files[path.relative_to(ROOT).as_posix()]=path
    for name in ('receipt.json','preflight.json','history-manifest.json','history-analysis.json','history-contact.png'):
        files['evidence/'+name]=receipt_path.with_name(name)
    files['evidence/window-shortcut.json']=receipt_path.with_name('window-shortcut.json')
    files['evidence/hud-complete-crop.png']=ROOT/'AutoTest/artifacts/frame_evidence_runs/shortcut_hud_r14_20260915/hud-complete-crop.png'
    files['docs/agent-history/2026-09-15-shortcut-and-one-second-fix.md']=ROOT/'docs/agent-history/2026-09-15-shortcut-and-one-second-fix.md'
    manifest={rel:verified_copy(src,out/rel) for rel,src in files.items()}
    metadata=dict(candidate=True,releaseReady=False,productAccepted=False,fissureFixed=False,
                  fullRecorderComplete=False,dllSha256=EXPECTED,baseCommit=git('rev-parse','HEAD').strip(),
                  branch=git('branch','--show-current').strip(),files=manifest)
    with (out/'manifest.json').open('x',encoding='utf-8') as f:json.dump(metadata,f,indent=2)
    candidate_identity=zip_verified(out,candidate_zip)
    source_zip=out.parent/(out.name+'-source-review.zip');require(not source_zip.exists(),'source zip exists')
    candidates=git('ls-files','-z','--cached','--others','--exclude-standard','--',
                   'src','include','subprojects/war3fx','subprojects/packagefiles','AutoTest','WarVK','docs','meson.build','meson_options.txt','build-win32.txt',
                   'build32_safe.cmd','.gitmodules','LICENSE','LICENSE*','README*','AGENTS.md').split('\0')
    source_manifest={};excluded=[]
    with zipfile.ZipFile(source_zip,'x',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for rel in sorted(set(candidates)):
            if not rel:continue
            path=ROOT/rel
            if not path.is_file() or path.suffix.lower() in ('.dll','.exe','.dmp','.mpq','.w3x','.w3m','.mdx','.i64','.idb','.zip','.7z'):
                excluded.append(rel);continue
            require(path.resolve().is_relative_to(ROOT.resolve()),'source escape')
            before=info(path);z.write(path,rel);require(info(path)==before,'source changed');source_manifest[rel]=before
        z.writestr('REVIEW_SNAPSHOT.json',json.dumps(dict(baseCommit=metadata['baseCommit'],branch=metadata['branch'],
            dirtyStatus=git('status','--porcelain'),submodules=git('ls-tree','-r','HEAD','subprojects'),files=source_manifest,
            excluded=excluded,releaseReady=False,note='Dirty review snapshot; external submodules and runtime evidence are not bundled.'),indent=2))
    with zipfile.ZipFile(source_zip) as z:
        for rel,expected in source_manifest.items():require(hashlib.sha256(z.read(rel)).hexdigest().upper()==expected['sha256'],'source archive hash')
    print(json.dumps(dict(candidateZip=str(candidate_zip),candidate=candidate_identity,
                          sourceReviewZip=str(source_zip),source=info(source_zip),sourceFiles=len(source_manifest)),indent=2))
if __name__=='__main__':main()
