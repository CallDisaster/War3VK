"""Exercise the actual release ZIP verifier; offline RC auditor stays strict."""
import json
import struct
from pathlib import Path
import tempfile
import unittest
import zipfile
from package_v122_release import (make_archive, verify_archive, verify_win32, members, PLAYER, AUTHOR,
                                  PLAYER_SOURCES, PLAYER_RUNNING_PREDECESSOR_SHA, VERSION)


class ReleaseArchiveTests(unittest.TestCase):
    def test_patch_version_and_player_identity(self):
        self.assertEqual(VERSION, '1.22.03')
        self.assertEqual(PLAYER_RUNNING_PREDECESSOR_SHA,
            '1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package = root / 'package'; package.mkdir()
            (package / 'README.md').write_text('payload', encoding='utf-8')
            make_archive(package, root / 'p.zip', ('README.md',), {})
            manifest = json.loads((package / 'manifest.json').read_text())
            self.assertEqual(manifest['version'], '1.22.03')

    def test_actual_pe_reader_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'fixture.dll'
            data = bytearray(512)
            data[:2] = b'MZ'
            struct.pack_into('<I', data, 0x3c, 0x80)
            data[0x80:0x84] = b'PE\0\0'
            struct.pack_into('<H', data, 0x84, 0x14c)
            struct.pack_into('<H', data, 0x98, 0x10b)
            path.write_bytes(data)
            self.assertTrue(verify_win32(path)['i386'])
            struct.pack_into('<H', data, 0x84, 0x8664)
            struct.pack_into('<H', data, 0x98, 0x20b)
            path.write_bytes(data)
            with self.assertRaises(ValueError):
                verify_win32(path)

    def test_whitelists(self):
        self.assertEqual([x for x in PLAYER if x.endswith('.dll')], ['d3d9.dll'])
        self.assertFalse(any(x.endswith(('.dll', '.exe', '.lua', '.ai')) for x in AUTHOR))
        self.assertIn('README_CN.md', PLAYER)
        self.assertIn('WarVK/icons/atom.blp', AUTHOR)
        self.assertIn('WarVK/WEATHER_API.md', AUTHOR)
        # Player scripts and docs map to repository files that exist.
        repo = Path(__file__).resolve().parents[1]
        for member, source in PLAYER_SOURCES.items():
            self.assertIn(member, PLAYER)
            self.assertTrue((repo / source).is_file(), source)
        for member in AUTHOR:
            self.assertTrue((repo / member).is_file(), member)

    def case(self, mutation):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package = root / 'package'; package.mkdir()
            (package / 'README.md').write_text('payload', encoding='utf-8')
            archive = root / 'package.zip'
            make_archive(package, archive, ('README.md',), {})
            mutation(package, archive)
            with self.assertRaises((ValueError, zipfile.BadZipFile)):
                verify_archive(package, archive, ('README.md',))

    def test_success_and_create_new(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); package = root / 'package'; package.mkdir()
            (package / 'README.md').write_text('hello', encoding='utf-8')
            archive = root / 'p.zip'
            make_archive(package, archive, ('README.md',), {'userReleaseAuthorized': True})
            verify_archive(package, archive, ('README.md',))
            with self.assertRaises(ValueError):
                make_archive(package, archive, ('README.md',), {})

    def test_tampered_file(self):
        self.case(lambda p, z: (p / 'README.md').write_text('changed', encoding='utf-8'))

    def test_extra_payload(self):
        self.case(lambda p, z: (p / 'leaked.log').write_text('private', encoding='utf-8'))

    def test_missing_file(self):
        self.case(lambda p, z: (p / 'README.md').unlink())

    def test_extra_archive_member(self):
        def change(p, z):
            with zipfile.ZipFile(z, 'a') as stream:
                stream.writestr('../escape', 'bad')
        self.case(change)

    def test_manifest_wrong_hash(self):
        def change(p, z):
            m = json.loads((p / 'manifest.json').read_text())
            m['files']['README.md']['sha256'] = '0' * 64
            (p / 'manifest.json').write_text(json.dumps(m))
        self.case(change)


if __name__ == '__main__':
    unittest.main()
