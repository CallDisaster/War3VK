from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
HEADER = (
    ROOT
    / "src/d3d9/war3/memory/war3_current_up_shadow_replay_contract.h"
).read_text(encoding="utf-8")
OPTIONS = (ROOT / "meson_options.txt").read_text(encoding="utf-8")
MESON = (ROOT / "src/d3d9/meson.build").read_text(encoding="utf-8")


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


class CurrentUpShadowReplayStaticTest(unittest.TestCase):
    def test_release_default_is_off(self):
        self.assertIn(
            "option('warvk_current_up_shadow_replay_dev', type : 'boolean', value : false",
            OPTIONS,
        )
        self.assertIn(
            "kCurrentUpShadowReplayDevelopmentEnabled = false", HEADER
        )
        self.assertIn(
            "get_option('warvk_current_up_shadow_replay_dev')", MESON
        )

    def test_value_contract_is_current_draw_and_range_bounded(self):
        body = function_body(HEADER, "EvaluateWar3CurrentUpPositionReplay(")
        for token in (
            "currentPositionUpload",
            "gpuSkinBacking",
            "hasPinnedAllocation",
            "hasUploadBuffer",
            "sameBuffer",
            "replayOffset < input.uploadOffset",
            "input.replayLength > input.uploadLength - localOffset",
        ):
            self.assertIn(token, body)
        self.assertNotIn("fingerprint", body.lower())
        self.assertNotIn("contentGeneration", body)

    def test_consume_reduces_budget_before_freeze_plan(self):
        body = function_body(DEVICE, "void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        decision = body.index("EvaluateWar3CurrentUpPositionReplay")
        budget = body.index("const uint64_t fallbackPosBudgetBytes")
        freeze = body.index("if (shouldFreezePosBufferIntoArena)")
        self.assertLess(decision, budget)
        self.assertLess(budget, freeze)
        self.assertIn(
            "shouldFreezePosBuffer && !currentUpReplayConsumed", body
        )
        self.assertIn(
            "draw.positionPinnedAllocation = std::move(posPinnedAllocation)",
            body,
        )

    def test_observe_does_not_change_release_path(self):
        body = function_body(DEVICE, "War3CurrentUpShadowReplayModeRuntime()")
        self.assertIn("kCurrentUpShadowReplayDevelopmentEnabled", body)
        self.assertIn("War3CurrentUpShadowReplayMode::Off", body)
        self.assertIn(
            '"DXVK_WAR3_CURRENT_UP_SHADOW_REPLAY_MODE"', body
        )
        self.assertNotIn("DXVK_WAR3_CURRENT_UP_SHADOW_REPLAY_MODE", HEADER)


if __name__ == "__main__":
    unittest.main()
