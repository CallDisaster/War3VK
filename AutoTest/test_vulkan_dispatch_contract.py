"""Registry-backed Vulkan function-table ownership; no DLL or native execution.

Run against the actual repository header/resolver and vendored Khronos registry.
No historical artifact, receipt, pre-fix hash, or external module is required.
Negative tests mutate only in-memory source text. This is static evidence, not
C++ execution or replacement for a validation-layer run.
"""

from collections import Counter
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / 'src/vulkan/vulkan_loader.h').is_file())
HEADER = (ROOT / 'src/vulkan/vulkan_loader.h').read_text(encoding='utf-8')
LOADER = (ROOT / 'src/vulkan/vulkan_loader.cpp').read_text(encoding='utf-8')
REGISTRY = (ROOT / 'include/vulkan/registry/vk.xml').read_text(encoding='utf-8')
QUERY = 'vkGetPhysicalDeviceMultisamplePropertiesEXT'
COMMAND = 'vkCmdSetSampleLocationsEXT'
GUARD = 'VK_EXT_sample_locations'
DEVICE_TYPES = frozenset(('VkDevice', 'VkQueue', 'VkCommandBuffer'))


def without_comments(source):
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)


def struct_body(header, owner):
    base = {'LibraryFn': 'LibraryLoader', 'InstanceFn': 'InstanceLoader',
            'DeviceFn': 'DeviceLoader'}[owner]
    matches = list(re.finditer(rf'^  struct {owner} : {base} \{{(.*?)^  \}};',
                              without_comments(header), re.M | re.S))
    if len(matches) != 1:
        raise AssertionError(f'unique function table/base missing: {owner}')
    return matches[0].group(1)


def declarations(header, owner):
    return re.findall(r'^\s*VULKAN_FN\((\w+)\);\s*$',
                      struct_body(header, owner), re.M)


def guards_for(header, owner, symbol):
    guards = []
    results = []
    for line in struct_body(header, owner).splitlines():
        text = line.strip()
        if text.startswith('#ifdef '):
            guards.append(text.split()[1])
        elif text.startswith('#if') or text.startswith('#else'):
            raise AssertionError('unsupported conditional: update the static reader')
        elif text == '#endif':
            if not guards:
                raise AssertionError('unmatched #endif')
            guards.pop()
        elif text == f'VULKAN_FN({symbol});':
            results.append(tuple(guards))
    if guards:
        raise AssertionError('unterminated conditional')
    return results


def registry_commands(registry):
    xml = ET.fromstring(registry)
    return {command.get('name') or command.findtext('proto/name'): command
            for command in xml.findall('commands/command')}


def first_parameter(commands, name):
    seen = set()
    while True:
        if name in seen or name not in commands:
            raise AssertionError(f'unknown/cyclic registry command: {name}')
        seen.add(name)
        command = commands[name]
        if command.get('alias'):
            name = command.get('alias')
        else:
            return command.findtext('param/type')


def assert_dispatch_contract(header, loader, registry):
    bindings = {owner: declarations(header, owner)
                for owner in ('LibraryFn', 'InstanceFn', 'DeviceFn')}
    counts = Counter(name for names in bindings.values() for name in names)
    if any(count != 1 for count in counts.values()):
        raise AssertionError('duplicate function-table binding')
    commands = registry_commands(registry)
    clean_header = without_comments(header)
    for name in bindings['DeviceFn']:
        if name in ('wine_vkAcquireKeyedMutex', 'wine_vkReleaseKeyedMutex'):
            # Existing local additions are not Khronos commands. Verify their
            # real PFN declaration still dispatches on VkDevice, not a waiver.
            pattern = (rf'using PFN_{name}\s*=\s*VkResult\s*'
                       r'\(VKAPI_PTR\s*\*\)\s*\(VkDevice\s+device\b')
            if not re.search(pattern, clean_header):
                raise AssertionError(f'non-Vulkan helper owner changed: {name}')
            continue
        first = first_parameter(commands, name)
        if first not in DEVICE_TYPES:
            raise AssertionError(f'{name}: {first} is not a device-level dispatch owner')
    if QUERY not in bindings['InstanceFn'] or QUERY in bindings['DeviceFn']:
        raise AssertionError('physical-device query must be in InstanceFn')
    if COMMAND not in bindings['DeviceFn'] or COMMAND in bindings['InstanceFn']:
        raise AssertionError('command-buffer setter must remain in DeviceFn')
    for owner, name in (('InstanceFn', QUERY), ('DeviceFn', COMMAND)):
        if guards_for(header, owner, name) != [(GUARD,)]:
            raise AssertionError(f'wrong extension guard for {name}')
    if not re.search(r'#define VULKAN_FN\(name\)\s*\\\s*'
                     r'::PFN_ ## name name = reinterpret_cast<::PFN_ ## name>\(sym\(#name\)\)',
                     clean_header):
        raise AssertionError('binding macro resolver changed')
    clean_loader = without_comments(loader)
    routes = (
        ('LibraryLoader::sym(VkInstance instance, const char* name) const',
         'return m_getInstanceProcAddr(instance, name);'),
        ('InstanceLoader::sym(const char* name) const',
         'return m_library->sym(m_instance, name);'),
        ('DeviceLoader::sym(const char* name) const',
         'return m_getDeviceProcAddr(m_device, name);'),
    )
    for signature, expected in routes:
        match = re.search(re.escape(signature) + r'\s*\{([^{}]*)\}', clean_loader)
        if not match or match.group(1).strip() != expected:
            raise AssertionError(f'resolver route changed: {signature}')


