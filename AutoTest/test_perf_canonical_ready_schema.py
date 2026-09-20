"""Compile the actual canonical-ready stream emitters, then strictly parse JSON.

This tests a production emitter slice, not the whole GPU-dependent monitor TU.
The independent object-scoped source check also rejects duplicate field names.
Historical invalid reports are never rewritten or accepted by this test.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(encoding="utf-8")
KEYS = ("semanticSceneCanonicalReadyCutoutCount", "semanticSceneCanonicalReadyAlphaBlendCount")


def budget_object(source):
    function = source.index("std::string War3PerfMonitor::generateJsonDataFromSnapshot(")
    start = source.index('json << "  \\"shadowBudgetSummary\\": {\\n";', function)
    end = source.index('json << "  },\\n";', start)
    return source[start:end]


def strict(text):
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate key: " + key)
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=pairs)


class CanonicalReadySchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        obj = budget_object(SOURCE)
        first = obj.index('json << "    \\"' + KEYS[0])
        end = obj.index('json << "    \\"semanticSceneCanonicalRejectNoStableIdentity', first)
        emitters = obj[first:end]
        cls.directory = tempfile.TemporaryDirectory(prefix="warvk-schema-")
        folder = Path(cls.directory.name)
        cpp = folder / "probe.cpp"
        cls.exe = folder / "probe.exe"
        cpp.write_text('#include <sstream>\n#include <iostream>\n#include <cstdlib>\n'
            'int main(int argc,char** argv){if(argc!=3)return 2;'
            'struct { unsigned long long ' + KEYS[0] + ', ' + KEYS[1] + '; } shadowAgg'
            '{std::strtoull(argv[1],nullptr,10),std::strtoull(argv[2],nullptr,10)};'
            'std::ostringstream json;json << "{\\\"shadowBudgetSummary\\\": {";\n' + emitters +
            '\njson << "\\\"tail\\\":0}}";std::cout << json.str();}', encoding="utf-8")
        compiler = os.environ.get("CXX") or shutil.which("g++")
        if not compiler:
            raise RuntimeError("C++ compiler required for real emitter test")
        subprocess.run([compiler, "-std=c++17", str(cpp), "-o", str(cls.exe)], check=True,
                       capture_output=True, text=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_object_field_names_unique(self):
        keys = re.findall(r'json << "    \\"([^"\\]+)\\":', budget_object(SOURCE))
        self.assertGreater(len(keys), 100)
        self.assertEqual(len(keys), len(set(keys)))
        for key in KEYS:
            self.assertEqual(keys.count(key), 1)

    def test_actual_emitters_zero_nonzero_and_max(self):
        for left, right in ((0, 0), (13, 29), (29, 13), (2**64-1, 2**64-1)):
            output = subprocess.check_output([str(self.exe), str(left), str(right)], text=True, timeout=10)
            obj = strict(output)["shadowBudgetSummary"]
            self.assertEqual((obj[KEYS[0]], obj[KEYS[1]]), (left, right))

    def test_duplicate_equal_and_different_values_rejected(self):
        for a, b in ((0, 0), (7, 7), (7, 8)):
            for key in KEYS:
                with self.assertRaises(ValueError):
                    strict('{"shadowBudgetSummary":{"%s":%d,"%s":%d}}' % (key, a, key, b))

    def test_other_objects_may_use_same_key(self):
        self.assertEqual(strict('{"a":{"x":1},"b":{"x":2}}')["b"]["x"], 2)


if __name__ == "__main__":
    unittest.main()
