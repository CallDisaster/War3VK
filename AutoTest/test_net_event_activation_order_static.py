"""Source-order contract only; actual native event delivery needs a game run."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ActivationOrder(unittest.TestCase):
    def test_native_hook_follows_published_jass_prerequisites(self):
        source = (ROOT / "src/d3d9/d3d9_war3_hook.cpp").read_text("utf-8")
        activation = source[source.index("void ActivateWar3Runtime("):source.index("void TryInstallShadowHooksEarly(")]
        self.assertEqual(activation.count("NetEventHook::get().init()"), 1)
        self.assertLess(activation.index("::war3_preinit();"), activation.index("jass::get_instance(0xD)"))
        self.assertLess(activation.index("jass::get_instance(0xD)"), activation.index("NetEventHook::get().init()"))
        self.assertLess(activation.index("NetEventHook::get().init()"), activation.index("::war3_init();"))
        bootstrap = (ROOT / "src/d3d9/war3/platform/war3_runtime_bootstrap.cpp").read_text("utf-8")
        self.assertNotIn("NetEventHook::get().init()", bootstrap)

    def test_scalar_observers_do_not_read_plain_callback_state(self):
        events = (ROOT / "src/d3d9/war3/core/war3_events.h").read_text("utf-8")
        for field, typ in (("m_jassReady", "bool"), ("m_gameStarted", "bool"), ("m_lastGameTick", "uint32_t")):
            self.assertIn(f"std::atomic<{typ}> {field}", events)
        net = (ROOT / "src/d3d9/war3/core/war3_net_event_hook.h").read_text("utf-8")
        self.assertIn("std::atomic<bool> m_runtimeReady", net)


if __name__ == "__main__":
    unittest.main()
