#!/usr/bin/env python3
"""Arm/trigger/freeze/export the low-overhead frame evidence recorder."""
import argparse
import json
import sys
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'AutoTest'))
from shadow_pose_full_trace_control import _request

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--pid',type=int,required=True)
    p.add_argument('action',choices=('arm','status','trigger','freeze','export','discard'))
    p.add_argument('--session',required=False)
    p.add_argument('--capacity',type=int,default=8192)
    p.add_argument('--post-presents',type=int,default=8)
    p.add_argument('--timeout',type=float,default=8.0)
    a=p.parse_args()
    payload={'action':a.action}
    if a.session is not None: payload['session']=a.session
    if a.action=='arm': payload['capacity']=a.capacity
    if a.action=='trigger': payload['postPresents']=a.post_presents
    result=_request(a.pid,'frame_evidence',payload,a.timeout)
    print(json.dumps(result,ensure_ascii=False,indent=2))
    return 0 if result.get('ok') else 1
if __name__=='__main__':raise SystemExit(main())
