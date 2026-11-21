"""1.30.00 player defaults and the comparison with the original game (source contracts only).

- Frame evidence is off for players: the auto mode sets WARVK_D3D9_EVIDENCE=0 unless the
  environment already has a value, the host's recorder then keeps no frame ring and runs
  no tear audit, and WarVK_EvidenceStart.cmd (pure ASCII, CRLF) turns it on.
- The comparison gate (run_v130_vanilla_compare.py) runs the original game without any
  WarVK file and counts both arms with the kernel's present events.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


class EvidenceDefaultTests(unittest.TestCase):
    def test_auto_mode_turns_evidence_off_unless_set(self):
        auto = read("src/d3d9/war3/platform/war3_proxy_auto.cpp")
        table = auto.split("const wchar_t* const kSettings[][2] = {", 1)[1].split("};", 1)[0]
        self.assertIn('{L"WARVK_D3D9_EVIDENCE", L"0"}', table)
        # A value already in the environment wins (the evidence launcher's 1).
        self.assertIn("if (::GetEnvironmentVariableW(name, nullptr, 0) == 0)", auto)
        self.assertRegex(auto, r"for \(const auto& setting : kSettings\)\s*\n\s*SetIfUnset\(")

    def test_recorder_keeps_no_ring_and_no_audit_when_off(self):
        recorder = read("tools/render_host/dxvk_core/evidence_recorder.cpp")
        self.assertIn('GetEnvironmentVariableA("WARVK_D3D9_EVIDENCE"', recorder)
        self.assertIn("if (size == 1 && value[0] == '0') {\n    m_noRing = true;", recorder.replace("\r\n", "\n"))
        audit = recorder.split("bool EvidenceRecorder::wantsAudit() const noexcept {", 1)[1].split("}", 1)[0]
        self.assertIn("!m_noRing", audit)

    def test_evidence_launcher_is_ascii_and_sets_the_switch(self):
        data = (ROOT / "tools/player/WarVK_EvidenceStart.cmd").read_bytes()
        data.decode("ascii")
        self.assertNotIn(b"\n", data.replace(b"\r\n", b""))
        text = data.decode("ascii")
        self.assertTrue(text.startswith("@echo off\r\n"))
        self.assertIn('set "WARVK_D3D9_EVIDENCE=1"', text)
        self.assertIn('start "" /D "%~dp0" "%~dp0war3.exe" %*', text)

    def test_player_x64_config_is_the_empty_one(self):
        conf = read("tools/player/x64/dxvk.conf")
        self.assertEqual([line for line in conf.splitlines() if line and not line.startswith("#")], [])


class PlayerDllGuardTests(unittest.TestCase):
    """Gates look at the player's DLL and require it unchanged; a file the author put there may stay."""

    def test_guard_compares_and_never_requires_absence(self):
        import sys
        sys.path.insert(0, str(ROOT / "AutoTest"))
        import x64_proxy_gate_mode as mode
        absent = {"path": "p", "exists": False}
        present = {"path": "p", "exists": True, "size": 3, "sha256": "AA"}
        self.assertTrue(mode.player_dll_unchanged(absent, dict(absent, stage="after")))
        self.assertTrue(mode.player_dll_unchanged(dict(present, stage="before"), dict(present)))
        self.assertFalse(mode.player_dll_unchanged(absent, present))
        self.assertFalse(mode.player_dll_unchanged(present, absent))
        self.assertFalse(mode.player_dll_unchanged(present, dict(present, sha256="BB")))
        self.assertFalse(hasattr(mode, "require_player_dll_absent"))
        for script in ("run_cross_map_ab_a_canary_20260925.py", "run_unit_walk_probe.py", "run_startup_diag_gate.py",
                       "run_weather_canary_20261010.py", "run_v12202_final_gate.py"):
            text = read("AutoTest/" + script)
            self.assertIn("x64mode.player_dll_guard(", text, script)
            self.assertIn("x64mode.player_dll_unchanged(", text, script)


class VanillaComparisonTests(unittest.TestCase):
    def test_probe_vanilla_runs_without_any_warvk_file(self):
        probe = read("AutoTest/run_x64_game_proxy_probe.py")
        self.assertIn('require(not target_dll.exists(), "a vanilla instance must have no d3d9.dll")', probe)
        self.assertIn('if not k.startswith(("WARVK_", "DXVK_"))', probe)
        self.assertIn('"--vanilla takes no DLL, host, environment or player setting"', probe)
        self.assertIn('*(["-opengl"] if args.opengl else [])', probe)
        self.assertIn('receipt["beganUnixSeconds"] = time.time()', probe)
        self.assertIn('receipt["hostPids"] = own_host_pids(expected["pid"])', probe)

    def test_gate_counts_both_arms_with_kernel_presents(self):
        gate = read("AutoTest/run_v130_vanilla_compare.py")
        self.assertIn('PROVIDER = ("Microsoft-Windows-DxgKrnl", "0x8000000", "4")', gate)
        self.assertIn('PRESENT_EVENT = "184"', gate)
        self.assertIn('pids = [pid] if arm == "vanilla" else (observer.get("hostPids") or [])', gate)
        self.assertIn('command += ["--vanilla", "--opengl"]', gate)
        self.assertIn('"--profile", "product-async-delta"', gate)
        # The same window for both arms, and the WarVK arm settled before it starts.
        self.assertIn("first <= clock <= last", gate)
        self.assertIn('row["readyInTime"] = ready is not None and ready + SETTLE_SECONDS <= first', gate)
        # The profile the gate runs is the transport the player package sets.
        auto = read("src/d3d9/war3/platform/war3_proxy_auto.cpp")
        probe = read("AutoTest/run_x64_game_proxy_probe.py")
        profile = probe.split('"product-async-delta": dict(', 1)[1].split(")", 1)[0]
        names = set(re.findall(r'(WARVK_D3D9_[A-Z0-9_]+)="1"', profile)) | {"WARVK_D3D9_BATCH_DRAWS"}
        for name in names:
            self.assertIn(f'{{L"{name}", L"1"}}', auto, name)


if __name__ == "__main__":
    unittest.main()
