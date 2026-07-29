#!/usr/bin/env python3
"""Static contract for bridge/ramp receiver and CSM A/B controls."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = ROOT / "src" / "d3d9" / "d3d9_war3_pipeline.cpp"


def main() -> int:
    text = PIPELINE.read_text(encoding="utf-8")
    required = {
        "receiver mode":
            'ParseEnvInt("DXVK_WAR3_SHADOW_RECEIVER_MODE"',
        "normal bias":
            'ParseEnvFloat("DXVK_WAR3_SHADOW_NORMAL_BIAS_SCALE"',
        "cascade blend":
            'ParseEnvFloat("DXVK_WAR3_SHADOW_CASCADE_BLEND_RANGE"',
        "stable snap":
            'ParseEnvInt("DXVK_WAR3_SHADOW_STABLE_SNAP"',
        "split lambda":
            'ParseEnvFloat("DXVK_WAR3_SHADOW_SPLIT_LAMBDA"',
    }
    missing = [label for label, needle in required.items() if needle not in text]
    if missing:
        raise AssertionError(f"missing A/B controls: {', '.join(missing)}")

    assert "std::clamp(receiverMode, 0, 2)" in text
    assert "std::max(normalBiasScale, 0.0f)" in text
    assert "std::max(cascadeBlendRange, 0.0f)" in text
    assert "stableSnap != 0 ? 1.0f : 0.0f" in text
    assert "std::clamp(splitLambda, 0.0f, 1.0f)" in text
    print("shadow receiver A/B env static contracts: 10/10 PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
