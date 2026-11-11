"""One isolated A->B->A check of the native-table map transition signal.

The older canary owns its exact maps and strict runtime gates. Only the run
identity and freshly built DLL pin change here; it never deploys to PLAYER.
"""

import run_cross_map_ab_a_canary_20260925 as canary


canary.RUN_ID = "crossmap-20260927-transition-gate"
canary.SESSION_ID = "mapgate"
canary.EXPECTED[canary.DLL] = (
    "83C1B7BE9E293A94C778FFEEE633C4A5F4089DD0C7AF6C136721FE866050B3E9"
)


if __name__ == "__main__":
    raise SystemExit(canary.main())
