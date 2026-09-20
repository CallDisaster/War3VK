from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE = (ROOT / "src/d3d9/d3d9_device.cpp").read_text(encoding="utf-8")
# M2-1：War3SemanticPaletteDiagnosticsRuntime 已迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（函数体逐字节相同，
# 仅去掉匿名命名空间内冗余的 inline）；策略断言跟随符号读取新模块。
MODULE = (ROOT / "src/d3d9/war3/semantic/war3_live_palette_selection.cpp").read_text(
    encoding="utf-8"
)
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


policy = function_body(MODULE, "bool War3SemanticPaletteDiagnosticsRuntime()")
assert '"DXVK_WAR3_SEMANTIC_PALETTE_DIAGNOSTICS", 0u' in policy

# M2-3：三个 War3Note*Motion 诊断函数已逐字节迁往
# src/d3d9/war3/semantic/war3_live_palette_selection.cpp（三个符号不匹配预算
# 门禁的语义选择命名族，已单独登记在 FROZEN_M2_3）。断言语义一字未改，只把
# 函数体读取锚从 device.cpp 跟随迁移改到模块 .cpp（M2-1/M2-2 同型的"跟随迁移
# 改锚、断言不削弱"）。
for signature in (
    "void War3NoteLivePaletteMotion(",
    "void War3NoteDrawTimePoseMotion(",
    "void War3NoteSubmittedPaletteMotion(",
):
    body = function_body(MODULE, signature)
    assert body.index("if (!War3SemanticPaletteDiagnosticsRuntime())") < body.index(
        "if (runtimeModelPtr == nullptr"
    )

# M2-5：skinned palette taxonomy 发射块的诊断门随块整块迁往
# src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp。断言语义一字未改，
# 只把读取锚从 device.cpp 跟随迁移改到模块 .cpp；device.cpp 只剩调用点，
# 且仍然只有一处（M2-1/M2-2/M2-3 同型）。
TAXONOMY_MODULE = (
    ROOT / "src/d3d9/war3/semantic/war3_palette_taxonomy_emission.cpp"
).read_text(encoding="utf-8")
assert "if (skinned && War3SemanticPaletteDiagnosticsRuntime())" in TAXONOMY_MODULE
assert "if (skinned && War3SemanticPaletteDiagnosticsRuntime())" not in DEVICE
assert DEVICE.count("War3EmitSemanticPaletteTaxonomy(") == 1
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
