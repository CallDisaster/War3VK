"""Pure buffer-census reader tests: a writer-shaped record and its mutations."""
import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyze_d3d9_buffer_census as c


def ring():
    return dict(caller="game", kind="vertex", length=4096, usage=520, pool=0, format=0, fvf=274, created=2,
                lockCallers=[3, 1, 0], locks=4, unlocks=3, bytes=7168, maxBytes=3840,
                flags=dict(discard=2, noOverwrite=2, readOnly=0, noSysLock=1, doNotWait=0, noDirtyUpdate=0, other=0),
                sizeZero=1, whole=0, outOfRange=0, nested=0, sequential=2, wrapped=1, wrappedDiscard=1,
                beyondWritten=0, sizeBins=[1, 1, 2, 0, 0, 0, 0, 0], framesLocked=2, maxLocksPerFrame=2,
                maxBytesPerFrame=4096)


def record(sequence=1, device=1):
    index = dict(ring(), kind="index", length=1000, usage=8, format=101, fvf=0, created=1, lockCallers=[1, 0, 0],
                 locks=1, unlocks=1, bytes=1000, maxBytes=1000,
                 flags=dict(discard=0, noOverwrite=0, readOnly=0, noSysLock=0, doNotWait=0, noDirtyUpdate=0, other=0),
                 sizeZero=0, whole=1, sequential=0, wrapped=0, wrappedDiscard=0, sizeBins=[0, 1, 0, 0, 0, 0, 0, 0],
                 framesLocked=1, maxLocksPerFrame=1, maxBytesPerFrame=1000)
    return dict(schema=c.SCHEMA, deviceOrdinal=device, snapshotSequence=sequence, reason="present-milestone",
                attribution="return-address-module", gameModuleKnown=True, recordBusy=0, presents=1,
                lockSizeBounds=list(c.LOCK_SIZE_BOUNDS), classes=[ring(), index], classesDropped=0,
                unclassifiedLocks=0, unclassifiedUnlocks=0, saturatedUpdates=0)


def line(row):
    return c.MARKER + json.dumps(row, separators=(",", ":"))


class BufferCensusReaderTest(unittest.TestCase):
    def test_writer_shaped_record(self):
        row = c.parse_record(line(record()))
        summary = c.summary(row)
        self.assertEqual(summary["classes"][0]["bytesPerLock"], 1792.0)
        self.assertEqual(summary["classes"][1]["locksPerPresent"], 1.0)

    def test_rejects_mutations(self):
        def mutate(change):
            row = record()
            change(row)
            return row
        cases = {
            "extra field": lambda r: r.update(extra=1),
            "schema": lambda r: r.update(schema="warvk-d3d9-buffer-census/v2"),
            "bounds": lambda r: r.update(lockSizeBounds=[256]),
            "caller": lambda r: r["classes"][0].update(caller="engine"),
            "kind": lambda r: r["classes"][0].update(kind="constant"),
            "vertex format": lambda r: r["classes"][0].update(format=101),
            "index fvf": lambda r: r["classes"][1].update(fvf=274),
            "zero length": lambda r: r["classes"][0].update(length=0),
            "never created": lambda r: r["classes"][0].update(created=0),
            "callers unbalanced": lambda r: r["classes"][0].update(lockCallers=[3, 0, 0]),
            "bins unbalanced": lambda r: r["classes"][0].update(sizeBins=[1, 1, 1, 0, 0, 0, 0, 0]),
            "flag order": lambda r: r["classes"][0].update(flags=dict(reversed(list(r["classes"][0]["flags"].items())))),
            "flag above locks": lambda r: r["classes"][0]["flags"].update(discard=5),
            "wrap discard": lambda r: r["classes"][0].update(wrappedDiscard=2),
            "ring above locks": lambda r: r["classes"][0].update(sequential=4),
            "lock beyond buffer": lambda r: r["classes"][0].update(maxBytes=4097),
            "bytes above max": lambda r: r["classes"][0].update(bytes=20000),
            "frames without locks": lambda r: r["classes"][1].update(locks=0, lockCallers=[0, 0, 0], sizeBins=[0] * 8),
            "frame bytes": lambda r: r["classes"][0].update(maxBytesPerFrame=1000),
            "frames above presents": lambda r: r.update(presents=0),
            "duplicate class": lambda r: r["classes"].append(copy.deepcopy(r["classes"][0])),
            "drops before full": lambda r: r.update(classesDropped=1),
            "negative": lambda r: r.update(unclassifiedLocks=-1),
            "boolean count": lambda r: r["classes"][0].update(nested=True),
            "reason": lambda r: r.update(reason="per-draw"),
        }
        for name, change in cases.items():
            with self.subTest(name):
                with self.assertRaises(ValueError):
                    c.parse_record(line(mutate(change)))

    def test_saturation_relaxes_balances(self):
        row = record()
        row.update(saturatedUpdates=1)
        row["classes"][0].update(lockCallers=[3, 0, 0])
        c.parse_record(line(row))

    def test_log_sequences(self):
        text = "\n".join(["noise", "info:  " + line(record(1)), "info:  " + line(record(2)),
                          "info:  " + line(record(1, device=2))])
        self.assertEqual(len(c.parse_log(text)), 3)
        with self.assertRaises(ValueError):
            c.parse_log("info:  " + line(record(2)) + "\ninfo:  " + line(record(2)))
        with self.assertRaises(ValueError):
            c.parse_log("info:  WARVK_D3D9_BUFFER_CENSUS {")

    def test_duplicate_keys_and_constants(self):
        with self.assertRaises(ValueError):
            c.parse_record(c.MARKER + '{"schema":1,"schema":2}')
        with self.assertRaises(ValueError):
            c.parse_record(line(record()).replace('"presents":1', '"presents":NaN'))


if __name__ == "__main__":
    unittest.main()
