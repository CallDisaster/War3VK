"""Bounded local asset lookup by observed geometry shape; not loaded-model identity."""
import hashlib,json,struct
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT.parent/'dxvk/AutoTest/artifacts/native_model_lights_20260913'
OUT=ROOT/'AutoTest/artifacts/issue8_localized_20260915_history34548'
def inspect(p):
    b=p.read_bytes();u=lambda x:struct.unpack_from('<I',b,x)[0]
    if b[:4]!=b'MDLX':return []
    cursor=4;found=[]
    while cursor+8<=len(b):
        tag=b[cursor:cursor+4];n=u(cursor+4);start=cursor+8;end=start+n
        if end>len(b):return []
        if tag==b'GEOS':
            q=start;index=0
            while q<end:
                stop=q+u(q)
                if not q<stop<=end or b[q+4:q+8]!=b'VRTX':break
                vertices=u(q+8);r=q+12+vertices*12
                valid=True;indices=None
                for t,size in ((b'NRMS',12),(b'PTYP',4),(b'PCNT',4),(b'PVTX',2)):
                    if r+8>stop or b[r:r+4]!=t:valid=False;break
                    count=u(r+4)
                    if t==b'PVTX':indices=count
                    r+=8+count*size
                    if r>stop:valid=False;break
                if valid and vertices==614 and indices==1587:
                    found.append(dict(path=str(p),geoset=index,vertices=vertices,indices=indices,
                        bytes=len(b),sha256=hashlib.sha256(b).hexdigest().upper(),mountedIdentityVerified=False))
                q=stop;index+=1
        cursor=end
    return found
def main():
    count=0;matches=[]
    for archive in ('war3.mpq','War3x.mpq'):
        root=BASE/archive/'locale_0'
        # Known extracted model scope only, never scan user home/other projects.
        for p in root.rglob('*.mdx'):
            count+=1
            try:matches.extend(inspect(p))
            except (ValueError,struct.error):pass
    result=dict(scannedLocalModels=count,matches=matches,scope='offline extraction, no mounted identity proof')
    with (OUT/'shape-candidates.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    print(json.dumps(result,indent=2))
if __name__=='__main__':main()
