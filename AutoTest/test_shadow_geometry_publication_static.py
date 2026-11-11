"""Lock the M3 production handoff, not allocation-failure algorithm correctness.

The shared C++ production test owns rollback/fault-injection evidence. These
source gates cover the device wiring and transaction boundaries only; they are
not GPU, memory-pressure recovery, or player acceptance.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEVICE_RAW = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
PUBLICATION_RAW = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_geometry_publication.h"
).read_text(encoding="utf-8")
RETIREMENT_RAW = (
    ROOT / "src/d3d9/war3/shadow/war3_shadow_session_retirement.h"
).read_text(encoding="utf-8")


def code_only(source: str) -> str:
    """Blank comments/literals so prose cannot satisfy a production-code gate."""
    tokens = re.compile(
        r'//[^\n]*|/\*.*?\*/|R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})'
        r'\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    return tokens.sub(lambda match: re.sub(r"[^\n]", " ", match.group()), source)


def braced_body(source: str, marker: str) -> str:
    match = re.search(marker, source)
    if match is None:
        raise AssertionError(f"Missing production boundary: {marker}")
    start = source.find("{", match.end())
    if start < 0:
        raise AssertionError(f"Missing body: {marker}")
    depth = 0
    for end in range(start, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:end]
    raise AssertionError(f"Unterminated body: {marker}")


DEVICE = code_only(DEVICE_RAW)
PUBLICATION = code_only(PUBLICATION_RAW)
RETIREMENT = code_only(RETIREMENT_RAW)
CREATE = braced_body(
    DEVICE, r"bool\s+D3D9DeviceEx::War3CreateShadowPersistentGeometryAfterMiss\s*\("
)
PUBLISH = braced_body(PUBLICATION, r"\bPublishPersistentGeometry\s*\(")


class ShadowGeometryPublicationStaticTests(unittest.TestCase):
    def position(self, pattern: str, source: str = CREATE) -> int:
        match = re.search(pattern, source, re.DOTALL)
        self.assertIsNotNone(match, f"Missing code boundary: {pattern}")
        return match.start()

    def test_device_has_one_shared_publication_call(self) -> None:
        self.assertRegex(
            DEVICE_RAW,
            r'#include\s+"war3/shadow/war3_shadow_geometry_publication\.h"',
        )
        self.assertEqual(len(re.findall(r"\bPublishPersistentGeometry\s*\(", DEVICE)), 1)
        self.assertEqual(len(re.findall(r"\bPublishPersistentGeometry\s*\(", CREATE)), 1)
        self.assertRegex(
            CREATE,
            r"PublishPersistentGeometry\s*\(\s*"
            r"m_war3ShadowPersistentGeometries\s*,\s*m_war3ShadowGeometryRegistry\s*,\s*"
            r"m_war3ShadowPersistentExpiryQueue\s*,\s*m_war3ShadowPersistentCharges\.live\s*,\s*"
            r"bytesNeeded\s*,\s*geometryId\s*,\s*key\s*,\s*std::move\s*\(\s*entry\s*\)\s*,\s*"
            r"registryEntry\s*,\s*War3ShadowPersistentExpiryEntry\s*\{\s*"
            r"m_war3ShadowPersistentFrameSerial\s*,\s*geometryId\s*\}",
        )

    def test_domain_admission_precedes_gc_and_gpu_preparation(self) -> None:
        lookup = self.position(r"m_war3ShadowGeometryRegistry\.find\s*\(\s*key\s*\)")
        admission = self.position(r"\bDecideShadowGeometryOwner\s*\(")
        rejection = self.position(r"return\s+fail\s*\(\s*War3ShadowPersistentCreateFailure::DomainConflict\s*\)")
        gc = self.position(r"\bWar3GcShadowPersistentGeometry\s*\(\s*\)")
        gpu = self.position(r"\bWar3CreateShadowPersistentBuffer\s*\(")
        self.assertLess(lookup, admission)
        self.assertLess(admission, rejection)
        self.assertLess(rejection, gc)
        self.assertLess(gc, gpu)
        self.assertRegex(CREATE, r"domain\s*,\s*0u\s*,\s*existingSlotIt->second\.domain")

    def test_gc_capacity_and_source_checks_stay_before_gpu_and_publication(self) -> None:
        gc = self.position(r"\bWar3GcShadowPersistentGeometry\s*\(")
        capacity = self.position(r"return\s+fail\s*\(\s*War3ShadowPersistentCreateFailure::Capacity\s*\)")
        source = self.position(
            r"upload\.bytes\s*!=\s*0u\s*&&\s*upload\.slice\.buffer\s*\(\s*\)"
            r"\s*==\s*nullptr\s*&&\s*upload\.hostData\s*==\s*nullptr"
        )
        malformed = self.position(r"return\s+fail\s*\(\s*War3ShadowPersistentCreateFailure::Other\s*\)")
        gpu = self.position(r"\bWar3CreateShadowPersistentBuffer\s*\(")
        publication = self.position(r"\bPublishPersistentGeometry\s*\(")
        self.assertEqual(len(re.findall(r"\bWar3GcShadowPersistentGeometry\s*\(", CREATE)), 1)
        self.assertLess(gc, capacity)
        self.assertLess(capacity, source)
        self.assertLess(source, malformed)
        self.assertLess(malformed, gpu)
        gpu_calls = list(re.finditer(r"\bWar3CreateShadowPersistentBuffer\s*\(", CREATE))
        self.assertEqual(len(gpu_calls), 4)
        self.assertTrue(all(match.start() < publication for match in gpu_calls))
        for stream in ("position", "index", "blend", "uv"):
            self.assertLess(self.position(rf"stored\.{stream}ReplayBinding\.capture\s*\("), publication)

    def test_outputs_are_empty_until_successful_transaction(self) -> None:
        publication = self.position(r"\bPublishPersistentGeometry\s*\(")
        failure = self.position(
            r"if\s*\(\s*inserted\s*==\s*m_war3ShadowPersistentGeometries\.end\s*\(\s*\)\s*\)"
            r"\s*(?:\{\s*)?return\s+fail\s*\(\s*War3ShadowPersistentCreateFailure::RegistryInsert\s*\)"
        )
        self.assertLess(publication, failure)
        defaults = {"outGeometryId": "0", "outGeometry": "nullptr", "outCreatedNew": "false"}
        values = {"outGeometryId": "geometryId", "outGeometry": "&inserted->second.geometry", "outCreatedNew": "true"}
        first_failure = self.position(r"\breturn\s+fail\s*\(")
        for output, default in defaults.items():
            assignments = list(re.finditer(rf"\b{output}\s*=\s*([^;]+);", CREATE))
            self.assertEqual(len(assignments), 2, output)
            self.assertEqual(re.sub(r"\s+", "", assignments[0].group(1)), default)
            self.assertLess(assignments[0].start(), first_failure)
            self.assertEqual(re.sub(r"\s+", "", assignments[1].group(1)), values[output])
            self.assertGreater(assignments[1].start(), failure)
        self.assertEqual(len(re.findall(r"return\s+true\s*;", CREATE)), 1)
        self.assertGreater(self.position(r"return\s+true\s*;"), self.position(r"outCreatedNew\s*=\s*true"))

    def test_device_cannot_bypass_the_transaction(self) -> None:
        for forbidden in (
            r"m_war3ShadowPersistentGeometries\s*\.\s*(?:emplace|insert|try_emplace)\s*\(",
            r"m_war3ShadowGeometryRegistry\s*\[",
            r"m_war3ShadowGeometryRegistry\s*\.\s*(?:emplace|insert|insert_or_assign|try_emplace)\s*\(",
            r"m_war3ShadowPersistentBytesUsed\s*(?:\+=|=|\+\+|-=)",
            r"m_war3ShadowPersistentCharges(?:\s*\.\s*(?:live|retired))?\s*(?:\+=|=(?!=)|\+\+|-=|--)",
            r"(?:\+\+|--)\s*m_war3ShadowPersistentCharges\s*\.\s*(?:live|retired)",
            r"m_war3ShadowPersistentCharges\s*\.\s*releaseLive\s*\(",
            r"m_war3ShadowPersistentExpiryQueue\s*\.\s*(?:push|emplace)\s*\(",
        ):
            self.assertNotRegex(CREATE, forbidden)

    def test_identity_and_existing_budget_policy_remain_with_device(self) -> None:
        for required in (
            r"if\s*\(\s*!upload\.retainSource\s*\)\s*bytesNeeded\s*\+=\s*War3AlignPersistentBytes\s*\(\s*upload\.bytes\s*\)",
            r"capBytes\s*=\s*War3GetShadowPersistentPoolCapBytes\s*\(\s*\)",
            r"fitsCapacity\s*=\s*m_war3ShadowPersistentCharges\.canAdmit\s*\(\s*bytesNeeded\s*,\s*capBytes\s*\)\s*;",
            r"if\s*\(\s*!fitsCapacity\s*\)\s*\{\s*diagnostics\.capacityRejectAllCallers\s*\+\+\s*;"
            r"\s*return\s+fail\s*\(\s*War3ShadowPersistentCreateFailure::Capacity\s*\)\s*;\s*\}",
            r"geometryId\s*=\s*m_war3NextShadowGeometryId\s*\+\+",
            r"stored\.key\s*=\s*key\s*;",
            r"entry\.key\s*=\s*key\s*;",
            r"entry\.domain\s*=\s*domain\s*;",
            r"registryEntry\.domain\s*=\s*domain\s*;",
        ):
            self.assertRegex(CREATE, required)
        # This boundary may consume an ID on failure; it must not rewind identity.
        self.assertEqual(len(re.findall(r"\bm_war3NextShadowGeometryId\b", CREATE)), 1)
        for forbidden in (
            r"\b(?:getenv|getEnvVar|GetEnvironmentVariable[A-Z]?|GetPrivateProfile\w*)\s*\(",
            r"\b(?:ShadowGeometryDomain|War3ShadowGeometryRegistryKeyHash)\s*\{",
            r"\b(?:retire|retirementSerial|completedSerial|waitIdle|waitForIdle)\b",
        ):
            self.assertNotRegex(CREATE + PUBLISH, forbidden)

    def test_admission_counts_live_and_retired_without_unsigned_overflow(self) -> None:
        admission = braced_body(RETIREMENT, r"\bbool\s+canAdmit\s*\(")
        # The ordered short-circuit guards make both subtractions safe, including
        # UINT64_MAX inputs. A wrapped live + retired sum must never admit work.
        self.assertIsNotNone(re.fullmatch(
            r"\s*return\s+bytes\s*<=\s*cap\s*&&\s*retired\s*<=\s*cap\s*-\s*bytes"
            r"\s*&&\s*live\s*<=\s*cap\s*-\s*bytes\s*-\s*retired\s*;\s*",
            admission))
        release = braced_body(RETIREMENT, r"\bvoid\s+releaseLive\s*\(")
        self.assertIsNotNone(re.fullmatch(
            r"\s*live\s*=\s*bytes\s*<=\s*live\s*\?\s*live\s*-\s*bytes\s*:\s*0\s*;\s*",
            release))
        total = braced_body(RETIREMENT, r"\buint64_t\s+total\s*\(")
        self.assertIsNotNone(re.fullmatch(
            r"\s*const\s+auto\s+max\s*=\s*std::numeric_limits<uint64_t>::max\s*\(\s*\)\s*;"
            r"\s*return\s+live\s*>\s*max\s*-\s*retired\s*\?\s*max\s*:\s*live\s*\+\s*retired\s*;\s*",
            total))

    def test_shared_helper_keeps_publication_after_last_throwing_stage(self) -> None:
        geometry = self.position(r"\bgeometries\.emplace\s*\(", PUBLISH)
        registry = self.position(r"\bregistry\.try_emplace\s*\(\s*key\s*\)", PUBLISH)
        expiry = self.position(r"\bexpiry\.push\s*\(\s*token\s*\)", PUBLISH)
        slot = self.position(r"\b\w+->second\s*=\s*publishedSlot\s*;", PUBLISH)
        account = self.position(r"\bbytesUsed\s*\+=\s*bytesNeeded\s*;", PUBLISH)
        self.assertLess(geometry, registry)
        self.assertLess(registry, expiry)
        self.assertLess(expiry, slot)
        self.assertLess(slot, account)
        self.assertNotRegex(PUBLISH, r"\bregistry\s*\[")
        self.assertNotRegex(PUBLISH, r"\bregistry\.insert_or_assign\s*\(")
        # A compile-time no-throw requirement, not a comment asserting that fact.
        self.assertRegex(PUBLICATION, r"static_assert\s*\(\s*std::is_nothrow_copy_assignable(?:_v)?\s*<")

    def test_shared_helper_exposes_failure_and_rollback_boundaries(self) -> None:
        bad_alloc = braced_body(PUBLISH, r"catch\s*\(\s*const\s+std::bad_alloc\s*&\s*\w*\s*\)")
        unknown = braced_body(PUBLISH, r"catch\s*\(\s*\.\.\.\s*\)")
        self.assertRegex(bad_alloc, r"return\s+geometries\.end\s*\(\s*\)\s*;")
        self.assertRegex(unknown, r"throw\s*;")
        self.assertNotRegex(unknown, r"return\s+")
        # Mere erase/rollback token presence misses an omitted, unreachable, or
        # conditionally skipped cleanup call. Both catch bodies must enter the
        # common rollback unconditionally before returning/rethrowing.
        self.assertIsNotNone(re.fullmatch(
            r"\s*rollback\s*\(\s*\)\s*;\s*"
            r"return\s+geometries\.end\s*\(\s*\)\s*;\s*", bad_alloc))
        self.assertIsNotNone(re.fullmatch(
            r"\s*rollback\s*\(\s*\)\s*;\s*throw\s*;\s*", unknown))
        # Structural witnesses only. The C++ injected failures prove which nodes
        # are erased and that an existing slot survives every rollback branch.
        self.assertRegex(PUBLISH, r"\bgeometries\.erase\s*\(")
        self.assertRegex(PUBLISH, r"\bregistry\.erase\s*\(")
        for forbidden in ("War3GcShadowPersistentGeometry", "War3CreateShadowPersistentBuffer",
                          "War3GetShadowPersistentPoolCapBytes", "D3D9DeviceEx", "getEnvVar"):
            self.assertNotIn(forbidden, PUBLISH)

    def test_shared_rollback_tracks_only_this_attempts_insertions(self) -> None:
        rollback = braced_body(
            PUBLISH, r"\b(?:const\s+)?auto\s+rollback\s*=\s*\[\s*&\s*\]"
            r"\s*\(\s*\)\s*noexcept"
        )
        # Keep cleanup limited to a newly inserted registry node and the exact
        # geometry inserted by this attempt. Never erase/restore a pre-GC slot.
        self.assertIsNotNone(re.fullmatch(
            r"\s*if\s*\(\s*insertedSlot\s*\)\s*(?:\{\s*)?"
            r"registry\.erase\s*\(\s*slot\s*\)\s*;\s*(?:\}\s*)?"
            r"if\s*\(\s*geometry\s*!=\s*geometries\.end\s*\(\s*\)\s*\)"
            r"\s*(?:\{\s*)?geometries\.erase\s*\(\s*geometry\s*\)\s*;\s*(?:\}\s*)?",
            rollback))
        mutation = self.position(r"\bgeometries\.emplace\s*\(", PUBLISH)
        registry = self.position(r"\bregistry\.try_emplace\s*\(", PUBLISH)
        expiry = self.position(r"\bexpiry\.push\s*\(", PUBLISH)
        for variable, initial, recorded, lower, upper in (
            ("geometry", "geometries.end()", "inserted.first", mutation, registry),
            ("slot", "registry.end()", "registryInsert.first", registry, expiry),
            ("insertedSlot", "false", "registryInsert.second", registry, expiry),
        ):
            assignments = list(re.finditer(rf"\b{variable}\s*=\s*([^;]+);", PUBLISH))
            self.assertEqual(len(assignments), 2, variable)
            self.assertEqual(re.sub(r"\s+", "", assignments[0].group(1)), initial)
            self.assertLess(assignments[0].start(), mutation)
            self.assertEqual(re.sub(r"\s+", "", assignments[1].group(1)), recorded)
            self.assertGreater(assignments[1].start(), lower)
            self.assertLess(assignments[1].start(), upper)


if __name__ == "__main__":
    unittest.main()
