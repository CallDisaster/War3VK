#!/usr/bin/env python3
"""Two-mode acceptance of the shadow pass (x64 migration, phase 4c-1: static
casters, the sun's cascades and the receiver; phase 4c-3: its lifecycle;
phase 4c-2: more casters; phase 4d: the unit outline, the volumetric light,
its media and the contact shadows).

One fixture (test_render_host_d3d9_shadow.cpp) issues the same D3D9 draws,
scene records, caster records and stage markers through
  - the x86 in-process renderer: the device's own registered shadow pass;
  - the x86 proxy and the x64 host: the same pass source, compiled into the
    host and fed by the wire.
Every frame is read back. The modes must agree byte for byte. Each arm has
exact expected CSM draws per layer and copied bytes. The remaining accounts
must match. Shadows must change the picture and leave the UI untouched. The receipt also
accounts x86 memory and frame time per mode; those are numbers, not
thresholds.

What the pass does with a caster set it cannot use is its contract, pinned
here: no new shadow map is published from an empty set or from an incomplete
one (no ShadowFrame record, a count that names a caster that never arrived).
The last map stays. A caster drawn from dynamic buffers that the caller
rewrites before BeforeUi is shadowed as it was drawn: both executors preserve
the indexed positions used by the draw. A caster drawn alpha-tested is shadowed
alpha-tested when its draw carries a ShadowCasterAlpha record: the texture is
the draw's own, and the shadow has the holes the draw has. Shadow TAA asked
for keeps a history from one frame to the next, and a new session leaves none.
A frame's point lights arrive as a PointLights record: a lamp lights the boxes,
and when it casts shadows the pass renders its cube map from the same casters.
The unit outline arrives as an Outline record that names the objects to
outline by handle: the pass outlines the casters with that batchHandle, by
their screen-space edges or by their geometry. The volumetric light arrives as
a VolumetricLight record: the shadow pass renders the light's own sun map, and
the volumetric light pass runs after it, with its input, through the froxel
grid or by the ray march, and lights the air with the lamp as well. Its media:
a FogVolumes record, built into the data layer's fog volume manager at either
end, scatters the light in a turned box of fog; the frame's Snow record gives
the weather's haze and low snow, which the volumetric light pass scatters
without a VolumetricLight record (and the same weather without them draws
nothing). With the lamp's contact shadows switched on in the Lighting record
the pass traces them through the depth.

The last map goes when the x86 data layer says so, in both modes at the same
frame: the tombstone serial or the policy revision of a ShadowFrame record
changed, the session was reset (taken at a Present), the device was Reset. The
empty set that follows each of these draws the world. Every run of the pass is
compared field by field with what its frame should have made of it, the data
layer's facts the pass read included, and the pass owns the same storage after
every run in both modes. A second pair of arms repeats the frames with the
shadow TAA module switched off - in the records, and in process in the runtime
profile - and adds a frame that asks for shadow TAA.

No game process, no player DLL, an isolated non-input desktop, one fixture at
a time.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import render_lab_vvl365 as vvl
from run_render_host_validation_gate import check_loader
from render_lab_layer_source import create_empty_source, empty_directory_witness
from run_d3d9_draw_batch_gate import (ROOT, COMPILERS, PROBES, PLAYER, identity, pe_identity, require,
    strict_json, child_settled, execute, sources, IsolatedRenderDesktop, validate_probe_receipt,
    check_launch, json_lines, singleton, ENV_ALLOW, capture_raw_artifacts, verify_raw_artifacts, prefixed)
from run_d3d9_factory_clear_gate import (guarded_identity, loader_file_version, command_ring_rows,
    STREAM_ENV, COMMAND_RING_ENV)
from package_x64_proxy_beta_20261118 import FRONT, FRONT_SHA256

FIXTURE = ROOT/'AutoTest/test_render_host_d3d9_shadow.cpp'
SAMPLES = ('world', 'shadows', 'lifecycle', 'timed', 'closed')
WIDTH = HEIGHT = 64
UI_PIXELS = 12*12
TIMED_FRAMES = 12
CASCADES = 4


# What a run of the pass makes of a frame (war3_shadow_scene_input.h
# ShadowRunFacts), by what the frame gave it.
# The volumetric light pass did not run (war3_shadow_scene_input.h
# AddVolumetricRunFacts).
NO_VOLUMETRIC = dict(volumetric=0, volumetricBackend=-1, volumetricComposite=0, volumetricFogVolumes=0,
                     volumetricWeather=0)


def published(casters, alpha=0, point=0, outline=0, volumetric=None, fog=0, weather=0):
    """A complete set: a new shadow map, every caster into each cascade, the
    receiver. With no caster the map is rendered empty. alpha: the casters
    prepared alpha-tested. point: the receiver had point lights with point
    shadows switched on. outline: the run drew the unit outline. volumetric:
    the volumetric light ran after it and composited, drawing with this
    War3VolumetricQuality (0 the ray march, 2 the high froxel grid). fog: the
    fog volumes it scattered in. weather: the volumetric light pass ran for the
    weather medium alone (no VolumetricLight record) and composited it."""
    ran = dict(NO_VOLUMETRIC)
    if volumetric is not None:
        ran.update(volumetric=1, volumetricBackend=volumetric, volumetricComposite=1, volumetricFogVolumes=fog)
    if weather:
        ran.update(volumetric=1, volumetricWeather=1)
    return dict(casters=casters, replayDraws=casters, drawnCasters=CASCADES*casters, cascades=CASCADES,
                cascadeDrawn=[casters]*CASCADES, mapExecuted=1, receiverExecuted=1, earlyReturn=0, inputReject=0,
                replayRejected=0, completeMap=1, alphaPrepared=alpha, taaHistory=0, taaInvalidation=0,
                pointShadow=point, outline=outline, **ran)


def held(casters=0, rejected=0):
    """No new map: the receiver draws with the last one."""
    return dict(casters=casters, replayDraws=casters, drawnCasters=0, cascades=CASCADES, cascadeDrawn=[0]*CASCADES,
                mapExecuted=0, receiverExecuted=1, earlyReturn=0, inputReject=0, replayRejected=rejected, completeMap=1,
                alphaPrepared=0, taaHistory=0, taaInvalidation=0, pointShadow=0, outline=0, **NO_VOLUMETRIC)


# A sun without strength: the pass returns before it looks at the casters.
NO_STRENGTH = dict(casters=0, replayDraws=0, drawnCasters=0, cascades=0, cascadeDrawn=[0]*CASCADES, mapExecuted=0,
                   receiverExecuted=0, earlyReturn=3, inputReject=0, replayRejected=0, completeMap=0, alphaPrepared=0,
                   taaHistory=0, taaInvalidation=0, pointShadow=0, outline=0, **NO_VOLUMETRIC)
# The fixture's frames in order (see its Run): the name, the caster records it
# sends, and the run of the pass; None where no record asks for shadows.
SPEC = (
    ('world', 0, None),
    # Shadow TAA asked for (taaMode below); a session reset before the third.
    ('taa-first', 3, published(3)), ('taa-history', 3, published(3)), ('taa-session', 3, published(3)),
    ('shadow', 3, published(3)), ('shadow-again', 3, published(3)),
    ('cascade-edge-control', 1, published(1)), ('cascade-edge', 3, published(3)), ('sun-moved', 3, published(3)),
    ('sun-back', 3, published(3)), ('terrain', 4, published(4)), ('one-caster', 1, published(1)),
    # An empty set and two incomplete ones (no ShadowFrame record, a count
    # one too high): each incomplete one is one refused replay. Then a box in
    # dynamic buffers, rewritten after its draw: a complete set. Then box 0
    # alpha-tested, without and with its companion record. Then a lamp with
    # point shadows switched on, its own shadows first off (the receiver has
    # point shadows on as soon as there is a lamp), then on.
    ('no-casters', 0, held()), ('unsealed', 3, held(3, 1)), ('short-count', 2, held(2, 1)), ('dynamic', 3, published(3)),
    ('alpha-off', 3, published(3)), ('alpha', 3, published(3, alpha=1)),
    ('point-light', 3, published(3, point=1)), ('point-shadow', 3, published(3, point=1)),
    # Box 1 outlined, by its screen-space edges, then by its geometry.
    ('outline', 3, published(3, outline=1)), ('outline-geometry', 3, published(3, outline=1)),
    # The volumetric light through the high froxel grid, the same frame again,
    # by the ray march, and with the lamp (whose receiver has point shadows on).
    ('volumetric', 3, published(3, volumetric=2)), ('volumetric-again', 3, published(3, volumetric=2)),
    ('volumetric-march', 3, published(3, volumetric=0)), ('volumetric-lamp', 3, published(3, point=1, volumetric=2)),
    # Its media: one fog volume on (of two), the weather's haze and low snow
    # alone, the same weather without them; then the lamp's contact shadows,
    # its cube shadows switched off: traced in the receiver, then from the
    # half-resolution depth pyramid the pass's compute passes build.
    ('fog-volume', 3, published(3, volumetric=2, fog=1)), ('weather-medium', 3, published(3, weather=1)),
    ('weather-clear', 3, published(3)), ('point-contact', 3, published(3)),
    ('point-contact-hiz', 3, published(3)),
    ('strength-zero', 3, NO_STRENGTH), ('shadow-ssao-smaa', 3, published(3)), ('off', 0, None),
    ('shadow-return', 3, published(3)),
    # Lifecycle. After the tombstone serial, the policy revision, the session
    # or the device changed, the empty set finds no map to hold: the pass
    # renders an empty one.
    ('tombstone-held', 0, published(0)), ('tombstone-fresh', 3, published(3)), ('facts-kept', 3, held(3, 1)),
    ('policy-held', 0, published(0)), ('policy-fresh', 3, published(3)),
    ('time-9', 3, published(3)), ('clock-held', 3, published(3)), ('time-15', 3, published(3)),
    ('time-return', 3, published(3)),
    ('session-pending', 0, held()), ('session-held', 0, published(0)), ('session-fresh', 3, published(3)),
    ('map-2', 3, published(3)), ('map-3', 3, published(3)), ('map-4', 3, published(3)),
    ('reset-held', 0, published(0)), ('reset-fresh', 3, published(3)))
# With the shadow TAA module off the fixture adds a frame that asks for it.
TAA_SPEC = SPEC+(('taa-requested', 3, published(3)),)
# The frames that ask for shadow TAA. With the module on the pass runs them in
# its Temporal mode (War3ShadowTaaMode 2), and what it did with the history
# (war3_shadow_scene_input.h ShadowTaaHistoryBit): the motion vector and
# visibility passes ran, the receiver wrote a history and it advanced; the
# second also found one and blended it in. A new session leaves none.
TAA_FRAMES = ('taa-first', 'taa-history', 'taa-session')
TAA_TEMPORAL = 2
TAA_WROTE = 1 | 2 | 8 | 16
TAA_READ = TAA_WROTE | 4 | 32
TAA_HISTORY = {'taa-first': TAA_WROTE, 'taa-history': TAA_READ, 'taa-session': TAA_WROTE}
# Why the pass dropped a shadow TAA history (its kShadowTaaInvalidate* bits),
# by frame and module switch. The first run creates the shadow map (and with
# the module on the TAA images); the first frame that asks for shadow TAA
# differently from the frame before is a mode switch; a moved tombstone serial
# or policy revision is a lifecycle change.
LIFECYCLE, MAP_RESOURCE, TAA_RESOURCE, MODE_SWITCH = 1 << 0, 1 << 8, 1 << 9, 1 << 11
TAA_INVALIDATION = {
    False: {'taa-first': MAP_RESOURCE | TAA_RESOURCE, 'shadow': MODE_SWITCH,
            'tombstone-held': LIFECYCLE, 'policy-held': LIFECYCLE},
    True: {'taa-first': MAP_RESOURCE, 'shadow': MODE_SWITCH, 'tombstone-held': LIFECYCLE,
           'policy-held': LIFECYCLE, 'taa-requested': MODE_SWITCH}}
# Frames without a ShadowFrame record.
UNSEALED = ('unsealed', 'facts-kept')
# The data layer's facts a frame's ShadowFrame record carries: the tombstone
# serial is 1 from 'tombstone-held' on, the policy revision 2 from
# 'policy-held' on, the time of day 12 but in the two frames named after
# theirs. A frame without the record leaves them as they were.
TIME_OF_DAY = {'time-9': 9.0, 'time-15': 15.0}
# Each arm copies both marked dynamic boxes in 'cascade-edge' and one in
# 'dynamic'. Each box has 24 vertices and 36 16-bit indices.
CASTERS_REFUSED = REFUSED_DYNAMIC = 0
CASTERS_COPIED = 3
REFERENCE_COPIED_BYTES = CASTERS_COPIED*(24*16+36*2)
# The two edge boxes share the same frozen IB. The box at X=100000
# exceeds the folded transform's float error bound and stays float3.
HOST_COPIED_BYTES = CASTERS_COPIED*24*12 + 2*36*2
HOST_QUANTIZED_BYTES = 24*(8+8+12) + 2*36*2
# The 'alpha' frame's companion record.
ALPHA_RECORDS = 1
# The two point frames' PointLights records, the two outline frames' Outline
# records.
POINT_LIGHT_RECORDS = 2
OUTLINE_RECORDS = 2
# The four volumetric frames' VolumetricLight records, and the lamp's
# PointLights record.
VOLUMETRIC_RECORDS = 4+1
# 'fog-volume''s VolumetricLight and FogVolumes records, the two contact
# frames' PointLights records.
MEDIA_RECORDS = 2
CONTACT_RECORDS = 2
# The frames at which the storage the pass owns grows, and keeps from then on:
# the first whose lamp casts shadows (the pass creates its point shadow cube)
# the first with a screen-space outline (its two masks), the first with the
# volumetric light (the light's own sun map) and the first with contact
# shadows from the depth pyramid (the pyramid and the visibility it traced,
# kept only when the compute passes produced this frame's). Before, between
# and after them the storage is one value each.
STORAGE_GROWS = ('point-shadow', 'outline', 'volumetric', 'point-contact-hiz')
# 5 draws a frame: the ground, three boxes, the UI. 12 refused records of
# 4c-1, four refused session reset requests, two refused alpha records.
REFUSALS = 18
# Session resets, each taken at a Present (one between the shadow TAA frames);
# one more Present after the device Reset.
SESSION_RESETS = 5
DEVICE_RESETS = 1
PRESENTS = SESSION_RESETS+DEVICE_RESETS
# 'shadow-ssao-smaa' carries an SSAO and an AA record.
EFFECT_RECORDS = 2+2
# Frames that must not differ from the world drawn without records: no
# records, no strength, and the empty set after each lifecycle change.
ENDED = ('tombstone-held', 'policy-held', 'session-held', 'reset-held')
# The last frame that brings something new into the host: 'shadow-ssao-smaa'
# before it is the first to run the SSAO and SMAA executors. From this frame
# on - every lifecycle frame and the timed ones - the host's device allocator
# has nothing to add.
SETTLED = 'shadow-return'
UNCHANGED = ('world', 'off', 'strength-zero')+ENDED
ACCOUNT = ('runs', 'mapRenders', 'receiverDraws', 'castersUsed', 'castersRefused', 'refusedDynamic',
           'refusedLayout', 'refusedRange', 'castersCopied', 'copiedBytes', 'castersDrawn', 'replayRejected',
           'peakResourceBytes', 'last')
ANNOTATE_ENV = dict(WARVK_D3D9_ANNOTATIONS='1')
REFERENCE_ENV = dict(DXVK_WAR3_SCENE_EFFECTS_FIXTURE='1')


def float_bits(value):
    return struct.pack('<I', struct.unpack('<I', struct.pack('<f', value))[0])[::-1].hex()


class Scenario:
    """What one run of the fixture sends and what the pass makes of it."""

    def __init__(self, taa_module_off, exact_cull=False):
        self.taa_module_off = taa_module_off
        spec = TAA_SPEC if taa_module_off else SPEC
        self.frames = tuple(name for name, _, _ in spec)
        self.draws = 5*(len(self.frames)+2*TIMED_FRAMES)
        self.caster_draws = sum(sent for _, sent, _ in spec)+3*TIMED_FRAMES
        # Every run in order, with the facts the pass read; the timed shadow
        # frames come last and mark the three boxes.
        self.runs = []
        serial, revision = 0, 1
        for name, _, run in spec:
            if run is None:
                continue
            if name == 'cascade-edge' and exact_cull:
                # Static box in all four. Edge box in C1..3. Far box in none.
                run = dict(run, drawnCasters=7, cascadeDrawn=[1, 2, 2, 2])
            if name == 'dynamic' and exact_cull:
                # After the moved sun, box 0 starts at light Y=1.079 in C0.
                run = dict(run, drawnCasters=11, cascadeDrawn=[2, 3, 3, 3])
            serial = 1 if name == 'tombstone-held' else serial
            revision = 2 if name == 'policy-held' else revision
            # A frame without the record follows one whose time of day was 12.
            self.runs.append(dict(run, taaModule=0 if taa_module_off else 1,
                                  taaMode=0 if taa_module_off or name not in TAA_FRAMES else TAA_TEMPORAL,
                                  taaHistory=0 if taa_module_off else TAA_HISTORY.get(name, 0),
                                  taaInvalidation=TAA_INVALIDATION[taa_module_off].get(name, 0),
                                  gameTimeBits=float_bits(TIME_OF_DAY.get(name, 12.0)),
                                  tombstoneSerial=serial, stagePolicyRevision=revision))
        # The timed frames repeat the last one without asking for shadow TAA:
        # after the module-off arm's request the first of them is a mode switch.
        timed = dict(self.runs[-1], taaInvalidation=0)
        self.runs += [dict(timed, taaInvalidation=MODE_SWITCH if taa_module_off else 0)]+[timed]*(TIMED_FRAMES-1)
        self.shadow_frames = tuple(name for name, _, run in spec if run is not None)
        # One Lighting record a shadow frame, a ShadowFrame record in all but
        # the unsealed ones, and the point frames' PointLights records.
        self.shadow_records = (2*len(self.runs)-len(UNSEALED)+POINT_LIGHT_RECORDS+OUTLINE_RECORDS
                               +VOLUMETRIC_RECORDS+MEDIA_RECORDS+CONTACT_RECORDS)
        self.shadowed = tuple(name for name in self.frames if name not in UNCHANGED)
        self.account = dict(
            runs=len(self.runs), mapRenders=sum(r['mapExecuted'] for r in self.runs),
            receiverDraws=sum(r['receiverExecuted'] for r in self.runs),
            castersUsed=self.caster_draws-CASTERS_REFUSED, castersRefused=CASTERS_REFUSED,
            refusedDynamic=REFUSED_DYNAMIC, refusedLayout=0, refusedRange=0,
            castersCopied=CASTERS_COPIED,
            castersDrawn=sum(r['drawnCasters'] for r in self.runs),
            replayRejected=sum(r['replayRejected'] for r in self.runs))


SCENARIOS = {False: Scenario(False), True: Scenario(True)}
# label, in-process reference, stream profile, command ring, validated under
# VVL, shadow TAA module off. The fifth arm repeats the stream arm without the
# validation layer: its times are the ones to read; the layer makes the others
# slow.
ARMS = (('shadow-reference', True, False, False, False, False),
        ('shadow-remote', False, False, False, True, False),
        ('shadow-remote-stream', False, True, False, True, False),
        ('shadow-remote-stream-ring', False, True, True, True, False),
        ('shadow-remote-stream-plain', False, True, False, False, False),
        ('shadow-reference-taa-off', True, False, False, False, True),
        ('shadow-remote-stream-taa-off', False, True, False, True, True))
REFERENCE = {False: 'shadow-reference', True: 'shadow-reference-taa-off'}


def image_difference(a, b):
    """Differing pixels and the largest channel difference of two BGRA images."""
    pixels = sum(a[i:i+4] != b[i:i+4] for i in range(0, len(a), 4))
    return dict(pixels=pixels, maxChannel=max((abs(x-y) for x, y in zip(a, b)), default=0))


def remote_environment(pairs):
    """--remote-env NAME=VALUE: x64 switch variables for the remote arms only."""
    result = {}
    for pair in pairs:
        name, separator, value = pair.partition('=')
        if not separator or not name.startswith('WARVK_D3D9_') or not value:
            raise ValueError('--remote-env takes WARVK_D3D9_NAME=VALUE')
        result[name] = value
    return result

BUDGET_FRAMES = ('world', 'near-only', 'far-only', 'complete', *(f'age{i}' for i in range(1, 11)), 'recovered')


def check_budget_pixels(frames, baseline, temporal):
    # Locate interior shadow pixels from independent controls. Neither a
    # still-dark far box surface nor the colour of the UI qualifies.
    world, near, far, full = (frames[k] for k in BUDGET_FRAMES[:4])
    witnesses = []
    for own, other in ((near, far), (far, near)):
        darkness = {i: sum(world[i:i+3]) - sum(own[i:i+3]) for i in range(0, len(world), 4)
                    if world[i:i+4] == other[i:i+4] and own[i:i+4] == full[i:i+4]}
        peak = max(darkness.values(), default=0)
        points = [i for i, d in darkness.items() if d >= max(12, peak * .75)]
        require(len(points) >= 2, 'two isolated interior pixels per caster')
        witnesses.append(points)
    require(all(frames[k] == baseline[k] for k in BUDGET_FRAMES[:4]), 'exact pixels before overflow')
    # Restarting temporal history changes shadow edge pixels by at most 8
    # channel levels. In g3 that was at most 71 of 4096 pixels, and 26 with
    # only the near shadow. The contact shadows of f15 lengthen those edges,
    # so the pixel limits allow for them; the level limit is unchanged.
    def compare(data, expected, pixel_limit, name):
        diff = image_difference(data, expected)
        require(diff['pixels'] <= (pixel_limit if temporal else 0) and
                diff['maxChannel'] <= (8 if temporal else 0), f'{name}: {diff}')

    for name in BUDGET_FRAMES[4:]:
        data = frames[name]
        far_lit = name in ('age9', 'age10')
        if far_lit:
            compare(data, near, 40, name + ' against the near-only control')
        else:
            compare(data, full, 96, name + ' against both shadows')
            compare(data, baseline[name], 96, name + ' against uninterrupted rendering')
        for points, shadow_control, lit in ((witnesses[0], near, False), (witnesses[1], far, far_lit)):
            expected = world if lit else shadow_control
            for i in points:
                distance = max(abs(data[i+k]-expected[i+k]) for k in range(3))
                require(distance <= (8 if temporal else 0), f'{name} shadow witness {i//4}: {distance}')
                if not lit:
                    require(distance < max(abs(data[i+k]-world[i+k]) for k in range(3)),
                            f'{name} shadow missing at {i//4}')
    compare(frames['age9'], frames['age10'], 40, 'stable near-only coverage')
    return {'nearPixels': len(witnesses[0]), 'farPixels': len(witnesses[1])}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--build-receipt', type=Path, required=True, help='x64 host build receipt')
    ap.add_argument('--reference-dll', type=Path, required=True, help='fresh isolated x86 product candidate')
    ap.add_argument('--frozen-proxy', type=Path, default=FRONT, help='packaged f8 x86 front DLL')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--validation', action='store_true', help='pinned VVL365 on the validated remote arms')
    ap.add_argument('--repin-loader', action='store_true', help='with --validation: accept the current system vulkan-1.dll')
    ap.add_argument('--remote-env', action='append', default=[], metavar='WARVK_D3D9_NAME=VALUE',
                    help='extra variable for the remote (proxy and host) arms only, e.g. a switch turned off for a control round')
    ap.add_argument('--ring-env', action='append', default=[], metavar='WARVK_D3D9_NAME=VALUE',
                    help='extra variable for the remote arms that run the stream profile over the command ring only '
                         '(the ordered asynchronous switches are refused anywhere else)')
    args = ap.parse_args()
    out = args.output.resolve()
    if args.repin_loader and not args.validation:
        ap.error('--repin-loader needs --validation')
    reference_dll = args.reference_dll.resolve()
    require(reference_dll.is_relative_to(ROOT) and reference_dll.suffix.lower() == '.dll', 'isolated reference DLL below repository')
    require(out.parent == ROOT/'AutoTest/artifacts' and not out.exists(), 'fresh output only')
    out.mkdir()
    receipt = dict(schema='warvk-x64-shadow-gate/v2', ok=False, gameTouched=False,
                   fullRendererMigrated=False, steps=[], cases=[], raw=[], copies=[])
    remote_extra = remote_environment(args.remote_env)
    receipt['remoteEnvironment'] = remote_extra
    ring_extra = remote_environment(args.ring_env)
    receipt['ringEnvironment'] = ring_extra
    frozen = []
    env = {k: v for k, v in os.environ.items() if k.upper() in ENV_ALLOW}
    env.update(DXVK_LOG_LEVEL='info', DXVK_SHADER_CACHE='0')

    def step(argv, label):
        try:
            receipt['steps'].append(execute(argv, ROOT, out/label, 180, env))
        finally:
            receipt['raw'].append(capture_raw_artifacts(out, [label+'.stdout', label+'.stderr']))

    def copy(a, b):
        before = identity(a)
        shutil.copy2(a, b)
        after = identity(b)
        require((before['bytes'], before['sha256']) == (after['bytes'], after['sha256']), 'exact copy')
        receipt['copies'].append(after)

    try:
        build = strict_json(args.build_receipt.read_text('utf-8'))
        require(build['ok'] and build['sourcesPreserved'], 'frozen host build')
        require(all(identity(Path(r['path'])) == r for r in build['sources']), 'host source drift')
        host = Path(build['host']['path'])
        require(identity(host) == build['host'], 'host artifact drift')
        frozen = [identity(p) for p in sources()]
        frozen += [guarded_identity(p) for p in (Path(__file__).resolve(), FIXTURE,
            ROOT/'AutoTest/test_render_host_d3d9_parent.cpp', ROOT/'src/d3d9/war3/effects/war3_effect_scene.h',
            ROOT/'src/d3d9/war3/effects/war3_shadow_scene.h', ROOT/'src/d3d9/war3/effects/war3_shadow_scene_input.h',
            PLAYER, reference_dll, host, PROBES/'desktop-probe-32.exe', PROBES/'desktop-probe-64.exe')]
        receipt['referenceDll'] = identity(reference_dll)
        receipt['host'] = build['host']
        frozen += [identity(Path(m.__file__).resolve()) for m in list(sys.modules.values())
                   if getattr(m, '__file__', None) and Path(m.__file__).suffix == '.py'
                   and Path(m.__file__).resolve().is_relative_to(ROOT/'AutoTest')]
        receipt['validationEnabled'] = args.validation
        if args.validation:
            pinned = ROOT/'AutoTest/artifacts/x64-dispatch-vvl365-candidate-20261023-r1/receipt.json'
            layer = strict_json(pinned.read_text('utf-8'))['layer']
            if args.repin_loader:
                current = identity(Path(layer['loader']['path']))
                receipt['loaderRepin'] = dict(pinned=layer['loader'], current=current, changed=current != layer['loader'],
                                              version=loader_file_version(Path(current['path'])))
                layer = dict(layer, loader=current)
            for kind in ('manifest', 'library', 'loader'):
                require(identity(Path(layer[kind]['path'])) == layer[kind], 'pinned VVL identity '+kind)
                frozen.append(layer[kind])
            frozen.append(identity(pinned))
            receipt['layer'] = layer
            manifest = Path(layer['manifest']['path'])
            library = Path(layer['library']['path'])
            loader = Path(layer['loader']['path'])
            loader_version = receipt['loaderRepin']['version'] if args.repin_loader else None
            vvl.check_manifest(strict_json(manifest.read_text('utf-8')))
            receipt['implicitSourceBefore'] = create_empty_source(out)
            empty = Path(receipt['implicitSourceBefore']['path'])
        receipt['inputs'] = frozen.copy()

        cc = COMPILERS/'i686-w64-mingw32-clang++.exe'
        flags = ['-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-pthread', '-D_WIN32_WINNT=0x0a00']
        proxy = args.frozen_proxy.resolve()
        require(identity(proxy)['sha256'].lower() == FRONT_SHA256, 'frozen f8 front identity')
        receipt['proxy'] = identity(proxy)
        fixture = out/'shadow32.exe'
        step([cc, *flags, '-municode', FIXTURE, ROOT/'tools/render_host/process_memory_probe.cpp',
              '-luser32', '-lpsapi', '-o', fixture], 'build-fixture')
        require(pe_identity(proxy) == pe_identity(fixture) == (0x14c, 0x10b), 'x86 COM caller')
        frozen += [identity(proxy), identity(fixture)]

        images = {}
        accounts = {}
        run_logs = {}
        volumetric_bytes = {}
        with IsolatedRenderDesktop(restricted_medium=True) as desktop:
            receipt['desktopSafety'] = desktop.receipt
            probe = desktop.run([PROBES/'desktop-probe-32.exe', '--expect-desktop', desktop.desktop_name,
                '--expect-station', 'WinSta0', '--input-desktop', desktop.input_before,
                '--child', PROBES/'desktop-probe-64.exe', '--child-log', out/'probe-child.log'], cwd=out,
                stdout_path=out/'probe.stdout', stderr_path=out/'probe.stderr', timeout_s=30, env=env)
            receipt['probe'] = probe
            receipt['raw'].append(capture_raw_artifacts(out, ['probe.stdout', 'probe.stderr', 'probe-child.log']))
            check_launch(probe, desktop.desktop_name, desktop.input_before)
            rows = json_lines(out/'probe.stdout')+json_lines(out/'probe-child.log')
            require(len(rows) == 2, 'both probes')
            validate_probe_receipt(rows[0], station='WinSta0', desktop=desktop.desktop_name,
                                   input_desktop=desktop.input_before, bits=32, child=rows[1], launch=probe)

            for label, reference, stream, ring, validated, taa_off in ARMS:
                folder = out/label
                folder.mkdir()
                exe = folder/'fixture.exe'
                dll = folder/'renderer.dll'
                copy(fixture, exe)
                copy(reference_dll if reference else proxy, dll)
                config = folder/'pipeline.conf'
                config.write_text('', encoding='utf-8')
                frozen.append(identity(config))
                local = env | dict(DXVK_LOG_PATH=str(folder), DXVK_CONFIG_FILE=str(config))
                if reference:
                    local.update(REFERENCE_ENV)
                else:
                    copy(host, folder/'host.exe')
                    local.update(ANNOTATE_ENV, WARVK_D3D9_HOST_EXE=str(folder/'host.exe'),
                                 WARVK_D3D9_HOST_LOG=str(folder/'host.log'))
                    local.update(remote_extra)
                if stream:
                    local.update(STREAM_ENV)
                if ring:
                    local.update(COMMAND_RING_ENV)
                    if stream and not reference:
                        local.update(ring_extra)
                exact_cull = not reference and local.get('WARVK_D3D9_SHADOW_EXACT_CULL') != '0'
                scenario = Scenario(taa_off, exact_cull)
                validate = args.validation and validated
                if validate:
                    local.update(vvl.validation_env(folder, manifest, empty))
                case = dict(label=label, ok=False, reference=reference, stream=stream, commandRing=ring, validated=validate,
                            taaModuleOff=taa_off)
                receipt['cases'].append(case)
                case['process'] = desktop.run([exe, '--dll', dll, '--scenario', 'normal']+(['--reference'] if reference else [])
                    +(['--taa-module-off'] if taa_off else []),
                    cwd=folder, stdout_path=folder/'case.stdout', stderr_path=folder/'case.stderr', timeout_s=180, env=local)
                frame_files = [f'frame-{name}.bgra' for name in scenario.frames]
                present = [name for name in frame_files if (folder/name).is_file()]
                receipt['raw'].append(capture_raw_artifacts(folder, ['case.stdout', 'case.stderr']+present
                    +([] if reference else ['host.log'])+(['validation.raw.log'] if validate else []), True))
                check_launch(case['process'], desktop.desktop_name, desktop.input_before)
                rows = json_lines(folder/'case.stdout')
                report = singleton(rows, 'warvk-d3d9-shadow-fixture/v2')
                require(report['ok'] and report['normalTeardown'] and report['memoryMeasured'] is True
                        and report['reference'] is reference and report['gameTouched'] is False
                        and report['taaModuleOff'] is taa_off
                        and report['x86VulkanAbsent'] is (not reference), 'complete shadow fixture')
                require(report['draws'] == scenario.draws and report['casterDraws'] == scenario.caster_draws
                        and report['refusals'] == REFUSALS and report['timedFrames'] == TIMED_FRAMES, 'fixture coverage')
                require(report['presents'] == PRESENTS and report['sessionResets'] == SESSION_RESETS
                        and report['deviceResets'] == DEVICE_RESETS, 'fixture lifecycle coverage')
                # The in-process renderer's Present leaves a window procedure
                # chain that leads into it (the fixture's release stage); the
                # proxy leaves the window alone.
                require(report['windowProcedureRestored'] is reference, 'whose window procedure was left behind')
                require(tuple(f['name'] for f in report['frames']) == scenario.frames
                        and all(f['uiPixels'] == UI_PIXELS for f in report['frames']), 'every frame, UI untouched')
                changed = {f['name']: f['changed'] for f in report['frames']}
                require(all(changed[name] == 0 for name in UNCHANGED)
                        and all(changed[name] > 0 for name in scenario.shadowed), 'shadows change the world, absence does not')
                require(tuple(s['name'] for s in report['samples']) == SAMPLES
                        and all(s['memory']['valid'] for s in report['samples']), 'complete x86 memory samples')
                require(present == frame_files, 'every frame image written')
                images[label] = {name: (folder/f'frame-{name}.bgra').read_bytes() for name in scenario.frames}
                require(all(len(data) == WIDTH*HEIGHT*4 for data in images[label].values()), 'frame image extent')
                case['result'] = report
                if reference:
                    accounts[label] = {k: report['shadow'][k] for k in ACCOUNT}
                    run_logs[label] = report['shadow']['runLog']
                else:
                    pr = singleton(rows, 'warvk-d3d9-proxy/v1')
                    hs = singleton(json_lines(folder/'host.log'), 'warvk-d3d9-host/v1')
                    case['host'] = hs
                    case['proxy'] = pr
                    require(pr['peerVerified'] and pr['x86VulkanAbsent'] and pr['childNaturalExit'] and pr['childProcessSettled']
                            and not pr['transportFailed'] and not pr['deviceLost'] and pr['childExit'] == 0, 'healthy remote shutdown')
                    require(hs['ok'] and hs['bits'] == 64 and hs['normalTeardown'] and hs['liveResources'] == 0
                            and hs['submitted'] == hs['gpuCompleted'] == hs['retired'] and hs['draws'] == scenario.draws,
                            'host ownership closed')
                    # Every session reset was asked for, sent ahead of a
                    # Present and taken by the host.
                    ap_ = singleton(prefixed(folder/'case.stderr', 'D3D9 annotation-proxy: '), 'warvk-d3d9-annotation-proxy/v1')
                    ah = singleton(prefixed(folder/'host.log', 'D3D9 annotation-host: '), 'warvk-d3d9-annotation-host/v1')
                    require(ap_['sessionResetsRequested'] == ap_['sessionResetsSent'] == SESSION_RESETS
                            and ap_['sessionResetsRefused'] == 0 and ah['sessionResets'] == SESSION_RESETS
                            and ah['sessionGeneration'] == SESSION_RESETS, 'session resets reached the host')
                    case['sessionResets'] = dict(proxy=ap_, host=ah)
                    require(pr['exchanges'] == hs['accepted']+hs['rejected'], 'ordered command balance')
                    case['commandRing'] = command_ring_rows(prefixed(folder/'case.stderr', 'D3D9 command-ring-proxy: '),
                        prefixed(folder/'case.stderr', 'D3D9 terminal: '), prefixed(folder/'host.log', 'D3D9 command-ring-host: '),
                        ring, pr, hs)
                    sh = singleton(prefixed(folder/'host.log', 'D3D9 shadow-host: '), 'warvk-d3d9-shadow-host/v2')
                    require(f'D3D9 shadow exact cull: enabled={int(exact_cull)}' in (folder/'host.log').read_text('utf-8'),
                            'host received the requested exact-cull switch')
                    require(sh['shadowRecords'] == scenario.shadow_records and sh['casterRecords'] == scenario.caster_draws
                            and sh['alphaRecords'] == ALPHA_RECORDS, 'host shadow records')
                    run_logs[label] = sh.pop('runLog')
                    device_log = sh.pop('deviceMemoryLog')
                    case['shadowHost'] = sh
                    accounts[label] = {k: sh[k] for k in ACCOUNT}
                    fx = singleton(prefixed(folder/'host.log', 'D3D9 scene-effects-host: '), 'warvk-d3d9-scene-effects-host/v1')
                    require(fx['effectRecords'] == EFFECT_RECORDS and fx['beforeUiRuns'] == len(scenario.runs)
                            and fx['ssaoRecorded'] == fx['aaRecorded'] == 1 and fx['snowRecorded'] == fx['bloomRecorded'] == 0
                            and fx['insertionsRefused'] == 0 and fx['beforeUiNs'] > 0, 'host executor account')
                    case['effectsHost'] = fx
                    case['childSettlement'] = child_settled(pr['childPid'], pr['childCreation'])
                    ds = singleton(json_lines(folder/'host.log'), 'warvk-d3d9-desktop/v1')
                    require(ds['pid'] == pr['childPid'] and ds['peerPid'] == case['process']['pid']
                            and ds['desktop'] == desktop.desktop_name and ds['inputDesktop'] == desktop.input_before, 'exact host/desktop')
                    if validate:
                        case['validation'] = vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                        case['loader'] = check_loader((folder/'host.log').read_text('utf-8'),
                            (folder/'host_warvk-d3d9-host.log').read_text('utf-8'), library, empty, loader, loader_version)
                account = accounts[label]
                case['shadow'] = account
                quantized = local.get('WARVK_D3D9_CASTER_QUANTIZE') == '1'
                expected_bytes = REFERENCE_COPIED_BYTES if reference else (HOST_QUANTIZED_BYTES if quantized else HOST_COPIED_BYTES)
                require(account['copiedBytes'] == expected_bytes, 'dynamic box copy size: '+json.dumps(account))
                require({k: account[k] for k in scenario.account} == scenario.account,
                        'shadow pass account: '+json.dumps(account))
                # Every run of the pass against what its frame should have
                # made of it: what it rendered and drew, and the data layer's
                # facts it read. The storage the pass owns grows at the
                # STORAGE_GROWS frames, by the point shadow cube, the outline
                # masks and the volumetric light's sun map, and nowhere else:
                # between them and from the last on it is the same after every
                # run - a new map, a new device and the last frame own what the
                # first frame did - and the last value is the account's peak.
                # The volumetric light pass owns storage after the runs it ran
                # in, and the runs say none in the others.
                log = run_logs[label]
                bytes_ = [run.pop('resourceBytes') for run in log]
                volumetric_bytes[label] = [run.pop('volumetricBytes') for run in log]
                require([b > 0 for b in volumetric_bytes[label]] == [run['volumetric'] == 1 for run in log],
                        'volumetric light storage: '+json.dumps(volumetric_bytes[label]))
                case['volumetricResourceBytes'] = sorted(set(volumetric_bytes[label]) - {0})
                grows = [scenario.shadow_frames.index(name) for name in STORAGE_GROWS]
                steps = [i for i in range(1, len(bytes_)) if bytes_[i] != bytes_[i-1]]
                storage = sorted(set(bytes_))
                case['shadowRuns'] = log
                case['shadowResourceBytes'] = storage
                wrong = [dict(run=index, frame=(scenario.shadow_frames+('timed',)*TIMED_FRAMES)[index], got=got, expected=expected)
                         for index, (got, expected) in enumerate(zip(log, scenario.runs)) if got != expected]
                require(len(log) == len(scenario.runs) and not wrong, 'shadow pass runs: '+json.dumps(wrong[:3]))
                require(len(bytes_) == len(log) and steps == grows and bytes_ == sorted(bytes_) and 0 < bytes_[0]
                        and account['peakResourceBytes'] == storage[-1]
                        and account['last'] == dict(scenario.runs[-1], resourceBytes=storage[-1], volumetricBytes=0),
                        'shadow pass storage: '+json.dumps(dict(storage=storage, steps=steps, grows=grows, account=account)))
                if not reference:
                    # The host's whole device, not the pass alone: what its
                    # allocator has handed out after each run. A map or a
                    # device that ended leaves nothing behind - the bytes in
                    # use are the same after the fourth map and the device
                    # Reset as before the first - and the chunks do not grow.
                    used, allocated = [u for u, _ in device_log], [a for _, a in device_log]
                    first = scenario.shadow_frames.index(SETTLED)
                    case['hostDeviceMemory'] = dict(firstRunUsedBytes=used[0], peakUsedBytes=max(used),
                        usedBytes=used[first], allocatedBytes=allocated[first], runs=len(device_log)-first)
                    require(len(device_log) == len(log) and min(used) >= storage[0]
                            and set(used[first:]) == {used[first]} and max(allocated[first:]) <= allocated[first],
                            'host device memory: '+json.dumps(dict(settled=first, log=device_log)))
                memory = {s['name']: s['memory'] for s in report['samples']}
                keys = ('privateBytes', 'usedVirtualBytes', 'privateVirtualBytes')
                case['x86'] = dict(
                    world={k: memory['world'][k] for k in keys},
                    shadowsDelta={k: memory['shadows'][k]-memory['world'][k] for k in keys},
                    lifecycleDelta={k: memory['lifecycle'][k]-memory['world'][k] for k in keys},
                    timedDelta={k: memory['timed'][k]-memory['world'][k] for k in keys},
                    peakPrivateBytes=memory['timed']['peakPrivateBytes'])
                case['frameTime'] = dict(worldUs=report['worldFrameUs'], shadowUs=report['shadowFrameUs'],
                    deltaUs=round(report['shadowFrameUs']-report['worldFrameUs'], 1), underValidation=validate)
                if not reference:
                    fx = case['effectsHost']
                    case['frameTime']['hostExecutorUsPerRun'] = round(fx['beforeUiNs']/fx['beforeUiRuns']/1000, 1)
                case['ok'] = True
            # A separate scene exercises real host-only pressure without
            # changing a single frame or account of the original comparison.
            receipt['budgetCases'] = []
            pressure_arms = ('reference', 'full', 'limited')
            if (env | remote_extra).get('WARVK_D3D9_CASTER_QUANTIZE') == '1':
                # Packed positions fit both boxes. Keep the original overflow
                # and recovery witnesses in a quantized run (opt-in, default off).
                pressure_arms += ('limited-float3',)
            for temporal in (False, True):
                baseline = None
                for kind in pressure_arms:
                    reference = kind == 'reference'
                    limited = kind.startswith('limited')
                    folder = out / ('pressure-' + ('temporal-' if temporal else 'direct-') + kind)
                    folder.mkdir()
                    exe, dll = folder/'fixture.exe', folder/'renderer.dll'
                    copy(fixture, exe)
                    copy(reference_dll if reference else proxy, dll)
                    config = folder/'pipeline.conf'
                    config.write_text('', encoding='utf-8')
                    local = env | dict(DXVK_LOG_PATH=str(folder), DXVK_CONFIG_FILE=str(config))
                    if reference:
                        local.update(REFERENCE_ENV)
                    else:
                        copy(host, folder/'host.exe')
                        local.update(ANNOTATE_ENV, **STREAM_ENV)
                        local.update(remote_extra)
                        local.update(WARVK_D3D9_HOST_EXE=str(folder/'host.exe'), WARVK_D3D9_HOST_LOG=str(folder/'host.log'),
                                     WARVK_D3D9_CASTER_AUDIT='1')
                        local.pop('WARVK_D3D9_CASTER_BUDGET_TEST_MIB', None)
                        if kind == 'limited-float3':
                            local['WARVK_D3D9_CASTER_QUANTIZE'] = '0'
                        if limited:
                            local['WARVK_D3D9_CASTER_BUDGET_TEST_MIB'] = '1'
                        if args.validation:
                            local.update(vvl.validation_env(folder, manifest, empty))
                    result = desktop.run([exe, '--dll', dll, '--scenario', 'normal', '--budget-fixture']
                                         + (['--reference'] if reference else [])
                                         + (['--budget-temporal'] if temporal else []),
                                         cwd=folder, stdout_path=folder/'case.stdout', stderr_path=folder/'case.stderr',
                                         timeout_s=180, env=local)
                    check_launch(result, desktop.desktop_name, desktop.input_before)
                    report = singleton(json_lines(folder/'case.stdout'), 'warvk-d3d9-shadow-fixture/v2')
                    require(report['ok'] and report['normalTeardown'] and report['reference'] == reference and
                            all(f['uiPixels'] == UI_PIXELS for f in report['frames']) and
                            tuple(f['name'] for f in report['frames']) == BUDGET_FRAMES, 'complete pressure scene')
                    files = [f'frame-{f}.bgra' for f in BUDGET_FRAMES]
                    raw = ['case.stdout', 'case.stderr'] + files + ([] if reference else ['host.log'])
                    case = {'label': folder.name, 'process': result}
                    if not reference:
                        host_report = singleton(json_lines(folder/'host.log'), 'warvk-d3d9-host/v1')
                        require(host_report['ok'] and host_report['normalTeardown'] and not host_report['liveResources']
                                and host_report['submitted'] == host_report['gpuCompleted'] == host_report['retired'],
                                'pressure host retirement')
                        if args.validation:
                            raw.append('validation.raw.log')
                            case['validation'] = vvl.parse_validation((folder/'validation.raw.log').read_text('utf-8'))
                        layers = prefixed(folder/'host.log', 'D3D9 caster-layers: ')
                        require(len(layers) == len(BUDGET_FRAMES), 'per-frame pressure telemetry')
                        case['layers'] = layers
                        runs = singleton(prefixed(folder/'host.log', 'D3D9 shadow-host: '), 'warvk-d3d9-shadow-host/v2')['runLog']
                        require(len(runs) == len(layers), 'each pressure frame ran')
                        far_c0 = int(local.get('WARVK_D3D9_SHADOW_EXACT_CULL') == '0')
                        control_draws = ([0]*4, [1]*4, [far_c0, 1, 1, 1], [1+far_c0, 2, 2, 2])
                        overflow = limited and local.get('WARVK_D3D9_CASTER_QUANTIZE') != '1'
                        for i, row in enumerate(layers):
                            age = i - 3
                            omitted = overflow and 1 <= age <= 10
                            expected = (15, 0, 0)
                            drawn = control_draws[min(i, 3)]
                            if omitted:
                                drawn = [int(age != 1), 0, 0, 0]
                                if age == 1:
                                    expected = (0, 15, 0)
                                elif age <= 8:
                                    expected = (1, 14, 0)
                                else:
                                    expected = (1, 0, 14)
                            require(tuple(row[k] for k in ('complete','held','unavailable')) == expected and
                                    row['omittedExact'] == int(omitted) and row['omittedUnknown'] == 0 and
                                    row['bytes'] <= (1 if limited else 64)*1048576, f'pressure census {i}: {row}')
                            require(runs[i]['cascadeDrawn'] == drawn,
                                    f'{BUDGET_FRAMES[i]} cascade draws: {runs[i]["cascadeDrawn"]}, expected {drawn}')
                            require(runs[i]['taaMode'] == (TAA_TEMPORAL if temporal else 0),
                                    'the requested receiver mode ran in the pressure scene')
                            if temporal and overflow and age in (1, 2, 9, 11):
                                require(runs[i]['taaInvalidation'] & (1 << 5) and not (runs[i]['taaHistory'] & 32),
                                        f'changed layer state revoked temporal history at {BUDGET_FRAMES[i]}')
                            if temporal and overflow and (3 <= age <= 8 or age == 10):
                                require(runs[i]['taaHistory'] & 32, 'stable partial coverage reused temporal history')
                    receipt['raw'].append(capture_raw_artifacts(folder, raw, True))
                    pictures = {f: (folder/name).read_bytes() for f, name in zip(BUDGET_FRAMES, files)}
                    require(all(len(data) == WIDTH*HEIGHT*4 for data in pictures.values()), 'pressure image extent')
                    if reference:
                        baseline = pictures
                    elif overflow:
                        case.update(check_budget_pixels(pictures, baseline, temporal))
                    else:
                        require(pictures == baseline, 'every normal-budget pressure-scene pixel matches in process')
                    receipt['budgetCases'].append(case)
            if args.validation:
                receipt['implicitSourceAfter'] = empty_directory_witness(empty)
                require(receipt['implicitSourceAfter'] == receipt['implicitSourceBefore'], 'private layer source drift')

        # The two modes on the same input: byte for byte, frame by frame. Each
        # arm against the in-process arm of its own module switch.
        reference_of = {label: REFERENCE[taa_off] for label, *_, taa_off in ARMS}
        comparison = {label: {name: image_difference(images[reference_of[label]][name], data) for name, data in frames.items()}
                      for label, frames in images.items() if label != reference_of[label]}
        receipt['comparison'] = comparison
        receipt['frameHashes'] = {c['label']: {f['name']: f['hash'] for f in c['result']['frames']} for c in receipt['cases']}
        different = {label: {name: d for name, d in frames.items() if d['pixels']} for label, frames in comparison.items()}
        different = {label: frames for label, frames in different.items() if frames}
        require(not different, 'in-process and x64 host images differ: '+json.dumps(different))
        receipt['modesEqual'] = True
        # Preserve the raw totals for both arms.
        receipt['shadowAccounts'] = accounts
        # Physical copy sizes and submitted draws were checked per arm.
        receipt['copiedBytesByArm'] = {label: a['copiedBytes'] for label, a in accounts.items()}
        receipt['castersDrawnByArm'] = {label: a['castersDrawn'] for label, a in accounts.items()}
        unequal = {label: account for label, account in accounts.items()
                   if any(account[k] != accounts[reference_of[label]][k] for k in ACCOUNT
                          if k not in ('copiedBytes', 'castersDrawn'))}
        require(not unequal, 'the shadow pass accounts differ: '+json.dumps(unequal))
        # The full runs, including both drawn fields, passed their exact arm
        # expectations above. Cross-check every other run field verbatim.
        def common_run(run):
            return {k: v for k, v in run.items() if k not in ('drawnCasters', 'cascadeDrawn')}
        require(all([common_run(r) for r in log] == [common_run(r) for r in run_logs[reference_of[label]]]
                    for label, log in run_logs.items()), 'the shadow pass runs differ')
        require(all(volumetric_bytes[label] == volumetric_bytes[reference_of[label]] for label in volumetric_bytes),
                'the volumetric light storage differs')
        receipt['accountsEqual'] = True
        # Without a request for shadow TAA the module switch changes nothing.
        base, hashes = images[REFERENCE[False]], receipt['frameHashes'][REFERENCE[False]]
        switched = receipt['frameHashes'][REFERENCE[True]]
        receipt['moduleSwitchDifference'] = {name: image_difference(base[name], images[REFERENCE[True]][name])
                                             for name in SCENARIOS[False].frames if name not in TAA_FRAMES}
        require(not any(d['pixels'] for d in receipt['moduleSwitchDifference'].values()),
                'the shadow TAA module switch changed a frame that does not ask for shadow TAA')
        # The pass's contract on the reference frames (equal in every mode by now).
        receipt['frameFacts'] = facts = dict(
            # A complete set that does not change gives the same picture again.
            repeatStable=hashes['shadow-again'] == hashes['shadow'],
            edgeCastsShadow=hashes['cascade-edge'] != hashes['cascade-edge-control'],
            # The sun and the caster set each change the shadows.
            sunMoves=hashes['sun-moved'] != hashes['shadow'],
            casterSetMatters=hashes['one-caster'] != hashes['sun-back'],
            # The flat ground is drawn into the cascades and shadows nothing.
            flatGroundCastsNothing=hashes['terrain'] == hashes['sun-back'],
            # An empty or incomplete set publishes no new shadow map: the
            # picture stays the one of the last published set.
            emptySetHolds=hashes['no-casters'] == hashes['one-caster'],
            unsealedHolds=hashes['unsealed'] == hashes['one-caster'],
            shortCountHolds=hashes['short-count'] == hashes['one-caster'],
            # A box drawn from dynamic buffers shadows as it was drawn, though
            # its buffers were rewritten before BeforeUi.
            dynamicCopied=hashes['dynamic'] == hashes['sun-back'],
            # Box 0 alpha-tested draws other than the plain box, and its
            # companion record makes its shadow another than without.
            alphaBoxDrawn=hashes['alpha-off'] != hashes['sun-back'],
            alphaReachesShadow=hashes['alpha'] != hashes['alpha-off'],
            # A lamp lights the boxes, and its shadows change the picture
            # again.
            pointLightLights=hashes['point-light'] != hashes['sun-back'],
            pointLightCastsShadow=hashes['point-shadow'] != hashes['point-light'],
            # Box 1 is outlined, by its edges and by its geometry, and the two
            # ways draw differently.
            outlineDrawn=hashes['outline'] != hashes['sun-back'],
            outlineGeometryDrawn=hashes['outline-geometry'] != hashes['sun-back'],
            outlineWaysDiffer=hashes['outline'] != hashes['outline-geometry'],
            # The volumetric light changes the frame, draws the same frame
            # again the same, draws differently by the ray march, and the
            # lamp lights the air.
            volumetricDrawn=hashes['volumetric'] != hashes['sun-back'],
            volumetricRepeats=hashes['volumetric-again'] == hashes['volumetric'],
            volumetricMarchDiffers=hashes['volumetric-march'] != hashes['volumetric'],
            volumetricLampLights=hashes['volumetric-lamp'] != hashes['volumetric']
            and hashes['volumetric-lamp'] != hashes['point-light'],
            # The fog volume scatters the light; the weather's haze and low
            # snow draw without a VolumetricLight record, and the weather
            # without them (and without snowfall) draws nothing; the contact
            # shadows darken what the lamp lights.
            fogVolumeScatters=hashes['fog-volume'] != hashes['volumetric'],
            weatherMediumDrawn=hashes['weather-medium'] != hashes['weather-clear'],
            weatherClearDrawsNothing=hashes['weather-clear'] == hashes['sun-back'],
            pointContactShadows=hashes['point-contact'] != hashes['point-light'],
            # The effects after the shadows see the shadowed world.
            effectsFollow=hashes['shadow-ssao-smaa'] != hashes['sun-back'],
            # A frame without records in between does not change what returns.
            returnsAfterOff=hashes['shadow-return'] == hashes['sun-back'],
            # Lifecycle. The empty set after the tombstone serial, the policy
            # revision, the session or the device changed draws the world:
            # the map of before is not drawn again.
            endedMapsGone=all(hashes[name] == hashes['world'] for name in ENDED),
            # The full set then gives the shadows of before the change.
            sameShadowsAfterChange=hashes['tombstone-fresh'] == hashes['policy-fresh'] == hashes['shadow-return'],
            # A frame without the ShadowFrame record leaves the facts alone:
            # it is incomplete, and it holds the map of the frame before.
            factsStayWithoutRecord=hashes['facts-kept'] == hashes['tombstone-fresh'],
            # The sun on its daily path stands where the record's time of day
            # puts it.
            timeOfDayMovesSun=len({hashes['time-9'], hashes['time-15'], hashes['time-return']}) == 3,
            # A session lasts until the Present that carries its reset.
            sessionEndsAtPresent=hashes['session-pending'] == hashes['time-return'],
            # Every new map, and the device after its Reset, draw the same
            # casters as the session before did.
            sessionsAlike=len({hashes[name] for name in ('time-return', 'session-fresh', 'map-2', 'map-3', 'map-4',
                                                          'reset-fresh')}) == 1,
            # A new session leaves no shadow TAA history: its first frame draws
            # as the first frame that asked for shadow TAA.
            taaHistoryEndsWithSession=hashes['taa-session'] == hashes['taa-first'],
            # The second frame blends the first one's history in.
            taaHistoryBlended=hashes['taa-history'] != hashes['taa-first'],
            # With its module off a request for shadow TAA draws as without.
            taaRequestIgnoredWhenOff=switched['taa-requested'] == switched['reset-fresh']
            and all(switched[name] == switched['shadow'] for name in TAA_FRAMES))
        require(all(facts.values()), 'shadow frame contract: '+json.dumps(facts))
        # The cascades' light basis continues from the frame before, so the
        # first sun after the sun moved is not its first frame; how far apart
        # the two land is a number.
        receipt['refitDistance'] = image_difference(base['shadow'], base['sun-back'])
        receipt['validatedSessions'] = sum(1 for c in receipt['cases'] if c['validated'])
        # The storage the pass owns is the executor's: in the x86 process in
        # process, in the host otherwise.
        receipt['accounting'] = {c['label']: dict(x86=c['x86'], frameTime=c['frameTime'],
            shadowResourceBytes=c['shadowResourceBytes'],
            hostEffectResourceBytes=c.get('effectsHost', {}).get('resourceBytes'),
            hostDeviceUsedBytes=c.get('hostDeviceMemory', {}).get('usedBytes'),
            hostDeviceAllocatedBytes=c.get('hostDeviceMemory', {}).get('allocatedBytes')) for c in receipt['cases']}
        require(receipt['desktopSafety']['ok'], 'isolated cleanup')
        receipt['ok'] = True
    except Exception as error:
        receipt['error'] = str(error)
    finally:
        try:
            receipt['inputsPreserved'] = bool(frozen) and all(guarded_identity(Path(r['path'])) == r for r in frozen)
            require(receipt['inputsPreserved'], 'input drift')
            require(all(identity(Path(r['path'])) == r for r in receipt['copies']), 'case copy drift')
            for raw in receipt['raw']:
                verify_raw_artifacts(raw)
            receipt['rawPreserved'] = True
        except Exception as error:
            receipt['ok'] = False
            receipt['preservationError'] = str(error)
        (out/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({k: receipt.get(k) for k in ('ok', 'error', 'preservationError', 'inputsPreserved', 'modesEqual',
                                                   'accountsEqual')}))
    return 0 if receipt['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
