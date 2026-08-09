#!/usr/bin/env python3
"""Static contracts for Present-safe Shadow Arena memory-budget admission."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


ARENA = read("src/d3d9/war3/memory/war3_shadow_arena.cpp")
BUDGET = read("src/d3d9/war3/memory/war3_shadow_arena_budget.h")
DIAG_H = read("src/d3d9/war3/tools/war3_diagnostics_hub.h")
DIAG_CPP = read("src/d3d9/war3/tools/war3_diagnostics_hub.cpp")
MESON = read("src/d3d9/meson.build")


class ShadowArenaMemoryBudgetStaticTests(unittest.TestCase):
    def test_policy_never_raises_fixed_residency_cap(self) -> None:
        self.assertIn("kShadowArenaFixedResidentLimitBytes", BUDGET)
        self.assertIn("kShadowArenaMemoryReserveBytes", BUDGET)
        self.assertIn("kShadowArenaBudgetFractionDenominator = 4u", BUDGET)
        self.assertIn("std::min(", BUDGET)
        self.assertIn("result.fixedResidentLimitBytes", BUDGET)
        self.assertIn("result.proportionalLimitBytes", BUDGET)
        self.assertIn("result.reserveLimitBytes", BUDGET)
        self.assertIn("if (!saneSnapshot)", BUDGET)

    def test_budget_query_is_confined_to_generation_safe_points(self) -> None:
        init = ARENA.split("bool ShadowArena_Init()", 1)[1].split(
            "bool ShadowArena_IsInitialized()", 1
        )[0]
        begin = ARENA.split("bool ShadowArena_BeginFrame(", 1)[1].split(
            "void ShadowArena_EndFrame", 1
        )[0]
        allocate = ARENA.split("bool AllocateArenaPage(", 1)[1].split(
            "bool IsValidAlignment", 1
        )[0]
        self.assertIn("RefreshArenaMemoryBudget(0u)", init)
        self.assertIn("RefreshArenaMemoryBudget(frameSerial)", begin)
        self.assertNotIn("getMemoryHeapInfo", allocate)
        self.assertIn("CanGrowArenaBy(pageCapacity)", allocate)

    def test_pressure_blocks_new_pages_without_reclaiming_inflight_pages(self) -> None:
        grow = ARENA.split("bool CanGrowArenaBy(", 1)[1].split(
            "bool AllocateArenaPage", 1
        )[0]
        self.assertIn("ShadowArenaCanGrowResident", grow)
        self.assertIn("g_budgetGrowthRejectCount", grow)
        self.assertNotIn("clear()", grow)
        self.assertNotIn("erase(", grow)
        self.assertNotIn("reset()", grow)

    def test_runtime_and_flight_diagnostics_expose_budget_decision(self) -> None:
        for token in (
            "shadowArenaFixedResidentLimitBytes",
            "shadowArenaMemoryBudgetBytes",
            "shadowArenaMemoryAllocatedBytes",
            "shadowArenaMemoryAvailableBytes",
            "shadowArenaProportionalLimitBytes",
            "shadowArenaReserveLimitBytes",
            "shadowArenaBudgetRefreshCount",
            "shadowArenaBudgetGrowthRejectCount",
            "shadowArenaMemoryBudgetSupported",
            "shadowArenaMemoryBudgetTrusted",
            "arenaResidentLimitBytes",
            "arenaMemoryAvailableBytes",
            "arenaBudgetGrowthRejectCount",
        ):
            self.assertIn(token, DIAG_H)
            self.assertIn(token, DIAG_CPP)

    def test_value_policy_has_a_runnable(self) -> None:
        self.assertIn("war3_shadow_arena_budget_test", MESON)
        self.assertIn("war3_shadow_arena_budget", MESON)


if __name__ == "__main__":
    unittest.main()
