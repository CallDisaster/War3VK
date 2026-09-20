"""Adopt external numerical/source review with immutable, bounded local reruns.
No C++ edits, build, game, deployment or network. Outputs go to a new audit dir.
"""
import hashlib
import json
import os
from pathlib import Path,PurePosixPath
import stat
import subprocess
import sys
import zipfile
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'AutoTest/artifacts/independent_review_adoption_20260915'
ORIGINAL=Path('C:/Users/Administrator/Desktop/WarVK-53372-VIDEO25-29-Research-20260915.zip')
EXTERNAL=Path('C:/Users/Administrator/Downloads/WarVK-independent-audit-20260915.zip')
REPORT=Path('C:/Users/Administrator/Downloads/WarVK-independent-review-ZH.md')
def require(ok,message):
    if not ok:raise ValueError(message)
def sha(data):return hashlib.sha256(data).hexdigest().upper()
def identity(path):
    with path.open('rb') as f:h=hashlib.file_digest(f,'sha256').hexdigest().upper()
    return dict(size=path.stat().st_size,sha256=h)
def dump(path,data):
    with path.open('x',encoding='utf-8') as f:json.dump(data,f,indent=2,allow_nan=False)
def unpack(archive,target):
    require(not target.exists(),'unpack target exists')
    with zipfile.ZipFile(archive) as z:
        require(z.testzip() is None,'ZIP CRC')
        names=set();size=0
        for member in z.infolist():
            p=PurePosixPath(member.filename);size+=member.file_size
            require(not p.is_absolute() and '..' not in p.parts and ':' not in member.filename and '\\' not in member.filename,'unsafe archive path')
            require(p.as_posix().casefold() not in names,'duplicate/case alias path');names.add(p.as_posix().casefold())
            require(not stat.S_ISLNK(member.external_attr>>16),'archive symlink')
        require(size<1024**3,'unexpected unpack size')
        target.mkdir()
        for member in z.infolist():
            path=target/Path(member.filename)
            require(path.resolve().is_relative_to(target.resolve()),'archive escape')
            if member.is_dir():path.mkdir(parents=True,exist_ok=True);continue
            path.parent.mkdir(parents=True,exist_ok=True)
            with z.open(member) as src,path.open('xb') as dst:
                while data:=src.read(1024*1024):dst.write(data)
def compare(a,b,path='root'):
    if isinstance(a,dict):
        require(isinstance(b,dict) and set(a)==set(b),'JSON keys '+path)
        for k in a:compare(a[k],b[k],path+'.'+k)
    elif isinstance(a,list):
        require(isinstance(b,list) and len(a)==len(b),'JSON length '+path)
        for i,(x,y) in enumerate(zip(a,b)):compare(x,y,path+f'[{i}]')
    elif isinstance(a,float):
        require(isinstance(b,(int,float)) and np.isclose(a,b,atol=1e-9,rtol=1e-12),'numeric drift '+path)
    else:require(a==b,'value drift '+path)
