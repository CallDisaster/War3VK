#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/d3d9/war3/render/war3_current_draw_contract.cpp").read_text(
    encoding="utf-8"
)


def function_body(text: str, signature: str) -> str:
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : index + 1]
    raise AssertionError(f"unterminated function: {signature}")


decode = function_body(SOURCE, "bool DecodeCapturedPaletteForRecord(")
resolve = function_body(
    SOURCE, "CurrentDrawResolveStatus ResolveCurrentDrawAuthoritativeSampleFromRecord("
)
query = function_body(SOURCE, "bool QueryCurrentDrawContractCapturedPalette(")

assert "CurrentDrawPaletteHashSummary paletteHash" in decode
assert decode.index("paletteHash.include(outPalette[i])") > decode.index("outPalette[i] =")
assert "*outPaletteHash = paletteHash.finish()" in decode
assert "true, &out.paletteHash" in resolve
assert "bit::fnv1a_hash(" not in resolve
assert "true, outPaletteHash" in query

print("current-draw palette hash handoff static checks passed")
