"""Strict new-report wall ledger analysis; no inferred CPU/optimization claims."""
import argparse,json,math
from pathlib import Path
from analyze_data_collection_perf_report import load
def analyze(path,last=2000):
 d,duplicates,h=load(path)
 if duplicates:raise ValueError('duplicate JSON keys: '+str([v['path'] for v in duplicates]))
 t=d['mainThreadTimeline'];rows=t['frames'][-last:];freq=t['frequency'];names=t['buckets']
 if not t['enabled'] or not rows or t['otherPresents'] or freq<=0:raise ValueError('owner/trace unavailable')
 previous=None
 for r in rows:
  start=int(r['start']);end=int(r['end'])
  if r['faults'] or end<=start or r['result']!=0:raise ValueError('fault/failed present')
  if previous is not None and previous!=start:raise ValueError('non-contiguous frames')
  previous=end
  if sum(r['self'])!=end-start or sum(r['legacySelf'])!=r['legacyTicks']:raise ValueError('integer closure failed')
  for k in ['self','inclusive','legacySelf','calls']:
   if len(r[k])!=len(names) or any(not isinstance(x,int) or x<0 for x in r[k]):raise ValueError('invalid bucket')
  if any(x>end-start for x in r['inclusive']):raise ValueError('inclusive union overflow')
 factor=1000/freq/len(rows)
 wall=sum(int(r['end'])-int(r['start']) for r in rows)*factor
 buckets=[{'name':name,'selfMs':sum(r['self'][i] for r in rows)*factor,
  'inclusiveMs':sum(r['inclusive'][i] for r in rows)*factor,
  'legacyWindowSelfMs':sum(r['legacySelf'][i] for r in rows)*factor,
  'callsPerFrame':sum(r['calls'][i] for r in rows)/len(rows)} for i,name in enumerate(names)]
 return {'report':str(path),'sha256':h,'strictJsonValid':True,'productAccepted':False,
  'threadId':t['threadId'],'frames':len(rows),'startQpc':rows[0]['start'],'endQpc':rows[-1]['end'],
  'qpcFrequency':freq,'fullPresentIntervalMs':wall,'applicationPresentCadenceFps':1000/wall,
  'legacyWindowIntersectionMs':sum(r['legacyTicks'] for r in rows)*factor,
  'threadCpuWindowMsPerFrame':sum(r['cpu100ns'] for r in rows)/10000/len(rows),
  'oldReportFrameCount':d['frameCount'],'oldReportFrameMs':d.get('avgFrameTimeMs'),
  'oldReportUncoveredMs':d.get('avgUncoveredFrameWallMs'),
  'oldReportDifferentWindow':True,'integerClosure':True,
  'buckets':sorted(buckets,key=lambda b:b['selfMs'],reverse=True),'meta':d.get('meta',{})}
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('report',type=Path);p.add_argument('--output',type=Path,required=True);p.add_argument('--last',type=int,default=2000);a=p.parse_args()
 r=analyze(a.report,a.last)
 with a.output.open('x',encoding='utf-8') as f:json.dump(r,f,indent=2,ensure_ascii=False,allow_nan=False)
 print(json.dumps({k:v for k,v in r.items() if k not in ['meta']},indent=2,ensure_ascii=False))
