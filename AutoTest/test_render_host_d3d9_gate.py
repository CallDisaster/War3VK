"""CPU-only counterexamples against the real public-D3D9 receipt checker.

Run with ``python -B AutoTest/test_render_host_d3d9_gate.py``. This imports and
calls the production rendering/full-fixture/remote checkers; it does not replace
their predicates, monkeypatch them, invoke main(), spawn children, build, load a DLL,
touch a player directory, or perform GPU/OS process checks.

The initial positive dictionaries are explicitly SYNTHETIC predicate inputs.
Their scene populations and command/resource accounting are independently
derived from the fixed fixture/API sequence, not copied from the checker.
Passing these tests is not a physical GPU run, a memory-saving measurement,
WSI coverage, Warcraft integration, or release acceptance.
"""

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_render_host_d3d9_gate import check_fixture, check_remote, check_render_observation, native_uint


CASES = (('normal', False), ('invalid', False), ('normal', True), ('invalid', True))
SCENARIOS = ('normal', 'invalid')
E_NOTIMPL = 0x80004001
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
REJECTIONS = (ValueError, KeyError, TypeError)

FIXTURE_NUMERIC = (
    'bits', 'width', 'height', 'drawCalls', 'primitiveCount', 'vertexBufferBytes',
    'streamOffset', 'vertexStride', 'fullVertexUploadBytes', 'partialVertexUploadBytes',
    'fullIndexUploadBytes', 'partialIndexUploadBytes', 'textureUploadBytes',
    'vertexShaderBytes', 'pixelShaderBytes', 'badVertexShaderBytes', 'comIdentityChecks', 'invalidChecks',
    'profileRestrictionChecks', 'backendRejectionChecks', 'idleRequestedMs',
    'processBoundaryChecks', 'windowMessages',
    'queryPolls', 'queryPending', 'presentHresult', 'deviceRefsWhileResourceHeld',
    'deviceFinalRefs', 'factoryFinalRefs',
)
FIXTURE_UINT64 = ('idleElapsedMs',)
FIXTURE_BOOLEAN = (
    'ok', 'reference', 'publicD3D9Api', 'parentVulkanAbsent', 'gameTouched',
    'globalInputUsed', 'productIntegrated', 'memorySavingsProven',
    'shaderBytesRoundtripped', 'queryCompleted', 'queryValueGuardChecked',
    'readbackVerified', 'resourceHeldDevice', 'windowClosed', 'normalTeardown',
)
PIXEL_NUMERIC = ('checked', 'clear', 'opaque', 'blend', 'depthProtected', 'failures', 'maxChannelError')
PROXY_NUMERIC = ('childPid', 'childCreation', 'childExit', 'exchanges')
PROXY_BOOLEAN = ('peerVerified', 'transportFailed', 'x86VulkanAbsent', 'productIntegrated', 'memorySavingsProven')
HOST_NUMERIC = (
    'bits', 'accepted', 'rejected', 'backendCalls', 'submitted', 'gpuCompleted',
    'retired', 'draws', 'clears', 'uploads', 'uploadedBytes', 'readbacks', 'queries',
    'buffersCreated', 'texturesCreated', 'shadersCompiled', 'declarationsCreated',
    'liveResources', 'liveLogicalBytes', 'peakLogicalBytes', 'vendorId', 'deviceId',
    'liveLogicalHandles', 'liveQueries', 'queryHandlesCreated', 'queryHandlesDestroyed', 'queryIssues', 'queryPolls',
)
HOST_BOOLEAN = (
    'ok', 'actualDxvkCore', 'actualDxso', 'normalTeardown', 'presentImplemented',
    'productIntegrated', 'gameTouched', 'memorySavingsProven',
)


