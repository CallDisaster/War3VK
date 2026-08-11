#!/usr/bin/env python3
"""Structural contracts for defrag-safe War3 replay buffer ownership."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCENE = (ROOT / "src/d3d9/d3d9_war3_scene.h").read_text(encoding="utf-8")
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
SHADOW = (ROOT / "src/d3d9/d3d9_war3_shadow.cpp").read_text(encoding="utf-8")
OUTLINE = (ROOT / "src/d3d9/d3d9_war3_shadow_outline.cpp").read_text(encoding="utf-8")
VALIDATION = (ROOT / "src/d3d9/war3/render/war3_shadow_replay_validation.cpp").read_text(encoding="utf-8")
POLICY = (ROOT / "src/d3d9/war3/render/war3_shadow_replay_binding_policy.h").read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"unterminated function: {signature}")


class ShadowReplayLogicalBindingStaticTest(unittest.TestCase):
    def test_policy_is_checked_and_overflow_safe(self):
        self.assertIn("capturedOffset < backingOffset", POLICY)
        self.assertIn("capturedLength > backingSize - localOffset", POLICY)
        self.assertIn("logical.length > backingSize - logical.offset", POLICY)
        self.assertIn(
            "backingOffset > std::numeric_limits<uint64_t>::max() - logical.offset",
            POLICY,
        )

    def test_scene_seal_captures_logical_bindings_before_seal(self):
        body = function_body(
            DEVICE, "void D3D9DeviceEx::War3SealShadowProducerCompleteness("
        )
        capture = body.index("War3CaptureShadowReplayBindings")
        seal = body.index("producerCompleteness.seal(")
        self.assertLess(capture, seal)
        self.assertIn("scene.shadowCasters", body)
        self.assertIn("scene.shadowFallbacks", body)

    def test_persistent_geometry_owns_and_propagates_logical_bindings(self):
        persistent = function_body(
            DEVICE,
            "bool D3D9DeviceEx::War3CreateShadowPersistentGeometryAfterMiss(",
        )
        last_create = persistent.rindex("War3CreateShadowPersistentBuffer")
        capture = persistent.index("stored.positionReplayBinding.capture", last_create)
        insert = persistent.index("m_war3ShadowPersistentGeometries.emplace", capture)
        self.assertLess(last_create, capture)
        self.assertLess(capture, insert)
        self.assertEqual(DEVICE.count("War3CopyPersistentReplayBindings("), 3)
        for name in ("position", "index", "blend", "uv"):
            self.assertIn(f"{name}ReplayBinding", SCENE)

    def test_resolved_snapshot_precedes_hash_worker_validation_and_draw(self):
        run = function_body(SHADOW, "void War3ShadowReceiverPass::Run(")
        resolve = run.index("ResolveWar3ShadowReplayDraws(")
        replay_hash = run.index("War3BuildReplayContinuityHashes(", resolve)
        point_prepare = run.index("beginPointShadowCpuPrepare(", resolve)
        shadow_draw = run.index("renderShadowMap(", resolve)
        self.assertLess(resolve, replay_hash)
        self.assertLess(resolve, point_prepare)
        self.assertLess(resolve, shadow_draw)

        directional = function_body(
            SHADOW, "bool War3ShadowReceiverPass::renderShadowMap("
        )
        self.assertLess(
            directional.index("ResolveWar3ShadowReplayDraws("),
            directional.index("validateShadowReplayDraws("),
        )
        point = function_body(
            SHADOW, "void War3ShadowReceiverPass::renderPointShadow("
        )
        self.assertLess(
            point.index("ResolveWar3ShadowReplayDraws("),
            point.index("validateShadowReplayDraws("),
        )

    def test_outline_resolves_before_validation(self):
        for signature in (
            "void War3ShadowReceiverPass::renderUnitOutlineScreenSpace(",
            "void War3ShadowReceiverPass::renderUnitOutline(",
        ):
            body = function_body(OUTLINE, signature)
            self.assertLess(
                body.index("ResolveOutlineReplayDraws("),
                body.index("validateShadowReplayDraws("),
            )

    def test_unresolved_binding_is_a_replay_reject(self):
        validate = function_body(
            VALIDATION, "War3ShadowReplayValidationResult ValidateWar3ShadowReplayDraw("
        )
        self.assertIn("UnresolvedBufferBinding", validate)
        self.assertLess(
            validate.index("UnresolvedBufferBinding"),
            validate.index("MissingMapEpoch"),
        )


if __name__ == "__main__":
    unittest.main()
