"""Synthetic schema/tamper coverage, separate from real process receipts."""
import copy
import struct
import unittest

import analyze_recorder_offload_e2 as a


def memory(private=16*1024**2):
    return dict(valid=True, win32=0, privateBytes=private, peakPrivateBytes=private,
                workingSetBytes=private, usedVirtualBytes=private+16*1024**2,
                privateVirtualBytes=private, mappedVirtualBytes=262144, regions=100)


def fixture(mode='small'):
    cap = 262144 if mode == 'normal' else 8192
    total, cut, evicted = cap+160, cap-cap//4+160, 160
    p = dict.fromkeys(a.PU, 0); p.update(dict.fromkeys(a.PB, False))
    p.update(mode=mode, bits=32, nonce='0123456789abcdef'*2, map=11, device=17,
             session=a.SESSION, capacity=cap, attempted=total, accepted=total, popped=total,
             ackedEvents=total, trigger=cut, triggerRequestedQpc=10, triggerAppliedQpc=11,
             qpcFrequency=1000, producerStartQpc=1, producerEndQpc=20, workerJoined=True,
             hostSettled=True, containersGone=True, hostPid=10, hostCreation=12345,
             completed=True, ingressBytes=3670400, poolBytes=262144, handlesBefore=5, handlesAfter=5,
             sentPackets=(total+159)//160+2, ackPackets=(total+159)//160+2,
             memoryBefore=memory(), memoryActive=memory(20*1024**2), memoryAfter=memory())
    h = dict.fromkeys(a.HU, 0); h.update(dict.fromkeys(a.HB, False))
    h.update(recorderHost=True, bits=64, mode='normal', session=a.SESSION, state=2,
             nextOrdinal=61, lastSequence=total, trigger=cut, capacity=cap, accepted=total,
             attempted=total, evicted=evicted, retained=cap, reason=1, storageBytes=cap*392,
             beginSeen=True, endSeen=True, closeSeen=True, visited=cap, firstSequence=161,
             lastVisited=total, contentValid=True, cpuComplete=True, qpcFrequency=1000,
             memoryBefore=memory(), memoryAllocated=memory(16*1024**2+cap*392),
             memoryActive=memory(16*1024**2+cap*392), digest=a.expected_digest(161,total))
    t = dict.fromkeys(a.TU, 0); t.update(dict.fromkeys(a.TB, True))
    t.update(bits=64, state='closed', reason='None', fault='none', stage='none', poolBytes=262144, viewProtection=2)
    if mode == 'slow':
        p.update(accepted=8352, attempted=20160, lost=11808, popped=8352, ackedEvents=8352,
                 trigger=8352, producerStartQpc=100, producerEndQpc=200, triggerRequestedQpc=201, triggerAppliedQpc=301)
        h.update(mode='slow', accepted=8352, attempted=20160, lost=11808, lastSequence=8352,
                 trigger=8352, evicted=2208, retained=6144, visited=6144, firstSequence=2209,
                 lastVisited=8352, digest=a.expected_digest(2209,8352), cpuComplete=False,
                 delayStartQpc=90, delayEndQpc=300)
    elif mode in a.REASONS:
        p.update(fault=True, completed=False, hostExit=2, attempted=640, accepted=640, popped=640,
                 ackedEvents=640, sentPackets=6, ackPackets=5, trigger=640)
        h.update(runtimeExit=2, state=1, nextOrdinal=6, accepted=640, lastSequence=640,
                 attempted=0, lost=0, retained=640, evicted=0, trigger=a.U64, reason=0,
                 visited=0, firstSequence=0, lastVisited=0, digest='', contentValid=False,
                 cpuComplete=False, endSeen=False, closeSeen=False)
        t.update(state='fault', reason=a.REASONS[mode])
        if mode == 'disconnect':
            p.update(attempted=480,accepted=480,popped=480,ackedEvents=320,ackPackets=3,sentPackets=4)
            h.update(mode='disconnect',accepted=320,lastSequence=320,retained=320)
        if mode in ('wrong-ordinal','wrong-session','wrong-scope','bad-kind'):
            p.update(attempted=160,accepted=160,popped=160,ackedEvents=0,ackPackets=1,sentPackets=2)
            h.update(accepted=0,lastSequence=0,retained=0)
        if mode=='missing-seal': p.update(sentPackets=5)
        if mode in ('late-data','seal-totals'): h.update(state=3,storeError=2 if mode=='late-data' else 13)
        if mode=='late-data':
            p.update(ackPackets=6,sentPackets=7)
            h.update(attempted=640,trigger=640,reason=1)
        if mode == 'bad-kind': h['wireError'] = 15
        if a.REASONS[mode] == 'HelloContract':
            p.update(attempted=0, accepted=0, popped=0, ackedEvents=0, sentPackets=0, ackPackets=0)
            t['viewProtection'] = 0
            h.update(state=0, session=0, capacity=0, storageBytes=0, accepted=0, lastSequence=0,
                     retained=0, nextOrdinal=1, beginSeen=False, memoryAllocated=memory())
            if mode.startswith('e2-to-'): h=None
    if h is not None: h['nextOrdinal']=p['ackPackets']+1
    good=mode in ('normal','small','slow');mismatch=mode in a.REASONS and a.REASONS[mode]=='HelloContract'
    controls=4+2*p['ackPackets'] if good else 0 if mismatch else 2+2*p['ackPackets'] if mode=='missing-seal' else 3+2*p['ackPackets']
    t.update(accepted=controls,writes=controls,copies=p['sentPackets'],
             copiedBytes=80*p['sentPackets']+392*(p['popped']+int(mode=='late-data')),
             reads=2*controls-2 if good else 2*controls+1 if mode=='missing-seal' else 2*(controls+1))
    return p,h,t


