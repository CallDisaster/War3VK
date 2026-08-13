#!/usr/bin/env python3
"""Static contract for the prepared-slice latest-part acceleration index."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8", errors="replace"
)


def function(name: str, next_name: str) -> str:
    begin = SOURCE.index(name)
    end = SOURCE.index(next_name, begin)
    return SOURCE[begin:end]


class CurrentDrawPreparedSliceIndexStaticTest(unittest.TestCase):
    def test_publish_updates_exact_and_latest_part_indexes(self) -> None:
        body = function(
            "void PublishCurrentDrawPreparedSlice(",
            "void MarkCurrentDrawPreparedSliceInterest(",
        )
        self.assertIn("g_preparedSliceCache[slot] = stored", body)
        self.assertIn("g_latestPreparedSliceByPart[partSlot] = stored", body)
        self.assertLess(
            body.index("g_preparedSliceCache[slot] = stored"),
            body.index("g_latestPreparedSliceByPart[partSlot] = stored"),
        )

    def test_query_keeps_exact_preference_and_collision_scan(self) -> None:
        body = function(
            "bool QueryCurrentDrawPreparedSlice(",
            "void PublishCurrentDrawContract(",
        )
        exact = body.index("const auto& record = g_preparedSliceCache[slot]")
        latest = body.index("g_latestPreparedSliceByPart[partSlot]")
        scan = body.index("for (const auto& candidate : g_preparedSliceCache)")
        self.assertLess(exact, latest)
        self.assertLess(latest, scan)
        self.assertIn("latest.renderablePart == renderablePart", body)
        self.assertIn("latestExact.captureSerial == latest.captureSerial", body)
        self.assertIn("if (best == nullptr)", body)
        self.assertIn("candidate.captureSerial > best->captureSerial", body)

    def test_reset_and_retire_clear_the_acceleration_index(self) -> None:
        reset = function(
            "void ResetCurrentDrawContractCache(",
            "CurrentDrawRetireResult RetireCurrentDrawContracts(",
        )
        retire = function(
            "CurrentDrawRetireResult RetireCurrentDrawContracts(",
            "void PublishCurrentDrawPreparedSlice(",
        )
        self.assertIn("for (auto& prepared : g_latestPreparedSliceByPart)", reset)
        self.assertIn("for (auto& prepared : g_latestPreparedSliceByPart)", retire)
        self.assertIn("prepared.renderablePart == tombstone.identity.renderablePart", retire)


if __name__ == "__main__":
    unittest.main()
