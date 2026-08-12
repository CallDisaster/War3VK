from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


def function_body(signature: str) -> str:
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth = 0
    for pos in range(brace, len(SOURCE)):
        if SOURCE[pos] == "{":
            depth += 1
        elif SOURCE[pos] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[start : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


builder = function_body("bool War3TryBuildShadowPacketFromCurrentDrawRecord(")
grouped = function_body("uint32_t D3D9DeviceEx::War3TryPopulateDirectCurrentDrawGrouped(")
populate = function_body("uint32_t D3D9DeviceEx::War3TryPopulateSemanticShadowScene(")

assert "bool producerPolicyPrevalidated = false" in builder
gate = builder.index("if (!producerPolicyPrevalidated &&")
policy = builder.index("ShadowProducerPolicyAllows(", gate)
reset = builder.index("ResetShadowDrawPacketPreserveScratch(out)", policy)
assert gate < policy < reset

loop = grouped.index("for (size_t buildIndex = 0u;")
call = grouped.index("War3TryBuildShadowPacketFromCurrentDrawRecord(", loop)
finish = grouped.index("packetBuildTiming.finish()", call)
call_body = grouped[call:finish]
assert "recordsForBuildCanonicalPrefiltered && !useSealedWork" in call_body

# The handoff may only be asserted after the grouped preselector has executed
# every canonical gate. Uncapped/static-supplement callers keep the default
# false argument and therefore retain the builder's policy check.
preselect = grouped.index('enterDirectDetailPhase("PreselectScan")')
handoff = grouped.index("recordsForBuildCanonicalPrefiltered = true", preselect)
assert preselect < handoff < loop < call
assert populate.count("War3TryBuildShadowPacketFromCurrentDrawRecord(") == 1
static_call = populate.index("War3TryBuildShadowPacketFromCurrentDrawRecord(")
static_end = populate.index(")) {", static_call)
assert "producerPolicyPrevalidated" not in populate[static_call:static_end]

print("direct producer policy handoff static checks passed")