def synthetic_fixture(scenario='normal', reference=False, pending=0):
    """One complete, explicitly synthetic receipt; never a physical result."""
    if scenario not in SCENARIOS or type(reference) is not bool:
        raise ValueError('test fixture arguments')
    guarded = scenario == 'invalid' and not reference
    return {
        'schema': 'warvk-d3d9-api-fixture/v1', 'ok': True, 'bits': 32,
        'scenario': scenario, 'reference': reference, 'stage': 'complete', 'error': '',
        'publicD3D9Api': True, 'parentVulkanAbsent': not reference,
        'processBoundaryChecks': 6, 'gameTouched': False, 'globalInputUsed': False,
        'productIntegrated': False, 'memorySavingsProven': False,
        'width': 64, 'height': 64, 'drawCalls': 3, 'primitiveCount': 6,
        'vertexBufferBytes': 131101, 'streamOffset': 65560, 'vertexStride': 40,
        'fullVertexUploadBytes': 131101, 'partialVertexUploadBytes': 160,
        'fullIndexUploadBytes': 12, 'partialIndexUploadBytes': 2, 'textureUploadBytes': 16,
        'vertexShaderBytes': 200, 'pixelShaderBytes': 208,
        'badVertexShaderBytes': 200 if guarded else 0, 'shaderBytesRoundtripped': True,
        # Opaque illustrative digests: checker syntax, not an invented GPU hash.
        'vertexShaderFnv': '0123456789abcdef', 'pixelShaderFnv': 'fedcba9876543210',
        'pixelOracle': {
            'checked': 2548, 'clear': 1392, 'opaque': 960, 'blend': 196,
            'depthProtected': 416, 'failures': 0, 'maxChannelError': 1,
            'imageFnv': '0011223344556677',
            'firstFailure': {'x': 0, 'y': 0, 'actualBGRA': 0, 'expectedBGRA': 0},
        },
        'comIdentityChecks': 9 if scenario == 'invalid' else 8,
        'invalidChecks': (3 if reference else 7) if scenario == 'invalid' else 0,
        'profileRestrictionChecks': 2 if guarded else 0,
        'backendRejectionChecks': 1 if guarded else 0,
        'idleRequestedMs': 6500 if guarded else 0,
        'idleElapsedMs': 6500 if guarded else 0,
        'queryPolls': pending + 1 + int(guarded), 'queryPending': pending,
        'queryCompleted': True, 'queryValueGuardChecked': guarded, 'readbackVerified': True,
        'present': 'not-requested' if reference else 'explicitly-unsupported',
        'presentHresult': E_NOTIMPL, 'resourceHeldDevice': True,
        'deviceRefsWhileResourceHeld': 1, 'deviceFinalRefs': 0, 'factoryFinalRefs': 0,
        'windowClosed': True, 'windowMessages': 1, 'normalTeardown': True,
    }


def synthetic_remote(scenario='normal', pending=0):
    """Accounting from the public call sequence and actual submission sites."""
    f = synthetic_fixture(scenario, False, pending)
    extra_texture = int(scenario == 'invalid')
    # Accepted calls: init + resource creates + 7 upload packets + clear + 3
    # draws + Issue + polls + readback + destroys + Finish. Invalid additionally
    # creates/destroys an unuploaded texture AND a legal missing-semantic VS.
    accepted = 29 + f['queryPolls'] + 4 * extra_texture
    # Present is always rejected; the invalid scenario also rejects one actual
    # backend Draw before GPU work, and proves subsequent legal draws recover.
    rejected = 1 + extra_texture
    # Actual GPU submits: init1 + texture-create(1+extra) + uploads7 + clear1
    # + draws3 + readback1. Query Issue/Poll itself does not submit a frame.
    submitted = 14 + extra_texture
    # Logical author resources only; not physical VRAM or the x86 saving.
    peak = 131101 + 12 + 16 * (1 + extra_texture) + 3 * 8 + 200 + 208 + f['badVertexShaderBytes']
    proxy = {
        'schema': 'warvk-d3d9-proxy/v1', 'childPid': 43210,
        'childCreation': 134361900012345678, 'childExit': 0,
        'exchanges': accepted + rejected, 'peerVerified': True, 'transportFailed': False,
        'x86VulkanAbsent': True, 'productIntegrated': False, 'memorySavingsProven': False,
    }
    host = {
        'schema': 'warvk-d3d9-host/v1', 'ok': True, 'bits': 64,
        'actualDxvkCore': True, 'actualDxso': True,
        'accepted': accepted, 'rejected': rejected, 'backendCalls': accepted - 2 + extra_texture,
        'submitted': submitted, 'gpuCompleted': submitted, 'retired': submitted,
        'draws': 3, 'clears': 1, 'uploads': 7, 'uploadedBytes': 131291,
        'readbacks': 1, 'queries': 1, 'buffersCreated': 2,
        'texturesCreated': 1 + extra_texture, 'shadersCompiled': 2 + extra_texture,
        'declarationsCreated': 1, 'liveResources': 0, 'liveLogicalBytes': 0,
        'liveLogicalHandles': 0, 'liveQueries': 0, 'queryHandlesCreated': 1, 'queryHandlesDestroyed': 1,
        'queryIssues': 1, 'queryPolls': f['queryPolls'],
        'peakLogicalBytes': peak, 'vendorId': 4318, 'deviceId': 10245,
        'normalTeardown': True, 'presentImplemented': False,
        'productIntegrated': False, 'gameTouched': False, 'memorySavingsProven': False,
    }
    return f, proxy, host


