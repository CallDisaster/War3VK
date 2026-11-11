"""Supplementary wiring gates, not substitutes for real x86 adapter tests."""
from pathlib import Path
import re
import unittest

ROOT=Path(__file__).resolve().parents[1]
SOURCE=(ROOT/'src/d3d9/war3/memory/war3_mdl_trace.cpp').read_text('utf-8')
HOOK=(ROOT/'src/d3d9/war3/memory/war3_mdl_trace_hooks.h').read_text('utf-8')
BOOT=(ROOT/'src/d3d9/d3d9_war3_hook.cpp').read_text('utf-8')


class Wiring(unittest.TestCase):
    def test_exact_hash_profiles(self):
        for name, sha in [('gameHash','E04D1716603C075EB0C8E1E21CF1093A664ADC5249EFAB396BFA08D7B09D0C3A'),
                          ('stormHash','F8F519CFAA6275A5172A014F0ABED2212284390A33F1194677155A7D408E63EB')]:
            body=re.search(name+r' = \{([^}]+)\}',SOURCE).group(1)
            self.assertEqual(bytes(int(v,16) for v in re.findall(r'0x([0-9a-fA-F]+)',body)).hex().upper(),sha)

    def test_default_off_and_early_seam(self):
        options=(ROOT/'meson_options.txt').read_text('utf-8')
        self.assertRegex(options,r"option\('warvk_mdl_allocation_trace_dev', type : 'boolean', value : false")
        seam=BOOT[BOOT.index('void War3Hook::InstallHooks('):]
        self.assertLess(seam.index('if (bootstrapNoGameHooks)'),seam.index('InstallMdlAllocationTrace'))
        self.assertRegex(seam,r'#if defined\(WARVK_MDLDATA_TRACE_DEV\)[\s\S]*?if \(!bootstrapMinimal\)\s+dxvk::war3::memory::InstallMdlAllocationTrace')
        self.assertIn('DXVK_WAR3_MDLDATA_TRACE_FILE',SOURCE)
        self.assertIn('CREATE_NEW',SOURCE)

    def test_no_allocator_or_decode_policy(self):
        for text in ('TlsfPool_', 'StormHook_Install(', 'SetLargeBlockThreshold(', 'VirtualAlloc('):
            self.assertNotIn(text,SOURCE)
        self.assertNotIn('MH_ALL_HOOKS',SOURCE.replace('// Only our known targets, never MH_ALL_HOOKS.',''))
        for text in ('Logger::','FlushFileBuffers','FlushViewOfFile','WriteFile','VirtualQuery','Sleep('):
            self.assertNotIn(text,HOOK)

    def test_allocator_is_argument_only(self):
        alloc=HOOK[HOOK.index('static void* __fastcall alloc('):HOOK.index('static int __stdcall free(')]
        self.assertNotIn('fields(',alloc)
        self.assertNotIn('read(',alloc)
        self.assertNotIn('strcmp',HOOK)
        self.assertIn('return result;',alloc)

    def test_thunks_and_relocation(self):
        for thunk,iat,ordinal in [('120628','94E678',401),('1205CE','94E5C8',403),('12062E','94E67C',405)]:
            self.assertIn(f'Import(base,0x{thunk},0x{iat},storm,{ordinal})',SOURCE)
        self.assertIn('const uint32_t relocated = base+iat;',SOURCE)
        self.assertIn('0x55,0x8b,0xec,0x56,0x8b,0x75,0x08',SOURCE)

    def test_no_device_lock_reintroduced(self):
        device=(ROOT/'src/d3d9/d3d9_device.cpp').read_text('utf-8')
        expr=re.search(r'm_multithread\s*\(([^\n]+)',device)
        self.assertIsNotNone(expr)
        self.assertNotIn('CENSUS',expr.group(1))
        self.assertNotIn('MDLDATA',expr.group(1))


if __name__=='__main__':
    unittest.main()
