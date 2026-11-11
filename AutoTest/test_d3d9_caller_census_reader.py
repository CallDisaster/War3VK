"""Pure caller-census reader tests: a writer-shaped record and its mutations."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_d3d9_caller_census as c
from analyze_d3d9_migration_census import API_NAMES


def account(**api):
    calls = {name: 0 for name in API_NAMES}
    calls.update(api)
    return dict(apiCalls=calls, drawKinds=[0, 0, 0, 0, 0], primitiveTypes=[0] * 7, primitiveTypeInvalid=0,
                primitiveCountOverProxyBound=0, maxPrimitiveCount=0, maxVertexCount=0, renderStates=[],
                textureStageStates=[], samplerStates=[], stateOutOfRange=0, fvfs=[], fvfsDropped=0)


def record(sequence=1, device=1):
    game = account(DrawIndexedPrimitive=3, SetFVF=2, CreateTexture=1, SetRenderState=4)
    game.update(drawKinds=[0, 0, 3, 0, 0], primitiveTypes=[0, 0, 0, 0, 2, 1, 0], maxPrimitiveCount=5896,
                maxVertexCount=5285, primitiveCountOverProxyBound=1, renderStates=[7, 27, 137], fvfs=[338, 274])
    own = account(StretchRect=432, DrawPrimitive=1)
    own.update(drawKinds=[0, 1, 0, 0, 0], primitiveTypes=[0, 0, 0, 1, 0, 0, 0])
    return dict(schema=c.SCHEMA, deviceOrdinal=device, snapshotSequence=sequence, reason="present-milestone",
                attribution="return-address-module", gameModuleKnown=True, recordBusy=0, proxyPrimitiveBound=4096,
                callers=dict(game=game, self=own, other=account()),
                textureClasses=[dict(caller="game", kind=1, format=21, usage=0, pool=1, count=1, fullMipChainRequests=0,
                                     maxWidth=256, maxHeight=256, maxLevels=9)],
                textureClassesDropped=0, invalidCallerApis=0, saturatedUpdates=0)


def line(row):
    return c.MARKER + json.dumps(row, separators=(",", ":"))


class CallerCensusReaderTests(unittest.TestCase):
    def rejected(self, row):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            c.parse_record(line(row))

    def test_valid_record_and_summary(self):
        s = c.summary(c.parse_record(line(record())))
        self.assertEqual(s["gameOnly"], ["CreateTexture", "DrawIndexedPrimitive", "SetFVF", "SetRenderState"])
        self.assertEqual(s["selfOnly"], ["DrawPrimitive", "StretchRect"])
        self.assertEqual(s["draws"]["game"]["overProxyBound"], 1)

    def test_exact_fields_and_identity(self):
        for key, value in (("schema", "warvk-d3d9-caller-census/v2"), ("attribution", "guess"),
                           ("deviceOrdinal", 0), ("snapshotSequence", 0), ("reason", "other"),
                           ("gameModuleKnown", 1), ("proxyPrimitiveBound", 8192), ("recordBusy", 2**32)):
            row = record(); row[key] = value
            with self.subTest(key=key):
                self.rejected(row)
        row = record(); row["extra"] = 0; self.rejected(row)
        row = record(); del row["saturatedUpdates"]; self.rejected(row)

    def test_caller_accounts_exact(self):
        row = record(); row["callers"] = dict(self=row["callers"]["self"], game=row["callers"]["game"],
                                              other=row["callers"]["other"])
        self.rejected(row)
        row = record(); del row["callers"]["other"]; self.rejected(row)
        row = record(); del row["callers"]["game"]["apiCalls"]["Reset"]; self.rejected(row)
        row = record(); row["callers"]["game"]["apiCalls"]["Reset"] = -1; self.rejected(row)

    def test_draw_accounts_balance(self):
        row = record(); row["callers"]["game"]["drawKinds"][2] = 2; self.rejected(row)
        row = record(); row["callers"]["game"]["primitiveTypes"][4] = 3; self.rejected(row)
        row = record(); row["callers"]["game"]["primitiveTypes"][0] = 1; self.rejected(row)
        row = record(); row["saturatedUpdates"] = 1; row["callers"]["game"]["drawKinds"][2] = 2
        c.parse_record(line(row))  # saturation forbids the equality, not the record

    def test_state_and_set_bounds(self):
        for key, bad in (("renderStates", [256]), ("renderStates", [27, 7]), ("renderStates", [7, 7]),
                         ("textureStageStates", [64]), ("samplerStates", [32]), ("fvfs", [1, 1])):
            row = record(); row["callers"]["game"][key] = bad
            with self.subTest(key=key, bad=bad):
                self.rejected(row)
        row = record(); row["callers"]["game"]["fvfsDropped"] = 1; self.rejected(row)

    def test_texture_classes(self):
        row = record(); row["textureClasses"].append(copy.deepcopy(row["textureClasses"][0])); self.rejected(row)
        row = record(); row["textureClasses"][0]["caller"] = "unknown"; self.rejected(row)
        row = record(); row["textureClasses"][0]["count"] = 0; self.rejected(row)
        row = record(); row["textureClasses"][0]["fullMipChainRequests"] = 2; self.rejected(row)
        row = record(); row["textureClassesDropped"] = 1; self.rejected(row)

    def test_duplicate_keys_and_constants(self):
        text = line(record()).replace('"recordBusy":0', '"recordBusy":0,"recordBusy":0', 1)
        with self.assertRaises(ValueError):
            c.parse_record(text)
        with self.assertRaises(ValueError):
            c.parse_record(line(record()).replace('"maxPrimitiveCount":5896', '"maxPrimitiveCount":NaN', 1))

    def test_log_sequences_never_regress_and_bad_lines_never_skip(self):
        good = "\n".join(["noise", line(record(1)), line(record(2)), line(record(1, device=2))])
        self.assertEqual(len(c.parse_log(good)), 3)
        with self.assertRaises(ValueError):
            c.parse_log(line(record(2)) + "\n" + line(record(2)))
        with self.assertRaises(ValueError):
            c.parse_log(line(record(1)) + "\n" + line(record(2))[:-5])


if __name__ == "__main__":
    unittest.main(verbosity=2)
