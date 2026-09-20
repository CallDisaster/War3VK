"""Static wiring checks for moving the resident recorder HUD into the ImGui
console (user request 2026-09-16). SCOPE: STATIC_TEXT_CONTRACT_ONLY - source
text checks; not runtime, GPU, game or commit evidence; no compiler here.

Scoping (per main-thread review): function bodies are extracted by signature
regex + comment/literal-aware brace matching; wiring assertions run on
comment-and-literal masked bodies so comment/string pseudo-code never counts;
guards are verified together with their paired return; the panel is locked to
its real display contract (switch cases 0..7 + default, both packageReady
messages, buffered/required and saved/total, the disable note with =0 +
restart + hiding-does-not-free) and to read-only Hud access; the private
declaration in war3_imgui.h is covered too. mask() always consumes complete
string literals so a "//" inside a string is never mistaken for a comment.
No whole-file string counting. Literal-payload checks (window titles, note
texts) run with comments masked but literals kept.

If production had not landed, wiring failures were the pending-integration
record (run1). The landed shapes directed by the main thread: render defers
with inScene && tools::evidence::Enabled().
"""

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
IMGUI = ROOT / "src/d3d9/war3/ui/war3_imgui.cpp"
IMGUI_HEADER = ROOT / "src/d3d9/war3/ui/war3_imgui.h"
CORE = ROOT / "src/d3d9/war3/tools/war3_frame_history_core.h"
WINDOW = ROOT / "src/d3d9/d3d9_window.cpp"
SCOPE = "STATIC_TEXT_CONTRACT_ONLY_NOT_RUNTIME_GPU_GAME_EVIDENCE"

NEW_FRAME_SIG = r"void\s+War3Imgui::newFrame\s*\(\s*\)\s*\{"
RENDER_SIG = r"void\s+War3Imgui::render\s*\(\s*bool\s+inScene\s*\)\s*\{"
DEBUG_WIN_SIG = r"void\s+War3Imgui::drawDebugWindow\s*\(\s*\)\s*\{"
PANEL_SIG = r"void\s+War3Imgui::drawFrameRecorderPanel\s*\(\s*\)\s*\{"


