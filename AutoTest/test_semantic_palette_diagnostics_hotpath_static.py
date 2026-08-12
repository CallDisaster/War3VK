from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
PERF = (ROOT / "src/d3d9/war3/tools/war3_perf_monitor.cpp").read_text(
    encoding="utf-8"
)


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
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


policy = function_body(DEVICE, "inline bool War3SemanticPaletteDiagnosticsRuntime()")
assert '"DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS", 0u' in policy

for signature in (
    "void War3NoteLivePaletteMotion(",
    "void War3NoteDrawTimePoseMotion(",
    "void War3NoteSubmittedPaletteMotion(",
):
    body = function_body(DEVICE, signature)
    assert body.index("if (!War3SemanticPaletteDiagnosticsRuntime())") < body.index(
        "if (runtimeModelPtr == nullptr"
    )

assert "if (skinned && War3SemanticPaletteDiagnosticsRuntime())" in DEVICE
assert (
    "if (War3SemanticPaletteDiagnosticsRuntime() &&\n"
    "      stableAuthoritativeSkinnedGeometryKey && currentDrawSample != nullptr)"
    in DEVICE
)
assert (
    "if (War3SemanticPaletteDiagnosticsRuntime() &&\n"
    "        !usesExplicitBlendContract &&"
    in DEVICE
)

append_tail = DEVICE[
    DEVICE.rindex("if (War3SemanticPaletteDiagnosticsRuntime())") :
]
assert append_tail.index("if (War3SemanticPaletteDiagnosticsRuntime())") < append_tail.index(
    "NoteSubmitPaletteFrameLag("
)
assert append_tail.index("if (War3SemanticPaletteDiagnosticsRuntime())") < append_tail.index(
    "QueryCurrentPaletteFrameTag("
)

# Correctness-bearing pose signatures, palette selection, and live rebuilds
# remain active independently of the optional historical probes.
assert "dynamicPoseSignature = bit::fnv1a_iter" in DEVICE
assert "War3GetOrCreateSemanticShadowPalette(" in DEVICE
assert "War3TryBuildLiveRuntimeGroupPalette(" in DEVICE

# Performance reports disclose the opt-in diagnostic configuration.
assert '"DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS"' in PERF

print("semantic palette diagnostics hot-path contract: ok")