class VulkanDispatchContractTests(unittest.TestCase):
    def assert_rejected(self, header, loader=LOADER):
        with self.assertRaises(AssertionError):
            assert_dispatch_contract(header, loader, REGISTRY)

    def test_actual_production_dispatch_contract(self):
        assert_dispatch_contract(HEADER, LOADER, REGISTRY)

    def test_khronos_registry_extension_has_mixed_dispatch_owners(self):
        xml = ET.fromstring(REGISTRY)
        extension = xml.find("extensions/extension[@name='VK_EXT_sample_locations']")
        self.assertIsNotNone(extension)
        self.assertEqual(extension.get('type'), 'device')
        self.assertEqual({entry.get('name') for entry in extension.findall('require/command')},
                         {QUERY, COMMAND})
        commands = registry_commands(REGISTRY)
        self.assertEqual(first_parameter(commands, QUERY), 'VkPhysicalDevice')
        self.assertEqual(first_parameter(commands, COMMAND), 'VkCommandBuffer')

    def test_existing_setter_consumer_remains_on_device_table(self):
        source = (ROOT / 'src/dxvk/dxvk_cmdlist.h').read_text(encoding='utf-8')
        self.assertIn('m_vkd->vkCmdSetSampleLocationsEXT(cmdBuffer, sampleLocations);',
                      without_comments(source))

    def test_m1_readding_query_to_device_is_rejected(self):
        mutated = HEADER.replace(f'    VULKAN_FN({COMMAND});',
                    f'    VULKAN_FN({COMMAND});\n    VULKAN_FN({QUERY});')
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)

    def test_m2_omitting_query_is_rejected(self):
        mutated = HEADER.replace(f'    VULKAN_FN({QUERY});\n', '')
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)

    def test_m3_guard_removal_is_rejected(self):
        block = f'    #ifdef {GUARD}\n    VULKAN_FN({QUERY});\n    #endif'
        mutated = HEADER.replace(block, f'    VULKAN_FN({QUERY});')
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)

    def test_m4_setter_relocated_with_query_is_rejected(self):
        mutated = HEADER.replace(f'    VULKAN_FN({COMMAND});\n', '')
        mutated = mutated.replace(f'    VULKAN_FN({QUERY});',
                    f'    VULKAN_FN({QUERY});\n    VULKAN_FN({COMMAND});')
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)

    def test_m5_duplicate_query_is_rejected(self):
        mutated = HEADER.replace(f'    VULKAN_FN({QUERY});',
                                  f'    VULKAN_FN({QUERY});\n' * 2)
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)

    def test_m6_instance_resolver_route_drift_is_rejected(self):
        mutated = LOADER.replace('return m_library->sym(m_instance, name);',
                                 'return m_library->sym(nullptr, name);')
        self.assertNotEqual(mutated, LOADER)
        self.assert_rejected(HEADER, mutated)

    def test_m7_device_resolver_route_drift_is_rejected(self):
        mutated = LOADER.replace('return m_getDeviceProcAddr(m_device, name);',
                                 'return m_library->sym(name);')
        self.assertNotEqual(mutated, LOADER)
        self.assert_rejected(HEADER, mutated)

    def test_m8_comment_is_not_a_query_binding(self):
        mutated = HEADER.replace(f'    VULKAN_FN({QUERY});',
                                  f'    // VULKAN_FN({QUERY});')
        self.assertNotEqual(mutated, HEADER)
        self.assert_rejected(mutated)


if __name__ == '__main__':
    unittest.main(verbosity=2)
