"""Read-only MDX800 geoset/material census for known local extraction candidates.

Archive variants are not proof of the mounted model in the player's process.
"""
import hashlib,json,struct
from pathlib import Path

BASE=Path(__file__).resolve().parents[2]/'dxvk/AutoTest/artifacts/native_model_lights_20260913'
OUT=Path(__file__).resolve().parents[1]/'AutoTest/artifacts/issue8_localized_20260915_history34548'
FILES=[BASE/'war3.mpq/locale_0/Units/Human/Footman/Footman.mdx',
       BASE/'War3x.mpq/locale_0/Units/Human/Phoenix/Phoenix.mdx',
       BASE/'War3x.mpq/locale_0/Units/Human/HeroBloodElf/HeroBloodElf.mdx']
def parse(path):
    data=path.read_bytes();u=lambda p:struct.unpack_from('<I',data,p)[0]
    assert data[:4]==b'MDLX'
    chunks={};p=4
    while p<len(data):
        tag=data[p:p+4];n=u(p+4);assert tag not in chunks and p+8+n<=len(data)
        chunks[tag]=(p+8,p+8+n);p+=8+n
    assert u(chunks[b'VERS'][0])==800
    mats=[];p,end=chunks[b'MTLS']
    while p<end:
        stop=p+u(p);assert p<stop<=end and data[p+12:p+16]==b'LAYS'
        layers=[];q=p+20
        for _ in range(u(p+16)):
            le=q+u(q);assert q<le<=stop
            layers.append(dict(filterMode=u(q+4),flags=u(q+8),textureId=u(q+12),
                               alpha=struct.unpack_from('<f',data,q+24)[0]));q=le
        assert q==stop;mats.append(layers);p=stop
    geos=[];p,end=chunks[b'GEOS']
    while p<end:
        stop=p+u(p);assert p<stop<=end;q=p+4;counts={};arrays={}
        for tag,stride in ((b'VRTX',12),(b'NRMS',12),(b'PTYP',4),(b'PCNT',4),(b'PVTX',2),(b'GNDX',1),(b'MTGC',4),(b'MATS',4)):
            assert data[q:q+4]==tag,(path,q,tag,data[q:q+4])
            n=u(q+4);assert q+8+n*stride<=stop;counts[tag.decode()]=n;arrays[tag]=data[q+8:q+8+n*stride];q+=8+n*stride
        material=u(q);assert material<len(mats)
        verts=list(struct.iter_unpack('<3f',arrays[b'VRTX']))
        geos.append(dict(index=len(geos),vertices=counts['VRTX'],indices=counts['PVTX'],
                         groupCount=counts['MTGC'],boneRefs=counts['MATS'],material=material,layers=mats[material],
                         bounds=[[min(v[i] for v in verts),max(v[i] for v in verts)] for i in range(3)]))
        p=stop
    return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest().upper(),geosets=geos,
                mountedIdentityVerified=False)
def main():
    result=[parse(path) for path in FILES]
    with (OUT/'local-model-geosets.json').open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    for r in result:print(json.dumps(r,ensure_ascii=False))
if __name__=='__main__':main()
