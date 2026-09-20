# v1.22 candidate — model-path automatic point lighting

## Request and API

User supplied `C:/Users/Administrator/Desktop/Light/TorchHuman.mdx`, 4,351 bytes /
`32059406BBEA8C2416EF03D5D1DAE6AA3A65D3CAB9A9304FB26C66A00072BCED`.
It is intended for map import, NOT an MPQ replacement. Original MPQs, map, model
and player DLL are preserved. Unfinished Water work is excluded.

```jass
// Resource path from the map Import Manager, not a disk path.
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", true, true)
// Keep light, disable its point shadow:
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", true, false)
// Exit WarVK registration/consumption, retain original game lighting:
call WarVKSetModelPointLightsEnabled("war3mapImported\\TorchHuman.mdx", false, false)
```

`WarVKIsModelPointLightRegistered(path)` reports the enabled rule.
`WarVKGetModelPointLightCount(path)` reports registered, loaded, lifetime-bound
native lights, not visible lights or this frame's shadow count. Registration
neither force-loads a model nor manufactures instances. Check the existing
`WarVKGetLastErrorCode()` immediately after mutation. Registration is CPU-only;
the exact supported Game.dll hooks must have installed, but pipeline readiness
is not required. GUI action: **注册模型自动点光与点阴影** (path + two booleans).
Use the updated JASS and GUI catalogs alongside the new DLL.

Rules apply to described existing instances and future clones. Actual mounted
bytes/SHA, parsed native ObjectID/ordinal, clone ownership, nonwrapping per-light
generation, map/frame and evaluated values are checked. Default built-ins still
require exact frozen hashes; custom content requires an explicit map rule.

Supported: MDX800, up to16 Light records, Omni positional lights with finite static
attenuation end > start and <=10000. Native animation/evaluated world position,
color, intensity and visibility remain authoritative. Animated attenuation is
skipped, not guessed from its first key. Missing/unsupported/malformed lights
stay native. An enabled rule can therefore have a bound count of zero.

Paths: <=240 ASCII bytes, case/slash normalized, `.mdl` aliases `.mdx`; absolute
paths, traversal, empty components, control characters and wire delimiters fail.
Bounds:64 rules,256 exact path+SHA catalogs,128 pending documents,4096 models/lights,
128 selected frame entries. Map reset clears CPU registrations; changing a rule
increments a policy generation and invalidates an old pending lease.

## Defaults and render boundary

Producer and consumer default ON in this user-requested candidate; explicit env0
disables them. Sync-only readback optimization and async screenshots retain their
default-on owner/code guards, independent of recording. Existing sun/exit fixes,
Froxel High and local fog are retained; authored effects keep their map settings.

Automatic point shadows currently select at most **one native light per frame**,
prioritizing screen center. Others keep original illumination. Authored JAPI lights
retain priority in the total4-cube/16-light budget. This is not unlimited all-light
shadowing. `shadows=false` retains native light and consumes no automatic cube.

Primary color C0 stays intact. A matched FFP/MRT C1 removes only the selected
native light's direct/specular response. After complete current-frame cube
publication the receiver applies `C0-(C0-C1)*(1-V)`; failures keep C0. No guessed
albedo or unconditional replacement. Unsupported custom writers/programmed draws,
application MRT, RGB-dependent alpha, stale identity and incomplete cubes fall back.

## Validation

- 137 targeted native/capture/point-shadow/JAPI Python/static cases pass. Command
 table count explicitly106→109, retaining exact C++/JASS/UI signature comparison.
- Win32 new path/parser test covers multiple lights, duplicate IDs, bounds, NaN,
 unsupported version/directional/animated range, aliases/rejections and user MDX.
 Existing JAPI protocol runnable passes. Exact generated map JASS passes pjass.
- 53-edge build then3-edge diagnostic shader/object/link build; exact DLL no-work.
 Final PE32/i386: **35,476,894 /
 3B0B548A5BE9573005424D31FCD62B36B419C59B8C10D343F209B60EC957452A**.
 Receiver SPIR-V validates for Vulkan1.3 with actual scalarBlockLayout enabled.
 No32-bit Vulkan validation-layer runtime proof.
- R17 `native_light_japi_defaults_r17`: imported user file, existing/future-instance
 registration, aliases, invalid OS path rejection, shadow toggle and disable pass.
 Counts1/1/2/0, all4 JASS stages. Initial DLL:35476576/0566653B…BDC073D.
 Viewed stage0/1: light visible with shadows OFF; shadows ON still dark near emitter.
- R18 `native_light_japi_depth_r18`: final DLL, debug6 distance visualization.
 Viewed stage0: affected footprint has nearest cube depth <20 world units from
 emitter, not ground depth. This does not identify the exact replay caster alone.
- R19 `native_light_japi_final_r19`: final DLL, no debug/light/sync environment
 switches. Four-stage normal regression passed, four owned1440p async screenshots.
- All three runs: isolated noninteractive2560×1440, global input=false, natural exit0,
 A570 test DLL restored, B1FCC player DLL and original map unchanged; zero GPU events
 or new dumps. Isolated FPS is not player performance. Long play, frontmost visuals,
 all custom models and multi-map lifecycle remain unaccepted.

## Important model-art limitation

The supplied point pivot is `(0,0.420795,103.1175)`; opaque cup top=102.5070.
It is only **0.6105** above the cup. Pinned static geometry finds **96/96** rays
to surrounding ground blocked (old buried model86/96), agreeing with near-emitter
depth evidence. Moving just above a cup does not make downward rays pass through
it. This is not a declaration that every runtime dark pixel's caster is proven.
The user asset is not modified. Compare with shadows OFF, or place its light where
intended receivers have clear lines of sight. Finished shadow art is not accepted.

R14's layout-fixed older build still had zero exact emitter owner matches and a
dark footprint; it is not retrospectively accepted. Original policy8 self-exclusion
requires exact matching and does not apply to arbitrary imported models.

### Later user edit — not the tested bytes

The packaging preflight detected a later save of the same desktop file at
2026-09-14 11:56:00 UTC: 4,351 bytes /
`40B3B21FAF825C027562C98D7E5A93EF9B69698FCBDACF0A33363302A047DFFF`.
The geometry and R17/R18/R19 conclusions above apply only to `32059406`, not this
new revision. No model is bundled with the DLL/SDK delivery; import the user's
chosen revision through the editor. Neither version has been overwritten by us.
The candidate API accepts supported custom model content by explicit path rule,
not by pinning every custom model to the test file's SHA.

## Fixture-tool correction

R15/R16 map preparation stopped before game launch. C++ bool was incorrectly
marshalled as four-byte BOOL, falsely reporting missing resources as present.
The helper now uses I1, a mandatory missing sentinel and exact imported-byte
readback, following the [StormLib declaration](https://github.com/ladislav-zezula/StormLib/blob/master/src/SFileOpenFileEx.cpp).
New fixtures use unique imports/CreateNew copies; old failed artifacts remain.
No resource was overwritten and no parser gate was weakened. Old successful
script byte readbacks remain evidence, not proof of old failure-return handling.