class D3D9ReceiptGateTests(unittest.TestCase):
    def reject_fixture(self, f, scenario='normal', reference=False):
        with self.assertRaises(REJECTIONS):
            check_fixture(f, scenario, reference)

    def reject_remote(self, scenario, records):
        with self.assertRaises(REJECTIONS):
            check_remote(scenario, *records)

    def mutate_fixture(self, field, value, cases=CASES):
        for scenario, reference in cases:
            with self.subTest(scenario=scenario, reference=reference, field=field, value=value):
                f = synthetic_fixture(scenario, reference)
                f[field] = copy.deepcopy(value)
                self.reject_fixture(f, scenario, reference)

    def mutate_remote(self, side, field, value, scenarios=SCENARIOS):
        for scenario in scenarios:
            with self.subTest(scenario=scenario, side=side, field=field, value=value):
                records = synthetic_remote(scenario)
                records[side][field] = copy.deepcopy(value)
                self.reject_remote(scenario, records)

    def test_synthetic_four_scope_positives(self):
        for scenario, reference in CASES:
            with self.subTest(scenario=scenario, reference=reference):
                check_fixture(synthetic_fixture(scenario, reference), scenario, reference)

    def test_synthetic_remote_normal_accounting_positive(self):
        check_remote('normal', *synthetic_remote('normal'))

    def test_synthetic_remote_invalid_accounting_positive(self):
        check_remote('invalid', *synthetic_remote('invalid'))

    def test_pending_query_counts_are_legitimate_not_required(self):
        for pending in (0, 1, 7):
            for scenario in SCENARIOS:
                with self.subTest(scenario=scenario, pending=pending):
                    check_remote(scenario, *synthetic_remote(scenario, pending))

    def test_independent_scene_population_arithmetic(self):
        # Geometry safe-region cardinalities, no invocation of CheckPixels.
        clear = 64 * 64 - 52 * 52
        opaque = 38 * 38 - 22 * 22
        blend = 14 * 14
        depth = 30 * 30 - 22 * 22
        p = synthetic_fixture()['pixelOracle']
        self.assertEqual((clear, opaque, blend, depth), (1392, 960, 196, 416))
        self.assertEqual(p['checked'], clear + opaque + blend)
        self.assertLess(depth, opaque)  # A subset, never added to total again.

    def test_independent_upload_population_arithmetic(self):
        self.assertEqual(131101 + 160 + 12 + 2 + 16, 131291)
        self.assertEqual((131101 + 65440 - 1) // 65440 + 4, 7)
        self.assertEqual(synthetic_remote()[2]['uploadedBytes'], 131291)

    def test_native_uint_accepts_exact_unsigned_boundaries(self):
        for bits in (32, 64):
            for value in (0, 1, (1 << bits) - 1):
                self.assertIs(native_uint(value, bits), True)

    def test_native_uint_rejects_bool_float_null_and_overflow(self):
        for bits in (32, 64):
            for bad in (-1, 1 << bits, 1 << 100, True, False, 0.0, 1.0, '1', None, [], {}):
                with self.subTest(bits=bits, value=bad):
                    self.assertIs(native_uint(bad, bits), False)

    def test_unknown_scenario_cannot_certify_a_matching_spoofed_record(self):
        for bad in ('', 'NORMAL', 'normal ', 'unknown', None, False, 0, [], {}):
            f = synthetic_fixture()
            f['scenario'] = bad
            with self.subTest(scenario=bad):
                self.reject_fixture(f, bad, False)

    def test_reference_argument_requires_boolean_not_truthiness(self):
        for bad in (0, 1, 0.0, 1.0, '', 'reference', None, [], {}):
            f = synthetic_fixture()
            f['reference'] = bad
            with self.subTest(reference=bad):
                self.reject_fixture(f, 'normal', bad)

    def test_scenario_and_reference_must_match_record(self):
        for scenario, reference in CASES:
            f = synthetic_fixture(scenario, reference)
            self.reject_fixture(f, 'invalid' if scenario == 'normal' else 'normal', reference)
            self.reject_fixture(f, scenario, not reference)

    def test_reports_must_be_objects(self):
        for bad in (None, [], '', 0, False):
            self.reject_fixture(bad)
            for side in range(3):
                records = list(synthetic_remote())
                records[side] = bad
                self.reject_remote('normal', records)

    def test_every_emitted_fixture_field_is_required(self):
        for scenario, reference in CASES:
            for field in synthetic_fixture(scenario, reference):
                f = synthetic_fixture(scenario, reference)
                del f[field]
                with self.subTest(scenario=scenario, reference=reference, missing=field):
                    self.reject_fixture(f, scenario, reference)

    def test_every_emitted_remote_field_is_required(self):
        for scenario in SCENARIOS:
            for side in (1, 2):
                for field in synthetic_remote(scenario)[side]:
                    records = synthetic_remote(scenario)
                    del records[side][field]
                    with self.subTest(scenario=scenario, side=side, missing=field):
                        self.reject_remote(scenario, records)

    def test_every_pixel_oracle_field_is_required(self):
        for field in synthetic_fixture()['pixelOracle']:
            f = synthetic_fixture()
            del f['pixelOracle'][field]
            with self.subTest(missing=field):
                self.reject_fixture(f)
        for field in synthetic_fixture()['pixelOracle']['firstFailure']:
            f = synthetic_fixture()
            del f['pixelOracle']['firstFailure'][field]
            with self.subTest(firstFailureMissing=field):
                self.reject_fixture(f)

    def test_fixture_integer_fields_reject_equal_floats_and_boolean_aliases(self):
        for scenario, reference in CASES:
            for field in FIXTURE_NUMERIC + FIXTURE_UINT64:
                original = synthetic_fixture(scenario, reference)[field]
                for bad in (float(original), True, False, str(original), None, [], {}):
                    self.mutate_fixture(field, bad, ((scenario, reference),))

    def test_fixture_integers_are_native_unsigned_32bit(self):
        for field in FIXTURE_NUMERIC:
            for bad in (-1, 1 << 32, 1 << 64, 1 << 100):
                self.mutate_fixture(field, bad)

    def test_fixture_tick_durations_are_unsigned_64bit(self):
        for field in FIXTURE_UINT64:
            for bad in (-1, 1 << 64, 1 << 100, True, False, 6500.0, None, '6500', [], {}):
                self.mutate_fixture(field, bad)

    def test_fixture_boolean_fields_reject_numeric_or_null_aliases(self):
        for field in FIXTURE_BOOLEAN:
            for bad in (0, 1, 0.0, 1.0, 'false', None, [], {}):
                self.mutate_fixture(field, bad)

    def test_required_fixture_success_flags_cannot_be_false(self):
        for field in ('ok', 'publicD3D9Api', 'shaderBytesRoundtripped', 'queryCompleted',
                      'readbackVerified', 'resourceHeldDevice', 'windowClosed', 'normalTeardown'):
            self.mutate_fixture(field, False)

    def test_production_game_input_and_savings_claims_are_rejected(self):
        for field in ('gameTouched', 'globalInputUsed', 'productIntegrated', 'memorySavingsProven'):
            self.mutate_fixture(field, True)
        for side, fields in ((1, ('productIntegrated', 'memorySavingsProven')),
                             (2, ('productIntegrated', 'memorySavingsProven', 'gameTouched', 'presentImplemented'))):
            for field in fields:
                self.mutate_remote(side, field, True)

    def test_success_cannot_include_error_or_noncomplete_stage(self):
        for error in ('driver failed', 0, None, False, [], {}):
            self.mutate_fixture('error', error)
        for stage in ('real-render-target-readback', '', None, False):
            self.mutate_fixture('stage', stage)

    def test_exact_scene_and_upload_numbers_cannot_drift(self):
        fields = ('width', 'height', 'drawCalls', 'primitiveCount', 'vertexBufferBytes',
                  'streamOffset', 'vertexStride', 'fullVertexUploadBytes', 'partialVertexUploadBytes',
                  'fullIndexUploadBytes', 'partialIndexUploadBytes', 'textureUploadBytes')
        for field in fields:
            value = synthetic_fixture()[field]
            for bad in (0, value - 1, value + 1):
                self.mutate_fixture(field, bad)

    def test_zero_population_is_not_vacuous_pixel_success(self):
        f = synthetic_fixture()
        for field in PIXEL_NUMERIC:
            f['pixelOracle'][field] = 0
        self.reject_fixture(f)
        for field in ('checked', 'clear', 'opaque', 'blend', 'depthProtected'):
            f = synthetic_fixture()
            f['pixelOracle'][field] = 0
            self.reject_fixture(f)

    def test_pixel_counters_reject_type_aliases_and_overflow(self):
        for field in PIXEL_NUMERIC:
            original = synthetic_fixture()['pixelOracle'][field]
            for bad in (float(original), True, False, str(original), None, [], {}, -1, 1 << 32):
                f = synthetic_fixture()
                f['pixelOracle'][field] = bad
                with self.subTest(field=field, value=bad):
                    self.reject_fixture(f)

    def test_any_pixel_failure_or_excess_error_is_rejected(self):
        for field, bad in (('failures', 1), ('maxChannelError', 2), ('maxChannelError', 255)):
            f = synthetic_fixture()
            f['pixelOracle'][field] = bad
            self.reject_fixture(f)

    def test_first_failure_diagnostics_cannot_contradict_zero_failures(self):
        for field in ('x', 'y', 'actualBGRA', 'expectedBGRA'):
            for bad in (1, True, False, 0.0, None, [], {}, -1, 1 << 32):
                f = synthetic_fixture()
                f['pixelOracle']['firstFailure'][field] = bad
                with self.subTest(field=field, value=bad):
                    self.reject_fixture(f)

    def test_full_hash_syntax_not_partial_or_coercible(self):
        for field in ('imageFnv', 'vertexShaderFnv', 'pixelShaderFnv'):
            for bad in ('', 'a' * 15, 'a' * 17, 'A' * 16, 'g' * 16, '0x' + '0' * 16,
                        '0' * 16 + '\n', 0, True, None, [], {}):
                f = synthetic_fixture()
                target = f['pixelOracle'] if field == 'imageFnv' else f
                target[field] = bad
                with self.subTest(field=field, value=bad):
                    self.reject_fixture(f)

    def test_shader_lengths_obey_original_dword_bounded_profile(self):
        for field in ('vertexShaderBytes', 'pixelShaderBytes'):
            for bad in (0, 8, 16, 17, 199, 201, 16385, 16388):
                self.mutate_fixture(field, bad)

    def test_query_success_count_matches_guarded_requery(self):
        for scenario, reference in CASES:
            f = synthetic_fixture(scenario, reference)
            f['queryPolls'] += 1
            self.reject_fixture(f, scenario, reference)
            f = synthetic_fixture(scenario, reference)
            f['queryPending'] = f['queryPolls']
            self.reject_fixture(f, scenario, reference)
        self.mutate_fixture('queryPolls', 0)

    def test_pending_queries_cannot_replace_gpu_completion(self):
        records = synthetic_remote(pending=5)
        records[2]['gpuCompleted'] = 0
        self.reject_remote('normal', records)
        records = synthetic_remote(pending=5)
        records[2]['retired'] -= 1
        self.reject_remote('normal', records)

    def test_negative_checks_are_not_optional_or_interchangeable(self):
        for scenario, reference in CASES:
            f = synthetic_fixture(scenario, reference)
            for field in ('invalidChecks', 'profileRestrictionChecks', 'backendRejectionChecks'):
                for delta in (-1, 1):
                    self.mutate_fixture(field, f[field] + delta, ((scenario, reference),))
            self.mutate_fixture('queryValueGuardChecked', not f['queryValueGuardChecked'], ((scenario, reference),))

    def test_remote_backend_rejection_is_distinct_from_local_profile_refusal(self):
        records = synthetic_remote('invalid')
        records[0]['backendRejectionChecks'] = 0
        records[0]['profileRestrictionChecks'] += 1
        self.reject_remote('invalid', records)
        records = synthetic_remote('invalid')
        records[2]['backendCalls'] = records[2]['accepted']
        self.reject_remote('invalid', records)
        records = synthetic_remote('invalid')
        records[2]['rejected'] = 1
        records[1]['exchanges'] -= 1
        self.reject_remote('invalid', records)

    def test_backend_refusal_does_not_submit_a_fourth_draw(self):
        for field in ('submitted', 'gpuCompleted', 'retired', 'draws'):
            records = synthetic_remote('invalid')
            records[2][field] += 1
            self.reject_remote('invalid', records)

    def test_bad_shader_has_real_bounded_original_bytes_only_in_remote_invalid(self):
        remote_invalid = (('invalid', False),)
        for bad in (0, 8, 16, 17, 199, 201, 16385, 16388):
            self.mutate_fixture('badVertexShaderBytes', bad, remote_invalid)
        for scenario, reference in CASES:
            if (scenario, reference) != ('invalid', False):
                self.mutate_fixture('badVertexShaderBytes', 200, ((scenario, reference),))

    def test_bad_shader_contributes_to_peak_while_both_extra_resources_are_alive(self):
        records = synthetic_remote('invalid')
        self.assertEqual(records[2]['peakLogicalBytes'], 131577 + records[0]['badVertexShaderBytes'])
        records[2]['peakLogicalBytes'] -= records[0]['badVertexShaderBytes']
        self.reject_remote('invalid', records)
        records = synthetic_remote('invalid')
        records[2]['peakLogicalBytes'] -= 16  # Cannot pretend the extra texture was already gone.
        self.reject_remote('invalid', records)

    def test_idle_probe_requires_full_requested_clean_boundary_pause(self):
        for requested in (0, 1, 5000, 6499, 6501):
            self.mutate_fixture('idleRequestedMs', requested, (('invalid', False),))
        for elapsed in (0, 1, 4999, 6499, 120000, UINT64_MAX):
            self.mutate_fixture('idleElapsedMs', elapsed, (('invalid', False),))
        for elapsed in (6500, 6501, 6532):
            records = synthetic_remote('invalid')
            records[0]['idleElapsedMs'] = elapsed
            check_remote('invalid', *records)

    def test_idle_probe_is_not_fabricated_for_normal_or_reference(self):
        cases = (('normal', False), ('normal', True), ('invalid', True))
        self.mutate_fixture('idleRequestedMs', 6500, cases)
        self.mutate_fixture('idleElapsedMs', 6500, cases)

    def test_idle_does_not_imply_memory_savings_or_extra_gpu_commands(self):
        records = synthetic_remote('invalid')
        records[2]['accepted'] += 1
        records[2]['backendCalls'] += 1
        records[1]['exchanges'] += 1
        self.reject_remote('invalid', records)

    def test_resource_lifetime_and_message_pump_require_actual_coverage(self):
        self.mutate_fixture('deviceRefsWhileResourceHeld', 0)
        self.mutate_fixture('deviceFinalRefs', 1)
        self.mutate_fixture('factoryFinalRefs', 1)
        self.mutate_fixture('windowMessages', 0)
        self.mutate_fixture('comIdentityChecks', 0)
        self.mutate_fixture('processBoundaryChecks', 0)

    def test_remote_process_must_not_load_vulkan(self):
        self.mutate_fixture('parentVulkanAbsent', False, (('normal', False), ('invalid', False)))
        self.mutate_remote(1, 'x86VulkanAbsent', False)

    def test_reference_can_load_vulkan_without_claiming_remote_savings(self):
        for scenario in SCENARIOS:
            for absent in (False, True):
                f = synthetic_fixture(scenario, True)
                f['parentVulkanAbsent'] = absent
                check_fixture(f, scenario, True)

    def test_known_reference_lifetime_failure_retains_rendering_only_evidence(self):
        # Shape of reference-r1: exact pixels passed, but final device Release
        # returned 15. This is NOT a synthetic full-run pass or a repaired DLL.
        f = synthetic_fixture('normal', True)
        f.update(ok=False, stage='bound-resource-com-teardown',
                 error='final device Release retained a bound-resource cycle',
                 deviceRefsWhileResourceHeld=16, deviceFinalRefs=15,
                 normalTeardown=False, windowClosed=False)
        f['pixelOracle'].update(maxChannelError=0, imageFnv='97937de3dc0c0c6d')
        check_render_observation(f, 'normal', True)
        self.reject_fixture(f, 'normal', True)
        # Dishonest success labels cannot erase the actual nonzero final refs.
        f.update(ok=True, stage='complete', error='', normalTeardown=True, windowClosed=True)
        self.reject_fixture(f, 'normal', True)

    def test_rendering_only_predicate_still_rejects_pixel_or_shader_failures(self):
        for field, value in (('failures', 1), ('maxChannelError', 2), ('checked', 0)):
            f = synthetic_fixture('normal', True)
            f['pixelOracle'][field] = value
            with self.subTest(field=field), self.assertRaises(REJECTIONS):
                check_render_observation(f, 'normal', True)
        f = synthetic_fixture('normal', True)
        f['shaderBytesRoundtripped'] = False
        with self.assertRaises(REJECTIONS):
            check_render_observation(f, 'normal', True)

    def test_rendering_only_observation_cannot_replace_remote_lifetime_gate(self):
        records = synthetic_remote()
        records[0].update(ok=False, stage='bound-resource-com-teardown',
                          error='final device Release retained a bound-resource cycle',
                          deviceFinalRefs=15, normalTeardown=False, windowClosed=False)
        check_render_observation(records[0], 'normal')
        self.reject_remote('normal', records)

    def test_remote_present_must_remain_explicitly_unsupported(self):
        for status in ('not-requested', 'not-attempted', 'succeeded-hidden-only', '', None):
            self.mutate_fixture('present', status, (('normal', False), ('invalid', False)))
        for code in (0, 1, 0x8876086c):
            self.mutate_fixture('presentHresult', code, (('normal', False), ('invalid', False)))

    def test_reference_present_was_not_requested_by_the_gate(self):
        for status in ('not-attempted', 'succeeded-hidden-only', 'explicitly-unsupported', None, False):
            self.mutate_fixture('present', status, (('normal', True), ('invalid', True)))
        self.mutate_fixture('presentHresult', 0, (('normal', True), ('invalid', True)))

    def test_remote_numbers_require_native_types(self):
        for side, fields in ((1, PROXY_NUMERIC), (2, HOST_NUMERIC)):
            for field in fields:
                original = synthetic_remote()[side][field]
                for bad in (float(original), True, False, str(original), None, [], {}):
                    self.mutate_remote(side, field, bad)

    def test_remote_numbers_are_unsigned_and_bounded(self):
        for side, fields in ((1, PROXY_NUMERIC), (2, HOST_NUMERIC)):
            for field in fields:
                for bad in (-1, 1 << 64, 1 << 100):
                    self.mutate_remote(side, field, bad)
        for side, field in ((1, 'childPid'), (1, 'childExit'), (2, 'bits'),
                            (2, 'vendorId'), (2, 'deviceId'), (2, 'liveResources')):
            self.mutate_remote(side, field, 1 << 32)

    def test_remote_boolean_fields_do_not_accept_integer_aliases(self):
        for side, fields in ((1, PROXY_BOOLEAN), (2, HOST_BOOLEAN)):
            for field in fields:
                for bad in (0, 1, 0.0, 1.0, 'false', None, [], {}):
                    self.mutate_remote(side, field, bad)

    def test_exact_child_identity_and_transport_settlement(self):
        for field in ('childPid', 'childCreation'):
            self.mutate_remote(1, field, 0)
        for field, bad in (('childExit', 2), ('peerVerified', False), ('transportFailed', True)):
            self.mutate_remote(1, field, bad)

    def test_existing_dxso_dxvk_and_normal_teardown_are_not_optional(self):
        for field in ('ok', 'actualDxvkCore', 'actualDxso', 'normalTeardown'):
            self.mutate_remote(2, field, False)

    def test_zero_work_cannot_be_remote_success(self):
        records = synthetic_remote()
        for field in HOST_NUMERIC:
            if field not in ('bits', 'vendorId', 'deviceId'):
                records[2][field] = 0
        records[1]['exchanges'] = 0
        self.reject_remote('normal', records)

    def test_exact_remote_work_counts_and_lifetimes(self):
        fields = ('accepted', 'rejected', 'backendCalls', 'submitted', 'gpuCompleted', 'retired',
                  'draws', 'clears', 'uploads', 'uploadedBytes', 'readbacks', 'queries',
                  'buffersCreated', 'texturesCreated', 'shadersCompiled', 'declarationsCreated',
                  'liveResources', 'liveLogicalBytes', 'peakLogicalBytes')
        for scenario in SCENARIOS:
            for field in fields:
                value = synthetic_remote(scenario)[2][field]
                for bad in (value - 1, value + 1):
                    self.mutate_remote(2, field, bad, (scenario,))

    def test_coherently_forged_old_twelve_submissions_are_rejected(self):
        for scenario in SCENARIOS:
            records = synthetic_remote(scenario)
            for field in ('submitted', 'gpuCompleted', 'retired'):
                records[2][field] = 12
            self.reject_remote(scenario, records)

    def test_accepted_backend_exchange_triangle_cannot_hide_extra_work(self):
        for scenario in SCENARIOS:
            records = synthetic_remote(scenario)
            records[2]['accepted'] += 1
            records[2]['backendCalls'] += 1
            records[1]['exchanges'] += 1
            self.reject_remote(scenario, records)
            records = synthetic_remote(scenario)
            records[1]['exchanges'] -= 1  # Silently dropped unsupported Present.
            self.reject_remote(scenario, records)

    def test_peak_logical_bytes_cannot_be_zero_or_a_physical_vram_claim(self):
        for bad in (0, 1, 384 * 1024 * 1024):
            self.mutate_remote(2, 'peakLogicalBytes', bad)

    def test_gpu_ids_must_be_nonzero_native_uint32(self):
        for field in ('vendorId', 'deviceId'):
            for bad in (0, -1, True, False, 1.0, None, 1 << 32):
                self.mutate_remote(2, field, bad)

    def test_real_r2_failure_shape_remains_a_negative_not_a_positive(self):
        # Values observed in preserved r2/remote-normal/parent.stdout. The gate
        # stopped on all-clear pixels, so no host success/teardown is fabricated.
        f = synthetic_fixture()
        f.update(ok=False, stage='real-render-target-readback',
                 error='independent clear/coverage/texture/depth/blend pixel oracle failed',
                 readbackVerified=False, normalTeardown=False, windowClosed=False)
        f['pixelOracle'].update(failures=1156, maxChannelError=239, imageFnv='830e5bdb13142325')
        f['pixelOracle']['firstFailure'] = {
            'x': 10, 'y': 10, 'actualBGRA': 4279244832, 'expectedBGRA': 4294901760,
        }
        self.reject_fixture(f)
        # Even dishonest status toggles must not turn the actual pixel failure
        # into success. The independent population/error predicate must reject.
        f.update(ok=True, stage='complete', error='', readbackVerified=True,
                 normalTeardown=True, windowClosed=True)
        self.reject_fixture(f)


if __name__ == '__main__':
    unittest.main(verbosity=2)
