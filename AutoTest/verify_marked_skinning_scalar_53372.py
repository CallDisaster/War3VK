"""Independent scalar verification of the captured legacy 0-weight shader path.
Does not call the main numpy reconstruction implementation.
"""
import json
import argparse
import math
from pathlib import Path
import struct
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'AutoTest/artifacts/marked_skinning_53372_25_29_20260915'

def row_mul(v,m):return [sum(v[i]*m[i*4+j] for i in range(4)) for j in range(4)]
def decode_draw(d,parts):
    w=d['words'];stride=w[4];count=w[8]
    assert w[6]==106 and w[33]==1 and len(parts[0])==count*(16+stride)
    assert w[15]==0 or (w[16]==1 and w[17]==0 and w[19]==1 and w[20]==16 and w[23]==12 and w[24]==39)
    index_size=2 if w[7]==0 else 4
    index_fmt='<H' if w[7]==0 else '<I'
    offset=w[10] if w[10]<2**31 else w[10]-2**32
    results=[];used=set();groups=[]
    for corner in range(count):
        start=corner*(16+stride)
        raw,vertex,valid,reserved=struct.unpack_from('<4I',parts[0],start)
        expected=struct.unpack_from(index_fmt,parts[1],(w[9]+corner)*index_size)[0]
        assert raw==expected and vertex==expected+offset and valid==1 and reserved==0
        xyz=struct.unpack_from('<3f',parts[0],start+16+w[5]);position=(*xyz,1.0)
        group=0
        if w[15]:
            record_size=16+w[20]
            assert len(parts[2])==count*record_size
            braw,bvertex,bvalid,breserved=struct.unpack_from('<4I',parts[2],corner*record_size)
            assert (braw,bvertex,bvalid,breserved)==(raw,vertex,1,0)
            group=parts[2][corner*record_size+16+w[23]]
        assert group<256
        matrix=struct.unpack_from('<16f',parts[4],group*64)
        result=row_mul(position,matrix)
        assert all(math.isfinite(x) for x in result)
        used.add(group);groups.append(group);results.append(result)
    return np.asarray(results),sorted(used),groups

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input-dir',type=Path,default=OUT)
    parser.add_argument('--check-only',action='store_true')
    args=parser.parse_args();directory=args.input_dir
    manifest=json.loads((directory/'inputs-selected.json').read_text(encoding='utf-8'))
    binary=(directory/'inputs-selected.bin').read_bytes()
    reference=np.load(directory/'reconstructed-triangle-corners.npz',allow_pickle=False)
    checks=[];parameters=[]
    for batch in manifest['batches']:
        video=int(batch['frame'])-2163
        for d in batch['draws']:
            w=d['words'];name=f'v{video}_vol{int(batch["volume"])}_part{d["part"]}_h{w[1]}_layer{w[3]}_n{w[8]}'
            if name not in reference or w[8]!=105:continue
            parts=[binary[int(batch['fileOffset'])+s['offset']:int(batch['fileOffset'])+s['offset']+int(s['bytes'])] if s['status']==1 else b'' for s in d['spans']]
            scalar,groups,corners=decode_draw(d,parts);expected=reference[name]
            maximum=float(np.abs(scalar-expected).max())
            assert scalar.shape==expected.shape and maximum<1e-9
            checks.append(dict(video=video,volume=batch['volume'],handle=w[1],part=d['part'],stride=w[4],corners=len(scalar),maxAbsAgainstNumpy=maximum,groups=groups))
            if video in (20,22,24,25,29,30) and not batch['volume']:
                parameters.append(dict(video=video,handle=w[1],part=d['part'],batch=batch['id'],draw=d['index'],stride=w[4],
                    rawWords=w,provenance=d['provenance'],groupPerTriangleCorner=corners,
                    selectedMatrices={str(g):list(struct.unpack_from('<16f',parts[4],g*64)) for g in groups}))
    result=dict(independentScalar=True,checkedDraws=len(checks),maxAbs=max(x['maxAbsAgainstNumpy'] for x in checks),
                checkedTriangleCorners=sum(x['corners'] for x in checks),checks=checks,
                limitation='Scalar mathematical reproduction, not an actual GPU vertex-shader output capture.')
    if not args.check_only:
        for name,value in (('scalar-verification.json',result),('selected-skinning-parameters.json',parameters)):
            with (directory/name).open('x',encoding='utf-8') as f:json.dump(value,f,indent=2,allow_nan=False)
    print({k:v for k,v in result.items() if k!='checks'})
if __name__=='__main__':main()
