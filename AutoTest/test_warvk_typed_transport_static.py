import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JAPI_H = ROOT / "src/d3d9/war3/japi/war3_japi_v1.h"
JAPI_CPP = ROOT / "src/d3d9/war3/japi/war3_japi_v1.cpp"
BRIDGE_H = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.h"
BRIDGE_CPP = ROOT / "src/d3d9/war3/hooks/war3_jass_command_bridge.cpp"
JASS = ROOT / "WarVK/jass/warvk_api.j"


class WarVkTypedTransportStaticTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.japi_h = JAPI_H.read_text(encoding="utf-8")
        cls.japi_cpp = JAPI_CPP.read_text(encoding="utf-8")
        cls.bridge_h = BRIDGE_H.read_text(encoding="utf-8")
        cls.bridge_cpp = BRIDGE_CPP.read_text(encoding="utf-8")
        cls.jass = JASS.read_text(encoding="utf-8")

    def test_optional_carriers_use_verified_stock_signatures(self):
        for signature in (
            "(Hhashtable;III)V",
            "(Hhashtable;IIR)V",
            "(Hhashtable;II)I",
            "(Hhashtable;II)R",
        ):
            self.assertIn(signature, self.bridge_cpp)
        self.assertIn("CoreCarriers[]", self.bridge_cpp)
        self.assertIn("TypedCarriers[]", self.bridge_cpp)
        self.assertIn("optional typed carriers unavailable", self.bridge_cpp)
        self.assertIn("string v1 remains active", self.bridge_cpp)

    def test_non_capability_calls_forward_to_warcraft(self):
        for original in (
            "CallOriginalSaveInteger",
            "CallOriginalSaveReal",
            "CallOriginalLoadInteger",
            "CallOriginalLoadReal",
        ):
            self.assertIn(original, self.bridge_cpp)
        self.assertIn("return false;", self.japi_cpp)
        self.assertIn("table != g_typedActiveTable", self.japi_cpp)

    def test_handshake_constants_match_jass(self):
        expected = {
            "kTypedRegisterParent": 1465273172,
            "kTypedRegisterChildA": 1380271921,
            "kTypedRegisterChildB": 1380271922,
            "kTypedRegisterCookieA": 324478056,
            "kTypedRegisterCookieB": 610800471,
            "kTypedProbeChild": 1347571522,
            "kTypedProbeAck": 1464555058,
            "kTypedBeginChild": -2147418111,
            "kTypedCommitChild": -2147418110,
            "kTypedQueryIntegerChild": -2147418109,
            "kTypedQueryRealChild": -2147418108,
        }
        for cpp_name, value in expected.items():
            cpp_match = re.search(
                rf"{cpp_name}\s*=\s*(-?(?:0x[0-9a-fA-F]+|[0-9]+))",
                self.japi_h,
            )
            self.assertIsNotNone(cpp_match, cpp_name)
            self.assertEqual(int(cpp_match.group(1), 0), value, cpp_name)
            jass_name = cpp_name.replace("kTyped", "wvkTyped")
            self.assertRegex(self.jass, rf"{jass_name}\s*=\s*{value}\b")

    def test_high_frequency_wrappers_have_typed_and_legacy_paths(self):
        wrappers = {
            "WarVKSetPointLightPosition": "wvkTypedPointLightSetPosition",
            "WarVKSetPointLightColorIntensity": "wvkTypedPointLightSetColorIntensity",
            "WarVKSetPointLightRadius": "wvkTypedPointLightSetRadius",
            "WarVKEvaluateMathReal": "wvkTypedMathEvaluateReal",
            "WarVKEvaluateMathInteger": "wvkTypedMathEvaluateInteger",
            "WarVKEvaluateCurveComponent": "wvkTypedCurveEvaluateComponent",
            "WarVKEvaluateCurveDerivativeComponent": "wvkTypedCurveDerivativeComponent",
            "WarVKGetCurveArcLength": "wvkTypedCurveArcLength",
            "WarVKAppendPointCurve4": "wvkTypedCurvePointAppend4",
            "WarVKSetLightningEndpoints": "wvkTypedLightningSetEndpoints",
            "WarVKSetLightningColor": "wvkTypedLightningSetColor",
            "WarVKSetLightningWidth": "wvkTypedLightningSetWidth",
            "WarVKGetVisualTimeSeconds": "wvkTypedTimeVisualSeconds",
            "WarVKGetFramesPerSecond": "wvkTypedStatsFramesPerSecond",
            "WarVKGetFrameTimeMilliseconds": "wvkTypedStatsFrameTimeMilliseconds",
        }
        for function, opcode in wrappers.items():
            match = re.search(
                rf"function\s+{function}\b(?P<body>.*?)endfunction",
                self.jass,
                flags=re.DOTALL,
            )
            self.assertIsNotNone(match, function)
            body = match.group("body")
            self.assertIn("WVKTypedReady()", body, function)
            self.assertIn(opcode, body, function)
            self.assertIn("warvk:v1;", body, function)
        real_body = re.search(
            r"function\s+WarVKEvaluateMathReal\b(?P<body>.*?)endfunction",
            self.jass,
            flags=re.DOTALL,
        ).group("body")
        self.assertIn("return LoadReal(", real_body)
        self.assertIn("return S2R(GetLocalizedString(payload))", real_body)

    def test_transactions_are_bounded_and_reset_with_map_vm(self):
        self.assertIn("kMaximumTypedTransactions = 4u", self.japi_cpp)
        self.assertIn("g_typedTransactions.clear()", self.japi_cpp)
        self.assertIn("ResetTypedTransport();", self.japi_cpp)
        self.assertIn("ResetTypedTransport();", self.bridge_cpp)
        self.assertIn("typedTransportInstalled", self.bridge_h)

    def test_clean_room_route_has_no_memhack_registration_identifier(self):
        combined = "\n".join(
            (self.japi_h, self.japi_cpp, self.bridge_h, self.bridge_cpp, self.jass)
        )
        self.assertNotIn("JapiFunc", combined)


if __name__ == "__main__":
    unittest.main()
