import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DataCollectionStatic(unittest.TestCase):
    def test_manifest_entries_are_actual_guarded_definitions(self):
        manifest = json.loads((ROOT / 'AutoTest/data_collection_entrypoints.json').read_text())
        count = 0
        for group in manifest['groups']:
            text = (ROOT / group['path']).read_text(encoding='utf-8')
            self.assertIn('#include "' + group['include'] + '"', text)
            for f in group['functions']:
                pattern = (
                    r'\b' + re.escape(f['name']) +
                    r'\([^;{}]*?\)\s*(?:const\s*)?(?:noexcept\s*)?\{\s*WARVK_DATA_SCOPE\(' +
                    f['tag'] + r'\);'
                )
                self.assertEqual(
                    len(re.findall(pattern, text, re.S)), 1, (group['path'], f['name']))
                count += 1
        self.assertEqual(count, 114)
        self.assertFalse(manifest['coverageComplete'])

    def test_disabled_gate_is_noop(self):
        header = (ROOT / 'src/d3d9/war3/tools/war3_data_collection_tree.h').read_text()
        self.assertIn('#define WARVK_DATA_SCOPE(tag) do {} while (false)', header)
        meson = (ROOT / 'meson_options.txt').read_text()
        self.assertIn("option('warvk_data_collection_tree_dev'", meson)
        self.assertIn("value : false", meson)

    def test_root_coherent_sampler_and_safe_export(self):
        source = (ROOT / 'src/d3d9/war3/tools/war3_data_collection_tree.cpp').read_text()
        self.assertIn('if (!l.depth)', source)
        self.assertIn('m_sampled = l.sampled', source)
        self.assertIn('out.tree = l.tree.snapshot()', source)
        self.assertIn('std::lock_guard<std::mutex> lock(s.mutex)', source)
        self.assertIn('recording-session-not-selected-report-window', source)
        self.assertIn('notAdditiveWithExistingHookTree', source)
        self.assertIn('unknownOutsideRoots', source)
        self.assertNotIn('coverageComplete\":true', source)

    def test_profiler_excludes_its_own_collection_queries(self):
        source = (ROOT / 'src/d3d9/war3/tools/war3_perf_monitor.cpp').read_text(encoding='utf-8')
        capture = source.split('War3PerfMonitor::captureExportSnapshotLocked() const {')[1]
        self.assertLess(capture.index('collection::Pause'), capture.index('ExportSnapshot snapshot'))
        self.assertIn('collection::ResetSession()', source)
        self.assertIn('json << "  \\"mainThreadTimeline\\": "', source)
        self.assertIn('json << "  \\"dataCollectionTree\\": "', source)
        for name in (
            'DXVK_WAR3_DATA_COLLECTION_TREE',
            'DXVK_WAR3_DATA_COLLECTION_SAMPLE_PERIOD',
            'DXVK_WAR3_FRAME_TIMELINE',
        ):
            self.assertIn('"' + name + '"', source)

    def test_identity_query_keeps_miss_and_lock_contract(self):
        source = (ROOT / 'src/d3d9/war3/model/war3_model_resource_cache.cpp').read_text(encoding='utf-8')
        body = source.split('bool ShadowModelResourceCache::findGeosetIdentityByData(')[1].split('\n}\n', 1)[0]
        self.assertIn('std::shared_lock<std::shared_mutex> lock(m_mutex)', body)
        self.assertIn('it == m_byGeosetData.end() || it->second == nullptr', body)
        self.assertIn('out = ProjectGeosetIdentity(*it->second)', body)
        self.assertNotIn('materializeGeosetDataRecordLocked(', body)
        self.assertNotIn('find(', body.split('out = ProjectGeosetIdentity')[1])

    def test_template_data_tree_is_separate(self):
        template = (ROOT / 'src/d3d9/war3/tools/war3_perf_report_template.h').read_text(encoding='utf-8')
        self.assertIn("treeThread.indexOf('data:') === 0", template)
        self.assertIn('sampled total ms', template)
        self.assertIn('根外入口覆盖尚未证明完整', template)
        self.assertNotIn('sections.push(', template)


if __name__ == '__main__':
    unittest.main()