def main():
    require(not BASE.exists(),'CreateNew audit dir required')
    original_pins={str(p):identity(p) for p in (ORIGINAL,EXTERNAL,REPORT)}
    BASE.mkdir();pkg=BASE/'original-package';audit=BASE/'external-audit';out=BASE/'rerun';out.mkdir()
    unpack(EXTERNAL,audit)
    sums=json.loads((audit/'SHA256SUMS.json').read_text(encoding='utf-8'))
    require(original_pins[str(ORIGINAL)]['sha256']==sums['inputArchiveSHA256'],'original archive identity')
    for name,pin in sums['files'].items():require(identity(audit/name)==pin,'external member hash '+name)
    require(REPORT.read_bytes()==(audit/REPORT.name).read_bytes(),'separate report differs')
    unpack(ORIGINAL,pkg)
    env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1',PYTHONIOENCODING='utf-8')
    print('ZIP/member/report identities verified. Rerunning reviewed offline tools.',flush=True)
    # External verify_integrity.py uses str(relative_path), which emits backslashes
    # on Windows and falsely labels POSIX-named manifest members as extras. Do not
    # edit that supplied script; perform the same manifest checks natively here.
    manifest=json.loads((pkg/'PACKAGE_MANIFEST.json').read_text(encoding='utf-8'))
    source=json.loads((pkg/'SOURCE_PROVENANCE.json').read_text(encoding='utf-8'))
    for name,pin in manifest['files'].items():require(identity(pkg/name)==pin,'source member '+name)
    found={p.relative_to(pkg).as_posix() for p in pkg.rglob('*') if p.is_file()}
    require(found==set(manifest['files'])|{'PACKAGE_MANIFEST.json'},'unexpected unpacked members')
    current_pins={}
    for rel in ('src/d3d9/d3d9_device.cpp','src/d3d9/war3/model/war3_model_hook.cpp',
                'src/d3d9/war3/render/war3_current_draw_contract.cpp','src/d3d9/war3/render/war3_canonical_draw.cpp'):
        require((ROOT/rel).read_bytes()==(pkg/'source'/rel).read_bytes(),'current source drift '+rel)
        current_pins[rel]=identity(ROOT/rel)
    commands=[
        [sys.executable,'-B',str(pkg/'tools/analyze_frame_inputs.py'),'--manifest',str(pkg/'evidence/inputs-selected.json'),
         '--binary',str(pkg/'evidence/inputs-selected.bin'),'--cpu',str(pkg/'evidence/cpu-events-selected.json'),'--output',str(out/'recomputed-new.json')],
        [sys.executable,'-B',str(pkg/'tools/verify_marked_skinning_scalar_53372.py'),'--input-dir',str(pkg/'evidence'),'--check-only'],
        [sys.executable,'-B',str(audit/'reaudit.py'),'--root',str(pkg),'--out',str(out)]]
    for name,command in zip(('reader','scalar','independent'),commands):
        with (out/(name+'-run.txt')).open('x',encoding='utf-8') as log:
            result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=env,cwd=ROOT)
        require(result.returncode==0,name+' rerun failed; see retained log')
    supplied=json.loads((audit/'independent-audit.json').read_text(encoding='utf-8'))
    repeated=json.loads((out/'independent-audit.json').read_text(encoding='utf-8'))
    compare(supplied,repeated)
    compare(json.loads((audit/'recomputed-new.json').read_text()),json.loads((out/'recomputed-new.json').read_text()))
    with np.load(audit/'focus-corners.npz',allow_pickle=False) as a,np.load(out/'focus-corners.npz',allow_pickle=False) as b:
        require(set(a.files)==set(b.files),'array names differ');max_error=0
        for name in a.files:
            require(a[name].shape==b[name].shape,'array shape')
            require(np.allclose(a[name],b[name],atol=1e-9,rtol=1e-12),'array drift '+name)
            max_error=max(max_error,float(np.max(np.abs(a[name]-b[name]))))
        array_count=len(a.files)
    for name,pin in manifest['files'].items():require(identity(pkg/name)==pin,'original input mutated '+name)
    for path,pin in original_pins.items():require(identity(Path(path))==pin,'download/archive changed')
    result=dict(externalArtifacts=original_pins,externalMemberChecks=len(sums['files']),originalMemberChecks=len(manifest['files']),
        numericalJsonEquivalent=True,comparisonAtol=1e-9,comparisonRtol=1e-12,arrayCount=array_count,maxArrayAbsError=max_error,
        focusBadRows=repeated['focusBadRows'],indexedBadRows=repeated['skinnedBadRows'],consumerPairs=len(repeated['pairs']),
        maxConsumerAbs=max(p['maxAbs'] for p in repeated['pairs']),maxScalarAbs=repeated['maxScalarAbs'],
        scalarCorners=repeated['scalarCheckedCorners'],group8Matches=len(repeated['group8ExactMatches']),
        correspondingEdges=repeated['maxEdgeChanges'],currentSourceMatches=current_pins,
        sourceUnchanged=True,productSourceModified=False,buildRun=False,gameRun=False,rootCauseReady=False,
        portabilityNote='External verify_integrity.py Windows str(path) separator issue; equivalent POSIX-normalized manifest checks run by this wrapper, supplied script untouched.')
    dump(BASE/'adoption-receipt.json',result)
    print(json.dumps({k:v for k,v in result.items() if k not in ('externalArtifacts','currentSourceMatches','correspondingEdges')},indent=2))
if __name__=='__main__':main()
