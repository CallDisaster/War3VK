#!/usr/bin/env python3
"""Contracts for the dev-only coherent REAL declared index domain."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
POLICY = (
    ROOT / "src/d3d9/war3/render/war3_terrain_bounds_provenance.h"
).read_text(encoding="utf-8")
TRIM = (
    ROOT / "src/d3d9/war3/memory/war3_coherent_real_index_trim_contract.h"
).read_text(encoding="utf-8")


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


class CoherentRealDeclaredDomainStaticTest(unittest.TestCase):
    def test_release_cannot_enable_declared_domain(self) -> None:
        self.assertIn("kCoherentRealIndexTrimDevelopmentEnabled = false", TRIM)
        gate = function_body(
            DEVICE, "inline bool War3CoherentRealHintDomainRuntime()"
        )
        self.assertIn("kCoherentRealIndexTrimDevelopmentEnabled", gate)
        self.assertIn("return false", gate)
        self.assertIn('"DXVK_WAR3_COHERENT_REAL_HINT_DOMAIN"', gate)
        self.assertIn("DefaultWar3CoherentRealHintDomainEnabled()", gate)
        self.assertIn("kCoherentRealPerformanceCandidateEnabled = false", TRIM)

    def test_policy_checks_declared_raw_and_position_domains(self) -> None:
        body = function_body(POLICY, "War3ResolveTerrainIndexedDeclaredDomain(")
        for token in (
            "numVertices == 0u",
            "vertexCapacity == 0u",
            "indexElementBytes != 2u",
            "indexElementBytes != 4u",
            "rawEnd <= uint64_t(minVertexIndex)",
            "rawEnd > rawDomainLimit",
            "first < 0",
            "end > int64_t(vertexCapacity)",
        ):
            self.assertIn(token, body)
        self.assertNotIn("new ", body)
        self.assertNotIn("Logger", body)

    def test_valid_declared_domain_bypasses_cache_and_scan(self) -> None:
        body = function_body(DEVICE, "void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        gate = body.index("const bool useCoherentRealDeclaredDomain")
        resolve = body.index("War3ResolveTerrainIndexedDeclaredDomain", gate)
        provenance = body.index("exactDomainFromDeclaredHint = true", resolve)
        cache = body.index("const bool useBoundsObserverDomainCache", provenance)
        scan = body.index("ComputeWar3ExactIndexVertexDomainPrepared", cache)
        self.assertLess(gate, resolve)
        self.assertLess(resolve, provenance)
        self.assertLess(provenance, cache)
        self.assertLess(cache, scan)
        self.assertIn("!exactDomainFromDeclaredHint", body[cache:scan])
        fallback = body.index("if (!exactDomainFromDeclaredHint &&", scan)
        self.assertLess(scan, fallback)
        self.assertIn("ComputeWar3ExactIndexVertexDomainPrepared", body[fallback:])

    def test_declared_domain_does_not_authorize_general_bounds_or_observer(self) -> None:
        body = function_body(DEVICE, "void D3D9DeviceEx::War3TryCaptureShadowCaster(")
        self.assertIn(
            "if (!exactDomainFromDeclaredHint &&\n"
            "          (exactIndexedTerrainBoundsObserveCandidate ||",
            body,
        )
        self.assertIn(
            "if (!exactDomainFromDeclaredHint && end <= positionCapacity64",
            body,
        )
        resolver = body.index("War3ResolveTerrainBoundsVertexRange(")
        self.assertIn("exactIndexedDomainKnown", body[resolver : resolver + 400])
        self.assertNotIn("exactDomainFromDeclaredHint", body[resolver : resolver + 400])

    def test_scope_does_not_touch_shadow_abi_or_point_shadow(self) -> None:
        self.assertNotIn("ShadowCasterPushConstants", POLICY)
        self.assertNotIn("PointShadow", POLICY)
        self.assertNotIn("descriptor", POLICY.lower())


if __name__ == "__main__":
    unittest.main()
