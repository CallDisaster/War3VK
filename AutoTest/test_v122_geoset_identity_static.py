"""Narrow source proof for the v1.21-based identity-only port, not a game test."""
import re
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = 'src/d3d9/war3/model/war3_model_resource_cache.cpp'
HEADER = 'src/d3d9/war3/model/war3_model_resource_cache.h'
VISIBLE = 'src/d3d9/war3/render/war3_visible_renderables.cpp'
BASE = 'ae890542d766470d1703f5bea7f5b73636039733'


def source(path):
    return (ROOT / path).read_text(encoding='utf-8')


def baseline(path):
    return subprocess.check_output(['git', 'show', BASE + ':' + path], cwd=ROOT).decode('utf-8').replace('\r\n', '\n')


def body(text, name):
    start = text.index(name)
    start = text.index('{', start)
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def strip_collection_scope(text):
    text = re.sub(r'\n#include "\.\./tools/war3_data_collection_tree.h"', '', text)
    return re.sub(r'\s*WARVK_DATA_SCOPE\([^)]*\);', '', text)


class IdentityPort(unittest.TestCase):
    def test_owned_exact_fields(self):
        block = body(source(HEADER), 'struct ShadowGeosetIdentityView')
        self.assertEqual(re.findall(r'(?:void\*|uint64_t|uint32_t)\s+(\w+)\s*=', block),
                         ['geosetPtr', 'geosetDataPtr', 'modelResourcePtr', 'modelKey', 'geosetIndex'])
        self.assertEqual(body(source(HEADER), 'inline ShadowGeosetIdentityView ProjectGeosetIdentity'),
                         '{\n  return {record.geosetPtr, record.geosetDataPtr, record.modelResourcePtr,\n'
                         '          record.modelKey, record.geosetIndex};\n}')

    def test_same_lookup_and_miss(self):
        old = body(baseline(CACHE), 'bool ShadowModelResourceCache::findGeosetByData(')
        new = strip_collection_scope(
            body(source(CACHE), 'bool ShadowModelResourceCache::findGeosetIdentityByData('))
        new = re.sub(r'//[^\n]*', '', new)
        old = old.replace('materializeGeosetDataRecordLocked(geosetDataPtr, it->second)',
                          'ProjectGeosetIdentity(*it->second)')
        self.assertEqual(re.sub(r'\s+', '', old), re.sub(r'\s+', '', new))

    def test_one_caller_change_only(self):
        expected = baseline(VISIBLE).replace(
            'model::ShadowGeosetResourceRecord directGeosetRecord = {};',
            'model::ShadowGeosetIdentityView directGeosetRecord = {};').replace(
            'resourceCache.findGeosetByData(record.meshData, directGeosetRecord)',
            'resourceCache.findGeosetIdentityByData(record.meshData, directGeosetRecord)')
        self.assertEqual(strip_collection_scope(source(VISIBLE)), expected)
        self.assertEqual(source(VISIBLE).count('findGeosetIdentityByData('), 1)

    def test_existing_readiness_and_alias_unchanged(self):
        for name in ('CopyReadyGeosetBinding(', 'MergeReadyGeosetAlias(',
                     'bool ShadowModelResourceCache::findReadyGeosetBindingByPtr(',
                     'bool ShadowModelResourceCache::findReadyGeosetBindingByData(',
                     'ShadowModelResourceCache::materializeGeosetDataRecordLocked(',
                     'ShadowModelResourceCache::materializeGeosetAliasRecordLocked('):
            self.assertEqual(strip_collection_scope(body(source(CACHE), name)),
                             body(baseline(CACHE), name), name)

    def test_no_readiness_or_payload_copy_in_query(self):
        query = body(source(CACHE), 'bool ShadowModelResourceCache::findGeosetIdentityByData(')
        for bad in ('findReady', 'materializeGeoset', 'positions', 'indices', 'new ', 'resize(', 'reserve('):
            self.assertNotIn(bad, query)


if __name__ == '__main__':
    unittest.main()
