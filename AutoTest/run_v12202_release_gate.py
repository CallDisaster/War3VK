"""Run bounded v1.22.02 RC1 checks against the frozen clean-build DLL.

Both existing gates create isolated instances; neither replaces the player DLL.
The pins below are intentionally exact so a rebuild requires a new gate identity.
"""

from __future__ import annotations

import argparse
from pathlib import Path


CANDIDATE = Path(
    "D:/WarVK-Builds/v1.22.02-rc1-20260927/"
    "build32-product/src/d3d9/d3d9.dll"
)
CANDIDATE_SHA256 = (
    "07D6AE38F4969580F15C0E2C6B5966A955EA303ABF6201200C5BED8A92EBFC23"
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", choices=("crossmap", "walk"))
    gate = parser.parse_args().gate

    if gate == "crossmap":
        import run_cross_map_ab_a_canary_20260925 as canary

        canary.EXPECTED.pop(canary.DLL)
        canary.DLL = CANDIDATE
        canary.EXPECTED[CANDIDATE] = CANDIDATE_SHA256
        canary.RUN_ID = "v12202-rc1b-crossmap-20260928"
        canary.SESSION_ID = "v12202map"
        return canary.main()

    import run_unit_walk_probe as walk

    walk.PINNED.pop(walk.DLL)
    walk.DLL = CANDIDATE
    walk.PINNED[CANDIDATE] = CANDIDATE_SHA256
    walk.PINNED[walk.MAP] = (
        "692E92F43BAC99EF8ECED7833E137B22475914F592DA7F78FCC24B54C221E95F"
    )
    walk.PINNED[walk.PLAYER / "d3d9.dll"] = (
        "1FFE0BD14C51C8E1D2A9EB94425A06260CD4B480A7CA735E13E946EA93EE35CF"
    )
    walk.RUN_ID = "v12202-rc1-walk-20260928"
    walk.SESSION_ID = "v12202walk"
    return walk.main()


if __name__ == "__main__":
    raise SystemExit(main())