def mask(text, blank_literals=True):
    """Length-preserving mask.

    Comments are always blanked. String/char literals are always consumed
    completely (so a "//" inside a literal is never read as a comment); their
    contents are blanked only when blank_literals is True, otherwise kept for
    literal-payload checks (which still cannot be satisfied from comments).
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if ch == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif ch == "/" and nxt == "*":
            out[i] = out[i + 1] = " "
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            if i + 1 < n:
                out[i] = out[i + 1] = " "
                i += 2
        elif ch == '"' or ch == "'":
            quote = ch
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    if blank_literals:
                        out[i] = out[i + 1] = " "
                    i += 2
                else:
                    if blank_literals and text[i] != "\n":
                        out[i] = " "
                    i += 1
            if i < n:
                i += 1
        else:
            i += 1
    return "".join(out)


def extract_span(masked_text, signature_regex):
    match = re.search(signature_regex, masked_text)
    if not match:
        return None
    opening = masked_text.find("{", match.end() - 1)
    if opening < 0:
        return None
    depth = 0
    for i in range(opening, len(masked_text)):
        if masked_text[i] == "{":
            depth += 1
        elif masked_text[i] == "}":
            depth -= 1
            if depth == 0:
                return (match.start(), i + 1)
    return (match.start(), None)


def has_identifier(masked_body, identifier):
    return masked_body is not None and bool(
        re.search(r"\b%s\s*\(" % re.escape(identifier), masked_body)
    )


class ConsoleContractBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.imgui_text = IMGUI.read_text(encoding="utf-8")
        cls.imgui_masked = mask(cls.imgui_text)
        cls.imgui_comments_only = mask(cls.imgui_text, blank_literals=False)
        cls.header_text = IMGUI_HEADER.read_text(encoding="utf-8")
        cls.core_text = CORE.read_text(encoding="utf-8")
        cls.window_text = WINDOW.read_text(encoding="utf-8")
        cls.new_frame_span = extract_span(cls.imgui_masked, NEW_FRAME_SIG)
        cls.render_span = extract_span(cls.imgui_masked, RENDER_SIG)
        cls.debug_span = extract_span(cls.imgui_masked, DEBUG_WIN_SIG)
        cls.panel_span = extract_span(cls.imgui_masked, PANEL_SIG)

    @classmethod
    def masked_body(cls, span):
        if span is None or span[1] is None:
            return None
        return cls.imgui_masked[span[0] : span[1]]

    @classmethod
    def comments_only_body(cls, span):
        if span is None or span[1] is None:
            return None
        return cls.imgui_comments_only[span[0] : span[1]]


class ConsoleWiringTargetState(ConsoleContractBase):
    """Frozen target state; a failure means absent or violated wiring."""

    def test_new_frame_gate_is_console_visibility_only_with_return(self):
        masked = self.masked_body(self.new_frame_span)
        self.assertIsNotNone(masked, "newFrame body not found")
        self.assertRegex(
            masked,
            r"if\s*\(\s*!\s*m_initialized\s*\|\|\s*!\s*m_visible\s*\)\s*return\s*;",
            "newFrame must return for a hidden console (guard and return "
            "verified together)")
        self.assertNotRegex(
            masked, r"QueryFrameHistoryHud\s*\(\s*\)\s*\.\s*enabled",
            "the recorder.enabled bypass must be gone from newFrame")

    def test_render_gate_is_console_visibility_only_with_return(self):
        masked = self.masked_body(self.render_span)
        self.assertIsNotNone(masked, "render body not found")
        self.assertRegex(
            masked,
            r"if\s*\(\s*!\s*m_initialized\s*\|\|\s*m_hasRendered\s*\|\|\s*"
            r"!\s*m_visible\s*\)\s*return\s*;",
            "render must return for a hidden console (guard and return "
            "verified together)")
        self.assertNotRegex(
            masked, r"!\s*m_visible\s*&&\s*recorder\s*\.\s*enabled",
            "the recorder.enabled bypass must be gone from render")

    def test_render_defers_inscene_to_present_gate(self):
        # Shape directed by the main thread with the landed implementation:
        # the deferral keys on the global forensics enable, not hud.enabled.
        masked = self.masked_body(self.render_span)
        self.assertIsNotNone(masked, "render body not found")
        self.assertRegex(
            masked,
            r"if\s*\(\s*inScene\s*&&\s*tools\s*::\s*evidence\s*::\s*Enabled"
            r"\s*\(\s*\)\s*\)\s*return\s*;",
            "with forensics enabled, inScene render defers to the Present "
            "gate so console pixels stay outside recorded history frames")

    def test_independent_recorder_status_window_removed(self):
        comments_only = self.comments_only_body(self.render_span)
        masked = self.masked_body(self.render_span)
        self.assertIsNotNone(comments_only, "render body not found")
        self.assertIsNotNone(masked, "render body not found")
        self.assertNotIn(
            "WarVK recorder status", comments_only,
            "the independent recorder status window must be fully removed")
        self.assertNotRegex(
            masked, r"SetNextWindowBgAlpha",
            "the status-window placement block must be removed with it")

    def test_debug_window_binds_close_button_to_m_visible(self):
        comments_only = self.comments_only_body(self.debug_span)
        self.assertIsNotNone(comments_only, "drawDebugWindow body not found")
        self.assertRegex(
            comments_only,
            r'Begin\s*\(\s*"War3VK 调试器"\s*,\s*&m_visible',
            "drawDebugWindow must bind the close button to m_visible")
        self.assertNotRegex(
            comments_only, r'Begin\s*\(\s*"War3VK 调试器"\s*,\s*nullptr',
            "drawDebugWindow must not pass nullptr as p_open any more")

    def test_panel_declared_private_defined_and_really_called(self):
        masked_debug = self.masked_body(self.debug_span)
        self.assertIsNotNone(masked_debug, "drawDebugWindow body not found")
        self.assertTrue(
            has_identifier(masked_debug, "drawFrameRecorderPanel"),
            "drawDebugWindow must really call the panel (comments/literals "
            "do not count)")
        self.assertIsNotNone(
            self.panel_span, "drawFrameRecorderPanel definition not found")
        self.assertIsNotNone(
            self.panel_span[1] if self.panel_span else None,
            "drawFrameRecorderPanel definition body is unbalanced")
        declaration = re.search(r"void\s+drawFrameRecorderPanel\s*\(\s*\)\s*;", mask(self.header_text))
        self.assertIsNotNone(declaration)
        access = re.findall(r"\b(public|protected|private)\s*:", mask(self.header_text)[:declaration.start()])
        self.assertEqual(access[-1], "private")
        self.assertRegex(
            mask(self.header_text),
            r"void\s+drawFrameRecorderPanel\s*\(\s*\)\s*;",
            "the private declaration must exist in war3_imgui.h")

    def test_panel_locks_full_hud_display_contract(self):
        masked = self.masked_body(self.panel_span)
        comments_only = self.comments_only_body(self.panel_span)
        self.assertIsNotNone(masked, "drawFrameRecorderPanel body not found")
        self.assertIsNotNone(
            comments_only, "drawFrameRecorderPanel body not found")
        self.assertRegex(
            masked, r"tools\s*::\s*QueryFrameHistoryHud\s*\(\s*\)",
            "the panel must really query the HUD snapshot")
        self.assertRegex(
            masked, r"CollapsingHeader\s*\(",
            "the panel content lives in a collapsing header")
        self.assertRegex(
            masked, r"switch\s*\(\s*recorder\s*\.\s*state\s*\)",
            "the panel must switch over the HUD state")
        for case in range(8):
            self.assertRegex(
                masked, r"case\s+%d\s*:" % case, "state case %d" % case)
        self.assertRegex(masked, r"default\s*:", "the default case")
        self.assertRegex(masked, r"recorder\s*\.\s*bufferedMs")
        self.assertRegex(masked, r"recorder\s*\.\s*requiredMs")
        self.assertRegex(masked, r"recorder\s*\.\s*saved")
        self.assertRegex(masked, r"recorder\s*\.\s*total")
        self.assertRegex(masked, r"recorder\s*\.\s*packageReady")
        self.assertRegex(masked, r"recorder\s*\.\s*selfContained")
        self.assertRegex(masked, r"recorder\s*\.\s*watcherAlive")
        self.assertRegex(masked, r"recorder\s*\.\s*notice")
        self.assertIn(
            "原始包已保存，可退出；完整性待离线分析", comments_only,
            "the packageReady=true message")
        self.assertIn(
            "图片已保存，正在整理日志，请勿退出", comments_only,
            "the packageReady=false (image done) message")
        self.assertRegex(
            comments_only,
            r'TextUnformatted\s*\(\s*recorder\.packageReady\s*\?\s*'
            r'"原始包已保存，可退出；完整性待离线分析"\s*:\s*'
            r'"图片已保存，正在整理日志，请勿退出"\s*\)',
            "packageReady must select the correct message, not just contain both")
        self.assertIn("DXVK_WAR3_FRAME_EVIDENCE=0", comments_only)
        self.assertIn("重新启动", comments_only)
        self.assertIn("不释放取证内存", comments_only)
        self.assertRegex(
            comments_only, r"不停止采集|不会停止采集",
            "hiding the console must be documented as not stopping capture")

    def test_panel_is_read_only_no_control_or_memory_queries(self):
        masked = self.masked_body(self.panel_span)
        self.assertIsNotNone(masked, "drawFrameRecorderPanel body not found")
        for pattern, why in (
            (r"evidence\s*::\s*Control", "no recorder control from the panel"),
            (r"FrameHistoryControl|TriggerFrameHistoryFromGame", "no implicit capture actions from display"),
            (r"->\s*cancel|\.cancel\s*\(", "no cancel from the panel"),
            (r"\breset\s*\(|\.store\s*\(", "no reset or atomic state writes from the panel"),
            (r"ReleaseLocalRecorder|\.release\s*\(", "no release from the panel"),
            (r"RecorderMemoryAdmission|RecorderDiskBudget",
             "no memory admission queries from the panel"),
            (r"GlobalMemoryStatusEx|VirtualQuery|MemoryHeapInfo",
             "no memory queries from the panel"),
            (r"hud\s*\(\s*\)\s*\.\s*[A-Za-z_]+\s*=",
             "the panel never writes HUD state"),
        ):
            self.assertNotRegex(masked, pattern, why)


class ConsoleContractPreserved(ConsoleContractBase):
    """Boundaries that must hold before and after the change."""

    def test_ctrl_shift_c_window_wiring_unchanged(self):
        self.assertIn("key==0x43 && ctrl && shift && !repeat", self.core_text)
        self.assertIn("message==0x100", self.core_text)
        self.assertIn("HandleFrameHistoryShortcut(message", self.window_text)

    def test_cursor_overlay_gate_preserved(self):
        masked = self.masked_body(self.render_span)
        self.assertIsNotNone(masked, "render body not found")
        self.assertRegex(
            masked,
            r"if\s*\(\s*!\s*inScene\s*&&\s*ImGui::GetIO\s*\(\s*\)\s*\.\s*WantCaptureMouse\s*\)")


class StaticCheckSelfTests(unittest.TestCase):
    """Negative self-tests for the scoping helpers on synthetic text only."""

    def test_comment_only_panel_call_does_not_count(self):
        text = ("void War3Imgui::drawDebugWindow(){\n"
                "  // drawFrameRecorderPanel();\n}")
        self.assertIn("drawFrameRecorderPanel", text)
        self.assertFalse(
            has_identifier(mask(text), "drawFrameRecorderPanel"),
            "a comment-only pseudo-call must not count as a real call")
        self.assertTrue(
            has_identifier(
                mask("void War3Imgui::drawDebugWindow(){"
                     "drawFrameRecorderPanel();}"), "drawFrameRecorderPanel"))

    def test_missing_panel_definition_fails_definition_check(self):
        caller = ("void War3Imgui::drawDebugWindow(){"
                  "drawFrameRecorderPanel();}")
        self.assertIsNotNone(extract_span(mask(caller), DEBUG_WIN_SIG))
        self.assertIsNone(
            extract_span(mask(caller), PANEL_SIG),
            "a missing drawFrameRecorderPanel definition must not pass")

    def test_slash_slash_inside_string_is_not_a_comment(self):
        text = 'const char* s = "// not a comment"; drawFrameRecorderPanel();'
        kept = mask(text, blank_literals=False)
        self.assertIn("// not a comment", kept)
        self.assertTrue(
            has_identifier(kept, "drawFrameRecorderPanel"),
            "a real call after a string containing // must still be seen")


if __name__ == "__main__":
    unittest.main()
