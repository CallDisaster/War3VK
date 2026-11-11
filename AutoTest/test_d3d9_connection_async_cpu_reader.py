"""Pure X1a CPU-output reader tests; no executable, compiler, Desktop, or GPU.

Synthetic witness strings test the real formal reader. They are not runtime
identity evidence. This file imports no artifacts and can move into AutoTest.
"""
import ast
import copy
import inspect
import json
from pathlib import Path
import sys
import unittest

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p/'AutoTest/run_d3d9_connection_async_cpu.py').is_file())
sys.path.insert(0, str(ROOT/'AutoTest'))
import run_d3d9_connection_async_cpu as m

SCHEMA = 'warvk-x1a-cpu-desktop/v1'
DESKTOP = 'WarVK-CPU-Test'
INPUT = 'Default'
LAUNCH = dict(pid=1234, initialThreadId=77)
COMPLETE = 'X1A_CPU checks=512 ipcTested=0 gpuTested=0 productOwnerComplete=0'


def witnesses(bits=32):
    owner = dict(schema=SCHEMA, role='owner', pid=LAUNCH['pid'], tid=LAUNCH['initialThreadId'],
        bits=bits, desktop=DESKTOP, inputDesktop=INPUT,
        isolatedDesktop='WinSta0\\'+DESKTOP, gpuTested=False)
    result = [owner]
    for number, name in enumerate(sorted(m.WORKERS), start=1):
        result.append(dict(owner, role='worker', case=name, tid=77+number))
    return result


def transcript(rows=None, complete=COMPLETE, trailing=True):
    rows = witnesses() if rows is None else rows
    lines = [json.dumps(row, separators=(',', ':')) for row in rows]
    if complete is not None:
        lines.append(complete)
    return '\n'.join(lines)+ ('\n' if trailing else '')


