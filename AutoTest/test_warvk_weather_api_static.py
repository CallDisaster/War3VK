"""Cross-check the weather author contract in C++, JASS and YDWE metadata.

This checks package/ABI wiring. Executable Dispatch -> WeatherRuntime tests
live in test_warvk_weather_protocol.cpp; this script does not prove rendering.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

CONTRACT = {
    "WarVKCreateSnowWeather": ("weather.createSnow", "Hotkey", "i", "integer"),
    "WarVKDestroyWeather": ("weather.destroy", "Preloader", "d", "nothing"),
    "WarVKIsWeatherAlive": ("weather.isAlive", "Hotkey", "d", "boolean"),
    "WarVKSetWeatherRegion": ("weather.setRegion", "Preloader", "drrrrrrr", "nothing"),
    "WarVKSetWeatherEnabled": ("weather.setEnabled", "Preloader", "db", "nothing"),
    "WarVKSetSnowWeatherTarget": ("weather.setSnowTarget", "Preloader", "drrrrrrr", "nothing"),
    "WarVKClearWeatherKeyframes": ("weather.clearKeyframes", "Preloader", "d", "nothing"),
    "WarVKAddSnowWeatherKeyframe": ("weather.addSnowKeyframe", "Preloader", "drrrrrrr", "nothing"),
    "WarVKPlayWeatherTimeline": ("weather.playTimeline", "Preloader", "dr", "nothing"),
    "WarVKSetWeatherPaused": ("weather.setPaused", "Preloader", "db", "nothing"),
    "WarVKSetWeatherTime": ("weather.setTime", "Preloader", "dr", "nothing"),
    "WarVKGetWeatherTime": ("weather.getTime", "LocalizedString", "d", "real"),
}


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def blocks(source):
    return dict(re.findall(r"(?ms)^\[(?!\[)([^\]]+)\]\s*\n(.*?)(?=^\[(?!\[)|\Z)", source))


class WeatherApiContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cpp = read("src/d3d9/war3/japi/war3_japi_v1.cpp")
        cls.jass = read("WarVK/jass/warvk_api.j")
        cls.actions = blocks(read("WarVK/action.txt"))
        cls.calls = blocks(read("WarVK/call.txt"))

    def test_exact_twelve_commands_allow_cpu_initialization(self):
        rows = re.findall(
            r'\{CommandId::\w+,\s*"(weather\.[^"]+)",\s*Carrier::(\w+),\s*'
            r'"([birds]*)",\s*kFeatureWeather,\s*(true|false)\}', self.cpp)
        self.assertEqual(len(rows), 12)
        expected = {item[0]: (item[1], item[2], "false") for item in CONTRACT.values()}
        self.assertEqual({name: (carrier, signature, ready) for name, carrier, signature, ready in rows}, expected)

    def test_jass_wire_types_and_carriers_match_cpp(self):
        for function, (command, carrier, signature, result) in CONTRACT.items():
            with self.subTest(function=function):
                match = re.search(rf"function {function} takes (.*?) returns (\w+)\s*\n(.*?)endfunction", self.jass, re.S)
                self.assertIsNotNone(match)
                args, returns, body = match.groups()
                self.assertEqual(returns, result)
                self.assertIn(f'"warvk:v1;{command}"', body)
                self.assertEqual("".join(re.findall(r'";([birds]):"', body)), signature)
                expected_types = ["integer" if tag in "id" else "boolean" if tag == "b" else "real" for tag in signature]
                self.assertEqual([arg.strip().split()[0] for arg in args.split(",")], expected_types)
                self.assertIn({"Preloader": "Preloader(payload)", "Hotkey": "GetLocalizedHotkey(payload)", "LocalizedString": "GetLocalizedString(payload)"}[carrier], body)

    def test_ydwe_arguments_preserve_signature_order(self):
        for function, (_, _, signature, result) in CONTRACT.items():
            with self.subTest(function=function):
                catalog, other = (self.actions, self.calls) if result == "nothing" else (self.calls, self.actions)
                self.assertIn(function, catalog)
                self.assertNotIn(function, other)
                body = catalog[function]
                self.assertIn('category = "TC_WarVKWeather"', body)
                self.assertIn(f'script = "{function}"', body)
                args = re.search(rf"function {function} takes (.*?) returns", self.jass).group(1)
                author_names = [arg.strip().split()[1] for arg in args.split(",")]
                description = re.search(r'^description = "(.*)"$', body, re.M).group(1)
                self.assertEqual(re.findall(r"\$\{(\w+)\}", description), author_names)
                self.assertEqual(re.findall(r"^type = (\w+)$", body, re.M), ["integer" if tag in "id" else "boolean" if tag == "b" else "real" for tag in signature])
                if result != "nothing":
                    self.assertIn(f"returns = {result}", body)
                self.assertRegex(body, re.compile(r'^comment = "[^"].+"$', re.M))

    def test_feature_and_managed_type_have_public_constants(self):
        constant = read("WarVK/jass/warvk_constant.j")
        self.assertIn("kFeatureWeather = 0x00020000u", self.cpp)
        self.assertIn("WARVK_FEATURE_WEATHER = 131072", constant)
        self.assertIn("WARVK_OBJECT_WEATHER = 4", constant)
        self.assertIn("Weather = 4", self.cpp)
        self.assertIn("TC_WarVKWeather=[", read("WarVK/define.txt"))

    def test_cpu_reset_does_not_reuse_public_ids(self):
        reset = self.cpp.split("void ResetAuthorState() noexcept {", 1)[1].split("void Reset()", 1)[0]
        self.assertIn("render::WeatherRuntime::instance().resetAuthorState();", reset)
        self.assertNotIn("g_nextPublicId =", reset)
        registration = self.cpp.split("int32_t RegisterObject(", 1)[1].split("void RemoveRegistryEntry", 1)[0]
        self.assertIn("std::numeric_limits<int32_t>::max()", registration)
        self.assertNotIn("g_nextPublicId = 1", registration)

    def test_author_dispatch_contains_no_render_device_requirement(self):
        dispatch = self.cpp.split("Reply DispatchWeather(", 1)[1].split("void AdvanceLightingClockRevision", 1)[0]
        for forbidden in ("GetMutableSettings", "HasActivePipeline", "GetActiveDevice", "CreateBuffer", "EmitCs", "copyBuffer", "getEnvVar"):
            self.assertNotIn(forbidden, dispatch)
        self.assertIn("RegisterObject(ManagedType::Weather, created)", dispatch)
        self.assertIn("runtime.destroy(created)", dispatch)
        self.assertIn("runtime.getTime(internalId, seconds)", dispatch)

    def test_main_runner_fallback_retires_weather_before_new_authoring(self):
        hook = read("src/d3d9/d3d9_war3_hook.cpp")
        fallback = hook.split("void ObserveWar3MainRunnerEnter(const char *source) {", 1)[1].split("void ObserveWar3MainRunnerComplete", 1)[0]
        self.assertLess(fallback.index("onMainRunnerEnter(worldLive)"), fallback.index("ResetWeatherAuthorState();"))
        self.assertLess(fallback.index("ResetWeatherAuthorState();"), fallback.index("RequestActiveDeviceShadowMapResetOrCpuFallback();"))
        scoped = self.cpp.split("void ResetWeatherAuthorState() noexcept {", 1)[1].split("void ResetAuthorState()", 1)[0]
        self.assertIn("it->second.type == ManagedType::Weather", scoped)
        self.assertIn("g_objects.erase(it)", scoped)
        self.assertIn("render::WeatherRuntime::instance().resetAuthorState();", scoped)
        for forbidden in ("createImage", "waitFor", "g_nextPublicId =", "RemovePointLight", "RemoveFogVolume"):
            self.assertNotIn(forbidden, scoped)

    def test_disabled_weather_retains_one_world_frame_for_owner_cleanup(self):
        pipeline = read("src/d3d9/d3d9_war3_pipeline.cpp")
        insertion = pipeline.split("m_wantsBeforeUiInsertion =\n", 1)[1].split(";\n", 1)[0]
        self.assertIn("WeatherRuntime::instance().hasEnabled() ||", insertion)
        self.assertIn("m_weatherSnapshot.enabled", insertion)
        capture = pipeline.split("void War3RenderPipeline::CaptureWeatherSnapshot", 1)[1].split("\n    }", 1)[0]
        self.assertIn("m_weatherSnapshot.frameSerial != input.frameSerial", capture)
        self.assertIn(": war3::render::WeatherFrameSnapshot{};", capture)
        self.assertIn("input.weather = m_weatherSnapshot;", capture)

    def test_demo_uses_only_declared_weather_functions_and_checks_setup(self):
        demo = read("WarVK/jass/warvk_weather_demo.j")
        functions = set(re.findall(r"\b(WarVK\w+)\(", demo))
        known = set(CONTRACT) | {"WarVKGetLastError", "WarVKGetLastErrorCode", "WarVKWeatherDemoCheck"}
        self.assertLessEqual(functions, known)
        self.assertEqual(demo.count("if not WarVKWeatherDemoCheck() then"), 6)


if __name__ == "__main__":
    unittest.main()
