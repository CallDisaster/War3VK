from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")


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
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


source_start = DEVICE.index("const Matrix4* sourceMatrices =")
append = DEVICE[source_start : source_start + 3000]
override_start = append.index("if (hasOverridePalette) {")
packet_start = append.index("} else if (War3SemanticPaletteLooksModelLocal(", override_start)
override_branch = append[override_start:packet_start]

assert "semanticScenePaletteOverrideNoComposeCount++" in override_branch
assert "War3SemanticPaletteDiagnosticsRuntime() &&" in override_branch
assert "War3SemanticPaletteLooksModelLocal(" in override_branch
assert override_branch.index("War3SemanticPaletteDiagnosticsRuntime() &&") < override_branch.index(
    "War3SemanticPaletteLooksModelLocal("
)

# The non-override packet route still performs the correctness-bearing
# model-local test and composes the world transform when required.
assert "composeWorldPalette = true;" in append[packet_start:]
assert "semanticScenePalettePacketWorldComposeCount++" in append[packet_start:]

print("semantic palette override compose bypass static checks passed")
