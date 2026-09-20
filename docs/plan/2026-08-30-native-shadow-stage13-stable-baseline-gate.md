# Native shadow / Stage13 stable-baseline gate — 2026-08-30

## Candidate identity

- Source baseline: `8f232cc7d5a69bb4cadc99d41795273e6900b9f1` plus the reviewed native-shadow,
  path-blocker, and Stage13 exact-persistence source changes.
- DLL: PE32/i386, 34,362,820 bytes.
- SHA-256: `BEB35D6AA13739311B9A6AC5D7E43CFB1AE96B07B19E68B2C42AE08F6E0590A8`.
- Frozen artifact:
  `AutoTest/artifacts/native_shadow_stage13_persistent_candidate_beb35d6a_20260830/d3d9.dll`.
- Build: exact 77-edge source-consistent build under Below Normal / `-j2`,
  followed by exact DLL no-work.

The candidate is not stable and must not be distributed or left in the live
Warcraft directory until every gate below is satisfied.

## Gate 1 — clean offline regression

Run from an independent clean validation worktree so the current user's 258
tracked `AutoTest/**` deletions are neither restored nor mistaken for a pass.
The candidate checkpoint must be applied without unrelated StormBreaker,
PlayerCrash, build-log, or stat-only worktree noise.

Required:

- all applicable static tests;
- all applicable Win32 runnables, including lifecycle/reset, Stage13 expiry,
  bridge/ramp safety, Arena/final replay, and ABI/layout gates;
- exact 32-bit DLL build and no-work;
- zero build/test processes at settlement.

## Gate 2 — isolated integrity and attribution

Use the frozen high-pressure map only after candidate/live/map SHA and process
preflight. Every run is a fresh process on the non-interactive isolated desktop,
with fixed low camera, High settings, zero global input, exact native witness,
GPU event/incident capture, and candidate/backup/live conditional restoration.

Minimum correctness conditions:

- exact 4000 report frames;
- zero TDR, device lost, GPU incident, incomplete CSM, receiver reject,
  replay/snapshot identity failure, required-caster omission, and stale-map or
  stale-device publication;
- path-blocker rejection counters close across semantic and native canonical
  paths;
- native RegisterImage/static-stamp counters prove the intended producer gates;
- Stage13 first exposure produces bounded miss/create population, later
  visibility re-entry produces exact hits, identity mismatch remains zero, and
  capacity/reject accounting closes;
- bridge/ramp shadows remain present and do not flicker or disappear at the
  fixed low angle.

Any strong-gate failure rejects the run; no partial result may be interpreted as
product evidence.

## Gate 3 — same-session performance A/B

The preferred design is A1-B1-B2-A2 using a source-consistent base and this
candidate in four fresh processes within one exclusive window. If Stage13 alone
is measured with one binary, A uses
`DXVK_WAR3_STAGE13_CONTENT_PERSISTENT_GEOMETRY=0` and B uses `=1`; all other
environment values must be byte-for-byte identical. This same-binary A/B only
measures Stage13 and cannot be used to claim the native-shadow/path-blocker
changes are faster.

Each run uses 88 seconds (1 second warmup + 87 seconds sample), exact 4000
report frames, `full_default`, High, fixed-low-angle, named pipe,
`section-top-n=512`, zero global input, and the final-shadow-publication-only
allowance. Experimental observers and unrelated product policies remain zero.

Freeze all thresholds before reading run data. Pair A1/B1 and A2/B2 without
fallback; require both pairs and their mean to pass. PostGate, full frame cost,
call density, integrity, GPU, witness, and restoration are co-equal gates; a
local Stage13 win cannot override a PostGate or total-frame regression.

## Gate 4 — player-visible stable acceptance

Isolated desktop results are not player-front performance or visual acceptance.
After Gates 1–3 pass:

- run the visible-desktop high-pressure baseline at at least 85 FPS;
- run the visible-desktop low-pressure baseline at at least 120 FPS;
- compare the fixed low-angle bridge/ramp/path-blocker/native-shadow views;
- obtain explicit player confirmation that shadows are complete and stable and
  that no visual regression was introduced.

Only after all four gates pass may the candidate be promoted to the stable
baseline, copied to the player-deliverable location, and added to root
`CHANGELOG.md`. Candidate, rejected, evidence-only, and invalid outcomes remain
in `docs/agent-history/DEVELOPMENT_CHANGELOG.md` only.