class CpuWitnessReaderTests(unittest.TestCase):
    def parse(self, text=None, bits=32, desktop=DESKTOP, input_desktop=INPUT, launch=None):
        return m.read_result(transcript(witnesses(bits)) if text is None else text,
            LAUNCH if launch is None else launch, desktop, input_desktop, bits)

    def rejected(self, text, **kwargs):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            self.parse(text, **kwargs)

    def test_actual_32_and_64_witness_types(self):
        for bits in (32,64):
            with self.subTest(bits=bits):
                result=self.parse(bits=bits)
                self.assertEqual(result['checks'],512)
                self.assertEqual(result['desktopWitnesses'],witnesses(bits))
                self.assertFalse(any(row.get('gpuTested') for row in result['desktopWitnesses']))

    def test_sequential_worker_tid_reuse_is_legal(self):
        for bits in (32,64):
            rows=witnesses(bits)
            for row in rows[1:]:row['tid']=99
            self.assertEqual(self.parse(transcript(rows),bits=bits)['checks'],512)

    def test_worker_cannot_claim_live_owner_tid(self):
        for index in range(1,len(m.WORKERS)+1):
            rows=witnesses();rows[index]['tid']=LAUNCH['initialThreadId']
            with self.subTest(index=index):self.rejected(transcript(rows))

    def test_real_owner_launch_identity_required(self):
        for key,value in (('tid',78),('pid',1235),('role','worker')):
            rows=witnesses();rows[0][key]=value
            with self.subTest(key=key):self.rejected(transcript(rows))

    def test_worker_identity_and_numeric_types_required(self):
        for key,value in (('pid',0),('pid',True),('pid',1235),('tid',0),('tid',True),
            ('tid',2**32),('bits',64),('bits',True),('bits','32')):
            rows=witnesses();rows[1][key]=value
            with self.subTest(key=key,value=value):self.rejected(transcript(rows))

    def test_reader_only_accepts_actual_supported_bitness(self):
        for bits in (0,16,128):
            with self.subTest(bits=bits):self.rejected(transcript(witnesses(bits)),bits=bits)

    def test_cross_bitness_transcript_rejected(self):
        self.rejected(transcript(witnesses(32)),bits=64)
        self.rejected(transcript(witnesses(64)),bits=32)

    def test_case_insensitive_same_desktop_is_not_isolated(self):
        for alias in (DESKTOP,DESKTOP.lower(),DESKTOP.upper()):
            rows=witnesses()
            for row in rows:row['inputDesktop']=alias
            with self.subTest(alias=alias):self.rejected(transcript(rows),input_desktop=alias)

    def test_default_desktop_always_rejected(self):
        for desktop in ('Default','default','DEFAULT'):
            rows=witnesses()
            for row in rows:
                row['desktop']=desktop;row['isolatedDesktop']='WinSta0\\'+desktop
            with self.subTest(desktop=desktop):self.rejected(transcript(rows),desktop=desktop)

    def test_actual_desktop_station_and_input_witnesses_required(self):
        for key,value in (('desktop','Wrong'),('inputDesktop','Wrong'),
            ('isolatedDesktop','OtherStation\\'+DESKTOP),('gpuTested',True)):
            rows=witnesses();rows[2][key]=value
            with self.subTest(key=key):self.rejected(transcript(rows))

    def test_exact_worker_population_not_vacuous(self):
        for missing in range(1,len(m.WORKERS)+1):
            rows=witnesses();rows.pop(missing)
            with self.subTest(missing=missing):self.rejected(transcript(rows))
        self.rejected(transcript(witnesses()+[copy.deepcopy(witnesses()[1])]))
        rows=witnesses();rows[1]['case']=rows[2]['case'];self.rejected(transcript(rows))
        rows=witnesses();rows[1]['case']='made-up';self.rejected(transcript(rows))
        rows=witnesses();rows[1]['role']='owner';self.rejected(transcript(rows))

    def test_no_foreign_or_contradictory_witness_fields(self):
        for index,key,value in ((0,'case','fake-owner-case'),(1,'ipcTested',True),
            (2,'productOwnerComplete',True),(3,'extra',0)):
            rows=witnesses();rows[index][key]=value
            with self.subTest(key=key):self.rejected(transcript(rows))

    def test_duplicate_json_keys_are_rejected(self):
        text=transcript().replace('"bits":32','"bits":64,"bits":32',1)
        self.rejected(text)
        text=transcript().replace('"gpuTested":false','"gpuTested":true,"gpuTested":false',1)
        self.rejected(text)

    def test_schema_not_found_by_nested_substring(self):
        rows=witnesses();rows[0]['schema']='wrong-schema';rows[0]['nested']={'schema':SCHEMA}
        self.rejected(transcript(rows))

    def test_malformed_extra_witness_never_silently_ignored(self):
        lines=transcript().splitlines()
        for extra in ('{"schema":"'+SCHEMA+'","role":\n',
            '{"schema": "'+SCHEMA+'", "role": "extra"}\n',
            'prefix {"schema":"'+SCHEMA+'","role":"extra"}\n'):
            with self.subTest(extra=extra):self.rejected('\n'.join(lines[:-1])+'\n'+extra+lines[-1]+'\n')

    def test_success_summary_must_be_unique_and_after_all_witnesses(self):
        self.rejected(COMPLETE+'\n'+transcript(complete=None))
        self.rejected(transcript()+COMPLETE+'\n')
        self.rejected(transcript(complete=None))

    def test_summary_never_promotes_fake_replies_to_ipc_gpu_or_product(self):
        for field in ('ipcTested','gpuTested','productOwnerComplete'):
            with self.subTest(field=field):self.rejected(transcript(complete=COMPLETE.replace(field+'=0',field+'=1')))
        for count in ('0','1','99','299','-1','1.5'):
            with self.subTest(count=count):self.rejected(transcript(complete=COMPLETE.replace('checks=512','checks='+count)))

    def test_fixed_notification_worker_cases(self):
        self.assertEqual(m.WORKERS, {'readback-backpressure', 'clear-failure-waiting-barrier',
            'draw-own-reply', 'slot-release-before-ack', 'empty-close', 'empty-fault',
    'stale-hint-then-fault', 'settled-reply-before-fault', 'bounded-draw-batch','gpu-group-end-scene','bounded-fixed-batch',
    'annotated-draw-batch'})

    def test_notification_worker_identity_cannot_be_borrowed(self):
        for index in range(1,len(m.WORKERS)+1):
            rows=witnesses();rows[index]['pid']=LAUNCH['pid']+1
            with self.subTest(index=index):self.rejected(transcript(rows))

    def test_truncated_tail_rejected(self):
        self.rejected(transcript(trailing=False))

    def test_runner_keeps_actual_process_gate_and_cpu_only_scope(self):
        tree=ast.parse(inspect.getsource(m.main))
        calls=[node for node in ast.walk(tree) if isinstance(node,ast.Call)]
        names=[ast.unparse(node.func) for node in calls]
        self.assertIn('check_launch',names);self.assertIn('read_result',names)
        self.assertIn('pe_identity',names);self.assertIn('verify_raw_artifacts',names)
        receipt=next(node for node in calls if isinstance(node.func,ast.Name) and node.func.id=='dict'
                     and any(k.arg=='schema' and isinstance(k.value,ast.Constant)
                             and k.value.value=='warvk-x1a-cpu-gate/v1' for k in node.keywords))
        flags={key.arg:ast.literal_eval(key.value) for key in receipt.keywords
               if key.arg in ('ipcTested','gpuTested','gameTouched','productOwnerComplete')}
        self.assertEqual(flags,dict(ipcTested=False,gpuTested=False,gameTouched=False,productOwnerComplete=False))


if __name__=='__main__':
    unittest.main(verbosity=2)