class TestE2(unittest.TestCase):
    def verdict(self, triple, ok=True):
        result=a.analyze_case(*triple)
        self.assertEqual(result['ok'],ok,result)
        self.assertFalse(result['gameMemoryBenefitMeasured'])
        if not ok:self.assertFalse(result['cpuComplete'])
        return result

    def test_all_modes(self):
        for mode in ['normal','small','slow',*a.REASONS]:
            with self.subTest(mode=mode):
                result=self.verdict(fixture(mode));self.assertEqual(result['cpuComplete'],mode in ('normal','small'))

    def test_missing_required_and_types(self):
        for index, ints, bools, strings in [(0,a.PU,a.PB,['mode','nonce']), (1,a.HU,a.HB,['mode','digest']),
                                          (2,a.TU,a.TB,['state','reason','fault','stage'])]:
            for key in ints+bools+strings:
                with self.subTest(index=index,key=key):
                    x=fixture();del x[index][key];self.verdict(x,False)
            for key in ints:
                for value in [True,-1,1.0,1<<64]:
                    x=fixture();x[index][key]=value;self.verdict(x,False)
            for key in bools:
                x=fixture();x[index][key]=1;self.verdict(x,False)

    def test_counter_identity_and_terminal_tamper(self):
        changes=[(0,'session',7),(0,'map',12),(0,'device',18),(0,'bits',64),(0,'nonce','f'*31),
                 (0,'ingressBytes',100000000),(0,'poolBytes',100000000),(0,'accepted',1),
                 (0,'queued',1),(0,'ackedEvents',9000),(0,'workerJoined',False),(0,'hostSettled',False),
                 (0,'containersGone',False),(0,'handlesAfter',6),(0,'hostExit',2),(0,'fault',True),
                 (0,'triggerAppliedQpc',0),(0,'producerEndQpc',1),
                 (1,'digest','0'*64),(1,'lastVisited',10),(1,'firstSequence',1),(1,'visited',1),
                 (1,'storageBytes',1),(1,'nextOrdinal',9999),(1,'endSeen',False),(1,'closeSeen',False),
                 (1,'retained',1),(1,'reason',0),(1,'state',1),(1,'cpuComplete',False),
                 (2,'viewProtection',4),(2,'peerVerified',False),(2,'abandoned',1),
                 (2,'cancelRequests',1),(2,'cancelCompletions',1),(2,'fault','unexpected'),(2,'win32',8)]
        changes += [(2,k,0) for k in ('accepted','copies','copiedBytes','reads','writes')]
        for index,key,value in changes:
            with self.subTest(key=key):
                x=fixture();x[index][key]=value;self.verdict(x,False)

    def test_memory_and_boundaries(self):
        for side, names in [(0,['memoryBefore','memoryActive','memoryAfter']), (1,['memoryBefore','memoryAllocated','memoryActive'])]:
            for name in names:
                for key in ['valid','win32',*a.MEM]:
                    x=fixture();del x[side][name][key];self.verdict(x,False)
                x=fixture();x[side][name]['valid']=False;self.verdict(x,False)
        x=fixture();x[0]['memoryActive']=memory(32*1024**2);self.verdict(x)
        x[0]['memoryActive']=memory(32*1024**2+1);self.verdict(x,False)
        x=fixture();x[0]['memoryAfter']=memory(24*1024**2+1);self.verdict(x,False)
        x=fixture('normal');x[1]['memoryAllocated']=memory(16*1024**2+x[1]['storageBytes']-2*1024**2);self.verdict(x)
        x[1]['memoryAllocated']['privateBytes']-=1;self.verdict(x,False)
        x=fixture();x[0]['memoryAfter']=memory(8*1024**2);self.verdict(x) # signed delta, no wrap/clamp

    def test_failed_and_loss_never_complete(self):
        for mode in ['slow',*a.REASONS]:
            x=fixture(mode)
            if x[1] is not None:
                x[1]['cpuComplete']=True;self.verdict(x,False)
                x=fixture(mode);x[1]['contentValid']=True
                if mode!='slow':self.verdict(x,False)
        x=fixture('slow');x[0]['producerStartQpc']=89;self.verdict(x,False)
        x=fixture('slow');x[0]['producerEndQpc']=301;self.verdict(x,False)
        x=fixture('slow');x[1]['qpcFrequency']=1;self.verdict(x,False)
        x=fixture('p2-to-e2');x[1]['storageBytes']=1;self.verdict(x,False)

    def test_json_fail_closed(self):
        for text in ['{"a":1,"a":2}', '{"a":{"b":1,"b":2}}','{"a":NaN}',
                     '{"a":Infinity}','{"a":-Infinity}','{"a":1e999}']:
            with self.subTest(text=text),self.assertRaises(ValueError):a.strict_json(text)
        x=fixture();x[0]['extra']=[{'bad':float('nan')}];self.verdict(x,False)
        root=dict(schema=1,scope='RECORDER_OFFLOAD_E2_CPU_LAB',cases=[dict(zip(['parent','host','transport'],fixture()))])
        self.assertTrue(a.analyze_receipt(root)['ok'])
        for key,value in [('schema',True),('scope','GAME'),('cases',[])]:
            b=copy.deepcopy(root);b[key]=value
            with self.assertRaises(ValueError):a.analyze_receipt(b)
        b=copy.deepcopy(root);b['cases'].append(copy.deepcopy(b['cases'][0]))
        with self.assertRaises(ValueError):a.analyze_receipt(b)
        x=fixture();self.verdict((x[0],None,x[2]),False)

    def test_explicit_binary_layout(self):
        event=a.event_bytes(161);self.assertEqual(len(event),392)
        self.assertEqual(struct.unpack_from('<8QII',event),(161,a.SESSION,161,0x12340000000000a1,
                         0x98760000000000a1,1,11,17,5,1))
        self.assertEqual(event[72:104],b'recorder-e2\0'+b'\0'*20)
        self.assertEqual(struct.unpack_from('<3I',event,200),(0x7f800000,0x7fc12345,0x80000000))


if __name__=='__main__':unittest.main()
