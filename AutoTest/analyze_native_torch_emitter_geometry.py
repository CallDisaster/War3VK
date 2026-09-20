"""Read-only pinned stock TorchHuman geometric self-occlusion evidence.

Not a complete MDX renderer: static opaque geoset only; no animation/alpha or
runtime caster claim. Runtime exact-owner comparison remains a separate gate.
"""
import hashlib
import json
from pathlib import Path
import struct
import math

def sub(a,b): return tuple(x-y for x,y in zip(a,b))
def dot(a,b): return sum(x*y for x,y in zip(a,b))
def cross(a,b): return (a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0])
def intersection(origin, endpoint, a,b,c):
    direction=sub(endpoint,origin); e1=sub(b,a); e2=sub(c,a)
    p=cross(direction,e2); det=dot(e1,p)
    if abs(det)<1e-9: return None
    t=sub(origin,a); u=dot(t,p)/det
    if not 0<=u<=1: return None
    q=cross(t,e1); v=dot(direction,q)/det
    if v<0 or u+v>1: return None
    distance=dot(e2,q)/det
    return distance if 0<distance<1 else None

def analyze(path):
    data=path.read_bytes(); sha=hashlib.sha256(data).hexdigest().upper()
    assert (len(data),sha) in (
        (4343,'98D1A8C63A13AC5C6FCD863EB0DF906ED096A6750941DEBC5F05CFE59BE81B6C'),
        # User-provided editable file, independently pinned; never rewrite it.
        (4351,'32059406BBEA8C2416EF03D5D1DAE6AA3A65D3CAB9A9304FB26C66A00072BCED'))
    u32=lambda offset:struct.unpack_from('<I',data,offset)[0]
    assert data[:4]==b'MDLX'
    chunks={}; offset=4
    while offset<len(data):
        tag=data[offset:offset+4]; size=u32(offset+4); start=offset+8
        assert tag not in chunks and start+size<=len(data)
        chunks[tag]=(start,size); offset=start+size
    start,length=chunks[b'GEOS']; end=start+u32(start)
    assert data[start+4:start+8]==b'VRTX'
    vertices=[struct.unpack_from('<3f',data,start+12+12*i) for i in range(u32(start+8))]
    p=data.index(b'PVTX',start,end); count=u32(p+4)
    assert count%3==0
    indices=struct.unpack_from('<'+'H'*count,data,p+8)
    assert max(indices)<len(vertices)
    # First opaque material/first geoset is the static wooden pole and cup.
    mats=data.index(b'MATS',start,end); material=u32(mats+8+4*u32(mats+4))
    assert material==0
    mtls=chunks[b'MTLS'][0]; assert u32(mtls+24)==0 # opaque layer filter mode
    light=struct.unpack_from('<3f',data,chunks[b'PIVT'][0]+2*12)
    rays=[]
    for radius in (50,100,150,175):
        for angle in range(0,360,15):
            radians=math.radians(angle); endpoint=(radius*math.cos(radians),radius*math.sin(radians),0)
            hits=[t for i in range(0,count,3) if (t:=intersection(light,endpoint,*[vertices[j] for j in indices[i:i+3]])) is not None]
            rays.append(dict(radius=radius,angle=angle,blocked=bool(hits),firstFraction=min(hits) if hits else None))
    return dict(path=str(path),size=len(data),sha256=sha,lightPivot=light,
                opaqueBounds=[[min(v[i] for v in vertices),max(v[i] for v in vertices)] for i in range(3)],
                rays=len(rays),blocked=sum(r['blocked'] for r in rays),samples=rays,
                runtimeProof=False)

if __name__=='__main__':
    root=Path(__file__).resolve().parents[2]
    path=root/'dxvk/AutoTest/artifacts/native_model_lights_20260913/War3x.mpq/locale_0/Doodads/LordaeronSummer/Props/TorchHuman/TorchHuman.mdx'
    print(json.dumps(analyze(path),indent=2))
