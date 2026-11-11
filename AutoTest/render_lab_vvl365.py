"""Pure adapter for the pinned official VVL 1.4.365, not the 1.4.335 gate.

Source contract: KhronosGroup/Vulkan-ValidationLayers at
edeeee0ff5ce6d79d0c4ccfc28aa9c18e2b94144, layers/layer_options.cpp:1131-1142,
layers/VkLayer_khronos_validation.json.in:448-501 and
layers/chassis/chassis_manual.cpp:80-125. The latter reports validation classes,
not every setting. Neither the banner nor this adapter proves filter/limit or
all-setting runtime compliance. No launch, environment mutation or suppression.
"""
from __future__ import annotations

import re

import run_render_host_validation_gate as previous
from run_render_host_transport_gate import require

API_VERSION = '1.4.365'
SOURCE_COMMIT = 'edeeee0ff5ce6d79d0c4ccfc28aa9c18e2b94144'
SOURCE_URLS = tuple('https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/'
                   + SOURCE_COMMIT + '/' + path for path in (
    'layers/layer_options.cpp', 'layers/VkLayer_khronos_validation.json.in',
    'layers/chassis/chassis_manual.cpp'))
STATUS_ID = 'CURRENT-VALIDATION-ENABLED'
STATUS_MESSAGE_ID = '0xfd7b2292'
ENABLED_CLASSES = ('Core Checks', 'Synchronization', 'Stateless Parameter',
                   'Object lifetime', 'Thread Safety', 'Handle Wrapping')

# Copy, never mutate the old recipe. The only changes are the documented alias
# replacement and an explicit true request for record-time sync reporting.
SETTINGS = dict(previous.VVL_SETTINGS)
SETTINGS['syncval_full_validation'] = SETTINGS.pop('syncval_submit_time_validation')
SETTINGS['syncval_record_time_validation'] = 'true'


def check_manifest(document):
    """Check supported recipe; caller separately pins the manifest/DLL bytes."""
    layer = document['layer']
    require(document['file_format_version'] == '1.2.0'
            and layer['name'] == previous.LAYER and layer['api_version'] == API_VERSION
            and layer['type'] == 'GLOBAL'
            and layer['library_path'] == '.\\VkLayer_khronos_validation.dll',
            'exact pinned official VVL 1.4.365 manifest required')
    keys = set()
    for node in layer['features']['settings']:
        keys.update(previous.setting_keys(node))
    require(set(SETTINGS) | {'log_filename'} <= keys,
            'VVL 1.4.365 manifest does not support every requested control')


def validation_env(case_dir, manifest, empty_implicit_source=None):
    """Return child-only controls; keep all non-version-specific controls exact."""
    controls = previous.validation_env(case_dir, manifest, empty_implicit_source)
    del controls['VK_KHRONOS_VALIDATION_SYNCVAL_SUBMIT_TIME_VALIDATION']
    controls.update({'VK_KHRONOS_VALIDATION_' + key.upper(): value
                     for key, value in SETTINGS.items()})
    return controls


def parse_validation(text):
    """Strict independent raw callback reader, with zero warning exemptions."""
    require(type(text) is str and bool(text) and text.endswith('\n'),
            'empty/truncated raw VVL file')
    matches = list(previous.VVL_HEADER.finditer(text))
    require(matches and not text[:matches[0].start()].strip(),
            'raw VVL prefix/header missing')
    records = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        body = text[match.end():end].strip()
        require(body and not re.search(
            r'(?m)^\s*(?:Validation|Verbose)(?:[ \t]+[^\r\n:]+:|'
            r'[ \t]+(?:Error|Warning|Performance Warning|Information)\b|[ \t]*$)', body),
            'malformed/unrecognized VVL record boundary')
        records.append(dict(severity=match[1], vuid=match[2],
                            messageId='0x' + match[3].lower(), body=body))
    failures = [row for row in records
                if row['severity'] in ('Error', 'Warning', 'Performance Warning')]
    require(not failures, 'VVL warning/error: ' + ', '.join(row['vuid'] for row in failures))
    status = [row for row in records if row['vuid'] == STATUS_ID]
    require(len(status) == 1 and status[0]['severity'] == 'Information'
            and status[0]['messageId'] == STATUS_MESSAGE_ID,
            'one exact VVL 1.4.365 Information activation record required')
    require(not any(row['vuid'] == previous.STATUS_ID for row in records),
            'mixed old/new VVL activation records are not a pinned single-version log')
    # Full body matching rejects omitted/duplicate/extra/renamed classes, stale
    # old-style Enables/Disables text, and an unparsed suffix. This pinned host
    # creates one unnamed VkInstance, as the independent r1 raw log witnesses.
    banner = (r'\[AppName: [^\]\r\n]+\] vkCreateInstance\(\): Current Validation Enabled:\r?\n'
              + ''.join('  - ' + re.escape(name) + r'\r?\n' for name in ENABLED_CLASSES)
              + r'\r?\nObjects: 1\r?\n    \[0\] VkInstance (0x[0-9a-fA-F]+)')
    complete = re.fullmatch(banner, status[0]['body'])
    require(complete is not None and int(complete[1], 16) != 0,
            'complete exact six-class VVL 1.4.365 activation banner required')
    require(not any(re.search(
        r'not recognized|not a valid option|could not be opened|falling back to stdout|'
        r'deprecated|LAYER SETTING.*error', row['body'], re.I) for row in records),
        'VVL settings/logging failure')
    return dict(records=records, apiVersion=API_VERSION,
        activationProven=True, synchronizationValidationProven=True,
        enabledValidationClasses=list(ENABLED_CLASSES),
        rawCallbackIndependentOfDxvkIgnoredIds=True, runtimeFilterLimitProven=False,
        runtimeAllSettingsProven=False, statusRecordClassifiedSeparately=True,
        nonStatusWarnings=0, errors=0)
