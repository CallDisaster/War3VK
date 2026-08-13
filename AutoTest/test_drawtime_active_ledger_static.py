#!/usr/bin/env python3
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_CPP = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(
    encoding="utf-8", errors="replace"
)
DEVICE_H = (ROOT / "src/d3d9/d3d9_device.h").read_text(
    encoding="utf-8", errors="replace"
)
LEDGER_H = (
    ROOT / "src/d3d9/war3/render/war3_drawtime_active_ledger.h"
).read_text(encoding="utf-8", errors="replace")


class DrawTimeActiveLedgerStaticTest(unittest.TestCase):
    def setUp(self) -> None:
        begin = DEVICE_CPP.index(
            "uint32_t D3D9DeviceEx::War3TryPopulateDrawTimeSemanticProducer("
        )
        end = DEVICE_CPP.index(
            "void D3D9DeviceEx::War3CollectRetiredShadowSessions", begin
        )
        self.producer = DEVICE_CPP[begin:end]

    def test_ledger_is_value_keyed_and_aba_safe(self) -> None:
        self.assertIn("Key key = {};", LEDGER_H)
        self.assertIn("uint64_t activationOrdinal", LEDGER_H)
        self.assertIn("m_nextActivationOrdinal++", LEDGER_H)
        self.assertIn("std::vector<Record> m_records", LEDGER_H)
        self.assertNotIn("iterator", LEDGER_H)
        self.assertNotRegex(LEDGER_H, r"\bKey\s*\*")
        self.assertIn("War3DrawTimeActiveLedger::EntryStamp", DEVICE_H)
        self.assertIn("War3DrawTimeActiveLedger m_war3DrawTimeActiveLedger", DEVICE_H)

    def test_both_current_frame_writes_activate_the_full_key(self) -> None:
        writes = list(
            re.finditer(
                r"(?:cached|entry)\.frameSerial\s*=\s*"
                r"m_war3ShadowPersistentFrameSerial;",
                DEVICE_CPP,
            )
        )
        self.assertEqual(len(writes), 2)
        for write in writes:
            tail = DEVICE_CPP[write.end() : write.end() + 180]
            self.assertRegex(
                tail,
                r"War3ActivateDrawTimeCacheEntry\(vbCacheKey,\s*"
                r"(?:cached|entry)\);",
            )

    def test_frame_and_session_boundaries_reset_logical_records(self) -> None:
        serial = DEVICE_CPP.index("m_war3ShadowPersistentFrameSerial++;")
        begin = DEVICE_CPP.index("m_war3DrawTimeActiveLedger.beginFrame(", serial)
        self.assertLess(serial, begin)
        reset = DEVICE_CPP.index("m_war3DrawTimeActiveLedger.resetSession();")
        move = DEVICE_CPP.index(
            "retired.drawTimeVbCache = std::move(m_war3DrawTimeVBCache);"
        )
        self.assertLess(move, reset)
        self.assertIn("m_records.clear();", LEDGER_H)
        reset_begin = LEDGER_H.index("void resetSession() noexcept")
        reset_end = LEDGER_H.index("\n  }", reset_begin)
        self.assertNotIn(
            "m_nextActivationOrdinal", LEDGER_H[reset_begin:reset_end]
        )

    def test_producer_revalidates_map_stamp_and_all_original_gates(self) -> None:
        self.assertIn("useActiveDrawTimeLedger", self.producer)
        self.assertIn("m_war3DrawTimeVBCache.find(activeRecord.key)", self.producer)
        self.assertIn("War3DrawTimeActiveLedger::matches(", self.producer)
        self.assertIn("auto cacheIt = m_war3DrawTimeVBCache.begin();", self.producer)
        self.assertIn("if (useActiveDrawTimeLedger)", self.producer)
        for gate in (
            "entry.MatchesKey(cacheKey)",
            "entry.frameSerial != m_war3ShadowPersistentFrameSerial",
            "War3DrawTimeExactRejectedCurrentFrame(cacheKey)",
            "entry.HasCompleteBacking()",
            "metadataRejectedBlocker",
            "entry.HasCompleteAlphaPayload()",
            "entry.exactSubmittedFrameSerial =",
        ):
            self.assertIn(gate, self.producer)
        self.assertNotIn(
            "for (auto& [cacheKey, entry] : m_war3DrawTimeVBCache)",
            self.producer,
        )


if __name__ == "__main__":
    unittest.main()
