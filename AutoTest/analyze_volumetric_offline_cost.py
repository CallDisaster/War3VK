#!/usr/bin/env python3
"""Offline workload reconstruction for the War3 volumetric pass.

This script performs arithmetic only. It never launches or attaches to
Warcraft III and never submits GPU work.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path


@dataclass(frozen=True)
class Scenario:
    name: str
    divisor: int
    requested_samples: int
    point_lights: int
    enforce_budget: bool


def resolve_column_readability(
    occlusion: float, weight: float, target_exponent: float = 0.5
) -> dict[str, float]:
    """Mirror the top-down shader contract with medium/source gates at one."""
    contrast = min(max(weight, 0.0), 2.5)
    if contrast <= 1.0:
        resolved = occlusion * contrast
    else:
        readability_mix = min(max((contrast - 1.0) / 1.5, 0.0), 1.0)
        exponent = (1.0 - readability_mix) + target_exponent * readability_mix
        resolved = math.pow(occlusion, exponent)
    alpha_mix = min(max((weight - 1.0) / 1.5, 0.0), 1.0)
    alpha_attenuation = min(resolved * 0.45 * alpha_mix, 0.18)
    return {
        "input_occlusion": occlusion,
        "weight": weight,
        "resolved_scattering_occlusion": resolved,
        "extra_base_attenuation": alpha_attenuation,
        "base_transmittance_multiplier": 1.0 - alpha_attenuation,
    }


def evaluate(
    scenario: Scenario, width: int, height: int, segment_budget: int
) -> dict[str, int | float | str | bool]:
    divisor = max(1, scenario.divisor)
    effect_width = math.ceil(width / divisor)
    effect_height = math.ceil(height / divisor)
    effect_pixels = effect_width * effect_height
    samples = max(4, scenario.requested_samples)
    if scenario.enforce_budget:
        samples = min(16, samples)
        budget_samples = segment_budget // max(effect_pixels, 1)
        samples = min(samples, max(4, min(16, budget_samples)))
    ray_segments = effect_pixels * samples
    return {
        **asdict(scenario),
        "effect_width": effect_width,
        "effect_height": effect_height,
        "effect_pixels": effect_pixels,
        "effective_samples": samples,
        "ray_segments": ray_segments,
        # Current bounded shader: at most eight longitudinal CSM probes. Most
        # probes perform one raw fetch; only a cascade-transition probe may
        # perform a second fetch for adjacent-cascade blending. Add the seam
        # worst case explicitly instead of silently preserving the old bound.
        # Point lights retain two cube probes for each of at most two lights.
        # These are potential upper workloads, not measured fetch counters.
        "potential_sun_csm_fetches_longitudinal_8": ray_segments * 8,
        "potential_sun_csm_fetches_cascade_seam_worst_16": ray_segments * 16,
        "potential_sun_clip_matvecs_hoisted": effect_pixels * 8,
        "potential_point_inner_iterations": (
            ray_segments * max(0, scenario.point_lights)
        ),
        "potential_point_cube_fetches_2probe": (
            ray_segments * min(max(0, scenario.point_lights), 2) * 2
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--width", type=int, default=2560)
    parser.add_argument("--height", type=int, default=1351)
    parser.add_argument("--segment-budget", type=int, default=4_000_000)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.width <= 0 or args.height <= 0 or args.segment_budget <= 0:
        parser.error("width, height and segment budget must be positive")

    scenarios = [
        Scenario("old_default", 2, 32, 4, False),
        Scenario("old_exposed_maximum", 1, 96, 8, False),
        Scenario("safe_default_and_runtime_cap", 4, 16, 2, True),
        Scenario("safe_4k_style_budget_example", 4, 16, 2, True),
    ]
    evaluated = [
        evaluate(item, args.width, args.height, args.segment_budget)
        for item in scenarios[:3]
    ]
    safe_segments = int(evaluated[2]["ray_segments"])
    result = {
        "generated_at": datetime.now().astimezone().isoformat(),
        "input_extent": {"width": args.width, "height": args.height},
        "segment_budget": args.segment_budget,
        "assumption": (
            "A low camera or camera-inside-light case is modeled as a "
            "full-effect-extent pass because conservative point ROI can "
            "legitimately fail soft to full screen."
        ),
        "scenarios": evaluated,
        "ratios_vs_safe": {
            item["name"]: int(item["ray_segments"]) / max(safe_segments, 1)
            for item in evaluated
        },
        "column_readability_contract": {
            "assumption": (
                "Fully top-down target exponent 0.5 with medium/source gates "
                "equal to one; weight<=1 must add no base attenuation."
            ),
            "cases": [
                resolve_column_readability(occlusion, weight)
                for occlusion in (1.0 / 64.0, 1.0 / 16.0, 1.0)
                for weight in (0.0, 1.0, 1.25, 2.0, 2.5, 3.0)
            ],
        },
        "far_caster_depth_extension_contract": {
            "extension": 384.0,
            "formula": (
                "eyeDistance=baseD+E; maxZ=2*baseD+E; therefore the far "
                "receiver boundary is invariant and only the toward-sun "
                "caster side expands by E."
            ),
            "examples": [
                {
                    "baseD": base,
                    "eyeDistance": base + 384.0,
                    "maxZ": 2.0 * base + 384.0,
                    "nearReceiverZ": 384.0,
                    "farReceiverZ": 2.0 * base + 384.0,
                }
                for base in (200.0, 800.0, 2000.0)
            ],
        },
        "conclusion": (
            "The safety change bounds resolution, samples and point lights "
            "at execution time; visibility must be tuned with scattering "
            "energy rather than by increasing loop bounds."
        ),
    }

    if args.output is None:
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output = (
            Path(__file__).resolve().parent
            / "artifacts"
            / f"volumetric_offline_cost_{stamp}"
            / "result.json"
        )
    else:
        output = args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    print(f"\nartifact={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
