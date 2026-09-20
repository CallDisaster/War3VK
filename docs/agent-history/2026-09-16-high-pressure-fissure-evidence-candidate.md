# High-pressure fissure evidence candidate

## Outcome

This checkpoint produces a player-facing **diagnostic candidate**, not a stable
release.  It combines the strict skin-palette publication candidate with the
self-contained frame recorder so a normal `war3.exe` launch needs no Python
watcher or control-plane connection.  Native model-light auto registration and
unfinished Water work remain excluded.

The internal recorder profile is reduced specifically for the 32-bit process:

| ring | previous internal profile | this candidate |
| --- | ---: | ---: |
| CPU events | 262,144 (about 100 MiB) | 65,536 (about 25 MiB) |
| raw-input slots | 576 (220.5 MiB upper bound) | 224 (85.75 MiB upper bound) |
| 2560x1440 images | 256 pre + 4 post (about 3.57 GiB device local) | 96 pre + 4 post (about 1.37 GiB device local) |

The retained draw representation is unchanged: exact position/index/blend/UV,
matrix and provenance fields are not truncated.  Only the rolling time window
is shorter.  Existing process-VA/commit/contiguous-region admission and 256 MiB
headroom remain fail-closed.

`warvk_skin_palette_contract_candidate` is a separate Meson option, default
false for ordinary/release builds and true only in this diagnostic build.  An
explicit process value `DXVK_WAR3_SKIN_PALETTE_CONTRACT=0` still requests the
legacy comparison path.

## Build identity

- PE32 / `pei-i386`, architecture `i386`
- 35,884,733 bytes
- SHA-256 `F855A67C61A82686EF2CA4C4CE3E688C1BB7966004DDE014D82B4779EDDA39DD`
- full option-consistent rebuild: 176 actions, Below Normal parent, `-j2`
- two later schema-compatibility rebuilds: exact evidence TU + DLL link
- final exact DLL dry-run: no work

## Verification

The final isolated 2560x1440 run is
`AutoTest/artifacts/self_contained_recorder_runs/lowmem_functional_r5_20260916`.
It used no recorder activation/output environment override and therefore tested
the compiled defaults.  It performed eight camera moves, invoked the installed
window-shortcut route, then let the DLL export without a watcher or frame-control
command.

- `ok=true`, one fresh process, zero global input
- 42 retained images, 1.0108384 seconds before trigger
- 3,930 reconstructed raw-input draws and 16 palette-selection events
- CPU capacity 65,536; history 96+4 / 1000 ms; input slots 224
- natural exit code 0; test DLL restored; protected player DLL/map unchanged
- zero new dump and zero GPU incident
- receipt: 1,440 bytes / `76921C4B7D64BC8C2495A2162E85F9A5D98780C9AD5D0983CF9ED0636E4443E5`

Seven native recorder-memory/default tests, 25 input-analyzer tests and the
directed recorder/history/static checks passed.  `git diff --check` passed.

## Honest limitation

The first isolated attempt used `(4)生与死v1.28读档bug修复.w3x`.  Its direct
`-loadfile` route remained at the menu for 100 seconds (`isInGame=false`) rather
than entering the map.  The process stayed alive, exited naturally, restored all
identities, and produced no dump/GPU incident, but this is **not** a high-pressure
map-entry pass.  The final functional proof used the known AutoTest map.  Manual
entry into the high-pressure map, normal shadow coverage and absence/presence of
the rare fissure remain the player's acceptance gate.

If the player sees a fissure, `Ctrl+Shift+C` freezes the package.  Submit the
complete history directory plus its sibling CPU JSON, input JSON and input BIN;
an image-only report cannot reconstruct the skinning input.

