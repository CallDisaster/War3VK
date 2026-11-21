"""The data layer reaches the execution end only through war3::IDeviceServices
and war3::IProcessServices (src/d3d9/war3/core/war3_execution_services.h).

Text checks only. The link-level proof is run_x86_data_layer_cut_gate.py; this
keeps the seam from regressing between builds.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8')


def between(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin + len(start))]


INTERFACE = read('src/d3d9/war3/core/war3_execution_services.h')
REGISTRY = read('src/d3d9/war3/core/war3_execution_services.cpp')
IN_PROCESS = read('src/d3d9/d3d9_war3_execution_services.cpp')
DEVICE_H = read('src/d3d9/d3d9_device.h')
WAR3 = read('src/d3d9/war3/war3.cpp')
WAR3_H = read('src/d3d9/war3/war3.h')
MESON = read('src/d3d9/meson.build')
HOOK = read('src/d3d9/war3/hooks/war3_hook_render.cpp')
HUB = read('src/d3d9/war3/tools/war3_diagnostics_hub.cpp')
TEST_API = read('src/d3d9/war3/tools/war3_internal_test_api.cpp')
PERF = read('src/d3d9/war3/tools/war3_perf_monitor.cpp')
BRIDGE = read('src/d3d9/war3/render/war3_shadow_runtime_bridge.cpp')
STARTUP = read('src/d3d9/war3/platform/war3_startup_error.cpp')
EXAMPLE = read('src/d3d9/war3/war3_user_example.cpp')
INSTANCE = read('src/d3d9/war3/reimpl/war3_instance_buffer.cpp')
INSTANCE_H = read('src/d3d9/war3/reimpl/war3_instance_buffer.h')
MATERIAL = read('src/d3d9/war3/shader/war3_material.cpp')
MATERIAL_SHADER = read('src/d3d9/d3d9_war3_material_shader.cpp')

DEVICE_INTERFACE = between(INTERFACE, 'class IDeviceServices {', '};')
PROCESS_INTERFACE = between(INTERFACE, 'class IProcessServices {', '};')


def virtuals(body):
    """Names of the virtual members, and whether every one is pure."""
    flat = ' '.join(body.split())
    declared = re.findall(r'virtual [^;]*?(\w+)\([^;]*?\)( const)? = 0;', flat)
    return [name for name, _ in declared], flat.count('virtual ') == len(declared)


class ExecutionServicesContracts(unittest.TestCase):
    def test_both_interfaces_are_pure(self):
        device, device_pure = virtuals(DEVICE_INTERFACE)
        process, process_pure = virtuals(PROCESS_INTERFACE)
        self.assertTrue(device_pure and process_pure)
        self.assertEqual(len(device), 12)
        self.assertEqual(len(process), 19)
        self.assertEqual(len(set(device)), 12)
        self.assertEqual(len(set(process)), 19)
        for body in (DEVICE_INTERFACE, PROCESS_INTERFACE):
            self.assertIn('protected:', body)
        # The header names execution-end types without including their owners.
        self.assertNotIn('#include "', INTERFACE)

    def test_device_implements_the_device_interface_under_the_same_names(self):
        self.assertIn('public war3::IDeviceServices {', DEVICE_H)
        self.assertIn('#include "war3/core/war3_execution_services.h"', DEVICE_H)
        device = DEVICE_H[DEVICE_H.index('class D3D9DeviceEx final'):]
        for name in virtuals(DEVICE_INTERFACE)[0]:
            self.assertRegex(device, r'\b%s\(' % name, name)

    def test_both_process_providers_answer_every_entry(self):
        names = virtuals(PROCESS_INTERFACE)[0]
        for provider in (REGISTRY, IN_PROCESS):
            flat = ' '.join(provider.split())
            for name in names:
                self.assertRegex(flat, r'\b%s\([^)]*\) override \{' % name, name)
            self.assertEqual(flat.count(' override {'), len(names))
        # The in-process provider forwards; it must not call itself.
        for name in names:
            if name in ('War3ResetCpuSemanticMapSession', 'StartupReportSnapshot', 'TryShowBrandingMessage'):
                continue
            self.assertRegex(IN_PROCESS, r'return (::dxvk::|war3_diag::|::war3shader::)%s\(' % name, name)
        self.assertIn('return D3D9DeviceEx::War3ResetCpuSemanticMapSession(outTombstoneSerial);', IN_PROCESS)
        self.assertIn('return startup_report::snapshot();', IN_PROCESS)
        self.assertIn('War3VKBranding::TryShowBrandingMessage(gameTime);', IN_PROCESS)
        # Inside the DLL the SDK functions are not imported.
        self.assertTrue(IN_PROCESS.lstrip().split('\n#include')[0].endswith('#define WAR3_SHADER_API_INTERNAL'))
        # A failed shader pack command must not read as OK.
        self.assertEqual(REGISTRY.count('return war3shader::ShaderPackError::INTERNAL_ERROR;'), 2)

    def test_registration_cannot_be_lost_to_initialization_order(self):
        # Constant-initialized slot in the data layer ...
        self.assertIn('constexpr UnavailableProcessServices() noexcept = default;', REGISTRY)
        self.assertIn('std::atomic<IProcessServices*> s_services{ &s_unavailable };', REGISTRY)
        self.assertIn('services != nullptr ? services : &s_unavailable', REGISTRY)
        # ... written once by the execution end while the DLL loads.
        self.assertIn('constexpr War3InProcessServices() noexcept = default;', IN_PROCESS)
        registration = between(IN_PROCESS, 'struct War3InProcessServicesRegistration {', '} // namespace')
        self.assertIn('war3::RegisterProcessServices(&g_war3InProcessServices);', registration)
        self.assertIn('} g_war3InProcessServicesRegistration;', registration)
        for line in ("'d3d9_war3_execution_services.cpp',", "'war3/core/war3_execution_services.cpp',"):
            self.assertEqual(MESON.count(line), 1, line)

    def test_services_are_published_with_the_device(self):
        self.assertIn('static std::atomic<IDeviceServices*> s_activeDeviceServices{ nullptr };', WAR3)
        publish = between(WAR3, 'void PublishActiveDeviceLocked(D3D9DeviceEx* device) {', '} // namespace')
        self.assertLess(publish.index('s_activeDevice.store(device,'),
                        publish.index('s_activeDeviceServices.store(device,'))
        clear = between(WAR3, 'bool ClearActiveDeviceIfCurrent(D3D9DeviceEx* device) {', 'bool IsActiveDevice(')
        self.assertLess(clear.index('s_activeDeviceServices.store(nullptr,'),
                        clear.index('s_activeDevice.store(nullptr,'))
        run = between(WAR3, 'bool RunWithActiveDeviceServices(', 'bool HasActivePipeline()')
        self.assertIn('std::lock_guard<std::recursive_mutex> lock(s_activePublicationMutex);', run)
        self.assertIn('s_activeDeviceServices.load(std::memory_order_acquire)', run)
        self.assertIn('transaction(*device)', run)
        self.assertNotIn('s_activeDevice.load', run)
        self.assertIn('const std::function<void(IDeviceServices&)>& transaction);', WAR3_H)

    def test_no_device_fallback_and_validation_use_the_interfaces(self):
        fallback = between(WAR3, 'void RequestActiveDeviceShadowMapResetOrCpuFallback()',
                           'struct War3SettingsWrite::Impl')
        self.assertIn('s_activeDeviceServices.load(std::memory_order_acquire)', fallback)
        self.assertNotIn('D3D9DeviceEx', fallback)
        validation = WAR3[WAR3.index('bool ExecuteSemanticShadowSceneForValidation('):]
        self.assertIn('RunWithActiveDeviceServices([&](IDeviceServices& device) {', validation)
        self.assertNotIn('D3D9DeviceEx', validation)

    def test_data_layer_callers_hold_no_concrete_device(self):
        for name, text in (('hub', HUB), ('test api', TEST_API)):
            self.assertNotIn('D3D9DeviceEx', text, name)
            self.assertNotIn('RunWithActiveDevice(', text, name)
        self.assertEqual(HUB.count('dxvk::war3::RunWithActiveDeviceServices('), 2)
        self.assertEqual(HUB.count('device.QueryWar3DeviceStatus()'), 2)
        self.assertEqual(TEST_API.count('dxvk::war3::RunWithActiveDeviceServices('), 2)
        # The render hook keeps one concrete-device transaction: the queue takeover (phase 3).
        self.assertEqual(HOOK.count('dxvk::war3::RunWithActiveDeviceServices('), 3)
        self.assertEqual(HOOK.count('RunWithActiveDevice('), 1)
        for call in ('device.War3PopulateSemanticShadowSceneForValidation(',
                     'device.War3SetWorldLightingScope(inWorld);',
                     'activeDevice.War3AttachGpuSkinNativeBridge(gameBase);'):
            start = HOOK.index(call)
            self.assertIn('dxvk::war3::IDeviceServices', HOOK[start - 160:start], call)

    def test_process_entries_go_through_the_registry(self):
        direct = (r'(?<![.\w])dxvk::Query(Shadow|Csm|PointShadow|War3Weather|War3Volumetric)\w+\(',
                  r'war3_diag::', r'startup_report::snapshot\(', r'::dxvk::TestFrameHistoryWindowShortcut\(',
                  r'(?<![.\w])QueryWar3(Weather|WeatherVolume|VolumetricExecution)Diagnostics\(')
        for name, text in (('hub', HUB), ('test api', TEST_API), ('perf', PERF),
                           ('bridge', BRIDGE), ('startup', STARTUP)):
            for pattern in direct:
                self.assertIsNone(re.search(pattern, text), (name, pattern))
        self.assertEqual(HUB.count('dxvk::war3::ProcessServices().'), 7)
        self.assertEqual(TEST_API.count('ProcessServices().'), 5)
        self.assertEqual(PERF.count('ProcessServices().'), 2)
        self.assertEqual(BRIDGE.count('::dxvk::war3::ProcessServices()'), 3)
        self.assertIn('ProcessServices().StartupReportSnapshot();', STARTUP)

    def test_phase3_callers_use_the_interfaces(self):
        # Shader pack commands and the branding notice: process services.
        self.assertEqual(HOOK.count('dxvk::war3::ProcessServices().TryShowBrandingMessage('), 2)
        self.assertNotIn('War3VKBranding', HOOK)
        self.assertEqual(EXAMPLE.count('dxvk::war3::ProcessServices().'), 4)
        self.assertNotRegex(EXAMPLE, r'war3shader::(LoadShaderPack|EnableShaderPack|SetParamVec4|GetErrorString)\(')
        self.assertIn('war3shader::ShaderPackErrorName(err)', EXAMPLE)
        # Instance buffer: COM interfaces and the device entry, never the final class.
        self.assertIn('IDirect3DDevice9Ex *m_device = nullptr;', INSTANCE_H)
        self.assertIn('return services.GetWar3InstanceBuffer();', INSTANCE)
        self.assertNotIn('device->GetWar3InstanceBuffer()', INSTANCE)
        self.assertNotIn('m_buffer->Lock(', INSTANCE)
        self.assertNotIn('m_buffer->Unlock(', INSTANCE)
        # Material: d3dcompiler stays, DXSO and shader creation are the device's.
        for name in ('DxsoModule', 'DxsoReader', 'DxsoOptions', 'dxso_module.h'):
            self.assertNotIn(name, MATERIAL)
        self.assertNotRegex(MATERIAL, r'device->Create(Vertex|Pixel)Shader\(')
        self.assertIn('services.War3CreateMaterialVertexShader(', MATERIAL)
        self.assertIn('services.War3CreateMaterialPixelShader(', MATERIAL)
        self.assertIn('module.analyze();', MATERIAL_SHADER)
        self.assertIn("'d3d9_war3_material_shader.cpp',", MESON)

    def test_stage_names_need_no_symbol(self):
        header = read('src/d3d9/d3d9_war3_weather.h')
        source = read('src/d3d9/d3d9_war3_weather.cpp')
        self.assertIn('inline const char* War3WeatherDrawStageName(War3WeatherDrawStage stage) noexcept {', header)
        self.assertNotIn('War3WeatherDrawStageName', source)


if __name__ == '__main__':
    unittest.main()
