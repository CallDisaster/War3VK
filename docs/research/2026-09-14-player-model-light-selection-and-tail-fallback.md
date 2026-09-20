# Player model-light report — selection and post-world fallback

Diagnostic checkpoint only. No C++/shader change, build, deployment or game run
was performed in this turn. The player's current DLL/model/map remain untouched.

## Exact evidence

Current player directory: `E:/Work/Warcraft III`.
DLL: 35,476,894 bytes /
`3B0B548A5BE9573005424D31FCD62B36B419C59B8C10D343F209B60EC957452A`.
This is the delivered candidate, not an older player DLL.

CreateNew, byte-verified read-only evidence copies are ignored under
`AutoTest/artifacts/v122_native_lights_20260914/player_report_203145_3b0b/`:

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| war3_d3d9.log | 71,146 | 0862852A6476A1546D3475F08F47F08EB8800C20C5A24A15CAEF04D3260F2C1B |
| TorchHuman.mdx | 4,351 | ED12243B5560E39BD118DF6591738EDF22C2131DA575E8202C62ADB4EF749C3A |
| WorldEditTestMap.w3x | 5,859,965 | DF4482EE2A585CB4FE0814D9D199A4E3ABA59B5C7EA6CEAF0B5158F3718746F4 |

The desktop MDX's Light ObjectID is 2, local pivot `(0,0.420795,150)`, static
attenuation start/end `300/600`. The actual model-load log identifies the SAME
ED12243B bytes at `war3mapimported\torchhuman.mdx`. This is neither the older
32059406 file nor the later intermediate 40B3B21F file. Prior 96/96 static ray
results must not be transferred to this model.

User images examined once, inline:

- `codex-clipboard-e4039c26-091f-42df-ab43-2df470df072c.png`: two torches,
  illumination visible, point-light/point-shadow UI checked; does not prove native
  ownership or any light's successful shadow publication.
- `codex-clipboard-adb6c4a4-97d6-4a0d-af4d-654f14bbc85b.png`: a sharply bounded dark
  region near the right torch. Not accepted as correct caster-specific shadow.

## Confirmed API and selection behavior

The log contains the successful rule:

```text
path=war3mapimported\torchhuman.mdx enabled=1 shadows=1 generation=2
```

The preceding load-time `registered=0` is before this call, not its rejection.
The pasted message's escaping is not evidence of a runtime path error: the
actual log's canonical path matches the loaded resource.

Three sampled selection records:

| Native-world frame | Policy | Generation | Resource | Position |
| ---: | ---: | ---: | --- | --- |
| 121 | 7 | 14 | LanternPost0 | 691.09,-264.709,75.1122 |
| 241 | 2 | 17 | TorchHumanOmni | 295.873,177.994,76.2476 |
| 361 | 2 | 17 | TorchHumanOmni | 295.873,177.994,76.2476 |

None of these records identifies the imported model. The source chooses ONE
generation by nearest projected screen center from the preceding complete census;
explicit JASS registration does not give it priority. A valid registration is
therefore not a guarantee of selection. Do not infer from these sampled records
that the imported lamp was never selected on any unlogged frame.

## Confirmed withdrawal mechanism

There are 11 sampled color fallbacks at native-world frames 481 through 1681,
all `reason=lit-draw-outside-world`, with a 120-frame reporting period. The late
census remains complete with four lights, zero rejection/overflow.

`d3d9_native_light.cpp` rejects a fixed-function lit draw while the native world
scope is inactive, even when a color transaction was already prepared. It marks
the transaction invalid and disables the split; final publication then refuses
that transaction. This retains the original native-lit C0 image with no automatic
shadow, explaining a light remaining visible while its shadow disappears.

There are four sampled receiver-commit records, not proof of exactly four rendered
frames. Receiver records use `input.frameSerial`, while selection/fallback use the
native-world serial; do not equate their numerical frame labels.

The established BeforeUi boundary and the native WorldRenderScene lifetime are
different. The current log proves the rejection branch but does not identify
the later draw's stage/caller/viewport. Portrait/UI 3D rendering or deferred world
work are possibilities, NOT established causes. Do not just remove the guard or
pretend all post-world draws are safe/identical.

## Next implementation and acceptance boundaries

1. Record the rejected draw's exact stage, viewport/RT/DS identity and owner/caller,
   then define a proven tail-color handling boundary; preserve original C0 and
   fail-closed unknown writers. No blind widening of native-light ownership.
2. Make explicit-registration selection semantics visible and predictable;
   consider prioritizing map-author rules within the existing single-light limit,
   with tests for multiple rules, destruction and camera changes. This is not
   authority to pretend a single C1 can represent several independently shadowed
   native contributions.
3. Add mixed built-in/imported fixtures and selection/hover/UI interactions over
   time. The earlier isolated single-custom-light lifecycle scenario did not
   cover this player's sustained mixed-light scene.
4. Verify the actual ED12243B light receives the cube and a known external caster
   produces a stable, correctly aligned shadow. Do not substitute API/count/build
   success or a black patch for this visual acceptance.

This report rejects the current candidate as completion of the requested stable
automatic point-shadow feature. Earlier isolated protocol/lifecycle receipts
remain valid for their narrow cases; they are not expanded or rewritten.
