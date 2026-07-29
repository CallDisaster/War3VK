#!/usr/bin/env python3
"""Rank one-frame shadow tears in a fixed-camera exact-frame sequence.

The bridge/ramp visual probe writes exact backbuffer BMPs.  This analyzer
builds a temporal median at reduced resolution, fits away slow global lighting
changes per frame, excludes persistently animated pixels using temporal MAD,
and ranks transient dark connected components.  A world-origin giant shadow
triangle should therefore rank as one large stable-background component rather
than being hidden by global day/night drift or unit animation.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import cv2
import numpy as np


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("frame_dir", type=Path)
    parser.add_argument("--pattern", default="*.bmp")
    parser.add_argument("--scene-ratio", type=float, default=0.60)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--threshold", type=float, default=22.0)
    parser.add_argument("--stable-mad", type=float, default=14.0)
    parser.add_argument("--top", type=int, default=10)
    parser.add_argument("--output", type=Path, default=None)
    return parser.parse_args()


def load_sequence(
    paths: list[Path], scene_ratio: float, target_width: int
) -> tuple[np.ndarray, list[np.ndarray], tuple[int, int]]:
    gray_frames: list[np.ndarray] = []
    color_frames: list[np.ndarray] = []
    source_size = (0, 0)
    for path in paths:
        color = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if color is None:
            raise RuntimeError(f"cannot read frame: {path}")
        height, width = color.shape[:2]
        source_size = (width, height)
        scene_height = max(1, min(height, int(round(height * scene_ratio))))
        scene = color[:scene_height, :, :]
        scaled_height = max(
            1, int(round(scene_height * target_width / max(width, 1)))
        )
        resized = cv2.resize(
            scene, (target_width, scaled_height), interpolation=cv2.INTER_AREA
        )
        color_frames.append(resized)
        gray_frames.append(cv2.cvtColor(resized, cv2.COLOR_BGR2GRAY))
    return np.stack(gray_frames, axis=0), color_frames, source_size


def fit_expected(
    reference: np.ndarray, frame: np.ndarray, stable: np.ndarray
) -> tuple[np.ndarray, float, float]:
    sample = stable.copy()
    # Avoid very dark UI/letterbox-like regions influencing the lighting fit.
    sample &= reference > 12.0
    y_idx, x_idx = np.nonzero(sample)
    if y_idx.size < 256:
        return reference, 1.0, 0.0
    stride = max(1, y_idx.size // 20000)
    x = reference[y_idx[::stride], x_idx[::stride]].astype(np.float64)
    y = frame[y_idx[::stride], x_idx[::stride]].astype(np.float64)
    design = np.column_stack((x, np.ones_like(x)))
    scale, offset = np.linalg.lstsq(design, y, rcond=None)[0]
    scale = float(np.clip(scale, 0.65, 1.35))
    offset = float(np.clip(offset, -48.0, 48.0))
    return reference * scale + offset, scale, offset


def largest_component(mask: np.ndarray) -> tuple[int, np.ndarray]:
    count, labels, stats, _ = cv2.connectedComponentsWithStats(
        mask.astype(np.uint8), connectivity=8
    )
    if count <= 1:
        return 0, np.zeros_like(mask, dtype=bool)
    areas = stats[1:, cv2.CC_STAT_AREA]
    best = 1 + int(np.argmax(areas))
    return int(stats[best, cv2.CC_STAT_AREA]), labels == best


def main() -> int:
    args = parse_args()
    paths = sorted(args.frame_dir.glob(args.pattern))
    if len(paths) < 3:
        raise SystemExit("at least three frames are required")

    stack_u8, colors, source_size = load_sequence(
        paths,
        float(np.clip(args.scene_ratio, 0.10, 1.0)),
        max(64, args.width),
    )
    stack = stack_u8.astype(np.float32)
    reference = np.median(stack, axis=0)
    mad = np.median(np.abs(stack - reference[None, :, :]), axis=0)
    stable = mad <= max(args.stable_mad, 0.0)
    kernel = np.ones((3, 3), dtype=np.uint8)

    rows: list[dict[str, Any]] = []
    masks: list[np.ndarray] = []
    for index, frame in enumerate(stack):
        expected, scale, offset = fit_expected(reference, frame, stable)
        dark_delta = expected - frame
        raw_mask = (dark_delta >= args.threshold) & stable
        clean = cv2.morphologyEx(
            raw_mask.astype(np.uint8), cv2.MORPH_OPEN, kernel
        )
        clean = cv2.morphologyEx(clean, cv2.MORPH_CLOSE, kernel)
        largest, component = largest_component(clean != 0)
        masks.append(component)
        stable_values = dark_delta[stable]
        rows.append(
            {
                "index": index,
                "path": str(paths[index]),
                "lightingScale": scale,
                "lightingOffset": offset,
                "darkPixelCount": int(np.count_nonzero(clean)),
                "darkPixelFraction": float(
                    np.count_nonzero(clean) / max(np.count_nonzero(stable), 1)
                ),
                "largestDarkComponent": largest,
                "p99DarkDelta": float(
                    np.percentile(stable_values, 99.0)
                    if stable_values.size
                    else 0.0
                ),
            }
        )

    ranked = sorted(
        rows,
        key=lambda row: (
            row["largestDarkComponent"],
            row["darkPixelCount"],
            row["p99DarkDelta"],
        ),
        reverse=True,
    )
    top = ranked[: max(1, args.top)]
    output = args.output or args.frame_dir.with_name(
        args.frame_dir.name + "_temporal_shadow_analysis.json"
    )
    output.parent.mkdir(parents=True, exist_ok=True)

    result = {
        "frameDir": str(args.frame_dir),
        "frameCount": len(paths),
        "sourceSize": list(source_size),
        "analysisSize": [int(stack.shape[2]), int(stack.shape[1])],
        "sceneRatio": args.scene_ratio,
        "threshold": args.threshold,
        "stableMad": args.stable_mad,
        "stablePixelFraction": float(np.mean(stable)),
        "top": top,
        "all": rows,
    }
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    tile_width = colors[0].shape[1]
    tile_height = colors[0].shape[0]
    contact_rows: list[np.ndarray] = []
    for row in top:
        index = int(row["index"])
        tile = colors[index].copy()
        overlay = tile.copy()
        overlay[masks[index]] = (0, 0, 255)
        tile = cv2.addWeighted(tile, 0.72, overlay, 0.28, 0.0)
        cv2.putText(
            tile,
            f"#{index} area={row['largestDarkComponent']}",
            (8, 24),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.62,
            (0, 255, 255),
            2,
            cv2.LINE_AA,
        )
        contact_rows.append(tile)
    contact = np.vstack(contact_rows)
    contact_path = output.with_name(output.stem + "_contact.png")
    cv2.imwrite(str(contact_path), contact)
    result["contactSheet"] = str(contact_path)
    output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8"
    )

    print(json.dumps({"output": str(output), "contact": str(contact_path),
                      "top": top}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
